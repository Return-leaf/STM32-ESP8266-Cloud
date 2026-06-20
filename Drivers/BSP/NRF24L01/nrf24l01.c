/**
 ****************************************************************************************************
 * @file        nrf24l01.c
 * @brief       NRF24L01 2.4GHz无线模块驱动 (SPI通信 + 寄存器配置 + 收发控制)
 *
 * ===== 通信协议 =====
 * NRF24L01通过SPI接口与MCU通信, 每条SPI事务的格式:
 *   第1字节=命令字节(寄存器地址|读写标志), 后续字节=数据
 * 命令字节格式: bit7-5=寄存器地址, bit4-0=取决于具体命令
 * 写寄存器: 0x20|reg_addr (例: 写CONFIG=0x20)
 * 读寄存器: 0x00|reg_addr (例: 读STATUS=0x07)
 *
 * ===== 关键时序 =====
 * - CSN下降沿开始SPI事务, CSN上升沿结束事务
 * - CE高电平在RX模式表示开始监听, TX模式CE脉冲(≥10us)触发发送
 * - STATUS寄存器读操作后自动清除中断标志(无需手动清)
 ****************************************************************************************************
 */

#include "nrf24l01.h"
#include "../SPI/spi.h"

extern SPI_HandleTypeDef g_spi1_handler;

/* 收发地址: 5字节(40位), 51端和STM32端必须完全一致, 这里用0xA5填充 */
const uint8_t TX_ADDRESS[TX_ADR_WIDTH] = {0xA5,0xA5,0xA5,0xA5,0xA5};
const uint8_t RX_ADDRESS[RX_ADR_WIDTH] = {0xA5,0xA5,0xA5,0xA5,0xA5};

static uint8_t nrf24l01_write_buf(uint8_t reg, uint8_t *pbuf, uint8_t len);
static uint8_t nrf24l01_read_buf(uint8_t reg, uint8_t *pbuf, uint8_t len);
static uint8_t nrf24l01_write_reg(uint8_t reg, uint8_t value);
static uint8_t nrf24l01_read_reg(uint8_t reg);

/**
 * @brief   针对NRF24L01调整SPI时序为模式0
 *
 * ===== 为什么需要单独切换SPI模式? =====
 * NRF24L01数据手册要求SPI模式0: CPOL=0(SCK空闲低电平), CPHA=0(第1个时钟边沿采样)
 * 而spi1_init()中默认初始化为模式3(CPOL=1,CPHA=1, 适合其他外设)
 * 必须在操作NRF24L01之前调用本函数切换到模式0
 *
 * ===== SPI四种模式速查 =====
 * Mode0: CPOL=0 CPHA=0   Mode1: CPOL=0 CPHA=1
 * Mode2: CPOL=1 CPHA=0   Mode3: CPOL=1 CPHA=1
 * NRF24L01仅支持Mode0!
 */
void nrf24l01_spi_init(void)
{
    __HAL_SPI_DISABLE(&g_spi1_handler);             /* 修改SPI配置前必须先禁用外设 */
    g_spi1_handler.Init.CLKPolarity = SPI_POLARITY_LOW;   /* CPOL=0: 时钟空闲时为低电平 */
    g_spi1_handler.Init.CLKPhase = SPI_PHASE_1EDGE;       /* CPHA=0: 在第1个边沿(上升沿)采样数据 */
    HAL_SPI_Init(&g_spi1_handler);                  /* 重新初始化SPI1(应用新模式) */
    __HAL_SPI_ENABLE(&g_spi1_handler);              /* 重新使能SPI1外设 */
}

/**
 * @brief  NRF24L01硬件初始化: GPIO配置 + SPI初始化 + 进入待机状态
 * 调用时机: main()中硬件初始化阶段, 在C51_Comm_Init()内部调用
 * 初始化后模块处于Standby-I状态(PWR_UP未设置, CE=0, CSN=1)
 */
