#include "./SYSTEM/sys/sys.h"
#include "./BSP/EXTI/exti.h"
#include "./SYSTEM/delay/delay.h"
#include "./SYSTEM/usart/usart.h"
#include "freertos_demo.h"
/*FreeRTOS*********************************************************************************************/
#include "FreeRTOS.h"
#include "task.h"


void exti_init(void)
{
     
    GPIO_InitTypeDef GPIO_Init_Struct;
    
    __HAL_RCC_GPIOE_CLK_ENABLE();
       //使能GPIOE口的时钟   GPIOE2-GPIOE4都挂在GPIOE下

    GPIO_Init_Struct.Pin = GPIO_PIN_2;
    GPIO_Init_Struct.Mode = GPIO_MODE_IT_FALLING;
    //GPIO_Init_Struct.Speed = GPIO_SPEED_FREQ_HIGH;  //输出才需要配置输出速度
    GPIO_Init_Struct.Pull = GPIO_PULLUP;//上拉，为下降沿触发做准备
 
    HAL_GPIO_Init(GPIOE, &GPIO_Init_Struct);//初始化GPIOE2  KEY 2
    
    
    
    //设置中断优先级分组
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
     

    //设置EXTI2的中断优先级  抢断5响应0
    HAL_NVIC_SetPriority(EXTI2_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(EXTI2_IRQn);  

}




//设置EXTI2中断服务函数
void EXTI2_IRQHandler(void)
{

    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_2);//真正的GPIO外部中断服务函数，HAL库提供
    __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_2);
}




TaskHandle_t Task1_HANDLER;

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    
    
    BaseType_t xYieldRequired;
    
    delay_ms(20);//消抖
    
    switch (GPIO_Pin)
    {

         case GPIO_PIN_2:
            if(HAL_GPIO_ReadPin(GPIOE,GPIO_PIN_2) == 0)//确认key2按键按下
            {
                xYieldRequired = xTaskResumeFromISR(Task1_HANDLER);
                if(xYieldRequired == pdTRUE)
                {
                     portYIELD_FROM_ISR( xYieldRequired );//任务切换
                    printf("在中断中任务1恢复\r\n");
                }
            }

    
    }
    



}