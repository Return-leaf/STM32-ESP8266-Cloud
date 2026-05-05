# STM32 + ESP8266 + OneNET MQTT 物联网项目

基于 STM32F407 + ESP8266 WiFi 模块，通过 MQTT 协议连接中国移动 OneNET 物联网平台，实现设备属性上报和云端指令下发。

## 硬件平台

| 组件 | 型号/说明 |
|------|----------|
| 主控芯片 | STM32F407ZGT6（LQFP144） |
| WiFi 模块 | ATK-ESP8266（正点原子） |
| 开发板 | 正点原子探索者 STM32F407 |
| 调试串口 | USART1（PA9/PA10，115200） |
| ESP8266 串口 | USART2（PA2/PA3，115200） |
| ESP8266 复位 | PF6（低电平有效） |

## 功能特性

- **WiFi 连接**：ESP8266 通过 AT 指令连接路由器
- **MQTT 连接**：连接 OneNET 物联网平台（mqtts.heclouds.com:1883）
- **属性上报**：每 5 秒向平台上报一次属性值（OneJSON 格式）
- **属性设置**：接收云端下发的属性设置指令并回复
- **属性获取**：接收云端下发的属性获取指令并返回当前值

## MQTT Topic 结构

| Topic | 方向 | 说明 |
|-------|------|------|
| `$sys/{pid}/{device}/thing/property/post` | 设备→平台 | 属性上报 |
| `$sys/{pid}/{device}/thing/property/post/reply` | 平台→设备 | 上报回复 |
| `$sys/{pid}/{device}/thing/property/set` | 平台→设备 | 属性设置指令 |
| `$sys/{pid}/{device}/thing/property/set/reply` | 设备→平台 | 设置回复 |
| `$sys/{pid}/{device}/thing/property/get` | 平台→设备 | 属性获取指令 |
| `$sys/{pid}/{device}/thing/property/get/reply` | 设备→平台 | 获取回复 |

## 通信协议

### 属性上报（OneJSON 格式）

```json
{
    "id": "123",
    "version": "1.0",
    "params": {
        "wifi": {
            "value": 42
        }
    }
}
```

### 属性设置回复

```json
{
    "id": "39",
    "code": 200,
    "msg": "success"
}
```

### 属性获取回复

```json
{
    "id": "40",
    "code": 200,
    "msg": "success",
    "data": {
        "wifi": {
            "value": 42
        }
    }
}
```

## 代码架构

```
Core/
├── Inc/
│   ├── main.h          # 主程序头文件
│   ├── usart.h         # 串口初始化及环形缓冲区 API
│   ├── wifi.h          # WiFi/MQTT 配置及函数声明
│   └── stm32f4xx_it.h  # 中断处理声明
└── Src/
    ├── main.c          # 主程序（系统初始化 + 主循环）
    ├── usart.c         # 串口初始化 + USART2 中断接收 + 环形缓冲区
    ├── wifi.c          # ESP8266 AT 指令 + OneNET MQTT 通信逻辑
    └── stm32f4xx_it.c  # 中断服务程序
```

### 关键设计

- **USART2 中断接收**：使用 1024 字节环形缓冲区，中断处理程序逐字节存入缓冲区
- **AT 指令收发**：发送 AT 指令时临时关闭 USART2 中断，使用阻塞方式接收响应
- **逐字节接收**：`uart2_recv_until` 函数按字节读取，遇到指定字符（如 `>`）立即返回
- **异步消息处理**：主循环从环形缓冲区读取数据，扫描 `+MQTTSUBRECV` 通知并处理

## 开发环境

- **代码生成**：STM32CubeMX（生成初始化代码）
- **IDE**：VS Code + Cortex-Debug 插件
- **HAL 库**：STM32CubeF4 HAL Driver
- **编译器**：ARM GCC
- **烧录**：ST-Link

## 使用方法

1. 修改 `wifi.h` 中的 WiFi 名称和密码
2. 在 OneNET 平台创建产品和设备，获取产品 ID、设备名和鉴权密钥
3. 修改 `wifi.h` 中的 OneNET 配置信息
4. 编译并烧录到 STM32F407 开发板
5. 打开串口调试助手（115200），查看连接状态和属性上报
6. 在 OneNET 控制台进行属性设置/获取操作

## 串口输出示例

```
[WiFi] Connected
[SUB] Subscribing property/set...
[SUB OK] AT+MQTTSUB=0,"$sys/q4igTV8l2n/stm32Wifi/thing/property/set",0
[SUB] Subscribing property/get...
[SUB OK] AT+MQTTSUB=0,"$sys/q4igTV8l2n/stm32Wifi/thing/property/get",0
[SUB] Subscribing post/reply...
[SUB OK] AT+MQTTSUB=0,"$sys/q4igTV8l2n/stm32Wifi/thing/property/post/reply",0
[MQTT] Connected
[Report] wifi=0
[Report] OK
[Report] wifi=1
[MQTT RECV] topic="$sys/q4igTV8l2n/stm32Wifi/thing/property/set"
[MQTT RECV] data={"id":"39","version":"1.0","params":{"wifi":11}}
[SET] id=39
[SET] Reply sent
```

## 已知问题

- 属性设置/获取回复后 MQTT 连接偶尔断开（`+MQTTDISCONNECTED`），可能与 OneNET 平台对回复消息的处理机制有关，后续需要进一步调试。

## License

MIT
