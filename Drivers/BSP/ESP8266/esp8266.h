/**
 ****************************************************************************************************
 * @file        esp8266.h
 * @brief       ESP8266 WiFi + MQTT 配置与 API 声明
 *
 * ===== 模块介绍 =====
 * ATK-MW8266D (ESP8266内核) 通过 USART3 串口与 STM32 通信
 * 使用 AT 指令集控制 (类似拨号 Modem), 支持 TCP/UDP 透传
 * 本项目用 TCP 透传模式发送 MQTT 报文到华为云 IoT 平台
 *
 * ===== 硬件接线 =====
 * PB10(USART3_TX) -> ESP8266 RX   PB11(USART3_RX) -> ESP8266 TX
 * PF6(GPIO输出) -> ESP8266 RST    波特率: 115200
 *
 * ===== MQTT 连接流程 =====
 * 1. AT+CIPSTART="TCP","host",1883  (TCP连接服务器)
 * 2. 发送 MQTT CONNECT 报文 (ClientID + Username + Password 认证)
 * 3. 服务器返回 CONNACK (rc=0表示成功)
 * 4. 每60秒发送 PINGREQ 心跳保活
 *
 * ===== 华为云 vs EMQX 切换 =====
 * 只需修改下面的 HUAWEI_* 宏为 EMQX 的地址/认证信息即可
 ****************************************************************************************************
 */

#ifndef __ESP8266_H
#define __ESP8266_H

#include "./SYSTEM/sys/sys.h"
#include <stdint.h>

/* ==================== ESP8266硬件引脚 ====================
 * RST: PF6(推挽输出), 低电平复位(≥100ms)后拉高 */
#define ESP8266_RST_PORT   GPIOF
#define ESP8266_RST_PIN    GPIO_PIN_6

/* ==================== WiFi路由器配置 ====================
 * ESP8266只支持2.4GHz频段, 不支持5GHz
 * 修改方法: 改SSID为目标WiFi名, PWD为密码 */
#define WIFI_SSID          "YOUR_WIFI_SSID"           /* WiFi名称(SSID), 支持中文但需GBK编码 */
#define WIFI_PWD           "YOUR_WIFI_PASSWORD"      /* WiFi密码, 明文存储(教学项目, 正式产品应加密) */

/* ==================== 华为云 IoT MQTT 连接参数 ====================
 *
 * === 端口说明 ===
 * 1883: TCP明文传输, ESP8266直接支持, 无需证书(本项目使用)
 * 8883: SSL/TLS加密传输, ESP8266需额外加载CA证书, 内存消耗大
 *
 * === HUAWEI_DEVICE_PWD 说明 ===
 * 这是平台为设备生成的 MQTT 连接密码(MQTT CONNECT报文的Password字段)
 * ≠ 设备密钥(Device Secret), 设备密钥用于平台API认证, 不用于MQTT
 * 获取方式: 华为云IoTDA控制台 → 设备详情 → MQTT连接参数
 *
 * === HUAWEI_CLIENT_ID 说明 ===
 * 格式: {设备ID}_{序号}_{序号}_{时间戳}
 * 每个MQTT连接必须唯一, 同一ClientID重复连接会导致旧连接被踢下线
 *
 * === 切换到EMQX ===
 * 只需修改HUAWEI_MQTT_HOST为EMQX地址(如<EMQX_SERVER_IP>),
 * HUAWEI_DEVICE_ID/HUAWEI_DEVICE_PWD改为EMQX的用户名/密码即可 */
#define HUAWEI_MQTT_HOST   "YOUR_MQTT_HOST"
#define HUAWEI_MQTT_PORT   1883
#define HUAWEI_DEVICE_ID   "YOUR_DEVICE_ID"     /* MQTT Username, 平台生成 */
#define HUAWEI_DEVICE_PWD  "YOUR_DEVICE_PASSWORD" /* MQTT Password */
#define HUAWEI_CLIENT_ID   "YOUR_CLIENT_ID"   /* MQTT ClientID, 必须唯一 */

/* ==================== 华为云 IoT Topic 说明 ====================
 *
 * === Topic 层级规则 ===
 * MQTT Topic用"/"分隔层级, 例: "stm32/temperature/data"
 * 发布(PUBLISH): Topic不能包含通配符 # 和 + (它们是订阅专用!)
 * 订阅(SUBSCRIBE): 可以用 # 匹配所有子级, + 匹配单级
 *    例: "stm32/+/data" 匹配 "stm32/temp/data" 和 "stm32/humi/data"
 *        "stm32/#" 匹配 "stm32/..." 下所有Topic
 *
 * === $oc 前缀 ===
 * $oc 是华为云IoT平台的特殊主题前缀, 所有设备数据交互必须使用
 * 格式: $oc/devices/YOUR_DEVICE_ID/sys/properties/report (属性上报)
 *       $oc/devices/YOUR_DEVICE_ID/sys/commands/# (命令下发订阅)
 */
#define HUAWEI_TOPIC_REPORT "$oc/devices/YOUR_DEVICE_ID/sys/properties/report"

#define WIFI_MSG_MAX_LEN   512     /* AT指令应答缓冲区最大长度 */
#define MQTT_BUF_SIZE      512     /* MQTT报文构建缓冲区大小(CONNECT~158字节, PUBLISH~200字节) */

/* ==================== API 函数声明 ==================== */

/* USART3环形缓冲区操作(中断驱动接收) */
uint16_t esp8266_rx_available(void);                    /* 获取环形缓冲区中可读字节数 */
void     esp8266_rx_flush(void);                        /* 清空环形缓冲区(丢弃未读数据) */
uint16_t esp8266_rx_read(uint8_t *buf, uint16_t max_len); /* 从环形缓冲区读最多max_len字节 */

/* ESP8266初始化与控制 */
void esp8266_init(void);                                /* 硬件初始化: RST复位 + USART3(115200, PB10/PB11) + 环形缓冲 */

/* WiFi AT指令 */
int  wifi_send_cmd(const char *cmd, const char *expected, uint16_t timeout_ms); /* 发AT指令+等待期望应答, 超时返回0 */
int  wifi_test(void);                                   /* AT测试: 发送"AT\r\n", 期望"OK"(验证串口通信) */
int  wifi_connect_router(void);                         /* 连接WiFi: AT+CWJAP(SSID+PWD), 非阻塞(单次尝试) */

/* 华为云MQTT操作 */
int  huawei_mqtt_connect(void);                         /* MQTT连接: TCP建连→发送CONNECT→等待CONNACK(rc=0成功) */
void huawei_report_temperature(float temp);             /* 温度上报: 构建JSON+MQTT PUBLISH(QoS1), TCP透传发送 */
void huawei_handle_incoming(void);                      /* 入站处理: 从环形缓冲读取并解析MQTT报文(CONNACK/PUBLISH等) */
void huawei_ping(void);                                 /* 心跳保活: 发送PINGREQ(2字节: 0xC0,0x00), 每60s调用一次 */

#endif
