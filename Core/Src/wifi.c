#include "wifi.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 前向声明 */
static void handle_property_set(const char *payload);
static void handle_property_get(const char *payload);

/* ========== ESP8266 复位 ========== */
static void esp8266_hw_reset(void)
{
    HAL_GPIO_WritePin(ESP8266_RST_PORT, ESP8266_RST_PIN, GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(ESP8266_RST_PORT, ESP8266_RST_PIN, GPIO_PIN_SET);
    HAL_Delay(2000);
}

void esp8266_init(void)
{
    __HAL_RCC_GPIOF_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin   = ESP8266_RST_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(ESP8266_RST_PORT, &GPIO_InitStruct);
    HAL_GPIO_WritePin(ESP8266_RST_PORT, ESP8266_RST_PIN, GPIO_PIN_SET);

    esp8266_hw_reset();
}

/* ========== AT 指令发送（和原始代码一样的阻塞接收方式） ========== */
static char rx_buf[512];

int wifi_send_cmd(const char *cmd, const char *expected, uint16_t timeout_ms)
{
    memset(rx_buf, 0, sizeof(rx_buf));

    HAL_UART_Transmit(&huart2, (uint8_t *)cmd, strlen(cmd), 1000);
    HAL_UART_Receive(&huart2, (uint8_t *)rx_buf, sizeof(rx_buf) - 1, timeout_ms);

    if (expected)
        printf("[CMD] %s[RECV] %s\r\n", cmd, rx_buf);

    if (expected && strstr(rx_buf, expected)) {
        printf("[OK]\r\n\r\n");
        return 1;
    }

    if (expected)
        printf("[FAIL]\r\n\r\n");
    return 0;
}

/* ========== WiFi 测试 ========== */
int wifi_test(void)
{
    int ret = wifi_send_cmd("AT\r\n", "OK", 2000);
    if (!ret)
        ret = wifi_send_cmd("AT\r\n", "OK", 2000);
    return ret;
}

/* ========== 连接路由器 ========== */
int wifi_connect_router(void)
{
    char cmd[128];

    if (!wifi_send_cmd("AT+CWMODE=1\r\n", "OK", 3000))
        return 0;

    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PWD);
    if (!wifi_send_cmd(cmd, "OK", 15000))
        return 0;

    printf("WiFi Connected\r\n");
    return 1;
}

