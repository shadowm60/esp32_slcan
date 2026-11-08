#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*------------------------------------------------------------*/
/* Macros */
#define NUM_OF_SPIN_TASKS   6
#define SPIN_ITER           500000  //Actual CPU cycles used will depend on compiler optimization
#define SPIN_TASK_PRIO      2
#define STATS_TASK_PRIO     3
#define ITF_TASK_PRIO       4
#define RX_TASK_PRIO        5
#define TX_TASK_PRIO        6
#define STATS_TICKS         pdMS_TO_TICKS(1000)
#define ARRAY_SIZE_OFFSET   5   //Increase this if print_real_time_stats returns ESP_ERR_INVALID_SIZE

esp_err_t bt_spp_send(const uint8_t *data, size_t len);
void store_slcan_command(const char *cmd_str);
void slcan_feed_bytes(const uint8_t *data, size_t len);
esp_err_t slcan_transport_init(bool use_bluetooth);
esp_err_t slcan_transport_write(const uint8_t *data, size_t len);
void slcan_uart_rx_task(void *arg);