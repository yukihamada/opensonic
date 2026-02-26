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
function localConnect() {
    const host = location.host || 'localhost:8400';
    localWs = new WebSocket('ws://' + host + '/ws');
    localWs.onopen  = () => { setBadge('connected');    pollTxStats(); initMonitor(); };
    localWs.onclose = () => { setBadge('disconnected'); setTimeout(localConnect, 3000); };
    localWs.onerror = () => {};
    localWs.onmessage = (evt) => {
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
let monState = { supported: false, running: false, volume: 1.0, muted: false, packets: 0, volTimer: null };
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
    monState.running = !!d.running;
    monState.volume  = d.volume ?? 1.0;
    monState.muted   = !!d.muted;
    monState.packets = d.packets ?? 0;
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

    return el;
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
    { label: '5 ms',   ms: 5   },
    { label: '10 ms',  ms: 10  },
    { label: '20 ms',  ms: 20  },
    { label: '40 ms',  ms: 40  },
    { label: '60 ms',  ms: 60  },
    { label: '100 ms', ms: 100 },
    { label: '150 ms', ms: 150 },
    { label: '200 ms', ms: 200 },
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
    <input type="range" min="0" max="100" value="100" class="vol-slider" id="rxc-vol-${idx}">
    <span class="vol-pct" id="rxc-vpct-${idx}">100%</span>
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

    function doConnect() {
        let ws;
        try { ws = new WebSocket('ws://' + rx.host + '/ws'); }
        catch (_) { setTimeout(doConnect, 6000); return; }
        conn.ws = ws;

        ws.onopen = () => {
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
            setTimeout(doConnect, 5000);
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
    const volPct = Math.round(conn.volume * 100);
    if ($vol && document.activeElement !== $vol) {
        $vol.value = volPct;
        updateSliderTrack($vol, conn.volume);
    }
    if ($vpct) $vpct.textContent = volPct + '%';

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
}

function setCardConn(idx, on) {
    const $ind = document.getElementById('rxc-ind-' + idx);
    if ($ind) $ind.className = 'rxc-indicator' + (on ? ' live' : '');
    patchCard(idx);
}

// ── Volume (debounced 50ms) ───────────────────────────────────
function onVolInput(idx, slider) {
    const vol  = slider.value / 100;
    const $pct = document.getElementById('rxc-vpct-' + idx);
    if ($pct) $pct.textContent = slider.value + '%';
    updateSliderTrack(slider, vol);

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

// ── Boot ─────────────────────────────────────────────────────
// Insert monitor card above the receiver grid
(function() {
    const grid = document.getElementById('rx-grid');
    if (grid && grid.parentNode) {
        grid.parentNode.insertBefore(buildMonitorCard(), grid);
    }
})();

initCards();
localConnect();
