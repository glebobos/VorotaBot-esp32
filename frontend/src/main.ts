// VorotaBot-ESP32 Frontend Controller
// Cyber-Industrial Dual Gate Controller & Configuration Sheet (REST HTTP)

interface SystemInfo {
  app: string;
  version: string;
  chip: string;
  cores: number;
  cpu_freq_mhz: number;
  heap_free: number;
  uptime_s: number;
  wifi: {
    mode: string;
    sta_connected: boolean;
    sta_ssid: string;
    sta_ip: string;
    sta_rssi: number;
  };
  wireguard: {
    configured: boolean;
    enabled: boolean;
    connected: boolean;
    ip: string;
    endpoint: string;
  };
  route53: {
    configured: boolean;
    synced: boolean;
    fqdn: string;
  };
  gates: {
    garage_state: number;
    driveway_state: number;
    last_action: string;
  };
  ota_partition?: string;
}

interface GateStatus {
  garage_state: number;
  garage_state_str: string;
  driveway_state: number;
  driveway_state_str: string;
  pulse_duration_ms: number;
  interlock_delay_ms: number;
  last_garage_ts: number;
  last_driveway_ts: number;
  last_action: string;
}

const GATE_STATE_NAMES: Record<number, string> = {
  0: 'UNKNOWN',
  1: 'OPENING',
  2: 'OPEN',
  3: 'VENTING',
  4: 'CLOSING',
  5: 'CLOSED',
  6: 'STOPPED'
};

class VorotaBotApp {
  private sysPollTimer: number | null = null;
  private gatePollTimer: number | null = null;
  private busy: Record<string, boolean> = {};

  constructor() {
    this.initGateActions();
    this.initModalControls();
    this.initSettingsTabs();
    this.initSettingsActions();
    this.initOtaFlasher();
    this.fetchSystemInfo();
    this.fetchGateStatus();

    // Regular polling for gate status with background visibility pause
    this.startGatePolling();
    document.addEventListener('visibilitychange', () => {
      if (document.hidden) {
        this.stopGatePolling();
      } else {
        this.fetchGateStatus();
        this.startGatePolling();
      }
    });
  }

  private startGatePolling(): void {
    if (!this.gatePollTimer) {
      this.gatePollTimer = window.setInterval(() => this.fetchGateStatus(), 3000);
    }
  }

  private stopGatePolling(): void {
    if (this.gatePollTimer) {
      clearInterval(this.gatePollTimer);
      this.gatePollTimer = null;
    }
  }

  private activityTimer: number | null = null;

  /* -------------------------------------------------------------
   * 4 Dedicated Gate Actions (Garage Toggle/Vent, Gate Open/Close)
   * ------------------------------------------------------------- */
  private initGateActions(): void {
    // Garage: Toggle
    document.getElementById('btn-garage-full')?.addEventListener('click', () => {
      this.triggerGateAction(
        'garage',
        'full',
        'btn-garage-full',
        'feedback-garage-full',
        '',
        'Garage'
      );
    });

    // Garage: Vent
    document.getElementById('btn-garage-vent')?.addEventListener('click', () => {
      this.triggerGateAction(
        'garage',
        'vent',
        'btn-garage-vent',
        'feedback-garage-vent',
        '',
        'Vent'
      );
    });

    // Gate: Open
    document.getElementById('btn-driveway-open')?.addEventListener('click', () => {
      this.triggerGateAction(
        'driveway',
        'open',
        'btn-driveway-open',
        'feedback-driveway-open',
        '',
        'Gate Open'
      );
    });

    // Gate: Close
    document.getElementById('btn-driveway-close')?.addEventListener('click', () => {
      this.triggerGateAction(
        'driveway',
        'close',
        'btn-driveway-close',
        'feedback-driveway-close',
        '',
        'Gate Close'
      );
    });
  }

