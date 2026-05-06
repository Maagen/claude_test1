#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"

void app_main(void)
{
    printf("ESP32-S3 starter project\n");

    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("Chip: %s, cores: %d\n", CONFIG_IDF_TARGET, chip_info.cores);

    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("Flash: %" PRIu32 " MB\n", flash_size / (1024 * 1024));
    }

    printf("Minimum free heap: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());

    while (1) {
        printf("Running...\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
