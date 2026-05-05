/**
 * @file    wifi.c
 * @brief   ESP8266 WiFi 模块驱动 + OneNET MQTT 通信
 *
 * 功能：
 *   1. ESP8266 硬件复位与 AT 指令收发
 *   2. WiFi 连接路由器
 *   3. OneNET MQTT 连接、订阅、属性上报
 *   4. 接收云端下发的属性设置/获取指令并回复
 *
 * 通信方式：
 *   - AT 指令通过 USART2（PA2-TX, PA3-RX）发送给 ESP8266
 *   - 使用中断+环形缓冲区接收异步 MQTT 消息（+MQTTSUBRECV）
 *   - 发送 AT 指令时临时关闭中断，使用阻塞方式接收响应
 */

#include "wifi.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 前向声明：云端下发指令的处理函数 */
static void handle_property_set(const char *payload);
static void handle_property_get(const char *payload);

/**
 * @brief  从缓冲区中扫描并处理 +MQTTSUBRECV 消息
 * @param  buf  包含 ESP8266 响应数据的缓冲区
 *
 * ESP8266 收到订阅的 MQTT 消息时，会以如下格式通知：
 *   +MQTTSUBRECV:<linkID>,<topic>,<data_len>,<data>
 *
 * 本函数按行扫描缓冲区，解析 topic 和 data，然后分发给对应的处理函数。
 */
static void process_mqtt_buf(const char *buf)
{
    printf("[PARSE] Processing MQTT buf\r\n");
    char *line = (char *)buf;
    while (line && *line) {
        /* 按行分割（ESP8266 用 \r\n 换行） */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (strstr(line, "+MQTTSUBRECV:")) {
            /* 解析格式：+MQTTSUBRECV:<linkID>,<topic>,<len>,<data> */
            char *tok = strchr(line, ':');
            if (tok) {
                tok++;  /* 跳过冒号，指向 linkID */
                char *c1 = strchr(tok, ',');  /* 第一个逗号：linkID 和 topic 之间 */
                if (c1) {
                    char *topic_s = c1 + 1;   /* topic 起始位置 */
                    char *c2 = strchr(topic_s, ',');  /* 第二个逗号：topic 和 len 之间 */
                    if (c2) {
                        /* 提取 topic */
                        uint16_t tlen = c2 - topic_s;
                        if (tlen >= WIFI_MSG_MAX_LEN) tlen = WIFI_MSG_MAX_LEN - 1;
                        char t[WIFI_MSG_MAX_LEN];
                        memcpy(t, topic_s, tlen);
                        t[tlen] = '\0';

                        /* 提取 data_len 和 data */
                        char *len_s = c2 + 1;   /* data_len 起始位置 */
                        char *c3 = strchr(len_s, ',');  /* 第三个逗号：len 和 data 之间 */
                        if (c3) {
                            int plen = atoi(len_s);      /* 数据长度 */
                            char *data_s = c3 + 1;       /* data 起始位置 */
                            char p[WIFI_MSG_MAX_LEN];
                            /* 取实际长度和声明长度的较小值，防止越界 */
                            uint16_t dl = (plen < WIFI_MSG_MAX_LEN - 1) ? plen : WIFI_MSG_MAX_LEN - 1;
                            uint16_t actual = strlen(data_s);
                            if (dl > actual) dl = actual;
                            memcpy(p, data_s, dl);
                            p[dl] = '\0';

                            printf("[MQTT RECV] topic=%s\r\n", t);
                            printf("[MQTT RECV] data=%s\r\n", p);

                            /* 根据 topic 分发处理 */
                            if (strstr(t, "thing/property/set")) {
                                handle_property_set(p);   /* 属性设置 */
                            } else if (strstr(t, "thing/property/get")) {
                                handle_property_get(p);   /* 属性获取 */
                            } else if (strstr(t, "thing/property/post/reply")) {
                                printf("[Post Reply] %s\r\n", p);  /* 属性上报回复 */
                            }
                        }
                    }
                }
            }
        }
        line = nl ? nl + 1 : NULL;  /* 移动到下一行 */
    }
}

