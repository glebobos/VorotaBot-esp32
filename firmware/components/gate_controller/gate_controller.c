#include "gate_controller.h"
#include "nvs_manager.h"
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "GATE_CTRL";

// Seeed Studio XIAO ESP32-C3 GPIO Mappings for 2x RDC1-2R Relay Modules (ULN2003 Driver)
// Module 1: Hörmann ProMatic (Garage)
#ifndef CONFIG_GARAGE_FULL_GPIO
#define CONFIG_GARAGE_FULL_GPIO    GPIO_NUM_2  // D0 -> Module 1 Relay 1 (Hörmann Terminals 21 & 20)
#endif

#ifndef CONFIG_GARAGE_VENT_GPIO
#define CONFIG_GARAGE_VENT_GPIO    GPIO_NUM_3  // D1 -> Module 1 Relay 2 (Hörmann Terminals 23 & 20)
#endif

// Module 2: Nice Street Gate Remote
#ifndef CONFIG_DRIVEWAY_OPEN_GPIO
#define CONFIG_DRIVEWAY_OPEN_GPIO  GPIO_NUM_4  // D2 -> Module 2 Relay 1 (Nice Open Button 1 Pads)
#endif

#ifndef CONFIG_DRIVEWAY_CLOSE_GPIO
#define CONFIG_DRIVEWAY_CLOSE_GPIO GPIO_NUM_5  // D3 -> Module 2 Relay 2 (Nice Close Button 2 Pads)
#endif

typedef struct {
    gpio_num_t gpio_num;
    uint32_t duration_ms;
    char desc[48];
} pulse_request_t;

static QueueHandle_t s_pulse_queue = NULL;
static SemaphoreHandle_t s_lock = NULL;
static gate_status_t s_status;
static int64_t s_last_pulse_end_time_us = 0;

/* Helper to configure RDC1-2R ULN2003 input pin (Active-HIGH, 0V idle) */
static void configure_relay_control_pin(gpio_num_t gpio) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(gpio, 0); // Initially deactivated (0V)
}

/* Background pulse execution task */
static void pulse_worker_task(void *pvParameters) {
    pulse_request_t req;

    while (1) {
        if (xQueueReceive(s_pulse_queue, &req, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Energizing relay: %s (GPIO %d, Duration %lu ms)",
                     req.desc, req.gpio_num, (unsigned long)req.duration_ms);

            // Active HIGH pulse: 3.3V logic turns on ULN2003 Darlington, energizing 5V coil
            gpio_set_level(req.gpio_num, 1);
            vTaskDelay(pdMS_TO_TICKS(req.duration_ms));
            gpio_set_level(req.gpio_num, 0);

            s_last_pulse_end_time_us = esp_timer_get_time();
            ESP_LOGI(TAG, "Relay released: %s", req.desc);
        }
    }
}

const char* gate_state_to_str(gate_state_t state) {
    switch (state) {
        case GATE_STATE_OPENING: return "OPENING";
        case GATE_STATE_OPEN:    return "OPEN";
        case GATE_STATE_VENTING: return "VENTING";
        case GATE_STATE_CLOSING: return "CLOSING";
        case GATE_STATE_CLOSED:  return "CLOSED";
        case GATE_STATE_STOPPED: return "STOPPED";
        default:                 return "UNKNOWN";
    }
}

