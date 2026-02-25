'use strict';

// --- Local WebSocket (this node) ---
let ws = null;
let reqId = 1;
const pending = new Map();
let meterInterval = null;
let statsInterval = null;

function connect() {
    const host = window.location.host || 'localhost:8400';
    ws = new WebSocket('ws://' + host + '/ws');
    ws.onopen = function() {
        document.getElementById('status').textContent = 'Connected';
        document.getElementById('status').className = 'status connected';
        refreshAll();
        startMeterUpdates();
        startStatsUpdates();
        pollTxStats();
    };
    ws.onclose = function() {
        document.getElementById('status').textContent = 'Disconnected';
        document.getElementById('status').className = 'status disconnected';
        stopMeterUpdates();
        stopStatsUpdates();
        setTimeout(connect, 2000);
    };
    ws.onmessage = function(evt) {
        try {
            const resp = JSON.parse(evt.data);
            const cb = pending.get(resp.id);
            if (cb) { pending.delete(resp.id); cb(resp); }
        } catch (e) {}
    };
}

function sendCommand(command, params, callback) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    const id = reqId++;
    if (callback) pending.set(id, callback);
    ws.send(JSON.stringify({ id, command, params: params || {} }));
}

// --- Tab Navigation ---
document.querySelectorAll('.tab').forEach(function(tab) {
    tab.addEventListener('click', function() {
        document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
        document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
        tab.classList.add('active');
        document.getElementById(tab.dataset.panel).classList.add('active');
    });
});

// --- TX Stats ---
function pollTxStats() {
    sendCommand('rx.stats', {}, function(resp) {
        if (!resp.success || !resp.data) return;
        try {
            const d = JSON.parse(resp.data);
            const dot = document.getElementById('tx-status-dot');
            const text = document.getElementById('tx-status-text');
            const pkts = document.getElementById('tx-packets');
            if (dot) dot.className = 'dot dot-on';
            if (text) text.textContent = d.muted ? 'Muted' : 'Active';
            if (pkts) pkts.textContent = fmtNum(d.packets);
        } catch (e) {}
    });
    setTimeout(pollTxStats, 2000);
}

// --- Receiver Management ---
const DEFAULT_RECEIVERS = [
    { name: 'RPi 1', host: 'soluna-rpi.local:8400' },
    { name: 'RPi 2', host: 'soluna-rpi2.local:8400' },
];

let receivers = loadReceivers();
const rxConns = {};  // host → { ws, stats, volume, muted }

function loadReceivers() {
    try {
        const s = localStorage.getItem('soluna_receivers');
        if (s) return JSON.parse(s);
    } catch (e) {}
    return DEFAULT_RECEIVERS.map(r => Object.assign({}, r));
}

function saveReceivers() {
    localStorage.setItem('soluna_receivers', JSON.stringify(receivers));
}

function renderReceivers() {
    const list = document.getElementById('receiver-list');
    list.innerHTML = '';
    receivers.forEach(function(rx, idx) {
        const card = document.createElement('div');
        card.className = 'rx-card card';
        card.id = 'rx-card-' + idx;

        const conn = rxConns[rx.host] || { connected: false, stats: null };
        const vol = conn.volume !== undefined ? conn.volume : 1.0;
        const muted = conn.muted || false;
        const volPct = Math.round(vol * 100);

        card.innerHTML =
            '<div class="card-header">' +
                '<div class="rx-title">' +
                    '<span class="dot ' + (conn.connected ? 'dot-on' : 'dot-off') + '"></span>' +
                    '<strong>' + escHtml(rx.name) + '</strong>' +
                    '<span class="rx-host">' + escHtml(rx.host) + '</span>' +
                '</div>' +
                '<button class="btn btn-icon btn-danger" onclick="removeReceiver(' + idx + ')" title="Remove">✕</button>' +
            '</div>' +
            '<div class="rx-stats">' +
                '<div class="rx-stat"><span class="label">Packets</span><span class="value" id="rx-pkts-' + idx + '">' + (conn.stats ? fmtNum(conn.stats.packets) : '—') + '</span></div>' +
                '<div class="rx-stat"><span class="label">Errors</span><span class="value ' + (conn.stats && conn.stats.errors > 0 ? 'warn' : '') + '" id="rx-errs-' + idx + '">' + (conn.stats ? conn.stats.errors : '—') + '</span></div>' +
                '<div class="rx-stat"><span class="label">Buffer</span><span class="value" id="rx-buf-' + idx + '">' + (conn.stats ? fmtBuf(conn.stats.buf_fill, conn.stats.buf_cap) : '—') + '</span></div>' +
            '</div>' +
            '<div class="rx-controls">' +
                '<button class="btn btn-mute ' + (muted ? 'active' : '') + '" id="rx-mute-' + idx + '" onclick="toggleMuteRx(' + idx + ')">' +
                    (muted ? '🔇 Muted' : '🔊 Live') +
                '</button>' +
                '<div class="volume-row">' +
                    '<span class="label">Vol</span>' +
                    '<input type="range" min="0" max="100" value="' + volPct + '" ' +
                        'class="volume-slider" id="rx-vol-' + idx + '" ' +
                        'oninput="setVolumeRx(' + idx + ', this.value)" ' +
                        'onchange="setVolumeRx(' + idx + ', this.value)">' +
                    '<span class="vol-label" id="rx-vol-label-' + idx + '">' + volPct + '%</span>' +
                '</div>' +
            '</div>';

        list.appendChild(card);
        connectReceiver(idx);
    });
}

