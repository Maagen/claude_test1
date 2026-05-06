#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "cellular.h"

/* ── Hardware ────────────────────────────────────────────────────────────── */
#define CELL_UART_PORT      UART_NUM_1
#define CELL_UART_TX        17
#define CELL_UART_RX        18
#define CELL_UART_BAUD      115200
#define CELL_UART_BUF_SIZE  2048

/* ── MQTT broker ─────────────────────────────────────────────────────────── */
#define MQTT_BROKER         "broker.example.com"
#define MQTT_PORT           1883
#define MQTT_CLIENT_ID      "esp32s3-node"
#define MQTT_USERNAME       ""
#define MQTT_PASSWORD       ""
#define MQTT_INSTANCE       1          /* Telit instance id 1–3 */

/* ── Timeouts ────────────────────────────────────────────────────────────── */
#define AT_TIMEOUT_MS       5000
#define MQTT_CONN_TIMEOUT_S 30         /* passed to AT#MQCONN */
#define MQTT_PUB_TIMEOUT_S  30         /* passed to AT#MQPUB  */
#define REG_TIMEOUT_MS      60000

static const char *TAG = "CELLULAR";

/* ── Low-level UART helpers ──────────────────────────────────────────────── */

static void at_send(const char *cmd)
{
    uart_write_bytes(CELL_UART_PORT, cmd, strlen(cmd));
    uart_write_bytes(CELL_UART_PORT, "\r\n", 2);
}

/* Read one LF-terminated line into buf; returns byte count or -1 on timeout */
static int at_readline(char *buf, int maxlen, uint32_t timeout_ms)
{
    int pos = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline && pos < maxlen - 1) {
        uint8_t c;
        if (uart_read_bytes(CELL_UART_PORT, &c, 1, pdMS_TO_TICKS(10)) <= 0) continue;
        if (c == '\r') continue;
        if (c == '\n') { buf[pos] = '\0'; return pos; }
        buf[pos++] = (char)c;
    }
    buf[pos] = '\0';
    return -1;
}

/*
 * Send an AT command and wait for OK / ERROR.
 * Copies the first non-echo, non-empty response line into resp_buf (optional).
 * Returns true on OK.
 */
static bool at_cmd(const char *cmd, char *resp_buf, int resp_len, uint32_t timeout_ms)
{
    uart_flush_input(CELL_UART_PORT);
    at_send(cmd);

    char line[256];
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    bool captured = false;

    while (xTaskGetTickCount() < deadline) {
        if (at_readline(line, sizeof(line), 200) < 0) continue;
        if (line[0] == '\0') continue;
        if (strncmp(line, cmd, strlen(cmd)) == 0) continue; /* echo */

        if (strcmp(line, "OK") == 0) return true;
        if (strncmp(line, "ERROR", 5) == 0 ||
            strncmp(line, "+CME ERROR", 10) == 0 ||
            strncmp(line, "+CMS ERROR", 10) == 0) {
            ESP_LOGW(TAG, "cmd=%s err=%s", cmd, line);
            return false;
        }
        if (!captured && resp_buf) {
            strncpy(resp_buf, line, resp_len - 1);
            resp_buf[resp_len - 1] = '\0';
            captured = true;
        }
    }
    ESP_LOGW(TAG, "cmd=%s → timeout", cmd);
    return false;
}

/* ── Network helpers ─────────────────────────────────────────────────────── */

/* Poll +CREG until stat==1 (home) or stat==5 (roaming) */
static bool wait_registered(uint32_t timeout_ms)
{
    char resp[64];
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        if (at_cmd("AT+CREG?", resp, sizeof(resp), 2000)) {
            int n, stat;
            if (sscanf(resp, "+CREG: %d,%d", &n, &stat) == 2 &&
                (stat == 1 || stat == 5)) {
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    return false;
}

/* ── MQTT helpers ────────────────────────────────────────────────────────── */

static esp_err_t mqtt_connect(void)
{
    char cmd[256];

    /* Enable MQTT instance */
    snprintf(cmd, sizeof(cmd), "AT#MQEN=%d,1", MQTT_INSTANCE);
    if (!at_cmd(cmd, NULL, 0, AT_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "AT#MQEN failed");
        return ESP_FAIL;
    }

    /* Configure broker address */
    snprintf(cmd, sizeof(cmd), "AT#MQCFG=%d,\"%s\",%d,0,0",
             MQTT_INSTANCE, MQTT_BROKER, MQTT_PORT);
    if (!at_cmd(cmd, NULL, 0, AT_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "AT#MQCFG failed");
        return ESP_FAIL;
    }

    /* Connect: instance, clientId, user, pass, cleanSession=1, keepAlive=60 */
    snprintf(cmd, sizeof(cmd), "AT#MQCONN=%d,\"%s\",\"%s\",\"%s\",1,60",
             MQTT_INSTANCE, MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD);
    if (!at_cmd(cmd, NULL, 0, (uint32_t)MQTT_CONN_TIMEOUT_S * 1000 + 2000)) {
        ESP_LOGE(TAG, "AT#MQCONN failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "MQTT connected  broker=%s:%d  client=%s",
             MQTT_BROKER, MQTT_PORT, MQTT_CLIENT_ID);
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t cellular_init(void)
{
    const uart_config_t uart_cfg = {
        .baud_rate  = CELL_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(CELL_UART_PORT,
                                        CELL_UART_BUF_SIZE, CELL_UART_BUF_SIZE,
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CELL_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(CELL_UART_PORT,
                                 CELL_UART_TX, CELL_UART_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    /* Give the module time to boot, then probe */
    vTaskDelay(pdMS_TO_TICKS(1000));
    bool alive = false;
    for (int i = 0; i < 5; i++) {
        if (at_cmd("AT", NULL, 0, 1000)) { alive = true; break; }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!alive) {
        ESP_LOGE(TAG, "ME310G1 not responding");
        return ESP_ERR_TIMEOUT;
    }

    at_cmd("ATE0", NULL, 0, AT_TIMEOUT_MS); /* disable echo */
    at_cmd("AT+CMEE=2", NULL, 0, AT_TIMEOUT_MS); /* verbose errors */

    ESP_LOGI(TAG, "Waiting for network registration (up to %us)...",
             REG_TIMEOUT_MS / 1000);
    if (!wait_registered(REG_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "Network registration timed out");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "Registered on network");

    return mqtt_connect();
}

esp_err_t cellular_mqtt_publish(const char *topic, const char *payload)
{
    char cmd[512];
    /* AT#MQPUB=<inst>,<retain>,<qos>,<timeout_s>,"<topic>","<payload>" */
    snprintf(cmd, sizeof(cmd), "AT#MQPUB=%d,0,0,%d,\"%s\",\"%s\"",
             MQTT_INSTANCE, MQTT_PUB_TIMEOUT_S, topic, payload);

    if (!at_cmd(cmd, NULL, 0, (uint32_t)MQTT_PUB_TIMEOUT_S * 1000 + 2000)) {
        /* Try to reconnect once */
        ESP_LOGW(TAG, "Publish failed, reconnecting...");
        if (mqtt_connect() != ESP_OK) return ESP_FAIL;
        if (!at_cmd(cmd, NULL, 0, (uint32_t)MQTT_PUB_TIMEOUT_S * 1000 + 2000)) {
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Published  topic=%s  payload=%s", topic, payload);
    return ESP_OK;
}
