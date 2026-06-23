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

#### 为什么要用状态机？

ESP8266 通过 AT 指令与 STM32 通信。AT 指令是阻塞式的——例如 `AT+CWJAP` 连接路由器最长需要等待 15 秒才返回结果。如果在裸机 `while(1)` 循环中直接调用，这 15 秒内 CPU 完全卡死在轮询串口，NRF24L01 无法接收 51 单片机发来的温度数据，导致数据丢失。

FreeRTOS 将系统拆分为三个独立任务，WiFi/MQTT 任务内部采用**非阻塞状态机**：每次进入任务只检查当前状态的一个条件，不符合就立即 `vTaskDelay` 让出 CPU，绝不占用。

```c
// freertos_demo.c — 核心循环
for (;;) {
    switch (g_wifi_state) {
        case WIFI_STATE_TESTING:    // 每 5 秒试一次 AT
        case WIFI_STATE_CONNECTING: // 每 3 秒试一次连 WiFi
        case WIFI_STATE_MQTT_INIT:  // 每 2 秒试一次 MQTT
        case WIFI_STATE_READY:      // 正常运行：上报 / 心跳 / 接收消息
    }
    vTaskDelay(pdMS_TO_TICKS(100));  // 100ms 后回来，绝不阻塞
}
```

#### 五个状态

```
IDLE (0) → TESTING (1) → CONNECTING (2) → MQTT_INIT (3) → READY (4)
                ↑ 失败重试              ↑ 失败重试
```

| 状态 | 含义 | 触发条件 | 执行动作 | 重试间隔 |
|------|------|---------|---------|---------|
| `IDLE` | 初始状态 | 任务启动 | 等待 3 秒后进入 TESTING | — |
| `TESTING` | AT 通信测试 | 每 5 秒 | 发送 `AT\r\n`，期望返回 `OK` | 5 秒 |
| `CONNECTING` | 连接 WiFi | AT 测试通过 | `AT+CWJAP` 连接路由器 | 3 秒 |
| `MQTT_INIT` | MQTT 认证 | WiFi 已连接 | TCP 建连 + MQTT CONNECT | 2 秒 |
| `READY` | 正常工作 | CONNACK rc=0 | 1s 上报 + 60s 心跳 + 1s 收消息 | — |

重试间隔的设计依据：
- **AT 测试 5 秒** — 指令简单，`wifi_send_cmd` 内部超时仅 2 秒，低频即可
- **WiFi 连接 3 秒** — `AT+CWJAP` 内部超时 15 秒，太频繁会让路由器反感
- **MQTT 握手 2 秒** — TCP + CONNECT 约 3-5 秒，2 秒兼顾响应速度和资源消耗

**设计原则：** 状态间必须串行——AT 不通就不连 WiFi，WiFi 没连就不发 MQTT。失败后停留在当前状态重试（不回退，因为前置步骤已验证通过）。

#### 代码实现关键细节

**`lastStep` 计时器防抖**

```c
case WIFI_STATE_TESTING:
    if (HAL_GetTick() - lastStep > 5000) {   // 距上次尝试过了 5 秒？
        if (wifi_test()) {
            g_wifi_state = WIFI_STATE_CONNECTING;  // 状态跃迁
        }
        lastStep = HAL_GetTick();  // 无论成败，都重置计时器
    }
    break;
```

成功和失败都重置 `lastStep`。如果成功不重置，进入下一状态后立即触发超时；如果失败不重置，会在下一个 100ms 循环中疯狂重试。

**`volatile` 跨任务共享**

```c
static volatile int g_wifi_state = 0;  // 两个 FreeRTOS 任务共享此变量
```

NRF24 任务也会读 `g_wifi_state`——MQTT 就绪后 15 秒无温度数据则复位 NRF24L01。WiFi 任务写入，NRF24 任务只读，`volatile` 阻止编译器优化到寄存器。本项目只有一个写入者，因此不需要互斥锁。

**READY 状态三个独立定时器**