/* ========================================================================== */
/*                          ESP8266 硬件复位                                   */
/* ========================================================================== */

/**
 * @brief  ESP8266 硬件复位（拉低 RST 引脚 100ms 后释放）
 */
static void esp8266_hw_reset(void)
{
    HAL_GPIO_WritePin(ESP8266_RST_PORT, ESP8266_RST_PIN, GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(ESP8266_RST_PORT, ESP8266_RST_PIN, GPIO_PIN_SET);
    HAL_Delay(2000);  /* 等待 ESP8266 启动完成 */
}

/**
 * @brief  初始化 ESP8266 复位引脚并执行硬件复位
 */
void esp8266_init(void)
{
    __HAL_RCC_GPIOF_CLK_ENABLE();

    /* 配置 PF6 为推挽输出（ESP8266 RST 引脚） */
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin   = ESP8266_RST_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(ESP8266_RST_PORT, &GPIO_InitStruct);
    HAL_GPIO_WritePin(ESP8266_RST_PORT, ESP8266_RST_PIN, GPIO_PIN_SET);

    esp8266_hw_reset();
}

/* ========================================================================== */
/*                          AT 指令收发                                        */
/* ========================================================================== */

/* AT 指令响应缓冲区 */
static char rx_buf[512];

/**
 * @brief  逐字节接收 USART2 数据，遇到指定字符或超时则停止
 * @param  buf         接收缓冲区
 * @param  max_len     缓冲区最大长度
 * @param  stop_chars  停止字符集（遇到其中任意字符立即返回），NULL 表示只按超时停止
 * @param  timeout_ms  每字节之间的最大等待时间（毫秒）
 * @return 实际接收到的字节数
 *
 * 与 HAL_UART_Receive 不同，本函数逐字节读取，每收到一个字节重置超时计时器。
 * 这样即使响应数据较少，也能在最后一个字节到达后及时返回，而非等待缓冲区填满。
 */
static int uart2_recv_until(char *buf, int max_len, const char *stop_chars, uint32_t timeout_ms)
{
    int i = 0;
    uint32_t start = HAL_GetTick();
    while (i < max_len - 1) {
        /* 直接检查 RXNE 标志（中断已关闭，无需担心竞争） */
        if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE)) {
            uint8_t ch = (uint8_t)(huart2.Instance->DR & 0xFF);  /* 读取数据寄存器，自动清除 RXNE */
            buf[i++] = ch;
            buf[i] = '\0';
            /* 如果指定了停止字符，匹配到则立即返回 */
            if (stop_chars && strchr(stop_chars, ch))
                return i;
            start = HAL_GetTick();  /* 收到字节，重置超时计时器 */
        }
        /* 无新数据到达，检查是否超时 */
        if ((HAL_GetTick() - start) >= timeout_ms)
            break;
    }
    return i;
}

/**
 * @brief  发送 AT 指令并检查响应
 * @param  cmd          AT 指令字符串（需包含 \r\n）
 * @param  expected     期望的响应关键字（如 "OK"），NULL 表示不检查
 * @param  timeout_ms   等待响应的超时时间
 * @return 1=成功（找到 expected），0=失败
 *
 * 流程：关闭中断 → 清空缓冲区 → 发送指令 → 阻塞接收响应 → 开启中断
 * 接收完成后还会检查响应中是否夹带了 +MQTTSUBRECV 消息，如有则一并处理。
 */
