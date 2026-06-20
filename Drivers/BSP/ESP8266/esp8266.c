/**
 ****************************************************************************************************
 * @file        esp8266.c
 * @brief       ESP8266 AT驱动 + MQTT 3.1.1 协议栈 (手动构建报文, 零依赖)
 *
 * ===== 为什么手动构建 MQTT 报文? =====
 * ESP8266 资源有限, 无法运行标准 MQTT 库 (如 paho.mqtt.embedded-c)
 * 本文件手动按 MQTT 3.1.1 协议规范构建二进制报文, 通过 TCP 透传发送
 * 只实现了核心功能: CONNECT, PUBLISH, PINGREQ + CONNACK/PINGRESP 解析
 *
 * ===== MQTT 报文结构 =====
 * 所有 MQTT 报文都由 [固定头] + [剩余长度] + [可变头/载荷] 组成
 * 固定头: 第1字节高4位=报文类型, 低4位=标志位
 *    0x10=CONNECT  0x30=PUBLISH  0xC0=PINGREQ
 * 剩余长度: 变长编码 (每字节低7位, 最高位=1表示还有后续字节)
 *
 * ===== 关键踩坑: +IPD 解析 =====
 * ESP8266 的 AT+CIPSEND 应答格式: "SEND OK\r\n\r\n+IPD,<len>:<data>"
 * CONNACK 报文可能出现在 SEND OK 同一帧中, 必须在 tcp_send_data() 中解析
 ****************************************************************************************************
 */

#include "esp8266.h"
#include "./SYSTEM/delay/delay.h"
#include "./BSP/LCD/lcd.h"
#include "./SYSTEM/usart/usart.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Forward declarations */
static void process_incoming_mqtt(const uint8_t *data, uint16_t len);

/* ========================================================================== */
/*               USART3 环形缓冲区 (中断驱动接收, 生产者-消费者模型)             */
/*                                                                              */
/* === 环形缓冲区原理 ===                                                        */
/* head(写指针): 中断ISR中递增, 每收到1字节+1 (生产者)                            */
/* tail(读指针): 任务上下文中递增, 每读出1字节+1 (消费者)                          */
/* 可读字节数 = (head - tail + SIZE) % SIZE                                     */
/* head==tail → 缓冲区空                                                        */
/* (head+1)%SIZE==tail → 缓冲区满 (保留1字节防混淆)                               */
/*                                                                              */
/* === 为什么用环形缓冲? ===                                                      */
/* ESP8266应答长度不定(CONNECT应答~20B, +IPD数据可达几百B),                        */
/* 中断逐字节存入环形缓冲, 任务空闲时批量取出处理                                    */
/* ========================================================================== */

UART_HandleTypeDef huart3;

#define UART3_RX_BUF_SIZE 2048

static volatile uint8_t  uart3_rx_buf[UART3_RX_BUF_SIZE];
static volatile uint16_t uart3_rx_head = 0;
static volatile uint16_t uart3_rx_tail = 0;

uint16_t esp8266_rx_available(void)
{
    return (uart3_rx_head - uart3_rx_tail + UART3_RX_BUF_SIZE) % UART3_RX_BUF_SIZE;
}

void esp8266_rx_flush(void)
{
    uart3_rx_tail = uart3_rx_head;
}

uint16_t esp8266_rx_read(uint8_t *buf, uint16_t max_len)
{
    uint16_t count = 0;
    while (count < max_len && uart3_rx_tail != uart3_rx_head) {
        buf[count++] = uart3_rx_buf[uart3_rx_tail];
        uart3_rx_tail = (uart3_rx_tail + 1) % UART3_RX_BUF_SIZE;
    }
    return count;
}

/* ===== USART3中断服务函数 =====
 * 触发条件: USART3收到1字节 → RXNE标志置位 → 进入中断
 * 处理: 从DR寄存器读1字节 → 写入环形缓冲区 → head指针前移
 * 安全: (head+1)%SIZE != tail 保证不覆盖未读数据(缓冲区满时丢弃新字节) */