```c
case WIFI_STATE_READY:
    if (HAL_GetTick() - lastPing >= 60000)  { huawei_ping(); }              // 60s 心跳
    if (HAL_GetTick() - lastReport >= 1000) { huawei_report_temperature(); } // 1s 上报
    if (HAL_GetTick() - lastIncoming >= 1000) { huawei_handle_incoming(); }  // 1s 收消息
    break;
```

各有独立 `lastXxx` 计时器，互不干扰——心跳触发不会推迟温度上报。

**WiFi 任务栈：1024 words (4KB)**

```c
xTaskCreate(vWiFiMQTTTask, "WiFiMQ", 1024, NULL, 2, NULL);
```

不是拍脑袋的数字。`huawei_mqtt_connect()` 内部局部变量累计超 800B，加上 `process_incoming_mqtt()` 的 768B 局部缓冲，栈深可达 1.7KB。最初设为 512 words 时，MQTT 连接成功后立即栈溢出，踩坏相邻任务的 TCB，NRF24 和 LCD 任务一起卡死。

#### 常见问题

**Q1：为什么不用事件驱动？** ESP8266 的标准 AT 固件不支持主动通知。它不会在 WiFi 连上后主动发中断——STM32 只能轮询。换 MQTT AT 固件或直接用 ESP32 可实现事件驱动，但属于另一种硬件方案。

**Q2：能跳过某个状态吗？** 技术上可以，但破坏阶段性错误定位能力。保留完整状态链，MQTT 失败时一眼就知道卡在 AT/密码/认证哪一环节。

**Q3：MQTT 断线会自动回退吗？** 当前停留在 READY 重试。更健壮的方案是连续上报失败 3 次后回退到 MQTT_INIT 重连。

**Q4：NRF24 和 ESP8266 都 2.4GHz，不干扰吗？** 排查后发现真正原因是栈溢出（见上），非射频干扰。两模块间距 10cm + NRF24 最低速率可降低误码。

**Q5：volatile 能保证同步吗？** 不能。只保证从内存读，不提供原子性。本项目恰好安全（单写多读）。多写场景必须用信号量或临界区。
### 环形缓冲区设计（USART3 + ESP8266）

> 本文档详细说明 STM32-ESP8266-Cloud 项目中 ESP8266 串口通信的环形缓冲区（Ring Buffer）设计——中断生产者 + 任务消费者的无锁架构，以及 AT 指令模式下的"关中断直接轮询"双模切换机制。



## 1. 背景：ESP8266 的串口通信特点

ESP8266 通过 USART3（PB10/PB11，115200 8N1）与 STM32 通信，使用 AT 指令集。数据收发有以下特点：

**发送方向（STM32 → ESP8266）：** 可控。发送 AT 指令时，知道"什么时候发、发什么、期望收到什么"。

**接收方向（ESP8266 → STM32）：不可控。** ESP8266 何时返回数据完全由它自己决定，STM32 无法预测：

- 发送 `AT\r\n` 后，ESP8266 可能在 1ms 内返回 `OK`，也可能等 2 秒才返回
- MQTT 连接成功后，服务器随时可能推送消息（命令下发、属性设置）
- TCP 接收的数据以 `+IPD,<len>:<data>` 格式夹杂在 AT 应答中

**如果不用缓冲区：** 在主循环中轮询 `USART3->DR` 寄存器，每次只读 1 字节。但主循环还要做 NRF24 轮询、LCD 刷新、MQTT 上报——轮询间隔 100ms，ESP8266 可能在两次轮询之间发来几十字节数据，`DR` 寄存器只能存 1 字节（新数据会覆盖旧数据），导致**数据丢失**。

**解决方案：** 用 USART3 的接收中断（RXNE）+ 环形缓冲区。中断每收到 1 字节就存入缓冲区，任务空闲时批量取出处理。中断和任务之间通过 head/tail 指针实现**无锁同步**。

---

## 2. 环形缓冲区的数据结构