function connectReceiver(idx) {
    const rx = receivers[idx];
    if (!rx) return;

    // Close existing
    if (rxConns[rx.host] && rxConns[rx.host].ws) {
        try { rxConns[rx.host].ws.close(); } catch (e) {}
    }

    const conn = { connected: false, ws: null, stats: null, volume: 1.0, muted: false, pingTimer: null, polling: null };
    rxConns[rx.host] = conn;

    const wsUrl = 'ws://' + rx.host + '/ws';
    let rxWs;
    try {
        rxWs = new WebSocket(wsUrl);
    } catch (e) {
        scheduleReconnect(idx);
        return;
    }
    conn.ws = rxWs;

    rxWs.onopen = function() {
        conn.connected = true;
        updateRxCard(idx);
        // Fetch initial stats & settings
        rxSend(idx, 'rx.stats', {}, function(resp) {
            if (resp.success) applyRxStats(idx, resp.data);
        });
        // Start polling
        conn.polling = setInterval(function() {
            rxSend(idx, 'rx.stats', {}, function(resp) {
                if (resp.success) applyRxStats(idx, resp.data);
            });
        }, 2000);
    };

    rxWs.onclose = function() {
        conn.connected = false;
        if (conn.polling) { clearInterval(conn.polling); conn.polling = null; }
        updateRxCard(idx);
        scheduleReconnect(idx);
    };

    const rxPending = new Map();
    let rxReqId = 1;
    conn.rxPending = rxPending;
    conn.rxReqId = function() { return rxReqId++; };

    rxWs.onmessage = function(evt) {
        try {
            const r = JSON.parse(evt.data);
            const cb = rxPending.get(r.id);
            if (cb) { rxPending.delete(r.id); cb(r); }
        } catch (e) {}
    };
}

function rxSend(idx, command, params, callback) {
    const rx = receivers[idx];
    if (!rx) return;
    const conn = rxConns[rx.host];
    if (!conn || !conn.ws || conn.ws.readyState !== WebSocket.OPEN) return;
    const id = conn.rxReqId();
    if (callback) conn.rxPending.set(id, callback);
    conn.ws.send(JSON.stringify({ id, command, params: params || {} }));
}

function applyRxStats(idx, dataStr) {
    const rx = receivers[idx];
    if (!rx) return;
    const conn = rxConns[rx.host];
    if (!conn) return;
    try {
        const d = typeof dataStr === 'string' ? JSON.parse(dataStr) : dataStr;
        conn.stats = d;
        if (d.volume !== undefined) conn.volume = d.volume;
        if (d.muted !== undefined) conn.muted = d.muted;
        updateRxStats(idx);
    } catch (e) {}
}

function updateRxStats(idx) {
    const rx = receivers[idx];
    if (!rx) return;
    const conn = rxConns[rx.host];
    if (!conn || !conn.stats) return;
    const s = conn.stats;

    const pkts = document.getElementById('rx-pkts-' + idx);
    const errs = document.getElementById('rx-errs-' + idx);
    const buf  = document.getElementById('rx-buf-' + idx);
    const muteBtn = document.getElementById('rx-mute-' + idx);
    const volSlider = document.getElementById('rx-vol-' + idx);
    const volLabel = document.getElementById('rx-vol-label-' + idx);

    if (pkts) pkts.textContent = fmtNum(s.packets);
    if (errs) {
        errs.textContent = s.errors;
        errs.className = 'value' + (s.errors > 0 ? ' warn' : ' good');
    }
    if (buf) buf.textContent = fmtBuf(s.buf_fill, s.buf_cap);
    if (muteBtn) {
        muteBtn.textContent = conn.muted ? '🔇 Muted' : '🔊 Live';
        muteBtn.className = 'btn btn-mute' + (conn.muted ? ' active' : '');
    }
    if (volSlider && document.activeElement !== volSlider) {
        volSlider.value = Math.round(conn.volume * 100);
    }
    if (volLabel) volLabel.textContent = Math.round(conn.volume * 100) + '%';
}

