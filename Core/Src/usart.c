/**
 * @file    usart.c
 * @brief   USART1 / USART2 初始化及 USART2 中断接收
 *
 * USART1（PA9-TX, PA10-RX）：用于 printf 调试输出，115200 波特率
 * USART2（PA2-TX, PA3-RX）：用于 ESP8266 AT 指令通信，115200 波特率
 *
 * USART2 使用中断方式接收数据，存入 1024 字节的环形缓冲区。
 * 主程序通过 uart2_rx_available/uart2_rx_read 读取缓冲区中的数据。
 */

#include "usart.h"
#include <string.h>

/* USART 句柄 */
UART_HandleTypeDef huart1;  /* printf 输出 */
UART_HandleTypeDef huart2;  /* ESP8266 通信 */

/* ========================================================================== */
/*                     USART2 环形缓冲区（中断接收）                            */
/* ========================================================================== */

#define UART2_RX_BUF_SIZE 1024  /* 环形缓冲区大小 */

static volatile uint8_t  uart2_rx_buf[UART2_RX_BUF_SIZE];  /* 缓冲区 */
static volatile uint16_t uart2_rx_head = 0;  /* 写入位置（中断更新） */
static volatile uint16_t uart2_rx_tail = 0;  /* 读取位置（主程序更新） */

/* ========================================================================== */
/*                          USART 初始化                                       */
/* ========================================================================== */

/**
 * @brief  USART1 初始化（printf 调试输出）
 *         PA9=TX, PA10=RX, 115200-8N1
 */
void MX_USART1_UART_Init(void)
{
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  HAL_UART_Init(&huart1);
}

/**
 * @brief  USART2 初始化（ESP8266 AT 指令通信）
 *         PA2=TX, PA3=RX, 115200-8N1
 */
void MX_USART2_UART_Init(void)
{
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  HAL_UART_Init(&huart2);
}

/* ========================================================================== */
/*                          MSP 底层初始化                                      */
/* ========================================================================== */

/**
 * @brief  UART 底层 GPIO 和时钟初始化（由 HAL_UART_Init 自动调用）
 */
void HAL_UART_MspInit(UART_HandleTypeDef* uartHandle)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  if(uartHandle->Instance==USART1)
  {
    /* USART1：PA9(TX), PA10(RX) */
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitStruct.Pin = GPIO_PIN_9|GPIO_PIN_10;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
  }
  else if(uartHandle->Instance==USART2)
  {
    /* USART2：PA2(TX), PA3(RX) */
    __HAL_RCC_USART2_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitStruct.Pin = GPIO_PIN_2|GPIO_PIN_3;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* 使能 USART2 中断（优先级 1,0） */
    HAL_NVIC_SetPriority(USART2_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
  }
}

/* ========================================================================== */
/*                          USART2 中断处理                                    */
/* ========================================================================== */

/**
 * @brief  USART2 中断处理程序
 *
 * 每收到一个字节（RXNE 标志置位），将数据存入环形缓冲区。
 * 直接读取 DR 寄存器获取数据并清除 RXNE 标志。
 */
void USART2_IRQHandler(void)
{
    if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE)) {
        uint8_t ch = (uint8_t)(huart2.Instance->DR & 0xFF);  /* 读取数据，清除 RXNE */
        uint16_t next = (uart2_rx_head + 1) % UART2_RX_BUF_SIZE;
        if (next != uart2_rx_tail) {  /* 缓冲区未满才写入 */
            uart2_rx_buf[uart2_rx_head] = ch;
            uart2_rx_head = next;
        }
    }
}

/* ========================================================================== */
/*                          环形缓冲区操作 API                                  */
/* ========================================================================== */

/**
 * @brief  查询环形缓冲区中的可读字节数
 * @return 可读字节数
 */
uint16_t uart2_rx_available(void)
{
    return (uart2_rx_head - uart2_rx_tail + UART2_RX_BUF_SIZE) % UART2_RX_BUF_SIZE;
}

/**
 * @brief  清空环形缓冲区（将读指针追上写指针）
 */
void uart2_rx_flush(void)
{
    uart2_rx_tail = uart2_rx_head;
}

/**
 * @brief  从环形缓冲区读取数据
 * @param  buf      目标缓冲区
 * @param  max_len  最多读取的字节数
 * @return 实际读取的字节数
 */
uint16_t uart2_rx_read(uint8_t *buf, uint16_t max_len)
{
    uint16_t count = 0;
    while (count < max_len && uart2_rx_tail != uart2_rx_head) {
        buf[count++] = uart2_rx_buf[uart2_rx_tail];
        uart2_rx_tail = (uart2_rx_tail + 1) % UART2_RX_BUF_SIZE;
    }
    return count;
}