```c
// esp8266.c
#define UART3_RX_BUF_SIZE 2048                    // 缓冲区容量：2KB

static volatile uint8_t  uart3_rx_buf[UART3_RX_BUF_SIZE];  // 数据存储区
static volatile uint16_t uart3_rx_head = 0;                // 写指针（生产者：ISR）
static volatile uint16_t uart3_rx_tail = 0;                // 读指针（消费者：任务）
```

### 2.1 指针移动规则

```
初始化：head = 0, tail = 0  （缓冲区空）

写入 1 字节后：head = 1, tail = 0  （1 字节可读）
写入 5 字节后：head = 5, tail = 0  （5 字节可读）
读出 3 字节后：head = 5, tail = 3  （2 字节可读）

head == tail  → 缓冲区空
(head + 1) % SIZE == tail → 缓冲区满（保留 1 字节防止空/满混淆）
```

### 2.2 为什么是 2048 字节？

ESP8266 的 TCP 透传数据以 `+IPD,<len>:<data>` 格式到达。MQTT 报文最大可达 512 字节（MQTT_BUF_SIZE），加上 `+IPD,4:...` 的头部约 10 字节，单帧约 522 字节。若 FreeRTOS 任务因高优先级任务抢占而来不及读取，可能积压 2-3 帧。2048 字节（约 4 帧 MQTT 报文）提供充足余量。

### 2.3 为什么用 `volatile`？

head 在中断中修改，tail 在任务上下文中修改。两个执行上下文访问同一变量，`volatile` 阻止编译器将其优化到寄存器中，确保每次读写都命中内存。

```
uint16_t → Cortex-M4 单条 LDR/STR 指令 → 天然原子
volatile → 阻止编译器优化到寄存器 → 保证跨上下文可见性
不需要锁 → 头尾分离 + 原子读写 + 单生产者单消费者
```

---

## 3. 生产者：USART3 中断服务函数

```c
// esp8266.c
void USART3_IRQHandler(void)
{
    /* 检查 RXNE: 接收缓冲区非空中断标志 */
    if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_RXNE)) {
        uint8_t ch = (uint8_t)(huart3.Instance->DR & 0xFF);   // 读 DR 寄存器（清 RXNE）
        uint16_t next = (uart3_rx_head + 1) % UART3_RX_BUF_SIZE; // 计算下一位置

        if (next != uart3_rx_tail) {      // 缓冲区未满？
            uart3_rx_buf[uart3_rx_head] = ch;  // 存入新字节
            uart3_rx_head = next;              // head 前移
        }
        // 缓冲区满 → 丢弃（不覆盖未读数据）
    }
}
```

### 执行流程

```
ESP8266 TX ──(1字节)──→ USART3 RX ──(RXNE中断)──→ ISR
                                                        │
                                              读 DR → 存入 buf[head] → head++
                                                        │
                                              主任务空闲时 ──→ 批量读取
```

**关键细节：**

1. **清 RXNE 标志：** 读 `DR` 寄存器会自动清除 RXNE。如果不清，中断会反复触发（中断风暴）
2. **满时丢弃：** `(head+1) % SIZE == tail` 表示缓冲区满。此时新字节被丢弃，而不是覆盖未读数据。这比覆盖旧数据更安全——丢一个字节可能只是某帧校验失败，覆盖则可能破坏多帧
3. **无锁：** 中断只写 head，任务只写 tail，互不干扰。中断读 tail 只是判断是否满，读到的值即使稍旧（任务刚读完但还没来得及更新 tail）也只会导致"提前判满"，不会数据错误

---

## 4. 消费者 API

三个函数，全部在任务上下文中调用（不会被中断打断的部分）：