esp_err_t gate_controller_init(void) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    // Default configuration values
    s_status.pulse_duration_ms = 400;
    s_status.interlock_delay_ms = 1500;
    s_status.garage_state = GATE_STATE_STOPPED;
    s_status.driveway_state = GATE_STATE_STOPPED;
    s_status.last_garage_action_ts = 0;
    s_status.last_driveway_action_ts = 0;
    strncpy(s_status.last_action_desc, "System Initialized", sizeof(s_status.last_action_desc) - 1);

    // Load overrides from NVS if present
    int32_t val = 0;
    if (nvs_manager_get_i32("pulse_ms", &val, 400) == ESP_OK && val >= 100 && val <= 2000) {
        s_status.pulse_duration_ms = (uint32_t)val;
    }
    if (nvs_manager_get_i32("interlock_ms", &val, 1500) == ESP_OK && val >= 500 && val <= 5000) {
        s_status.interlock_delay_ms = (uint32_t)val;
    }

    // Configure all 4 relay control pins
    configure_relay_control_pin(CONFIG_GARAGE_FULL_GPIO);
    configure_relay_control_pin(CONFIG_GARAGE_VENT_GPIO);
    configure_relay_control_pin(CONFIG_DRIVEWAY_OPEN_GPIO);
    configure_relay_control_pin(CONFIG_DRIVEWAY_CLOSE_GPIO);

    // Create Pulse Queue and Worker Task
    if (s_pulse_queue == NULL) {
        s_pulse_queue = xQueueCreate(8, sizeof(pulse_request_t));
        xTaskCreate(pulse_worker_task, "gate_pulse_task", 3072, NULL, 5, NULL);
    }

    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Dual 2-Channel Gate Controller Initialized (RDC1-2R / ULN2003):");
    ESP_LOGI(TAG, "  Hörmann Full Cycle:   GPIO %d (D0 -> Module 1 Relay 1)", CONFIG_GARAGE_FULL_GPIO);
    ESP_LOGI(TAG, "  Hörmann Ventilation:  GPIO %d (D1 -> Module 1 Relay 2)", CONFIG_GARAGE_VENT_GPIO);
    ESP_LOGI(TAG, "  Nice Open Button:     GPIO %d (D2 -> Module 2 Relay 1)", CONFIG_DRIVEWAY_OPEN_GPIO);
    ESP_LOGI(TAG, "  Nice Close Button:    GPIO %d (D3 -> Module 2 Relay 2)", CONFIG_DRIVEWAY_CLOSE_GPIO);
    ESP_LOGI(TAG, "  Pulse Duration:       %lu ms", (unsigned long)s_status.pulse_duration_ms);
    ESP_LOGI(TAG, "  Interlock Delay:      %lu ms", (unsigned long)s_status.interlock_delay_ms);

    return ESP_OK;
}

static bool is_interlock_active(void) {
    int64_t now = esp_timer_get_time();
    int64_t elapsed_ms = (now - s_last_pulse_end_time_us) / 1000;
    return (elapsed_ms < (int64_t)s_status.interlock_delay_ms);
}

