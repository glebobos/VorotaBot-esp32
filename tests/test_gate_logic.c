#include "test_runner.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define ESP_OK                  0
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_ARG     0x102

typedef enum {
    GATE_STATE_UNKNOWN = 0,
    GATE_STATE_OPENING,
    GATE_STATE_OPEN,
    GATE_STATE_VENTING,
    GATE_STATE_CLOSING,
    GATE_STATE_CLOSED,
    GATE_STATE_STOPPED,
} gate_state_t;

typedef enum {
    GARAGE_ACTION_FULL = 1,
    GARAGE_ACTION_VENT = 2,
} garage_action_t;

typedef enum {
    DRIVEWAY_ACTION_OPEN  = 1,
    DRIVEWAY_ACTION_CLOSE = 2,
} driveway_action_t;

/* -------------------------------------------------------------------------
   Gate Controller Model Under Test
   ------------------------------------------------------------------------- */

typedef struct {
    uint32_t pulse_duration_ms;
    uint32_t interlock_delay_ms;
    gate_state_t garage_state;
    gate_state_t driveway_state;
    char last_action_desc[64];
} sim_gate_status_t;

static sim_gate_status_t s_status;
static volatile bool s_is_pulsing = false;
static int64_t s_simulated_time_ms = 10000;
static int64_t s_last_pulse_end_time_ms = 0;

static void sim_gate_init(void) {
    memset(&s_status, 0, sizeof(s_status));
    s_status.pulse_duration_ms = 400;
    s_status.interlock_delay_ms = 1500;
    s_status.garage_state = GATE_STATE_STOPPED;
    s_status.driveway_state = GATE_STATE_STOPPED;
    s_is_pulsing = false;
    s_simulated_time_ms = 10000;
    s_last_pulse_end_time_ms = 0;
}

static bool sim_is_interlock_active(void) {
    if (s_is_pulsing) {
        return true;
    }
    int64_t elapsed_ms = s_simulated_time_ms - s_last_pulse_end_time_ms;
    return (elapsed_ms < (int64_t)s_status.interlock_delay_ms);
}

static int sim_gate_garage_trigger(garage_action_t action) {
    if (action != GARAGE_ACTION_FULL && action != GARAGE_ACTION_VENT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sim_is_interlock_active()) {
        return ESP_ERR_INVALID_STATE;
    }

    // Begin pulse
    s_is_pulsing = true;

    if (action == GARAGE_ACTION_FULL) {
        snprintf(s_status.last_action_desc, sizeof(s_status.last_action_desc), "Garage: Full Cycle");
        if (s_status.garage_state == GATE_STATE_OPENING ||
            s_status.garage_state == GATE_STATE_CLOSING ||
            s_status.garage_state == GATE_STATE_VENTING) {
            s_status.garage_state = GATE_STATE_STOPPED;
        } else if (s_status.garage_state == GATE_STATE_OPEN) {
            s_status.garage_state = GATE_STATE_CLOSING;
        } else {
            s_status.garage_state = GATE_STATE_OPENING;
        }
    } else { // GARAGE_ACTION_VENT
        snprintf(s_status.last_action_desc, sizeof(s_status.last_action_desc), "Garage: Ventilation");
        if (s_status.garage_state == GATE_STATE_VENTING) {
            s_status.garage_state = GATE_STATE_STOPPED;
        } else {
            s_status.garage_state = GATE_STATE_VENTING;
        }
    }

    return ESP_OK;
}

static void sim_complete_pulse(void) {
    s_simulated_time_ms += s_status.pulse_duration_ms;
    s_last_pulse_end_time_ms = s_simulated_time_ms;
    s_is_pulsing = false;
}

