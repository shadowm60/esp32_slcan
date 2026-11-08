#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"
#include "esp_log.h"

#define SPP_TAG "BT_SPP"
#define SPP_SERVER_NAME "BT_SPP_SRV"

esp_err_t bt_spp_transport_init(void);
