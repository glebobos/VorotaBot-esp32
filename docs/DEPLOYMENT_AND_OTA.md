# Deployment, Captive Portal & Web OTA Guide

This guide covers flashing procedures, captive portal behavior across mobile OS platforms, and performing browser-based Over-The-Air (OTA) firmware updates.

---

## 1. Initial USB Flashing

### Auto-Detection & Flashing
```bash
./run.sh flash-monitor
```
The script scans `/dev/ttyACM*` and `/dev/ttyUSB*` automatically.

### Bootloader Recovery Mode (If Flashing Times Out)
1. Hold down the **BOOT** button on your ESP32 board.
2. Press and release the **RESET / EN** button (or reconnect the USB cable).
3. Release the **BOOT** button.
4. Run `./run.sh flash` again.

---

## 2. Captive Portal Mechanism

### Default Credentials
- **Access Point SSID:** `VorotaBot-AP`
- **Password:** `12345678` (WPA2-PSK)
- **Portal IP:** `http://192.168.4.1/`
- **Portal Domain:** `http://vorota.local/`

### OS-Specific Captive Portal Behaviors
- **Apple iOS / iPadOS / macOS**: Automatically opens the Captive Network Assistant (CNA) browser upon Wi-Fi association.
- **Google Android**: Pops up a system notification stating *"Sign in to Wi-Fi network"*. Tapping opens the portal webview.
- **Microsoft Windows 10/11**: Opens default browser navigating to the portal homepage.

---

## 3. Over-The-Air (OTA) Firmware Updates via Web Browser

The device contains an integrated dual-bank A/B OTA updater:

```mermaid
flowchart LR
    DevHost[Host Computer] -->|1. ./run.sh build-ota| BinFile[dist/firmware-update.bin]
    BinFile -->|2. Drag & Drop into Browser /ota Tab| WebPortal[Web Browser Interface]
    WebPortal -->|3. POST /api/system/ota| ESP32OTA[ESP32 OTA Engine]
    ESP32OTA -->|4. Writes to Next Partition (ota_1)| FlashPart[Target Partition]
    FlashPart -->|5. Verify & Set Boot Flag| Reboot[Reboots into New Firmware]
```

### Steps to Perform an OTA Update:
1. Build the OTA binary package on your host machine:
   ```bash
   ./run.sh build-ota
   ```
   This generates `dist/firmware-update.bin`.

2. Open the device web portal (`http://192.168.4.1/`) on your phone or laptop.
3. Navigate to the **OTA Update** tab.
4. Drag and drop `dist/firmware-update.bin` into the upload zone (or click to browse).
5. Watch the live chunked upload progress bar.
6. Once verification passes, the ESP32 automatically reboots into the new firmware partition within 5 seconds!
