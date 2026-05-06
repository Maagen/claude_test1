#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "CAN.h"

#define CAN_TX_GPIO         GPIO_NUM_4
#define CAN_RX_GPIO         GPIO_NUM_5
#define CAN_BITRATE         500000
#define CAN_TX_QUEUE_LEN    10
#define CAN_RX_QUEUE_LEN    10

static const char *TAG = "CAN";

static twai_node_handle_t s_node;
static QueueHandle_t      s_tx_queue;
static QueueHandle_t      s_rx_queue;

/* Called from ISR when a frame arrives — reads it and posts to the RX queue */
static bool IRAM_ATTR on_rx_done(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *user_ctx)
{
    uint8_t buf[8] = {0};
    twai_frame_t frame = { .buffer = buf, .buffer_len = sizeof(buf) };

    if (twai_node_receive_from_isr(handle, &frame) != ESP_OK) {
        return false;
    }

    can_msg_t msg = { .id = frame.header.id, .dlc = (uint8_t)frame.header.dlc };
    memcpy(msg.data, buf, msg.dlc <= 8 ? msg.dlc : 8);

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR((QueueHandle_t)user_ctx, &msg, &woken);
    return woken == pdTRUE;
}

/* Dequeues messages and transmits them on the CAN bus */
static void can_tx_task(void *arg)
{
    can_msg_t msg;
    while (1) {
        if (xQueueReceive(s_tx_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        twai_frame_t frame = {
            .header = { .id = msg.id, .dlc = msg.dlc, .ide = 0, .rtr = 0 },
            .buffer     = msg.data,
            .buffer_len = msg.dlc,
        };
        esp_err_t ret = twai_node_transmit(s_node, &frame, 100);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "TX id=0x%03" PRIx32 " len=%d  %02x %02x %02x %02x %02x %02x %02x %02x",
                     msg.id, msg.dlc,
                     msg.data[0], msg.data[1], msg.data[2], msg.data[3],
                     msg.data[4], msg.data[5], msg.data[6], msg.data[7]);
        } else {
            ESP_LOGW(TAG, "TX failed: %s", esp_err_to_name(ret));
        }
    }
}

/* Receives messages posted by the ISR callback and logs them */
static void can_rx_task(void *arg)
{
    can_msg_t msg;
    while (1) {
        if (xQueueReceive(s_rx_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        ESP_LOGI(TAG, "RX id=0x%03" PRIx32 " len=%d  %02x %02x %02x %02x %02x %02x %02x %02x",
                 msg.id, msg.dlc,
                 msg.data[0], msg.data[1], msg.data[2], msg.data[3],
                 msg.data[4], msg.data[5], msg.data[6], msg.data[7]);
    }
}

esp_err_t CAN_init(void)
{
    twai_onchip_node_config_t node_cfg = {
        .io_cfg = {
            .tx                = CAN_TX_GPIO,
            .rx                = CAN_RX_GPIO,
            .quanta_clk_out    = -1,
            .bus_off_indicator = -1,
        },
        .bit_timing     = { .bitrate = CAN_BITRATE },
        .tx_queue_depth = CAN_TX_QUEUE_LEN,
    };

    esp_err_t ret = twai_new_node_onchip(&node_cfg, &s_node);
    if (ret != ESP_OK) return ret;

    s_rx_queue = xQueueCreate(CAN_RX_QUEUE_LEN, sizeof(can_msg_t));
    s_tx_queue = xQueueCreate(CAN_TX_QUEUE_LEN, sizeof(can_msg_t));
    if (!s_rx_queue || !s_tx_queue) return ESP_ERR_NO_MEM;

    twai_event_callbacks_t cbs = { .on_rx_done = on_rx_done };
    ret = twai_node_register_event_callbacks(s_node, &cbs, s_rx_queue);
    if (ret != ESP_OK) return ret;

    twai_mask_filter_config_t f_cfg = { .id = 0, .mask = 0, .is_ext = false };
    ret = twai_node_config_mask_filter(s_node, 0, &f_cfg);
    if (ret != ESP_OK) return ret;

    ret = twai_node_enable(s_node);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Started  TX=GPIO%d  RX=GPIO%d  %u kbps",
             CAN_TX_GPIO, CAN_RX_GPIO, CAN_BITRATE / 1000);

    xTaskCreate(can_tx_task, "can_tx", 4096, NULL, 5, NULL);
    xTaskCreate(can_rx_task, "can_rx", 4096, NULL, 5, NULL);

    return ESP_OK;
}

bool CAN_send(const can_msg_t *msg, uint32_t timeout_ms)
{
    return xQueueSend(s_tx_queue, msg, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