esp_err_t gate_garage_trigger(garage_action_t action) {
    if (action != GARAGE_ACTION_FULL && action != GARAGE_ACTION_VENT) {
        ESP_LOGW(TAG, "Invalid garage action: %d", action);
        return ESP_ERR_INVALID_ARG;
    }

    if (is_interlock_active()) {
        ESP_LOGW(TAG, "Garage action rejected: Interlock active");
        return ESP_ERR_INVALID_STATE;
    }

    pulse_request_t req;
    req.duration_ms = s_status.pulse_duration_ms;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (action == GARAGE_ACTION_FULL) {
        req.gpio_num = CONFIG_GARAGE_FULL_GPIO;
        strncpy(req.desc, "Garage: Full Cycle (Term 21)", sizeof(req.desc) - 1);

        if (s_status.garage_state == GATE_STATE_OPENING || s_status.garage_state == GATE_STATE_CLOSING || s_status.garage_state == GATE_STATE_VENTING) {
            s_status.garage_state = GATE_STATE_STOPPED;
        } else if (s_status.garage_state == GATE_STATE_OPEN) {
            s_status.garage_state = GATE_STATE_CLOSING;
        } else {
            s_status.garage_state = GATE_STATE_OPENING;
        }
    } else { // GARAGE_ACTION_VENT
        req.gpio_num = CONFIG_GARAGE_VENT_GPIO;
        strncpy(req.desc, "Garage: Ventilation (Term 23)", sizeof(req.desc) - 1);

        if (s_status.garage_state == GATE_STATE_VENTING || s_status.garage_state == GATE_STATE_OPENING || s_status.garage_state == GATE_STATE_CLOSING) {
            s_status.garage_state = GATE_STATE_STOPPED;
        } else {
            s_status.garage_state = GATE_STATE_VENTING;
        }
    }

    s_status.last_garage_action_ts = esp_timer_get_time() / 1000;
    strncpy(s_status.last_action_desc, req.desc, sizeof(s_status.last_action_desc) - 1);
    xSemaphoreGive(s_lock);

    if (xQueueSend(s_pulse_queue, &req, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Pulse queue full, action dropped");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t gate_driveway_trigger(driveway_action_t action) {
    if (action != DRIVEWAY_ACTION_OPEN && action != DRIVEWAY_ACTION_CLOSE) {
        ESP_LOGW(TAG, "Invalid driveway action: %d", action);
        return ESP_ERR_INVALID_ARG;
    }

    if (is_interlock_active()) {
        ESP_LOGW(TAG, "Driveway action rejected: Interlock active");
        return ESP_ERR_INVALID_STATE;
    }

    pulse_request_t req;
    req.duration_ms = s_status.pulse_duration_ms;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (action == DRIVEWAY_ACTION_OPEN) {
        req.gpio_num = CONFIG_DRIVEWAY_OPEN_GPIO;
        strncpy(req.desc, "Driveway: Open (Button 1)", sizeof(req.desc) - 1);

        // Open button cycle: Open -> Stop -> Open -> Stop; if already fully open, do nothing
        if (s_status.driveway_state == GATE_STATE_OPENING) {
            s_status.driveway_state = GATE_STATE_STOPPED;
        } else if (s_status.driveway_state == GATE_STATE_OPEN) {
            // Already fully open - controller logic ignores or maintains open
            s_status.driveway_state = GATE_STATE_OPEN;
        } else {
            s_status.driveway_state = GATE_STATE_OPENING;
        }
    } else { // DRIVEWAY_ACTION_CLOSE
        req.gpio_num = CONFIG_DRIVEWAY_CLOSE_GPIO;
        strncpy(req.desc, "Driveway: Close (Button 2)", sizeof(req.desc) - 1);

        // Close button cycle: Close -> Stop -> Close -> Stop; if already fully closed, do nothing
        if (s_status.driveway_state == GATE_STATE_CLOSING) {
            s_status.driveway_state = GATE_STATE_STOPPED;
        } else if (s_status.driveway_state == GATE_STATE_CLOSED) {
            // Already fully closed - controller logic ignores or maintains closed
            s_status.driveway_state = GATE_STATE_CLOSED;
        } else {
            s_status.driveway_state = GATE_STATE_CLOSING;
        }
    }

    s_status.last_driveway_action_ts = esp_timer_get_time() / 1000;
    strncpy(s_status.last_action_desc, req.desc, sizeof(s_status.last_action_desc) - 1);
    xSemaphoreGive(s_lock);

    if (xQueueSend(s_pulse_queue, &req, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Pulse queue full, action dropped");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void gate_controller_get_status(gate_status_t *out_status) {
    if (!out_status) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out_status, &s_status, sizeof(gate_status_t));
    xSemaphoreGive(s_lock);
}

esp_err_t gate_controller_set_pulse_duration(uint32_t pulse_ms) {
    if (pulse_ms < 100 || pulse_ms > 2000) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.pulse_duration_ms = pulse_ms;
    nvs_manager_set_i32("pulse_ms", (int32_t)pulse_ms);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t gate_controller_set_interlock_delay(uint32_t interlock_ms) {
    if (interlock_ms < 500 || interlock_ms > 5000) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.interlock_delay_ms = interlock_ms;
    nvs_manager_set_i32("interlock_ms", (int32_t)interlock_ms);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
