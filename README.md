# VorotaBot-ESP32

A production-grade dual gate controller firmware for **Seeed Studio XIAO ESP32-C3**, designed to operate an **internal garage door (Hörmann ProMatic 3)** and an **external street/driveway gate (Nice remote)**.

Built on the `esp32-template` architecture — featuring **100% Docker-based builds with zero host dependencies**, Bluetooth (NimBLE) provisioning, WireGuard VPN client, AWS Route 53 dynamic DNS registration, and a local cyber-industrial web portal.

---

## ⚡ Quick Start (Docker Only)

Zero host dependencies required — only Docker & Docker Compose:

```bash
# 1. Build frontend & compile ESP-IDF firmware
./run.sh build

# 2. Provision WireGuard & AWS Route 53 credentials (optional: pre-flash into NVS)
./run.sh provision \
  --wg-config ./my_wg0.conf \
  --aws-access-key AKIAIOSFODNN7EXAMPLE \
  --aws-secret-key wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY \
  --root-domain mygates.net \
  --hosted-zone-id Z1234567890ABCDEF

# 3. Flash to connected ESP32-C3 and open serial monitor
./run.sh flash-monitor
```

Or run everything in one step:
```bash
./run.sh all --wg-config ./my_wg0.conf --root-domain mygates.net
```

---

## 🔌 Hardware Wiring & Pinout

### Controller: Seeed Studio XIAO ESP32-C3
Exposed GPIOs used for gate relay triggering:

| Gate | Function | ESP32-C3 Pin | Logic / Mode | Target Device & Terminals |
| :--- | :--- | :--- | :--- | :--- |
| **Hörmann** | Full Cycle (*Полное*) | **D0 (GPIO 2)** | Active-HIGH (3.3V) | Module 1, Input S1 $\to$ Relay 1 (Terminals 21 & 20) |
| **Hörmann** | Ventilation (*Проветривание*) | **D1 (GPIO 3)** | Active-HIGH (3.3V) | Module 1, Input S2 $\to$ Relay 2 (Terminals 23 & 20) |
| **Nice** | Open Only (*Открыть*) | **D2 (GPIO 4)** | Active-HIGH (3.3V) | Module 2, Input S1 $\to$ Relay 1 (Button 1 Pads) |
| **Nice** | Close Only (*Закрыть*) | **D3 (GPIO 5)** | Active-HIGH (3.3V) | Module 2, Input S2 $\to$ Relay 2 (Button 2 Pads) |
| **Power** | 24V $\to$ 5V Buck | **5V / GND** | +5V DC Supply Rail | Hörmann Terminals 5 (+24V) & 20 (0V) $\to$ DC-DC $\to$ ESP32 & Relays |

---

### 1. Relay Modules: 2x RDC1-2R (ULN2003 Darlington Driver + 5V Relays)
* **Chip**: ULN2003 Darlington transistor array driving 5V Songle relays (SRD-05VDC-SL-C).
* **Signal Voltage**: 3.3V – 5V logic. Active-HIGH (a HIGH pulse turns on the Darlington pair, energizing the relay coil and connecting COM to NO).
* **Galvanic Isolation**: Potential-free "dry contacts" (COM & NO) isolate the ESP32 logic from both Hörmann 24V control lines and the Nice remote battery circuit.

---

### 2. Internal Garage Door — Hörmann ProMatic (4 Contacts + DC-DC Converter)
Hörmann ProMatic 3/4 terminal block connections:
* **Terminal 5 (+24V DC):** Connected to DC-DC step-down converter input (+).
* **Terminal 20 (0V / Masse):** Connected to DC-DC converter input (-), Module 1 Relay 1 COM, and Module 1 Relay 2 COM.
* **Terminal 21 (Full Cycle Impulse):** Connected to Module 1 Relay 1 NO.
  * **Behavior**: Open $\rightarrow$ Stop $\rightarrow$ Close $\rightarrow$ Stop.
* **Terminal 23 (Ventilation / *Teilöffnung*):** Connected to Module 1 Relay 2 NO.
  * **Behavior**: Open partially $\rightarrow$ Stop $\rightarrow$ Close $\rightarrow$ Stop.
* **DC-DC Step-Down Converter (24V $\rightarrow$ 5V):**
  * Input: Hörmann Terminal 5 (+24V) and Terminal 20 (0V).
  * Output: +5V and GND. Powers Seeed XIAO ESP32-C3 (5V/GND pins) and both RDC1-2R relay modules (V & G terminals).

---

