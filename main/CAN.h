#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
} can_msg_t;

/**
 * Initialize the CAN/TWAI driver, queues, and internal tasks.
 * Must be called once before CAN_send().
 */
esp_err_t CAN_init(void);

/**
 * Place a message onto the CAN TX queue.
 * Thread-safe; can be called from any task.
 *
 * @param msg        Message to send
 * @param timeout_ms Max time to wait if the queue is full (ms)
 * @return true if enqueued, false if queue full / timeout
 */
bool CAN_send(const can_msg_t *msg, uint32_t timeout_ms);