void nrf24l01_init(void)
{
    GPIO_InitTypeDef gpio_init_struct;

    /* 使能GPIOG时钟(CE=PG6, CSN=PG7, IRQ=PG8) */
    NRF24L01_CE_GPIO_CLK_ENABLE();
    NRF24L01_CSN_GPIO_CLK_ENABLE();
    NRF24L01_IRQ_GPIO_CLK_ENABLE();

    /* CE引脚: 推挽输出, 内部上拉, 初始低电平(禁用收发) */
    gpio_init_struct.Pin = NRF24L01_CE_GPIO_PIN;
    gpio_init_struct.Mode = GPIO_MODE_OUTPUT_PP;
    gpio_init_struct.Pull = GPIO_PULLUP;
    gpio_init_struct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(NRF24L01_CE_GPIO_PORT, &gpio_init_struct);

    /* CSN引脚: 推挽输出, 内部上拉, 初始高电平(取消片选) */
    gpio_init_struct.Pin = NRF24L01_CSN_GPIO_PIN;
    HAL_GPIO_Init(NRF24L01_CSN_GPIO_PORT, &gpio_init_struct);

    /* IRQ引脚: 输入模式, 内部上拉(无中断时=高, 有事件=低) */
    gpio_init_struct.Pin = NRF24L01_IRQ_GPIO_PIN;
    gpio_init_struct.Mode = GPIO_MODE_INPUT;
    gpio_init_struct.Pull = GPIO_PULLUP;
    gpio_init_struct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(NRF24L01_IRQ_GPIO_PORT, &gpio_init_struct);

    spi1_init();                                    /* 初始化SPI1(PB3-SCK, PB4-MISO, PB5-MOSI)为模式3 */
    nrf24l01_spi_init();                            /* 切换SPI1为NRF24L01兼容的模式0 */
    NRF24L01_CE(0);                                 /* CE=0: 禁用收发, 进入待机状态 */
    NRF24L01_CSN(1);                                /* CSN=1: 取消片选, 准备第一次SPI通信 */
    HAL_Delay(100);                                 /* 等待NRF24L01上电稳定(手册要求≥100ms) */
}

/**
 * @brief  NRF24L01自检: 通过回读验证确认SPI通信正常
 *
 * ===== 检测原理 =====
 * 1. 向TX_ADDR寄存器写入测试值 {0xA5,0xA5,0xA5,0xA5,0xA5}
 * 2. 从TX_ADDR寄存器读回5字节
 * 3. 逐字节比对: 全部一致→SPI通信OK, 不一致→NRF24L01未焊接/SPI线接错/模块损坏
 *
 * @return 0=NRF24L01正常, 1=检测失败
 */
uint8_t nrf24l01_check(void)
{
    uint8_t buf[5] = {0XA5,0XA5,0XA5,0XA5,0XA5};    /* 测试数据: 全0xA5 */
    uint8_t i;

    spi1_set_speed(4);                              /* SPI时钟分频=32, 降低速率提高稳定性 */
    nrf24l01_write_buf(NRF_WRITE_REG + TX_ADDR, buf, 5);  /* 写TX_ADDR寄存器(地址0x10, 命令0x20|0x10=0x30) */
    nrf24l01_read_buf(TX_ADDR, buf, 5);                   /* 读回TX_ADDR(命令0x00|0x10=0x10) */

    for (i = 0; i < 5; i++) { if (buf[i] != 0XA5) break; } /* 逐字节比对 */
    if (i != 5) return 1;                           /* 不匹配→SPI通信异常 */
    return 0;                                       /* 全部匹配→NRF24L01正常 */
}

/**
 * @brief  写NRF24L01单个寄存器
 * SPI事务: [命令字节=NRF_WRITE_REG|reg] [数据字节=value]
 * 返回: STATUS寄存器值(可从中断标志位判断收发状态)
 */
static uint8_t nrf24l01_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t status;
    NRF24L01_CSN(0);                                /* CSN拉低→选中NRF24L01, 开始SPI事务 */
    status = spi1_read_write_byte(reg);             /* 发送命令字节, 同时收到STATUS寄存器值 */
    spi1_read_write_byte(value);                    /* 发送数据字节, 写入目标寄存器 */
    NRF24L01_CSN(1);                                /* CSN拉高→结束SPI事务 */
    return status;                                  /* 返回STATUS(供调用者检查TX_OK/RX_OK等标志) */
}

/**
 * @brief  读NRF24L01单个寄存器
 * SPI事务: [命令字节=NRF_READ_REG|reg] [0xFF(dummy)] → 返回寄存器值
 */
static uint8_t nrf24l01_read_reg(uint8_t reg)
{
    uint8_t reg_val;
    NRF24L01_CSN(0);                                /* CSN拉低→选中NRF24L01 */
    spi1_read_write_byte(reg);                      /* 发送读命令字节 */
    reg_val = spi1_read_write_byte(0XFF);           /* 发送dummy字节(0xFF), 同时接收寄存器值 */
    NRF24L01_CSN(1);                                /* CSN拉高→结束SPI事务 */
    return reg_val;
}

/**
 * @brief  批量读NRF24L01寄存器(多字节)
 * 用于读RX_ADDR/TX_ADDR(5字节)或RX_FIFO(32字节载荷)
 */
