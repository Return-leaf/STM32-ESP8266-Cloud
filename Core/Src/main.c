#include "main.h"
#include "usart.h"
#include "wifi.h"
#include <stdio.h>

#ifdef __GNUC__
#define PUTCHAR_PROTOTYPE int __io_putchar(int ch)
#else
#define PUTCHAR_PROTOTYPE int fputc(int ch, FILE *f)
#endif

PUTCHAR_PROTOTYPE {
  HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, 0xFFFF);
  return ch;
}

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

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

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();

  MX_USART1_UART_Init(); // printf
  MX_USART2_UART_Init(); // ESP8266

  printf("\r\n==== USART2 ESP8266 TEST ====\r\n");

  esp8266_init();

  if (!wifi_test())
  {
      printf("ESP8266 ERROR\r\n");
      while(1);
  }

  printf("ESP8266 OK\r\n");

  // 连接 WiFi
  if (!wifi_connect_router())
  {
      printf("WiFi Connect FAILED\r\n");
      while(1);
  }

  // 连接 OneNET MQTT
  if (!OneNET_MQTT_Init())
  {
      printf("MQTT Connect FAILED\r\n");
      while(1);
  }

  printf("==== All Connected, Start Reporting ====\r\n");

  int count = 0;
  while(1)
  {
      printf("---- Report #%d ----\r\n", count);
      OneNET_Report_Wifi(count);
      HAL_Delay(3000);
      OneNET_Report_Event(count);
      count++;
      HAL_Delay(5000);
  }
}
