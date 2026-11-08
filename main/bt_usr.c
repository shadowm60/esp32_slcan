#include "bt_usr.h"
#include "slcan.h"
#include "main.h"
#include "freertos/event_groups.h"


static const char local_device_name[] = CONFIG_ESP32_SLCAN_LOCAL_DEVICE_NAME;
static const esp_spp_mode_t esp_spp_mode = ESP_SPP_MODE_CB;
static const bool esp_spp_enable_l2cap_ertm = true;

static const esp_spp_sec_t sec_mask = ESP_SPP_SEC_AUTHENTICATE;
static const esp_spp_role_t role_slave = ESP_SPP_ROLE_SLAVE;

static portMUX_TYPE spp_connection_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t spp_connection_handle;
static bool spp_connected;
static EventGroupHandle_t spp_start_events;

#define SPP_SERVER_READY_BIT  BIT0
#define SPP_SERVER_FAILED_BIT BIT1
#define SPP_SERVER_START_TIMEOUT_MS 10000

static esp_err_t spp_write(uint32_t handle, const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0 || len > INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_spp_write(handle, (int)len, (uint8_t *)data);
}

esp_err_t bt_spp_send(const uint8_t *data, size_t len)
{
    uint32_t handle;
    bool connected;

    if (data == NULL || len == 0 || len > INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&spp_connection_mux);
    connected = spp_connected;
    handle = spp_connection_handle;
    portEXIT_CRITICAL(&spp_connection_mux);

    if (!connected) {
        return ESP_ERR_INVALID_STATE;
    }

    return spp_write(handle, data, len);
}


static char *bda2str(uint8_t * bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }

    uint8_t *p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}

