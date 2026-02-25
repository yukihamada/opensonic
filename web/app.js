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
    localWs.onopen  = () => { setBadge('connected');    pollTxStats(); };
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
    el.querySelector('#rxc-rm-' + idx).addEventListener('click', () => removeReceiver(idx));
    el.querySelector('#rxc-mute-' + idx).addEventListener('click', () => toggleMuteRx(idx));
    const volSlider = el.querySelector('#rxc-vol-' + idx);
    volSlider.addEventListener('input', () => onVolInput(idx, volSlider));
    updateSliderTrack(volSlider, 1.0);

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

// ── Stats: surgical DOM patch (no full re-render) ────────────
function applyStats(idx, raw) {
    const rx = receivers[idx]; if (!rx) return;
    const conn = rxConns[rx.host]; if (!conn) return;
    try {
        const d = typeof raw === 'string' ? JSON.parse(raw) : raw;
        conn.stats = d;
        if (d.volume !== undefined) conn.volume = d.volume;
        if (d.muted  !== undefined) conn.muted  = d.muted;
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
initCards();
localConnect();