void USART3_IRQHandler(void)
{
    if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_RXNE)) {        /* 检查RXNE: 接收缓冲区非空中断标志 */
        uint8_t ch = (uint8_t)(huart3.Instance->DR & 0xFF);    /* 读DR寄存器低8位(清RXNE标志) */
        uint16_t next = (uart3_rx_head + 1) % UART3_RX_BUF_SIZE; /* 计算head下一位置 */
        if (next != uart3_rx_tail) {                           /* 缓冲区未满? */
            uart3_rx_buf[uart3_rx_head] = ch;                  /* 存入新字节 */
            uart3_rx_head = next;                              /* head前移(生产者) */
        }
        /* 缓冲区满: 静默丢弃(嵌入式系统的典型做法, 避免阻塞ISR) */
    }
}

/* ========================================================================== */
/*           USART3 初始化 (PB10-TX, PB11-RX, 波特率115200, AF=7)               */
/* PB10=USART3_TX(复用推挽)  PB11=USART3_RX(复用推挽)                           */
/* 中断优先级=1(稍高于SysTick), 确保接收不丢字节                                   */
/* ========================================================================== */

static void esp8266_usart3_init(uint32_t baudrate)
{
    __HAL_RCC_USART3_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitTypeDef gpio_init = {0};
    gpio_init.Pin = GPIO_PIN_10 | GPIO_PIN_11;
    gpio_init.Mode = GPIO_MODE_AF_PP;
    gpio_init.Pull = GPIO_PULLUP;
    gpio_init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio_init.Alternate = GPIO_AF7_USART3;
    HAL_GPIO_Init(GPIOB, &gpio_init);

    huart3.Instance = USART3;
    huart3.Init.BaudRate = baudrate;
    huart3.Init.WordLength = UART_WORDLENGTH_8B;
    huart3.Init.StopBits = UART_STOPBITS_1;
    huart3.Init.Parity = UART_PARITY_NONE;
    huart3.Init.Mode = UART_MODE_TX_RX;
    huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    HAL_UART_Init(&huart3);

    HAL_NVIC_SetPriority(USART3_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(USART3_IRQn);
}

/* ========================================================================== */
/*               ESP8266 硬件复位 (RST引脚时序: 低100ms→高2s等待)                */
/* ESP8266手册要求: RST低电平≥100ms触发复位, 拉高后需≥2s完成内部固件启动            */
/* ========================================================================== */

static void esp8266_hw_reset(void)
{
    HAL_GPIO_WritePin(ESP8266_RST_PORT, ESP8266_RST_PIN, GPIO_PIN_RESET); /* RST=0: 开始复位 */
    delay_ms(100);                                  /* 保持低电平100ms(手册最小要求) */
    HAL_GPIO_WritePin(ESP8266_RST_PORT, ESP8266_RST_PIN, GPIO_PIN_SET);   /* RST=1: 结束复位 */
    delay_ms(2000);                                 /* 等待ESP8266固件启动(2秒安全余量) */
}

/* ===== ESP8266总初始化: RST引脚+USART3+硬件复位 =====
 * 调用时机: vWiFiMQTTTask启动后立即调用
 * 注意: USART3使用115200波特率(ESP8266默认), 不是AT指令可改的 */
void esp8266_init(void)
{
    __HAL_RCC_GPIOF_CLK_ENABLE();                   /* 使能GPIOF时钟(RST=PF6) */
    GPIO_InitTypeDef gpio_init = {0};
    gpio_init.Pin = ESP8266_RST_PIN;
    gpio_init.Mode = GPIO_MODE_OUTPUT_PP;           /* 推挽输出: 可驱动高低电平 */
    gpio_init.Pull = GPIO_PULLUP;                   /* 上拉: 默认高电平(不复位) */
    gpio_init.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(ESP8266_RST_PORT, &gpio_init);
    HAL_GPIO_WritePin(ESP8266_RST_PORT, ESP8266_RST_PIN, GPIO_PIN_SET); /* 初始RST=1(正常运行) */

    esp8266_usart3_init(115200);                    /* USART3初始化: PB10-TX, PB11-RX, 115200bps */
    esp8266_hw_reset();                             /* 硬件复位: 确保ESP8266从已知状态启动 */
}

/* ========================================================================== */
/*                      AT 指令发送 (阻塞轮询, 关中断防止冲突)                     */
/* ========================================================================== */

int wifi_send_cmd(const char *cmd, const char *expected, uint16_t timeout_ms)
{
    static char rx_buf[1024];
    memset(rx_buf, 0, sizeof(rx_buf));
    esp8266_rx_flush();

    HAL_NVIC_DisableIRQ(USART3_IRQn);
    HAL_UART_Transmit(&huart3, (uint8_t *)cmd, strlen(cmd), 1000);

    if (expected == NULL) {
        HAL_NVIC_EnableIRQ(USART3_IRQn);
        return 1;
    }

    uint16_t idx = 0;
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < timeout_ms && idx < sizeof(rx_buf) - 1) {
        if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_RXNE)) {
            rx_buf[idx++] = (uint8_t)(huart3.Instance->DR & 0xFF);
            start = HAL_GetTick();
        }
    }
    rx_buf[idx] = '\0';
    HAL_NVIC_EnableIRQ(USART3_IRQn);

    int ret = 0; if (strstr(rx_buf, expected) != NULL) { ret = 1; } /* 简单子串匹配 */
    if (!ret) {
        printf("[AT] FAIL: %s -> %s\r\n", cmd, rx_buf);
    }
    return ret;
}