```c
/* 查询缓冲区中可读字节数 */
uint16_t esp8266_rx_available(void)
{
    return (uart3_rx_head - uart3_rx_tail + UART3_RX_BUF_SIZE) % UART3_RX_BUF_SIZE;
}

/* 清空缓冲区（丢弃所有未读数据） */
void esp8266_rx_flush(void)
{
    uart3_rx_tail = uart3_rx_head;  // 读指针追平写指针 → 逻辑清空
}

/* 从缓冲区批量读取数据 */
uint16_t esp8266_rx_read(uint8_t *buf, uint16_t max_len)
{
    uint16_t count = 0;
    while (count < max_len && uart3_rx_tail != uart3_rx_head) {
        buf[count++] = uart3_rx_buf[uart3_rx_tail];
        uart3_rx_tail = (uart3_rx_tail + 1) % UART3_RX_BUF_SIZE;
    }
    return count;  // 返回实际读出的字节数
}
```

**为什么 `available()` 不用锁？** `head` 和 `tail` 都是 `uint16_t`，Cortex-M4 的 16 位读是原子的。即使中断恰好在两次读之间修改了 `head`，最坏情况下返回值略小于实际可用字节数（任务下次再读即可），不会造成数据错误。

---

## 5. 双模切换：关中断直读 vs 缓冲区模式

这是整个设计的**核心巧思**——系统在两种接收模式之间切换，各取所长。

### 模式一：关中断直接轮询（`wifi_send_cmd`）

```c
int wifi_send_cmd(const char *cmd, const char *expected, uint16_t timeout_ms)
{
    static char rx_buf[1024];
    memset(rx_buf, 0, sizeof(rx_buf));

    esp8266_rx_flush();                         // ① 清空环形缓冲区
    HAL_NVIC_DisableIRQ(USART3_IRQn);           // ② 关闭 USART3 中断
    HAL_UART_Transmit(&huart3, cmd, ...);        // ③ 发送 AT 指令

    uint16_t idx = 0;
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < timeout_ms) {
        if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_RXNE)) {
            rx_buf[idx++] = (uint8_t)(huart3.Instance->DR & 0xFF);
            start = HAL_GetTick();              // ④ 每收一个字节就刷新超时计时器
        }
    }
    rx_buf[idx] = '\0';
    HAL_NVIC_EnableIRQ(USART3_IRQn);            // ⑤ 恢复中断
    return (strstr(rx_buf, expected) != NULL) ? 1 : 0;
}
```

**为什么要关中断？** 发送 AT 指令后，需要精准匹配"这条指令的应答"。如果中断也在同时填充环形缓冲区，新数据会混入缓冲区的已有数据中，无法区分哪些字节属于当前应答、哪些是之前的残留。

**为什么要先 flush？** 清空环形缓冲区中的旧数据，防止之前的 AT 应答残留被误读。如果不清，`strstr(rx_buf, "OK")` 可能匹配到上一次指令留下的 `OK`。

**为什么每收到一字节就刷新计时器？** `start = HAL_GetTick()` 实现了**字节间隔超时**（inter-byte timeout），而非总超时。ESP8266 应答可能在 2 秒内断续到达——如果只计时第一次和最后一次，中间停顿 1 秒就会被误判为超时。字节间隔超时保证只要数据还在持续到达就不会超时。

```
时间轴：
发送 AT\r\n ───── 收到 'O' ── 等 800ms ── 收到 'K' ── 等 200ms ── 收到 '\r' ── 等 50ms ── 收到 '\n'
                ↑ start 刷新          ↑ start 刷新         ↑ start 刷新
                
如果用总超时：发送后 2 秒不管有没有收到数据都退出 → 可能漏掉后面的字节（✗）
用字节间隔超时：每次收到字节就重置计时器，只要数据还在来就不超时 → 拿到完整应答（✓）
```

### 模式二：中断后台接收（`check_mqtt_incoming`）

```c
static void check_mqtt_incoming(void)
{
    uint16_t avail = esp8266_rx_available();
    if (!avail) return;                         // 无数据，立即返回

    static uint8_t buf[1024];                   // static 避免栈上分配大数组
    uint16_t n = esp8266_rx_read(buf, sizeof(buf) - 1);
    if (n) process_incoming_mqtt(buf, n);
}
```

