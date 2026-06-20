/**
 ****************************************************************************************************
 * @file        freertos_demo.c
 * @brief       FreeRTOS 3任务调度 + 华为云 IoT MQTT 温度上报
 *
 * ===== 系统数据流 =====
 * 51 MCU (DS18B20+NRF24 TX) --2.4GHz--> STM32 (NRF24 RX) --ESP8266--> 华为云 IoT
 *
 * ===== 为什么用3个任务? =====
 * 如果所有操作放在一个任务里, WiFi/MQTT 连接时 AT 指令可能阻塞 20 秒,
 * NRF24L01 在此期间无法接收 51 发来的温度数据, 导致丢包
 *
 * ===== 三任务设计 =====
 * vNRF24Task     prio=3,  50ms  | 轮询 NRF24L01, 永不阻塞, 15s无数据自动复位
 * vWiFiMQTTTask  prio=2, 100ms  | 状态机: IDLE->TEST->CONNECT->MQTT->READY
 *                                   READY状态下每秒上报温度, 每60s PING心跳
 * vLCDTask       prio=1,    1s  | 刷新 LCD 温度显示
 *
 * ===== 状态机 WIFI_STATE =====
 * 0=IDLE 1=TESTING(每5s发AT) 2=CONNECTING(每3s连WiFi)
 * 3=MQTT_INIT(TCP+MQTT握手)  4=READY(正常运行)
 *
 * ===== 关键参数 =====
 * WiFi任务栈 1024 words (4KB): huawei_mqtt_connect() 内部栈消耗大
 * NRF 任务 50ms 周期: 远小于 51 的 2.5s 发送间隔
 * MQTT 保活 120s, 每60s发 PINGREQ (留余量)
 ****************************************************************************************************
 */

#include "freertos_demo.h"
#include "./SYSTEM/usart/usart.h"
#include "./BSP/LCD/lcd.h"
#include "./SYSTEM/delay/delay.h"

#include "FreeRTOS.h"
#include "task.h"
#include "./BSP/ESP8266/esp8266.h"
#include "./BSP/NRF24L01/51_comm.h"
#include <stdio.h>

/* ===== WiFi/MQTT 状态机枚举 =====
 * 0=IDLE:      初始状态, 等待任务启动
 * 1=TESTING:   每5秒发送 AT 测试指令, 确认ESP8266串口通信正常
 * 2=CONNECTING:每3秒尝试连接WiFi路由器(SSID+PWD)
 * 3=MQTT_INIT: TCP连接+MQTT CONNECT认证握手
 * 4=READY:     正常运行, 定时上报温度+心跳保活 */
#define WIFI_STATE_IDLE       0
#define WIFI_STATE_TESTING    1
#define WIFI_STATE_CONNECTING 2
#define WIFI_STATE_MQTT_INIT  3
#define WIFI_STATE_READY      4

static volatile int g_wifi_state = 0;           /* WiFi/MQTT连接状态机当前状态, 多任务共享(volatile防编译器优化) */

extern int16_t  g_latest_temperature;           /* 来自51_comm.c, 最新温度值(单位0.1°C), NRF任务写入, LCD/MQTT任务读取 */
extern uint32_t g_last_temp_update_tick;        /* 来自51_comm.c, 最后一次温度更新时间戳(HAL_GetTick), 用于超时判断 */

static void vNRF24Task(void *pv);
static void vWiFiMQTTTask(void *pv);
static void vLCDTask(void *pv);

/* ===== 创建3个FreeRTOS任务 =====
 * 栈大小单位: words(4字节), 256 words = 1KB, 1024 words = 4KB
 * vNRF24Task:    栈256w(1KB), prio=3(最高)     — NRF数据不能丢, 必须最高优先级
 * vWiFiMQTTTask: 栈1024w(4KB), prio=2(中等)    — MQTT握手内部栈消耗大, 需4KB
 * vLCDTask:      栈128w(512B), prio=1(最低)    — LCD刷新是纯UI, 不敏感 */
