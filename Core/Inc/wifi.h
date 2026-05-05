#ifndef __WIFI_H__
#define __WIFI_H__

#include "main.h"

// --- ESP8266 硬件复位引脚 (ATK模块 RST -> 开发板 PF6) ---
#define ESP8266_RST_PORT   GPIOF
#define ESP8266_RST_PIN    GPIO_PIN_6

// --- WiFi 配置 ---
#define WIFI_SSID          "110"
#define WIFI_PWD           "y6239946"

// --- OneNET 配置 ---
#define ONENET_MQTT_HOST   "mqtts.heclouds.com"
#define ONENET_MQTT_PORT   1883
#define ONENET_PRODUCT_ID  "q4igTV8l2n"
#define ONENET_DEVICE_NAME "stm32Wifi"
#define ONENET_DEVICE_KEY  "version=2018-10-31&res=products%2Fq4igTV8l2n%2Fdevices%2Fstm32Wifi&et=1903964915&method=md5&sign=PPQCMNUPAm5%2BEw8WFJaJzA%3D%3D"

// --- ESP8266 接收缓冲区 ---
#define WIFI_RX_BUF_SIZE   512

// 函数声明
void esp8266_init(void);
int  usart3_loopback_test(void);
int  wifi_send_cmd(char *cmd, char *expected, uint16_t wait_ms);
int  wifi_test(void);
int  wifi_connect_router(void);
int  OneNET_MQTT_Init(void);
void OneNET_Report_Wifi(int val);
void OneNET_Report_Event(int val);

#endif
