#ifndef __51_COMM_H__
#define __51_COMM_H__

/**
 ****************************************************************************************************
 * @file        51_comm.h
 * @brief       51单片机→STM32 NRF24L01 通信协议定义
 *
 * ===== 通信链路 =====
 * 51 MCU (DS18B20采集 → NRF24L01 TX) —2.4GHz射频→ STM32 (NRF24L01 RX) —解析→ 温度值
 *
 * ===== 温度帧协议 (NRF24L01载荷32字节, 前6字节有效) =====
 * Byte0: 0xAA  帧头(固定)
 * Byte1: 0x01  数据类型(0x01=温度)
 * Byte2: tempH 温度高字节(int16_t, 单位0.1°C, 大端序)
 * Byte3: tempL 温度低字节(例: 205=0x00CD → tempH=0x00, tempL=0xCD → 20.5°C)
 * Byte4: checksum 校验和 = Byte0+Byte1+Byte2+Byte3 (低8位)
 * Byte5: 0x55  帧尾(固定)
 * Byte6-31: 0x00 填充(未使用)
 *
 * ===== 全局变量共享机制 =====
 * g_latest_temperature:    NRF任务(C51_Comm_Poll)写入, LCD/MQTT任务只读, 原子访问无需锁
 * g_last_temp_update_tick: 记录最后收到有效帧的时间戳, 用于NRF超时看门狗(15s无数据→复位)
 ****************************************************************************************************
 */

#include "./SYSTEM/sys/sys.h"
#include <stdint.h>

/* ===== 帧协议常量 ===== */
#define FRAME_HEADER            0xAA    /* 帧头: 标识一帧开始 */
#define FRAME_TAIL              0x55    /* 帧尾: 标识一帧结束 */
#define FRAME_TYPE_TEMP         0x01    /* 数据类型=温度 (可扩展: 0x02=湿度, 0x03=气压) */
#define FRAME_MIN_LEN           6       /* 最小有效帧长度(前6字节) */
#define FRAME_MAX_LEN           32      /* NRF24L01一包最大载荷(32字节) */

extern int16_t  g_latest_temperature;           /* 最新温度值(单位0.1°C), 多任务共享只读 */
extern uint32_t g_last_temp_update_tick;        /* 最后温度更新时间戳(HAL_GetTick返回值, 毫秒) */

void    C51_Comm_Init(void);                                /* 初始化: NRF24L01硬件+RX模式+帧解析器就绪 */
void    C51_Comm_Poll(void);                                /* 轮询: 尝试接收一帧, 有效帧→更新g_latest_temperature */
uint8_t C51_IsTemperatureFresh(uint32_t timeout_ms);        /* 判断温度数据是否在超时时间内有效(1=有效, 0=失效) */

#endif