int wifi_send_cmd(const char *cmd, const char *expected, uint16_t timeout_ms)
{
    memset(rx_buf, 0, sizeof(rx_buf));

    /* 关闭 USART2 中断，防止中断处理程序抢走 HAL_UART_Receive 的数据 */
    HAL_NVIC_DisableIRQ(USART2_IRQn);
    uart2_rx_flush();  /* 清空环形缓冲区中的旧数据 */
    HAL_UART_Transmit(&huart2, (uint8_t *)cmd, strlen(cmd), 1000);
    HAL_UART_Receive(&huart2, (uint8_t *)rx_buf, sizeof(rx_buf) - 1, timeout_ms);
    HAL_NVIC_EnableIRQ(USART2_IRQn);

    /* 检查响应中是否夹带了 +MQTTSUBRECV（云端消息在 AT 命令期间到达） */
    if (strstr(rx_buf, "+MQTTSUBRECV:")) {
        printf("[IN CMD] Found +MQTTSUBRECV\r\n");
        process_mqtt_buf(rx_buf);
    }

    /* 检查是否包含期望的响应关键字 */
    if (expected && strstr(rx_buf, expected)) {
        if (strstr(cmd, "MQTTSUB"))
            printf("[SUB OK] %s", cmd);
        return 1;
    }

    if (expected)
        printf("[FAIL] resp: %s\r\n", rx_buf);
    return 0;
}

/* ========================================================================== */
/*                          WiFi 连接                                          */
/* ========================================================================== */

/**
 * @brief  测试 ESP8266 AT 指令是否正常
 * @return 1=成功，0=失败
 */
int wifi_test(void)
{
    int ret = wifi_send_cmd("AT\r\n", "OK", 2000);
    if (!ret)
        ret = wifi_send_cmd("AT\r\n", "OK", 2000);  /* 第一次失败则重试一次 */
    return ret;
}

/**
 * @brief  连接 WiFi 路由器
 * @return 1=成功，0=失败
 *
 * 步骤：设置 Station 模式 → 发送 AT+CWJAP 连接路由器
 */
int wifi_connect_router(void)
{
    char cmd[128];

    /* 设置 WiFi 模式为 Station（客户端模式） */
    if (!wifi_send_cmd("AT+CWMODE=1\r\n", "OK", 3000))
        return 0;

    /* 连接路由器（SSID 和密码在 wifi.h 中配置） */
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PWD);
    if (!wifi_send_cmd(cmd, "OK", 15000))
        return 0;

    return 1;
}

/* ========================================================================== */
/*                          OneNET MQTT 初始化                                 */
/* ========================================================================== */

/**
 * @brief  初始化 OneNET MQTT 连接并订阅所需 Topic
 * @return 1=成功，0=失败
 *
 * 步骤：
 *   1. 清理旧的 MQTT 连接（AT+MQTTCLEAN）
 *   2. DNS 解析（AT+CIPDOMAIN）
 *   3. 配置 MQTT 用户信息（AT+MQTTUSERCFG）
 *   4. 连接 MQTT 服务器（AT+MQTTCONN）
 *   5. 订阅三个 Topic：
 *      - property/set   ：云端下发属性设置指令
 *      - property/get   ：云端下发属性获取指令
 *      - post/reply     ：属性上报的平台回复
 */