function updateRxCard(idx) {
    const rx = receivers[idx];
    if (!rx) return;
    const conn = rxConns[rx.host];
    const card = document.getElementById('rx-card-' + idx);
    if (!card) return;
    const dot = card.querySelector('.dot');
    if (dot) dot.className = 'dot ' + (conn && conn.connected ? 'dot-on' : 'dot-off');
}

function scheduleReconnect(idx) {
    setTimeout(function() { connectReceiver(idx); }, 5000);
}

function toggleMuteRx(idx) {
    const rx = receivers[idx];
    if (!rx) return;
    const conn = rxConns[rx.host];
    if (!conn) return;
    const newMuted = !conn.muted;
    rxSend(idx, 'rx.set_mute', { muted: newMuted ? 'true' : 'false' }, function(resp) {
        if (resp.success) {
            conn.muted = newMuted;
            updateRxStats(idx);
        }
    });
}

function setVolumeRx(idx, pct) {
    const vol = parseInt(pct) / 100;
    const rx = receivers[idx];
    if (!rx) return;
    const conn = rxConns[rx.host];
    if (!conn) return;
    conn.volume = vol;
    const label = document.getElementById('rx-vol-label-' + idx);
    if (label) label.textContent = Math.round(vol * 100) + '%';
    rxSend(idx, 'rx.set_volume', { volume: String(vol.toFixed(3)) });
}

function addReceiver(name, host) {
    receivers.push({ name, host });
    saveReceivers();
    renderReceivers();
}

function removeReceiver(idx) {
    const rx = receivers[idx];
    if (rx && rxConns[rx.host]) {
        const conn = rxConns[rx.host];
        if (conn.polling) clearInterval(conn.polling);
        if (conn.ws) try { conn.ws.close(); } catch (e) {}
        delete rxConns[rx.host];
    }
    receivers.splice(idx, 1);
    saveReceivers();
    renderReceivers();
}

document.getElementById('btn-add-receiver').addEventListener('click', function() {
    const name = prompt('Receiver name:', 'RPi 3');
    if (!name) return;
    const host = prompt('Host (e.g. soluna-rpi3.local:8400):', 'soluna-rpi3.local:8400');
    if (!host) return;
    addReceiver(name, host);
});

// --- Device List ---
function refreshDevices() {
    sendCommand('device.list', {}, function(resp) {
        var list = document.getElementById('device-list');
        list.innerHTML = '';
        if (!resp.success || !resp.data) return;
        try {
            JSON.parse(resp.data).forEach(function(d) {
                var card = document.createElement('div');
                card.className = 'card';
                card.innerHTML = '<h3>' + escHtml(d.name) + '</h3>' +
                    '<div class="detail">Host: ' + escHtml(d.host) + '</div>' +
                    '<div class="detail">In: ' + d.inputs + ' / Out: ' + d.outputs + '</div>' +
                    '<div class="detail">' + (d.local ? 'Local' : 'Remote') + '</div>';
                list.appendChild(card);
            });
        } catch (e) {}
    });
}

// --- Routes ---
function refreshRoutes() {
    sendCommand('route.list', {}, function(resp) {
        var tbody = document.querySelector('#route-table tbody');
        tbody.innerHTML = '';
        if (!resp.success || !resp.data) return;
        try {
            JSON.parse(resp.data).forEach(function(r) {
                var tr = document.createElement('tr');
                tr.innerHTML = '<td>' + escHtml(r.source) + '</td>' +
                    '<td>' + escHtml(r.sink) + '</td>' +
                    '<td>' + r.gain_db.toFixed(1) + '</td>' +
                    '<td>' + (r.muted ? 'Yes' : 'No') + '</td>' +
                    '<td>' +
                    '<button class="btn btn-small" onclick="toggleMute(\'' + escAttr(r.source) + '\',\'' + escAttr(r.sink) + '\',' + !r.muted + ')">Mute</button> ' +
                    '<button class="btn btn-danger btn-small" onclick="removeRoute(\'' + escAttr(r.source) + '\',\'' + escAttr(r.sink) + '\')">Remove</button>' +
                    '</td>';
                tbody.appendChild(tr);
            });
        } catch (e) {}
    });
}

function removeRoute(src, dst) {
    sendCommand('route.remove', { source: src, sink: dst }, refreshRoutes);
}
function toggleMute(src, dst, muted) {
    sendCommand('route.set_mute', { source: src, sink: dst, muted: String(muted) }, refreshRoutes);
}