void freertos_demo(void)
{
    xTaskCreate(vNRF24Task,    "NRF24",  256,  NULL, 3, NULL);
    xTaskCreate(vWiFiMQTTTask, "WiFiMQ", 1024, NULL, 2, NULL);
    xTaskCreate(vLCDTask,      "LCD",    128,  NULL, 1, NULL);

    printf("[SYS] Starting FreeRTOS scheduler...\r\n");
    vTaskStartScheduler();
}

/* ===== NRF24L01 轮询任务 =====
 * 周期: 50ms(远小于51的2.5s发送间隔, 确保不漏包)
 * 优先级: 3(最高) — NRF数据实时性要求最高, 必须优先处理
 * 超时复位: 15s无有效温度数据 → 自动重新初始化NRF24L01(硬件看门狗机制) */
static void vNRF24Task(void *pv)
{
    TickType_t xLastWake = xTaskGetTickCount(); /* 记录起始tick, 用于精确周期调度 */

    for (;;) {
        C51_Comm_Poll();                        /* 尝试从NRF24L01接收一帧: 收到有效温度帧→更新g_latest_temperature */

        /* NRF数据超时检测: WiFi/MQTT已就绪但15s没收到51的数据 → 复位NRF24L01 */
        if (g_wifi_state == WIFI_STATE_READY && !C51_IsTemperatureFresh(15000)) {
            printf("[NRF] Timeout, re-init\r\n");
            lcd_show_string(130, 60, 100, 16, 16, "Reinit", YELLOW);
            C51_Comm_Init();                    /* 重新初始化: SPI复位 + 寄存器重配 + 回读验证 */
            lcd_show_string(130, 60, 100, 16, 16, "OK    ", GREEN);
        }

        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(50)); /* 绝对延时: 确保严格50ms周期, 不受执行时间影响 */
    }
}

/* ===== WiFi/MQTT 状态机任务 =====
 * 周期: 100ms(状态轮询间隔, 足够响应AT指令应答)
 * 优先级: 2(中等) — 比NRF低以保证温度数据不丢, 比LCD高以保证连接及时
 * 3个定时器(lastReport/lastPing/lastIncoming): 在READY状态下独立计时, 互不阻塞 */