static uint8_t nrf24l01_read_buf(uint8_t reg, uint8_t *pbuf, uint8_t len)
{
    uint8_t status, i;
    NRF24L01_CSN(0);                                /* CSN拉低→开始SPI事务 */
    status = spi1_read_write_byte(reg);             /* 发送读命令, 收到STATUS */
    for (i = 0; i < len; i++) { pbuf[i] = spi1_read_write_byte(0XFF); } /* 连续读len字节到缓冲区 */
    NRF24L01_CSN(1);                                /* CSN拉高→结束 */
    return status;
}

/**
 * @brief  批量写NRF24L01寄存器(多字节)
 * 用于写RX_ADDR/TX_ADDR(5字节)或TX_FIFO(32字节载荷)
 */
static uint8_t nrf24l01_write_buf(uint8_t reg, uint8_t *pbuf, uint8_t len)
{
    uint8_t status, i;
    NRF24L01_CSN(0);                                /* CSN拉低→开始SPI事务 */
    status = spi1_read_write_byte(reg);             /* 发送写命令, 收到STATUS */
    for (i = 0; i < len; i++) { spi1_read_write_byte(*pbuf++); } /* 连续写len字节 */
    NRF24L01_CSN(1);                                /* CSN拉高→结束 */
    return status;
}

/**
 * @brief  发送一包数据 (TX模式, STM32备用; 本项目由51端发送, 此函数未使用)
 *
 * ===== 发送流程 =====
 * 1. CE=0(进入待机) → 2.写载荷到TX_FIFO → 3.CE=1(脉冲触发发送)
 * 4.等待IRQ变低(发送完成/失败/超时) → 5.读STATUS清除中断 → 6.根据标志返回
 *
 * @param ptxbuf 32字节载荷数据
 * @return 0=发送成功(TX_OK, 收到ACK), 1=失败(MAX_TX, 达最大重发次数)
 */
uint8_t nrf24l01_tx_packet(uint8_t *ptxbuf)
{
    uint8_t sta;
    uint8_t rval = 0XFF;

    NRF24L01_CE(0);                                         /* CE=0: 禁用收发, 准备写FIFO */
    nrf24l01_write_buf(WR_TX_PLOAD, ptxbuf, TX_PLOAD_WIDTH); /* 写32字节到TX_FIFO */
    NRF24L01_CE(1);                                         /* CE脉冲≥10us: 触发发送(模块进入TX模式) */

    while (NRF24L01_IRQ != 0);                              /* 轮询等待IRQ变低(发送完成中断) */

    sta = nrf24l01_read_reg(STATUS);                        /* 读STATUS(自动清除中断标志) */
    nrf24l01_write_reg(NRF_WRITE_REG + STATUS, sta);        /* 手动写回清除(双保险) */

    if (sta & MAX_TX) { nrf24l01_write_reg(FLUSH_TX, 0xff); rval = 1; } /* MAX_RT=1: 重发10次均失败 */
    if (sta & TX_OK)  { rval = 0; }                         /* TX_DS=1: 发送成功(收到ACK) */

    return rval;
}

/**
 * @brief  接收一包数据 (RX模式, 本项目主要使用的接收函数)
 *
 * ===== 接收流程 =====
 * 1. 读STATUS检查RX_OK(bit6) → 2.读FIFO载荷(32字节) → 3.FLUSH_RX清FIFO
 * 非阻塞: RX_OK未置位→立即返回1, 不阻塞调用者
 *
 * @param prxbuf 输出缓冲区(至少32字节), 接收到的有效载荷
 * @return 0=收到有效数据, 1=无数据(RX_OK未置位)
 */
uint8_t nrf24l01_rx_packet(uint8_t *prxbuf)
{
    uint8_t sta;
    uint8_t rval = 1;                                       /* 默认返回值=1(无数据) */

    sta = nrf24l01_read_reg(STATUS);                        /* 读取STATUS寄存器 */
    nrf24l01_write_reg(NRF_WRITE_REG + STATUS, sta);        /* 写回清除中断标志(避免下次误判) */

    if (sta & RX_OK) {                                      /* RX_DR=1: 接收FIFO中有有效数据 */
        nrf24l01_read_buf(RD_RX_PLOAD, prxbuf, RX_PLOAD_WIDTH); /* 读32字节载荷到用户缓冲区 */
        nrf24l01_write_reg(FLUSH_RX, 0xff);                 /* 清空接收FIFO(准备下一包) */
        rval = 0;                                           /* 成功→返回0 */
    }

    return rval;
}