static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    char bda_str[18] = {0};

    switch (event) {
    case ESP_SPP_INIT_EVT:
        if (param->init.status == ESP_SPP_SUCCESS) {
            ESP_LOGI(SPP_TAG, "ESP_SPP_INIT_EVT");
            esp_err_t err = esp_spp_start_srv(sec_mask, role_slave, 0, SPP_SERVER_NAME);
            if (err != ESP_OK) {
                ESP_LOGE(SPP_TAG, "Failed to start SPP server: %s", esp_err_to_name(err));
                xEventGroupSetBits(spp_start_events, SPP_SERVER_FAILED_BIT);
            }
        } else {
            ESP_LOGE(SPP_TAG, "ESP_SPP_INIT_EVT status:%d", param->init.status);
            xEventGroupSetBits(spp_start_events, SPP_SERVER_FAILED_BIT);
        }
        break;
    case ESP_SPP_DISCOVERY_COMP_EVT:
        ESP_LOGI(SPP_TAG, "ESP_SPP_DISCOVERY_COMP_EVT");
        break;
    case ESP_SPP_OPEN_EVT:
        ESP_LOGI(SPP_TAG, "ESP_SPP_OPEN_EVT");
        break;
    case ESP_SPP_CLOSE_EVT:
        ESP_LOGI(SPP_TAG, "ESP_SPP_CLOSE_EVT status:%d handle:%"PRIu32" close_by_remote:%d", param->close.status,
                 param->close.handle, param->close.async);
        portENTER_CRITICAL(&spp_connection_mux);
        if (spp_connected && spp_connection_handle == param->close.handle) {
            spp_connected = false;
        }
        portEXIT_CRITICAL(&spp_connection_mux);
        break;
    case ESP_SPP_START_EVT:
        if (param->start.status == ESP_SPP_SUCCESS) {
            ESP_LOGI(SPP_TAG, "ESP_SPP_START_EVT handle:%"PRIu32" sec_id:%d scn:%d", param->start.handle, param->start.sec_id,
                     param->start.scn);
            ESP_LOGI(SPP_TAG, "Setting device name to: %s", local_device_name);
            esp_err_t err = esp_bt_gap_set_device_name(local_device_name);
            if (err != ESP_OK) {
                ESP_LOGE(SPP_TAG, "Failed to set device name: %s", esp_err_to_name(err));
                xEventGroupSetBits(spp_start_events, SPP_SERVER_FAILED_BIT);
                break;
            }

            err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            if (err != ESP_OK) {
                ESP_LOGE(SPP_TAG, "Failed to set discoverable scan mode: %s", esp_err_to_name(err));
                xEventGroupSetBits(spp_start_events, SPP_SERVER_FAILED_BIT);
                break;
            }

            ESP_LOGI(SPP_TAG, "Connectable and discoverable scan mode request accepted");
            xEventGroupSetBits(spp_start_events, SPP_SERVER_READY_BIT);
        } else {
            ESP_LOGE(SPP_TAG, "ESP_SPP_START_EVT status:%d", param->start.status);
            xEventGroupSetBits(spp_start_events, SPP_SERVER_FAILED_BIT);
        }
        break;
    case ESP_SPP_CL_INIT_EVT:
        ESP_LOGI(SPP_TAG, "ESP_SPP_CL_INIT_EVT");
        break;
    case ESP_SPP_DATA_IND_EVT:
#ifdef DEBUG_BT_RCV    
        ESP_LOGI(SPP_TAG, "ESP_SPP_DATA_IND_EVT len:%d handle:%"PRIu32, param->data_ind.len, param->data_ind.handle);
        if (param->data_ind.len < 128) {
            ESP_LOG_BUFFER_HEX("", param->data_ind.data, param->data_ind.len);
        }

        if (param->data_ind.status == ESP_SPP_SUCCESS && param->data_ind.len > 0) {
            esp_err_t err = spp_write(param->data_ind.handle,
                                      param->data_ind.data,
                                      param->data_ind.len);
            if (err != ESP_OK) {
                ESP_LOGE(SPP_TAG, "Failed to echo SPP data: %s", esp_err_to_name(err));
            }
        }
#else
        if (param->data_ind.status == ESP_SPP_SUCCESS && param->data_ind.len > 0) {
            slcan_feed_bytes(param->data_ind.data, param->data_ind.len);
        }
#endif        
        break;
    case ESP_SPP_CONG_EVT:
        ESP_LOGI(SPP_TAG, "ESP_SPP_CONG_EVT");
        break;
    case ESP_SPP_WRITE_EVT:
        ESP_LOGI(SPP_TAG, "ESP_SPP_WRITE_EVT");
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        ESP_LOGI(SPP_TAG, "ESP_SPP_SRV_OPEN_EVT status:%d handle:%"PRIu32", rem_bda:[%s]", param->srv_open.status,
                 param->srv_open.handle, bda2str(param->srv_open.rem_bda, bda_str, sizeof(bda_str)));
        if (param->srv_open.status == ESP_SPP_SUCCESS) {
            portENTER_CRITICAL(&spp_connection_mux);
            spp_connection_handle = param->srv_open.handle;
            spp_connected = true;
            portEXIT_CRITICAL(&spp_connection_mux);
        }
        break;
    case ESP_SPP_SRV_STOP_EVT:
        ESP_LOGI(SPP_TAG, "ESP_SPP_SRV_STOP_EVT");
        break;
    case ESP_SPP_UNINIT_EVT:
        ESP_LOGI(SPP_TAG, "ESP_SPP_UNINIT_EVT");
        break;
    default:
        break;
    }
}

