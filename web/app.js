'use strict';

// --- WebSocket Connection ---
let ws = null;
let reqId = 1;
const pending = new Map();

function connect() {
    const host = window.location.host || 'localhost:8400';
    ws = new WebSocket('ws://' + host + '/ws');

    ws.onopen = function() {
        document.getElementById('status').textContent = 'Connected';
        document.getElementById('status').className = 'status connected';
        refreshAll();
    };

    ws.onclose = function() {
        document.getElementById('status').textContent = 'Disconnected';
        document.getElementById('status').className = 'status disconnected';
        setTimeout(connect, 2000);
    };

    ws.onmessage = function(evt) {
        try {
            const resp = JSON.parse(evt.data);
            const cb = pending.get(resp.id);
            if (cb) {
                pending.delete(resp.id);
                cb(resp);
            }
        } catch (e) {
            console.error('Parse error:', e);
        }
    };
}

function sendCommand(command, params, callback) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    const id = reqId++;
    const req = { id: id, command: command, params: params || {} };
    if (callback) pending.set(id, callback);
    ws.send(JSON.stringify(req));
}

// --- Tab Navigation ---
document.querySelectorAll('.tab').forEach(function(tab) {
    tab.addEventListener('click', function() {
        document.querySelectorAll('.tab').forEach(function(t) { t.classList.remove('active'); });
        document.querySelectorAll('.panel').forEach(function(p) { p.classList.remove('active'); });
        tab.classList.add('active');
        document.getElementById(tab.dataset.panel).classList.add('active');
    });
});

// --- Device List ---
function refreshDevices() {
    sendCommand('device.list', {}, function(resp) {
        var list = document.getElementById('device-list');
        list.innerHTML = '';
        if (!resp.success || !resp.data) return;
        try {
            var devices = JSON.parse(resp.data);
            devices.forEach(function(d) {
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

// --- Stream List ---
function refreshStreams() {
    sendCommand('stream.list', {}, function(resp) {
        var list = document.getElementById('stream-list');
        list.innerHTML = '';
        if (!resp.success || !resp.data) return;
        try {
            var streams = JSON.parse(resp.data);
            streams.forEach(function(s) {
                var card = document.createElement('div');
                card.className = 'card';
                card.innerHTML = '<h3>Stream #' + s.id + '</h3>' +
                    '<div class="detail">' + escHtml(s.source) + ' → ' + escHtml(s.sink) + '</div>' +
                    '<div class="detail">Channels: ' + s.channels + ' | Port: ' + s.port + '</div>' +
                    '<div class="detail">State: ' + escHtml(s.state) + '</div>' +
                    '<button class="btn btn-danger btn-small" onclick="destroyStream(' + s.id + ')">Delete</button>';
                list.appendChild(card);
            });
        } catch (e) {}
    });
}

function destroyStream(id) {
    sendCommand('stream.destroy', { stream_id: String(id) }, function() {
        refreshStreams();
    });
}

document.getElementById('btn-stream-create').addEventListener('click', function() {
    var src = prompt('Source device:');
    var dst = prompt('Sink device:');
    var ch = prompt('Channels:', '2');
    if (src && dst) {
        sendCommand('stream.create', { source: src, sink: dst, channels: ch || '2' }, function() {
            refreshStreams();
        });
    }
});

// --- Route List ---
function refreshRoutes() {
    sendCommand('route.list', {}, function(resp) {
        var tbody = document.querySelector('#route-table tbody');
        tbody.innerHTML = '';
        if (!resp.success || !resp.data) return;
        try {
            var routes = JSON.parse(resp.data);
            routes.forEach(function(r) {
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
    sendCommand('route.remove', { source: src, sink: dst }, function() {
        refreshRoutes();
    });
}

function toggleMute(src, dst, muted) {
    sendCommand('route.set_mute', { source: src, sink: dst, muted: String(muted) }, function() {
        refreshRoutes();
    });
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

// --- Refresh All ---
function refreshAll() {
    refreshDevices();
    refreshStreams();
    refreshRoutes();
}

// --- Helpers ---
function escHtml(s) {
    var div = document.createElement('div');
    div.appendChild(document.createTextNode(s || ''));
    return div.innerHTML;
}
function escAttr(s) {
    return (s || '').replace(/'/g, "\\'").replace(/"/g, '&quot;');
}

// --- Start ---
connect();
setInterval(refreshAll, 5000);
