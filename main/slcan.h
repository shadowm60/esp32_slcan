#pragma once

#include "esp_err.h"

#define SLCAN_BUF_SIZE          64
#define SLCAN_MAX_CMD_LEN       SLCAN_BUF_SIZE
#define SLCAN_QUEUE_LEN         20

#define SLCAN_TX_GPIO GPIO_NUM_4
#define SLCAN_RX_GPIO GPIO_NUM_5
#define SLCAN_CAN_RX_QUEUE_LEN 20

typedef struct {
    char data[SLCAN_MAX_CMD_LEN];
} slcan_queue_item_t;

esp_err_t slcan_init(void);
void slcan_process_command(const char *command);
void slcan_can_rx_task(void *arg);