MQTT 连接建立后，服务器随时可能推送 CONNACK/PINGRESP/PUBLISH 消息。这些消息**不是对某条 AT 指令的应答**，而是主动推送的。不能关中断轮询（会阻塞整个 WiFi 任务），所以让中断在后台默默接收，任务每 1 秒批量读取一次。

### 双模对比

| 维度 | wifi_send_cmd（关中断直读） | check_mqtt_incoming（缓冲区） |
|------|---------------------------|----------------------------|
| **使用场景** | 发送 AT 指令后等应答 | 后台接收 MQTT 推送消息 |
| **中断状态** | 关闭（`HAL_NVIC_DisableIRQ`） | 开启（ISR 后台填充） |
| **读取方式** | 逐个轮询 DR 寄存器 | 批量从环形缓冲区读出 |
| **阻塞性** | 阻塞（最长 timeout_ms） | 非阻塞（无数据直接返回） |
| **数据来源** | 当前 AT 指令的应答 | 服务器主动推送 |
| **flush 操作** | 发送前先 flush | 不 flush（数据持续累积） |

---

## 6. MQTT 报文解析链路

完整的数据解析链路（从字节到业务逻辑）：

```
ESP8266 ──(TCP)──→ USART3 ──(RXNE中断)──→ 环形缓冲区
                                              │
                           check_mqtt_incoming() 每 1s 调用
                                              │
                              esp8266_rx_read() 批量读取
                                              │
                           process_incoming_mqtt() 逐帧解析
                            ┌─────────┬─────────┐
                            │         │         │
                         0x20      0x30      0xD0
                        CONNACK   PUBLISH   PINGRESP
                        (rc=0)   (命令/属性) (心跳响应)
```

**`process_incoming_mqtt()` 解析逻辑：**

```c
static void process_incoming_mqtt(const uint8_t *data, uint16_t len) {
    uint16_t pos = 0;
    while (pos < len) {
        uint8_t type = data[pos] >> 4;      // 高 4 位 = 报文类型
        // 读取变长剩余长度（1-4 字节）
        uint32_t rl = 0;
        do {
            uint8_t b = data[++pos];
            rl = (rl << 7) | (b & 0x7F);
        } while (b & 0x80);                 // 最高位=1 表示还有后续字节

        switch (type) {
            case 0x20:  // CONNACK → 记录返回码 rc
            case 0x30:  // PUBLISH → 提取 Topic + Payload，打印日志
            case 0xD0:  // PINGRESP → 连接正常
        }
        pos += rl;                          // 跳到下一帧
    }
}
```

**为什么需要手动构建和解析 MQTT 报文？** 本项目用的是 ESP8266 标准 AT 固件（而非 MQTT AT 固件），ESP8266 只提供 TCP 透传（`AT+CIPSEND` 发送原始字节、`+IPD` 接收原始字节），不提供 MQTT 协议的封装/解析。所有 MQTT 报文（CONNECT/PUBLISH/PINGREQ/SUBSCRIBE）都需要在 STM32 端手动构建和解析。这是轻量级嵌入式设备中非常常见的做法。

---

## 7. 踩坑记录

### 坑 1：+IPD 数据与 SEND OK 同帧到达

**现象：** MQTT CONNECT 发送后，`tcp_send_data()` 只匹配 `SEND OK`，CONNACK 报文被忽略。

**原因：** ESP8266 的 `AT+CIPSEND` 应答格式为：

```
SEND OK\r\n\r\n+IPD,4:\x20\x02\x00\x00
```

`SEND OK` 和 `+IPD(CONNACK)` 在**同一帧**中到达。如果只检查 `SEND OK` 而忽略后面的 `+IPD`，CONNACK 就丢了。

**解决：** `tcp_send_data()` 在匹配到 `SEND OK` 后，继续在当前 `rx_buf` 中搜索 `+IPD,`，如果找到就立即解析：