document.getElementById('btn-route-add').addEventListener('click', function() {
    var src = document.getElementById('route-src').value;
    var dst = document.getElementById('route-dst').value;
    var gain = document.getElementById('route-gain').value;
    if (src && dst) {
        sendCommand('route.add', { source: src, sink: dst, gain_db: gain || '0' }, function() {
            refreshRoutes();
            document.getElementById('route-src').value = '';
            document.getElementById('route-dst').value = '';
            document.getElementById('route-gain').value = '0';
        });
    }
});

// --- Level Meters ---
function startMeterUpdates() {
    if (meterInterval) return;
    meterInterval = setInterval(refreshMeters, 100);
}
function stopMeterUpdates() {
    if (meterInterval) { clearInterval(meterInterval); meterInterval = null; }
}
function refreshMeters() {
    if (!document.getElementById('meter-auto-refresh').checked) return;
    sendCommand('meter.get_all', {}, function(resp) {
        var display = document.getElementById('meter-display');
        var status = document.getElementById('meter-status');
        if (!resp.success || !resp.data) { if (status) status.textContent = 'No data'; return; }
        try {
            var channels = JSON.parse(resp.data).channels || [];
            if (status) status.textContent = channels.length + ' channels';
            display.innerHTML = '';
            channels.forEach(function(ch) {
                var peakPct = ((Math.max(-60, Math.min(0, ch.peak_db)) + 60) / 60) * 100;
                var rmsPct  = ((Math.max(-60, Math.min(0, ch.rms_db))  + 60) / 60) * 100;
                var card = document.createElement('div');
                card.className = 'meter-card';
                card.innerHTML =
                    '<h4>' + escHtml(ch.channel) + '</h4>' +
                    '<div class="meter-bar"><div class="fill" style="width:' + rmsPct + '%"></div></div>' +
                    '<div class="meter-values">' +
                    '<span>RMS: ' + ch.rms_db.toFixed(1) + ' dB</span>' +
                    '<span class="meter-peak">Peak: ' + ch.peak_db.toFixed(1) + ' dB</span>' +
                    (ch.clip_count > 0 ? '<span class="meter-clip">Clips: ' + ch.clip_count + '</span>' : '') +
                    '</div>';
                display.appendChild(card);
            });
        } catch (e) {}
    });
}

// --- System Stats ---
function startStatsUpdates() {
    if (statsInterval) return;
    refreshStats();
    statsInterval = setInterval(refreshStats, 2000);
}
function stopStatsUpdates() {
    if (statsInterval) { clearInterval(statsInterval); statsInterval = null; }
}
function refreshStats() {
    sendCommand('system.stats', {}, function(resp) {
        if (!resp.success || !resp.data) return;
        try {
            var s = JSON.parse(resp.data);
            setVal('stat-devices', s.device_count || 0);
            setVal('stat-streams', (s.active_streams || 0) + '/' + (s.stream_count || 0));
            setVal('stat-routes', s.route_count || 0);
            var bps = s.bandwidth_bps || 0;
            setVal('stat-bandwidth', bps < 1e6 ? (bps/1000).toFixed(1)+' kbps' : (bps/1e6).toFixed(2)+' Mbps');
            var ptpEl = document.getElementById('stat-ptp');
            if (ptpEl) { ptpEl.textContent = s.ptp_synced ? 'Synced' : 'Not synced'; ptpEl.className = 'stat-value '+(s.ptp_synced?'good':'warn'); }
            setVal('stat-ptp-role', s.ptp_role || 'unknown');
            var oNs = s.ptp_offset_ns || 0;
            setVal('stat-ptp-offset', Math.abs(oNs)<1000 ? oNs+' ns' : (oNs/1000).toFixed(1)+' µs');
            var dNs = s.ptp_path_delay_ns || 0;
            setVal('stat-ptp-delay', Math.abs(dNs)<1000 ? dNs+' ns' : (dNs/1000).toFixed(1)+' µs');
        } catch (e) {}
    });
}

// --- Helpers ---
function refreshAll() { refreshDevices(); refreshRoutes(); renderReceivers(); }
function setVal(id, v) { var e = document.getElementById(id); if (e) e.textContent = v; }
function fmtNum(n) { return n >= 1e6 ? (n/1e6).toFixed(1)+'M' : n >= 1e3 ? (n/1e3).toFixed(1)+'k' : String(n||0); }
function fmtBuf(fill, cap) { if (!cap) return '—'; return fill + '/' + cap + ' (' + Math.round(fill/cap*100) + '%)'; }
function escHtml(s) { var d = document.createElement('div'); d.appendChild(document.createTextNode(s||'')); return d.innerHTML; }
function escAttr(s) { return (s||'').replace(/'/g,"\\'").replace(/"/g,'&quot;'); }

connect();
setInterval(refreshAll, 10000);
