#!/usr/bin/env bash
set -e

# Color definitions
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

echo -e "${CYAN}${BOLD}======================================================${RESET}"
echo -e "${CYAN}${BOLD}       VorotaBot-ESP32 Synthetic Test Suite          ${RESET}"
echo -e "${CYAN}${BOLD}======================================================${RESET}"

MBEDTLS_INC="/opt/esp/idf/components/mbedtls/mbedtls/include"
MBEDTLS_SRC="/opt/esp/idf/components/mbedtls/mbedtls/library/sha256.c /opt/esp/idf/components/mbedtls/mbedtls/library/platform_util.c"
TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_DIR="/tmp/vorotabot_tests_bin"
mkdir -p "$BIN_DIR"

TOTAL_SUITES=0
PASSED_SUITES=0
FAILED_SUITES=0

run_suite() {
    local name="$1"
    local src="$2"
    local extra_args="${3:-}"
    TOTAL_SUITES=$((TOTAL_SUITES + 1))

    echo ""
    echo -e "${YELLOW}Compiling test suite: ${name}...${RESET}"
    if ! gcc -I"$TESTS_DIR" $extra_args "$src" -o "$BIN_DIR/$name"; then
        echo -e "${RED}[COMPILATION ERROR] Failed to compile ${name}${RESET}"
        FAILED_SUITES=$((FAILED_SUITES + 1))
        return 1
    fi

    if "$BIN_DIR/$name"; then
        echo -e "${GREEN}[SUITE PASSED] ${name}${RESET}"
        PASSED_SUITES=$((PASSED_SUITES + 1))
    else
        echo -e "${RED}[SUITE FAILED] ${name}${RESET}"
        FAILED_SUITES=$((FAILED_SUITES + 1))
        return 1
    fi
}

# 1. Authentication Logic & Password Hashing
run_suite "test_auth_logic" "$TESTS_DIR/test_auth_logic.c" "-I$MBEDTLS_INC $MBEDTLS_SRC"

# 2. WireGuard Key Validation & Trust Decision Matrix
run_suite "test_wireguard_rules" "$TESTS_DIR/test_wireguard_rules.c"

# 3. Wi-Fi Manager FSM & SoftAP Anti-Lockout Invariants
run_suite "test_wifi_logic" "$TESTS_DIR/test_wifi_logic.c"

# 4. Gate Controller Pulse Interlocks & Hörmann/Nice FSM
run_suite "test_gate_logic" "$TESTS_DIR/test_gate_logic.c"

echo ""
echo -e "${CYAN}${BOLD}======================================================${RESET}"
echo -e "${CYAN}${BOLD}                   TEST SUITE SUMMARY                 ${RESET}"
echo -e "${CYAN}${BOLD}======================================================${RESET}"
echo -e "Total Suites Run : ${BOLD}${TOTAL_SUITES}${RESET}"
echo -e "Suites Passed    : ${GREEN}${BOLD}${PASSED_SUITES}${RESET}"
echo -e "Suites Failed    : ${RED}${BOLD}${FAILED_SUITES}${RESET}"

if [ "$FAILED_SUITES" -eq 0 ]; then
    echo -e "\n${GREEN}${BOLD}>>> ALL SYNTHETIC TESTS PASSED SUCCESSFULLY! <<<${RESET}\n"
    exit 0
else
    echo -e "\n${RED}${BOLD}>>> SOME TESTS FAILED! <<<${RESET}\n"
    exit 1
fi
