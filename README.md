# 51 MCU + NRF24L01 → STM32F407 + ESP8266 → 华为云 IoT

> 基于 FreeRTOS 的无线温度采集与云端监控系统

## 项目简介

51 单片机采集 DS18B20 温度传感器数据，通过 NRF24L01 无线模块发送给 STM32F407。STM32 接收后通过 ESP8266 WiFi 模块经 MQTT 协议上报至华为云 IoT 平台，实现温度的远程实时监控。

```
┌──────────────┐    NRF24L01     ┌─────────────────────────────┐    WiFi/MQTT    ┌──────────┐
│ 51 MCU       │   (2.4GHz)      │ STM32F407 + FreeRTOS        │   (TCP:1883)   │ 华为云IoT │
│  + DS18B20   │ ═══════════════ │  + NRF24L01 (RX)            │ ═══════════════ │          │
│  + NRF24L01  │                 │  + ESP8266                  │                │ 实时温度  │
│  + LCD1602   │                 │  + TFT LCD (2.8")           │                │ 监控面板  │
└──────────────┘                 └─────────────────────────────┘                └──────────┘
```

## 硬件清单

| 组件 | 型号 | 说明 |
|------|------|------|
| 51 开发板 | 51 MCU | 温度采集 + 无线发送 |
| 温度传感器 | DS18B20 | 数字温度传感器，精度 0.1°C |
| 无线模块 ×2 | NRF24L01 | 2.4GHz 无线通信，32 字节载荷 |
| 主控板 | 正点原子探索者 STM32F407 | Cortex-M4，168MHz |
| WiFi 模块 | ATK-ESP8266 (MW8266D) | AT 指令集，TCP/MQTT 透传 |
| 显示屏 | TFT LCD 2.8" (ILI9341) | FSMC 接口，320×240 |
| 调试工具 | ST-Link V2 | 烧录与调试 |
| 云平台 | 华为云 IoT | MQTT 设备接入 |

## 软件架构

### FreeRTOS 三任务设计

采用 FreeRTOS 实时操作系统，将原本互相阻塞的 NRF24 轮询、WiFi/MQTT 通信、LCD 刷新拆分为三个独立任务：

```
优先级3 ── vNRF24Task   (50ms周期)  ── 轮询NRF24接收51发来的温度数据
优先级2 ── vWiFiMQTTTask (100ms周期) ── WiFi连接 → MQTT连接 → 定时上报温度
优先级1 ── vLCDTask      (1s周期)    ── 每秒刷新LCD温度显示
```

**关键设计决策：为什么要用 FreeRTOS？**

在没有操作系统的情况下，ESP8266 的 AT 指令（`wifi_send_cmd`）是阻塞调用，最长可达 20 秒。在此期间 CPU 无法轮询 NRF24L01，导致温度数据丢失。FreeRTOS 将三个任务分离到不同优先级，通过抢占式调度确保 NRF24 数据接收永不间断。

### 状态机设计（WiFi/MQTT 任务）

```
IDLE → TESTING → CONNECTING → MQTT_INIT → READY
         ↓           ↓            ↓          ↓
       AT测试    连接路由器    MQTT连接   定时上报+保活
```

- **TESTING**：每 5 秒发送 AT 测试指令
- **CONNECTING**：每 3 秒尝试连接 WiFi
- **MQTT_INIT**：连接后 2 秒发起 MQTT CONNECT
- **READY**：每 2 秒上报温度，每 60 秒 PING 保活

### NRF24L01 通信协议

51 单片机每 2.5 秒采集并发送一帧 32 字节数据：

| 字节 | 含义 | 示例 |
|------|------|------|
| [0] | 帧头 0xAA | AA |
| [1] | 帧类型 0x01（温度） | 01 |
| [2] | 温度 MSB（×0.1°C） | 00 |
| [3] | 温度 LSB | C3 (= 19.5°C) |
| [4] | 校验和 = [0]+[1]+[2]+[3] | 6E |
| [5] | 帧尾 0x55 | 55 |
| [6..31] | 填充 0x00 | 00... |

### MQTT 上报格式（华为云）

**Topic**: `$oc/devices/{device_id}/sys/properties/report`

