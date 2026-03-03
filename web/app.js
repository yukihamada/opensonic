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
    localWs = new WebSocket('ws://' + host + '/ws');
    localWs.onopen  = () => {
        localRetryDelay = 1000;
        setBadge('connected');
        pollTxStats(); initMonitor(); startSyncPolling(); pollSysVol(); setInterval(pollSysVol, 2000);
        pollInputDevices(); pollInputStats(); setInterval(pollInputStats, 2000);
        initTune();
        if (baActive) baSubscribe();
    };
    localWs.onclose = () => {
        setBadge('disconnected');
        setTimeout(localConnect, localRetryDelay);
        localRetryDelay = Math.min(localRetryDelay * 2, 30000);
    };
    localWs.onerror = () => {};
    localWs.binaryType = 'arraybuffer';
    localWs.onmessage = (evt) => {
        if (evt.data instanceof ArrayBuffer) { baHandleChunk(evt.data); return; }
        try {
            const r = JSON.parse(evt.data);
            const cb = localPending.get(r.id);
            if (cb) { localPending.delete(r.id); cb(r); }
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
    el.textContent = cls === 'connected' ? '● Connected' : '● Disconnected';
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
        try { ws = new WebSocket('ws://' + rx.host + '/ws'); }
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

// Stop browser audio on page unload to prevent AudioContext hanging
window.addEventListener('pagehide', () => { if (baActive) baStop(); });
window.addEventListener('beforeunload', () => { if (baActive) baStop(); });
