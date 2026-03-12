'use strict';

// ── Constants ────────────────────────────────────────────────
const DEFAULT_RECEIVERS = [
    { name: 'RPi 1',  host: 'soluna-rpi.local:8400'  },
    { name: 'RPi 2',  host: 'soluna-rpi2.local:8400' },
    { name: 'iPhone', host: 'Yukis-iPhone.local:8400' },
];

// ── State ────────────────────────────────────────────────────
let localWs   = null;
let localReqId = 1;
const localPending = new Map();

let receivers = loadReceivers();
const rxConns = {};  // host → conn object

// ── Local WebSocket (TX node serving this page) ──────────────
let localRetryDelay = 1000; // exponential backoff: 1→2→4→8→…→30s

function localConnect() {
    const host = location.host || 'localhost:8400';
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    localWs = new WebSocket(proto + '//' + host + '/ws');
    localWs.onopen  = () => {
        localRetryDelay = 1000;
        setBadge('connected');
        pollTxStats(); initMonitor(); startSyncPolling(); pollSysVol(); setInterval(pollSysVol, 2000);
        pollInputDevices(); pollInputStats(); setInterval(pollInputStats, 2000);
        initTune();
        syncAudioControls(); setInterval(syncAudioControls, 5000);
        if (baActive) baSubscribe();
    };
    localWs.onclose = () => {
        if (!relayMode) setBadge('disconnected');
        // On remote dashboards, don't retry aggressively — relay mode handles it
        if (isRemoteDashboard() && localRetryDelay >= 8000) return;
        setTimeout(localConnect, localRetryDelay);
        localRetryDelay = Math.min(localRetryDelay * 2, 30000);
    };
    localWs.onerror = () => {};
    localWs.binaryType = 'arraybuffer';
    localWs.onmessage = (evt) => {
        if (evt.data instanceof ArrayBuffer) {
            // Route binary: file chunks (0xFA 0xFB magic) vs PCM audio
            const u8 = new Uint8Array(evt.data);
            if (u8.length >= 2 && u8[0] === 0xFA && u8[1] === 0xFB) {
                _pHandleBinaryFrame(evt.data);
            } else {
                baHandleChunk(evt.data);
            }
            return;
        }
        try {
            const r = JSON.parse(evt.data);
            // Pending command response
            const cb = localPending.get(r.id);
            if (cb) { localPending.delete(r.id); cb(r); }
            // Broadcast event (no id field, has event field)
            if (r.event) _pHandleWsEvent(evt.data);
        } catch (_) {}
    };
}

function localSend(cmd, params, cb) {
    if (!localWs || localWs.readyState !== WebSocket.OPEN) return;
    const id = localReqId++;
    if (cb) localPending.set(id, cb);
    localWs.send(JSON.stringify({ id, command: cmd, params: params || {} }));
}

// ── TX pill ──────────────────────────────────────────────────
let txPktsPrev = 0;

function pollTxStats() {
    localSend('rx.stats', {}, (resp) => {
        if (resp.success && resp.data) {
            try {
                const d = JSON.parse(resp.data);
                const n    = d.packets ?? 0;
                const live = n !== txPktsPrev;
                txPktsPrev = n;
                const dot   = document.getElementById('tx-dot');
                const label = document.getElementById('tx-label');
                const pkts  = document.getElementById('tx-pkts');
                if (dot)   dot.className    = 'tx-dot' + (live ? ' live' : '');
                if (label) label.textContent = d.muted ? 'TX Muted' : 'TX';
                if (pkts)  pkts.textContent  = fmtNum(n);
            } catch (_) {}
        }
    });
    setTimeout(pollTxStats, 1000);
}

// ── Monitor card (Mac local playback via TX daemon) ───────────
let monState = { supported: false, running: false, volume: 1.0, muted: false, packets: 0, volTimer: null, lastRtt: 0, lastSyncAt: null, rxDelayMs: 0 };
let monPollTimer = null;

function initMonitor() {
    // Ask TX daemon if monitor is supported (only in TX mode)
    localSend('monitor.stats', {}, (resp) => {
        if (!resp.success || !resp.data) return;
        try {
            const d = JSON.parse(resp.data);
            if (!d.supported) return;
            // Show the monitor card
            const card = document.getElementById('mon-card');
            if (card) card.style.display = '';
            // Load device list
            localSend('monitor.list_devices', {}, (r2) => {
                if (!r2.success || !r2.data) return;
                try {
                    const devs = JSON.parse(r2.data);
                    const sel = document.getElementById('mon-dev-sel');
                    if (!sel) return;
                    sel.innerHTML = devs.map(name =>
                        `<option value="${escHtml(name)}">${escHtml(name)}</option>`
                    ).join('');
                } catch (_) {}
            });
            applyMonStats(d);
            // Poll monitor stats
            monPollTimer = setInterval(pollMonStats, 1000);
        } catch (_) {}
    });
}

function pollMonStats() {
    localSend('monitor.stats', {}, (resp) => {
        if (resp.success && resp.data) {
            try { applyMonStats(JSON.parse(resp.data)); } catch (_) {}
        }
    });
}

function applyMonStats(d) {
    monState.running   = !!d.running;
    monState.volume    = d.volume ?? 1.0;
    monState.muted     = !!d.muted;
    monState.packets   = d.packets ?? 0;
    monState.underruns = d.underruns ?? 0;
    monState.rxDelayMs = d.rx_delay_ms ?? 0;
    patchMonCard();
}

function patchMonCard() {
    const $card  = document.getElementById('mon-card');
    const $dot   = document.getElementById('mon-dot');
    const $badge = document.getElementById('mon-badge');
    const $btn   = document.getElementById('mon-start-btn');
    const $vol   = document.getElementById('mon-vol');
    const $vpct  = document.getElementById('mon-vpct');
    const $mute  = document.getElementById('mon-mute-btn');

    if (!$card) return;

    $card.classList.toggle('running', monState.running);
    if ($dot)   $dot.className    = 'mon-dot'   + (monState.running ? ' running' : '');
    if ($badge) { $badge.className = 'mon-badge' + (monState.running ? ' running' : ''); $badge.textContent = monState.running ? 'Running' : 'Idle'; }
    if ($btn)   { $btn.className   = 'mon-start-btn' + (monState.running ? ' running' : ''); $btn.textContent = monState.running ? '⏹ Stop' : '▶ Start'; }
    if ($mute)  { $mute.className  = 'btn-mute' + (monState.muted ? ' muted' : ''); $mute.textContent = monState.muted ? '🔇 Muted' : '🔊 Live'; }

    const volPct = Math.round(monState.volume * 100);
    if ($vol && document.activeElement !== $vol) {
        $vol.value = volPct;
        updateSliderTrack($vol, monState.volume);
    }
    if ($vpct) $vpct.textContent = volPct + '%';

    const $ur = document.getElementById('mon-underrun-row');
    const $urv = document.getElementById('mon-underrun-val');
    if ($ur && $urv) {
        const u = monState.underruns ?? 0;
        $ur.style.display = u > 0 ? '' : 'none';
        $urv.textContent = u;
    }

    const $rxSl = document.getElementById('mon-rx-delay-sl');
    const $rxVal = document.getElementById('mon-rx-delay-val');
    if ($rxSl && $rxVal && document.activeElement !== $rxSl) {
        $rxSl.value = monState.rxDelayMs ?? 0;
        $rxVal.textContent = (monState.rxDelayMs ?? 0) + 'ms';
    }
}

function buildMonitorCard() {
    const el = document.createElement('div');
    el.id = 'mon-card';
    el.className = 'mon-card';
    el.style.display = 'none';
    el.innerHTML = `
<div class="mon-header">
  <div class="mon-title">
    <span class="mon-dot" id="mon-dot"></span>
    Mac Monitor
  </div>
  <div class="mon-actions">
    <span class="mon-badge" id="mon-badge">Idle</span>
  </div>
</div>
<div class="mon-body">
  <div class="mon-dev-row">
    <select class="mon-dev-sel" id="mon-dev-sel"><option>Loading…</option></select>
    <button class="mon-start-btn" id="mon-start-btn">▶ Start</button>
  </div>
  <div class="vol-row">
    <span class="vol-label-txt">Vol</span>
    <input type="range" min="0" max="100" value="100" class="vol-slider" id="mon-vol">
    <span class="vol-pct" id="mon-vpct">100%</span>
  </div>
  <div class="buf-row">
    <span class="buf-label-txt">Buf</span>
    <select class="buf-sel" id="mon-bsel">${buildBufOptions(20)}</select>
  </div>
  <div class="rxc-footer" style="justify-content:flex-end">
    <button class="btn-mute" id="mon-mute-btn">🔊 Live</button>
  </div>
  <div class="buf-row" style="margin-top:0.5rem">
    <span class="buf-label-txt">iPhone Delay</span>
    <input type="range" min="0" max="2000" step="10" value="0" class="vol-slider" id="mon-rx-delay-sl" style="flex:1">
    <span class="vol-pct" id="mon-rx-delay-val">0ms</span>
    <button class="btn-sm" id="mon-rx-delay-btn" style="margin-left:6px">Set</button>
  </div>
  <div id="mon-sync-row" style="display:none;font-size:0.72rem;color:var(--text3);padding-top:0.4rem">
    🔄 Last sync: <span id="mon-sync-val">—</span>
  </div>
  <div id="mon-underrun-row" style="display:none;font-size:0.72rem;color:var(--amber);padding-top:0.2rem">
    ⚠ Underruns: <span id="mon-underrun-val">0</span>
  </div>
</div>`;

    el.querySelector('#mon-start-btn').addEventListener('click', () => {
        if (monState.running) {
            localSend('monitor.stop', {});
            monState.running = false;
            patchMonCard();
        } else {
            const sel = el.querySelector('#mon-dev-sel');
            const dev = sel ? sel.value : '';
            localSend('monitor.start', { device: dev });
            monState.running = true;
            patchMonCard();
        }
    });

    const monVol = el.querySelector('#mon-vol');
    monVol.addEventListener('input', () => {
        const vol = monVol.value / 100;
        monState.volume = vol;
        document.getElementById('mon-vpct').textContent = monVol.value + '%';
        updateSliderTrack(monVol, vol);
        clearTimeout(monState.volTimer);
        monState.volTimer = setTimeout(() => localSend('monitor.set_volume', { volume: vol }), 50);
    });
    updateSliderTrack(monVol, 1.0);

    el.querySelector('#mon-mute-btn').addEventListener('click', () => {
        monState.muted = !monState.muted;
        localSend('monitor.set_mute', { muted: monState.muted });
        patchMonCard();
    });

    el.querySelector('#mon-bsel').addEventListener('change', (e) => {
        localSend('monitor.set_buffer', { ms: parseInt(e.target.value, 10) });
    });

    const rxDelaySl = el.querySelector('#mon-rx-delay-sl');
    const rxDelayVal = el.querySelector('#mon-rx-delay-val');
    rxDelaySl.addEventListener('input', () => {
        rxDelayVal.textContent = rxDelaySl.value + 'ms';
    });
    el.querySelector('#mon-rx-delay-btn').addEventListener('click', () => {
        const ms = parseInt(rxDelaySl.value, 10);
        localSend('rx.set_global_delay', { ms });
        monState.rxDelayMs = ms;
    });

    return el;
}

// ── System Volume card (Mac output volume) ──────────────────────
let sysVolState = { volume: -1, muted: false, timer: null };

function buildSystemVolumeCard() {
    const el = document.createElement('div');
    el.id = 'sysvol-card';
    el.className = 'mon-card';
    el.style.display = 'none';
    el.innerHTML = `
<div class="mon-header">
  <div class="mon-title">
    <span class="mon-dot running" id="sysvol-dot"></span>
    Mac Volume
  </div>
  <div class="mon-actions">
    <button class="btn-mute" id="sysvol-mute-btn">🔊</button>
  </div>
</div>
<div class="mon-body">
  <div class="vol-row">
    <span class="vol-label-txt">Vol</span>
    <input type="range" min="0" max="100" value="50" class="vol-slider" id="sysvol-sl">
    <span class="vol-pct" id="sysvol-pct">50%</span>
  </div>
</div>`;

    const $sl = el.querySelector('#sysvol-sl');
    $sl.addEventListener('input', () => {
        const vol = $sl.value / 100;
        sysVolState.volume = vol;
        document.getElementById('sysvol-pct').textContent = $sl.value + '%';
        updateSliderTrack($sl, vol);
        clearTimeout(sysVolState.timer);
        sysVolState.timer = setTimeout(() => localSend('system.set_volume', { volume: vol }), 50);
    });
    updateSliderTrack($sl, 0.5);

    el.querySelector('#sysvol-mute-btn').addEventListener('click', () => {
        sysVolState.muted = !sysVolState.muted;
        localSend('system.set_mute', { muted: sysVolState.muted });
        patchSysVolCard();
    });

    return el;
}

function pollSysVol() {
    localSend('system.volume', {}, (resp) => {
        if (!resp.success || !resp.data) return;
        try {
            const d = JSON.parse(resp.data);
            if (d.volume < 0) return; // not supported
            const card = document.getElementById('sysvol-card');
            if (card) card.style.display = '';
            sysVolState.volume = d.volume;
            sysVolState.muted = !!d.muted;
            patchSysVolCard();
        } catch (_) {}
    });
}

function patchSysVolCard() {
    const $sl  = document.getElementById('sysvol-sl');
    const $pct = document.getElementById('sysvol-pct');
    const $mute = document.getElementById('sysvol-mute-btn');
    const $dot  = document.getElementById('sysvol-dot');

    const volPct = Math.round(sysVolState.volume * 100);
    if ($sl && document.activeElement !== $sl) {
        $sl.value = volPct;
        updateSliderTrack($sl, sysVolState.volume);
    }
    if ($pct) $pct.textContent = volPct + '%';
    if ($mute) {
        $mute.className = 'btn-mute' + (sysVolState.muted ? ' muted' : '');
        $mute.textContent = sysVolState.muted ? '🔇 Muted' : '🔊';
    }
    if ($dot) $dot.className = 'mon-dot' + (sysVolState.muted ? '' : ' running');
}

// ── Input Passthrough Card ──────────────────────────────────────
let inputState = { active: false, volume: 5.0, channel: 2, devices: [], timer: null };

function buildInputCard() {
    const el = document.createElement('div');
    el.id = 'input-card';
    el.className = 'mon-card';
    el.innerHTML = `
<div class="mon-header">
  <div class="mon-title">
    <span class="mon-dot" id="input-dot"></span>
    Input Passthrough
  </div>
  <div class="mon-actions">
    <button class="btn-mute" id="input-toggle-btn">▶ Start</button>
  </div>
</div>
<div class="mon-body">
  <div class="vol-row">
    <span class="vol-label-txt">Device</span>
    <select id="input-device-sel" style="flex:1;padding:4px;border-radius:6px;border:1px solid #555;background:#222;color:#fff;font-size:13px"></select>
  </div>
  <div class="vol-row">
    <span class="vol-label-txt">Ch</span>
    <input type="number" min="1" max="32" value="3" id="input-ch" style="width:50px;padding:4px;border-radius:6px;border:1px solid #555;background:#222;color:#fff;text-align:center">
  </div>
  <div class="vol-row">
    <span class="vol-label-txt">Gain</span>
    <input type="range" min="0" max="200" value="50" class="vol-slider" id="input-vol-sl">
    <span class="vol-pct" id="input-vol-pct">5.0x</span>
  </div>
</div>`;

    // Volume slider
    const $sl = el.querySelector('#input-vol-sl');
    $sl.addEventListener('input', () => {
        const vol = $sl.value / 10;  // 0..20
        inputState.volume = vol;
        document.getElementById('input-vol-pct').textContent = vol.toFixed(1) + 'x';
        updateSliderTrack($sl, $sl.value / 200);
        clearTimeout(inputState.timer);
        inputState.timer = setTimeout(() => localSend('input.set_volume', { volume: vol }), 50);
    });
    updateSliderTrack($sl, 50 / 200);

    // Toggle button
    el.querySelector('#input-toggle-btn').addEventListener('click', () => {
        if (inputState.active) {
            localSend('input.stop', {});
        } else {
            const sel = document.getElementById('input-device-sel');
            const ch = parseInt(document.getElementById('input-ch').value) - 1; // 1-indexed UI → 0-indexed
            localSend('input.start', { device: sel.value, channel: ch });
        }
        setTimeout(pollInputStats, 500);
    });

    return el;
}