int wifi_test(void) { return wifi_send_cmd("AT\r\n", "OK", 2000); }

/* ========================================================================== */
/*                      WiFi 连接路由器 (AT+CWJAP)                              */
/* ========================================================================== */

int wifi_connect_router(void)
{
    char cmd[128];

    wifi_send_cmd("ATE0\r\n", "OK", 1000);         /* 关闭回显 */
    wifi_send_cmd("AT+CIPMUX=0\r\n", "OK", 2000);  /* 单连接模式 (CIPSEND 不需要连接ID) */
    delay_ms(200);

    snprintf(cmd, sizeof(cmd), "AT+CWMODE=1\r\n"); /* Station 模式 (连接路由器) */
    if (!wifi_send_cmd(cmd, "OK", 3000)) { printf("[WiFi] CWMODE fail\r\n"); return 0; }
    delay_ms(500);

    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PWD);
    printf("[WiFi] Connecting to %s ...\r\n", WIFI_SSID);
    if (!wifi_send_cmd(cmd, "OK", 15000)) { printf("[WiFi] Connect FAIL!\r\n"); return 0; }

    printf("[WiFi] Connected!\r\n");
    return 1;
}

/* ========================================================================== */
/*                      MQTT CONNECT 报文构建 (MQTT 3.1.1)                     */
/*                                                                             */
/*  报文结构: [0x10] [剩余长度(变长)] [协议名"MQTT"(6B)] [协议级别4]              */
/*            [连接标志0xC2] [保活120s] [ClientID] [Username] [Password]        */
/*                                                                             */
/*  连接标志 0xC2 = 1100 0010                                                   */
/*    bit7=1: 含用户名  bit6=1: 含密码  bit1=1: 清除会话                          */
/* ========================================================================== */

