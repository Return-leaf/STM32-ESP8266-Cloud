/**
 ****************************************************************************************************
 * @file        main.c
 * @brief       STM32F407 硬件初始化 + 系统入口
 *
 * ===== 系统架构 =====
 * 51 MCU (DS18B20+NRF24 TX) --2.4GHz--> STM32F407 (NRF24 RX)
 *    --ESP8266 WiFi--> 华为云 IoT MQTT (TCP:1883)
 *
 * ===== 启动流程 =====
 * 1. HAL库 + 时钟168MHz + 串口/LED/LCD/按键
 * 2. NRF24L01 初始化 (接收模式, 等待 51 发温度)
 * 3. LCD 显示主界面标题和状态行
 * 4. freertos_demo() 创建 3 个任务并启动调度器 (永不返回)
 *
 * ===== FreeRTOS 三任务 =====
 * vNRF24Task   (prio3, 50ms):  NRF 数据轮询, 最高优先级
 * vWiFiMQTTTask(prio2, 100ms): WiFi/MQTT 状态机
 * vLCDTask     (prio1, 1s):    LCD 温度刷新, 最低优先级
 *
 * ===== LCD 屏幕布局 =====
 * 第1行: "NRF24L01 + ESP8266" (红色)
 * 第2行: "Huawei Cloud IoT"   (蓝色)
 * 第3行: NRF:{状态}  第4行: WiFi:{状态}
 * 第5行: MQTT:{状态} 第6行: TEMP:{温度}C
 ****************************************************************************************************
 */

#include "./SYSTEM/sys/sys.h"
#include "./SYSTEM/usart/usart.h"
#include "./SYSTEM/delay/delay.h"
#include "./BSP/LED/led.h"
#include "./BSP/LCD/lcd.h"
#include "./BSP/KEY/key.h"
#include "./MALLOC/malloc.h"
#include "./BSP/NRF24L01/51_comm.h"
#include "freertos_demo.h"

int main(void)
{
    HAL_Init();                                 /* HAL库初始化, 配置系统滴答定时器 */
    sys_stm32_clock_init(336, 8, 2, 7);         /* 时钟树: PLL=336MHz -> SYSCLK=168MHz, AHB=168M, APB1=42M, APB2=84M */
    delay_init(168);                            /* 延时函数初始化(参数=SYSCLK频率MHz), 使用SysTick */
    usart_init(115200);                         /* USART1初始化(PA9-TX,PA10-RX), 波特率115200, printf串口调试 */
    led_init();                                 /* LED初始化: DS0=PF9, DS1=PF10, 低电平点亮 */
    lcd_init();                                 /* LCD初始化(FSMC接口), ILI9341控制器, 320x240分辨率 */
    key_init();                                 /* 按键初始化: KEY0=PE4, KEY1=PE3, KEY2=PE2, KEY_UP=PA0 */
    my_mem_init(SRAMIN);                        /* 内部SRAM内存池初始化, 为FreeRTOS动态内存分配做准备 */

    printf("\r\n[SYS] STM32F407 + FreeRTOS Booting...\r\n");

    /* LCD 主界面 — 6行布局: 标题 / 副标题 / NRF状态 / WiFi状态 / MQTT状态 / 温度 */
    lcd_show_string(30, 10, 200, 16, 16, "NRF24L01 + ESP8266", RED);     /* 第1行(Y=10): 主标题(红色) */
    lcd_show_string(30, 30, 200, 16, 16, "Huawei Cloud IoT", BLUE);      /* 第2行(Y=30): 副标题(蓝色) */
    lcd_show_string(30, 60, 200, 16, 16, "NRF:", BLACK);                 /* 第3行(Y=60): NRF24L01状态标签 */
    lcd_show_string(30, 80, 200, 16, 16, "WiFi:", BLACK);                /* 第4行(Y=80): WiFi状态标签 */
    lcd_show_string(30, 100, 200, 16, 16, "MQTT:", BLACK);               /* 第5行(Y=100): MQTT状态标签 */
    lcd_show_string(30, 120, 200, 16, 16, "TEMP:", BLUE);                /* 第6行(Y=120): 温度值标签(蓝色) */

    /* NRF24L01 初始化 (进入接收模式, 等待51发温度) */
    printf("[NRF] Init...\r\n");
    C51_Comm_Init();                            /* 51通信初始化: 配置NRF24L01为RX模式 + 地址验证 + 帧解析器就绪 */
    lcd_show_string(130, 60, 100, 16, 16, "OK", GREEN);

    /* 创建 FreeRTOS 任务并启动调度器 (永不返回) */
    printf("[SYS] Starting tasks...\r\n");
    freertos_demo();                            /* 创建3任务并启动FreeRTOS调度器, 此函数永不返回! 之后由RTOS接管 */
}
