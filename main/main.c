#include <inttypes.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "CAN.h"
#include "cellular.h"

#define MQTT_TOPIC          "esp32s3/status"
#define MQTT_PUBLISH_MS     60000

static const char *TAG = "MAIN";

/* Sends one CAN message per second using CAN_send() */
static void can_producer_task(void *arg)
{
    uint32_t counter = 0;
    while (1) {
        can_msg_t msg = {
            .id  = 0x123,
            .dlc = 8,
            .data = {
                (counter >> 24) & 0xFF,
                (counter >> 16) & 0xFF,
                (counter >>  8) & 0xFF,
                (counter      ) & 0xFF,
                0xDE, 0xAD, 0xBE, 0xEF,
            },
        };
        if (!CAN_send(&msg, 100)) {
            ESP_LOGW(TAG, "CAN_send failed — queue full");
        }
        counter++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* Publishes an MQTT message every MQTT_PUBLISH_MS milliseconds */
static void mqtt_publish_task(void *arg)
{
    uint32_t counter = 0;
    char payload[64];

    /* Wait for cellular_init() to have finished before first publish */
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        snprintf(payload, sizeof(payload), "counter=%"PRIu32",uptime=%"PRIu32"s",
                 counter, uptime_s);
        if (cellular_mqtt_publish(MQTT_TOPIC, payload) != ESP_OK) {
            ESP_LOGW(TAG, "MQTT publish failed, will retry next interval");
        }
        counter++;
        vTaskDelay(pdMS_TO_TICKS(MQTT_PUBLISH_MS));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(CAN_init());
    xTaskCreate(can_producer_task, "can_producer", 4096, NULL, 4, NULL);

    ESP_ERROR_CHECK(cellular_init());
    xTaskCreate(mqtt_publish_task, "mqtt_pub", 4096, NULL, 3, NULL);
}