static uint16_t mqtt_build_connect(uint8_t *buf, uint16_t buf_size,
    const char *client_id, const char *username, const char *password)
{
    uint16_t cid_len = (uint16_t)strlen(client_id);
    uint16_t un_len  = (uint16_t)strlen(username);
    uint16_t pw_len  = (uint16_t)strlen(password);
    uint16_t payload_len = 2 + cid_len + 2 + un_len + 2 + pw_len;
    uint16_t rem = 10 + payload_len;
    uint16_t pos = 0;

    buf[pos++] = 0x10;  /* 固定头: CONNECT */

    /* 剩余长度: 变长编码 (每字节低7位, 最高位=1表示还有下一字节) */
    {
        uint16_t r = rem;
        do {
            uint8_t b = r & 0x7F;
            r >>= 7;
            if (r) b |= 0x80;
            buf[pos++] = b;
        } while (r);
    }

    /* 可变头: 协议名 "MQTT" + 协议级别 4 */
    buf[pos++] = 0x00; buf[pos++] = 0x04;
    buf[pos++] = 'M';  buf[pos++] = 'Q';
    buf[pos++] = 'T';  buf[pos++] = 'T';
    buf[pos++] = 0x04;    /* MQTT 3.1.1 */
    buf[pos++] = 0xC2;    /* 连接标志 */
    buf[pos++] = 0x00;
    buf[pos++] = 0x78;    /* 保活 120 秒 */

    /* 载荷: ClientID + Username + Password */
    buf[pos++] = (cid_len >> 8) & 0xFF; buf[pos++] = cid_len & 0xFF;
    memcpy(&buf[pos], client_id, cid_len); pos += cid_len;

    buf[pos++] = (un_len >> 8) & 0xFF; buf[pos++] = un_len & 0xFF;
    memcpy(&buf[pos], username, un_len); pos += un_len;

    buf[pos++] = (pw_len >> 8) & 0xFF; buf[pos++] = pw_len & 0xFF;
    memcpy(&buf[pos], password, pw_len); pos += pw_len;

    return pos;
}

/* ========================================================================== */
/*                      MQTT PUBLISH 报文构建                                    */
/*  格式: [0x30|QoS<<1] [剩余长度] [Topic] [PacketID(QoS>0)] [Payload]         */
/* ========================================================================== */

static uint16_t mqtt_build_publish(uint8_t *buf, uint16_t buf_size,
    const char *topic, const uint8_t *payload, uint16_t plen, uint8_t qos)
{
    uint16_t tlen = (uint16_t)strlen(topic);
    uint16_t rem = 2 + tlen + plen + (qos ? 2 : 0);
    uint16_t pos = 0;

    buf[pos++] = 0x30 | ((qos & 3) << 1);
    {
        uint16_t r = rem;
        do { uint8_t b = r & 0x7F; r >>= 7; if (r) b |= 0x80; buf[pos++] = b; } while (r);
    }

    buf[pos++] = (tlen >> 8) & 0xFF;
    buf[pos++] = tlen & 0xFF;
    memcpy(&buf[pos], topic, tlen);
    pos += tlen;

    if (qos) { buf[pos++] = 0x00; buf[pos++] = 0x01; }  /* PacketID (QoS>0) */

    if (plen) { memcpy(&buf[pos], payload, plen); pos += plen; }

    return pos;
}

/* MQTT PINGREQ: 固定 0xC0 0x00 */
static void mqtt_build_pingreq(uint8_t buf[2]) { buf[0] = 0xC0; buf[1] = 0x00; }

/* ========================================================================== */
/*                      TCP 数据透传 (AT+CIPSEND)                               */
/* ========================================================================== */

