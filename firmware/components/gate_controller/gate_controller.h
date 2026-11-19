#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Actions for Internal Garage Door (Hörmann ProMatic)
 * 2 Relays on Module 1:
 * - FULL: Terminal 21 (Cycle: Open -> Stop -> Close -> Stop)
 * - VENT: Terminal 23 (Teilöffnung: Open to partial -> Stop -> Close -> Stop)
 */
typedef enum {
    GARAGE_ACTION_FULL = 1,  /**< Terminal 21: Full cycle (Open -> Stop -> Close -> Stop) */
    GARAGE_ACTION_VENT = 2   /**< Terminal 23: Ventilation / Teilöffnung (Partial -> Stop -> Close -> Stop) */
} garage_action_t;

/**
 * @brief Actions for External Street Gate (Nice Controller / Remote)
 * 2 Relays on Module 2:
 * - OPEN:  Nice Open button (Cycle: Open -> Stop -> Open; no-op if fully open)
 * - CLOSE: Nice Close button (Cycle: Close -> Stop -> Close; no-op if fully closed)
 */
typedef enum {
    DRIVEWAY_ACTION_OPEN  = 1, /**< Button 1: Open-only cycle (Open -> Stop -> Open) */
    DRIVEWAY_ACTION_CLOSE = 2  /**< Button 2: Close-only cycle (Close -> Stop -> Close) */
} driveway_action_t;

/**
 * @brief Gate motion state
 */
typedef enum {
    GATE_STATE_UNKNOWN = 0,
    GATE_STATE_OPENING,
    GATE_STATE_OPEN,
    GATE_STATE_VENTING,
    GATE_STATE_CLOSING,
    GATE_STATE_CLOSED,
    GATE_STATE_STOPPED
} gate_state_t;

typedef struct {
    gate_state_t garage_state;
    gate_state_t driveway_state;
    uint32_t pulse_duration_ms;
    uint32_t interlock_delay_ms;
    uint64_t last_garage_action_ts;
    uint64_t last_driveway_action_ts;
    char last_action_desc[64];
} gate_status_t;

/**
 * @brief Convert gate state enum to human-readable string.
 */
const char* gate_state_to_str(gate_state_t state);

/**
 * @brief Initialize GPIOs for 2x 2-channel RDC1-2R relay modules with ULN2003.
 *
 * Configures:
 * - Module 1, Relay 1: GPIO 2 (D0) -> Hörmann Full Cycle (Terminals 21 & 20)
 * - Module 1, Relay 2: GPIO 3 (D1) -> Hörmann Ventilation (Terminals 23 & 20)
 * - Module 2, Relay 1: GPIO 4 (D2) -> Nice Open Button (Remote Pad 1)
 * - Module 2, Relay 2: GPIO 5 (D3) -> Nice Close Button (Remote Pad 2)
 *
 * All pins configured as Active-HIGH push-pull outputs (0V idle, 3.3V pulse to ULN2003).
 *
 * @return ESP_OK on success.
 */
esp_err_t gate_controller_init(void);

/**
 * @brief Trigger Hörmann ProMatic garage door action (Module 1).
 *
 * @param action GARAGE_ACTION_FULL or GARAGE_ACTION_VENT.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if interlock active.
 */
esp_err_t gate_garage_trigger(garage_action_t action);

/**
 * @brief Trigger Nice street gate action (Module 2).
 *
 * @param action DRIVEWAY_ACTION_OPEN or DRIVEWAY_ACTION_CLOSE.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if interlock active.
 */
esp_err_t gate_driveway_trigger(driveway_action_t action);

/**
 * @brief Get current gate status and telemetry.
 *
 * @param out_status Pointer to struct to receive telemetry.
 */
void gate_controller_get_status(gate_status_t *out_status);

/**
 * @brief Set pulse duration in milliseconds (default 400ms).
 */
esp_err_t gate_controller_set_pulse_duration(uint32_t pulse_ms);

/**
 * @brief Set safety interlock delay in milliseconds (default 1500ms).
 */
esp_err_t gate_controller_set_interlock_delay(uint32_t interlock_ms);

#ifdef __cplusplus
}
#endif
