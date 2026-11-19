# Frontend Guide & Customization

The web portal is built using **Vite + TypeScript + Vanilla CSS** for instant load times, zero heavy runtime overhead, and a tiny memory footprint (<30KB total gzipped bundle) that easily fits into SPIFFS.

---

## 1. Local Development (Dockerized)

To build and package frontend assets into `firmware/spiffs_image/`:

```bash
./run.sh build-frontend
```

This runs the Vite production build inside the Docker container and automatically compresses all HTML, CSS, JS, and SVG assets with `gzip -9`.

---

## 2. Structure & Views

- **Main Dashboard (`100dvh` Mobile-First View)**:
  - **Garage Card (Hörmann ProMatic)**: Full Cycle and Ventilation (*Teilöffnung*) action buttons with live status badge.
  - **Driveway Card (Nice Remote)**: Dedicated Open and Close buttons with live status badge.
  - **Activity Banner**: Real-time feedback showing impulse status, safety delay timers, and network state.
- **Slide-Up Configuration Sheet (`...`)**:
  - **Network & VPN Tab**: WireGuard VPN switch, IP/endpoint display, `.conf` text editor, AWS Route 53 status/sync, and Wi-Fi connection manager.
  - **Gate Timings Tab**: Live range sliders for pulse duration (150–1200ms) and interlock safety delay (500–4000ms).
  - **System & OTA Tab**: Diagnostic metrics (free heap, uptime, RSSI, partition), system reboot, factory reset, and drag-and-drop OTA firmware updater.

---

## 3. Theming & Styling

All colors, glassmorphism borders, and font sizing are defined as CSS variables in `frontend/src/style.css`:

```css
:root {
  --bg-dark: #090d16;
  --bg-card: rgba(17, 24, 39, 0.8);
  --border-accent: rgba(56, 189, 248, 0.3);

  --accent-cyan: #38bdf8;
  --accent-green: #22c55e;
  --accent-amber: #f59e0b;
  --accent-red: #ef4444;
}
```