static int tcp_send_data(const uint8_t *data, uint16_t len)
{
    char cmd[32];
    static char rx[512];
    uint16_t idx;
    uint32_t start;
    int ret;
    char *ipd;

    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d\r\n", (int)len);
    if (!wifi_send_cmd(cmd, ">", 5000)) {
        printf("[TCP] CIPSEND failed\r\n");
        return 0;
    }

    /* 发送前清空缓冲, 防止旧数据干扰 */
    esp8266_rx_flush();
    HAL_NVIC_DisableIRQ(USART3_IRQn);
    HAL_UART_Transmit(&huart3, (uint8_t *)data, len, 5000);
    HAL_NVIC_EnableIRQ(USART3_IRQn);

    idx = 0;
    start = HAL_GetTick();
    memset(rx, 0, sizeof(rx));
    while ((HAL_GetTick() - start) < 5000 && idx < sizeof(rx) - 1) {
        if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_RXNE)) {
            rx[idx++] = (uint8_t)(huart3.Instance->DR & 0xFF);
            start = HAL_GetTick();
        }
    }
    rx[idx] = '\0';
    ret = (strstr(rx, "SEND OK") != NULL) ? 1 : 0;

    printf("[TCP_RAW] %s\r\n", rx);
    if (!ret) { printf("[TCP] Send timeout, rx: %s\r\n", rx); return 0; }

    /* !! 关键 !! SEND OK 和 +IPD(CONNACK) 可能在同一帧, 必须解析 */
    ipd = strstr(rx, "+IPD,");
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
}

/* ========================================================================== */
/*                   MQTT 入站报文解析 (从环形缓冲区读取+解析)                     */
/*                                                                              */
/* === 支持的报文类型 ===                                                         */
/* 0x20=CONNACK:   连接确认(rc=0成功)                                            */
/* 0x30=PUBLISH:   服务器下发消息(华为云命令/属性设置)                              */
/* 0xD0=PINGRESP:  心跳响应(收到即表示连接正常)                                    */
/*                                                                              */
/* === CONNACK rc码速查 (MQTT 3.1.1规范 Table 3.1) ===                          */
/* rc=0: 连接已接受           rc=1: 协议版本不支持                                 */
/* rc=2: ClientID被拒绝       rc=3: 服务器不可用                                  */
/* rc=4: 用户名或密码错误      rc=5: 未授权(没有访问权限)                           */
/* ========================================================================== */

static int g_connack_rc = -1;                      /* 全局CONNACK返回码: -1=未收到, 0=成功, 1-5=失败原因(见上方速查表) */

/* ===== MQTT报文解析器(从TCP透传数据中提取MQTT报文) =====
 * 一次可能包含多个报文(如+SEND OK帧中带CONNACK)
 * 解析循环: 读类型→读剩余长度→读载荷→根据类型分发处理 */
static void process_incoming_mqtt(const uint8_t *data, uint16_t len)
{
    uint16_t pos = 0;

    while (pos < len) {
        if (pos + 1 >= len) break;                  /* 至少需要1字节类型 */
        uint8_t type = data[pos] & 0xF0;             /* 提取高4位=MQTT报文类型 */
        pos++;

        /* 解析剩余长度(变长编码, 最多4字节) */
        uint32_t rl = 0;
        int shift = 0;
        while (pos < len && shift < 28) {           /* 最多4字节, shift最大21(7*3) */
            uint8_t b = data[pos++];
            rl |= (uint32_t)(b & 0x7F) << shift;    /* 低7位是数值, 拼接到rl */
            shift += 7;
            if (!(b & 0x80)) break;                 /* 最高位=0→编码结束 */
        }

        if (pos + (uint16_t)rl > len) break;         /* 报文不完整(跨帧), 安全退出 */

        if (type == 0x20) {  /* CONNACK: 连接确认 */
            uint8_t rc = (rl >= 2) ? data[pos + 1] : 0xFF; /* 剩余长度≥2才有rc字段 */
            g_connack_rc = rc;                       /* 更新全局CONNACK返回码 */
            printf("[MQTT] CONNACK rc=%d\r\n", rc);
            pos += (uint16_t)rl;
        } else if (type == 0x30) {  /* PUBLISH: 服务器下发消息 */
            uint16_t tlen = (data[pos] << 8) | data[pos + 1]; /* Topic长度(大端序2字节) */
            char topic[128] = {0};
            if (tlen < sizeof(topic)) memcpy(topic, &data[pos + 2], tlen); /* 提取Topic字符串 */
            pos += (uint16_t)rl;
        } else if (type == 0xD0) {  /* PINGRESP: 心跳响应 */
            pos += (uint16_t)rl;                     /* PINGRESP无载荷, 跳过即可 */
        } else {
            pos += (uint16_t)rl;                     /* 未知类型: 跳过(不处理但也不崩溃) */
        }
    }
}