int OneNET_MQTT_Init(void)
{
    char cmd[512];

    /* 清理旧的 MQTT 连接 */
    wifi_send_cmd("AT+MQTTCLEAN=0\r\n", NULL, 3000);
    HAL_Delay(1000);

    /* DNS 解析 MQTT 服务器域名 */
    snprintf(cmd, sizeof(cmd), "AT+CIPDOMAIN=\"%s\"\r\n", ONENET_MQTT_HOST);
    wifi_send_cmd(cmd, "OK", 5000);
    HAL_Delay(1000);

    /* 配置 MQTT 用户信息（设备名、产品ID、鉴权信息） */
    snprintf(cmd, sizeof(cmd),
        "AT+MQTTUSERCFG=0,1,\"%s\",\"%s\",\"%s\",0,0,\"\"\r\n",
        ONENET_DEVICE_NAME, ONENET_PRODUCT_ID, ONENET_DEVICE_KEY);
    if (!wifi_send_cmd(cmd, "OK", 5000))
        return 0;
    HAL_Delay(2000);

    /* 连接 OneNET MQTT 服务器 */
    snprintf(cmd, sizeof(cmd),
        "AT+MQTTCONN=0,\"%s\",%d,1\r\n",
        ONENET_MQTT_HOST, ONENET_MQTT_PORT);
    if (!wifi_send_cmd(cmd, "OK", 15000))
        return 0;
    HAL_Delay(1000);

    /* 订阅属性设置 Topic（云端 → 设备） */
    printf("[SUB] Subscribing property/set...\r\n");
    snprintf(cmd, sizeof(cmd),
        "AT+MQTTSUB=0,\"$sys/%s/%s/thing/property/set\",0\r\n",
        ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
    if (!wifi_send_cmd(cmd, "OK", 5000)) {
        printf("[SUB] property/set FAILED\r\n");
        return 0;
    }
    HAL_Delay(1000);

    /* 订阅属性获取 Topic（云端 → 设备） */
    printf("[SUB] Subscribing property/get...\r\n");
    snprintf(cmd, sizeof(cmd),
        "AT+MQTTSUB=0,\"$sys/%s/%s/thing/property/get\",0\r\n",
        ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
    if (!wifi_send_cmd(cmd, "OK", 5000)) {
        printf("[SUB] property/get FAILED\r\n");
        return 0;
    }
    HAL_Delay(1000);

    /* 订阅属性上报回复 Topic（平台确认收到上报） */
    printf("[SUB] Subscribing post/reply...\r\n");
    snprintf(cmd, sizeof(cmd),
        "AT+MQTTSUB=0,\"$sys/%s/%s/thing/property/post/reply\",0\r\n",
        ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
    if (!wifi_send_cmd(cmd, "OK", 5000)) {
        printf("[SUB] post/reply FAILED\r\n");
        return 0;
    }

    return 1;
}

/* ========================================================================== */
/*                          属性上报                                           */
/* ========================================================================== */

/**
 * @brief  向 OneNET 上报属性值
 * @param  val  要上报的 wifi 属性值
 *
 * 使用 AT+MQTTPUBRAW 发送 OneJSON 格式的属性数据：
 *   {"id":"123","version":"1.0","params":{"wifi":{"value":N}}}
 *
 * 流程：发送 AT 命令 → 等待 '>' 提示符 → 发送 JSON 数据 → 等待 OK
 * 期间会扫描响应中是否夹带了 +MQTTSUBRECV 消息并处理。
 */
void OneNET_Report_Property(int val)
{
    char cmd[128];
    char payload[256];
    char buf[512];
    int len;

    /* 构造 OneJSON 格式的属性上报数据 */
    len = snprintf(payload, sizeof(payload),
        "{\"id\":\"123\",\"version\":\"1.0\",\"params\":{\"wifi\":{\"value\":%d}}}",
        val);

    printf("[Report] wifi=%d\r\n", val);

    /* 构造 AT+MQTTPUBRAW 命令 */
    snprintf(cmd, sizeof(cmd),
        "AT+MQTTPUBRAW=0,\"$sys/%s/%s/thing/property/post\",%d,0,0\r\n",
        ONENET_PRODUCT_ID, ONENET_DEVICE_NAME, len);

    /* 关闭中断，使用逐字节接收方式 */
    HAL_NVIC_DisableIRQ(USART2_IRQn);
    uart2_rx_flush();
    HAL_UART_Transmit(&huart2, (uint8_t *)cmd, strlen(cmd), 1000);

    /* 等待 ESP8266 返回 '>' 提示符（表示可以发送数据） */
    memset(buf, 0, sizeof(buf));
    uart2_recv_until(buf, sizeof(buf), ">", 3000);

    if (strstr(buf, ">")) {
        /* 发送 JSON 数据 */
        HAL_UART_Transmit(&huart2, (uint8_t *)payload, len, 1000);
        /* 等待 ESP8266 返回结果 */
        memset(buf, 0, sizeof(buf));
        uart2_recv_until(buf, sizeof(buf), NULL, 3000);
        HAL_NVIC_EnableIRQ(USART2_IRQn);

        /* 检查响应中是否夹带了 +MQTTSUBRECV */
        if (strstr(buf, "+MQTTSUBRECV:"))
            process_mqtt_buf(buf);
        if (strstr(buf, "OK"))
            printf("[Report] OK\r\n");
        else
            printf("[Report] FAIL\r\n");
    } else {
        HAL_NVIC_EnableIRQ(USART2_IRQn);
    }
}

/* ========================================================================== */
/*                          MQTT 回复发送                                      */
/* ========================================================================== */

/**
 * @brief  通过 AT+MQTTPUBRAW 发送 MQTT 消息（用于回复云端指令）
 * @param  topic    MQTT Topic 字符串
 * @param  payload  JSON 格式的回复数据
 *
 * 与 OneNET_Report_Property 类似，但用于发送属性 set/get 的回复。
 * 发送前会等待 500ms，让 ESP8266 有时间处理之前的 +MQTTSUBRECV 通知。
 */
static void mqtt_pub_raw_reply(const char *topic, const char *payload)
{
    char cmd[256];
    char buf[512];
    int len = strlen(payload);

    HAL_Delay(500);  /* 等待 ESP8266 就绪 */

    snprintf(cmd, sizeof(cmd),
        "AT+MQTTPUBRAW=0,\"%s\",%d,0,0\r\n", topic, len);

    HAL_NVIC_DisableIRQ(USART2_IRQn);
    uart2_rx_flush();
    HAL_UART_Transmit(&huart2, (uint8_t *)cmd, strlen(cmd), 1000);

    /* 等待 '>' 提示符 */
    memset(buf, 0, sizeof(buf));
    uart2_recv_until(buf, sizeof(buf), ">", 3000);

    if (strstr(buf, ">")) {
        /* 发送 JSON 数据 */
        HAL_UART_Transmit(&huart2, (uint8_t *)payload, len, 1000);
        /* 等待发送结果 */
        memset(buf, 0, sizeof(buf));
        uart2_recv_until(buf, sizeof(buf), NULL, 3000);
    }
    HAL_NVIC_EnableIRQ(USART2_IRQn);

    /* 检查响应中是否夹带了 +MQTTSUBRECV */
    if (strstr(buf, "+MQTTSUBRECV:"))
        process_mqtt_buf(buf);
}

/* ========================================================================== */
/*                          异步 MQTT 消息检查                                 */
/* ========================================================================== */

/**
 * @brief  检查环形缓冲区中是否有新的 MQTT 消息
 *
 * 在主循环中调用。中断处理程序将 ESP8266 的异步数据存入环形缓冲区，
 * 本函数从缓冲区读取数据并检查是否包含 +MQTTSUBRECV 消息。
 */
static void check_mqtt_incoming(void)
{
    uint16_t avail = uart2_rx_available();
    if (avail == 0)
        return;

    printf("[RING] %d bytes\r\n", avail);

    char buf[1024];
    memset(buf, 0, sizeof(buf));
    uint16_t n = uart2_rx_read((uint8_t *)buf, sizeof(buf) - 1);
    if (n == 0) return;

    printf("[RING DATA] %s\r\n", buf);

    if (strstr(buf, "+MQTTSUBRECV:"))
        process_mqtt_buf(buf);
}

/* ========================================================================== */
/*                          JSON 解析辅助                                      */
/* ========================================================================== */

/**
 * @brief  从 JSON 字符串中提取 "id" 字段的值
 * @param  json     输入 JSON 字符串
 * @param  id_out   输出缓冲区
 * @param  max_len  输出缓冲区最大长度
 *
 * 简单的字符串搜索方式提取 id，不依赖 JSON 解析库。
 * 默认值为 "123"（如果未找到 id 字段）。
 */
static void json_get_id(const char *json, char *id_out, int max_len)
{
    strcpy(id_out, "123");  /* 默认值 */
    char *pos = strstr(json, "\"id\"");
    if (!pos) return;
    char *colon = strchr(pos, ':');
    if (!colon) return;
    colon++;
    while (*colon == ' ') colon++;  /* 跳过空格 */
    if (*colon == '"') {
        colon++;  /* 跳过开头的引号 */
        char *end = strchr(colon, '"');
        if (end && (end - colon) < max_len) {
            memcpy(id_out, colon, end - colon);
            id_out[end - colon] = '\0';
        }
    }
}

/* ========================================================================== */
/*                          云端指令处理                                        */
/* ========================================================================== */

/**
 * @brief  处理云端下发的属性设置指令
 * @param  payload  JSON 格式的设置指令数据
 *
 * 收到格式：{"id":"39","version":"1.0","params":{"wifi":11}}
 * 回复格式：{"id":"39","code":200,"msg":"success"}
 * 回复 Topic：$sys/{pid}/{device}/thing/property/set/reply
 */
static void handle_property_set(const char *payload)
{
    char id_str[32];
    char reply[128];
    char topic[128];

    json_get_id(payload, id_str, sizeof(id_str));
    printf("[SET] id=%s\r\n", id_str);

    /* 构造成功回复 */
    snprintf(reply, sizeof(reply),
        "{\"id\":\"%s\",\"code\":200,\"msg\":\"success\"}", id_str);
    snprintf(topic, sizeof(topic),
        "$sys/%s/%s/thing/property/set/reply",
        ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);

    printf("[SET] Reply: %s\r\n", reply);
    HAL_Delay(2000);  /* 等待 ESP8266 处理完通知 */
    mqtt_pub_raw_reply(topic, reply);
    printf("[SET] Reply sent\r\n");
}

/**
 * @brief  处理云端下发的属性获取指令
 * @param  payload  JSON 格式的获取指令数据
 *
 * 收到格式：{"id":"40","version":"1.0","params":["wifi"]}
 * 回复格式：{"id":"40","code":200,"msg":"success","data":{"wifi":{"value":N}}}
 * 回复 Topic：$sys/{pid}/{device}/thing/property/get/reply
 */
static void handle_property_get(const char *payload)
{
    char id_str[32];
    char reply[256];
    char topic[128];

    json_get_id(payload, id_str, sizeof(id_str));
    printf("[GET] id=%s payload=%s\r\n", id_str, payload);

    int has_wifi = strstr(payload, "\"wifi\"") ? 1 : 0;

    if (has_wifi) {
        /* 请求了 wifi 属性，返回当前值 */
        static int report_count = 0;
        snprintf(reply, sizeof(reply),
            "{\"id\":\"%s\",\"code\":200,\"msg\":\"success\",\"data\":{\"wifi\":{\"value\":%d}}}",
            id_str, report_count);
        report_count++;
    } else {
        /* 未请求特定属性，返回空数据 */
        snprintf(reply, sizeof(reply),
            "{\"id\":\"%s\",\"code\":200,\"msg\":\"success\",\"data\":{}}", id_str);
    }

    snprintf(topic, sizeof(topic),
        "$sys/%s/%s/thing/property/get/reply",
        ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);

    HAL_Delay(2000);  /* 等待 ESP8266 处理完通知 */
    mqtt_pub_raw_reply(topic, reply);
}

/* ========================================================================== */
/*                          主循环入口                                         */
/* ========================================================================== */

/**
 * @brief  主循环中调用，检查并处理云端下发的 MQTT 消息
 */
void OneNET_Handle_Incoming(void)
{
    check_mqtt_incoming();
}