static int sim_gate_driveway_trigger(driveway_action_t action) {
    if (action != DRIVEWAY_ACTION_OPEN && action != DRIVEWAY_ACTION_CLOSE) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sim_is_interlock_active()) {
        return ESP_ERR_INVALID_STATE;
    }

    s_is_pulsing = true;

    if (action == DRIVEWAY_ACTION_OPEN) {
        snprintf(s_status.last_action_desc, sizeof(s_status.last_action_desc), "Driveway: Open");
        s_status.driveway_state = GATE_STATE_OPENING;
    } else {
        snprintf(s_status.last_action_desc, sizeof(s_status.last_action_desc), "Driveway: Close");
        s_status.driveway_state = GATE_STATE_CLOSING;
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------
   Test Suites
   ------------------------------------------------------------------------- */

static void test_suite_active_pulse_interlock(void) {
    TEST_SUITE("Gate Controller: Active Pulse Mutual Exclusion & Interlock");

    sim_gate_init();

    // Initial state: Idle, ready to pulse
    ASSERT_FALSE(sim_is_interlock_active());

    // First pulse triggered
    int res = sim_gate_garage_trigger(GARAGE_ACTION_FULL);
    ASSERT_EQ(res, ESP_OK);
    ASSERT_TRUE(s_is_pulsing);
    ASSERT_TRUE(sim_is_interlock_active());

    // Concurrent trigger while pulse is in progress MUST be rejected
    res = sim_gate_garage_trigger(GARAGE_ACTION_FULL);
    ASSERT_EQ(res, ESP_ERR_INVALID_STATE);

    res = sim_gate_driveway_trigger(DRIVEWAY_ACTION_OPEN);
    ASSERT_EQ(res, ESP_ERR_INVALID_STATE);

    // Pulse completes
    sim_complete_pulse();
    ASSERT_FALSE(s_is_pulsing);

    // Right after pulse finishes (0ms elapsed < 1500ms interlock): must still be rejected
    ASSERT_TRUE(sim_is_interlock_active());
    res = sim_gate_garage_trigger(GARAGE_ACTION_FULL);
    ASSERT_EQ(res, ESP_ERR_INVALID_STATE);

    // Advance 1400ms (total 1400ms < 1500ms): still rejected
    s_simulated_time_ms += 1400;
    ASSERT_TRUE(sim_is_interlock_active());
    res = sim_gate_garage_trigger(GARAGE_ACTION_FULL);
    ASSERT_EQ(res, ESP_ERR_INVALID_STATE);

    // Advance another 101ms (total 1501ms > 1500ms): interlock releases!
    s_simulated_time_ms += 101;
    ASSERT_FALSE(sim_is_interlock_active());

    // Now trigger succeeds
    res = sim_gate_garage_trigger(GARAGE_ACTION_FULL);
    ASSERT_EQ(res, ESP_OK);
    sim_complete_pulse();
}

static void test_suite_hormann_fsm(void) {
    TEST_SUITE("Gate Controller: Hörmann Full Cycle & Vent FSM");

    sim_gate_init();
    s_status.interlock_delay_ms = 0; // Disable interlock delay to isolate state transitions

    // 1. From STOPPED -> Full trigger -> OPENING
    ASSERT_EQ(s_status.garage_state, GATE_STATE_STOPPED);
    sim_gate_garage_trigger(GARAGE_ACTION_FULL);
    ASSERT_EQ(s_status.garage_state, GATE_STATE_OPENING);
    sim_complete_pulse();

    // 2. From OPENING -> Full trigger (mid-travel toggle) -> STOPPED
    sim_gate_garage_trigger(GARAGE_ACTION_FULL);
    ASSERT_EQ(s_status.garage_state, GATE_STATE_STOPPED);
    sim_complete_pulse();

    // 3. From STOPPED -> Full trigger -> OPENING
    sim_gate_garage_trigger(GARAGE_ACTION_FULL);
    ASSERT_EQ(s_status.garage_state, GATE_STATE_OPENING);
    sim_complete_pulse();

    // 4. Simulate gate reaches fully open: OPEN -> Full trigger -> CLOSING
    s_status.garage_state = GATE_STATE_OPEN;
    sim_gate_garage_trigger(GARAGE_ACTION_FULL);
    ASSERT_EQ(s_status.garage_state, GATE_STATE_CLOSING);
    sim_complete_pulse();

    // 5. From CLOSING -> Full trigger (safety halt) -> STOPPED
    sim_gate_garage_trigger(GARAGE_ACTION_FULL);
    ASSERT_EQ(s_status.garage_state, GATE_STATE_STOPPED);
    sim_complete_pulse();

    // 6. Ventilation trigger from STOPPED -> VENTING
    sim_gate_garage_trigger(GARAGE_ACTION_VENT);
    ASSERT_EQ(s_status.garage_state, GATE_STATE_VENTING);
    sim_complete_pulse();

    // 7. Ventilation trigger from VENTING -> STOPPED
    sim_gate_garage_trigger(GARAGE_ACTION_VENT);
    ASSERT_EQ(s_status.garage_state, GATE_STATE_STOPPED);
    sim_complete_pulse();
}

static void test_suite_driveway_fsm(void) {
    TEST_SUITE("Gate Controller: Nice Driveway Remote FSM");

    sim_gate_init();
    s_status.interlock_delay_ms = 0;

    // Open button
    sim_gate_driveway_trigger(DRIVEWAY_ACTION_OPEN);
    ASSERT_EQ(s_status.driveway_state, GATE_STATE_OPENING);
    sim_complete_pulse();

    // Close button
    sim_gate_driveway_trigger(DRIVEWAY_ACTION_CLOSE);
    ASSERT_EQ(s_status.driveway_state, GATE_STATE_CLOSING);
    sim_complete_pulse();

    // Invalid action rejected
    int err = sim_gate_driveway_trigger((driveway_action_t)99);
    ASSERT_EQ(err, ESP_ERR_INVALID_ARG);
}

int main(void) {
    printf(ANSI_BOLD ">>> RUNNING SUITE: test_gate_logic <<<" ANSI_RESET "\n");

    test_suite_active_pulse_interlock();
    test_suite_hormann_fsm();
    test_suite_driveway_fsm();

    PRINT_TEST_SUMMARY();
    return (g_tests_failed == 0) ? 0 : 1;
}