/* ===== 检查环形缓冲区中是否有MQTT入站报文并解析 =====
 * 非阻塞: 环形缓冲为空→直接返回
 * 通常在PING之后或定时轮询时调用 */
static void check_mqtt_incoming(void)
{
    uint16_t avail = esp8266_rx_available();
    if (!avail) return;                             /* 无数据可读, 立即返回 */

    static uint8_t buf[1024];                       /* static避免栈上分配大数组 */
    uint16_t n = esp8266_rx_read(buf, sizeof(buf) - 1); /* 批量读出所有可用字节 */
    if (n) process_incoming_mqtt(buf, n);           /* 解析MQTT报文 */
}

/* ========================================================================== */
/*                 华为云 IoT MQTT 连接 (TCP建连 + MQTT认证握手)                 */
/*                                                                              */
/* === 连接流程 ===                                                              */
/* Step1: AT+CIPSTART → TCP三次握手 → 等待"CONNECT"应答(CIPSTART成功后ESP8266返回CONNECT+OK, 匹配OK更可靠)                 */
/* Step2: 构建MQTT CONNECT报文(ClientID+Username+Password)                      */
/* Step3: AT+CIPSEND发送CONNECT → 在SEND OK帧中解析CONNACK                     */
/* Step4: 轮询等待g_connack_rc更新(最多15s, 每500ms检查一次)                      */
/* ========================================================================== */

int huawei_mqtt_connect(void)
{
    char cmd[160];
    uint8_t pkt[MQTT_BUF_SIZE];
    uint16_t pl;
    int i;

    /* Step1: TCP连接 (AT+CIPSTART="TCP","host",1883) */
    lcd_show_string(130, 100, 200, 16, 16, "TCP...    ", BLACK);
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", HUAWEI_MQTT_HOST, HUAWEI_MQTT_PORT);
    printf("[HUAWEI] TCP %s:%d\r\n", HUAWEI_MQTT_HOST, HUAWEI_MQTT_PORT);
    if (!wifi_send_cmd(cmd, "CONNECT", 20000)) {    /* 20s超时(云服务器可能较远) */
        printf("[HUAWEI] TCP FAIL\r\n");
        lcd_show_string(130, 100, 200, 16, 16, "TCP Fail  ", RED);
        return 0;
    }
    delay_ms(500);                                  /* 等待ESP8266切换为透传模式 */

    /* Step2: 构建MQTT CONNECT报文(手动按MQTT 3.1.1协议拼接二进制) */
    lcd_show_string(130, 100, 200, 16, 16, "MQTT...   ", BLACK);
    printf("[HUAWEI] MQTT connect...\r\n");
    pl = mqtt_build_connect(pkt, sizeof(pkt), HUAWEI_CLIENT_ID, HUAWEI_DEVICE_ID, HUAWEI_DEVICE_PWD);
    printf("[HUAWEI] CONNECT %u bytes\r\n", (unsigned int)pl);
    g_connack_rc = -1;                              /* 重置CONNACK返回码(-1=等待中) */

    /* Step3: 通过TCP透传发送CONNECT报文 */
    if (!tcp_send_data(pkt, pl)) {
        printf("[HUAWEI] Send fail\r\n");
        lcd_show_string(130, 100, 200, 16, 16, "Send Fail ", RED);
        return 0;
    }

    /* Step4: 轮询等待CONNACK(最多15s=30次×500ms) */
    for (i = 0; i < 30; i++) {
        delay_ms(500);
        check_mqtt_incoming();                      /* 检查环形缓冲中是否有CONNACK */
        if (g_connack_rc >= 0) break;               /* 收到CONNACK(rc≥0)→退出轮询 */
    }

    if (g_connack_rc == 0) {                        /* rc=0: 连接成功 */
        printf("[HUAWEI] Connected!\r\n");
        lcd_show_string(130, 100, 200, 16, 16, "Connected ", GREEN);
        return 1;
    }

    printf("[HUAWEI] Fail rc=%d\r\n", g_connack_rc);
    lcd_show_string(130, 100, 200, 16, 16, "Auth Fail ", RED);
    return 0;
}