function pollInputDevices() {
    localSend('input.list_devices', {}, (resp) => {
        if (!resp.success || !resp.data) return;
        try {
            const devs = JSON.parse(resp.data);
            inputState.devices = devs;
            const sel = document.getElementById('input-device-sel');
            if (!sel) return;
            sel.innerHTML = devs.map(d =>
                `<option value="${d.name}">${d.name} (${d.channels}ch)</option>`
            ).join('');
            // Select Babyface if present
            for (const d of devs) {
                if (d.name.includes('Babyface')) { sel.value = d.name; break; }
            }
        } catch (_) {}
    });
}

function pollInputStats() {
    localSend('input.stats', {}, (resp) => {
        if (!resp.success || !resp.data) return;
        try {
            const d = JSON.parse(resp.data);
            inputState.active = d.active;
            inputState.volume = d.volume;
            inputState.channel = d.channel;
            patchInputCard();
        } catch (_) {}
    });
}

function patchInputCard() {
    const $dot = document.getElementById('input-dot');
    const $btn = document.getElementById('input-toggle-btn');
    const $sl  = document.getElementById('input-vol-sl');
    const $pct = document.getElementById('input-vol-pct');
    const $ch  = document.getElementById('input-ch');

    if ($dot) $dot.className = 'mon-dot' + (inputState.active ? ' running' : '');
    if ($btn) {
        $btn.textContent = inputState.active ? '⏹ Stop' : '▶ Start';
        $btn.className = 'btn-mute' + (inputState.active ? ' muted' : '');
    }
    if ($sl && document.activeElement !== $sl) {
        $sl.value = inputState.volume * 10;
        updateSliderTrack($sl, inputState.volume / 20);
    }
    if ($pct) $pct.textContent = inputState.volume.toFixed(1) + 'x';
    if ($ch && document.activeElement !== $ch) $ch.value = inputState.channel + 1;
}

// ── Auto-sync RTT (Web UI) ────────────────────────────────────
let pingT1 = null;

function doSyncPing() {
    if (!localWs || localWs.readyState !== WebSocket.OPEN) return;
    pingT1 = performance.now();
    localSend('time.ping', {}, (resp) => {
        if (!resp.success || !resp.data) return;
        try {
            const d = JSON.parse(resp.data);
            if (!d.pong || !pingT1) return;
            const rttMs = Math.round(performance.now() - pingT1);
            pingT1 = null;
            monState.lastRtt = rttMs;
            monState.lastSyncAt = new Date();
            const $row = document.getElementById('mon-sync-row');
            const $val = document.getElementById('mon-sync-val');
            if ($row) $row.style.display = '';
            if ($val) $val.textContent = `RTT ${rttMs}ms · ${monState.lastSyncAt.toLocaleTimeString()}`;
        } catch (_) {}
    });
}

// Ping once connected, then every 30s
function startSyncPolling() {
    setTimeout(doSyncPing, 1500);
    setInterval(doSyncPing, 30000);
}

function setBadge(cls) {
    const el = document.getElementById('ws-badge');
    if (!el) return;
    el.className  = 'ws-badge ' + cls;
    el.textContent = cls === 'connected' ? '● Connected'
                   : cls === 'relay'     ? '● Relay'
                   : '● Disconnected';
}

// ── Relay mode (when no local solunad) ──────────────────────────
// Connects to wss://soluna-relay.fly.dev/ws/audio?channel=<name>
// and feeds PCM audio directly to baHandleChunk.
let relayWs = null;
let relayMode = false;
const RELAY_WS_BASE = 'wss://soluna-relay.fly.dev/ws/audio';

function isRemoteDashboard() {
    // Dashboard is served from a web server (not localhost/solunad)
    const h = location.hostname;
    return h !== 'localhost' && h !== '127.0.0.1' && !h.endsWith('.local') && !h.match(/^192\.168\./);
}

function relayConnect(channel) {
    if (!channel) return;
    relayDisconnect();
    const url = RELAY_WS_BASE + '?channel=' + encodeURIComponent(channel);
    relayWs = new WebSocket(url);
    relayWs.binaryType = 'arraybuffer';
    relayWs.onopen = () => {
        relayMode = true;
        setBadge('relay');
        const statusEl = document.getElementById('relay-status');
        if (statusEl) { statusEl.textContent = 'Relay connected: ' + channel; statusEl.style.color = '#4a4'; }
        // Auto-start browser audio if not already
        if (!baActive) baStart();
    };
    relayWs.onmessage = (evt) => {
        if (evt.data instanceof ArrayBuffer) {
            baHandleChunk(evt.data);
        }
    };
    relayWs.onclose = () => {
        relayMode = false;
        if (!localWs || localWs.readyState !== WebSocket.OPEN) setBadge('disconnected');
        const statusEl = document.getElementById('relay-status');
        if (statusEl) { statusEl.textContent = 'Relay disconnected'; statusEl.style.color = '#f66'; }
    };
    relayWs.onerror = () => {};
}

function relayDisconnect() {
    if (relayWs) { relayWs.close(); relayWs = null; }
    relayMode = false;
}

// Show relay UI when dashboard is served remotely (not from solunad)
function initRelayMode() {
    if (!isRemoteDashboard()) return;
    // Insert relay connection card before the channel-card
    const channelCard = document.getElementById('channel-card');
    if (!channelCard) return;
    const card = document.createElement('div');
    card.className = 'audio-ctrl-card';
    card.style.marginTop = '12px';
    card.innerHTML = `
        <div class="ac-header"><span class="ac-title">Relay Listen</span></div>
        <div style="padding:8px 12px">
            <p style="font-size:12px;color:#888;margin-bottom:8px">
                No local solunad detected. Listen via WAN relay by entering a channel name.
            </p>
            <div style="display:flex;gap:8px;align-items:center">
                <input type="text" id="relay-ch-input" placeholder="channel name"
                       style="flex:1;padding:6px 10px;border:1px solid #444;border-radius:6px;background:#1a1a2e;color:#fff;font-size:14px"
                       maxlength="20">
                <button id="relay-connect-btn"
                        style="padding:7px 16px;border:none;border-radius:6px;background:linear-gradient(135deg,#10b981,#06b6d4);color:#fff;font-weight:600;cursor:pointer;font-size:13px">
                    Listen
                </button>
            </div>
            <div id="relay-status" style="font-size:12px;margin-top:6px;color:#888"></div>
        </div>
    `;
    channelCard.parentNode.insertBefore(card, channelCard);

    // Load saved channel
    const saved = localStorage.getItem('soluna-channel') || localStorage.getItem('soluna-random-channel');
    const relayInput = document.getElementById('relay-ch-input');
    if (saved && relayInput) relayInput.value = saved;

    document.getElementById('relay-connect-btn').addEventListener('click', () => {
        const ch = document.getElementById('relay-ch-input').value.trim();
        if (!ch) return;
        localStorage.setItem('soluna-channel', ch);
        relayConnect(ch);
    });

    // Auto-connect if we have a saved channel
    if (saved) {
        setTimeout(() => relayConnect(saved), 1000);
    }
}

// ── Bottom nav / panels ──────────────────────────────────────
document.querySelectorAll('.bnav').forEach(btn => {
    btn.addEventListener('click', () => {
        const panel = btn.dataset.panel;
        document.querySelectorAll('.bnav').forEach(b => b.classList.remove('active'));
        btn.classList.add('active');
        document.querySelectorAll('.panel-overlay').forEach(p => p.classList.add('hidden'));
        if (panel !== 'dashboard') {
            const el = document.getElementById('panel-' + panel);
            if (el) el.classList.remove('hidden');
        }
        if (panel === 'routing') refreshRoutes();
        if (panel === 'stats')   refreshStats();
    });
});

document.querySelectorAll('.close-panel').forEach(btn => {
    btn.addEventListener('click', () => {
        btn.closest('.panel-overlay').classList.add('hidden');
        document.querySelectorAll('.bnav').forEach(b => b.classList.remove('active'));
        const dash = document.querySelector('.bnav[data-panel="dashboard"]');
        if (dash) dash.classList.add('active');
    });
});

// ── Add receiver ─────────────────────────────────────────────
document.getElementById('btn-add-rx').addEventListener('click', () => {
    const name = prompt('Receiver name:');
    if (!name) return;
    const host = prompt('Host (e.g. device.local:8400):');
    if (!host) return;
    receivers.push({ name, host });
    saveReceivers();
    spawnCard(receivers.length - 1);
    updateEmpty();
});

// ── Receiver storage ─────────────────────────────────────────
function loadReceivers() {
    try {
        const s = localStorage.getItem('soluna_rx_v2');
        if (s) return JSON.parse(s);
    } catch (_) {}
    return DEFAULT_RECEIVERS.map(r => ({ ...r }));
}

function saveReceivers() {
    localStorage.setItem('soluna_rx_v2', JSON.stringify(receivers));
}

// ── Receiver DOM ─────────────────────────────────────────────
function updateEmpty() {
    const grid  = document.getElementById('rx-grid');
    const empty = document.getElementById('rx-empty');
    const has   = grid && grid.children.length > 0;
    if (grid)  grid.style.display  = has ? '' : 'none';
    if (empty) empty.style.display = has ? 'none' : '';
}

const BUF_OPTIONS = [
    { label: '5 ms',    ms: 5    },
    { label: '10 ms',   ms: 10   },
    { label: '20 ms',   ms: 20   },
    { label: '40 ms',   ms: 40   },
    { label: '60 ms',   ms: 60   },
    { label: '100 ms',  ms: 100  },
    { label: '150 ms',  ms: 150  },
    { label: '200 ms',  ms: 200  },
    { label: '500 ms',  ms: 500  },
    { label: '1000 ms', ms: 1000 },
    { label: '1500 ms', ms: 1500 },
    { label: '2000 ms', ms: 2000 },
];

function buildBufOptions(selectedMs) {
    return BUF_OPTIONS.map(o =>
        `<option value="${o.ms}"${o.ms === selectedMs ? ' selected' : ''}>${o.label}</option>`
    ).join('');
}

function buildCard(idx) {
    const rx = receivers[idx];
    const el = document.createElement('div');
    el.className = 'rx-card';
    el.id = 'rx-card-' + idx;
    el.innerHTML = `
<div class="rxc-header">
  <div class="rxc-title">
    <span class="rxc-indicator" id="rxc-ind-${idx}"></span>
    <div>
      <div class="rxc-name">${escHtml(rx.name)}</div>
      <span class="rxc-host">${escHtml(rx.host)}</span>
    </div>
  </div>
  <div class="rxc-actions">
    <span class="rxc-badge off" id="rxc-badge-${idx}">Off</span>
    <button class="btn-remove" id="rxc-rm-${idx}" title="Remove">✕</button>
  </div>
</div>
<div class="rxc-meter"><div class="rxc-meter-fill" id="rxc-mfill-${idx}"></div></div>
<div class="rxc-body">
  <div class="vol-row">
    <span class="vol-label-txt">Vol</span>
    <input type="range" min="0" max="100" value="50" class="vol-slider" id="rxc-vol-${idx}">
    <span class="vol-pct" id="rxc-vpct-${idx}">50%</span>
  </div>
  <div class="buf-row">
    <span class="buf-label-txt">Buf</span>
    <select class="buf-sel" id="rxc-bsel-${idx}">${buildBufOptions(20)}</select>
  </div>
  <div class="delay-bar-row" id="rxc-delay-row-${idx}" style="display:none">
    <span class="buf-label-txt">Dly</span>
    <div class="delay-bar-wrap">
      <div class="delay-bar-fill" id="rxc-dbar-${idx}"></div>
    </div>
    <span class="rxc-stat-val" id="rxc-delay-${idx}" style="font-size:0.72rem;min-width:3em;text-align:right">—</span>
  </div>
  <div class="loss-sparkline" id="rxc-spark-${idx}"></div>
  <div class="rxc-footer">
    <div class="rxc-stats">
      <span>Pkts&thinsp;<span class="rxc-stat-val" id="rxc-pkts-${idx}">—</span></span>
      <span>Err&thinsp;<span class="rxc-stat-val" id="rxc-errs-${idx}">—</span></span>
      <span>Buf&thinsp;<span class="rxc-stat-val" id="rxc-buf-${idx}">—</span></span>
    </div>
    <button class="btn-mute" id="rxc-mute-${idx}">🔊 Live</button>
  </div>
</div>`;

    // Wire events after build (no inline handlers)
    el.querySelector('#rxc-rm-'   + idx).addEventListener('click', () => removeReceiver(idx));
    el.querySelector('#rxc-mute-' + idx).addEventListener('click', () => toggleMuteRx(idx));
    const volSlider = el.querySelector('#rxc-vol-' + idx);
    volSlider.addEventListener('input', () => onVolInput(idx, volSlider));
    updateSliderTrack(volSlider, 1.0);

    const bufSel = el.querySelector('#rxc-bsel-' + idx);
    bufSel.addEventListener('change', () => {
        rxSend(idx, 'rx.set_buffer', { ms: parseInt(bufSel.value, 10) });
    });

    return el;
}

function spawnCard(idx) {
    const grid = document.getElementById('rx-grid');
    if (!grid) return;
    const old = document.getElementById('rx-card-' + idx);
    if (old) old.remove();
    grid.appendChild(buildCard(idx));
    connectReceiver(idx);
}

function initCards() {
    receivers.forEach((_, idx) => spawnCard(idx));
    updateEmpty();
}

