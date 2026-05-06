#pragma once

#include "esp_err.h"

/**
 * Initialize UART, wait for modem, register on network, and connect MQTT.
 * Blocks until connected or returns an error code.
 */
esp_err_t cellular_init(void);

/**
 * Publish payload to an MQTT topic.
 * Reconnects automatically if the broker connection was lost.
 * Thread-safe (caller must not call concurrently).
 */
esp_err_t cellular_mqtt_publish(const char *topic, const char *payload);
