#!/usr/bin/env bash

set -euo pipefail

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

log_info() {
  echo -e "${YELLOW}[INFO] $1${NC}"
}

log_success() {
  echo -e "${GREEN}[SUCCESS] $1${NC}"
}

log_error() {
  echo -e "${RED}[ERROR] $1${NC}" >&2
}

log_title() {
  echo -e "${CYAN}${BOLD}$1${NC}"
}

print_usage() {
  log_title "VorotaBot-ESP32 • Build, Flash & Provisioning Tool (100% Docker)"
  echo ""
  echo "Usage: ./run.sh <command> [options]"
  echo ""
  echo "Commands:"
  echo "  build-frontend     Build and compress Vite frontend into firmware/spiffs_image/"
  echo "  build-firmware     Compile ESP-IDF firmware & generate SPIFFS filesystem image"
  echo "  build              Run full pipeline (frontend build -> firmware compile)"
  echo "  provision          Generate & flash NVS credentials (WireGuard, AWS, Wi-Fi)"
  echo "  flash              Flash the compiled firmware and SPIFFS image to target device"
  echo "  monitor            Start interactive ESP-IDF serial monitor"
  echo "  flash-monitor      Flash firmware and immediately open serial monitor"
  echo "  all                Build frontend, compile firmware, flash, and open monitor"
  echo "  build-ota          Compile and prepare OTA binary into dist/firmware-update.bin"
  echo "  menuconfig         Open interactive ESP-IDF configuration menu"
  echo "  size               Analyze firmware memory usage and flash partition sizes"
  echo "  erase-flash        Erase entire flash memory of connected ESP32"
  echo "  clean              Remove build directories, SPIFFS assets, and docker caches"
  echo "  shell              Open an interactive bash shell inside ESP-IDF container"
  echo ""
  echo "Provisioning & Flashing Options:"
  echo "  --port PORT              Override serial port (default: auto-detected)"
  echo "  --baud RATE              Override flashing baud rate (default: 115200)"
  echo "  --wg-config PATH         Path to WireGuard client .conf file or config string"
  echo "  --aws-access-key KEY     AWS Access Key ID"
  echo "  --aws-secret-key SECRET  AWS Secret Access Key"
  echo "  --root-domain DOMAIN     AWS Route 53 root domain name (e.g. example.com)"
  echo "  --hosted-zone-id ID      AWS Route 53 Hosted Zone ID"
  echo "  --record-name FQDN       Host record name (defaults to vorota.<root-domain>)"
  echo "  --wifi-ssid SSID         Home Wi-Fi SSID"
  echo "  --wifi-pass PASS         Home Wi-Fi Password"
  echo "  --dry-run                Print Docker commands without executing them"
  echo "  -h, --help               Show this help message"
  echo ""
  echo "Examples:"
  echo "  # 1. Full build"
  echo "  ./run.sh build"
  echo ""
  echo "  # 2. Provision WireGuard & AWS Route 53 credentials via Docker"
  echo "  ./run.sh provision --wg-config ./my_wg.conf --aws-access-key AKIA... --aws-secret-key secret... --root-domain mygates.net"
  echo ""
  echo "  # 3. Flash firmware and monitor output"
  echo "  ./run.sh flash-monitor"
  echo ""
}

detect_port() {
  for port in /dev/ttyACM0 /dev/ttyACM1 /dev/ttyACM2 /dev/ttyUSB0 /dev/ttyUSB1 /dev/ttyUSB2; do
    if [ -e "$port" ]; then
      echo "$port"
      return 0
    fi
  done
  log_error "No USB serial device found in /dev/ttyACM* or /dev/ttyUSB*."
  log_error "Please connect your ESP32-C3 board or specify --port /dev/yourPort."
  exit 1
}