// ── Receiver WebSocket connections ───────────────────────────
function connectReceiver(idx) {
    const rx = receivers[idx];
    if (!rx) return;

    const old = rxConns[rx.host];
    if (old) {
        if (old.polling) clearInterval(old.polling);
        if (old.ws) try { old.ws.close(); } catch (_) {}
    }

    const conn = {
        ws: null, connected: false,
        stats: null, volume: 1.0, muted: false,
        polling: null, pending: new Map(), reqId: 1, volTimer: null,
    };
    rxConns[rx.host] = conn;

    conn.retryDelay = 1000; // exponential backoff per-connection

    function doConnect() {
        let ws;
        const rxProto = location.protocol === 'https:' ? 'wss:' : 'ws:';
        try { ws = new WebSocket(rxProto + '//' + rx.host + '/ws'); }
        catch (_) {
            setTimeout(doConnect, conn.retryDelay);
            conn.retryDelay = Math.min(conn.retryDelay * 2, 30000);
            return;
        }
        conn.ws = ws;

        ws.onopen = () => {
            conn.retryDelay = 1000;
            conn.connected = true;
            setCardConn(idx, true);
            // Fetch initial state
            rxSend(idx, 'rx.stats', {}, (r) => { if (r.success) applyStats(idx, r.data); });
            // 500ms polling — fast enough to feel live, light enough for WiFi
            conn.polling = setInterval(() => {
                rxSend(idx, 'rx.stats', {}, (r) => { if (r.success) applyStats(idx, r.data); });
            }, 500);
        };
        ws.onclose = () => {
            conn.connected = false;
            if (conn.polling) { clearInterval(conn.polling); conn.polling = null; }
            setCardConn(idx, false);
            setTimeout(doConnect, conn.retryDelay);
            conn.retryDelay = Math.min(conn.retryDelay * 2, 30000);
        };
        ws.onerror = () => {};
        ws.onmessage = (evt) => {
            try {
                const r = JSON.parse(evt.data);
                const cb = conn.pending.get(r.id);
                if (cb) { conn.pending.delete(r.id); cb(r); }
            } catch (_) {}
        };
    }
    doConnect();
}

function rxSend(idx, cmd, params, cb) {
    const rx = receivers[idx]; if (!rx) return;
    const conn = rxConns[rx.host];
    if (!conn || !conn.ws || conn.ws.readyState !== WebSocket.OPEN) return;
    const id = conn.reqId++;
    if (cb) conn.pending.set(id, cb);
    conn.ws.send(JSON.stringify({ id, command: cmd, params: params || {} }));
}

// ── Packet-loss history (30s, one sample/500ms = 60 samples) ─
const LOSS_HISTORY_LEN = 60;

function recordLoss(conn, dropped) {
    if (!conn.lossHistory) conn.lossHistory = [];
    conn.lossHistory.push(dropped ?? 0);
    if (conn.lossHistory.length > LOSS_HISTORY_LEN) conn.lossHistory.shift();
}

function renderLossSvg(history, width, height) {
    if (!history || history.length < 2) return '';
    const max = Math.max(...history, 1);
    const w = width, h = height;
    const pts = history.map((v, i) => {
        const x = Math.round(i / (history.length - 1) * w);
        const y = Math.round(h - (v / max) * h);
        return `${x},${y}`;
    });
    const line = `M${pts.join('L')}`;
    const fill = `${line}L${w},${h}L0,${h}Z`;
    return `<svg width="${w}" height="${h}" viewBox="0 0 ${w} ${h}" xmlns="http://www.w3.org/2000/svg">
        <path d="${fill}" fill="rgba(239,68,68,0.12)"/>
        <path d="${line}" fill="none" stroke="var(--red)" stroke-width="1.5" stroke-linejoin="round"/>
    </svg>`;
}

// ── Stats: surgical DOM patch (no full re-render) ────────────
function applyStats(idx, raw) {
    const rx = receivers[idx]; if (!rx) return;
    const conn = rxConns[rx.host]; if (!conn) return;
    try {
        const d = typeof raw === 'string' ? JSON.parse(raw) : raw;
        const prevDropped = conn.stats ? (conn.stats.errors ?? 0) : 0;
        conn.stats = d;
        if (d.volume !== undefined) conn.volume = d.volume;
        if (d.muted  !== undefined) conn.muted  = d.muted;
        // Record incremental drop count for sparkline
        const curDropped = d.errors ?? 0;
        recordLoss(conn, Math.max(0, curDropped - prevDropped));
        patchCard(idx);
    } catch (_) {}
}

function patchCard(idx) {
    const rx = receivers[idx]; if (!rx) return;
    const conn = rxConns[rx.host]; if (!conn) return;
    const s = conn.stats;

    // ── status indicators ──
    const $badge   = document.getElementById('rxc-badge-'  + idx);
    const $card    = document.getElementById('rx-card-'    + idx);
    const $muteBtn = document.getElementById('rxc-mute-'   + idx);

    if ($badge) {
        if (!conn.connected)  { $badge.className = 'rxc-badge off';   $badge.textContent = 'Off';   }
        else if (conn.muted)  { $badge.className = 'rxc-badge muted'; $badge.textContent = 'Muted'; }
        else                  { $badge.className = 'rxc-badge live';  $badge.textContent = 'Live';  }
    }
    if ($card) {
        $card.classList.toggle('live',       conn.connected && !conn.muted);
        $card.classList.toggle('muted-card', conn.connected &&  conn.muted);
    }
    if ($muteBtn) {
        $muteBtn.className   = 'btn-mute' + (conn.muted ? ' muted' : '');
        $muteBtn.textContent = conn.muted ? '🔇 Muted' : '🔊 Live';
    }

    if (!s) return;

    // ── packets / errors ──
    const $pkts = document.getElementById('rxc-pkts-' + idx);
    const $errs = document.getElementById('rxc-errs-' + idx);
    if ($pkts) $pkts.textContent = fmtNum(s.packets ?? 0);
    if ($errs) {
        const e = s.errors ?? 0;
        $errs.textContent = e;
        $errs.className   = 'rxc-stat-val' + (e > 0 ? ' err' : '');
    }

    // ── buffer meter ──
    const cap  = s.buf_cap  ?? 0;
    const fill = s.buf_fill ?? 0;
    const $buf   = document.getElementById('rxc-buf-'   + idx);
    const $mfill = document.getElementById('rxc-mfill-' + idx);
    if ($buf)   $buf.textContent   = cap ? fill + '/' + cap : '—';
    if ($mfill) $mfill.style.width = cap ? Math.min(100, fill / cap * 100) + '%' : '0%';

    // ── volume slider (skip if user is dragging) ──
    const $vol  = document.getElementById('rxc-vol-'  + idx);
    const $vpct = document.getElementById('rxc-vpct-' + idx);
    const sliderVal = gainToSlider(conn.volume);
    if ($vol && document.activeElement !== $vol) {
        $vol.value = sliderVal;
        updateSliderTrack($vol, sliderVal / 100);
    }
    if ($vpct) $vpct.textContent = sliderVal + '%';

    // ── buffer selector ──
    const $bsel = document.getElementById('rxc-bsel-' + idx);
    if ($bsel && document.activeElement !== $bsel && s.buf_target_ms !== undefined) {
        // Pick the closest option
        const ms = s.buf_target_ms;
        let best = BUF_OPTIONS[0].ms;
        let bestDiff = Math.abs(BUF_OPTIONS[0].ms - ms);
        for (const o of BUF_OPTIONS) {
            const d = Math.abs(o.ms - ms);
            if (d < bestDiff) { bestDiff = d; best = o.ms; }
        }
        $bsel.value = String(best);
    }

    // ── delay bar ──
    const delayMs = s.buf_target_ms ?? 0;
    const $delayRow = document.getElementById('rxc-delay-row-' + idx);
    const $dbar = document.getElementById('rxc-dbar-' + idx);
    const $delayVal = document.getElementById('rxc-delay-' + idx);
    if ($delayRow && delayMs > 0) {
        $delayRow.style.display = '';
        const pct = Math.min(100, delayMs / 200 * 100);
        if ($dbar) $dbar.style.width = pct + '%';
        if ($delayVal) $delayVal.textContent = delayMs + 'ms';
    }

    // ── loss sparkline ──
    const $spark = document.getElementById('rxc-spark-' + idx);
    if ($spark && conn.lossHistory && conn.lossHistory.some(v => v > 0)) {
        $spark.innerHTML = renderLossSvg(conn.lossHistory, $spark.offsetWidth || 240, 28);
    }
}

function setCardConn(idx, on) {
    const $ind = document.getElementById('rxc-ind-' + idx);
    if ($ind) $ind.className = 'rxc-indicator' + (on ? ' live' : '');
    patchCard(idx);
}

// ── Volume (debounced 50ms) ───────────────────────────────────
// Logarithmic curve: slider 0-100 maps to gain 0.0-0.015
// PCM1794A DAC has very hot output; 0.015 = loud, 0.005 = normal
function sliderToGain(pct) {
    if (pct <= 0) return 0;
    const maxGain = 0.015;
    return maxGain * Math.pow(pct / 100, 2);
}
function gainToSlider(gain) {
    if (gain <= 0) return 0;
    const maxGain = 0.015;
    return Math.min(100, Math.round(100 * Math.pow(gain / maxGain, 0.5)));
}
function onVolInput(idx, slider) {
    const vol  = sliderToGain(parseFloat(slider.value));
    const $pct = document.getElementById('rxc-vpct-' + idx);
    if ($pct) $pct.textContent = slider.value + '%';
    updateSliderTrack(slider, parseFloat(slider.value) / 100);

    const rx = receivers[idx]; if (!rx) return;
    const conn = rxConns[rx.host]; if (!conn) return;
    conn.volume = vol;

    clearTimeout(conn.volTimer);
    conn.volTimer = setTimeout(() => {
        rxSend(idx, 'rx.set_volume', { volume: vol });
    }, 50);
}

function updateSliderTrack(el, vol) {
    if (!el) return;
    const pct = Math.round(vol * 100);
    el.style.background =
        `linear-gradient(to right,var(--blue) 0%,var(--blue) ${pct}%,var(--s3) ${pct}%,var(--s3) 100%)`;
}

// ── Mute ─────────────────────────────────────────────────────
function toggleMuteRx(idx) {
    const rx = receivers[idx]; if (!rx) return;
    const conn = rxConns[rx.host]; if (!conn) return;
    const newMuted = !conn.muted;
    rxSend(idx, 'rx.set_mute', { muted: newMuted }, (resp) => {
        if (resp.success) { conn.muted = newMuted; patchCard(idx); }
    });
}

// ── Remove ───────────────────────────────────────────────────
function removeReceiver(idx) {
    const rx = receivers[idx];
    if (rx) {
        const conn = rxConns[rx.host];
        if (conn) {
            if (conn.polling) clearInterval(conn.polling);
            if (conn.ws) try { conn.ws.close(); } catch (_) {}
            delete rxConns[rx.host];
        }
    }
    receivers.splice(idx, 1);
    saveReceivers();
    // Rebuild all cards (indices shifted)
    const grid = document.getElementById('rx-grid');
    if (grid) grid.innerHTML = '';
    receivers.forEach((_, i) => spawnCard(i));
    updateEmpty();
}

// ── Routing panel ────────────────────────────────────────────
function refreshRoutes() {
    localSend('route.list', {}, (resp) => {
        const tbody = document.querySelector('#route-table tbody');
        if (!tbody) return;
        tbody.innerHTML = '';
        if (!resp.success || !resp.data) return;
        try {
            JSON.parse(resp.data).forEach((r) => {
                const tr     = document.createElement('tr');
                const srcTd  = document.createElement('td'); srcTd.textContent  = r.source;
                const dstTd  = document.createElement('td'); dstTd.textContent  = r.sink;
                const gainTd = document.createElement('td'); gainTd.textContent = r.gain_db.toFixed(1);
                const muteTd = document.createElement('td'); muteTd.textContent = r.muted ? 'Yes' : 'No';
                const actTd  = document.createElement('td');

                const muteBtn = document.createElement('button');
                muteBtn.className   = 'btn btn-sm';
                muteBtn.textContent = r.muted ? 'Unmute' : 'Mute';
                muteBtn.addEventListener('click', () =>
                    localSend('route.set_mute', { source: r.source, sink: r.sink, muted: !r.muted }, refreshRoutes));

                const rmBtn = document.createElement('button');
                rmBtn.className   = 'btn btn-sm btn-danger';
                rmBtn.textContent = 'Remove';
                rmBtn.style.marginLeft = '0.4rem';
                rmBtn.addEventListener('click', () =>
                    localSend('route.remove', { source: r.source, sink: r.sink }, refreshRoutes));

                actTd.appendChild(muteBtn);
                actTd.appendChild(rmBtn);
                tr.append(srcTd, dstTd, gainTd, muteTd, actTd);
                tbody.appendChild(tr);
            });
        } catch (_) {}
    });
}

document.getElementById('btn-route-add').addEventListener('click', () => {
    const src  = document.getElementById('route-src').value.trim();
    const dst  = document.getElementById('route-dst').value.trim();
    const gain = parseFloat(document.getElementById('route-gain').value) || 0;
    if (!src || !dst) return;
    localSend('route.add', { source: src, sink: dst, gain_db: gain }, () => {
        refreshRoutes();
        document.getElementById('route-src').value  = '';
        document.getElementById('route-dst').value  = '';
        document.getElementById('route-gain').value = '0';
    });
});

// ── Stats panel ──────────────────────────────────────────────
function refreshStats() {
    localSend('system.stats', {}, (resp) => {
        if (!resp.success || !resp.data) return;
        try {
            const s = JSON.parse(resp.data);
            setVal('stat-devices', s.device_count ?? 0);
            setVal('stat-streams', (s.active_streams ?? 0) + '/' + (s.stream_count ?? 0));
            setVal('stat-routes',  s.route_count ?? 0);
            const bps = s.bandwidth_bps ?? 0;
            setVal('stat-bw', bps < 1e6 ? (bps / 1000).toFixed(1) + ' kbps'
                                        : (bps / 1e6).toFixed(2)  + ' Mbps');
            setVal('stat-ptp',    s.ptp_synced ? 'Synced' : 'Unsynced');
            const oNs = s.ptp_offset_ns ?? 0;
            setVal('stat-offset', Math.abs(oNs) < 1000 ? oNs + ' ns'
                                                       : (oNs / 1000).toFixed(1) + ' µs');
        } catch (_) {}
    });
    // Audio quality engine stats
    localSend('rx.stats', {}, (resp) => {
        if (!resp.success || !resp.data) return;
        try {
            const d = JSON.parse(resp.data);
            if (d.dll_ratio !== undefined) {
                const ppm = ((d.dll_ratio - 1.0) * 1e6).toFixed(1);
                setVal('stat-dll-ratio', ppm + ' ppm');
            }
            if (d.opus_fec !== undefined) setVal('stat-fec', d.opus_fec ? 'ON' : 'OFF');
            if (d.deglitch !== undefined) setVal('stat-deglitch', d.deglitch ? 'ON' : 'OFF');
        } catch (_) {}
    });
}

// ── Audio Controls toggles ──────────────────────────────────
const acMap = {
    'ac-fec':      'fec',
    'ac-nack':     'nack',
    'ac-plc':      'wsola_plc',
    'ac-dup':      'dup_send',
    'ac-adaptive': 'adaptive_jitter',
};

// Toggle switches → wifi.set
Object.entries(acMap).forEach(([elId, param]) => {
    const cb = document.getElementById(elId);
    if (!cb) return;
    cb.addEventListener('change', () => {
        const p = {}; p[param] = cb.checked;
        localSend('wifi.set', p);
    });
});

// Deglitch → repair.enable / repair.disable
const acDeglitch = document.getElementById('ac-deglitch');
if (acDeglitch) {
    acDeglitch.addEventListener('change', () => {
        localSend(acDeglitch.checked ? 'repair.enable' : 'repair.disable');
    });
}

// Settings expand
const acExpand = document.getElementById('ac-expand');
const acDetail = document.getElementById('ac-detail');
if (acExpand && acDetail) {
    acExpand.addEventListener('click', () => {
        acDetail.style.display = acDetail.style.display === 'none' ? '' : 'none';
    });
}