/**
 * @brief   配置NRF24L01为接收模式(RX) — STM32端的主要工作模式
 *
 * ===== 寄存器配置详解 =====
 * RX_ADDR_P0 = {0xA5x5}: 接收地址, 必须与51端TX_ADDR完全一致
 * EN_AA      = 0x01:     通道0自动应答使能, 收到数据后自动回ACK(硬件自动, 无需CPU干预)
 * EN_RXADDR  = 0x01:     启用通道0接收(6通道可同时接收, 本项目只用P0)
 * RF_CH      = 40:       射频频道=2400+40=2440MHz (2.440GHz, WiFi信道7附近, 避免干扰)
 * RX_PW_P0   = 32:       通道0载荷宽度=32字节(与51端TX_PLOAD_WIDTH一致)
 * RF_SETUP   = 0x0f:     2Mbps速率 + 0dBm发射功率 + LNA高增益(bit3=1)
 *                        0x0f = 0000_1111b → LNA_HCURR=1(开启高增益), RF_DR=0(2Mbps)
 * CONFIG     = 0x0f:     PWR_UP=1(上电) + CRC=11(2字节CRC16) + PRIM_RX=1(接收模式)
 *                        CE=1后模块进入RX模式, 持续监听空中数据包
 */
void nrf24l01_rx_mode(void)
{
    NRF24L01_CE(0);                                                 /* CE=0: 禁用收发, 允许修改寄存器 */
    nrf24l01_write_buf(NRF_WRITE_REG + RX_ADDR_P0, (uint8_t *)RX_ADDRESS, RX_ADR_WIDTH); /* 配置接收地址 */
    nrf24l01_write_reg(NRF_WRITE_REG + EN_AA, 0x01);               /* 通道0自动应答: 收包后自动发ACK */
    nrf24l01_write_reg(NRF_WRITE_REG + EN_RXADDR, 0x01);           /* 启用通道0接收 */
    nrf24l01_write_reg(NRF_WRITE_REG + RF_CH, 40);                 /* 频道40→2440MHz */
    nrf24l01_write_reg(NRF_WRITE_REG + RX_PW_P0, RX_PLOAD_WIDTH);  /* 载荷宽度=32字节(最大) */
    nrf24l01_write_reg(NRF_WRITE_REG + RF_SETUP, 0x0f);            /* 2Mbps+0dBm+LNA */
    nrf24l01_write_reg(NRF_WRITE_REG + CONFIG, 0x0f);              /* PWR_UP+CRC16+PRIM_RX */
    NRF24L01_CE(1);                                                 /* CE=1: 进入接收模式, 开始监听 */
}

uint8_t nrf24l01_diag_status(void) { return nrf24l01_read_reg(STATUS); }  /* 诊断: 读STATUS(调试串口打印) */
uint8_t nrf24l01_diag_cd(void)     { return nrf24l01_read_reg(CD); }      /* 诊断: 读载波检测(调试串口打印) */


/**
 * @brief   配置NRF24L01为发送模式(TX) — 51端使用, STM32备用
 *
 * ===== 与RX模式的关键区别 =====
 * SETUP_RETR = 0x1a: 自动重发使能, 间隔500us(0x1<<4), 最多10次(0xa)
 *                    发送后等ACK, 收不到则自动重发(硬件自动, 无需CPU干预)
 * CONFIG     = 0x0e: PRIM_RX=0(发送模式, bit0=0)
 * 还需配置TX_ADDR: 发送目标地址(必须与接收端RX_ADDR_P0一致)
 */
void nrf24l01_tx_mode(void)
{
    NRF24L01_CE(0);                                                 /* CE=0: 禁用收发, 允许修改寄存器 */
    nrf24l01_write_buf(NRF_WRITE_REG + TX_ADDR, (uint8_t *)TX_ADDRESS, TX_ADR_WIDTH);   /* 配置发送目标地址 */
    nrf24l01_write_buf(NRF_WRITE_REG + RX_ADDR_P0, (uint8_t *)RX_ADDRESS, RX_ADR_WIDTH);/* 为接收ACK配置P0地址 */
    nrf24l01_write_reg(NRF_WRITE_REG + EN_AA, 0x01);               /* 通道0自动应答使能 */
    nrf24l01_write_reg(NRF_WRITE_REG + EN_RXADDR, 0x01);           /* 启用通道0(接收ACK用) */
    nrf24l01_write_reg(NRF_WRITE_REG + SETUP_RETR, 0x1a);          /* 重发: 500us×10次=最多等5ms */
    nrf24l01_write_reg(NRF_WRITE_REG + RF_CH, 40);                 /* 频道40→2440MHz */
    nrf24l01_write_reg(NRF_WRITE_REG + RF_SETUP, 0x0f);            /* 2Mbps+0dBm+LNA */
    nrf24l01_write_reg(NRF_WRITE_REG + CONFIG, 0x0e);              /* PWR_UP+CRC16+PRIM_TX(发送模式) */
    NRF24L01_CE(1);                                                 /* CE=1: 进入TX模式(待发送数据) */
}
