#include "sdkconfig.h"
#include "main.h"
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "bt_usr.h"
#include "slcan.h"



// PIN 4  CANTX 
// PIN 5  CANRX 
// PIN 12 BLUETOOTH SWITCH

// Define the targeted GPIO pin

#define CAN_TX_GPIO       GPIO_NUM_4
#define CAN_RX_GPIO       GPIO_NUM_5
/* BT pin level 0 -> BT serial, 1 -> USB serial */
#define BT_SWITCH_GPIO    GPIO_NUM_12
#define BT_SERIAL_CON     0
#define USB_SERIAL_CON    1


/****** SLCAN *******/


static QueueHandle_t slcan_rx_queue = NULL;



static const char *TAG = "ESP_SLCAN";
static char task_names[NUM_OF_SPIN_TASKS][configMAX_TASK_NAME_LEN];
static SemaphoreHandle_t sync_spin_task;
static SemaphoreHandle_t sync_stats_task;
static bool use_bluetooth_transport;
static char slcan_input_buffer[SLCAN_MAX_CMD_LEN];
static size_t slcan_input_length;

static int discard_log_output(const char *format, va_list args)
{
    (void)format;
    (void)args;
    return 0;
}

esp_err_t slcan_transport_init(bool use_bluetooth)
{
    use_bluetooth_transport = use_bluetooth;
    if (use_bluetooth) {
        return bt_spp_transport_init();
    }

    if (!uart_is_driver_installed(UART_NUM_0)) {
        esp_err_t err = uart_driver_install(UART_NUM_0, 2048, 2048, 0, NULL, 0);
        if (err != ESP_OK) {
            return err;
        }
    }

    esp_err_t err = uart_set_baudrate(UART_NUM_0, CONFIG_ESP_CONSOLE_UART_BAUDRATE);
    if (err != ESP_OK) {
        return err;
    }

    return uart_flush_input(UART_NUM_0);
}

esp_err_t slcan_transport_write(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (use_bluetooth_transport) {
        return bt_spp_send(data, len);
    }

    int written = uart_write_bytes(UART_NUM_0, data, len);
    return written == (int)len ? ESP_OK : ESP_FAIL;
}

void store_slcan_command(const char *cmd_str) {
    if (slcan_rx_queue == NULL) return;

    slcan_queue_item_t item;
    // Safely copy string and ensure null-termination
    strncpy(item.data, cmd_str, sizeof(item.data) - 1);
    item.data[sizeof(item.data) - 1] = '\0';

    // Push to queue. If queue is full, don't block the BT stack (timeout = 0)
    if (xQueueSend(slcan_rx_queue, &item, 0) != pdPASS) {
        ESP_LOGE(TAG, "Queue full! Dropped command: %s", cmd_str);
    }
}

void slcan_feed_bytes(const uint8_t *data, size_t len)
{
    if (data == NULL) {
        return;
    }

    for (size_t i = 0; i < len; i++) {
        char current_char = (char)data[i];
        if (current_char == '\n') {
            continue;
        }
        if (current_char == '\r') {
            slcan_input_buffer[slcan_input_length] = '\0';
            if (slcan_input_length > 0) {
                store_slcan_command(slcan_input_buffer);
            }
            slcan_input_length = 0;
        } else if (slcan_input_length < sizeof(slcan_input_buffer) - 1) {
            slcan_input_buffer[slcan_input_length++] = current_char;
        } else {
            ESP_LOGE(TAG, "SLCAN command too long; discarded");
            slcan_input_length = 0;
        }
    }
}


static int read_bt_switch_state(void)
{
    // 1. Structure definition for the GPIO configuration
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BT_SWITCH_GPIO), // Set bit mask for GPIO 12
        .mode = GPIO_MODE_INPUT,                  // Configure as an input mode
        .pull_up_en = GPIO_PULLUP_ENABLE,         // Activate the internal pull-up resistor
        .pull_down_en = GPIO_PULLDOWN_DISABLE,    // Deactivate the internal pull-down resistor
        .intr_type = GPIO_INTR_DISABLE            // Disable interrupts for standard reading
    };
    
    // 2. Apply the configuration to the register
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "GPIO %d initialized. Waiting 250ms...", BT_SWITCH_GPIO);

    // 3. Wait for 250 milliseconds using FreeRTOS delays
    // pdMS_TO_TICKS safely converts milliseconds into system ticks
    vTaskDelay(pdMS_TO_TICKS(250));

    // 4. Read the state of the pin
    // With an active pull-up, it defaults to 1 (HIGH) unless grounded (LOW)
    int pin_status = gpio_get_level(BT_SWITCH_GPIO);

    ESP_LOGI(TAG, "GPIO %d status read: %d", BT_SWITCH_GPIO, pin_status);

    return pin_status;
}