// Noise params sliders
const acSigma = document.getElementById('ac-sigma');
if (acSigma) {
    acSigma.addEventListener('input', () => {
        setVal('ac-sigma-val', parseFloat(acSigma.value).toFixed(1));
    });
    acSigma.addEventListener('change', () => {
        localSend('noise.set', { sigma: parseFloat(acSigma.value) });
    });
}
const acXfade = document.getElementById('ac-xfade');
if (acXfade) {
    acXfade.addEventListener('input', () => {
        setVal('ac-xfade-val', acXfade.value);
    });
    acXfade.addEventListener('change', () => {
        localSend('noise.set', { crossfade_frames: parseInt(acXfade.value) });
    });
}

// Sync toggle states from server
function syncAudioControls() {
    localSend('tune.status', {}, (resp) => {
        if (!resp.success || !resp.data) return;
        try {
            const d = JSON.parse(resp.data);
            setCheck('ac-fec', d.wifi_fec);
            setCheck('ac-nack', d.wifi_nack);
            setCheck('ac-plc', d.wifi_wsola_plc);
            setCheck('ac-dup', d.wifi_dup_send);
            setCheck('ac-adaptive', d.wifi_adaptive_jitter);
            if (d.repair_enabled !== undefined) setCheck('ac-deglitch', d.repair_enabled);
        } catch (_) {}
    });
    // Noise params
    localSend('noise.get', {}, (resp) => {
        if (!resp.success || !resp.data) return;
        try {
            const d = JSON.parse(resp.data);
            const s = document.getElementById('ac-sigma');
            if (s && d.sigma !== undefined) { s.value = d.sigma; setVal('ac-sigma-val', d.sigma.toFixed(1)); }
            const x = document.getElementById('ac-xfade');
            if (x && d.crossfade_frames !== undefined) { x.value = d.crossfade_frames; setVal('ac-xfade-val', d.crossfade_frames); }
        } catch (_) {}
    });
    // DLL ratio
    localSend('rx.stats', {}, (resp) => {
        if (!resp.success || !resp.data) return;
        try {
            const d = JSON.parse(resp.data);
            if (d.dll_ratio !== undefined) {
                const ppm = ((d.dll_ratio - 1.0) * 1e6).toFixed(1);
                setVal('ac-dll-ratio', ppm + ' ppm');
            }
        } catch (_) {}
    });
}

function setCheck(id, val) {
    const el = document.getElementById(id);
    if (el && val !== undefined) el.checked = !!val;
}

// ── Helpers ──────────────────────────────────────────────────
function setVal(id, v) {
    const e = document.getElementById(id);
    if (e) e.textContent = v;
}

function fmtNum(n) {
    n = n ?? 0;
    return n >= 1e6 ? (n / 1e6).toFixed(1) + 'M'
         : n >= 1e3 ? (n / 1e3).toFixed(1) + 'k'
         : String(n);
}

function escHtml(s) {
    return String(s ?? '')
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;');
}

// ── Browser Audio Receiver ────────────────────────────────────

// AudioWorklet processor: ring-buffer-based, runs in audio thread
// Loaded via Blob URL — no extra HTTP request needed
const BA_WORKLET_SRC = `
class SolunaProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this._cap  = 131072; // 131072 samples ~2.7s at 48kHz
    this._L    = new Float32Array(this._cap);
    this._R    = new Float32Array(this._cap);
    this._w    = 0;
    this._r    = 0;
    this._tick = 0;
    this.port.onmessage = ({ data: [l, r] }) => {
      for (let i = 0; i < l.length; i++) {
        this._L[this._w % this._cap] = l[i];
        this._R[this._w % this._cap] = r[i];
        this._w++;
      }
    };
  }
  process(_, outputs) {
    const L = outputs[0][0], R = outputs[0][1] || outputs[0][0];
    const n = L.length; // 128 samples per block
    const avail = this._w - this._r;
    if (avail < n) {
      L.fill(0); R.fill(0);
      this.port.postMessage({ t: 'u' });
    } else {
      for (let i = 0; i < n; i++) {
        const p = this._r % this._cap;
        L[i] = this._L[p];
        R[i] = this._R[p];
        this._r++;
      }
    }
    // Report fill level every ~375 blocks (≈1s)
    if (++this._tick >= 375) {
      this._tick = 0;
      this.port.postMessage({ t: 's', f: this._w - this._r });
    }
    return true;
  }
}
registerProcessor('soluna', SolunaProcessor);
`;

let baCtx        = null;
let baWorklet    = null;
let baGain       = null;
let baAnalyser   = null;
let baActive     = false;
let baSampleRate = 48000;
let baChannels   = 2;
let baPkts       = 0;
let baUnderruns  = 0;
let baVuRaf      = null;
let baPeakL = 0, baPeakR = 0, baPeakTimer = 0;
let baWakeLock   = null;
let baVizMode    = 'vu'; // 'vu' | 'spectrum'
let baPingTimer  = null;

// Safari-compatible roundRect helper
function baRoundRect(ctx, x, y, w, h, r) {
    if (typeof ctx.roundRect === 'function') {
        ctx.beginPath(); ctx.roundRect(x, y, w, h, r); ctx.fill();
    } else {
        ctx.fillRect(x, y, w, h);
    }
}

function baSubscribe() {
    localSend('audio.subscribe', {}, (r) => {
        if (!r.success || !r.data) return;
        try {
            const d = JSON.parse(r.data);
            if (d.sample_rate) baSampleRate = d.sample_rate;
            if (d.channels)    baChannels   = d.channels;
            const el = document.getElementById('ba-fmt');
            if (el) el.textContent = `${(baSampleRate/1000).toFixed(1)} kHz · ${baChannels === 1 ? 'Mono' : 'Stereo'}`;
        } catch (_) {}
    });
}

async function baStart() {
    if (baActive) return;

    // Create AudioContext — must resume() to satisfy autoplay policy
    baCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: baSampleRate });
    if (baCtx.state === 'suspended') await baCtx.resume().catch(() => {});
    baCtx.onstatechange = () => {
        if (baCtx && baCtx.state === 'suspended') baCtx.resume().catch(() => {});
    };

    // Load AudioWorklet via Blob URL (no server file needed)
    const blob    = new Blob([BA_WORKLET_SRC], { type: 'application/javascript' });
    const blobURL = URL.createObjectURL(blob);
    try {
        await baCtx.audioWorklet.addModule(blobURL);
    } catch (e) {
        // Fallback: AudioWorklet not supported — continue with degraded mode
        console.warn('[ba] AudioWorklet unavailable:', e);
    } finally {
        URL.revokeObjectURL(blobURL);
    }

    // Audio graph: Worklet → Gain → Analyser → destination
    baWorklet  = new AudioWorkletNode(baCtx, 'soluna', { outputChannelCount: [2] });
    baGain     = baCtx.createGain();
    baAnalyser = baCtx.createAnalyser();
    baAnalyser.fftSize = 2048;
    baAnalyser.smoothingTimeConstant = 0.7;
    baWorklet.connect(baGain);
    baGain.connect(baAnalyser);
    baAnalyser.connect(baCtx.destination);

    // Messages from worklet thread
    baWorklet.port.onmessage = ({ data }) => {
        if (data.t === 'u') {
            baUnderruns++;
            const el = document.getElementById('ba-underrun');
            if (el) el.textContent = baUnderruns;
        } else if (data.t === 's') {
            const ms = Math.round(data.f / baSampleRate * 1000);
            const el = document.getElementById('ba-buf-val');
            if (el) el.textContent = `${ms} ms`;
        }
    };

    baActive    = true;
    baPkts      = 0;
    baUnderruns = 0;
    baSubscribe();

    // Screen Wake Lock — prevent sleep on mobile
    if ('wakeLock' in navigator) {
        navigator.wakeLock.request('screen').then(wl => { baWakeLock = wl; }).catch(() => {});
    }
    document.addEventListener('visibilitychange', baOnVisibility);

    // UI
    document.getElementById('ba-btn').classList.add('ba-playing');
    document.getElementById('ba-icon').textContent = '■';
    document.getElementById('ba-label').textContent = 'Listening…';
    document.getElementById('ba-controls').style.display = '';
    const urlEl = document.getElementById('ba-url');
    if (urlEl) urlEl.textContent = location.origin;

    baDrawViz();
    baPingTimer = setInterval(baPing, 3000);
}

function baStop() {
    if (!baActive) return;
    baActive = false;
    document.removeEventListener('visibilitychange', baOnVisibility);
    localSend('audio.unsubscribe', {}, null);
    if (baVuRaf)    { cancelAnimationFrame(baVuRaf); baVuRaf = null; }
    if (baPingTimer){ clearInterval(baPingTimer); baPingTimer = null; }
    if (baWakeLock) { baWakeLock.release(); baWakeLock = null; }
    if (baCtx)      { baCtx.close(); baCtx = null; }
    baWorklet = baGain = baAnalyser = null;
    document.getElementById('ba-btn').classList.remove('ba-playing');
    document.getElementById('ba-icon').textContent = '▶';
    document.getElementById('ba-label').textContent = 'Click to listen';
    document.getElementById('ba-controls').style.display = 'none';
    baClearCanvas();
}

function baOnVisibility() {
    if (document.visibilityState !== 'visible') return;
    // Resume context if browser suspended it (background policy)
    if (baCtx && baCtx.state === 'suspended') baCtx.resume().catch(() => {});
    // Re-acquire wake lock (auto-released on hide)
    if (baActive && 'wakeLock' in navigator) {
        navigator.wakeLock.request('screen').then(wl => { baWakeLock = wl; }).catch(() => {});
    }
}

function baHandleChunk(buf) {
    if (!baWorklet || !baActive) return;
    const s16 = new Int16Array(buf);
    const n   = (s16.length / baChannels) | 0;
    if (n <= 0) return;

    // Deinterleave S16 → Float32 per channel, transfer ownership (zero-copy)
    const L = new Float32Array(n), R = new Float32Array(n);
    if (baChannels === 1) {
        for (let i = 0; i < n; i++) L[i] = R[i] = s16[i] / 32768;
    } else {
        for (let i = 0; i < n; i++) {
            L[i] = s16[i * 2]     / 32768;
            R[i] = s16[i * 2 + 1] / 32768;
        }
    }
    baWorklet.port.postMessage([L, R], [L.buffer, R.buffer]);

    baPkts++;
    const el = document.getElementById('ba-pkt-val');
    if (el) el.textContent = baPkts;
}

// ── Visualization ─────────────────────────────────────────────

function baDrawViz() {
    if (!baAnalyser || !baActive) return;
    baVizMode === 'spectrum' ? baDrawSpectrum() : baDrawVU();
    baVuRaf = requestAnimationFrame(baDrawViz);
}

function baDrawVU() {
    const canvas = document.getElementById('ba-vu');
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const W = canvas.width, H = canvas.height;

    // Time-domain data → per-channel RMS
    const td = new Uint8Array(baAnalyser.frequencyBinCount);
    baAnalyser.getByteTimeDomainData(td);
    let sL = 0, sR = 0;
    for (let i = 0; i < td.length; i++) {
        const v = (td[i] - 128) / 128;
        (i % 2 === 0 ? (sL += v * v) : (sR += v * v));
    }
    const half = td.length / 2;
    const rmsL = Math.min(1, Math.sqrt(sL / half) * 4.5);
    const rmsR = Math.min(1, Math.sqrt(sR / half) * 4.5);

    // Peak hold
    if (rmsL > baPeakL) { baPeakL = rmsL; baPeakTimer = 45; }
    if (rmsR > baPeakR)   baPeakR = rmsR;
    if (baPeakTimer > 0) baPeakTimer--;
    else { baPeakL = Math.max(0, baPeakL - 0.01); baPeakR = baPeakL; }

    ctx.clearRect(0, 0, W, H);
    const gap = 4, ch = (H - gap) / 2;

    [[rmsL, baPeakL], [rmsR, baPeakR]].forEach(([lvl, peak], i) => {
        const y = i * (ch + gap);
        // Track
        ctx.fillStyle = 'rgba(255,255,255,0.05)';
        ctx.fillRect(0, y, W, ch);
        // Level bar
        const gr = ctx.createLinearGradient(0, 0, W, 0);
        gr.addColorStop(0, '#10b981'); gr.addColorStop(0.65, '#f59e0b'); gr.addColorStop(1, '#ef4444');
        ctx.fillStyle = gr;
        ctx.fillRect(0, y, W * lvl, ch);
        // Peak marker
        const px = Math.max(0, W * peak - 2);
        ctx.fillStyle = peak > 0.9 ? '#ef4444' : 'rgba(255,255,255,0.75)';
        ctx.fillRect(px, y, 2, ch);
        // Channel label
        ctx.fillStyle = 'rgba(255,255,255,0.25)';
        ctx.font = `${Math.round(ch * 0.7)}px monospace`;
        ctx.fillText(i === 0 ? 'L' : 'R', 4, y + ch * 0.8);
    });
}

function baDrawSpectrum() {
    const canvas = document.getElementById('ba-vu');
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const W = canvas.width, H = canvas.height;

    const fd = new Uint8Array(baAnalyser.frequencyBinCount);
    baAnalyser.getByteFrequencyData(fd);

    ctx.clearRect(0, 0, W, H);
    // Show ~0–16kHz (first 1/3 of bins at 48kHz)
    const bins = Math.min(Math.floor(fd.length / 3), W);
    const bw   = W / bins;
    for (let i = 0; i < bins; i++) {
        const v  = fd[i] / 255;
        const bh = v * H;
        // bass (blue) → mid (green) → treble (amber)
        const hue = 220 - (i / bins) * 160;
        ctx.fillStyle = `hsla(${hue},80%,55%,${0.4 + v * 0.6})`;
        ctx.fillRect(i * bw, H - bh, Math.max(1, bw - 0.5), bh);
    }
    // Frequency labels
    ctx.fillStyle = 'rgba(255,255,255,0.2)';
    ctx.font = '10px monospace';
    ['100Hz','1kHz','4kHz','8kHz','16kHz'].forEach((lbl, i, arr) => {
        const x = (i / (arr.length - 1)) * W;
        ctx.fillText(lbl, x + 2, H - 3);
    });
}

function baClearCanvas() {
    const c = document.getElementById('ba-vu');
    if (!c) return;
    c.getContext('2d').clearRect(0, 0, c.width, c.height);
}

// ── RTT ping ─────────────────────────────────────────────────

function baPing() {
    if (!baActive) return;
    const t0 = performance.now();
    localSend('time.ping', {}, () => {
        const rtt = Math.round(performance.now() - t0);
        const el = document.getElementById('ba-latency');
        if (el) el.textContent = `${rtt} ms`;
    });
}

// ── Init ─────────────────────────────────────────────────────

function baInit() {
    const btn = document.getElementById('ba-btn');
    if (btn) btn.addEventListener('click', () => baActive ? baStop() : baStart());

    const toggle = document.getElementById('ba-viz-toggle');
    if (toggle) toggle.addEventListener('click', () => {
        baVizMode = baVizMode === 'vu' ? 'spectrum' : 'vu';
        toggle.textContent = baVizMode === 'vu' ? 'Spec' : 'VU';
        toggle.title = baVizMode === 'vu' ? 'Switch to Spectrum' : 'Switch to VU Meter';
    });

    const vol = document.getElementById('ba-volume');
    if (vol) vol.addEventListener('input', () => {
        if (baGain) baGain.gain.value = parseFloat(vol.value);
        const el = document.getElementById('ba-vol-val');
        if (el) el.textContent = Math.round(vol.value * 100) + '%';
    });

    baClearCanvas();
}