PORT=""
BAUD="115200"
DRY_RUN=false
WG_CONFIG=""
AWS_KEY=""
AWS_SECRET=""
ROOT_DOMAIN="glebos.click"
HOSTED_ZONE=""
RECORD_NAME="vorota.glebos.click"
WIFI_SSID=""
WIFI_PASS=""
COMMANDS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)
      PORT="$2"
      shift 2
      ;;
    --baud)
      BAUD="$2"
      shift 2
      ;;
    --wg-config)
      WG_CONFIG="$2"
      shift 2
      ;;
    --aws-access-key)
      AWS_KEY="$2"
      shift 2
      ;;
    --aws-secret-key)
      AWS_SECRET="$2"
      shift 2
      ;;
    --root-domain)
      ROOT_DOMAIN="$2"
      shift 2
      ;;
    --hosted-zone-id)
      HOSTED_ZONE="$2"
      shift 2
      ;;
    --record-name)
      RECORD_NAME="$2"
      shift 2
      ;;
    --wifi-ssid)
      WIFI_SSID="$2"
      shift 2
      ;;
    --wifi-pass)
      WIFI_PASS="$2"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=true
      shift
      ;;
    -h|--help)
      print_usage
      exit 0
      ;;
    -*)
      log_error "Unknown option: $1"
      print_usage
      exit 1
      ;;
    *)
      COMMANDS+=("$1")
      shift
      ;;
  esac
done

if [ ${#COMMANDS[@]} -eq 0 ]; then
  log_error "No command specified."
  print_usage
  exit 1
fi

CMD="${COMMANDS[0]}"

# Helper to run command (supporting dry-run)
run_cmd() {
  if [ "$DRY_RUN" = true ]; then
    echo -e "${YELLOW}[DRY-RUN]${NC} $*"
  else
    "$@"
  fi
}

needs_port() {
  case "$1" in
    flash|monitor|flash-monitor|all|erase-flash|provision) return 0 ;;
    *) return 1 ;;
  esac
}

release_port() {
  # Stop any lingering docker monitor containers that might be holding the serial port
  local active_containers
  active_containers=$(docker ps -q --filter "ancestor=vorotabot-idf" 2>/dev/null || true)
  if [ -n "$active_containers" ]; then
    log_info "Stopping lingering monitor container(s) holding serial port..."
    docker kill $active_containers >/dev/null 2>&1 || true
    sleep 0.5
  fi
}

if needs_port "$CMD"; then
  if [ -z "$PORT" ]; then
    PORT=$(detect_port)
    log_info "Auto-detected serial port: $PORT"
  else
    log_info "Using specified serial port: $PORT"
  fi
  case "$CMD" in
    flash|flash-monitor|all|erase-flash) release_port ;;
  esac
fi

# Provisioning generator helper
generate_provision_bin() {
  log_info "Generating NVS Partition binary inside Docker container..."
  mkdir -p dist

  GEN_ARGS=()
  if [ -n "$WG_CONFIG" ]; then
    if [ -f "$WG_CONFIG" ]; then
      if [ "$(realpath "$WG_CONFIG")" != "$(realpath dist/wg_provision.conf 2>/dev/null)" ]; then
        cp "$WG_CONFIG" dist/wg_provision.conf
      fi
      GEN_ARGS+=(--wg-config /project/dist/wg_provision.conf)
    else
      log_error "WireGuard config '$WG_CONFIG' does not exist!"
      exit 1
    fi
  elif [ -f "dist/wg_provision.conf" ]; then
    log_info "Reusing existing WireGuard configuration from dist/wg_provision.conf"
    GEN_ARGS+=(--wg-config /project/dist/wg_provision.conf)
  fi
  if [ -n "$AWS_KEY" ]; then GEN_ARGS+=(--aws-access-key "$AWS_KEY"); fi
  if [ -n "$AWS_SECRET" ]; then GEN_ARGS+=(--aws-secret-key "$AWS_SECRET"); fi
  if [ -n "$ROOT_DOMAIN" ]; then GEN_ARGS+=(--root-domain "$ROOT_DOMAIN"); fi
  if [ -n "$HOSTED_ZONE" ]; then GEN_ARGS+=(--hosted-zone-id "$HOSTED_ZONE"); fi
  if [ -n "$RECORD_NAME" ]; then GEN_ARGS+=(--record-name "$RECORD_NAME"); fi
  if [ -n "$WIFI_SSID" ]; then GEN_ARGS+=(--wifi-ssid "$WIFI_SSID"); fi
  if [ -n "$WIFI_PASS" ]; then GEN_ARGS+=(--wifi-pass "$WIFI_PASS"); fi

  run_cmd docker run --rm -v "$(pwd):/project" -w /project vorotabot-idf python3 /project/scripts/generate_nvs.py "${GEN_ARGS[@]}"
  log_success "NVS binary generated at dist/nvs_config.bin"
}

