#ifndef __24L01_H
#define __24L01_H

/**
 ****************************************************************************************************
 * @file        nrf24l01.h
 * @brief       NRF24L01 2.4GHz无线模块 引脚定义 + 寄存器地址 + API声明
 *
 * ===== 模块介绍 =====
 * NRF24L01是Nordic半导体的2.4GHz ISM频段无线收发芯片
 * 通信距离: 空旷约100m / 室内约30m
 * 接口: SPI(四线: SCK/MISO/MOSI/CSN) + 控制线(CE/IRQ)
 *
 * ===== 硬件接线 (STM32F407 ? NRF24L01) =====
 * PB3(SPI1_SCK)  ? SCK    PB4(SPI1_MISO) ? MISO
 * PB5(SPI1_MOSI) ? MOSI   PG7(GPIO)      ? CSN(片选)
 * PG6(GPIO)      ? CE(使能)  PG8(GPIO)   ? IRQ(中断, 低有效)
 * VCC=3.3V, GND=GND
 *
 * ===== SPI时序要求 =====
 * NRF24L01要求SPI模式0: CPOL=0(时钟空闲低电平), CPHA=0(第1个边沿采样)
 * 最高SPI时钟: 10MHz
 ****************************************************************************************************
 */

#include "./SYSTEM/sys/sys.h"

/* ==================== NRF24L01 控制引脚 ==================== */
/* CE  (Chip Enable):  高电平使能收发, 在RX模式下CE=1开始监听, TX模式下CE=1启动发送 */
#define NRF24L01_CE_GPIO_PORT              GPIOG
#define NRF24L01_CE_GPIO_PIN               GPIO_PIN_6
#define NRF24L01_CE_GPIO_CLK_ENABLE()      do{ __HAL_RCC_GPIOG_CLK_ENABLE(); }while(0)

/* CSN (Chip Select Not): SPI片选, 低电平有效, 每次SPI操作前拉低/后拉高 */
#define NRF24L01_CSN_GPIO_PORT             GPIOG
#define NRF24L01_CSN_GPIO_PIN              GPIO_PIN_7
#define NRF24L01_CSN_GPIO_CLK_ENABLE()     do{ __HAL_RCC_GPIOG_CLK_ENABLE(); }while(0)

/* IRQ (Interrupt Request): 中断输出, 低电平表示有事件(发送完成/接收就绪/重发达上限) */
#define NRF24L01_IRQ_GPIO_PORT             GPIOG
#define NRF24L01_IRQ_GPIO_PIN              GPIO_PIN_8
#define NRF24L01_IRQ_GPIO_CLK_ENABLE()     do{ __HAL_RCC_GPIOG_CLK_ENABLE(); }while(0)

/* CE引脚控制宏: 1=使能(高电平), 0=禁用(低电平); do{}while(0)保证宏在任何上下文安全使用 */
#define NRF24L01_CE(x)    do{ x ? \
                              HAL_GPIO_WritePin(NRF24L01_CE_GPIO_PORT, NRF24L01_CE_GPIO_PIN, GPIO_PIN_SET) : \
                              HAL_GPIO_WritePin(NRF24L01_CE_GPIO_PORT, NRF24L01_CE_GPIO_PIN, GPIO_PIN_RESET); \
                          }while(0)

/* CSN引脚控制宏: 1=取消片选(高电平), 0=选中(低电平, 开始SPI通信) */
#define NRF24L01_CSN(x)   do{ x ? \
                              HAL_GPIO_WritePin(NRF24L01_CSN_GPIO_PORT, NRF24L01_CSN_GPIO_PIN, GPIO_PIN_SET) : \
                              HAL_GPIO_WritePin(NRF24L01_CSN_GPIO_PORT, NRF24L01_CSN_GPIO_PIN, GPIO_PIN_RESET); \
                          }while(0)

/* IRQ引脚读取宏: 0=有中断事件, 1=空闲; 在TX模式下轮询等待发送完成 */
#define NRF24L01_IRQ      HAL_GPIO_ReadPin(NRF24L01_IRQ_GPIO_PORT, NRF24L01_IRQ_GPIO_PIN)

/* ===== 地址和载荷宽度 ===== */
#define TX_ADR_WIDTH    5       /* 发送地址宽度: 5字节(40位), 可区分海量设备 */
#define RX_ADR_WIDTH    5       /* 接收地址宽度: 5字节, 必须与发送端TX_ADDR一致 */
#define TX_PLOAD_WIDTH  32      /* 发送载荷宽度: 32字节(最大), 不足自动补0 */
#define RX_PLOAD_WIDTH  32      /* 接收载荷宽度: 32字节, 与TX_PLOAD_WIDTH保持一致 */