// ── Tune / Repair / Latency card ─────────────────────────────
let tuneState = {
    active: false, rms_db: -100, clicks: 0, dropouts: 0, adjustments: 0,
    buf_target_ms: 20, mon_target_ms: 20,
    repair_enabled: true, repair_clicks: 0, repair_fades: 0,
    wifi_dup_send: true, wifi_fec: true, wifi_nack: true,
    wifi_wsola_plc: true, wifi_adaptive_jitter: true, wifi_dedup: true,
    lat: null
};
let noiseParams = {
    sigma: 6.0, floor: 0.005, crossfade_thresh: 0.02, crossfade_frames: 16,
    step_up: 2, step_down: 1, stable_sec: 5, loaded: false
};

function buildTuneCard() {
    const el = document.createElement('div');
    el.id = 'tune-card';
    el.className = 'mon-card';
    el.style.display = 'none';
    el.innerHTML = `
<div class="mon-header">
  <div class="mon-title">
    <span class="mon-dot" id="tune-dot"></span>
    Noise Control
  </div>
  <div class="mon-actions">
    <span class="mon-badge" id="tune-badge">Off</span>
  </div>
</div>
<div class="mon-body">
  <!-- Toggles -->
  <div class="tune-toggle-row">
    <div>
      <div class="tune-toggle-label">Auto Tune</div>
      <div class="tune-toggle-desc">Mic listens for noise, auto-adjusts buffers</div>
    </div>
    <label class="toggle green"><input type="checkbox" id="tune-sw"><span class="toggle-track"></span></label>
  </div>
  <div class="tune-toggle-row">
    <div>
      <div class="tune-toggle-label">Audio Repair</div>
      <div class="tune-toggle-desc">Declicker + crossfade on speaker output</div>
    </div>
    <label class="toggle green"><input type="checkbox" id="repair-sw" checked><span class="toggle-track"></span></label>
  </div>

  <!-- WiFi Reliability -->
  <div class="tune-section-label" style="margin-top:0.7rem;font-size:0.72rem;color:var(--text3);text-transform:uppercase;letter-spacing:0.05em">WiFi Reliability</div>
  <div style="display:grid;grid-template-columns:1fr 1fr;gap:0.25rem 0.5rem;margin-top:0.3rem">
    <div class="tune-toggle-row" style="padding:0.25rem 0">
      <div><div class="tune-toggle-label" style="font-size:0.75rem">Dup Send</div></div>
      <label class="toggle green"><input type="checkbox" id="wifi-dup-sw" checked><span class="toggle-track"></span></label>
    </div>
    <div class="tune-toggle-row" style="padding:0.25rem 0">
      <div><div class="tune-toggle-label" style="font-size:0.75rem">FEC</div></div>
      <label class="toggle green"><input type="checkbox" id="wifi-fec-sw" checked><span class="toggle-track"></span></label>
    </div>
    <div class="tune-toggle-row" style="padding:0.25rem 0">
      <div><div class="tune-toggle-label" style="font-size:0.75rem">NACK</div></div>
      <label class="toggle green"><input type="checkbox" id="wifi-nack-sw" checked><span class="toggle-track"></span></label>
    </div>
    <div class="tune-toggle-row" style="padding:0.25rem 0">
      <div><div class="tune-toggle-label" style="font-size:0.75rem">WSOLA PLC</div></div>
      <label class="toggle green"><input type="checkbox" id="wifi-plc-sw" checked><span class="toggle-track"></span></label>
    </div>
    <div class="tune-toggle-row" style="padding:0.25rem 0">
      <div><div class="tune-toggle-label" style="font-size:0.75rem">Adaptive Jitter</div></div>
      <label class="toggle green"><input type="checkbox" id="wifi-jitter-sw" checked><span class="toggle-track"></span></label>
    </div>
    <div class="tune-toggle-row" style="padding:0.25rem 0">
      <div><div class="tune-toggle-label" style="font-size:0.75rem">Dedup</div></div>
      <label class="toggle green"><input type="checkbox" id="wifi-dedup-sw" checked><span class="toggle-track"></span></label>
    </div>
  </div>

  <!-- Presets -->
  <div class="tune-presets" style="display:flex;gap:0.4rem;margin-top:0.7rem">
    <button class="preset-btn" id="preset-low" data-preset="low">
      <span class="preset-icon">&#9889;</span>
      <span class="preset-name">Low Latency</span>
      <span class="preset-desc">6ms buf / repair aggressive</span>
    </button>
    <button class="preset-btn" id="preset-bal" data-preset="bal">
      <span class="preset-icon">&#9878;</span>
      <span class="preset-name">Balanced</span>
      <span class="preset-desc">14ms buf / auto tune</span>
    </button>
    <button class="preset-btn" id="preset-safe" data-preset="safe">
      <span class="preset-icon">&#9974;</span>
      <span class="preset-name">Safe</span>
      <span class="preset-desc">24ms buf / max repair</span>
    </button>
  </div>

  <!-- RMS meter -->
  <div style="margin-top:0.6rem">
    <div style="display:flex;justify-content:space-between;font-size:0.72rem;color:var(--text3);margin-bottom:0.2rem">
      <span>Mic Level</span>
      <span id="tune-rms-val" style="color:var(--text2);font-variant-numeric:tabular-nums">—</span>
    </div>
    <div class="rms-meter"><div class="rms-meter-fill" id="tune-rms-bar"></div></div>
  </div>

  <!-- Stats -->
  <div class="tune-stats">
    <div class="tune-stat"><div class="tune-stat-label">Clicks</div><div class="tune-stat-val" id="tune-clicks">0</div></div>
    <div class="tune-stat"><div class="tune-stat-label">Repaired</div><div class="tune-stat-val ok" id="tune-repaired">0</div></div>
    <div class="tune-stat"><div class="tune-stat-label">Adjusts</div><div class="tune-stat-val" id="tune-adjusts">0</div></div>
    <div class="tune-stat"><div class="tune-stat-label">Dropouts</div><div class="tune-stat-val" id="tune-dropouts">0</div></div>
    <div class="tune-stat"><div class="tune-stat-label">Fades</div><div class="tune-stat-val ok" id="tune-fades">0</div></div>
    <div class="tune-stat"><div class="tune-stat-label">CRC Err</div><div class="tune-stat-val" id="tune-crc-errors">0</div></div>
    <div class="tune-stat"><div class="tune-stat-label">PLC</div><div class="tune-stat-val ok" id="tune-plc-frames">0</div></div>
    <div class="tune-stat"><div class="tune-stat-label">Lost</div><div class="tune-stat-val" id="tune-lost-pkts">0</div></div>
    <div class="tune-stat"><div class="tune-stat-label">Buf</div><div class="tune-stat-val" id="tune-buf-stat">—</div></div>
  </div>
  <div style="text-align:right;margin-top:0.15rem"><button class="tune-reset-link" id="tune-reset-btn">Reset counters</button></div>

  <!-- Buffer sliders -->
  <div class="tune-section">
    <div class="tune-section-hdr open" data-sec="buffers">
      <span class="sec-title">Buffers</span>
      <span class="sec-arrow">&#9654;</span>
    </div>
    <div class="tune-section-body open" id="sec-buffers">
      <div class="tune-slider-row">
        <span class="sl-label">Buf Target</span>
        <input type="range" min="2" max="100" step="1" value="20" class="vol-slider" id="tune-buf-sl" style="flex:1">
        <span class="sl-val" id="tune-buf-val">20ms</span>
      </div>
      <div class="tune-slider-row">
        <span class="sl-label">Mon Target</span>
        <input type="range" min="2" max="100" step="1" value="20" class="vol-slider" id="tune-mon-sl" style="flex:1">
        <span class="sl-val" id="tune-mon-val">20ms</span>
      </div>
      <div class="tune-slider-row">
        <span class="sl-label">Spk Delay</span>
        <input type="range" min="0" max="200" step="5" value="40" class="vol-slider" id="tune-delay-sl" style="flex:1">
        <span class="sl-val" id="tune-delay-val">40ms</span>
      </div>
    </div>
  </div>

  <!-- Noise detection params -->
  <div class="tune-section">
    <div class="tune-section-hdr" data-sec="noise">
      <span class="sec-title">Noise Detection</span>
      <span class="sec-arrow">&#9654;</span>
    </div>
    <div class="tune-section-body" id="sec-noise">
      <div class="tune-slider-row">
        <span class="sl-label">Sigma</span>
        <input type="range" min="1" max="20" step="0.5" value="6" class="vol-slider" id="ns-sigma-sl" style="flex:1">
        <span class="sl-val" id="ns-sigma-val">6.0</span>
      </div>
      <div class="tune-slider-row">
        <span class="sl-label">Floor</span>
        <input type="range" min="0" max="0.1" step="0.001" value="0.005" class="vol-slider" id="ns-floor-sl" style="flex:1">
        <span class="sl-val" id="ns-floor-val">0.005</span>
      </div>
      <div class="tune-slider-row">
        <span class="sl-label">CF Thresh</span>
        <input type="range" min="0" max="0.2" step="0.005" value="0.02" class="vol-slider" id="ns-cfthresh-sl" style="flex:1">
        <span class="sl-val" id="ns-cfthresh-val">0.020</span>
      </div>
      <div class="tune-slider-row">
        <span class="sl-label">CF Frames</span>
        <input type="range" min="2" max="128" step="2" value="16" class="vol-slider" id="ns-cfframes-sl" style="flex:1">
        <span class="sl-val" id="ns-cfframes-val">16</span>
      </div>
    </div>
  </div>

  <!-- Tune speed params -->
  <div class="tune-section">
    <div class="tune-section-hdr" data-sec="speed">
      <span class="sec-title">Tune Speed</span>
      <span class="sec-arrow">&#9654;</span>
    </div>
    <div class="tune-section-body" id="sec-speed">
      <div class="tune-slider-row">
        <span class="sl-label">Step Up</span>
        <input type="range" min="1" max="10" step="1" value="2" class="vol-slider" id="ns-stepup-sl" style="flex:1">
        <span class="sl-val" id="ns-stepup-val">2ms</span>
      </div>
      <div class="tune-slider-row">
        <span class="sl-label">Step Down</span>
        <input type="range" min="1" max="10" step="1" value="1" class="vol-slider" id="ns-stepdn-sl" style="flex:1">
        <span class="sl-val" id="ns-stepdn-val">1ms</span>
      </div>
      <div class="tune-slider-row">
        <span class="sl-label">Stable</span>
        <input type="range" min="1" max="30" step="1" value="5" class="vol-slider" id="ns-stable-sl" style="flex:1">
        <span class="sl-val" id="ns-stable-val">5s</span>
      </div>
    </div>
  </div>

  <!-- Latency -->
  <div class="tune-section" id="tune-lat-box" style="display:none">
    <div class="tune-section-hdr open" data-sec="latency">
      <span class="sec-title">Latency</span>
      <span class="sec-arrow">&#9654;</span>
    </div>
    <div class="tune-section-body open" id="sec-latency">
      <div class="lat-grid">
        <div>SHM <span class="lat-val" id="lat-shm">—</span></div>
        <div>TX Ring <span class="lat-val" id="lat-tx">—</span></div>
        <div>Spk Ring <span class="lat-val" id="lat-spk">—</span></div>
        <div>Mon Ring <span class="lat-val" id="lat-mon">—</span></div>
        <div>Spk Delay <span class="lat-val" id="lat-delay">—</span></div>
        <div>RX Ring <span class="lat-val" id="lat-rx">—</span></div>
      </div>
      <div class="lat-total">
        Local: <span id="lat-total-local">—</span> &middot; Monitor: <span id="lat-total-mon">—</span>
      </div>
    </div>
  </div>
</div>`;

    // ── Toggle switches ──
    el.querySelector('#tune-sw').addEventListener('change', (e) => {
        localSend(e.target.checked ? 'tune.start' : 'tune.stop', {});
    });
    el.querySelector('#repair-sw').addEventListener('change', (e) => {
        localSend(e.target.checked ? 'repair.enable' : 'repair.disable', {});
        tuneState.repair_enabled = e.target.checked;
    });

    // ── WiFi feature toggles ──
    const wifiToggles = [
        ['#wifi-dup-sw', 'dup_send', 'wifi_dup_send'],
        ['#wifi-fec-sw', 'fec', 'wifi_fec'],
        ['#wifi-nack-sw', 'nack', 'wifi_nack'],
        ['#wifi-plc-sw', 'wsola_plc', 'wifi_wsola_plc'],
        ['#wifi-jitter-sw', 'adaptive_jitter', 'wifi_adaptive_jitter'],
        ['#wifi-dedup-sw', 'dedup', 'wifi_dedup'],
    ];
    wifiToggles.forEach(([sel, param, stateKey]) => {
        el.querySelector(sel).addEventListener('change', (e) => {
            const p = {}; p[param] = e.target.checked;
            localSend('wifi.set', p);
            tuneState[stateKey] = e.target.checked;
        });
    });

    // ── Presets ──
    const presets = {
        low:  { sigma: 3.5, floor: 0.003, crossfade_thresh: 0.008, crossfade_frames: 32, step_up: 1, step_down: 1, stable_sec: 3, buf: 6,  mon: 8,  delay: 15 },
        bal:  { sigma: 5.0, floor: 0.005, crossfade_thresh: 0.015, crossfade_frames: 20, step_up: 2, step_down: 1, stable_sec: 5, buf: 14, mon: 16, delay: 30 },
        safe: { sigma: 2.5, floor: 0.002, crossfade_thresh: 0.005, crossfade_frames: 48, step_up: 3, step_down: 1, stable_sec: 8, buf: 24, mon: 26, delay: 40 },
    };
    let activePreset = null;
    el.querySelectorAll('.preset-btn').forEach(btn => {
        btn.addEventListener('click', () => {
            const key = btn.dataset.preset;
            const p = presets[key];
            if (!p) return;
            activePreset = key;
            // Apply noise params
            localSend('noise.set', {
                sigma: p.sigma, floor: p.floor,
                crossfade_thresh: p.crossfade_thresh, crossfade_frames: p.crossfade_frames,
                step_up: p.step_up, step_down: p.step_down, stable_sec: p.stable_sec,
            });
            // Apply buffers
            localSend('monitor.set_buffer', { ms: p.buf });
            localSend('monitor.set_delay', { ms: p.delay });
            // Enable tune + repair
            localSend('tune.start', {});
            localSend('repair.enable', {});
            // Visual feedback
            el.querySelectorAll('.preset-btn').forEach(b => b.classList.remove('active'));
            btn.classList.add('active');
        });
    });

    // ── Collapsible sections ──
    el.querySelectorAll('.tune-section-hdr').forEach(hdr => {
        hdr.addEventListener('click', () => {
            hdr.classList.toggle('open');
            const body = hdr.nextElementSibling;
            body.classList.toggle('open');
        });
    });

    // ── Reset ──
    el.querySelector('#tune-reset-btn').addEventListener('click', () => {
        localSend('repair.reset', {});
    });

    // ── Buffer sliders ──
    const bindSlider = (slId, valId, fmt, cmd, paramName) => {
        const sl = el.querySelector('#' + slId);
        sl.addEventListener('input', () => {
            document.getElementById(valId).textContent = fmt(sl.value);
        });
        sl.addEventListener('change', () => {
            const p = {}; p[paramName] = parseFloat(sl.value);
            localSend(cmd, p);
        });
    };
    bindSlider('tune-buf-sl',   'tune-buf-val',   v => v + 'ms',   'monitor.set_buffer', 'ms');
    bindSlider('tune-mon-sl',   'tune-mon-val',   v => v + 'ms',   'monitor.set_buffer', 'ms');
    bindSlider('tune-delay-sl', 'tune-delay-val', v => v + 'ms',   'monitor.set_delay',  'ms');

    // ── Noise param sliders ──
    const noiseSend = () => {
        localSend('noise.set', {
            sigma: parseFloat(document.getElementById('ns-sigma-sl').value),
            floor: parseFloat(document.getElementById('ns-floor-sl').value),
            crossfade_thresh: parseFloat(document.getElementById('ns-cfthresh-sl').value),
            crossfade_frames: parseInt(document.getElementById('ns-cfframes-sl').value, 10),
            step_up: parseInt(document.getElementById('ns-stepup-sl').value, 10),
            step_down: parseInt(document.getElementById('ns-stepdn-sl').value, 10),
            stable_sec: parseInt(document.getElementById('ns-stable-sl').value, 10),
        });
    };
    const bindNoise = (slId, valId, fmt) => {
        const sl = el.querySelector('#' + slId);
        const vl = el.querySelector('#' + valId);
        sl.addEventListener('input', () => { vl.textContent = fmt(sl.value); });
        sl.addEventListener('change', noiseSend);
    };
    bindNoise('ns-sigma-sl',    'ns-sigma-val',    v => parseFloat(v).toFixed(1));
    bindNoise('ns-floor-sl',    'ns-floor-val',    v => parseFloat(v).toFixed(3));
    bindNoise('ns-cfthresh-sl', 'ns-cfthresh-val', v => parseFloat(v).toFixed(3));
    bindNoise('ns-cfframes-sl', 'ns-cfframes-val', v => v);
    bindNoise('ns-stepup-sl',   'ns-stepup-val',   v => v + 'ms');
    bindNoise('ns-stepdn-sl',   'ns-stepdn-val',   v => v + 'ms');
    bindNoise('ns-stable-sl',   'ns-stable-val',   v => v + 's');

    return el;
}