### 3. External Driveway Gate — Nice Remote Control (2 Dedicated Buttons)
The second RDC1-2R module connects directly to the button pads of the Nice remote control:
* **Relay 1 (Open Only):** COM and NO connect across the Nice Button 1 pads.
  * **Behavior**: Open $\rightarrow$ Stop $\rightarrow$ Open $\rightarrow$ Stop. If fully opened, pressing does nothing.
* **Relay 2 (Close Only):** COM and NO connect across the Nice Button 2 pads.
  * **Behavior**: Close $\rightarrow$ Stop $\rightarrow$ Close $\rightarrow$ Stop. If fully closed, pressing does nothing.

---

### 4. Complete Wiring Schematic
```
   Hörmann ProMatic                                      Seeed Studio XIAO ESP32-C3
 ┌──────────────────┐                                   ┌───────────────────────────┐
 │ Term 5 (+24V) ───┼──────────┐                        │                           │
 │                  │          ▼                        │                           │
 │ Term 20 (0V) ────┼───┬─►┌───────────────┐            │                           │
 │                  │   │  │ DC-DC Buck    │            │                           │
 │ Term 21 (Full) ──┼─┐ │  │ 24V -> 5V     ├───+5V ────►│ 5V                        │
 │                  │ │ │  │               ├───GND ────►│ GND                       │
 │ Term 23 (Vent) ──┼─┼─┼─►└───────────────┘            │                           │
 └──────────────────┘ │ │                               │                           │
                      │ │  Module 1 (RDC1-2R Hörmann)   │                           │
                      │ │ ┌─────────────────────────┐   │                           │
                      │ └►│ COM 1 & COM 2 (0V)      │   │                           │
                      └──►│ NO 1 (Term 21: Full)    │   │                           │
                          │ NO 2 (Term 23: Vent)    │   │                           │
                          │ S1 (Full Trigger) ◄─────┼───┼─ GPIO 2 (D0)              │
                          │ S2 (Vent Trigger) ◄─────┼───┼─ GPIO 3 (D1)              │
                          │ V (+5V) & G (GND)       │   │                           │
                          └─────────────────────────┘   │                           │
                                                        │                           │
   Nice Remote Controller  Module 2 (RDC1-2R Nice)      │                           │
 ┌──────────────────┐     ┌─────────────────────────┐   │                           │
 │ Button 1 (Open)  │◄───►│ NO 1 & COM 1 (Dry)      │   │                           │
 │ Button 2 (Close) │◄───►│ NO 2 & COM 2 (Dry)      │   │                           │
 └──────────────────┘     │ S1 (Open Trigger) ◄─────┼───┼─ GPIO 4 (D2)              │
                          │ S2 (Close Trigger) ◄────┼───┼─ GPIO 5 (D3)              │
                          │ V (+5V) & G (GND)       │   │                           │
                          └─────────────────────────┘   └───────────────────────────┘
```

## 🌐 Connectivity & Networking

### 1. Wi-Fi & Captive Portal
* Connects to home Wi-Fi in Station (STA) mode.
* Falls back to SoftAP captive portal (`VorotaBot-AP`, password: `12345678`) if disconnected.
* Local portal accessible at `http://192.168.4.1/` or `http://vorota.local/`.

### 2. Bluetooth (NimBLE) Provisioning & VPN Remote Toggle
* Advertises as **VorotaBot** with custom 128-bit GATT service:
  * **Wi-Fi SSID & Password Characteristics:** Configure home Wi-Fi credentials over Bluetooth.
  * **WireGuard Toggle Characteristic:** Remotely enable or disable the VPN tunnel over Bluetooth without needing local network access.
  * **Gate Action Characteristic:** Trigger gate actions directly over Bluetooth.
* **Web Bluetooth API:** Pair and configure directly from your smartphone's Chrome browser without downloading any third-party app!

### 3. WireGuard VPN Client
* Embedded client connects directly to your AWS WireGuard server (e.g. BelKeeper VPN).
* Time is automatically synchronized via SNTP before the handshake.
* Assigned IP (`10.0.0.X`) allows secure remote control from anywhere over the VPN.

### 4. AWS Route 53 Dynamic DNS
* Direct SigV4 authenticated HTTPS client running on the ESP32.
* Automatically registers/updates an `A` record (`vorota.<root_domain>` $\rightarrow$ ESP32 WireGuard IP).

---

## 🛠️ CLI Reference (`./run.sh`)

