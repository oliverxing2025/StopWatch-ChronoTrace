#pragma once

// Every ChronoTrace tunable lives here. Anything you might want to change to
// alter the look or the performance/quality tradeoff is a constant below.

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

#define LCD_H_RES 466
#define LCD_V_RES 466

// The renderer never holds a whole frame. It draws one horizontal band at a
// time into internal SRAM and DMAs it out while drawing the next one, which
// keeps the framebuffer off PSRAM entirely.
#define BAND_ROWS 16
#define BAND_COUNT ((LCD_V_RES + BAND_ROWS - 1) / BAND_ROWS)

#define DISPLAY_BRIGHTNESS 180  // 0..255, written to panel register 0x51

// ---------------------------------------------------------------------------
// Simulation box
// ---------------------------------------------------------------------------

// x and y are screen pixels. z is depth into the case: 0 is the glass, BOX_D
// is the back of the enclosure.
//
// The panel is 322 ppi, so a physically accurate case depth would be about
// 150 px. That reads as a tunnel rather than a slim device once perspective is
// applied, so the box is deliberately half that. The side benefit is that the
// same 900 particles now fill about a third of the volume instead of a fifth,
// which makes the pool look like a body of liquid rather than a puddle.
//
// If you change this, check GRID_CZ in sim.c: every grid cell has to stay at
// least one smoothing radius deep.
#define BOX_W 466.0f
#define BOX_H 466.0f
#define BOX_D 75.0f

#define PX_PER_METER 10484.0f
#define PX_PER_MM (PX_PER_METER / 1000.0f)

// The panel's corners are rounded, so the box has to be too. With a square box
// the fluid collects in four corners that are physically not visible, and the
// box overshoots the glass. Measured at roughly 4.5 mm, which is about 57 px at
// this panel's 322 ppi.
// A rounded square whose corner radius is half its width degenerates into an
// exact circle in resolve_walls(). This reuses the upstream continuous
// side/front/back fillet solver without adding a separate circular code path.
#define BOX_CORNER_R (BOX_W * 0.5f)

// The case does not meet its own back panel at a sharp edge either, so the
// walls curve into the backplane over this radius. It has to stay well under
// both BOX_D and BOX_CORNER_R: the box is only about 5.9 mm deep, so anything
// near 3 mm turns the back of the box into a dome.
#define BOX_BACK_FILLET_MM 2.0f
#define BOX_BACK_FILLET (BOX_BACK_FILLET_MM * PX_PER_MM)

// The glass meets the walls at a much tighter radius than the back panel does.
// Keep the two fillets summing to well under BOX_D, or the straight section of
// wall between them disappears.
#define BOX_FRONT_FILLET (BOX_BACK_FILLET * 0.25f)

// The simulation runs in slow motion. Real water in a 3 cm box sloshes far too
// fast to see; scaling time down turns it into the syrupy, readable motion
// that reads as "liquid" on a small screen. All physical constants stay real,
// only the clock is slowed, so gravity, shake and rotation stay consistent
// with each other.
// Lower also means a smaller effective timestep, which is what keeps the
// pressure solver quiet: gravity adds less velocity per step for the
// relaxation to cancel, so the fluid settles instead of simmering. What
// actually matters is TIME_SCALE divided by the step rate: measured at 900
// particles, 0.085 settled to 65 px/s and 0.115 only reached 140. This is the
// compromise, and it is the first thing to lower if the surface looks noisy.
#define TIME_SCALE 0.100f

// Ceiling on the physics timestep, in scaled seconds per substep.
//
// This is a stability limit, not a pacing one, and it matters more than any
// other constant here. Relaxation displacement scales as dt^2, so a step that
// takes longer than usual does not merely advance further, it advances with a
// quadratically stiffer solver. Without a ceiling that closes a loop: a deep
// pool gives every particle more neighbours, which slows the solver, which
// raises dt, which grows the displacements until they saturate
// MAX_DISPLACEMENT, and a saturated displacement becomes velocity of about
// MAX_DISPLACEMENT/dt — roughly 1000 px/s of pure invention. That agitates the
// fluid, which clusters it further, which slows the solver again.
//
// Measured: at 39 steps/s the fluid settles, at 25 steps/s a fifth of all pairs
// were clamped and the whole box simmered at 1800 px/s regardless of how still
// the board was held. 2.7 ms corresponds to about 37 steps/s, so above that
// rate the fluid still runs in real time; below it the fluid runs slow instead
// of running unstable, which is invisible next to a solver that never settles.
#define SIM_DT_MAX 0.0022f

#define GRAVITY_MPS2 9.81f

