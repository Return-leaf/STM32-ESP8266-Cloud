/**
 * @file    wifi.h
 * @brief   ESP8266 WiFi 模块 + OneNET MQTT 配置与函数声明
 */

#ifndef __WIFI_H__
#define __WIFI_H__

#include "main.h"
#include <stdint.h>

/* ========================================================================== */
/*                          硬件配置                                           */
/* ========================================================================== */

/* ESP8266 硬件复位引脚（ATK模块 RST → 开发板 PF6） */
#define ESP8266_RST_PORT   GPIOF
#define ESP8266_RST_PIN    GPIO_PIN_6

/* ========================================================================== */
/*                          WiFi 配置                                          */
/* ========================================================================== */

#define WIFI_SSID          "110"          /* WiFi 热点名称 */
#define WIFI_PWD           "y6239946"     /* WiFi 密码 */

/* ========================================================================== */
/*                          OneNET MQTT 配置                                   */
/* ========================================================================== */

#define ONENET_MQTT_HOST   "mqtts.heclouds.com"   /* MQTT 服务器地址 */
#define ONENET_MQTT_PORT   1883                    /* MQTT 端口 */
#define ONENET_PRODUCT_ID  "q4igTV8l2n"            /* 产品 ID */
#define ONENET_DEVICE_NAME "stm32Wifi"             /* 设备名称 */

/* 设备鉴权信息（包含签名的 Token） */
#define ONENET_DEVICE_KEY  "version=2018-10-31&res=products%2Fq4igTV8l2n%2Fdevices%2Fstm32Wifi&et=1903964915&method=md5&sign=PPQCMNUPAm5%2BEw8WFJaJzA%3D%3D"

/* ========================================================================== */
/*                          缓冲区配置                                         */
/* ========================================================================== */

#define WIFI_MSG_MAX_LEN   256   /* MQTT 消息解析缓冲区最大长度 */

/* ========================================================================== */
/*                          函数声明                                           */
/* ========================================================================== */

/** @brief 初始化 ESP8266（配置复位引脚并执行硬件复位） */
void esp8266_init(void);

/** @brief 发送 AT 指令并检查响应 */
int  wifi_send_cmd(const char *cmd, const char *expected, uint16_t timeout_ms);

/** @brief 测试 ESP8266 AT 指令是否正常 */
int  wifi_test(void);

/** @brief 连接 WiFi 路由器 */
int  wifi_connect_router(void);

/** @brief 初始化 OneNET MQTT 连接并订阅 Topic */
int  OneNET_MQTT_Init(void);

/** @brief 向 OneNET 上报属性值 */
void OneNET_Report_Property(int val);

/** @brief 检查并处理云端下发的 MQTT 消息 */
void OneNET_Handle_Incoming(void);

#endif