```c
// tcp_send_data() 内部
int ret = (strstr(rx, "SEND OK") != NULL) ? 1 : 0;

/* !! 关键 !! SEND OK 和 +IPD(CONNACK) 可能在同一帧 */
char *ipd = strstr(rx, "+IPD,");
if (ipd) {
    int il = 0;
    sscanf(ipd, "+IPD,%d:", &il);
    char *pl = strchr(ipd, ':');
    if (pl && il > 0) {
        pl++;
        process_incoming_mqtt((uint8_t *)pl, (uint16_t)il);
    }
}
return ret;
```

### 坑 2：环形缓冲区不是"万能缓冲"

环形缓冲区只在**中断接收模式**下工作。`wifi_send_cmd()` 关中断后，所有到达的字节都会丢失（因为 ISR 被屏蔽了，DR 寄存器的数据被覆盖）。这就是为什么 `wifi_send_cmd` 必须在 100ms 任务循环中调用——任务每 100ms 轮一次，`wifi_send_cmd` 阻塞期间（最长 20 秒），高优先级的 NRF24 任务可以抢占，但 USART3 中断被关闭，后台数据会丢失。

**这实际上是个合理的设计取舍：** AT 指令应答期间不需要后台接收（MQTT 推送只会发生在连接建立后），关中断保证应答匹配的准确性。

---

## 8. 常见问题

### Q1：为什么不用 HAL 的 `HAL_UART_Receive_IT()` 而要手写环形缓冲区？

`HAL_UART_Receive_IT()` 需要预先指定接收长度。比如指定 100 字节，收到 100 字节后才触发完成回调。ESP8266 的应答长度完全不可预测——AT 应答可能只有 2 字节（`OK`），TCP 数据可能达 500 字节。手写环形缓冲区可以处理**不定长数据流**，每来 1 字节就存 1 字节，上层随时可以取出任意长度。

### Q2：缓冲区满时丢数据，会不会丢 MQTT 关键报文？

可能。如果 MQTT PUBLISH 报文在缓冲区满时到达，确实会被丢弃。华为云 IoT 平台对 QoS 1 报文有重传机制——服务器未收到 PUBACK 会重发。但在本项目当前实现中，MQTT PUBLISH 使用的是 QoS 0，丢失后不会重传。**增大缓冲区到 2048 字节 + 1 秒一次 `check_mqtt_incoming()` 已经大幅降低了丢数据的概率。**

### Q3：为什么缓冲区要保留 1 字节（head+1 == tail 判满）？

如果不保留，`head == tail` 既可能表示"空"也可能表示"满"。用 `(head+1) % SIZE == tail` 判满，代价是牺牲 1 字节容量，换来空/满判断的 O(1) 复杂度（不需要额外计数器）。在 2048 字节的缓冲区中牺牲 1 字节完全可以接受。

### Q4：`wifi_send_cmd` 的 `rx_buf` 为什么用 `static`？

```c
static char rx_buf[1024];  // 1024 字节，static 分配在 .bss 段而非栈上
```

1024 字节的局部数组如果放在栈上，会大大增加调用者的栈压力。`static` 将其移到 `.bss` 段（全局静态存储区），不占栈空间。代价是函数不可重入——但在本项目单任务架构下无关紧要。

### Q5：为什么不直接在中断中解析 MQTT 报文？

中断服务函数应该尽可能短（嵌入式开发的基本准则）。MQTT 报文解析涉及变长剩余长度解码、多字节比较、字符串匹配，在中断中执行会显著延长中断响应时间，可能导致其他中断丢失。将解析推迟到任务上下文，中断只做"收字节 → 存缓冲区"这一件事（约 10 条指令），耗时 < 1μs。

---

> **一句话总结：** USART3 用中断 + 环形缓冲区解耦 ESP8266 的异步数据到达和任务的批量处理。AT 指令应答期间切换到关中断直读模式保证应答精准匹配；MQTT 推送消息由中断后台接收、任务定期批量取出解析。head/tail 分离的无锁设计在 Cortex-M4 的原子读写支持下无需互斥锁。


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
