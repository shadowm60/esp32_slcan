#include "slcan.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_bt_device.h"
#include "esp_log.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "main.h"



typedef struct {
    twai_frame_header_t header;
    uint8_t data[TWAI_FRAME_MAX_LEN];
} slcan_can_rx_item_t;

static const char *TAG = "SLCAN";
static const uint32_t slcan_bitrates[] = {
    10000, 20000, 50000, 100000, 125000, 250000, 500000, 800000, 1000000
};
static QueueHandle_t can_rx_queue;
static twai_node_handle_t can_node;
static uint32_t can_bitrate = 500000;
static bool can_open;
static bool can_listen_only;
static bool timestamps_enabled;
static volatile bool rx_queue_overflow;

static bool slcan_twai_rx_done(twai_node_handle_t handle,
                               const twai_rx_done_event_data_t *event,
                               void *user_ctx);

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static bool parse_hex(const char *text, size_t digits, uint32_t *value)
{
    uint32_t parsed = 0;

    for (size_t i = 0; i < digits; i++) {
        int nibble = hex_value(text[i]);
        if (nibble < 0) {
            return false;
        }
        parsed = (parsed << 4) | (uint32_t)nibble;
    }

    *value = parsed;
    return true;
}

static void send_response(const char *response)
{
    esp_err_t err = slcan_transport_write((const uint8_t *)response, strlen(response));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not send SLCAN response: %s", esp_err_to_name(err));
    }
}

static void send_ack(bool success)
{
    const char response[] = { success ? '\r' : '\a' };
    esp_err_t err = slcan_transport_write((const uint8_t *)response, sizeof(response));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not send SLCAN response: %s", esp_err_to_name(err));
    }
}

static bool create_can_node(bool listen_only)
{
    twai_onchip_node_config_t config = {0};
    config.io_cfg.tx = SLCAN_TX_GPIO;
    config.io_cfg.rx = SLCAN_RX_GPIO;
    config.io_cfg.quanta_clk_out = GPIO_NUM_NC;
    config.io_cfg.bus_off_indicator = GPIO_NUM_NC;
    config.bit_timing.bitrate = can_bitrate;
    config.tx_queue_depth = 5;
    config.fail_retry_cnt = -1;
    config.flags.enable_listen_only = listen_only;

    esp_err_t err = twai_new_node_onchip(&config, &can_node);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TWAI node creation failed: %s", esp_err_to_name(err));
        can_node = NULL;
        return false;
    }

    twai_event_callbacks_t callbacks = {
        .on_rx_done = slcan_twai_rx_done,
    };
    err = twai_node_register_event_callbacks(can_node, &callbacks, NULL);
    if (err == ESP_OK) {
        err = twai_node_enable(can_node);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TWAI node start failed: %s", esp_err_to_name(err));
        twai_node_delete(can_node);
        can_node = NULL;
        return false;
    }

    can_open = true;
    can_listen_only = listen_only;
    return true;
}

static bool stop_can_node(void)
{
    if (can_node == NULL) {
        can_open = false;
        return true;
    }

    esp_err_t err = twai_node_disable(can_node);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TWAI node stop failed: %s", esp_err_to_name(err));
        return false;
    }

    err = twai_node_delete(can_node);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TWAI node delete failed: %s", esp_err_to_name(err));
        return false;
    }

    can_node = NULL;
    can_open = false;
    can_listen_only = false;
    xQueueReset(can_rx_queue);
    return true;
}

