import { defineConfig, Plugin } from 'vite';

function mockEsp32DevPlugin(): Plugin {
  return {
    name: 'mock-esp32-backend',
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        if (req.url === '/api/gate/garage' && req.method === 'POST') {
          res.setHeader('Content-Type', 'application/json');
          res.end(JSON.stringify({ status: 'ok', action: 'garage_triggered' }));
          return;
        }

        if (req.url === '/api/gate/driveway' && req.method === 'POST') {
          res.setHeader('Content-Type', 'application/json');
          res.end(JSON.stringify({ status: 'ok', action: 'driveway_triggered' }));
          return;
        }

        if (req.url === '/api/gate/status' && req.method === 'GET') {
          res.setHeader('Content-Type', 'application/json');
          res.end(
            JSON.stringify({
              garage_state: 0,
              driveway_state: 0,
              pulse_duration_ms: 400,
              interlock_delay_ms: 1500,
              last_garage_ts: 0,
              last_driveway_ts: 0,
              last_action: 'Ready',
            })
          );
          return;
        }

        if (req.url === '/api/system/info' && req.method === 'GET') {
          res.setHeader('Content-Type', 'application/json');
          res.end(
            JSON.stringify({
              app: 'VorotaBot-esp32',
              version: '1.0.0',
              chip: 'ESP32-C3',
              cores: 1,
              cpu_freq_mhz: 160,
              heap_free: 215400,
              uptime_s: Math.floor(process.uptime()),
              wifi: {
                mode: 'AP+STA',
                sta_connected: true,
                sta_ssid: 'Home_WiFi_5G',
                sta_ip: '192.168.1.106',
                sta_rssi: -62,
              },
              wireguard: {
                configured: true,
                enabled: true,
                connected: true,
                ip: '10.0.0.7',
                endpoint: 'wg.glebos.click:443',
              },
              route53: {
                configured: true,
                synced: true,
                fqdn: 'vorota.glebos.click',
              },
              gates: {
                garage_state: 0,
                driveway_state: 0,
                last_action: 'Ready',
              },
              ota_partition: 'ota_0',
              build_time: new Date().toLocaleTimeString(),
            })
          );
          return;
        }

        if (req.url === '/api/wireguard/toggle' && req.method === 'POST') {
          res.setHeader('Content-Type', 'application/json');
          res.end(JSON.stringify({ status: 'ok', vpn: 'toggled' }));
          return;
        }

        if (req.url === '/api/wireguard/config' && req.method === 'POST') {
          res.setHeader('Content-Type', 'application/json');
          res.end(JSON.stringify({ status: 'ok', message: 'WireGuard configuration updated' }));
          return;
        }

        if (req.url === '/api/route53/sync' && req.method === 'POST') {
          res.setHeader('Content-Type', 'application/json');
          res.end(JSON.stringify({ status: 'ok', message: 'Route 53 DNS sync initiated' }));
          return;
        }

        if (req.url === '/api/wifi/connect' && req.method === 'POST') {
          res.setHeader('Content-Type', 'application/json');
          res.end(JSON.stringify({ status: 'ok', message: 'Connecting to Wi-Fi...' }));
          return;
        }

        if (req.url === '/api/gate/settings' && req.method === 'POST') {
          res.setHeader('Content-Type', 'application/json');
          res.end(JSON.stringify({ status: 'ok', message: 'Gate timings updated' }));
          return;
        }

        if (req.url === '/api/system/restart' && req.method === 'POST') {
          res.setHeader('Content-Type', 'application/json');
          res.end(JSON.stringify({ status: 'ok', message: 'Rebooting ESP32...' }));
          return;
        }

        if (req.url === '/api/system/factory-reset' && req.method === 'POST') {
          res.setHeader('Content-Type', 'application/json');
          res.end(JSON.stringify({ status: 'ok', message: 'Factory reset complete.' }));
          return;
        }

        if (req.url === '/api/system/ota' && req.method === 'POST') {
          res.setHeader('Content-Type', 'application/json');
          res.end(JSON.stringify({ status: 'ok', message: 'OTA Flash successful!' }));
          return;
        }

        next();
      });
    },
  };
}

export default defineConfig({
  plugins: [mockEsp32DevPlugin()],
  build: {
    target: 'es2020',
    minify: 'esbuild',
    cssMinify: true,
    outDir: 'dist',
    assetsDir: 'assets',
    rollupOptions: {
      output: {
        entryFileNames: 'assets/[name]-[hash].js',
        chunkFileNames: 'assets/[name]-[hash].js',
        assetFileNames: 'assets/[name]-[hash].[ext]',
        manualChunks: undefined,
      },
    },
  },
});
