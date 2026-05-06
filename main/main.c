#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "CAN.h"

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

void app_main(void)
{
    ESP_ERROR_CHECK(CAN_init());
    xTaskCreate(can_producer_task, "can_producer", 4096, NULL, 4, NULL);
}
