# C++ Usage

本示例演示如何在 ESP-IDF 工程中初始化 `BMI270 + BMM150` 传感器，并周期性读取：

- 加速度（单位：`g`）
- 角速度（单位：`dps`）
- 磁场（单位：`uT`）

示例入口代码位于 [main/main.cpp](main/main.cpp)。

## 功能说明

示例流程：

1. 创建 I2C 主机总线；
2. 通过 `bmi270_bmm150_sensor_create()` 初始化传感器；
3. 循环检查数据可用性并读取三轴数据；
4. 每 200 ms 打印一次日志。

当前默认初始化模式为：

- `BOSCH_ACCEL_AND_MAGN`（加速度计 + 磁力计）

> 说明：陀螺仪数据同样在示例中读取并打印。

## 硬件连接

请根据你的板卡修改 [main/main.c](main/main.c) 中 I2C 宏定义。

当前默认配置：

- `SCL = GPIO10`
- `SDA = GPIO8`
- `I2C Port = I2C_NUM_0`
- `I2C 频率 = 400kHz`

同时请确认：

- 传感器供电与地线连接正确；
- I2C 线上有上拉（代码中开启了内部上拉）；
- 传感器 I2C 地址与配置一致（示例使用 `BMI2_I2C_PRIM_ADDR`，通常为 `0x68`）。

## 编译与烧录

在本示例目录执行：

```bash
cd examples/simple_usage
idf.py set-target <your_target>
idf.py build
idf.py -p <your_port> flash monitor
```

## 预期日志

运行后可看到类似输出：

```text
I (...) BMI270_BMM150_DEMO: BMI270 init done, start reading acceleration...
I (...) BMI270_BMM150_DEMO: ACC[g]: x=..., y=..., z=...
I (...) BMI270_BMM150_DEMO: GYR[dps]: x=..., y=..., z=...
I (...) BMI270_BMM150_DEMO: MAG[uT]: x=..., y=..., z=...
```

## 常见问题

1. **初始化失败 / 无输出**

    - 检查 I2C 引脚是否与硬件一致；
    - 检查供电与地线；
    - 使用代码中的 I2C 扫描片段确认设备地址是否可见。

2. **读数全 0 或异常抖动**

    - 确认传感器安装方向；
    - 检查线缆接触与供电稳定性；
    - 适当降低 I2C 频率进行排查（如 100kHz）。

3. **构建依赖问题**

    - 确认 ESP-IDF 版本满足组件要求；
    - 确认工程可正常解析组件依赖（`idf.py reconfigure` 后重试）。