function patchTuneCard() {
    const s = tuneState;
    const $card = document.getElementById('tune-card');
    if (!$card) return;

    // Card border glow
    $card.classList.toggle('running', s.active);

    // Header
    const $dot = document.getElementById('tune-dot');
    const $badge = document.getElementById('tune-badge');
    if ($dot) $dot.className = 'mon-dot' + (s.active ? ' running' : '');
    if ($badge) { $badge.className = 'mon-badge' + (s.active ? ' running' : ''); $badge.textContent = s.active ? 'Tuning' : 'Off'; }

    // Toggle switches (sync without re-triggering change)
    const $tuneSw = document.getElementById('tune-sw');
    if ($tuneSw && $tuneSw !== document.activeElement) $tuneSw.checked = s.active;
    const $repairSw = document.getElementById('repair-sw');
    if ($repairSw && $repairSw !== document.activeElement) $repairSw.checked = s.repair_enabled;

    // WiFi feature toggles sync
    const wifiSwitches = [
        ['wifi-dup-sw', 'wifi_dup_send'],
        ['wifi-fec-sw', 'wifi_fec'],
        ['wifi-nack-sw', 'wifi_nack'],
        ['wifi-plc-sw', 'wifi_wsola_plc'],
        ['wifi-jitter-sw', 'wifi_adaptive_jitter'],
        ['wifi-dedup-sw', 'wifi_dedup'],
    ];
    wifiSwitches.forEach(([swId, key]) => {
        const $sw = document.getElementById(swId);
        if ($sw && $sw !== document.activeElement) $sw.checked = s[key];
    });

    // RMS meter
    const rmsVal = document.getElementById('tune-rms-val');
    const rmsBar = document.getElementById('tune-rms-bar');
    if (rmsVal) rmsVal.textContent = s.rms_db > -90 ? s.rms_db.toFixed(1) + ' dB' : 'Silent';
    if (rmsBar) {
        // Map -80..0 dB to 0..100%
        const pct = Math.max(0, Math.min(100, (s.rms_db + 80) / 80 * 100));
        rmsBar.style.width = pct + '%';
        // Color: green < -20dB, amber -20..-6, red > -6
        rmsBar.style.background = s.rms_db > -6 ? 'var(--red)' : s.rms_db > -20 ? 'var(--amber)' : 'var(--green)';
    }

    // Stats
    const setText = (id, v) => { const e = document.getElementById(id); if (e) e.textContent = v; };
    setText('tune-clicks', s.clicks);
    setText('tune-dropouts', s.dropouts);
    setText('tune-adjusts', s.adjustments);
    setText('tune-repaired', s.repair_clicks);
    setText('tune-fades', s.repair_fades);
    setText('tune-crc-errors', s.crc_errors ?? 0);
    setText('tune-plc-frames', s.plc_frames ?? 0);
    setText('tune-lost-pkts', s.lost_packets ?? 0);
    setText('tune-buf-stat', s.buf_target_ms + 'ms');

    // Color code clicks
    const $clicks = document.getElementById('tune-clicks');
    if ($clicks) { $clicks.className = 'tune-stat-val' + (s.clicks > 0 ? ' warn' : ''); }
    const $drops = document.getElementById('tune-dropouts');
    if ($drops) { $drops.className = 'tune-stat-val' + (s.dropouts > 0 ? ' warn' : ''); }
    const $crc = document.getElementById('tune-crc-errors');
    if ($crc) { $crc.className = 'tune-stat-val' + ((s.crc_errors ?? 0) > 0 ? ' warn' : ''); }
    const $lost = document.getElementById('tune-lost-pkts');
    if ($lost) { $lost.className = 'tune-stat-val' + ((s.lost_packets ?? 0) > 0 ? ' warn' : ''); }

    // Update sliders only if not being dragged
    const setSlider = (slId, valId, val, unit) => {
        const sl = document.getElementById(slId);
        if (sl && document.activeElement !== sl) {
            sl.value = val;
            const vl = document.getElementById(valId);
            if (vl) vl.textContent = val + unit;
        }
    };
    setSlider('tune-buf-sl', 'tune-buf-val', s.buf_target_ms, 'ms');
    setSlider('tune-mon-sl', 'tune-mon-val', s.mon_target_ms, 'ms');
}

function applyLatency(d) {
    const setText = (id, v) => { const e = document.getElementById(id); if (e) e.textContent = v; };
    const fmt = (ms) => ms != null ? ms.toFixed(1) + 'ms' : '—';
    setText('lat-shm', fmt(d.shm_ms));
    setText('lat-tx', fmt(d.tx_ring_ms));
    setText('lat-spk', fmt(d.spk_ring_ms));
    setText('lat-delay', fmt(d.spk_delay_ms));
    setText('lat-mon', fmt(d.mon_ring_ms));
    setText('lat-rx', fmt(d.rx_ring_ms));
    setText('lat-total-local', fmt(d.total_local_ms));
    setText('lat-total-mon', fmt(d.total_monitor_ms));

    // Update speaker delay slider from latency data
    const delaySl = document.getElementById('tune-delay-sl');
    if (delaySl && document.activeElement !== delaySl) {
        delaySl.value = d.spk_delay_ms ?? 40;
        document.getElementById('tune-delay-val').textContent = (d.spk_delay_ms ?? 40).toFixed(0) + 'ms';
    }

    const box = document.getElementById('tune-lat-box');
    if (box) box.style.display = '';
}

function applyNoiseParams(d) {
    const setSlider = (slId, valId, val, fmt) => {
        const sl = document.getElementById(slId);
        const vl = document.getElementById(valId);
        if (sl && document.activeElement !== sl) {
            sl.value = val;
            if (vl) vl.textContent = fmt(val);
        }
    };
    setSlider('ns-sigma-sl',    'ns-sigma-val',    d.sigma,            v => parseFloat(v).toFixed(1));
    setSlider('ns-floor-sl',    'ns-floor-val',    d.floor,            v => parseFloat(v).toFixed(3));
    setSlider('ns-cfthresh-sl', 'ns-cfthresh-val', d.crossfade_thresh, v => parseFloat(v).toFixed(3));
    setSlider('ns-cfframes-sl', 'ns-cfframes-val', d.crossfade_frames, v => v);
    setSlider('ns-stepup-sl',   'ns-stepup-val',   d.step_up,          v => v + 'ms');
    setSlider('ns-stepdn-sl',   'ns-stepdn-val',   d.step_down,        v => v + 'ms');
    setSlider('ns-stable-sl',   'ns-stable-val',   d.stable_sec,       v => v + 's');
    noiseParams = { ...d, loaded: true };
}

function pollTuneStatus() {
    localSend('tune.status', {}, (resp) => {
        if (resp.success && resp.data) {
            try {
                const d = JSON.parse(resp.data);
                tuneState.active = d.active;
                tuneState.rms_db = d.rms_db;
                tuneState.clicks = d.clicks;
                tuneState.dropouts = d.dropouts;
                tuneState.adjustments = d.adjustments;
                tuneState.buf_target_ms = d.buf_target_ms;
                tuneState.mon_target_ms = d.mon_target_ms;
                tuneState.repair_enabled = d.repair_enabled;
                tuneState.repair_clicks = d.repair_clicks;
                tuneState.repair_fades = d.repair_fades;
                if (d.wifi_dup_send !== undefined) tuneState.wifi_dup_send = d.wifi_dup_send;
                if (d.wifi_fec !== undefined) tuneState.wifi_fec = d.wifi_fec;
                if (d.wifi_nack !== undefined) tuneState.wifi_nack = d.wifi_nack;
                if (d.wifi_wsola_plc !== undefined) tuneState.wifi_wsola_plc = d.wifi_wsola_plc;
                if (d.wifi_adaptive_jitter !== undefined) tuneState.wifi_adaptive_jitter = d.wifi_adaptive_jitter;
                if (d.wifi_dedup !== undefined) tuneState.wifi_dedup = d.wifi_dedup;
                patchTuneCard();
                // Show card on first successful response
                const card = document.getElementById('tune-card');
                if (card) card.style.display = '';
            } catch (_) {}
        }
    });
    localSend('latency', {}, (resp) => {
        if (resp.success && resp.data) {
            try { applyLatency(JSON.parse(resp.data)); } catch (_) {}
        }
    });
    // Fetch noise params (only until first load, then every 10th poll)
    if (!noiseParams.loaded || Math.random() < 0.1) {
        localSend('noise.get', {}, (resp) => {
            if (resp.success && resp.data) {
                try { applyNoiseParams(JSON.parse(resp.data)); } catch (_) {}
            }
        });
    }
}

function initTune() {
    pollTuneStatus();
    setInterval(pollTuneStatus, 1500);
}

// ── Music Player ─────────────────────────────────────────────

const playerState = {
    playlist: [],   // [{name,url,title,artist,album,art,duration,size}]
    idx: -1,
    shuffle: false,
    repeat: 'none', // 'none' | 'all' | 'one'
    shuffleOrder: [],
};

let _pAudio    = null;   // HTMLAudioElement
let _pCtx      = null;   // AudioContext
let _pAnalyser = null;   // AnalyserNode
let _pGain     = null;   // GainNode
let _pSrc      = null;   // MediaElementSourceNode
let _pAnimId   = null;
let _pSeeking  = false;

function _pFmt(sec) {
    if (!isFinite(sec) || sec < 0) return '0:00';
    const m = Math.floor(sec / 60);
    const s = Math.floor(sec % 60);
    return m + ':' + String(s).padStart(2, '0');
}

// ── Minimal ID3v2 tag parser ──────────────────────────────────

function _id3DecodeText(u8, start, end, enc) {
    let e = end;
    while (e > start && u8[e - 1] === 0) e--;
    if (e <= start) return '';
    if (enc === 0) {
        return Array.from(u8.slice(start, e)).map(c => String.fromCharCode(c)).join('');
    }
    if (enc === 1 || enc === 2) {
        const view = new DataView(u8.buffer, u8.byteOffset + start, e - start);
        let off = 0, le = true;
        if (enc === 1 && e - start >= 2) {
            const bom = view.getUint16(0, false);
            if (bom === 0xFFFE) { le = true; off = 2; }
            else if (bom === 0xFEFF) { le = false; off = 2; }
        }
        let s = '';
        for (let j = off; j + 1 < e - start; j += 2)
            s += String.fromCharCode(view.getUint16(j, le));
        return s;
    }
    try { return new TextDecoder('utf-8').decode(u8.slice(start, e)); } catch (_) { return ''; }
}

function _parseId3(buf) {
    const u8 = new Uint8Array(buf);
    const r = { title: null, artist: null, album: null, art: null };
    if (u8[0] !== 0x49 || u8[1] !== 0x44 || u8[2] !== 0x33) return r;
    const ver = u8[3];
    const tagSize = (u8[6] << 21) | (u8[7] << 14) | (u8[8] << 7) | u8[9];
    let i = 10;
    const end = Math.min(i + tagSize, u8.length);

    while (i < end - 10) {
        let fid, fsz;
        if (ver >= 3) {
            fid = String.fromCharCode(u8[i], u8[i+1], u8[i+2], u8[i+3]);
            fsz = (ver >= 4)
                ? (u8[i+4] << 21) | (u8[i+5] << 14) | (u8[i+6] << 7) | u8[i+7]
                : (u8[i+4] << 24) | (u8[i+5] << 16) | (u8[i+6] << 8) | u8[i+7];
            i += 10;
        } else {
            fid = String.fromCharCode(u8[i], u8[i+1], u8[i+2]);
            fsz = (u8[i+3] << 16) | (u8[i+4] << 8) | u8[i+5];
            i += 6;
        }
        if (fsz <= 0 || fsz > end - i) break;
        const fe = i + fsz;
        const enc = u8[i];

        if (fid === 'TIT2' || fid === 'TT2')       r.title  = _id3DecodeText(u8, i+1, fe, enc);
        else if (fid === 'TPE1' || fid === 'TP1')   r.artist = _id3DecodeText(u8, i+1, fe, enc);
        else if (fid === 'TALB' || fid === 'TAL')   r.album  = _id3DecodeText(u8, i+1, fe, enc);
        else if (fid === 'APIC' || fid === 'PIC') {
            let j = i + 1; // skip enc byte
            while (j < fe && u8[j] !== 0) j++; j++; // skip mime + null
            j++; // skip picture type
            // skip description (null-terminated, enc-aware)
            if (enc === 1 || enc === 2) {
                while (j < fe - 1 && !(u8[j] === 0 && u8[j+1] === 0)) j += 2;
                j += 2;
            } else {
                while (j < fe && u8[j] !== 0) j++; j++;
            }
            if (j < fe) {
                r.art = URL.createObjectURL(new Blob([u8.slice(j, fe)]));
            }
        }
        i = fe;
    }
    return r;
}

async function _pLoadFile(file) {
    const url  = URL.createObjectURL(file);
    const name = file.name.replace(/\.[^.]+$/, '');
    const buf  = await file.slice(0, 262144).arrayBuffer();
    const tags = _parseId3(buf);
    const ext  = (file.name.split('.').pop() || '').toUpperCase();
    const kb   = (file.size / 1024).toFixed(0);
    return {
        file, url, name,
        title:  tags.title  || name,
        artist: tags.artist || 'Unknown Artist',
        album:  tags.album  || '',
        art:    tags.art,
        fmt:    ext + ' · ' + kb + ' KB',
        duration: 0,
    };
}

