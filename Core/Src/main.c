/**
 * @file    main.c
 * @brief   STM32F407 + ESP8266 + OneNET MQTT 主程序
 *
 * 系统架构：
 *   STM32F407ZGT6 通过 USART2（PA2/PA3）连接 ESP8266 WiFi 模块，
 *   ESP8266 通过 AT 指令连接 OneNET 物联网平台的 MQTT 服务。
 *   每 5 秒上报一次属性值，并监听云端下发的属性设置/获取指令。
 *
 * 引脚分配：
 *   USART1 PA9/PA10  → 调试串口（printf 输出，115200）
 *   USART2 PA2/PA3   → ESP8266 AT 指令（115200）
 *   PF6              → ESP8266 RST 复位引脚
 */

#include "main.h"
#include "usart.h"
#include "wifi.h"
#include <stdio.h>

/* ========================================================================== */
/*                          printf 重定向                                      */
/* ========================================================================== */

/* 将 printf 重定向到 USART1（调试串口） */
#ifdef __GNUC__
#define PUTCHAR_PROTOTYPE int __io_putchar(int ch)
#else
#define PUTCHAR_PROTOTYPE int fputc(int ch, FILE *f)
#endif

PUTCHAR_PROTOTYPE {
  HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, 0xFFFF);
  return ch;
}

/* ========================================================================== */
/*                          系统时钟配置                                       */
/* ========================================================================== */

/**
 * @brief  系统时钟配置
 *         HSE 8MHz → PLL → SYSCLK 168MHz
 *         AHB = 168MHz, APB1 = 42MHz, APB2 = 84MHz
 */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /* HSE 8MHz → PLL ×336/2 = 168MHz */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  HAL_RCC_OscConfig(&RCC_OscInitStruct);

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                              | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
  HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5);
}

/* ========================================================================== */
/*                          主函数                                             */
/* ========================================================================== */

int main(void)
{
  /* 系统初始化 */
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();

  /* 串口初始化 */
  MX_USART1_UART_Init();  /* USART1：printf 调试输出 */
  MX_USART2_UART_Init();  /* USART2：ESP8266 AT 指令 */

  /* ESP8266 硬件复位 */
  esp8266_init();

  /* 测试 AT 指令是否正常 */
  if (!wifi_test()) {
      printf("[ERR] AT Failed!\r\n");
      while(1);
  }

  /* 连接 WiFi 路由器 */
  if (!wifi_connect_router()) {
      printf("[ERR] WiFi Failed!\r\n");
      while(1);
  }
  printf("[WiFi] Connected\r\n");

  /* 连接 OneNET MQTT 并订阅 Topic */
  if (!OneNET_MQTT_Init()) {
      printf("[ERR] MQTT Failed!\r\n");
      while(1);
  }
  printf("[MQTT] Connected\r\n");

  /* ========== 主循环 ========== */
  int count = 0;
  while(1)
  {
      /* 检查是否有云端下发的 MQTT 消息（属性设置/获取） */
      OneNET_Handle_Incoming();

      /* 向 OneNET 上报属性值 */
      OneNET_Report_Property(count);

      count++;
      HAL_Delay(5000);  /* 每 5 秒上报一次 */
  }
}