/* ========================================================================== */
/*           温度上报 (构建华为云IoT JSON格式 + MQTT PUBLISH QoS1)               */
/*                                                                              */
/* === 华为云IoT属性上报JSON格式 ===                                              */
/* {"services":[{"service_id":"temptest","properties":{"temperature":"25.3"}}]}*/
/* service_id必须与华为云平台"产品模型"中定义的服务ID一致                            */
/*                                                                              */
/* === 切换方法 ===                                                            */
/* 1. 修改esp8266.h中的HUAWEI_TOPIC_REPORT为"stm32/temperature"                  */
/* 2. 修改JSON格式为简单格式: {"services":[{"service_id":"temptest","properties":{"temperature":"25.3"}}]}                               */
/* ========================================================================== */

void huawei_report_temperature(float temp)
{
    char json[256];
    static uint8_t pkt[MQTT_BUF_SIZE];              /* static: 复用缓冲区, 节省栈空间(注意: 非线程安全, 单任务使用OK) */

    int t_int = (int)(temp * 10);                   /* 温度转0.1°C整数: 25.3°C → 253 */
    int t_deg = t_int / 10;                         /* 整数部分: 25 */
    int t_dec = (t_int < 0 ? -t_int : t_int) % 10;  /* 小数部分(取绝对值): 3 */

    /* 构建华为云IoT属性上报JSON */
    snprintf(json, sizeof(json), "{\"services\":[{\"service_id\":\"temptest\",\"properties\":{\"temperature\":\"%d.%d\"}}]}", t_deg, t_dec);

    printf("[PUBLISH] %s -> %s\r\n", HUAWEI_TOPIC_REPORT, json);

    /* 构建MQTT PUBLISH报文(QoS1, 带PacketID=1, 期望PUBACK确认) */
    uint16_t pl = mqtt_build_publish(pkt, sizeof(pkt), HUAWEI_TOPIC_REPORT,
                                      (uint8_t *)json, (uint16_t)strlen(json), 1);
    if (!tcp_send_data(pkt, pl)) {                  /* TCP透传发送PUBLISH报文 */
        printf("[PUBLISH] Send fail\r\n");
    }
}

/* ===== 处理入站MQTT报文(由vWiFiMQTTTask每1s调用一次) ===== */
void huawei_handle_incoming(void) { check_mqtt_incoming(); }

/* ===== 发送MQTT心跳(PINGREQ)保持连接活跃 =====
 * PINGREQ报文固定2字节: [0xC0, 0x00] (类型=0xC0, 剩余长度=0)
 * 服务器收到后必须回复PINGRESP([0xD0, 0x00])
 * 调用间隔60s, MQTT保活120s, 留有1倍余量 */
void huawei_ping(void)
{
    uint8_t pkt[2];
    mqtt_build_pingreq(pkt);                        /* 构建PINGREQ: pkt={0xC0, 0x00} */
    tcp_send_data(pkt, 2);                          /* 通过TCP透传发送 */
    delay_ms(500);                                  /* 等待服务器响应 */
    check_mqtt_incoming();                          /* 检查是否有PINGRESP或其他入站消息 */
}