```json
{
  "services": [{
    "service_id": "temptest",
    "properties": {
      "temperature": "19.5"
    }
  }]
}
```

## 代码结构

```
STM32-ESP8266-Cloud-main/
├── 8051_TX/                    # 51单片机发送端（不可修改）
│   ├── main.c                  # 主程序：DS18B20采集 + NRF24发送 + LCD1602显示
│   ├── 18b20.c/h               # DS18B20 驱动
│   ├── NRF/                    # NRF24L01 驱动（51端）
│   └── DS1602/                 # LCD1602 驱动
├── Core/
│   ├── Inc/
│   │   ├── FreeRTOSConfig.h    # FreeRTOS 内核配置
│   │   ├── wifi.h              # WiFi/MQTT/Huawei Cloud 配置宏 + 函数声明
│   │   ├── 51_comm.h           # 51通信协议层（帧解析）
│   │   ├── nrf24l01.h          # NRF24L01 寄存器定义 + 驱动（STM32端）
│   │   ├── lcd.h               # TFT LCD 驱动
│   │   ├── spi.h               # SPI 驱动
│   │   ├── sha256.h            # SHA256/HMAC-SHA256（MQTT密码计算备用）
│   │   ├── usart.h             # 串口驱动
│   │   └── stm32f4xx_it.h      # 中断服务声明
│   └── Src/
│       ├── main.c              # ★ 主程序：FreeRTOS 三任务 + 状态机
│       ├── wifi.c              # ESP8266 AT指令 + MQTT报文构建 + 华为云通信
│       ├── 51_comm.c           # NRF24接收 + 温度帧解析
│       ├── nrf24l01.c          # NRF24L01 SPI驱动（寄存器读写）
│       ├── lcd.c               # TFT LCD 底层驱动（FSMC）
│       ├── spi.c               # SPI1 驱动（用于NRF24）
│       ├── sha256.c            # SHA256 实现
│       ├── stm32f4xx_it.c      # 中断服务（SysTick/FreeRTOS hooks）
│       └── usart.c             # 串口初始化
├── Middlewares/Third_Party/
│   └── FreeRTOS/               # FreeRTOS V10.6.2 内核源码
├── Drivers/                    # STM32 HAL 库 + CMSIS
├── CMakeLists.txt              # 顶层 CMake（含 FreeRTOS 源文件）
└── README.md                   # 本文档
```

## 踩坑记录

### 坑1：MQTT 密码 — 设备密钥 vs MQTT 连接密码

**现象**：MQTT CONNACK 返回 rc=4（Bad username or password）

**原因**：华为云 IoT **设备密钥**（注册设备时生成的 `secret`）≠ **MQTT 连接密码**。MQTT 连接密码需要通过 HMAC-SHA256 用设备密钥对时间戳签名计算得到，或者直接从平台的「MQTT 连接参数」中复制。

**解决**：使用平台提供的 MQTT 连接参数中的 password，硬编码到 `wifi.h` 中。

### 坑2：MQTT 端口 — SSL vs TCP

**现象**：连上 WiFi 后 MQTT 反复 NTP→SSL→MQTT→Retry

**原因**：默认使用 8883 端口（MQTT over SSL/TLS），ESP8266 AT 固件的 SSL 功能需要额外配置证书，流程复杂。

**解决**：改用 1883 端口（TCP 明文 MQTT），配合华为云 IoT 的 TCP 接入地址即可。

### 坑3：FreeRTOS SVC/PendSV Handler 缺失

**现象**：编译通过但系统卡死，LCD 停在启动画面不动，串口停在 `NRF24L01 ready - polling...`

**原因**：`FreeRTOSConfig.h` 中的 `#define vPortSVCHandler SVC_Handler` 和 `#define xPortPendSVHandler PendSV_Handler` 被误删。FreeRTOS 依赖 **SVC 异常**启动第一个任务、依赖 **PendSV 异常**进行上下文切换。没有这两个 handler，调度器无法启动。

**解决**：在 `FreeRTOSConfig.h` 中保留 SVC/PendSV 的映射，但 SysTick 的映射不保留（由 `stm32f4xx_it.c` 自定义处理以同时调用 `HAL_IncTick()`）。

### 坑4：SysTick_Handler 冲突

**现象**：链接时报 `multiple definition of SysTick_Handler`

