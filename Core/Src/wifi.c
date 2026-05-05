#include "wifi.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>

static char rx_buf[WIFI_RX_BUF_SIZE];

// ===== 复位 =====
static void esp8266_hw_reset(void)
{
    HAL_GPIO_WritePin(ESP8266_RST_PORT, ESP8266_RST_PIN, GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(ESP8266_RST_PORT, ESP8266_RST_PIN, GPIO_PIN_SET);
    HAL_Delay(2000);
}

// ===== 初始化 =====
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

// ===== 发送AT =====
int wifi_send_cmd(char *cmd, char *expected, uint16_t wait_ms)
{
    printf("[CMD] %s", cmd);

    memset(rx_buf, 0, sizeof(rx_buf));

    HAL_UART_Transmit(&huart2, (uint8_t *)cmd, strlen(cmd), 1000);

    // 一次性接收
    HAL_UART_Receive(&huart2, (uint8_t*)rx_buf, WIFI_RX_BUF_SIZE, wait_ms);

    printf("[RECV] %s\r\n", rx_buf);

    if (expected && strstr(rx_buf, expected))
    {
        printf("[OK]\r\n\r\n");
        return 1;
    }

    printf("[FAIL]\r\n\r\n");
    return 0;
}

// ===== 测试 =====
int wifi_test(void)
{
    int ret;

    ret = wifi_send_cmd("AT\r\n", "OK", 2000);

    if (!ret)
        ret = wifi_send_cmd("AT\r\n", "OK", 2000);

    return ret;
}

// ===== 连接路由器 =====
int wifi_connect_router(void)
{
    char cmd[128];

    // 设置 Station 模式
    if (!wifi_send_cmd("AT+CWMODE=1\r\n", "OK", 3000))
        return 0;

    // 连接 WiFi
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PWD);
    if (!wifi_send_cmd(cmd, "OK", 15000))
        return 0;

    printf("WiFi Connected\r\n");
    return 1;
}