// ── Shuffle ───────────────────────────────────────────────────

function _pBuildShuffle() {
    const n = playerState.playlist.length;
    const o = Array.from({length: n}, (_, i) => i);
    for (let i = n - 1; i > 0; i--) {
        const j = Math.floor(Math.random() * (i + 1));
        [o[i], o[j]] = [o[j], o[i]];
    }
    playerState.shuffleOrder = o;
}

function _pNext() {
    const n = playerState.playlist.length;
    if (!n) return -1;
    if (playerState.shuffle) {
        let p = playerState.shuffleOrder.indexOf(playerState.idx);
        return playerState.shuffleOrder[(p + 1) % n];
    }
    return (playerState.idx + 1) % n;
}

function _pPrev() {
    const n = playerState.playlist.length;
    if (!n) return -1;
    if (playerState.shuffle) {
        let p = playerState.shuffleOrder.indexOf(playerState.idx);
        return playerState.shuffleOrder[(p - 1 + n) % n];
    }
    return (playerState.idx - 1 + n) % n;
}

// ── Playback ──────────────────────────────────────────────────

function _pPlay(idx) {
    const n = playerState.playlist.length;
    if (idx < 0 || idx >= n) return;
    playerState.idx = idx;
    const t = playerState.playlist[idx];

    if (!_pAudio) {
        _pAudio = new Audio();
        _pAudio.preload = 'auto';
        _pAudio.addEventListener('timeupdate', _pUpdateSeek);
        _pAudio.addEventListener('ended',      _pOnEnded);
        _pAudio.addEventListener('play',       _pUpdateBtn);
        _pAudio.addEventListener('pause',      _pUpdateBtn);
        _pAudio.addEventListener('loadedmetadata', () => {
            t.duration = _pAudio.duration;
            _pUpdateSeek();
            _pRenderList();
        });
    }

    // Build Web Audio graph on first real play (requires user gesture)
    if (!_pCtx) {
        _pCtx      = new (window.AudioContext || window.webkitAudioContext)();
        _pSrc      = _pCtx.createMediaElementSource(_pAudio);
        _pAnalyser = _pCtx.createAnalyser();
        _pAnalyser.fftSize = 512;
        _pAnalyser.smoothingTimeConstant = 0.82;
        _pGain     = _pCtx.createGain();
        _pSrc.connect(_pGain);
        _pGain.connect(_pAnalyser);
        _pAnalyser.connect(_pCtx.destination);
        _pStartViz();
    }

    if (_pCtx.state === 'suspended') _pCtx.resume();
    _pAudio.src = t.url;
    _pAudio.play().catch(() => {});

    _pUpdateNP();
    _pRenderList();
    _pUpdateBtn();
}

function _pOnEnded() {
    const n = playerState.playlist.length;
    if (!n) return;
    if (playerState.repeat === 'one') {
        _pAudio.currentTime = 0;
        _pAudio.play().catch(() => {});
    } else if (playerState.idx === n - 1 && !playerState.shuffle && playerState.repeat === 'none') {
        _pUpdateBtn();
    } else {
        _pPlay(_pNext());
    }
}

// ── UI update ─────────────────────────────────────────────────

function _pUpdateNP() {
    const t = playerState.playlist[playerState.idx];
    if (!t) return;
    const $title  = document.getElementById('mp-title');
    const $artist = document.getElementById('mp-artist');
    const $fmt    = document.getElementById('mp-fmt');
    const $art    = document.getElementById('mp-art');
    if ($title)  $title.textContent  = t.title;
    if ($artist) $artist.textContent = t.artist + (t.album ? ' · ' + t.album : '');
    if ($fmt)    $fmt.textContent    = t.fmt;
    if ($art) {
        if (t.art) {
            $art.style.backgroundImage = `url('${t.art}')`;
            $art.classList.remove('mp-art-placeholder');
        } else {
            $art.style.backgroundImage = '';
            $art.classList.add('mp-art-placeholder');
        }
    }
}

function _pUpdateSeek() {
    if (!_pAudio || _pSeeking) return;
    const $seek = document.getElementById('mp-seek');
    const $curr = document.getElementById('mp-curr');
    const $dur  = document.getElementById('mp-dur');
    if ($seek && _pAudio.duration) $seek.value = (_pAudio.currentTime / _pAudio.duration) * 1000;
    if ($curr) $curr.textContent = _pFmt(_pAudio.currentTime);
    if ($dur)  $dur.textContent  = _pFmt(_pAudio.duration);
}

function _pUpdateBtn() {
    const $icon = document.getElementById('mp-play-icon');
    if ($icon) $icon.textContent = (_pAudio && !_pAudio.paused) ? '⏸' : '▶';
}

function _pRenderList() {
    const $list = document.getElementById('mp-playlist');
    const $hdr  = document.getElementById('mp-playlist-header');
    const $cnt  = document.getElementById('mp-playlist-count');
    if (!$list) return;
    const n = playerState.playlist.length;
    if ($hdr) $hdr.style.display = n > 0 ? '' : 'none';
    if ($cnt) $cnt.textContent = n + ' track' + (n !== 1 ? 's' : '');

    $list.innerHTML = '';
    playerState.playlist.forEach((t, i) => {
        const li = document.createElement('li');
        li.className = 'mp-track' + (i === playerState.idx ? ' active' : '');
        const thumbStyle = t.art
            ? `background-image:url('${t.art}')`
            : '';
        li.innerHTML = `
            <span class="mp-track-num"><span class="mp-num-txt">${i + 1}</span></span>
            <div class="mp-track-thumb ${t.art ? '' : 'no-art'}" style="${thumbStyle}"></div>
            <div class="mp-track-info">
                <div class="mp-track-title">${t.title.replace(/</g,'&lt;')}</div>
                <div class="mp-track-artist">${t.artist.replace(/</g,'&lt;')}</div>
            </div>
            <span class="mp-track-dur">${_pFmt(t.duration)}</span>
            <button class="mp-track-remove" title="Remove">✕</button>
        `.trim();
        li.addEventListener('click', e => {
            if (!e.target.closest('.mp-track-remove')) _pPlay(i);
        });
        li.querySelector('.mp-track-remove').addEventListener('click', e => {
            e.stopPropagation();
            _pRemove(i);
        });
        $list.appendChild(li);
    });
}

function _pRemove(idx) {
    const t = playerState.playlist[idx];
    if (t.url) URL.revokeObjectURL(t.url);
    if (t.art) URL.revokeObjectURL(t.art);
    playerState.playlist.splice(idx, 1);

    if (playerState.idx === idx) {
        if (playerState.playlist.length > 0) {
            _pPlay(Math.min(idx, playerState.playlist.length - 1));
        } else {
            playerState.idx = -1;
            if (_pAudio) { _pAudio.pause(); _pAudio.src = ''; }
            _pUpdateBtn();
            const $title  = document.getElementById('mp-title');
            const $artist = document.getElementById('mp-artist');
            const $fmt    = document.getElementById('mp-fmt');
            const $art    = document.getElementById('mp-art');
            if ($title)  $title.textContent  = 'No Track Selected';
            if ($artist) $artist.textContent = '—';
            if ($fmt)    $fmt.textContent    = '';
            if ($art) { $art.style.backgroundImage = ''; $art.classList.add('mp-art-placeholder'); }
        }
    } else if (playerState.idx > idx) {
        playerState.idx--;
    }
    _pRenderList();
}

// ── Spectrum Visualizer ───────────────────────────────────────

function _pStartViz() {
    const canvas = document.getElementById('mp-spectrum');
    if (!canvas || !_pAnalyser) return;

    // Match canvas resolution to CSS size (retina-aware)
    const dpr = window.devicePixelRatio || 1;
    const resize = () => {
        const W = canvas.clientWidth;
        const H = canvas.clientHeight;
        if (canvas.width !== W * dpr || canvas.height !== H * dpr) {
            canvas.width  = W * dpr;
            canvas.height = H * dpr;
        }
    };
    const ro = new ResizeObserver(resize);
    ro.observe(canvas);
    resize();

    const ctx2d = canvas.getContext('2d');

    const draw = () => {
        _pAnimId = requestAnimationFrame(draw);
        resize();

        const W = canvas.width;
        const H = canvas.height;
        ctx2d.clearRect(0, 0, W, H);

        const bufLen = _pAnalyser.frequencyBinCount;
        const data   = new Uint8Array(bufLen);
        _pAnalyser.getByteFrequencyData(data);

        const bars   = Math.min(bufLen, 90);
        const gap    = Math.ceil(W / bars * 0.12);
        const barW   = (W - gap * (bars - 1)) / bars;

        for (let i = 0; i < bars; i++) {
            const v    = data[i] / 255;
            const barH = Math.max(2, v * H * 0.95);
            const x    = i * (barW + gap);
            const y    = H - barH;

            // Hue: 220 (blue-indigo) → 170 (cyan-teal) → 130 (green) at high energy
            const hue = 220 - v * 90;
            const sat = 70 + v * 20;
            const lit = 50 + v * 15;

            const grd = ctx2d.createLinearGradient(0, y, 0, H);
            grd.addColorStop(0,   `hsla(${hue}, ${sat}%, ${lit}%, 0.95)`);
            grd.addColorStop(0.6, `hsla(${hue}, ${sat}%, ${lit - 10}%, 0.7)`);
            grd.addColorStop(1,   `hsla(${hue}, ${sat}%, ${lit - 20}%, 0.3)`);

            ctx2d.fillStyle = grd;

            // Rounded tops
            const rad = Math.min(barW / 2, 3);
            ctx2d.beginPath();
            if (ctx2d.roundRect) {
                ctx2d.roundRect(x, y, barW, barH, [rad, rad, 0, 0]);
            } else {
                ctx2d.rect(x, y, barW, barH);
            }
            ctx2d.fill();

            // Peak dot
            if (v > 0.05) {
                ctx2d.fillStyle = `hsla(${hue}, ${sat}%, 80%, 0.8)`;
                ctx2d.fillRect(x, y, barW, 2);
            }
        }
    };

    draw();
}

// ── File loading ──────────────────────────────────────────────

async function _pHandleFiles(files) {
    const exts = new Set(['mp3','wav','flac','aac','m4a','ogg','opus','mp4','aiff','aif']);
    const valid = Array.from(files).filter(f => exts.has(f.name.split('.').pop().toLowerCase()));
    if (!valid.length) return;

    // If daemon is connected, upload first file to daemon for network distribution + auto-switch
    const daemonConnected = localWs && localWs.readyState === WebSocket.OPEN;
    const daemonSupported = ['mp3','wav'].includes(valid[0].name.split('.').pop().toLowerCase());

    if (daemonConnected && daemonSupported) {
        // Daemon path: upload → stream → auto-switch
        await _pUploadToDaemon(valid[0]);
        // Also parse remaining files for local playlist
        if (valid.length > 1) {
            const $zone = document.getElementById('mp-drop-zone');
            const $txt  = $zone?.querySelector('.mp-drop-text');
            if ($zone) $zone.classList.add('loading');
            if ($txt)  $txt.textContent = 'Loading…';
            for (const file of valid.slice(1)) {
                playerState.playlist.push(await _pLoadFile(file));
            }
            if ($zone) $zone.classList.remove('loading');
            if ($txt)  $txt.textContent = 'Drop more files';
            _pRenderList();
        }
        return;
    }

    // Local-only path (daemon not connected or unsupported format)
    const $zone = document.getElementById('mp-drop-zone');
    const $txt  = $zone?.querySelector('.mp-drop-text');
    if ($zone) $zone.classList.add('loading');
    if ($txt)  $txt.textContent = 'Loading…';

    const startIdx = playerState.playlist.length;
    for (const file of valid) {
        playerState.playlist.push(await _pLoadFile(file));
    }

    if ($zone) $zone.classList.remove('loading');
    if ($txt)  $txt.textContent = 'Drop more files';
    _pRenderList();

    // Auto-play if nothing was playing
    if (playerState.idx === -1) _pPlay(startIdx);
}

// ── Daemon sync (upload + auto-switch) ───────────────────────

// State for file received from daemon (for auto-switch from PCM stream)
const _pSync = {
    receiving:    false,
    name:         '',
    totalBytes:   0,
    chunks:       [],   // Uint8Array[]
    receivedBytes:0,
    fileUrl:      null, // object URL once complete
    switchDelay:  0,    // ms from now to switch
    switchPosMs:  0,    // target file position at switch time
    switchTimer:  null,
};

// Show upload progress badge on drop-zone
function _pSetUploadStatus(msg, cls) {
    const $txt = document.getElementById('mp-drop-zone')?.querySelector('.mp-drop-text');
    if ($txt) { $txt.textContent = msg; $txt.className = 'mp-drop-text' + (cls ? ' ' + cls : ''); }
}

// Upload a single file to the daemon and start playback
async function _pUploadToDaemon(file) {
    _pSetUploadStatus('Uploading to daemon…');
    const $zone = document.getElementById('mp-drop-zone');
    if ($zone) $zone.classList.add('loading');

    try {
        const resp = await fetch(
            `/api/player/upload?name=${encodeURIComponent(file.name)}`,
            { method: 'POST', body: file }
        );
        if (!resp.ok) throw new Error('HTTP ' + resp.status);
        const json = await resp.json();
        if (!json.ok) throw new Error(json.error || 'upload failed');

        _pSetUploadStatus('Uploaded · streaming…');
        if ($zone) $zone.classList.remove('loading');

        // Tell daemon to play
        localSend('player.play', {}, (r) => {
            if (!r.success) console.warn('[player] play failed:', r);
        });

        // Update UI with file info from daemon
        const $title  = document.getElementById('mp-title');
        const $artist = document.getElementById('mp-artist');
        const $fmt    = document.getElementById('mp-fmt');
        const $dur    = document.getElementById('mp-dur');
        if ($title)  $title.textContent  = file.name.replace(/\.[^.]+$/, '');
        if ($artist) $artist.textContent = 'Streaming from daemon';
        if ($fmt)    $fmt.textContent    = json.fmt + ' · ' + (json.size / 1024).toFixed(0) + ' KB';
        if ($dur)    $dur.textContent    = _pFmt(json.dur_ms / 1000);
        document.getElementById('mp-play-icon').textContent = '⏸';

        // Start the browser AudioWorklet stream receive (same as Browser Audio panel)
        if (!baActive) baStart();

    } catch (err) {
        console.warn('[player] upload error:', err);
        _pSetUploadStatus('Upload failed — playing locally');
        if ($zone) $zone.classList.remove('loading');
    }
}

