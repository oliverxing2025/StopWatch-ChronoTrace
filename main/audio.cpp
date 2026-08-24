#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "board.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "render.h"

namespace {

constexpr int kSampleRate = 44100;
constexpr int kBlockSamples = 256;
constexpr gpio_num_t kMclk = GPIO_NUM_18;
constexpr gpio_num_t kBclk = GPIO_NUM_17;
constexpr gpio_num_t kDin = GPIO_NUM_16;
constexpr gpio_num_t kLrck = GPIO_NUM_15;
constexpr gpio_num_t kDout = GPIO_NUM_21;

i2s_chan_handle_t s_tx;
i2s_chan_handle_t s_rx;
esp_codec_dev_handle_t s_codec;
const audio_codec_data_if_t *s_data_if;
const audio_codec_ctrl_if_t *s_ctrl_if;
const audio_codec_gpio_if_t *s_gpio_if;
const audio_codec_if_t *s_codec_if;

int16_t s_output[kBlockSamples];
int16_t s_input[kBlockSamples];
int16_t s_wave[256];
volatile uint8_t s_theme;
volatile uint8_t s_volume;
volatile bool s_reactive;
volatile float s_impact;
volatile audio_event_t s_event;
volatile bool s_event_pending;
volatile uint8_t s_tick_urgency;
bool s_tick_tock;
float s_event_phase;
float s_event_phase_b;
float s_event_phase_c;
int s_event_samples;
int s_event_total_samples;
float s_surf_low;
float s_surf_mid;
float s_surf_high;
float s_surf_envelope = 0.18f;
float s_surf_target;
float s_surf_step;
float s_surf_wander;
int s_surf_transition_samples;
bool s_surf_first_logged;
uint32_t s_noise = 0x729a41u;
float s_low_lp;
float s_mid_lp;
float s_floor[3] = {220.0f, 220.0f, 220.0f};
float s_peak[3] = {1800.0f, 1800.0f, 1800.0f};
float s_level[3];
int s_analysis_hold;
const char *TAG = "audio";

float clamp01(float value)
{
    return std::max(0.0f, std::min(1.0f, value));
}

float wave(float *phase, float hz)
{
    *phase += hz * 256.0f / kSampleRate;
    while (*phase >= 256.0f) *phase -= 256.0f;
    return s_wave[(int)*phase & 255] / 32768.0f;
}

float noise_sample()
{
    s_noise ^= s_noise << 13;
    s_noise ^= s_noise >> 17;
    s_noise ^= s_noise << 5;
    return ((int32_t)(s_noise & 0xFFFF) - 32768) / 32768.0f;
}

void analyze_microphone(void)
{
    float energy[3] = {};
    for (int i = 0; i < kBlockSamples; i++) {
        const float x = (float)s_input[i];
        s_low_lp += 0.026f * (x - s_low_lp);       // roughly below 180 Hz
        s_mid_lp += 0.24f * (x - s_mid_lp);        // roughly below 2.2 kHz
        energy[0] += std::fabs(s_low_lp);
        energy[1] += std::fabs(s_mid_lp - s_low_lp);
        energy[2] += std::fabs(x - s_mid_lp);
    }

    if (s_analysis_hold > 0) {
        --s_analysis_hold;
        render_set_audio_levels(0.0f, 0.0f, 0.0f);
        return;
    }

    for (int band = 0; band < 3; band++) {
        const float raw = energy[band] / kBlockSamples;
        if (raw < s_floor[band]) s_floor[band] += (raw - s_floor[band]) * 0.08f;
        else s_floor[band] += (raw - s_floor[band]) * 0.001f;
        s_peak[band] = std::max(raw, s_peak[band] * 0.992f);
        const float span = std::max(350.0f, s_peak[band] - s_floor[band]);
        const float normalized = clamp01((raw - s_floor[band] * 1.08f) / span);
        const float smoothing = normalized > s_level[band] ? 0.48f : 0.10f;
        s_level[band] += (normalized - s_level[band]) * smoothing;
    }
    render_set_audio_levels(s_level[0], s_level[1], s_level[2]);
}

void begin_event(audio_event_t event)
{
    s_event = event;
    switch (event) {
        case AUDIO_EVENT_BOOT: s_event_total_samples = kSampleRate * 55 / 100; break;
        case AUDIO_EVENT_THEME: s_event_total_samples = kSampleRate * 46 / 100; break;
        case AUDIO_EVENT_COUNTDOWN_TICK: s_event_total_samples = kSampleRate * 14 / 100; break;
        case AUDIO_EVENT_COUNTDOWN: s_event_total_samples = kSampleRate * 120 / 100; break;
        case AUDIO_EVENT_UI_CLICK: s_event_total_samples = kSampleRate * 7 / 100; break;
        default: s_event_total_samples = kSampleRate * 36 / 100; break;
    }
    s_event_samples = s_event_total_samples;
    s_event_phase = 0.0f;
    s_event_phase_b = 0.0f;
    s_event_phase_c = 0.0f;
    if (event == AUDIO_EVENT_COUNTDOWN_TICK) s_tick_tock = !s_tick_tock;
    s_analysis_hold = event == AUDIO_EVENT_COUNTDOWN_TICK ? 25 : 45;
    if (event != AUDIO_EVENT_COUNTDOWN_TICK) {
        ESP_LOGI(TAG, "healing water-drop event %d, volume %u", (int)event, s_volume);
    }
}

float event_sample(void)
{
    if (s_event_samples <= 0) return 0.0f;
    const float elapsed = (s_event_total_samples - s_event_samples) / (float)kSampleRate;

    if (s_event == AUDIO_EVENT_COUNTDOWN_TICK) {
        // Alternating mechanical-watch escapement tick/tock. A very short dry
        // contact transient supplies the precision, while two damped case
        // resonances add weight. It intentionally stops well before the next
        // second, so it never turns into an electronic tone or metronome beep.
        const bool urgent = s_tick_urgency != 0;
        float root = s_tick_tock ? 468.0f : 392.0f;
        if (urgent) root *= 1.1225f;
        const float attack = std::min(1.0f, elapsed * 520.0f);
        const float decay = expf(-elapsed * (urgent ? 24.0f : 29.0f));
        const float contact = noise_sample() * expf(-elapsed * 125.0f) * 0.18f;
        float tone = wave(&s_event_phase, root) * 0.57f;
        tone += wave(&s_event_phase_b, root * 2.73f) * 0.18f;
        tone += wave(&s_event_phase_c, root * 0.497f) * 0.25f;
        --s_event_samples;
        return (tone + contact) * attack * decay * (urgent ? 1.03f : 0.82f);
    }

    if (s_event == AUDIO_EVENT_UI_CLICK) {
        // A compact, rounded UI tick: enough attack to confirm a selection,
        // with a very short decay so repeated settings taps stay unobtrusive.
        const float attack = std::min(1.0f, elapsed * 700.0f);
        const float decay = expf(-elapsed * 48.0f);
        const float contact = noise_sample() * expf(-elapsed * 175.0f) * 0.08f;
        float tone = wave(&s_event_phase, 720.0f) * 0.68f;
        tone += wave(&s_event_phase_b, 1080.0f) * 0.22f;
        tone += wave(&s_event_phase_c, 1440.0f) * 0.10f;
        --s_event_samples;
        return (tone + contact) * attack * decay * 0.72f;
    }

    float root = 523.25f;
    switch (s_event) {
        case AUDIO_EVENT_BOOT: root = 523.25f; break;
        case AUDIO_EVENT_RESET: root = 392.00f; break;
        case AUDIO_EVENT_THEME: root = 440.00f + 36.0f * s_theme; break;
        case AUDIO_EVENT_DENSITY: root = 587.33f; break;
        case AUDIO_EVENT_TOUCH: root = 659.25f; break;
        case AUDIO_EVENT_UI_CLICK: root = 720.00f; break;
        case AUDIO_EVENT_REACTIVE: root = 493.88f; break;
        case AUDIO_EVENT_VOLUME: root = 523.25f; break;
        case AUDIO_EVENT_COUNTDOWN_TICK: root = 392.00f; break;
        case AUDIO_EVENT_COUNTDOWN: root = 523.25f; break;
    }

    // A soft pentatonic water-drop pair: round fundamental, restrained upper
    // partials, and a slower decay than the former metallic cyber chirp.
    const float second_strike = s_event == AUDIO_EVENT_BOOT ? 0.20f : 0.145f;
    float local;
    float lift;
    if (s_event == AUDIO_EVENT_COUNTDOWN) {
        if (elapsed < 0.22f) { local = elapsed; lift = 1.0f; }
        else if (elapsed < 0.48f) { local = elapsed - 0.22f; lift = 1.25f; }
        else { local = elapsed - 0.48f; lift = 1.4983f; }
    } else {
        local = elapsed < second_strike ? elapsed : elapsed - second_strike;
        lift = elapsed < second_strike ? 1.0f : 1.4983f;
    }
    const float strike = std::min(1.0f, local * 120.0f) * expf(-local * 8.5f);
    const float note = root * lift;
    float drop = wave(&s_event_phase, note) * 0.78f;
    drop += wave(&s_event_phase_b, note * 2.0f) * 0.16f;
    drop += wave(&s_event_phase_c, note * 3.0f) * 0.06f;

    --s_event_samples;
    return drop * strike;
}

float healing_surf_sample(float activity)
{
    const float n = noise_sample();
    s_surf_low += 0.008f * (n - s_surf_low);
    s_surf_mid += 0.055f * (n - s_surf_mid);
    s_surf_high += 0.26f * (n - s_surf_high);

    // There is deliberately no oscillator in the surf texture. Each target and
    // duration is unrelated to the previous one. The envelope has a wide range
    // so a continuously moving pool still produces audible crests and retreats;
    // particle motion remains the overall ceiling for the result.
    if (s_surf_transition_samples <= 0) {
        const float level_random = (noise_sample() + 1.0f) * 0.5f;
        const float time_random = (noise_sample() + 1.0f) * 0.5f;
        if (level_random < 0.22f) {
            s_surf_target = 0.025f + level_random * 0.55f;
        } else if (level_random > 0.82f) {
            s_surf_target = 0.76f + (level_random - 0.82f) * 1.33f;
        } else {
            s_surf_target = 0.15f + (level_random - 0.22f) * 1.02f;
        }
        float seconds;
        if (s_surf_target > s_surf_envelope) {
            seconds = 0.18f + time_random * 0.82f;
        } else {
            seconds = 0.48f + time_random * time_random * 2.15f;
        }
        s_surf_transition_samples = std::max(1, (int)(seconds * kSampleRate));
        s_surf_step = (s_surf_target - s_surf_envelope) /
                      s_surf_transition_samples;
    }
    s_surf_envelope += s_surf_step;
    --s_surf_transition_samples;
    s_surf_wander += 0.00008f * (n - s_surf_wander);
    const float swell = clamp01(s_surf_envelope + s_surf_wander * 3.2f);
    float movement = clamp01((activity - 0.012f) / 0.92f);
    // Cheap approximation of x^1.16. This runs at 44.1 kHz, so a generic
    // powf here would steal an entire render-frame budget from the simulator.
    movement *= 0.84f + movement * 0.16f;
    const float body = (s_surf_mid - s_surf_low) * (0.62f + movement * 0.26f);
    const float foam = (s_surf_high - s_surf_mid) * (0.07f + movement * 0.58f);
    const float texture = 0.14f + swell * 0.86f;
    if (!s_surf_first_logged && movement > 0.04f && s_volume > 0) {
        s_surf_first_logged = true;
        ESP_LOGI(TAG, "amplitude-linked healing surf active: motion %.2f",
                 (double)activity);
    }
    return (body + foam) * texture * movement;
}

void audio_task(void *)
{
    for (;;) {
        const bool reactive = s_reactive;
        if (reactive) {
            if (esp_codec_dev_read(s_codec, s_input, sizeof(s_input)) == ESP_OK) {
                analyze_microphone();
            }
        } else {
            render_set_audio_levels(0.0f, 0.0f, 0.0f);
        }

        if (s_event_pending) {
            const audio_event_t event = s_event;
            s_event_pending = false;
            begin_event(event);
        }

        uint8_t current_volume = s_volume;
        if (current_volume > 100) current_volume = 100;
        const float volume_gain = current_volume / 100.0f;
        const float surf_gain = 0.58f * volume_gain;
        const float event_gain = 0.36f * volume_gain;
        const float impact = clamp01(s_impact);
        for (int i = 0; i < kBlockSamples; i++) {
            float sample = 0.0f;
            if (!reactive) {
                const float tick_duck =
                    (s_event == AUDIO_EVENT_COUNTDOWN_TICK && s_event_samples > 0) ?
                    0.30f : 1.0f;
                sample += healing_surf_sample(impact) * surf_gain * tick_duck;
            }
            sample += event_sample() * event_gain;
            sample = std::max(-1.0f, std::min(1.0f, sample));
            s_output[i] = (int16_t)(sample * 32767.0f);
        }
        const int ret = esp_codec_dev_write(s_codec, s_output, sizeof(s_output));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "codec write failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

}  // namespace

extern "C" esp_err_t audio_init(i2c_bus_handle_t bus, uint8_t theme, uint8_t volume,
                                 bool reactive)
{
    if (bus == nullptr) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < 256; i++) {
        s_wave[i] = (int16_t)(sinf(2.0f * (float)M_PI * i / 256.0f) * 32767.0f);
    }

    const i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = kMclk,
            .bclk = kBclk,
            .ws = kLrck,
            .dout = kDout,
            .din = kDin,
            .invert_flags = {},
        },
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, &s_tx, &s_rx), TAG, "I2S channel failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &standard), TAG, "I2S TX failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &standard), TAG, "I2S RX failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "I2S TX enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "I2S RX enable failed");

    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.rx_handle = s_rx;
    i2s_cfg.tx_handle = s_tx;
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.addr = ES8311_CODEC_DEFAULT_ADDR;
    i2c_cfg.bus_handle = i2c_bus_get_internal_bus_handle(bus);
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    s_gpio_if = audio_codec_new_gpio();
    es8311_codec_cfg_t codec_cfg = {};
    codec_cfg.ctrl_if = s_ctrl_if;
    codec_cfg.gpio_if = s_gpio_if;
    codec_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    codec_cfg.pa_pin = GPIO_NUM_NC;
    codec_cfg.pa_reverted = false;
    codec_cfg.use_mclk = true;
    s_codec_if = es8311_codec_new(&codec_cfg);
    esp_codec_dev_cfg_t device_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    s_codec = esp_codec_dev_new(&device_cfg);
    if (s_codec == nullptr) return ESP_FAIL;

    esp_codec_dev_sample_info_t format = {};
    format.bits_per_sample = 16;
    format.channel = 1;
    format.sample_rate = kSampleRate;
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_codec, &format), TAG, "codec open failed");
    esp_codec_dev_set_in_gain(s_codec, 30.0f);
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_codec, 78), TAG,
                        "codec output volume failed");

    s_theme = theme % 8;
    s_volume = std::min<uint8_t>(volume, 100);
    s_reactive = reactive;
    ESP_RETURN_ON_ERROR(board_speaker_enable(s_volume > 0), TAG, "speaker enable failed");
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 6, nullptr, 0);
    if (s_volume > 0) audio_trigger(AUDIO_EVENT_BOOT);
    ESP_LOGI(TAG, "ES8311 ready: 44.1kHz, codec 78, volume %u, reactive %s",
             s_volume, s_reactive ? "on" : "off");
    return ESP_OK;
}