**原因**：`FreeRTOSConfig.h` 中 `#define xPortSysTickHandler SysTick_Handler` 将 port.c 中的 `xPortSysTickHandler` 重命名为 `SysTick_Handler`，与 `stm32f4xx_it.c` 中的 `SysTick_Handler` 冲突。

**解决**：删除该 `#define`，在 `stm32f4xx_it.c` 中手动调用 `extern void xPortSysTickHandler(void)`，同时保留 `HAL_IncTick()` 调用以维持 HAL 时钟。

### 坑5：WiFi 任务栈溢出导致全部任务卡死

**现象**：MQTT 连接前温度流畅更新，MQTT 刚连上所有数据立即卡死 —— NRF24 不再接收、LCD 不再刷新。

**原因**：WiFi/MQTT 任务栈设为 512 words（2KB）。但 `Huawei_MQTT_Init()` 内部局部变量累计超 800B，加上 `process_incoming_mqtt()` 的 768B 局部缓冲区和函数调用栈帧，总栈深轻松突破 2KB。栈溢出踩坏相邻任务的 TCB，导致 NRF24 和 LCD 任务一起挂。

**解决**：将 WiFi/MQTT 任务栈增加到 1024 words（4KB）。

### 坑6：NRF24 与 ESP8266 的"假冲突"

**现象**：一度怀疑 NRF24 在 2.4GHz 频段与 ESP8266 WiFi 产生干扰，更换 WiFi 信道后温度反而完全收不到。

**结论**：不是硬件干扰问题。更换 WiFi 后收不到数据，实际是因为 WiFi 连接失败的阻塞等待时间更长（15 秒重试），FreeRTOS 调度没问题但 51 的发送间隔不变。真正的问题是栈溢出（坑5），在原始 WiFi 下恰好因时序不同而"勉强工作"。

### 坑7：温度上报间隔默认为 30 秒

**现象**：华为云上温度不刷新或刷新很慢

**原因**：`main.c` 中 `lastReport >= 30000`（30 秒上报一次）

**解决**：改为 `2000`（2 秒），兼顾实时性和网络负载。

## 华为云 IoT 配置要点

1. **设备类型**：自定义类型，直接填写如 `TempSensor`
2. **服务 ID**：如 `temptest`
3. **属性名**：`temperature`，类型 string
4. **设备认证**：密钥方式，留空让平台自动生成
5. **MQTT 接入地址**：从设备详情页的「MQTT 连接参数」获取
6. **Topic**：`$oc/devices/{device_id}/sys/properties/report`

## 编译与烧录

### 环境要求

- **IDE**：Keil MDK-ARM (uVision5)，建议 V5.36 及以上
- **编译器**：ARM Compiler 5 (ARMCC V5.06) 或 ARM Compiler 6
- **设备包**：Keil.STM32F4xx_DFP (2.15.0 及以上)
- **调试器**：ST-Link V2

### 编译

1. 用 Keil uVision5 打开 `Projects\MDK-ARM\atk_f407.uvprojx`
2. 点击 **Build**（F7）编译工程
3. 编译输出位于 `Output\` 目录，生成 `atk_f407.axf` 和 `atk_f407.hex`

### 烧录

1. 通过 ST-Link 连接探索者 STM32F407 开发板
2. 在 Keil 中点击 **Download**（F8）即可自动烧录
3. 或使用 STM32 ST-LINK Utility 烧录 `Output\atk_f407.hex`

### 配置 WiFi 和云平台参数

在 `Drivers\BSP\ESP8266\esp8266.h` 中修改以下宏为你的实际参数：

```c
#define WIFI_SSID          "YOUR_WIFI_SSID"
#define WIFI_PWD           "YOUR_WIFI_PASSWORD"
#define HUAWEI_MQTT_HOST   "YOUR_MQTT_HOST"
#define HUAWEI_DEVICE_ID   "YOUR_DEVICE_ID"
#define HUAWEI_DEVICE_PWD  "YOUR_DEVICE_PASSWORD"
#define HUAWEI_CLIENT_ID   "YOUR_CLIENT_ID"
```

## 许可证

本项目基于教学实验用途，部分驱动代码来源于正点原子（ALIENTEK）示例程序。
