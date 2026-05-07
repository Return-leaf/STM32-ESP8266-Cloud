/**
 * @file    wifi.h
 * @brief   ESP8266 WiFi 模块 + OneNET MQTT 配置宏定义与函数声明
 *
 * 本文件集中管理所有 WiFi 和 MQTT 的可配置参数，修改这些宏即可适配
 * 不同的路由器或 OneNET 设备，无需改动 .c 文件。
 */

/* 防止头文件被重复包含 */
#ifndef __WIFI_H__
#define __WIFI_H__

#include "main.h"    /* HAL 库基础类型 */
#include <stdint.h>  /* uint16_t 等标准整数类型 */

/* ========================================================================== */
/*                          硬件配置                                           */
/* ========================================================================== */

/* ESP8266 硬件复位引脚 */
#define ESP8266_RST_PORT   GPIOF       /* RST 引脚所在 GPIO 端口（PF6） */
#define ESP8266_RST_PIN    GPIO_PIN_6  /* RST 引脚位号（拉低复位，高电平运行） */

/* ========================================================================== */
/*                          WiFi 配置                                          */
/* ========================================================================== */

#define WIFI_SSID          "wifi名字"         /* 要连接的 WiFi 热点名称（SSID） */
#define WIFI_PWD           "wifi密码"   /* WiFi 热点密码 */

/* ========================================================================== */
/*                          OneNET MQTT 配置                                   */
/* ========================================================================== */

#define ONENET_MQTT_HOST   "mqtts.heclouds.com"   /* OneNET MQTT 服务器域名 */
#define ONENET_MQTT_PORT   1883                    /* MQTT 协议端口（非加密 1883） */
#define ONENET_PRODUCT_ID  "设备的ID(一串数字英文组成)"            /* OneNET 产品 ID */
#define ONENET_DEVICE_NAME "你的命名的设备名称"             /* OneNET 设备名称 */

/*
 * 设备鉴权信息（签名字符串）
 * 格式：version=2018-10-31&res=products/{pid}/devices/{device}&et={过期时间}&method=md5&sign={签名}
 * 该 Token 由 OneNET 平台的 Token 生成工具使用设备密钥计算得到
 */
#define ONENET_DEVICE_KEY  "Token_ID"  /* 设备密钥（Token）字符串，需替换为实际值 */

/* ========================================================================== */
/*                          缓冲区配置                                         */
/* ========================================================================== */

#define WIFI_MSG_MAX_LEN   256   /* MQTT Topic 和 Payload 解析缓冲区的最大长度 */

/* ========================================================================== */
/*                          函数声明                                           */
/* ========================================================================== */

/** @brief 初始化 ESP8266 硬件（配置 PF6 复位引脚 + 执行硬件复位） */
void esp8266_init(void);

/**
 * @brief  发送 AT 指令并等待期望的响应
 * @param  cmd         AT 指令字符串（需包含 \r\n）
 * @param  expected    期望在响应中出现的字符串（如 "OK"），NULL 表示不检查
 * @param  timeout_ms  等待响应的超时时间（毫秒）
 * @return 1=收到期望响应，0=超时或未收到
 */
int  wifi_send_cmd(const char *cmd, const char *expected, uint16_t timeout_ms);

/** @brief 发送 "AT\r\n" 测试 ESP8266 是否正常（失败自动重试一次） */
int  wifi_test(void);

/** @brief 连接 WiFi 路由器：设置 Station 模式 → 发送 AT+CWJAP */
int  wifi_connect_router(void);

/** @brief 通过 DNS 解析域名并建立 MQTT 连接，订阅属性设置/获取/上报回复 3 个 Topic */
int  OneNET_MQTT_Init(void);

/** @brief 通过 AT+MQTTPUBRAW 向 OneNET 上报 wifi 属性值 */
void OneNET_Report_Property(int val);

/** @brief 检查环形缓冲区中是否有 +MQTTSUBRECV 消息，有则分发处理 */
void OneNET_Handle_Incoming(void);

#endif /* __WIFI_H__ */