// Extra weight, to buy back the pace lost to the timestep ceiling.
//
// SIM_DT_MAX caps how much scaled time each step may advance, so whenever the
// solver runs slower than about 45 steps/s the fluid advances less than
// TIME_SCALE asks for and visibly drags. Gravity is the right knob to make up
// the difference: fall distance goes as g * t^2, so a stronger pull restores the
// pace without touching dt, and dt is the only thing the solver's stability
// actually depends on. Turning this up is always safe for stability, unlike
// turning up TIME_SCALE, which is not.
// 2.2 measured: at rest the fluid runs at 150-250 px/s, which at a 2.2 ms step
// is a third of a pixel of overlap per step against a 4 px allowance, so there is
// a wide margin before displacement clamping — the thing that used to inject
// energy — can restart. Shaking hard does reach it, but nobody can see extra
// energy in fluid that is already being thrown across the box.
#define GRAVITY_GAIN 2.2f

// ---------------------------------------------------------------------------
// Particles
// ---------------------------------------------------------------------------

// Arrays are sized for MAX; PARTICLE_COUNT is what actually runs and is the
// first thing to turn down if the frame rate drops.
//
// MAX is capped by internal SRAM, not by speed: every particle costs about
// 150 bytes across the position, velocity, density and snapshot arrays, and
// 1200 overflows the static DRAM segment at link time. COUNT is capped by
// speed: 1200 particles ran at 23 steps/s while being shaken, 900 holds
// around 30, and the extra smoothness reads better than the extra density.
#define PARTICLE_MAX 1000
#define PARTICLE_COUNT 900

// Distance between neighbouring particles when the fluid is at rest.
#define REST_SPACING 17.0f

// Interaction radius. Larger means smoother fluid but cubically more neighbour
// work; 1.65x spacing gives roughly 18 neighbours per particle.
//
// Tempting as it looks as a performance knob, it is not one. Dropping it to 24
// (1.41x) was measured: the kernel weight at rest spacing is (1 - r/h)^2, so a
// narrower kernel does not just find fewer neighbours, it weakens the pressure
// response between the ones it keeps. Rest density fell 1.17 -> 0.51 and the
// fluid then compressed to 1.7x its rest density instead of 1.1x, packing
// neighbours back in and returning only 15% of the 30% saving, in exchange for a
// visibly squashed body of liquid. Retuning K to compensate raises stiffness,
// which costs the timestep straight back. Leave it at 1.65x.
#define SMOOTH_RADIUS 28.0f

// One relaxation pass per frame. The solver is position based, so a single
// substep stays stable; two would look slightly smoother but halves the rate.
#define SUBSTEPS 1

// Double density relaxation stiffness, as an acceleration in px/s^2 per unit
// of density error. Displacement is scaled by dt^2, so these behave like real
// accelerations and the solver stays stable if SUBSTEPS changes. For scale,
// slowed-down gravity is about 2400 px/s^2, so the fluid resists compression
// roughly twenty times harder than gravity squeezes it.
// PRESSURE keeps the bulk at rest density and, being signed, also pulls the
// surface together like surface tension. NEAR_PRESSURE is always repulsive and
// stops particles collapsing onto each other.
// Do not trade these away for a larger timestep. Displacement goes as
// K * dt^2, so quartering the stiffness does buy a doubling of dt at the same
// displacement per step, and it looks like a free way to make the fluid run at
// full speed. Measured, it is the opposite: at a quarter stiffness a tilted pool
// settled at 2.4 times rest density instead of 1.2, and a compressed pool packs
// every particle in among more neighbours, so the density and relaxation passes
// each doubled to 22 ms and the step rate fell from 25/s to 18/s.
//
// Density drives solver cost, and solver cost drives the step rate, so a softer
// fluid ends up being a slower fluid. Keeping the fluid stiff keeps it cheap.
#define K_PRESSURE 400000.0f
#define K_NEAR_PRESSURE 800000.0f

// Cap on how far one neighbour pair may push a particle in a single step, in
// pixels. Deliberately a per-pair limit rather than a per-particle one; see the
// note on corner behaviour in the README.
#define MAX_DISPLACEMENT 4.0f

// Wall projection lands a particle up to this many pixels inside the surface,
// chosen at random, instead of exactly on it.
//
// This is what stops a tight corner from stacking particles on one point. The
// projection maps a whole wedge of space onto a short arc, so without the
// jitter every particle arriving at a corner ends up with near-identical
// coordinates. Pairs that close are dropped by the solver as degenerate, so
// they sit welded together while still inflating their neighbours' density
// until the relaxation throws something out — the corner "jump". Nudging them
// apart as they land costs nothing and removes the degenerate case at source.
//
// It is applied after velocity has been recovered from the step's motion, so it
// shifts positions without injecting any velocity of its own.
#define WALL_JITTER 0.35f