static bool transmit_command(const char *command, size_t length)
{
    bool extended = command[0] == 'T' || command[0] == 'R';
    bool remote = command[0] == 'r' || command[0] == 'R';
    size_t id_digits = extended ? 8 : 3;
    size_t header_length = 1 + id_digits + 1;
    uint32_t id;

    if (!can_open || can_listen_only || length < header_length ||
        !parse_hex(command + 1, id_digits, &id)) {
        return false;
    }

    int dlc_nibble = hex_value(command[1 + id_digits]);
    if (dlc_nibble < 0 || dlc_nibble > TWAI_FRAME_MAX_LEN) {
        return false;
    }
    uint16_t dlc = (uint16_t)dlc_nibble;

    size_t expected_length = header_length + (remote ? 0 : dlc * 2);
    if (length != expected_length || (extended && id > TWAI_EXT_ID_MASK) ||
        (!extended && id > TWAI_STD_ID_MASK)) {
        return false;
    }

    uint8_t data[TWAI_FRAME_MAX_LEN] = {0};
    if (!remote) {
        for (size_t i = 0; i < dlc; i++) {
            uint32_t byte;
            if (!parse_hex(command + header_length + i * 2, 2, &byte)) {
                return false;
            }
            data[i] = (uint8_t)byte;
        }
    }

    twai_frame_t frame = {0};
    frame.header.id = id;
    frame.header.dlc = dlc;
    frame.header.ide = extended;
    frame.header.rtr = remote;
    frame.buffer = data;
    frame.buffer_len = remote ? 0 : dlc;

    esp_err_t err = twai_node_transmit(can_node, &frame, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TWAI transmit failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static uint8_t get_status_flags(void)
{
    uint8_t flags = 0;
    if (rx_queue_overflow) {
        flags |= 1 << 3;
        rx_queue_overflow = false;
    }

    if (can_open) {
        twai_node_status_t status = {0};
        if (twai_node_get_info(can_node, &status, NULL) == ESP_OK) {
            if (status.state == TWAI_ERROR_WARNING) {
                flags |= 1 << 2;
            } else if (status.state == TWAI_ERROR_PASSIVE) {
                flags |= 1 << 4;
            } else if (status.state == TWAI_ERROR_BUS_OFF) {
                flags |= 1 << 7;
            }
        }
    }
    return flags;
}

static bool slcan_twai_rx_done(twai_node_handle_t handle,
                               const twai_rx_done_event_data_t *event,
                               void *user_ctx)
{
    uint8_t data[TWAI_FRAME_MAX_LEN] = {0};
    twai_frame_t frame = {
        .buffer = data,
        .buffer_len = sizeof(data),
    };

    if (twai_node_receive_from_isr(handle, &frame) != ESP_OK) {
        return false;
    }

    slcan_can_rx_item_t item = {
        .header = frame.header,
    };
    if (!frame.header.rtr && frame.header.dlc <= sizeof(item.data)) {
        memcpy(item.data, data, frame.header.dlc);
    }

    BaseType_t higher_priority_task_woken = pdFALSE;
    if (xQueueSendFromISR(can_rx_queue, &item, &higher_priority_task_woken) != pdTRUE) {
        rx_queue_overflow = true;
    }
    return higher_priority_task_woken == pdTRUE;
}

esp_err_t slcan_init(void)
{
    can_rx_queue = xQueueCreate(SLCAN_CAN_RX_QUEUE_LEN, sizeof(slcan_can_rx_item_t));
    return can_rx_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void slcan_process_command(const char *command)
{
    if (command == NULL || command[0] == '\0') {
        send_ack(false);
        return;
    }

    size_t length = strlen(command);
    switch (command[0]) {
    case 'O':
    case 'L':
        if (length == 1 && !can_open) {
            send_ack(create_can_node(command[0] == 'L'));
        } else {
            send_ack(false);
        }
        break;
    case 'C':
        send_ack(length == 1 && stop_can_node());
        break;
    case 'S': {
        uint32_t index;
        if (length != 2 || can_open || !parse_hex(command + 1, 1, &index) ||
            index >= sizeof(slcan_bitrates) / sizeof(slcan_bitrates[0])) {
            send_ack(false);
            break;
        }
        can_bitrate = slcan_bitrates[index];
        send_ack(true);
        break;
    }
    case 't':
    case 'T':
    case 'r':
    case 'R':
        send_ack(transmit_command(command, length));
        break;
    case 'V':
        send_response("V0100\r");
        break;
    case 'v':
        send_response("v0100\r");
        break;
    case 'N': {
        const uint8_t *address = esp_bt_dev_get_address();
        if (address == NULL) {
            send_ack(false);
            break;
        }
        char response[9];
        snprintf(response, sizeof(response), "N%02X%02X%02X\r", address[3], address[4], address[5]);
        send_response(response);
        break;
    }
    case 'F': {
        char response[5];
        snprintf(response, sizeof(response), "F%02X\r", get_status_flags());
        send_response(response);
        break;
    }
    case 'Z':
        if (length != 2 || (command[1] != '0' && command[1] != '1')) {
            send_ack(false);
        } else {
            timestamps_enabled = command[1] == '1';
            send_ack(true);
        }
        break;
    default:
        ESP_LOGW(TAG, "Unsupported SLCAN command: %s", command);
        send_ack(false);
        break;
    }
}

void slcan_can_rx_task(void *arg)
{
    slcan_can_rx_item_t item;
    char response[32];

    while (xQueueReceive(can_rx_queue, &item, portMAX_DELAY) == pdTRUE) {
        bool extended = item.header.ide;
        bool remote = item.header.rtr;
        size_t id_digits = extended ? 8 : 3;
        size_t position = (size_t)snprintf(response, sizeof(response), "%c%0*" PRIX32 "%X",
                                           remote ? (extended ? 'R' : 'r') : (extended ? 'T' : 't'),
                                           (int)id_digits, item.header.id, item.header.dlc);
        if (!remote) {
            for (size_t i = 0; i < item.header.dlc && position + 2 < sizeof(response); i++) {
                position += (size_t)snprintf(response + position, sizeof(response) - position,
                                              "%02X", item.data[i]);
            }
        }
        if (timestamps_enabled && position + 4 < sizeof(response)) {
            uint16_t timestamp = (uint16_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            position += (size_t)snprintf(response + position, sizeof(response) - position, "%04X", timestamp);
        }
        response[position++] = '\r';
        response[position] = '\0';
        send_response(response);
    }
}