extern "C" void audio_set_theme(uint8_t theme)
{
    s_theme = theme % 8;
}

extern "C" void audio_set_volume(uint8_t volume)
{
    s_volume = std::min<uint8_t>(volume, 100);
    board_speaker_enable(s_volume > 0);
}

extern "C" void audio_set_reactive(bool reactive)
{
    s_reactive = reactive;
    if (!reactive) render_set_audio_levels(0.0f, 0.0f, 0.0f);
}

extern "C" void audio_set_motion(float mean_speed, float max_speed, int wall_hits,
                                   int clamped_pairs)
{
    const float mean_motion = clamp01(std::max(0.0f, mean_speed - 150.0f) / 4200.0f);
    const float peak_motion = clamp01(std::max(0.0f, max_speed - 700.0f) / 22000.0f);
    const float speed = mean_motion * 0.74f + peak_motion * 0.26f;
    const float contacts = clamp01(wall_hits / 180.0f + clamped_pairs / 80.0f);
    const float impact = clamp01(speed + contacts * 0.12f);
    s_impact += (impact - s_impact) * (impact > s_impact ? 0.42f : 0.055f);
}

extern "C" void audio_get_levels(float *bass, float *mid, float *treble)
{
    if (bass) *bass = s_level[0];
    if (mid) *mid = s_level[1];
    if (treble) *treble = s_level[2];
}

extern "C" void audio_trigger(audio_event_t event)
{
    s_event = event;
    s_event_pending = true;
}

extern "C" void audio_trigger_countdown_tick(uint8_t urgency)
{
    s_tick_urgency = urgency ? 1 : 0;
    audio_trigger(AUDIO_EVENT_COUNTDOWN_TICK);
}