/* TASKS */

static void slcan_itf_task(void *arg)
{
    slcan_queue_item_t item;
    while (1) {
        if (xQueueReceive(slcan_rx_queue, &item, portMAX_DELAY) == pdTRUE) {
            slcan_process_command(item.data);
        }
    }
}

static void slcan_tx_task(void *arg)
{
    while (1) {
        //Consume CPU cycles
        for (int i = 0; i < SPIN_ITER; i++) {
            __asm__ __volatile__("NOP");
        }
        vTaskDelay(1);
    }    

}

void slcan_uart_rx_task(void *arg)
{
    uint8_t data[64];
    while (1) {
        int len = uart_read_bytes(UART_NUM_0, data, sizeof(data), portMAX_DELAY);
        if (len > 0) {
            slcan_feed_bytes(data, (size_t)len);
        }
    }
}

void app_main(void)
{
    //Allow other core to finish initialization
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    // Initialize the FreeRTOS Queue
    slcan_rx_queue = xQueueCreate(SLCAN_QUEUE_LEN, sizeof(slcan_queue_item_t));

    if (slcan_rx_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create SLCAN RX Queue!");
        return;
    }
    ESP_ERROR_CHECK(slcan_init());
    //startup preparation
    //init gpio
    //read startup configuration
    bool use_bluetooth = read_bt_switch_state() == BT_SERIAL_CON;
    ret = slcan_transport_init(use_bluetooth);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Selected SLCAN transport failed to initialize: %s", esp_err_to_name(ret));
        return;
    }

    if (use_bluetooth) {
        ESP_LOGI(TAG, "Bluetooth SPP server is ready; waiting for a host connection");
    } else {
        ESP_LOGI(TAG, "UART transport is ready");
        if (xTaskCreatePinnedToCore(slcan_uart_rx_task, "uart_rx_tsk", 2048, NULL,
                                    RX_TASK_PRIO, NULL, tskNO_AFFINITY) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create UART receive task; SLCAN startup cancelled");
            return;
        }
    }
    //start tasks for slcan interface, tx and rx

    /*
     * Creates a counting semaphore and returns a handle by which the newly created semaphore can be referenced.
     * UBaseType_t uxMaxCount - The maximum count value that can be reached. When the semaphore reaches this value it can no longer be 'given'.
     *  UBaseType_t uxInitialCount - The count value assigned to the semaphore when it is created.
     */
    sync_spin_task = xSemaphoreCreateCounting(NUM_OF_SPIN_TASKS, 0);
    //Creates a binary semaphore, and returns a handle by which the semaphore can be referenced.
    sync_stats_task = xSemaphoreCreateBinary();

    ESP_LOGI(TAG, "Creating Tasks");
    /*
     * Creates a task and places it in the Ready state.
     * TaskFunction_t pvTaskCode - A pointer to the function that will be used as the task entry function.
     * const char *pcName - A descriptive name for the task. This is not used by the FreeRTOS kernel, but is used by debugging tools.
     * configSTACK_DEPTH_TYPE usStackDepth - The size of the stack to allocate for the task.
     * void *pvParameters - A pointer to the parameters to be passed to the task.
     * UBaseType_t uxPriority - The priority of the task.
     * TaskHandle_t *pxCreatedTask - A pointer to a variable that will hold the handle of the created task.
     * BaseType_t xCoreID - The core on which the task will run.
     */
    xTaskCreatePinnedToCore(slcan_itf_task, "itf_tsk", 4096, NULL, ITF_TASK_PRIO, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(slcan_tx_task, "tx_tsk", 1024, NULL, TX_TASK_PRIO, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(slcan_can_rx_task, "rx_tsk", 2048, NULL, RX_TASK_PRIO, NULL, tskNO_AFFINITY);

    xSemaphoreGive(sync_stats_task);    
    if (!use_bluetooth) {
        esp_log_set_vprintf(discard_log_output);
    }
}