void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    char bda_str[18] = {0};

    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:{
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(SPP_TAG, "authentication success: %s bda:[%s]", param->auth_cmpl.device_name,
                     bda2str(param->auth_cmpl.bda, bda_str, sizeof(bda_str)));
        } else {
            ESP_LOGE(SPP_TAG, "authentication failed, status:%d", param->auth_cmpl.stat);
        }
        break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT:{
        ESP_LOGI(SPP_TAG, "ESP_BT_GAP_PIN_REQ_EVT min_16_digit:%d", param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit) {
            ESP_LOGI(SPP_TAG, "Input pin code: 0000 0000 0000 0000");
            esp_bt_pin_code_t pin_code = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {
            ESP_LOGI(SPP_TAG, "Input pin code: 1234");
            esp_bt_pin_code_t pin_code;
            pin_code[0] = '1';
            pin_code[1] = '2';
            pin_code[2] = '3';
            pin_code[3] = '4';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }

#if (CONFIG_ESP32_SLCAN_SSP_ENABLED == true)
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(SPP_TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %06"PRIu32, param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(SPP_TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey:%06"PRIu32, param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(SPP_TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
        break;
#endif

    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(SPP_TAG, "ESP_BT_GAP_MODE_CHG_EVT mode:%d bda:[%s]", param->mode_chg.mode,
                 bda2str(param->mode_chg.bda, bda_str, sizeof(bda_str)));
        break;
    case ESP_BT_GAP_READ_RSSI_DELTA_EVT: // 16
            ESP_LOGI("BT_GAP", "Event 16: RSSI Delta Read Complete");
        break;
    case ESP_BT_GAP_CONFIG_EIR_DATA_EVT: // 17
            ESP_LOGI("BT_GAP", "Event 17: EIR Data Configured");
        break;

    default: {
        ESP_LOGI(SPP_TAG, "event: %d", event);
        break;
    }
    }
    return;
}


esp_err_t bt_spp_transport_init(void)
{
    char bda_str[18] = {0};
    esp_err_t ret;

    spp_start_events = xEventGroupCreate();
    if (spp_start_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(SPP_TAG, "Failed to release Bluetooth LE memory: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        ESP_LOGE(SPP_TAG, "%s initialize controller failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }

    if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        ESP_LOGE(SPP_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
#if (CONFIG_ESP32_SLCAN_SSP_ENABLED == false)
    bluedroid_cfg.ssp_en = false;
#endif
    if ((ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg)) != ESP_OK) {
        ESP_LOGE(SPP_TAG, "%s initialize bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }

    if ((ret = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(SPP_TAG, "%s enable bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }

    if ((ret = esp_bt_gap_register_callback(esp_bt_gap_cb)) != ESP_OK) {
        ESP_LOGE(SPP_TAG, "%s gap register failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }

    if ((ret = esp_spp_register_callback(esp_spp_cb)) != ESP_OK) {
        ESP_LOGE(SPP_TAG, "%s spp register failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }

    esp_spp_cfg_t bt_spp_cfg = {
        .mode = esp_spp_mode,
        .enable_l2cap_ertm = esp_spp_enable_l2cap_ertm,
        .tx_buffer_size = 0, /* Only used for ESP_SPP_MODE_VFS mode */
    };
    if ((ret = esp_spp_enhanced_init(&bt_spp_cfg)) != ESP_OK) {
        ESP_LOGE(SPP_TAG, "%s spp init failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }

#if (CONFIG_ESP32_SLCAN_SSP_ENABLED == true)
    /* Set default parameters for Secure Simple Pairing */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    ret = esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
    if (ret != ESP_OK) {
        ESP_LOGE(SPP_TAG, "Failed to configure Secure Simple Pairing: %s", esp_err_to_name(ret));
        return ret;
    }
#endif

    /*
     * Set default parameters for Legacy Pairing
     * Use variable pin, input pin code when pairing
     */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    ret = esp_bt_gap_set_pin(pin_type, 0, pin_code);
    if (ret != ESP_OK) {
        ESP_LOGE(SPP_TAG, "Failed to configure Bluetooth PIN: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(SPP_TAG, "Own address:[%s]", bda2str((uint8_t *)esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));

    EventBits_t bits = xEventGroupWaitBits(spp_start_events,
                                           SPP_SERVER_READY_BIT | SPP_SERVER_FAILED_BIT,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(SPP_SERVER_START_TIMEOUT_MS));
    if ((bits & SPP_SERVER_READY_BIT) != 0) {
        return ESP_OK;
    }
    if ((bits & SPP_SERVER_FAILED_BIT) != 0) {
        return ESP_FAIL;
    }

    ESP_LOGE(SPP_TAG, "Timed out waiting for the SPP server to become ready");
    return ESP_ERR_TIMEOUT;
}