/* ===== NRF24L01 SPI命令 (发送给模块的指令字节) ===== */
#define NRF_READ_REG    0x00    /* 读寄存器命令: 或上寄存器地址, 例: NRF_READ_REG+STATUS=0x07 */
#define NRF_WRITE_REG   0x20    /* 写寄存器命令: 或上寄存器地址, 例: NRF_WRITE_REG+CONFIG=0x20 */
#define RD_RX_PLOAD     0x61    /* 读接收FIFO载荷: 读取已收到的数据包(32字节) */
#define WR_TX_PLOAD     0xA0    /* 写发送FIFO载荷: 写入待发送的数据包 */
#define FLUSH_TX        0xE1    /* 清空发送FIFO: 丢弃所有待发送数据 */
#define FLUSH_RX        0xE2    /* 清空接收FIFO: 丢弃所有已接收数据 */
#define REUSE_TX_PL     0xE3    /* 重用上次发送载荷: 重发上一包(不重新写FIFO) */
#define NOP             0xFF    /* 空操作: 仅用于读取STATUS寄存器(无副作用) */

/* ===== NRF24L01 寄存器地址 (nRF24L01+数据手册 Table 25) ===== */
#define CONFIG          0x00    /* 配置寄存器: PRIM_RX(bit0), PWR_UP(bit1), CRC模式(bit2-3), 中断屏蔽 */
#define EN_AA           0x01    /* 自动应答使能: 按通道(0-5), 接收端自动回ACK包确认收到 */
#define EN_RXADDR       0x02    /* 接收地址使能: 按通道(0-5), 本项目只用通道0 */
#define SETUP_AW        0x03    /* 地址宽度设置: 3/4/5字节, 默认5字节 */
#define SETUP_RETR      0x04    /* 自动重发设置: 高4位=重发间隔, 低4位=最大重发次数 */
#define RF_CH           0x05    /* 射频频道: 2400MHz + RF_CH MHz, 例: 40→2440MHz */
#define RF_SETUP        0x06    /* 射频设置: 速率(1M/2Mbps), 发射功率, LNA增益 */
#define STATUS          0x07    /* 状态寄存器: RX_DR(bit6)=收就绪, TX_DS(bit5)=发完成, MAX_RT(bit4)=重发上限 */
#define MAX_TX          0x10    /* STATUS bit4掩码: 达到最大重发次数(发送失败) */
#define TX_OK           0x20    /* STATUS bit5掩码: 发送成功(收到ACK) */
#define RX_OK           0x40    /* STATUS bit6掩码: 接收FIFO中有有效数据 */
#define OBSERVE_TX      0x08    /* 发送观察: 低4位=丢包计数, 高4位=重发计数(调试用) */
#define CD              0x09    /* 载波检测: bit0=检测到同频信号(用于诊断射频环境) */
#define RX_ADDR_P0      0x0A    /* 接收通道0地址: 5字节, 本项目唯一使用的接收地址 */
#define RX_ADDR_P1      0x0B    /* 接收通道1地址: 仅低字节可不同(其余4字节与P0共享) */
#define RX_ADDR_P2      0x0C
#define RX_ADDR_P3      0x0D
#define RX_ADDR_P4      0x0E
#define RX_ADDR_P5      0x0F
#define TX_ADDR         0x10    /* 发送地址: 5字节, 必须与对端RX_ADDR_P0一致 */
#define RX_PW_P0        0x11    /* 通道0载荷宽度: 1-32字节, 本项目=32 */
#define RX_PW_P1        0x12
#define RX_PW_P2        0x13
#define RX_PW_P3        0x14
#define RX_PW_P4        0x15
#define RX_PW_P5        0x16
#define NRF_FIFO_STATUS 0x17    /* FIFO状态: TX_FULL/TX_EMPTY/RX_FULL/RX_EMPTY(调试用) */

/* ===== API 函数声明 ===== */
void nrf24l01_spi_init(void);                       /* 切换SPI1为模式0(CPOL=0,CPHA=0), NRF24L01专用时序 */
void nrf24l01_init(void);                           /* 硬件初始化: GPIO(CE/CSN/IRQ) + SPI1, CE拉低进入待机 */
void nrf24l01_rx_mode(void);                        /* 配置为接收模式: 地址0xA5x5, 通道0, 2Mbps, 32B载荷, CRC16 */
void nrf24l01_tx_mode(void);                        /* 配置为发送模式: 自动重发10次/500us间隔 (51端使用, STM32备用) */
uint8_t nrf24l01_check(void);                       /* 自检: 写TX_ADDR→读回→比对, 0=芯片正常, 1=SPI通信异常 */
uint8_t nrf24l01_tx_packet(uint8_t *ptxbuf);        /* 发送一包: 写FIFO→CE脉冲→等待IRQ→读STATUS, 0=成功(TX_OK) */
uint8_t nrf24l01_diag_status(void);                 /* 诊断: 读取STATUS寄存器值 (调试用) */
uint8_t nrf24l01_diag_cd(void);                     /* 诊断: 读取载波检测CD寄存器 (调试用, 检测是否有同频干扰) */

uint8_t nrf24l01_rx_packet(uint8_t *prxbuf);        /* 接收一包: 读STATUS→检查RX_OK→读FIFO→FLUSH_RX, 0=收到数据 */

#endif
