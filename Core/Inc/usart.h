/**
 * @file    usart.h
 * @brief   USART1 / USART2 初始化及环形缓冲区 API 声明
 */

#ifndef __USART_H__
#define __USART_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* USART 句柄（extern，供 HAL 库和 printf 使用） */
extern UART_HandleTypeDef huart1;  /* 调试串口 */
extern UART_HandleTypeDef huart2;  /* ESP8266 串口 */

/* USART 初始化函数 */
void MX_USART1_UART_Init(void);  /* USART1 初始化（printf） */
void MX_USART2_UART_Init(void);  /* USART2 初始化（ESP8266） */

/* ========================================================================== */
/*              USART2 环形缓冲区 API（中断接收模式）                           */
/* ========================================================================== */

/** @brief 查询环形缓冲区中的可读字节数 */
uint16_t uart2_rx_available(void);

/** @brief 清空环形缓冲区 */
void     uart2_rx_flush(void);

/** @brief 从环形缓冲区读取数据 */
uint16_t uart2_rx_read(uint8_t *buf, uint16_t max_len);

#ifdef __cplusplus
}
#endif

#endif