// ===== 连接 OneNET MQTT =====
int OneNET_MQTT_Init(void)
{
    char cmd[512];

    // 先断开之前的连接
    wifi_send_cmd("AT+MQTTCLEAN=0\r\n", NULL, 3000);
    HAL_Delay(1000);

    // 测试 DNS 解析
    printf("=== Step0: DNS Test ===\r\n");
    snprintf(cmd, sizeof(cmd), "AT+CIPDOMAIN=\"%s\"\r\n", ONENET_MQTT_HOST);
    wifi_send_cmd(cmd, "OK", 5000);
    HAL_Delay(1000);

    // MQTT 用户配置
    printf("=== Step1: MQTTUSERCFG ===\r\n");
    snprintf(cmd, sizeof(cmd),
        "AT+MQTTUSERCFG=0,1,\"%s\",\"%s\",\"%s\",0,0,\"\"\r\n",
        ONENET_DEVICE_NAME, ONENET_PRODUCT_ID, ONENET_DEVICE_KEY);
    if (!wifi_send_cmd(cmd, "OK", 5000))
        return 0;

    HAL_Delay(2000);

    // 连接 MQTT 服务器
    printf("=== Step2: MQTTCONN ===\r\n");
    snprintf(cmd, sizeof(cmd),
        "AT+MQTTCONN=0,\"%s\",%d,1\r\n",
        ONENET_MQTT_HOST, ONENET_MQTT_PORT);
    if (!wifi_send_cmd(cmd, "OK", 15000))
        return 0;

    HAL_Delay(1000);

    // 订阅属性上报回复 topic
    printf("=== Step3: Subscribe post/reply ===\r\n");
    snprintf(cmd, sizeof(cmd),
        "AT+MQTTSUB=0,\"$sys/%s/%s/thing/property/post/reply\",1\r\n",
        ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
    if (!wifi_send_cmd(cmd, "OK", 5000))
        return 0;

    // 订阅属性设置 topic（接收平台下发指令）
    printf("=== Step4: Subscribe property/set ===\r\n");
    snprintf(cmd, sizeof(cmd),
        "AT+MQTTSUB=0,\"$sys/%s/%s/thing/property/set\",1\r\n",
        ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
    if (!wifi_send_cmd(cmd, "OK", 5000))
        return 0;

    printf("MQTT Connected & Subscribed\r\n");
    return 1;
}

// ===== 上报数据（使用 AT+MQTTPUBRAW，原始数据 + 长度） =====
void OneNET_Report_Wifi(int val)
{
    char cmd[128];
    char payload[256];
    int len;

    // OneJSON 格式上报属性
    len = snprintf(payload, sizeof(payload),
        "{\"id\":\"123\",\"version\":\"1.0\",\"params\":{\"wifi\":{\"value\":%d}}}",
        val);

    printf("---- payload (%d bytes) ----\r\n", len);
    printf("%s\r\n", payload);

    // AT+MQTTPUBRAW=<link_id>,<topic>,<length>,<qos>,<retain>
    // 收到 > 后发送原始数据
    snprintf(cmd, sizeof(cmd),
        "AT+MQTTPUBRAW=0,\"$sys/%s/%s/thing/property/post\",%d,0,0\r\n",
        ONENET_PRODUCT_ID, ONENET_DEVICE_NAME, len);

    printf("[CMD] %s", cmd);
    memset(rx_buf, 0, sizeof(rx_buf));

    HAL_UART_Transmit(&huart2, (uint8_t *)cmd, strlen(cmd), 1000);

    // 等待 '>' 提示符
    HAL_UART_Receive(&huart2, (uint8_t*)rx_buf, WIFI_RX_BUF_SIZE, 3000);
    printf("[RECV] %s\r\n", rx_buf);

    if (strstr(rx_buf, ">"))
    {
        // 发送原始 JSON（无引号转义）
        HAL_UART_Transmit(&huart2, (uint8_t *)payload, len, 1000);
        printf("[SENT] %s\r\n", payload);

        // 等待 OK
        memset(rx_buf, 0, sizeof(rx_buf));
        HAL_UART_Receive(&huart2, (uint8_t*)rx_buf, WIFI_RX_BUF_SIZE, 5000);
        printf("[RECV] %s\r\n", rx_buf);
    }
    else
    {
        printf("[FAIL] No '>' prompt\r\n");
    }
}

// ===== 上报事件 =====
void OneNET_Report_Event(int val)
{
    char cmd[128];
    char payload[256];
    int len;

    // OneJSON 事件上报格式
    len = snprintf(payload, sizeof(payload),
        "{\"id\":\"123\",\"version\":\"1.0\",\"events\":[{\"event_id\":\"fault\",\"params\":{\"wifi\":{\"value\":%d}}}]}",
        val);

    printf("---- event payload (%d bytes) ----\r\n", len);
    printf("%s\r\n", payload);

    snprintf(cmd, sizeof(cmd),
        "AT+MQTTPUBRAW=0,\"$sys/%s/%s/thing/event/post\",%d,0,0\r\n",
        ONENET_PRODUCT_ID, ONENET_DEVICE_NAME, len);

    printf("[CMD] %s", cmd);
    memset(rx_buf, 0, sizeof(rx_buf));

    HAL_UART_Transmit(&huart2, (uint8_t *)cmd, strlen(cmd), 1000);

    HAL_UART_Receive(&huart2, (uint8_t*)rx_buf, WIFI_RX_BUF_SIZE, 3000);
    printf("[RECV] %s\r\n", rx_buf);

    if (strstr(rx_buf, ">"))
    {
        HAL_UART_Transmit(&huart2, (uint8_t *)payload, len, 1000);
        printf("[SENT] %s\r\n", payload);

        memset(rx_buf, 0, sizeof(rx_buf));
        HAL_UART_Receive(&huart2, (uint8_t*)rx_buf, WIFI_RX_BUF_SIZE, 5000);
        printf("[RECV] %s\r\n", rx_buf);
    }
    else
    {
        printf("[FAIL] No '>' prompt\r\n");
    }
}