static void vWiFiMQTTTask(void *pv)
{
    uint32_t lastStep = 0, lastReport = 0, lastPing = 0, lastIncoming = 0;

    vTaskDelay(pdMS_TO_TICKS(3000));            /* 启动后先等3秒, 让NRF任务先跑起来收集初始数据 */

    printf("[WiFi] Starting ESP8266...\r\n");
    esp8266_init();                             /* ESP8266硬件初始化: RST复位 + USART3(115200) + 环形缓冲区 */
    g_wifi_state = WIFI_STATE_TESTING;          /* 进入状态1: AT指令测试 */
    lastStep = HAL_GetTick();                   /* 记录状态进入时间, 用于超时重试 */

    for (;;) {
        switch (g_wifi_state) {

        /* STATE1: 每5秒发送 AT 测试指令, 确认ESP8266串口通信正常
         * AT指令是ESP8266的命令集(类似Modem的Hayes指令集), 发送"AT\r\n"应返回"OK" */
        case WIFI_STATE_TESTING:
            if (HAL_GetTick() - lastStep > 5000) {
                if (wifi_test()) {
                    printf("[WiFi] AT OK\r\n");
                    lcd_show_string(130, 80, 100, 16, 16, "AT OK", GREEN);
                    g_wifi_state = WIFI_STATE_CONNECTING;   /* AT测试通过 → 进入WiFi连接阶段 */
                }
                lastStep = HAL_GetTick();
            }
            break;

        /* STATE2: 每3秒尝试连接WiFi路由器(AT+CWJAP指令)
         * ESP8266只支持2.4GHz频段, SSID和密码在esp8266.h中配置 */
        case WIFI_STATE_CONNECTING:
            if (HAL_GetTick() - lastStep > 3000) {
                printf("[WiFi] Connecting...\r\n");
                lcd_show_string(130, 80, 100, 16, 16, "Connecting", BLACK);
                if (wifi_connect_router()) {
                    printf("[WiFi] Connected\r\n");
                    lcd_show_string(130, 80, 100, 16, 16, "Connected ", GREEN);
                    g_wifi_state = WIFI_STATE_MQTT_INIT;    /* WiFi已连接 → 进入MQTT连接阶段 */
                } else {
                    printf("[WiFi] Failed, retry 10s\r\n");
                    lcd_show_string(130, 80, 100, 16, 16, "Retry..  ", YELLOW);
                }
                lastStep = HAL_GetTick();
            }
            break;

        /* STATE3: TCP连接(AT+CIPSTART) + MQTT CONNECT认证(手动构建CONNECT报文)
         * huawei_mqtt_connect()内部: TCP建连 → 构建MQTT CONNECT → 等待CONNACK(rc=0成功) */
        case WIFI_STATE_MQTT_INIT:
            if (HAL_GetTick() - lastStep > 2000) {
                printf("[MQTT] Connecting...\r\n");
                lcd_show_string(130, 100, 100, 16, 16, "Connecting", BLACK);
                if (huawei_mqtt_connect()) {
                    printf("[MQTT] Connected\r\n");
                    lcd_show_string(130, 100, 100, 16, 16, "Connected ", GREEN);
                    g_wifi_state = WIFI_STATE_READY;        /* MQTT认证成功 → 进入正常工作状态 */
                    lastReport = HAL_GetTick();             /* 初始化上报定时器 */
                    lastPing = HAL_GetTick();               /* 初始化心跳定时器 */
                    lastIncoming = HAL_GetTick();           /* 初始化入站消息定时器 */
                } else {
                    printf("[MQTT] Failed, retry 10s\r\n");
                    lcd_show_string(130, 100, 100, 16, 16, "Retry..  ", YELLOW);
                }
                lastStep = HAL_GetTick();
            }
            break;

        /* STATE4: 正常工作模式 — 三个独立定时器并行运转
         * 心跳(PINGREQ): 每60s发一次, MQTT保活间隔120s(留1倍余量防止断连)
         * 温度上报(PUBLISH): 每1s检查, 温度30s内有效则发布QoS1报文
         * 入站处理: 每1s检查服务器下发的CONNACK/PINGRESP/PUBLISH消息 */
        case WIFI_STATE_READY:
            /* 心跳: MQTT保活机制, 服务器在1.5倍保活间隔内收不到报文会断开连接 */
            if (HAL_GetTick() - lastPing >= 60000) {
                lastPing = HAL_GetTick();
                huawei_ping();                              /* 发送PINGREQ(2字节: 0xC0, 0x00) */
            }
            /* 温度上报: QoS1(PUBACK确认)保证至少送达一次 */
            if (HAL_GetTick() - lastReport >= 1000) {
                lastReport = HAL_GetTick();
                if (C51_IsTemperatureFresh(30000)) {        /* 温度数据必须在30s内有效 */
                    huawei_report_temperature(g_latest_temperature / 10.0f);  /* 上报到华为云IoT平台 */
                    printf("[MQTT] Temp: %d.%d C\r\n",
                           g_latest_temperature / 10,
                           (g_latest_temperature < 0 ? -g_latest_temperature : g_latest_temperature) % 10);
                }
            }
            /* 入站消息: 处理服务器的CONNACK/PUBLISH/PINGRESP等MQTT报文 */
            if (HAL_GetTick() - lastIncoming >= 1000) {
                lastIncoming = HAL_GetTick();
                huawei_handle_incoming();                   /* 从环形缓冲区读取并解析MQTT报文 */
            }
            break;

        default:
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ===== LCD 温度刷新任务 =====
 * 周期: 1s(温度变化慢, 1秒刷新足够)
 * 优先级: 1(最低) — 纯显示任务, 不影响数据采集和通信
 * 数据来源: 只读 g_latest_temperature(由NRF任务写入), 无需互斥锁 */
static void vLCDTask(void *pv)
{
    char buf[32];
    TickType_t xLastWake = xTaskGetTickCount();

    for (;;) {
        int16_t t = g_latest_temperature;       /* 读取全局温度值(单位0.1°C, 例: 205=20.5°C) */
        snprintf(buf, sizeof(buf), "%d.%d C  ", t / 10, (t < 0 ? -t : t) % 10); /* 格式化为 "XX.X C" */
        lcd_show_string(110, 120, 150, 16, 16, buf, BLUE);   /* 刷新LCD第6行温度显示 */
        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(1000));     /* 精确1秒周期 */
    }
}