# Direct serial monitor helper (uses esp_idf_monitor directly without invoking cmake/ninja rebuild)
run_monitor() {
  log_info "Opening serial monitor on $PORT at $BAUD baud... (Press Ctrl+] to exit)"
  local elf_arg=""
  if [ -f "firmware/build/vorotabot_esp32.elf" ]; then
    elf_arg="/project/build/vorotabot_esp32.elf"
  fi

  if [ "$DRY_RUN" = true ]; then
    echo -e "${YELLOW}[DRY-RUN]${NC} docker run -it --rm --privileged --device $PORT -v $(pwd)/firmware:/project -w /project vorotabot-idf python3 -m esp_idf_monitor -p $PORT -b $BAUD --target esp32c3 $elf_arg"
  else
    docker run -it --rm --privileged --device "$PORT" -v "$(pwd)/firmware:/project" -w /project vorotabot-idf python3 -m esp_idf_monitor -p "$PORT" -b "$BAUD" --target esp32c3 $elf_arg
  fi
}

case "$CMD" in
  build-frontend)
    log_info "Building frontend assets with Vite..."
    run_cmd docker compose run --rm frontend-builder
    log_success "Frontend assets compiled and gzipped to firmware/spiffs_image/."
    ;;
    
  build-firmware)
    log_info "Compiling ESP-IDF firmware & SPIFFS binary..."
    run_cmd docker compose run --rm firmware-builder
    log_success "Firmware compilation completed successfully."
    ;;
    
  build)
    log_info "Starting full end-to-end project build..."
    run_cmd docker compose run --rm frontend-builder
    run_cmd docker compose run --rm firmware-builder
    log_success "Full project build completed successfully."
    ;;

  provision)
    generate_provision_bin
    log_info "Flashing NVS partition to ESP32 at offset 0x9000 on $PORT..."
    run_cmd docker run --rm --privileged --device "$PORT" -v "$(pwd):/project" -w /project vorotabot-idf esptool.py -p "$PORT" -b "$BAUD" write_flash 0x9000 dist/nvs_config.bin
    log_success "Provisioning data successfully flashed into NVS!"
    ;;
    
  flash)
    if [ ! -f "firmware/build/flash_args" ]; then
      log_info "Build artifacts not found. Building firmware..."
      run_cmd docker run --rm -v "$(pwd)/firmware:/project" -w /project vorotabot-idf idf.py build
    fi
    if [ -n "$WG_CONFIG" ] || [ -n "$AWS_KEY" ] || [ -n "$ROOT_DOMAIN" ] || [ -n "$WIFI_SSID" ]; then
      generate_provision_bin
    elif [ ! -f "dist/nvs_config.bin" ] && [ -f "dist/wg_provision.conf" ]; then
      generate_provision_bin
    fi
    log_info "Flashing firmware to device on $PORT at $BAUD baud..."
    run_cmd docker run --rm --privileged --device "$PORT" -v "$(pwd):/project" -w /project/firmware/build vorotabot-idf esptool.py --chip esp32c3 -p "$PORT" -b "$BAUD" --before default_reset --after hard_reset write_flash @flash_args
    if [ -f "dist/nvs_config.bin" ]; then
      log_info "Flashing provisioned NVS partition at 0x9000..."
      run_cmd docker run --rm --privileged --device "$PORT" -v "$(pwd):/project" -w /project vorotabot-idf esptool.py --chip esp32c3 -p "$PORT" -b "$BAUD" write_flash 0x9000 dist/nvs_config.bin
    fi
    log_success "Flashing completed successfully!"
    ;;
    
  monitor)
    run_monitor
    ;;
    
  flash-monitor)
    if [ ! -f "firmware/build/flash_args" ]; then
      log_info "Build artifacts not found. Building firmware..."
      run_cmd docker run --rm -v "$(pwd)/firmware:/project" -w /project vorotabot-idf idf.py build
    fi
    if [ -n "$WG_CONFIG" ] || [ -n "$AWS_KEY" ] || [ -n "$ROOT_DOMAIN" ] || [ -n "$WIFI_SSID" ]; then
      generate_provision_bin
    elif [ ! -f "dist/nvs_config.bin" ] && [ -f "dist/wg_provision.conf" ]; then
      generate_provision_bin
    fi
    log_info "Flashing firmware to device on $PORT at $BAUD baud..."
    run_cmd docker run --rm --privileged --device "$PORT" -v "$(pwd):/project" -w /project/firmware/build vorotabot-idf esptool.py --chip esp32c3 -p "$PORT" -b "$BAUD" --before default_reset --after hard_reset write_flash @flash_args
    if [ -f "dist/nvs_config.bin" ]; then
      log_info "Flashing provisioned NVS partition at 0x9000..."
      run_cmd docker run --rm --privileged --device "$PORT" -v "$(pwd):/project" -w /project vorotabot-idf esptool.py --chip esp32c3 -p "$PORT" -b "$BAUD" write_flash 0x9000 dist/nvs_config.bin
    fi
    run_monitor
    ;;
    
  all)
    log_info "Executing complete sequence: Build Frontend -> Build Firmware -> Flash -> Monitor..."
    run_cmd docker compose run --rm frontend-builder
    run_cmd docker compose run --rm firmware-builder
    if [ -n "$WG_CONFIG" ] || [ -n "$AWS_KEY" ] || [ -n "$ROOT_DOMAIN" ] || [ -n "$WIFI_SSID" ]; then
      generate_provision_bin
    fi
    run_cmd docker run --rm --privileged --device "$PORT" -v "$(pwd):/project" -w /project/firmware/build vorotabot-idf esptool.py --chip esp32c3 -p "$PORT" -b "$BAUD" --before default_reset --after hard_reset write_flash @flash_args
    if [ -f "dist/nvs_config.bin" ]; then
      log_info "Flashing provisioned NVS partition at 0x9000..."
      run_cmd docker run --rm --privileged --device "$PORT" -v "$(pwd):/project" -w /project vorotabot-idf esptool.py --chip esp32c3 -p "$PORT" -b "$BAUD" write_flash 0x9000 dist/nvs_config.bin
    fi
    run_monitor
    ;;

  build-ota)
    log_info "Building OTA-ready binary..."
    run_cmd docker compose up --build frontend-builder
    run_cmd docker compose up --build firmware-builder
    mkdir -p dist
    if [ -f "firmware/build/vorotabot_esp32.bin" ]; then
      cp firmware/build/vorotabot_esp32.bin dist/firmware-update.bin
      log_success "OTA binary generated: dist/firmware-update.bin"
      log_info "Upload this file via the Web Portal OTA flasher tab (http://192.168.4.1/)."
    else
      log_error "Could not find firmware/build/vorotabot_esp32.bin."
    fi
    ;;
    
  size)
    log_info "Analyzing firmware binary size & memory allocation..."
    run_cmd docker run --rm -v "$(pwd)/firmware:/project" -w /project vorotabot-idf idf.py size
    ;;

  erase-flash)
    log_info "Erasing entire flash chip on $PORT..."
    run_cmd docker run --rm --privileged --device "$PORT" -v "$(pwd)/firmware:/project" -w /project vorotabot-idf idf.py -p "$PORT" erase-flash
    log_success "Flash erased successfully."
    ;;

  clean)
    log_info "Cleaning compilation outputs..."
    run_cmd rm -rf firmware/build dist
    run_cmd find firmware/spiffs_image -type f ! -name ".gitkeep" -delete
    log_info "Removing Docker build caches..."
    run_cmd docker compose down -v --remove-orphans 2>/dev/null || true
    log_success "Clean completed successfully."
    ;;
    
  shell)
    log_info "Entering interactive ESP-IDF build environment..."
    DEVICE_ARG=""
    if [ -e "$PORT" ]; then
      DEVICE_ARG="--privileged --device $PORT"
    fi
    if [ "$DRY_RUN" = true ]; then
      echo -e "${YELLOW}[DRY-RUN]${NC} docker run -it --rm $DEVICE_ARG -v $(pwd)/firmware:/project -w /project vorotabot-idf bash"
    else
      docker run -it --rm $DEVICE_ARG -v "$(pwd)/firmware:/project" -w /project vorotabot-idf bash
    fi
    ;;
    
  menuconfig)
    log_info "Opening interactive ESP-IDF menuconfig..."
    if [ "$DRY_RUN" = true ]; then
      echo -e "${YELLOW}[DRY-RUN]${NC} docker run -it --rm -v $(pwd)/firmware:/project -w /project vorotabot-idf idf.py menuconfig"
    else
      docker run -it --rm -v "$(pwd)/firmware:/project" -w /project vorotabot-idf idf.py menuconfig
    fi
    ;;
    
  *)
    log_error "Unknown command: $CMD"
    print_usage
    exit 1
    ;;
esac