  private async triggerGateAction(
    target: 'garage' | 'driveway',
    action: string,
    btnId: string,
    feedbackId: string,
    defaultLabel: string,
    displayName: string
  ): Promise<void> {
    const lockKey = `${target}_${action}`;
    if (this.busy[lockKey]) return;
    this.busy[lockKey] = true;

    if ('vibrate' in navigator) {
      try { navigator.vibrate([40, 30, 40]); } catch {}
    }

    const btn = document.getElementById(btnId);
    const feedback = document.getElementById(feedbackId);

    btn?.classList.add('triggering');
    if (feedback) feedback.innerText = 'Sending...';
    this.showActivity(`${displayName}...`);

    try {
      const res = await fetch(`/api/gate/${target}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action })
      });

      if (res.ok) {
        if (feedback) feedback.innerText = 'Sent';
        this.showActivity(`${displayName}: Sent`);
        // Immediately refresh state
        setTimeout(() => this.fetchGateStatus(), 450);
      } else {
        const data = await res.json().catch(() => ({}));
        if (res.status === 429) {
          if (feedback) feedback.innerText = 'Wait';
          this.showActivity('Safety delay active');
        } else {
          if (feedback) feedback.innerText = 'Error';
          this.showActivity(`${displayName}: ${data.message || `HTTP ${res.status}`}`);
        }
      }
    } catch (e: any) {
      if (feedback) feedback.innerText = 'Offline';
      this.showActivity(`Network error`);
    } finally {
      setTimeout(() => {
        btn?.classList.remove('triggering');
        if (feedback) feedback.innerText = defaultLabel;
        this.busy[lockKey] = false;
      }, 1500);
    }
  }

  private showActivity(msg: string): void {
    const textEl = document.getElementById('activity-text');
    if (textEl) textEl.innerText = msg;
    if (this.activityTimer) clearTimeout(this.activityTimer);
    this.activityTimer = window.setTimeout(() => {
      if (textEl) textEl.innerText = 'Ready';
    }, 3000);
  }

  /* -------------------------------------------------------------
   * Gate Status Live Telemetry
   * ------------------------------------------------------------- */
  private async fetchGateStatus(): Promise<void> {
    try {
      const res = await fetch('/api/gate/status');
      if (!res.ok) {
        this.setOnlineStatus(false);
        return;
      }
      this.setOnlineStatus(true);
      const st: GateStatus = await res.json();

      this.renderGateState('garage', st.garage_state, st.garage_state_str);
      this.renderGateState('driveway', st.driveway_state, st.driveway_state_str);

      // Update timing ranges in settings if they exist and not being edited
      const pulseInput = document.getElementById('range-pulse-ms') as HTMLInputElement | null;
      const pulseLabel = document.getElementById('val-label-pulse');
      if (pulseInput && pulseLabel && document.activeElement !== pulseInput) {
        pulseInput.value = st.pulse_duration_ms.toString();
        pulseLabel.innerText = st.pulse_duration_ms.toString();
      }

      const interlockInput = document.getElementById('range-interlock-ms') as HTMLInputElement | null;
      const interlockLabel = document.getElementById('val-label-interlock');
      if (interlockInput && interlockLabel && document.activeElement !== interlockInput) {
        interlockInput.value = st.interlock_delay_ms.toString();
        interlockLabel.innerText = st.interlock_delay_ms.toString();
      }
    } catch {
      this.setOnlineStatus(false);
    }
  }

  private setOnlineStatus(online: boolean): void {
    const dot = document.getElementById('status-dot');
    const text = document.getElementById('status-text');
    if (dot) {
      dot.className = online ? 'dot dot-online' : 'dot dot-offline';
    }
    if (text) {
      text.innerText = online ? 'Online' : 'Offline';
    }
  }

  private renderGateState(target: 'garage' | 'driveway', stateCode: number, stateStr?: string): void {
    const label = document.getElementById(`label-${target}-state`);
    const dot = document.getElementById(`dot-${target}`);
    const resolvedName = stateStr || GATE_STATE_NAMES[stateCode] || 'UNKNOWN';

    if (label) {
      label.innerText = resolvedName;
    }

    if (dot) {
      dot.className = 'status-pulse-dot';
      if (resolvedName === 'OPENING' || resolvedName === 'CLOSING' || resolvedName === 'VENTING') {
        dot.classList.add('moving');
      } else if (resolvedName === 'OPEN' || resolvedName === 'VENT') {
        dot.classList.add('active');
      }
    }
  }

  /* -------------------------------------------------------------
   * Configuration Modal Sheet (...)
   * ------------------------------------------------------------- */
  private initModalControls(): void {
    const openBtn = document.getElementById('btn-open-settings');
    const closeBtn = document.getElementById('btn-close-settings');
    const backdrop = document.getElementById('settings-backdrop');
    const sheet = document.getElementById('settings-sheet');

    openBtn?.addEventListener('click', () => {
      backdrop?.classList.add('open');
      this.fetchSystemInfo();
      if (!this.sysPollTimer) {
        this.sysPollTimer = window.setInterval(() => this.fetchSystemInfo(), 5000);
      }
    });

    const closeModal = () => {
      backdrop?.classList.remove('open');
      if (this.sysPollTimer) {
        clearInterval(this.sysPollTimer);
        this.sysPollTimer = null;
      }
    };

    closeBtn?.addEventListener('click', closeModal);
    backdrop?.addEventListener('click', (e) => {
      if (e.target === backdrop) closeModal();
    });
    sheet?.addEventListener('click', (e) => e.stopPropagation());
  }

  /* -------------------------------------------------------------
   * Modal Tab Switching
   * ------------------------------------------------------------- */
  private initSettingsTabs(): void {
    const tabBtns = document.querySelectorAll<HTMLButtonElement>('.settings-tab-btn');
    const panels = document.querySelectorAll<HTMLElement>('.settings-panel');

    tabBtns.forEach((btn) => {
      btn.addEventListener('click', () => {
        const targetTab = btn.getAttribute('data-tab');
        tabBtns.forEach((b) => b.classList.remove('active'));
        panels.forEach((p) => p.classList.remove('active'));

        btn.classList.add('active');
        const activePanel = document.getElementById(targetTab || '');
        activePanel?.classList.add('active');
      });
    });
  }

  /* -------------------------------------------------------------
   * Settings Actions: Timings, Wi-Fi, WireGuard, Route 53
   * ------------------------------------------------------------- */
  private initSettingsActions(): void {
    // Timing Range Sliders Live Values
    const pulseRange = document.getElementById('range-pulse-ms') as HTMLInputElement | null;
    const pulseLabel = document.getElementById('val-label-pulse');
    pulseRange?.addEventListener('input', () => {
      if (pulseLabel) pulseLabel.innerText = pulseRange.value;
    });

    const interlockRange = document.getElementById('range-interlock-ms') as HTMLInputElement | null;
    const interlockLabel = document.getElementById('val-label-interlock');
    interlockRange?.addEventListener('input', () => {
      if (interlockLabel) interlockLabel.innerText = interlockRange.value;
    });

    // WireGuard Toggle Switch
    const vpnSwitch = document.getElementById('toggle-vpn-switch') as HTMLInputElement | null;
    vpnSwitch?.addEventListener('change', async () => {
      try {
        await this.postJson('/api/wireguard/toggle', { enabled: vpnSwitch.checked });
        this.fetchSystemInfo();
      } catch (err: any) {
        alert('Failed to toggle WireGuard: ' + err.message);
        vpnSwitch.checked = !vpnSwitch.checked;
      }
    });

    // Save WireGuard Config
    document.getElementById('btn-save-wg-conf')?.addEventListener('click', async () => {
      const textarea = document.getElementById('input-wg-conf') as HTMLTextAreaElement | null;
      const conf = textarea?.value.trim();
      if (!conf) {
        alert('Please paste a valid WireGuard .conf file');
        return;
      }

      try {
        const res = await fetch('/api/wireguard/config', {
          method: 'POST',
          headers: { 'Content-Type': 'text/plain' },
          body: conf
        });
        if (res.ok) {
          alert('WireGuard config saved! Reconnecting tunnel...');
          textarea!.value = '';
          this.fetchSystemInfo();
        } else {
          alert('Failed to update config: ' + (await res.text()));
        }
      } catch (err: any) {
        alert('Error: ' + err.message);
      }
    });

    // Route 53 Force Sync
    document.getElementById('btn-r53-force-sync')?.addEventListener('click', async () => {
      try {
        const res = await fetch('/api/route53/sync', { method: 'POST' });
        if (res.ok) {
          alert('Route 53 DNS sync initiated!');
          this.fetchSystemInfo();
        } else {
          alert('Sync error: ' + (await res.text()));
        }
      } catch (err: any) {
        alert('Error: ' + err.message);
      }
    });

    // Wi-Fi Connect
    document.getElementById('btn-connect-wifi')?.addEventListener('click', async () => {
      const ssidInput = document.getElementById('wifi-sta-ssid') as HTMLInputElement | null;
      const passInput = document.getElementById('wifi-sta-password') as HTMLInputElement | null;
      const ssid = ssidInput?.value.trim();
      const password = passInput?.value.trim() || '';

      if (!ssid) {
        alert('Please enter a Wi-Fi SSID');
        return;
      }

      try {
        await this.postJson('/api/wifi/connect', { ssid, password });
        alert(`Connecting to "${ssid}"... Device will obtain home IP.`);
        this.fetchSystemInfo();
      } catch (err: any) {
        alert('Wi-Fi connection error: ' + err.message);
      }
    });

    // Save Gate Timings
    document.getElementById('btn-save-gate-timings')?.addEventListener('click', async () => {
      const pulse_ms = parseInt(pulseRange?.value || '400', 10);
      const interlock_ms = parseInt(interlockRange?.value || '1500', 10);

      try {
        await this.postJson('/api/gate/settings', { pulse_ms, interlock_ms });
        alert('Gate pulse & interlock timings saved!');
      } catch (err: any) {
        alert('Error: ' + err.message);
      }
    });

    // Reboot & Reset
    document.getElementById('btn-reboot')?.addEventListener('click', () => {
      if (confirm('Reboot ESP32 controller now?')) {
        this.postJson('/api/system/restart', {});
      }
    });

    document.getElementById('btn-factory-reset')?.addEventListener('click', () => {
      if (confirm('Erase all saved settings and factory reset?')) {
        this.postJson('/api/system/factory-reset', {});
      }
    });
  }

  /* -------------------------------------------------------------
   * System Info REST Fetch
   * ------------------------------------------------------------- */
  private async fetchSystemInfo(): Promise<void> {
    try {
      const res = await fetch('/api/system/info');
      if (!res.ok) {
        this.setOnlineStatus(false);
        return;
      }
      this.setOnlineStatus(true);
      const info: SystemInfo = await res.json();

      // Top Bar VPN Status
      const vpnBadge = document.getElementById('vpn-status-badge');
      const vpnText = document.getElementById('vpn-badge-text');
      if (vpnBadge && vpnText) {
        const isConn = Boolean(info.wireguard?.connected);
        vpnBadge.className = isConn ? 'vpn-pill active' : 'vpn-pill';
        vpnText.innerText = isConn ? 'VPN ON' : 'VPN OFF';
      }

      // Modal elements
      const netWgState = document.getElementById('net-wg-state');
      const netWgIp = document.getElementById('net-wg-ip');
      const netWgEndpoint = document.getElementById('net-wg-endpoint');
      const vpnSwitch = document.getElementById('toggle-vpn-switch') as HTMLInputElement | null;

      if (netWgState) {
        netWgState.innerText = info.wireguard.connected ? 'Connected' : (info.wireguard.enabled ? 'Connecting' : 'Disabled');
        netWgState.className = info.wireguard.connected ? 'info-val badge text-cyan' : 'info-val badge';
      }
      if (netWgIp) netWgIp.innerText = info.wireguard.ip || '--';
      if (netWgEndpoint) netWgEndpoint.innerText = info.wireguard.endpoint || '--';
      if (vpnSwitch && document.activeElement !== vpnSwitch) {
        vpnSwitch.checked = info.wireguard.enabled;
      }

      // Route 53
      const r53Fqdn = document.getElementById('net-r53-fqdn');
      const r53Synced = document.getElementById('net-r53-synced');
      if (r53Fqdn) r53Fqdn.innerText = info.route53.fqdn || '--';
      if (r53Synced) {
        r53Synced.innerText = info.route53.synced ? 'Synced' : 'Pending';
        r53Synced.className = info.route53.synced ? 'info-val badge text-emerald' : 'info-val badge';
      }

      // Wi-Fi
      const wifiIp = document.getElementById('net-wifi-ip');
      const wifiSsid = document.getElementById('net-wifi-ssid');
      if (wifiIp) wifiIp.innerText = info.wifi.sta_connected ? info.wifi.sta_ip : '192.168.4.1';
      if (wifiSsid) wifiSsid.innerText = info.wifi.sta_connected ? info.wifi.sta_ssid : 'VorotaBot-AP';

      // Diagnostics
      const freeHeap = document.getElementById('val-free-heap');
      const uptime = document.getElementById('val-system-uptime');
      const rssi = document.getElementById('val-system-rssi');
      const otaPart = document.getElementById('val-ota-part');

      if (freeHeap) freeHeap.innerText = `${Math.round(info.heap_free / 1024)} KB`;
      if (uptime) uptime.innerText = this.formatUptime(info.uptime_s);
      if (rssi) rssi.innerText = `${info.wifi.sta_rssi} dBm`;
      if (otaPart && info.ota_partition) otaPart.innerText = info.ota_partition;
    } catch {
      this.setOnlineStatus(false);
    }
  }

  private formatUptime(seconds: number): string {
    const h = Math.floor(seconds / 3600).toString().padStart(2, '0');
    const m = Math.floor((seconds % 3600) / 60).toString().padStart(2, '0');
    const s = Math.floor(seconds % 60).toString().padStart(2, '0');
    return `${h}:${m}:${s}`;
  }

  /* -------------------------------------------------------------
   * OTA Firmware Flasher
   * ------------------------------------------------------------- */
  private initOtaFlasher(): void {
    const dropZone = document.getElementById('ota-drop-zone');
    const fileInput = document.getElementById('ota-file-input') as HTMLInputElement | null;

    dropZone?.addEventListener('click', () => fileInput?.click());
    dropZone?.addEventListener('dragover', (e) => {
      e.preventDefault();
      dropZone.style.borderColor = 'var(--accent-cyan)';
    });
    dropZone?.addEventListener('dragleave', () => {
      dropZone.style.borderColor = '';
    });
    dropZone?.addEventListener('drop', (e) => {
      e.preventDefault();
      dropZone.style.borderColor = '';
      if (e.dataTransfer && e.dataTransfer.files.length > 0) {
        this.uploadOtaFile(e.dataTransfer.files[0]);
      }
    });
    fileInput?.addEventListener('change', () => {
      if (fileInput.files && fileInput.files.length > 0) {
        this.uploadOtaFile(fileInput.files[0]);
      }
    });
  }

  private uploadOtaFile(file: File): void {
    if (!file.name.endsWith('.bin')) {
      alert('Invalid file format. Please select an ESP32 firmware .bin file.');
      return;
    }

    const progressContainer = document.getElementById('ota-progress-container');
    const progressBar = document.getElementById('ota-progress-bar');
    const statusText = document.getElementById('ota-status-text');
    const percentage = document.getElementById('ota-percentage');

    if (progressContainer) progressContainer.style.display = 'flex';

    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/system/ota', true);
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');

    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable) {
        const percent = Math.round((e.loaded / e.total) * 100);
        if (progressBar) progressBar.style.width = `${percent}%`;
        if (percentage) percentage.innerText = `${percent}%`;
      }
    };

    xhr.onload = () => {
      if (xhr.status === 200) {
        if (statusText) statusText.innerText = 'Flashing complete! Device rebooting...';
        setTimeout(() => window.location.reload(), 4000);
      } else {
        alert('OTA update failed: ' + xhr.responseText);
      }
    };

    xhr.onerror = () => {
      alert('Network transfer error during OTA upload.');
    };

    xhr.send(file);
  }

  private async postJson(url: string, data: any): Promise<any> {
    const res = await fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(data)
    });
    if (!res.ok) {
      const text = await res.text();
      throw new Error(text || `HTTP ${res.status}`);
    }
    return res.json();
  }
}

// Initialize on DOM load
window.addEventListener('DOMContentLoaded', () => {
  new VorotaBotApp();
});
