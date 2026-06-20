/**
 ****************************************************************************************************
 * @file        adc_temp.c
 * @brief       ADC内部温度传感器 驱动
 *
 * STM32F407内部温度传感器连接在ADC1通道16
 * 温度计算公式: T = ((V_sense - V25) / Avg_Slope) + 25
 *   V25 = 0.76V (25°C时的电压)
 *   Avg_Slope = 2.5mV/°C
 *   V_sense = ADC值 * 3.3 / 4096
 ****************************************************************************************************
 */

#include "./BSP/ADC/adc_temp.h"
#include "./SYSTEM/delay/delay.h"

ADC_HandleTypeDef g_adc_temp_handle;    /* ADC1句柄 */

/**
 * @brief       初始化ADC1, 用于内部温度传感器采样
 *   @note      ADC1时钟 = PCLK2/4 = 84MHz/4 = 21MHz
 *              采样通道: ADC_CHANNEL_16 (通道16)
 *              采样时间: 480周期 (内部温度传感器需要较长采样时间)
 * @param       无
 * @retval      无
 */
void adc_temp_init(void)
{
    ADC_ChannelConfTypeDef sConfig = {0};

    __HAL_RCC_ADC1_CLK_ENABLE();        /* 使能ADC1时钟 */

    g_adc_temp_handle.Instance = ADC1;
    g_adc_temp_handle.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;       /* PCLK2/4 = 21MHz */
    g_adc_temp_handle.Init.Resolution = ADC_RESOLUTION_12B;                 /* 12位分辨率 */
    g_adc_temp_handle.Init.ScanConvMode = DISABLE;                          /* 非扫描模式 */
    g_adc_temp_handle.Init.ContinuousConvMode = DISABLE;                    /* 单次转换 */
    g_adc_temp_handle.Init.DiscontinuousConvMode = DISABLE;
    g_adc_temp_handle.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    g_adc_temp_handle.Init.DataAlign = ADC_DATAALIGN_RIGHT;                 /* 右对齐 */
    g_adc_temp_handle.Init.NbrOfConversion = 1;                             /* 1个通道 */
    g_adc_temp_handle.Init.DMAContinuousRequests = DISABLE;
    g_adc_temp_handle.Init.EOCSelection = ADC_EOC_SINGLE_CONV;

    HAL_ADC_Init(&g_adc_temp_handle);

    /* 配置温度传感器通道 */
    sConfig.Channel = ADC_CHANNEL_16;           /* 内部温度传感器: 通道16 */
    sConfig.Rank = 1;                                   /* 序列第1个 */
    sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;    /* 480周期采样 */
    HAL_ADC_ConfigChannel(&g_adc_temp_handle, &sConfig);
}

/**
 * @brief       获取MCU内部温度
 *   @note      使能温度传感器 → 启动ADC → 等待转换完成 → 读取值 → 计算温度
 * @param       无
 * @retval      温度值, 单位: 0.1°C (例如 253 表示 25.3°C)
 */
int16_t adc_temp_get(void)
{
    uint32_t adc_value;
    float voltage;
    float temperature;

    /* 使能内部温度传感器 (TSVREFE位) */
    ADC->CCR |= ADC_CCR_TSVREFE;
    delay_us(20);   /* 等待温度传感器稳定 */

    /* 启动ADC转换 */
    HAL_ADC_Start(&g_adc_temp_handle);

    /* 等待转换完成 */
    if (HAL_ADC_PollForConversion(&g_adc_temp_handle, 10) == HAL_OK)
    {
        adc_value = HAL_ADC_GetValue(&g_adc_temp_handle);
    }
    else
    {
        adc_value = 0;
    }

    /* 停止ADC */
    HAL_ADC_Stop(&g_adc_temp_handle);

    /* 关闭温度传感器以省电 */
    ADC->CCR &= ~ADC_CCR_TSVREFE;

    /* 温度换算 */
    /* VREF = 3.3V, 12位分辨率 */
    voltage = (float)adc_value * 3.3f / 4096.0f;

    /* STM32F407 内部温度传感器参数:
     * V25 = 0.76V (典型值, 25°C时的电压)
     * Avg_Slope = 2.5 mV/°C (平均斜率)
     */
    temperature = ((voltage - 0.76f) / 0.0025f) + 25.0f;

    return (int16_t)(temperature * 10.0f);   /* 返回 0.1°C 为单位 */
}