/* ========== OneNET MQTT 初始化 ========== */
int OneNET_MQTT_Init(void)
{
    char cmd[512];

    wifi_send_cmd("AT+MQTTCLEAN=0\r\n", NULL, 3000);
    HAL_Delay(1000);

    printf("=== DNS Test ===\r\n");
    snprintf(cmd, sizeof(cmd), "AT+CIPDOMAIN=\"%s\"\r\n", ONENET_MQTT_HOST);
    wifi_send_cmd(cmd, "OK", 5000);
    HAL_Delay(1000);

    printf("=== MQTT User Config ===\r\n");
    snprintf(cmd, sizeof(cmd),
        "AT+MQTTUSERCFG=0,1,\"%s\",\"%s\",\"%s\",0,0,\"\"\r\n",
        ONENET_DEVICE_NAME, ONENET_PRODUCT_ID, ONENET_DEVICE_KEY);
    if (!wifi_send_cmd(cmd, "OK", 5000))
        return 0;
    HAL_Delay(2000);

    printf("=== MQTT Connect ===\r\n");
    snprintf(cmd, sizeof(cmd),
        "AT+MQTTCONN=0,\"%s\",%d,1\r\n",
        ONENET_MQTT_HOST, ONENET_MQTT_PORT);
    if (!wifi_send_cmd(cmd, "OK", 15000))
        return 0;
    HAL_Delay(1000);

    printf("=== Subscribe post/reply ===\r\n");
    snprintf(cmd, sizeof(cmd),
        "AT+MQTTSUB=0,\"$sys/%s/%s/thing/property/post/reply\",1\r\n",
        ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
    if (!wifi_send_cmd(cmd, "OK", 5000))
        return 0;

    printf("=== Subscribe property/set ===\r\n");
    snprintf(cmd, sizeof(cmd),
        "AT+MQTTSUB=0,\"$sys/%s/%s/thing/property/set\",1\r\n",
        ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
    if (!wifi_send_cmd(cmd, "OK", 5000))
        return 0;

    printf("=== Subscribe property/get ===\r\n");
    snprintf(cmd, sizeof(cmd),
        "AT+MQTTSUB=0,\"$sys/%s/%s/thing/property/get\",1\r\n",
        ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
    if (!wifi_send_cmd(cmd, "OK", 5000))
        return 0;

    printf("MQTT Connected & Subscribed\r\n");
    return 1;
}

/* ========== 属性上报（MQTTPUBRAW） ========== */
void OneNET_Report_Property(int val)
{
    char cmd[128];
    char payload[256];
    char buf[512];
    int len;

    len = snprintf(payload, sizeof(payload),
        "{\"id\":\"123\",\"version\":\"1.0\",\"params\":{\"wifi\":{\"value\":%d}}}",
        val);

    printf("[Report] wifi=%d (%d bytes)\r\n", val, len);

    snprintf(cmd, sizeof(cmd),
        "AT+MQTTPUBRAW=0,\"$sys/%s/%s/thing/property/post\",%d,0,0\r\n",
        ONENET_PRODUCT_ID, ONENET_DEVICE_NAME, len);

    HAL_UART_Transmit(&huart2, (uint8_t *)cmd, strlen(cmd), 1000);

    /* 等待 '>' 提示符 */
    memset(buf, 0, sizeof(buf));
    HAL_UART_Receive(&huart2, (uint8_t *)buf, sizeof(buf) - 1, 3000);

    if (strstr(buf, ">")) {
        HAL_UART_Transmit(&huart2, (uint8_t *)payload, len, 1000);
        memset(buf, 0, sizeof(buf));
        HAL_UART_Receive(&huart2, (uint8_t *)buf, sizeof(buf) - 1, 5000);
        printf("[RECV] %s\r\n", buf);
        if (strstr(buf, "OK"))
            printf("[Report] OK\r\n");
    } else {
        printf("[Report] No '>' prompt\r\n");
    }
}

/* ========== MQTT 发送回复（MQTTPUBRAW） ========== */
static void mqtt_pub_raw_reply(const char *topic, const char *payload)
{
    char cmd[256];
    char buf[512];
    int len = strlen(payload);

    snprintf(cmd, sizeof(cmd),
        "AT+MQTTPUBRAW=0,\"%s\",%d,0,0\r\n", topic, len);

    HAL_UART_Transmit(&huart2, (uint8_t *)cmd, strlen(cmd), 1000);

    memset(buf, 0, sizeof(buf));
    HAL_UART_Receive(&huart2, (uint8_t *)buf, sizeof(buf) - 1, 3000);

    if (strstr(buf, ">")) {
        HAL_UART_Transmit(&huart2, (uint8_t *)payload, len, 1000);
        memset(buf, 0, sizeof(buf));
        HAL_UART_Receive(&huart2, (uint8_t *)buf, sizeof(buf) - 1, 5000);
        printf("[RECV] %s\r\n", buf);
    }
}

/* ========== 检查异步 MQTT 消息（轮询方式） ========== */
static void check_mqtt_incoming(void)
{
    char buf[512];
    memset(buf, 0, sizeof(buf));
    /* 短超时轮询，看看有没有异步数据 */
    HAL_UART_Receive(&huart2, (uint8_t *)buf, sizeof(buf) - 1, 100);

    if (!strstr(buf, "+MQTTSUBRECV:"))
        return;

    /* 解析 +MQTTSUBRECV:<linkID>,<topic>,<len>,<data> */
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (strstr(line, "+MQTTSUBRECV:")) {
            char *tok = strchr(line, ':');
            if (tok) {
                tok++;
                char *c1 = strchr(tok, ',');
                if (c1) {
                    char *topic_s = c1 + 1;
                    char *c2 = strchr(topic_s, ',');
                    if (c2) {
                        uint16_t tlen = c2 - topic_s;
                        if (tlen >= WIFI_MSG_MAX_LEN) tlen = WIFI_MSG_MAX_LEN - 1;
                        char t[WIFI_MSG_MAX_LEN];
                        memcpy(t, topic_s, tlen);
                        t[tlen] = '\0';

                        char *len_s = c2 + 1;
                        char *c3 = strchr(len_s, ',');
                        if (c3) {
                            int plen = atoi(len_s);
                            char *data_s = c3 + 1;
                            char p[WIFI_MSG_MAX_LEN];
                            uint16_t dl = (plen < WIFI_MSG_MAX_LEN - 1) ? plen : WIFI_MSG_MAX_LEN - 1;
                            uint16_t actual = strlen(data_s);
                            if (dl > actual) dl = actual;
                            memcpy(p, data_s, dl);
                            p[dl] = '\0';

                            printf("[MQTT RECV] %s\r\n", t);
                            printf("  %s\r\n", p);

                            /* 分发处理 */
                            if (strstr(t, "thing/property/set")) {
                                handle_property_set(p);
                            } else if (strstr(t, "thing/property/get")) {
                                handle_property_get(p);
                            } else if (strstr(t, "thing/property/post/reply")) {
                                printf("[Post Reply] %s\r\n", p);
                            }
                        }
                    }
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
}

/* 提取 JSON 中的 id 字段 */
static void json_get_id(const char *json, char *id_out, int max_len)
{
    strcpy(id_out, "123");
    char *pos = strstr(json, "\"id\"");
    if (!pos) return;
    char *colon = strchr(pos, ':');
    if (!colon) return;
    colon++;
    while (*colon == ' ') colon++;
    if (*colon == '"') {
        colon++;
        char *end = strchr(colon, '"');
        if (end && (end - colon) < max_len) {
            memcpy(id_out, colon, end - colon);
            id_out[end - colon] = '\0';
        }
    }
}

/* ========== 处理云端下发的属性设置 ========== */
static void handle_property_set(const char *payload)
{
    char id_str[32];
    char reply[128];
    char topic[128];

    json_get_id(payload, id_str, sizeof(id_str));
    printf("[SET] id=%s\r\n", id_str);

    char *p_pos = strstr(payload, "\"params\"");
    if (p_pos) {
        char *brace = strchr(p_pos, '{');
        if (brace) {
            char *end = strchr(brace, '}');
            if (end) {
                printf("[SET] Params: ");
                for (char *p = brace; p <= end; p++) putchar(*p);
                printf("\r\n");
            }
        }
    }

    snprintf(reply, sizeof(reply),
        "{\"id\":\"%s\",\"code\":200,\"msg\":\"success\"}", id_str);
    snprintf(topic, sizeof(topic),
        "$sys/%s/%s/thing/property/set/reply",
        ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);

    printf("[SET] Reply: %s\r\n", reply);
    mqtt_pub_raw_reply(topic, reply);
}

/* ========== 处理云端下发的属性获取 ========== */
static void handle_property_get(const char *payload)
{
    char id_str[32];
    char reply[256];
    char topic[128];

    json_get_id(payload, id_str, sizeof(id_str));
    printf("[GET] id=%s\r\n", id_str);

    int has_wifi = strstr(payload, "\"wifi\"") ? 1 : 0;

    if (has_wifi) {
        static int report_count = 0;
        snprintf(reply, sizeof(reply),
            "{\"id\":\"%s\",\"code\":200,\"msg\":\"success\",\"data\":{\"wifi\":{\"value\":%d}}}",
            id_str, report_count);
        report_count++;
    } else {
        snprintf(reply, sizeof(reply),
            "{\"id\":\"%s\",\"code\":200,\"msg\":\"success\",\"data\":{}}", id_str);
    }

    snprintf(topic, sizeof(topic),
        "$sys/%s/%s/thing/property/get/reply",
        ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);

    printf("[GET] Reply: %s\r\n", reply);
    mqtt_pub_raw_reply(topic, reply);
}

/* ========== 主循环调用 ========== */
void OneNET_Handle_Incoming(void)
{
    check_mqtt_incoming();
}