// Clavet viscosity: linear and quadratic terms applied to the closing speed
// between neighbours. Higher is thicker, more honey than water. Speeds here
// are in pixels per second and run into the hundreds, which is why SIGMA is
// large: a pair sheds roughly 0.5 * dt * q * SIGMA of its closing speed per
// step, so SIGMA around 30 damps a few percent per neighbour.
#define VISC_SIGMA 45.0f
#define VISC_BETA 0.03f

#define WALL_RESTITUTION 0.25f

// Tangential drag applied once per step while a particle is touching a wall.
// This is per step, not per second, so it compounds fast: 0.86 at ~34 steps/s
// left the fluid crawling along the walls instead of sliding into a corner.
#define WALL_FRICTION 0.96f

// ---------------------------------------------------------------------------
// IMU
// ---------------------------------------------------------------------------

// Time constant of the low pass that separates steady gravity from shake.
#define GRAVITY_LP_HZ 1.2f

// How hard a shake pushes the fluid, relative to the true pseudo-force.
#define SHAKE_GAIN 1.6f

// Rotating-frame forces from the gyro: centrifugal, Euler (angular
// acceleration) and Coriolis. Set to 0 to disable them.
#define ROTATION_GAIN 1.0f

// Maps the BMI270 axes already aligned to the official M5Stack StopWatch HAL
// convention onto simulation axes (x right, y down, z into the case).
// Flip a sign here if the fluid falls the wrong way.
//
// An accelerometer reports the reaction to gravity, so whichever axis points
// up reads positive. Measured on this board:
//   flat, screen up          -> (0, 0, -9.9)   up is IMU -z, so sim z = az
//   upright, port right      -> (+10.6, 0, 0)  up is IMU +x, so sim y = -ax
//   right edge to the floor  -> (0, -9.6, 0)   up is IMU -y, so sim x = ay
#define IMU_MAP_X(ax, ay, az) (ax)
#define IMU_MAP_Y(ax, ay, az) (ay)
#define IMU_MAP_Z(ax, ay, az) (az)

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Pinhole projection. A particle at the glass draws at full size; one at the
// back of the case draws at FOCAL/(FOCAL+BOX_D) of that. Shorter focal length
// means stronger perspective: at 220 against a 75 deep box, the back of the
// case works out at about 75% of the front. Since the enclosure itself is never
// drawn, this and the depth dimming are what make the fluid read as 3D.
#define PROJ_FOCAL 280.0f

// Drawn radius, at the glass; particles further back shrink with perspective.
//
// Well under half the rest spacing of 17, so discs stay separate and the fluid
// reads as distinct beads with gaps between them rather than as one continuous
// surface. Pushing this past about 0.5x the spacing closes the gaps and the
// individual particles merge into a single body of liquid — a different look,
// and deliberately not this one.
#define PARTICLE_RADIUS_PX 6.5f
#define DISC_MAX_R 10

// A smaller, brighter disc offset towards the top left of each particle, which
// turns a flat circle into something that reads as a lit sphere.
//
// This only works while the discs are separate. If PARTICLE_RADIUS_PX is ever
// raised far enough that they overlap into a continuous surface, nothing hides
// the neighbouring highlights any more and they stop reading as spheres and
// start reading as a regular grid of pale dots.
#define HIGHLIGHT_ENABLE 1
#define HIGHLIGHT_LIFT 0.55f  // 0 = no lift, 1 = pure white

// Colour ramp resolution: speed is quantised to this many steps, depth to
// DEPTH_LEVELS, and the product is baked into one lookup table.
#define SPEED_LEVELS 64
#define DEPTH_LEVELS 16

// Speed (px/s in simulation time) that saturates the ramp at pure white.
// Measured on the device: settled fluid sits near 60, a tilt-driven pour runs
// a few hundred, and a hard shake peaks above 10000. Set high so that white is
// reserved for genuinely violent motion.
#define SPEED_COLOR_MAX 5000.0f

// Curve applied to speed before it indexes the ramp. Below 1 it stretches the
// low end, so gentle motion still shows colour while white stays rare.
#define SPEED_COLOR_GAMMA 0.55f

// Brightness of the far plane relative to the near one, so this is the floor of
// the depth ramp rather than the strength of it: the darkening applied is
// 1 - DEPTH_DIM_MIN. At 0.60 the back of the box is dimmed by 40%, half as much
// as the 80% it started at.
#define DEPTH_DIM_MIN 0.60f