// Handle WebSocket events from daemon relating to the player
function _pHandleWsEvent(ev) {
    try {
        const d = JSON.parse(ev);
        if (!d.event) return;

        if (d.event === 'player.stream_start') {
            _pSetUploadStatus('Streaming · waiting for file…');
            document.getElementById('mp-play-icon').textContent = '⏸';
            // Reset file sync state
            _pSync.fileUrl = null;
            _pSync.receiving = false;

        } else if (d.event === 'player.file_start') {
            _pSync.receiving    = true;
            _pSync.name         = d.name || '';
            _pSync.totalBytes   = d.size || 0;
            _pSync.chunks       = [];
            _pSync.receivedBytes = 0;
            _pSetUploadStatus('Receiving file… 0%');

        } else if (d.event === 'player.file_done') {
            // Assemble file blob
            const blob = new Blob(_pSync.chunks, { type: _pSync.name.endsWith('.mp3') ? 'audio/mpeg' : 'audio/wav' });
            if (_pSync.fileUrl) URL.revokeObjectURL(_pSync.fileUrl);
            _pSync.fileUrl   = URL.createObjectURL(blob);
            _pSync.receiving = false;
            _pSetUploadStatus('File ready · switching soon…');

            // Tell daemon we're ready — it will send player.switch
            localSend('player.file_ready', {}, () => {});

        } else if (d.event === 'player.switch') {
            // daemon says: in switch_delay_ms, play local file from file_pos_ms
            if (!_pSync.fileUrl) return; // no file yet
            const delay = d.switch_delay_ms || 2000;
            const posMs = d.file_pos_ms    || 0;

            _pSetUploadStatus('Switching to local file…');

            clearTimeout(_pSync.switchTimer);
            _pSync.switchTimer = setTimeout(() => {
                _pDoSwitch(posMs);
            }, delay);

        } else if (d.event === 'player.done' || d.event === 'player.stopped') {
            document.getElementById('mp-play-icon').textContent = '▶';
            _pSetUploadStatus('Drop audio files here or click to browse');
        }
    } catch (_) {}
}

// Handle binary frame from daemon — could be audio PCM or file chunk
function _pHandleBinaryFrame(data) {
    const u8 = new Uint8Array(data);
    // File chunks are marked with magic bytes [0xFA, 0xFB, hi, lo]
    if (u8.length >= 4 && u8[0] === 0xFA && u8[1] === 0xFB && _pSync.receiving) {
        const chunkSize = (u8[2] << 8) | u8[3];
        const chunk = u8.slice(4, 4 + chunkSize);
        _pSync.chunks.push(chunk);
        _pSync.receivedBytes += chunk.byteLength;
        if (_pSync.totalBytes > 0) {
            const pct = Math.round(_pSync.receivedBytes / _pSync.totalBytes * 100);
            _pSetUploadStatus(`Receiving file… ${pct}%`);
        }
        // (Audio PCM chunks are handled by existing baWorklet path)
    }
}

// Perform the actual switch: stop AudioWorklet stream, play local file from posMs
function _pDoSwitch(posMs) {
    if (!_pSync.fileUrl) return;

    // Stop browser PCM stream
    if (baActive) baStop();

    // Create (or reuse) local audio element
    if (!_pAudio) {
        _pAudio = new Audio();
        _pAudio.addEventListener('timeupdate', _pUpdateSeek);
        _pAudio.addEventListener('ended',      _pOnEnded);
        _pAudio.addEventListener('play',       _pUpdateBtn);
        _pAudio.addEventListener('pause',      _pUpdateBtn);
    }

    // Setup Web Audio graph if needed
    if (!_pCtx) {
        _pCtx      = new (window.AudioContext || window.webkitAudioContext)();
        _pSrc      = _pCtx.createMediaElementSource(_pAudio);
        _pAnalyser = _pCtx.createAnalyser();
        _pAnalyser.fftSize = 512;
        _pAnalyser.smoothingTimeConstant = 0.82;
        _pGain     = _pCtx.createGain();
        _pSrc.connect(_pGain);
        _pGain.connect(_pAnalyser);
        _pAnalyser.connect(_pCtx.destination);
        _pStartViz();
    }

    if (_pCtx.state === 'suspended') _pCtx.resume();

    _pAudio.src = _pSync.fileUrl;
    _pAudio.currentTime = posMs / 1000;
    _pAudio.play().catch(() => {});

    _pSetUploadStatus('Playing locally ✓');
    document.getElementById('mp-play-icon').textContent = '⏸';

    // Add to playlist UI if not already there
    const name = _pSync.name.replace(/\.[^.]+$/, '');
    if (playerState.playlist.length === 0) {
        playerState.playlist.push({
            url: _pSync.fileUrl, name, title: name,
            artist: 'Synced from daemon', album: '', art: null,
            fmt: _pSync.name.split('.').pop().toUpperCase(), duration: 0,
        });
        playerState.idx = 0;
        _pUpdateNP();
        _pRenderList();
    }
}

// ── Init ──────────────────────────────────────────────────────

function initPlayer() {
    const $zone  = document.getElementById('mp-drop-zone');
    const $input = document.getElementById('mp-file-input');

    if ($zone) {
        $zone.addEventListener('dragover',  e => { e.preventDefault(); $zone.classList.add('drag-over'); });
        $zone.addEventListener('dragleave', ()  => $zone.classList.remove('drag-over'));
        $zone.addEventListener('drop', e => {
            e.preventDefault();
            $zone.classList.remove('drag-over');
            _pHandleFiles(e.dataTransfer.files);
        });
        $zone.addEventListener('click', () => $input?.click());
    }

    if ($input) $input.addEventListener('change', () => _pHandleFiles($input.files));

    // Play / Pause
    document.getElementById('mp-btn-play')?.addEventListener('click', () => {
        if (playerState.idx < 0) { if (playerState.playlist.length) _pPlay(0); return; }
        if (_pCtx?.state === 'suspended') _pCtx.resume();
        if (_pAudio?.paused) _pAudio.play().catch(() => {});
        else _pAudio?.pause();
    });

    // Prev
    document.getElementById('mp-btn-prev')?.addEventListener('click', () => {
        if (_pAudio && _pAudio.currentTime > 3) { _pAudio.currentTime = 0; }
        else _pPlay(_pPrev());
    });

    // Next
    document.getElementById('mp-btn-next')?.addEventListener('click', () => _pPlay(_pNext()));

    // Shuffle
    document.getElementById('mp-btn-shuffle')?.addEventListener('click', function () {
        playerState.shuffle = !playerState.shuffle;
        if (playerState.shuffle) _pBuildShuffle();
        this.classList.toggle('active', playerState.shuffle);
    });

    // Repeat (cycle: none → all → one → none)
    const repeatModes = ['none', 'all', 'one'];
    const repeatLabel = { none: '↻', all: '↻', one: '⟳' };
    const repeatTitle = { none: 'Repeat Off', all: 'Repeat All', one: 'Repeat One' };
    document.getElementById('mp-btn-repeat')?.addEventListener('click', function () {
        const next = repeatModes[(repeatModes.indexOf(playerState.repeat) + 1) % 3];
        playerState.repeat = next;
        this.textContent = repeatLabel[next];
        this.title       = repeatTitle[next];
        this.classList.toggle('active', next !== 'none');
    });

    // Seek
    const $seek = document.getElementById('mp-seek');
    if ($seek) {
        $seek.addEventListener('mousedown',  () => { _pSeeking = true; });
        $seek.addEventListener('touchstart', () => { _pSeeking = true; }, { passive: true });
        $seek.addEventListener('input', () => {
            if (_pAudio && _pAudio.duration) {
                const $curr = document.getElementById('mp-curr');
                const t = (_pSeek_value() / 1000) * _pAudio.duration;
                if ($curr) $curr.textContent = _pFmt(t);
            }
        });
        const commit = () => {
            if (_pAudio && _pAudio.duration) {
                _pAudio.currentTime = ($seek.value / 1000) * _pAudio.duration;
            }
            _pSeeking = false;
        };
        $seek.addEventListener('mouseup',  commit);
        $seek.addEventListener('touchend', commit);
        $seek.addEventListener('change',   commit);
    }

    // Volume
    const $vol = document.getElementById('mp-volume');
    if ($vol) {
        $vol.addEventListener('input', () => {
            const v = parseFloat($vol.value);
            if (_pGain)       _pGain.gain.value = v;
            else if (_pAudio) _pAudio.volume    = v;
            const $icon = document.getElementById('mp-vol-icon');
            const $val  = document.getElementById('mp-vol-val');
            if ($icon) $icon.textContent = v === 0 ? '🔇' : v < 0.4 ? '🔉' : '🔊';
            if ($val)  $val.textContent  = Math.round(v * 100) + '%';
        });
    }

    // Clear all
    document.getElementById('mp-btn-clear')?.addEventListener('click', () => {
        playerState.playlist.forEach(t => {
            if (t.url) URL.revokeObjectURL(t.url);
            if (t.art) URL.revokeObjectURL(t.art);
        });
        playerState.playlist = [];
        playerState.idx = -1;
        if (_pAudio) { _pAudio.pause(); _pAudio.src = ''; }
        _pUpdateBtn();
        _pRenderList();
        const $title  = document.getElementById('mp-title');
        const $artist = document.getElementById('mp-artist');
        const $art    = document.getElementById('mp-art');
        if ($title)  $title.textContent  = 'No Track Selected';
        if ($artist) $artist.textContent = '—';
        if ($art) { $art.style.backgroundImage = ''; $art.classList.add('mp-art-placeholder'); }
    });
}

function _pSeek_value() {
    return parseFloat(document.getElementById('mp-seek')?.value || 0);
}

// ── Boot ─────────────────────────────────────────────────────
// Insert system volume card and monitor card above the receiver grid
(function() {
    const grid = document.getElementById('rx-grid');
    if (grid && grid.parentNode) {
        grid.parentNode.insertBefore(buildTuneCard(), grid);
        grid.parentNode.insertBefore(buildMonitorCard(), grid);
        grid.parentNode.insertBefore(buildInputCard(), grid);
        grid.parentNode.insertBefore(buildSystemVolumeCard(), grid);
    }
})();

initCards();
localConnect();
baInit();
initPlayer();
initRelayMode();

// Stop browser audio on page unload to prevent AudioContext hanging
window.addEventListener('pagehide', () => { if (baActive) baStop(); });
window.addEventListener('beforeunload', () => { if (baActive) baStop(); });

// ── Channel Name Purchase (Web) ──────────────────────────────────
(function initChannelUI() {
    const RELAY_HTTP = 'https://relay.solun.art';
    const STRIPE_LINK = 'https://buy.stripe.com/eVqfZh4jV03Ebk4exyefC3x';
    const input = document.getElementById('ch-name-input');
    const avail = document.getElementById('ch-avail');
    const btn = document.getElementById('ch-claim-btn');
    const status = document.getElementById('ch-status');
    if (!input || !btn) return;

    // ── Random channel (free) ────────────────────────────────────
    const randomBtn = document.getElementById('ch-random-btn');
    const randomResult = document.getElementById('ch-random-result');
    if (randomBtn) {
        // Generate 6 hex chars
        function genRandomChannel() {
            const arr = new Uint8Array(3);
            crypto.getRandomValues(arr);
            return Array.from(arr, b => b.toString(16).padStart(2, '0')).join('');
        }
        // Load saved random channel
        const savedRandom = localStorage.getItem('soluna-random-channel');
        if (savedRandom && randomResult) {
            randomResult.style.display = '';
            randomResult.innerHTML = 'Channel: <strong>' + savedRandom + '</strong> <span style="color:#888;font-size:11px">(free)</span>';
        }
        randomBtn.addEventListener('click', () => {
            const ch = genRandomChannel();
            localStorage.setItem('soluna-random-channel', ch);
            localStorage.setItem('soluna-channel', ch);
            if (randomResult) {
                randomResult.style.display = '';
                randomResult.innerHTML = 'Channel: <strong>' + ch + '</strong> <span style="color:#888;font-size:11px">(free)</span>';
            }
            if (status) { status.textContent = 'Random channel set: ' + ch; status.style.color = '#4a4'; }
            // Also set the channel on the local TX daemon
            localSend('channel.set', { channel: ch });
        });
    }

    let debounce = null;
    const deviceID = localStorage.getItem('soluna-device-id') ||
        (() => { const id = crypto.randomUUID(); localStorage.setItem('soluna-device-id', id); return id; })();

    // Check if already purchased
    const purchased = localStorage.getItem('soluna-channel-purchased') === 'true';

    // Load saved channel
    const saved = localStorage.getItem('soluna-channel');
    if (saved) {
        input.value = saved;
        status.textContent = 'Current: ' + saved;
        status.style.color = '#4a4';
    }

    // Update button text based on purchase state
    function updateBtn() {
        if (purchased || saved) {
            btn.textContent = 'Set Channel Name';
        } else {
            btn.textContent = 'Purchase Channel — $4.99/yr';
        }
    }
    updateBtn();

    // Handle return from Stripe checkout
    const params = new URLSearchParams(location.search);
    const stripeSessionId = params.get('session_id') || '';
    if (stripeSessionId) {
        localStorage.setItem('soluna-channel-purchased', 'true');
        localStorage.setItem('soluna-session-id', stripeSessionId);
        const pending = localStorage.getItem('soluna-channel-pending');
        if (pending) {
            input.value = pending;
            localStorage.removeItem('soluna-channel-pending');
            status.textContent = 'Payment confirmed! Claiming...';
            status.style.color = '#4a4';
            setTimeout(() => claimChannel(pending, stripeSessionId), 500);
        }
        history.replaceState({}, '', location.pathname);
    }

    function validate(name) {
        if (name.length < 3 || name.length > 20) return false;
        if (!/^[a-z0-9-]+$/.test(name)) return false;
        if (/^[0-9a-f]+$/.test(name)) return false; // all hex = reserved
        if (['soluna','default','test'].includes(name)) return false;
        return true;
    }

    input.addEventListener('input', () => {
        clearTimeout(debounce);
        const name = input.value.trim().toLowerCase();
        avail.textContent = '';
        btn.disabled = true;
        btn.style.opacity = '0.5';
        if (!validate(name)) return;

        debounce = setTimeout(async () => {
            try {
                const r = await fetch(`${RELAY_HTTP}/api/channel/check?name=${encodeURIComponent(name)}`);
                const d = await r.json();
                if (d.available) {
                    avail.textContent = '✓';
                    avail.style.color = '#4a4';
                    btn.disabled = false;
                    btn.style.opacity = '1';
                } else {
                    avail.textContent = '✗';
                    avail.style.color = '#f44';
                }
            } catch (e) {
                avail.textContent = '⚠';
                avail.style.color = '#fa0';
            }
        }, 500);
    });

    async function claimChannel(name, sessionId) {
        btn.disabled = true;
        btn.textContent = 'Setting up...';
        const sid = sessionId || localStorage.getItem('soluna-session-id') || '';
        try {
            const r = await fetch(`${RELAY_HTTP}/api/channel/claim`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ name, device: deviceID, session_id: sid })
            });
            const d = await r.json();
            if (d.status === 'claimed' || d.status === 'renewed') {
                localStorage.setItem('soluna-channel', name);
                localStorage.setItem('soluna-channel-purchased', 'true');
                status.textContent = 'Channel set: ' + name;
                status.style.color = '#4a4';
            } else if (d.error === 'taken') {
                status.textContent = 'Name already taken';
                status.style.color = '#f44';
            } else {
                status.textContent = d.error || 'Error';
                status.style.color = '#f44';
            }
        } catch (e) {
            status.textContent = 'Connection failed';
            status.style.color = '#f44';
        }
        updateBtn();
        btn.disabled = false;
        btn.style.opacity = '1';
    }

    btn.addEventListener('click', async () => {
        const name = input.value.trim().toLowerCase();
        if (!validate(name)) return;

        const hasPurchased = localStorage.getItem('soluna-channel-purchased') === 'true';
        if (hasPurchased) {
            // Already purchased — just claim directly
            await claimChannel(name);
        } else {
            // Save pending name and redirect to Stripe
            localStorage.setItem('soluna-channel-pending', name);
            window.open(STRIPE_LINK, '_blank');
            status.textContent = 'Complete payment in the new tab...';
            status.style.color = '#fa0';
        }
    });
})();