| Command | Description |
| :--- | :--- |
| `./run.sh build-frontend` | Builds Vite TypeScript app & gzips to `firmware/spiffs_image/` |
| `./run.sh build-firmware` | Compiles ESP-IDF C firmware & generates SPIFFS image |
| `./run.sh build` | Full pipeline build (Frontend $\rightarrow$ Firmware) |
| `./run.sh provision [flags]` | Generates NVS partition binary & flashes credentials to ESP32 |
| `./run.sh flash` | Flashes firmware and SPIFFS filesystem to ESP32 |
| `./run.sh monitor` | Opens interactive serial monitor (`Ctrl+]` to exit) |
| `./run.sh flash-monitor` | Flashes device and immediately launches serial monitor |
| `./run.sh all [flags]` | Complete build, flash, provision, and monitor sequence |
| `./run.sh build-ota` | Compiles OTA update binary into `dist/firmware-update.bin` |
| `./run.sh size` | Analyzes RAM heap and flash partition usage |
| `./run.sh erase-flash` | Erases full flash memory of connected ESP32 |
| `./run.sh clean` | Removes build folders, generated images, and Docker caches |

### Required / Supported Flags
* `--wg-config <path|str>`: WireGuard client configuration file or string.
* `--aws-access-key <key>`: AWS Access Key ID for Route 53.
* `--aws-secret-key <sec>`: AWS Secret Access Key.
* `--root-domain <domain>`: Root hosted domain (e.g. `mygates.net`).
* `--hosted-zone-id <id>`: AWS Hosted Zone ID.
* `--record-name <fqdn>`: Custom record hostname (default: `vorota.<root-domain>`).
* `--wifi-ssid <ssid>`: Home Wi-Fi SSID.
* `--wifi-pass <pass>`: Home Wi-Fi Password.

---

## 📡 REST API Reference

| Endpoint | Method | Payload | Description |
| :--- | :--- | :--- | :--- |
| `/api/gate/garage` | `POST` | `{"action":"full"}` or `{"action":"vent"}` | Trigger Hörmann Full Cycle or Ventilation mode |
| `/api/gate/driveway` | `POST` | `{"action":"open"}` or `{"action":"close"}` | Trigger Nice Open or Close action |
| `/api/gate/status` | `GET` | — | Get gate states, timings, and telemetry |
| `/api/gate/settings` | `POST` | `{"pulse_ms":400,"interlock_ms":1500}` | Update pulse durations and interlock delay |
| `/api/wireguard/toggle` | `POST` | `{"enabled":true\|false}` | Toggle WireGuard tunnel |
| `/api/wireguard/config` | `POST` | Text (`.conf` format) | Update WireGuard credentials |
| `/api/route53/sync` | `POST` | — | Force immediate Route 53 DNS update |
| `/api/wifi/connect` | `POST` | `{"ssid":"...","password":"..."}` | Connect to Wi-Fi network |
| `/api/system/info` | `GET` | — | Complete system diagnostics JSON |
| `/api/system/restart` | `POST` | — | Reboot ESP32 |
| `/api/system/factory-reset` | `POST` | — | Reset all NVS settings to factory defaults |
| `/api/system/ota` | `POST` | Binary stream | OTA firmware flash |

---

## 📁 Repository Structure

```
VorotaBot-esp32/
├── docker/
│   ├── Dockerfile.idf             # ESP-IDF v5.4 build image
│   └── Dockerfile.frontend        # Node 20 frontend + gzip packer
├── docker-compose.yml             # Docker service definitions
├── run.sh                         # Primary CLI tool (Dockerized)
├── scripts/
│   └── generate_nvs.py            # Dockerized NVS partition generator
├── firmware/
│   ├── partitions.csv             # 4MB Dual-OTA (1.4MB each) + 1.15MB SPIFFS
│   ├── sdkconfig.defaults         # NimBLE, mbedTLS, LwIP defaults
│   ├── CMakeLists.txt
│   ├── components/
│   │   ├── gate_controller/       # Dual RDC1-2R relay modules (ULN2003)
│   │   ├── ble_service/           # NimBLE GATT Wi-Fi provisioning & VPN toggle
│   │   ├── wireguard_manager/     # WireGuard client & SNTP time sync
│   │   ├── aws_route53/           # SigV4 Route 53 dynamic DNS updater
│   │   ├── wifi_manager/          # SoftAP + STA Wi-Fi manager
│   │   ├── nvs_manager/           # Persistent key-value storage
│   │   ├── web_server/            # REST API & SPIFFS server
│   │   ├── dns_server/            # Captive portal DNS redirect
│   │   └── ota_manager/           # Web OTA flasher engine
│   └── main/
│       ├── app_config.h           # System defaults
│       └── main.c                 # System orchestration entry point
└── frontend/
    ├── package.json
    ├── vite.config.ts
    ├── index.html                 # Mobile-first cyber-industrial dashboard
    └── src/
        ├── style.css              # Glassmorphism design system & gate animations
        └── main.ts                # REST API client, gate triggers, live telemetry
```

---

## 📄 License
MIT License.
