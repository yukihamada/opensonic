'use strict';

// --- WebSocket Connection ---
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
let deviceCache = [];

function refreshDevices() {
    sendCommand('device.list', {}, function(resp) {
        var list = document.getElementById('device-list');
        list.innerHTML = '';
        if (!resp.success || !resp.data) return;
        try {
            deviceCache = JSON.parse(resp.data);
            deviceCache.forEach(function(d) {
                var card = document.createElement('div');
                card.className = 'card';
                card.innerHTML = '<h3>' + escHtml(d.name) + '</h3>' +
                    '<div class="detail">Host: ' + escHtml(d.host) + '</div>' +
                    '<div class="detail">In: ' + d.inputs + ' / Out: ' + d.outputs + '</div>' +
                    '<div class="detail">' + (d.local ? 'Local' : 'Remote') + '</div>';
                list.appendChild(card);
            });
            updateRoutingMatrix();
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

// --- Route List (Table View) ---
let routeCache = [];

function refreshRoutes() {
    sendCommand('route.list', {}, function(resp) {
        var tbody = document.querySelector('#route-table tbody');
        tbody.innerHTML = '';
        if (!resp.success || !resp.data) return;
        try {
            routeCache = JSON.parse(resp.data);
            routeCache.forEach(function(r) {
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
            updateRoutingMatrix();
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

// --- Visual Routing Matrix ---
function updateRoutingMatrix() {
    var container = document.getElementById('routing-matrix');
    container.innerHTML = '';

    if (deviceCache.length === 0) {
        container.innerHTML = '<p class="detail">No devices available</p>';
        return;
    }

    // Collect all input and output channels
    var inputs = [];
    var outputs = [];

    deviceCache.forEach(function(d) {
        for (var i = 0; i < d.inputs; i++) {
            inputs.push(d.name + ':' + (i + 1));
        }
        for (var i = 0; i < d.outputs; i++) {
            outputs.push(d.name + ':' + (i + 1));
        }
    });

    if (inputs.length === 0 || outputs.length === 0) {
        container.innerHTML = '<p class="detail">No channels available</p>';
        return;
    }

    // Build route lookup
    var routeMap = {};
    routeCache.forEach(function(r) {
        routeMap[r.source + '|' + r.sink] = r;
    });

    // Create table
    var table = document.createElement('table');

    // Header row
    var thead = document.createElement('thead');
    var headerRow = document.createElement('tr');
    headerRow.innerHTML = '<th></th>';
    outputs.forEach(function(o) {
        var th = document.createElement('th');
        th.textContent = o.split(':').pop();
        th.title = o;
        headerRow.appendChild(th);
    });
    thead.appendChild(headerRow);
    table.appendChild(thead);

    // Body
    var tbody = document.createElement('tbody');
    inputs.forEach(function(inp) {
        var row = document.createElement('tr');
        var rowHeader = document.createElement('th');
        rowHeader.className = 'row-header';
        rowHeader.textContent = inp;
        rowHeader.title = inp;
        row.appendChild(rowHeader);

        outputs.forEach(function(out) {
            var cell = document.createElement('td');
            cell.className = 'cell';
            var key = inp + '|' + out;
            var route = routeMap[key];

            if (route) {
                if (route.muted) {
                    cell.classList.add('muted');
                    cell.textContent = 'M';
                } else {
                    cell.classList.add('active');
                    cell.textContent = route.gain_db.toFixed(0);
                }
            } else {
                cell.classList.add('empty');
            }

            cell.dataset.source = inp;
            cell.dataset.sink = out;
            cell.onclick = function() { toggleMatrixCell(inp, out, route); };
            row.appendChild(cell);
        });

        tbody.appendChild(row);
    });
    table.appendChild(tbody);
    container.appendChild(table);
}

function toggleMatrixCell(src, dst, existingRoute) {
    if (existingRoute) {
        // Route exists - toggle mute or remove
        if (existingRoute.muted) {
            sendCommand('route.set_mute', { source: src, sink: dst, muted: 'false' }, refreshRoutes);
        } else {
            var action = confirm('Route exists.\n\nOK = Toggle Mute\nCancel = Remove Route');
            if (action) {
                sendCommand('route.set_mute', { source: src, sink: dst, muted: 'true' }, refreshRoutes);
            } else {
                sendCommand('route.remove', { source: src, sink: dst }, refreshRoutes);
            }
        }
    } else {
        // Create new route
        var gain = prompt('Create route ' + src + ' → ' + dst + '\n\nGain (dB):', '0');
        if (gain !== null) {
            sendCommand('route.add', { source: src, sink: dst, gain_db: gain || '0' }, refreshRoutes);
        }
    }
}

// --- Level Meters ---
function startMeterUpdates() {
    if (meterInterval) return;
    meterInterval = setInterval(refreshMeters, 100);
}

function stopMeterUpdates() {
    if (meterInterval) {
        clearInterval(meterInterval);
        meterInterval = null;
    }
}

function refreshMeters() {
    var checkbox = document.getElementById('meter-auto-refresh');
    if (!checkbox || !checkbox.checked) return;

    sendCommand('meter.get_all', {}, function(resp) {
        var display = document.getElementById('meter-display');
        var status = document.getElementById('meter-status');

        if (!resp.success || !resp.data) {
            if (status) status.textContent = 'No data';
            return;
        }

        try {
            var data = JSON.parse(resp.data);
            var channels = data.channels || [];

            if (status) status.textContent = channels.length + ' channels';

            // Build meter display
            display.innerHTML = '';
            channels.forEach(function(ch) {
                var card = document.createElement('div');
                card.className = 'meter-card';

                // Calculate meter percentage (0-100 from -60 to 0 dB)
                var peakDb = Math.max(-60, Math.min(0, ch.peak_db));
                var rmsDb = Math.max(-60, Math.min(0, ch.rms_db));
                var peakPct = ((peakDb + 60) / 60) * 100;
                var rmsPct = ((rmsDb + 60) / 60) * 100;

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
        } catch (e) {
            if (status) status.textContent = 'Error';
        }
    });
}

// --- System Stats ---
function startStatsUpdates() {
    if (statsInterval) return;
    refreshStats();
    statsInterval = setInterval(refreshStats, 2000);
}

function stopStatsUpdates() {
    if (statsInterval) {
        clearInterval(statsInterval);
        statsInterval = null;
    }
}

function refreshStats() {
    sendCommand('system.stats', {}, function(resp) {
        if (!resp.success || !resp.data) return;

        try {
            var stats = JSON.parse(resp.data);

            setStatValue('stat-devices', stats.device_count || 0);
            setStatValue('stat-streams', (stats.active_streams || 0) + '/' + (stats.stream_count || 0));
            setStatValue('stat-routes', stats.route_count || 0);

            // Format bandwidth
            var bps = stats.bandwidth_bps || 0;
            var bwStr = bps < 1000000
                ? (bps / 1000).toFixed(1) + ' kbps'
                : (bps / 1000000).toFixed(2) + ' Mbps';
            setStatValue('stat-bandwidth', bwStr);

            // PTP status
            var ptpEl = document.getElementById('stat-ptp');
            if (ptpEl) {
                ptpEl.textContent = stats.ptp_synced ? 'Synced' : 'Not synced';
                ptpEl.className = 'stat-value ' + (stats.ptp_synced ? 'good' : 'warn');
            }

            // PTP role
            var roleEl = document.getElementById('stat-ptp-role');
            if (roleEl) {
                var role = stats.ptp_role || 'unknown';
                roleEl.textContent = role.charAt(0).toUpperCase() + role.slice(1);
                roleEl.className = 'stat-value ' + (role === 'master' ? 'good' : role === 'slave' ? 'info' : '');
            }

            // PTP offset
            var offsetNs = stats.ptp_offset_ns || 0;
            var offsetStr = Math.abs(offsetNs) < 1000
                ? offsetNs + ' ns'
                : (offsetNs / 1000).toFixed(1) + ' µs';
            setStatValue('stat-ptp-offset', offsetStr);

            // PTP path delay
            var delayNs = stats.ptp_path_delay_ns || 0;
            var delayStr = Math.abs(delayNs) < 1000
                ? delayNs + ' ns'
                : (delayNs / 1000).toFixed(1) + ' µs';
            setStatValue('stat-ptp-delay', delayStr);

            // PTP sync count
            setStatValue('stat-ptp-sync-count', stats.ptp_sync_count || 0);

        } catch (e) {}
    });
}

function setStatValue(id, value) {
    var el = document.getElementById(id);
    if (el) el.textContent = value;
}

// --- Presets ---
function refreshPresets() {
    sendCommand('preset.list', {}, function(resp) {
        var list = document.getElementById('preset-list');
        list.innerHTML = '';

        if (!resp.success || !resp.data) {
            list.innerHTML = '<p class="detail">No presets saved</p>';
            return;
        }

        try {
            var data = JSON.parse(resp.data);
            var presets = data.presets || [];

            if (presets.length === 0) {
                list.innerHTML = '<p class="detail">No presets saved</p>';
                return;
            }

            presets.forEach(function(p) {
                var card = document.createElement('div');
                card.className = 'preset-card';
                card.innerHTML =
                    '<h4>' + escHtml(p.name) + '</h4>' +
                    '<div class="preset-actions">' +
                    '<button class="btn btn-small" onclick="loadPreset(\'' + escAttr(p.name) + '\')">Load</button>' +
                    '<button class="btn btn-danger btn-small" onclick="deletePreset(\'' + escAttr(p.name) + '\')">Delete</button>' +
                    '</div>';
                list.appendChild(card);
            });
        } catch (e) {
            list.innerHTML = '<p class="detail">Error loading presets</p>';
        }
    });
}

function loadPreset(name) {
    if (confirm('Load preset "' + name + '"?\n\nThis will replace current routing.')) {
        sendCommand('preset.load', { name: name }, function(resp) {
            if (resp.success) {
                refreshRoutes();
                alert('Preset loaded');
            } else {
                alert('Error: ' + (resp.error || 'Unknown'));
            }
        });
    }
}

function deletePreset(name) {
    if (confirm('Delete preset "' + name + '"?')) {
        sendCommand('preset.delete', { name: name }, function(resp) {
            refreshPresets();
        });
    }
}

document.getElementById('btn-preset-save').addEventListener('click', function() {
    var name = document.getElementById('preset-name').value.trim();
    if (!name) {
        alert('Enter a preset name');
        return;
    }

    sendCommand('preset.save', { name: name }, function(resp) {
        if (resp.success) {
            document.getElementById('preset-name').value = '';
            refreshPresets();
            alert('Preset saved');
        } else {
            alert('Error: ' + (resp.error || 'Unknown'));
        }
    });
});

// --- Security ---
function refreshSecurityStatus() {
    sendCommand('security.status', {}, function(resp) {
        if (!resp.success || !resp.data) return;

        try {
            var sec = JSON.parse(resp.data);

            // DTLS Available
            var availEl = document.getElementById('sec-dtls-available');
            if (availEl) {
                availEl.textContent = sec.dtls_available ? 'Yes' : 'No';
                availEl.className = 'stat-value ' + (sec.dtls_available ? 'good' : 'warn');
            }

            // DTLS Enabled
            var enabledEl = document.getElementById('sec-dtls-enabled');
            if (enabledEl) {
                enabledEl.textContent = sec.dtls_enabled ? 'Yes' : 'No';
                enabledEl.className = 'stat-value ' + (sec.dtls_enabled ? 'good' : '');
            }

            // Certificate status
            var certEl = document.getElementById('sec-has-cert');
            if (certEl) {
                certEl.textContent = sec.has_certificate ? 'Loaded' : 'Not loaded';
                certEl.className = 'stat-value ' + (sec.has_certificate ? 'good' : 'warn');
            }

            // Populate form fields
            var checkbox = document.getElementById('sec-enable-dtls');
            if (checkbox) checkbox.checked = sec.dtls_enabled;

            if (sec.cert_path) {
                document.getElementById('sec-cert-path').value = sec.cert_path;
            }
            if (sec.key_path) {
                document.getElementById('sec-key-path').value = sec.key_path;
            }

            // Certificate info
            var infoEl = document.getElementById('cert-info');
            if (infoEl) {
                if (sec.has_certificate && sec.cert_subject) {
                    infoEl.innerHTML =
                        '<p><strong>Subject:</strong> ' + escHtml(sec.cert_subject) + '</p>' +
                        '<p><strong>Expires:</strong> ' + escHtml(sec.cert_expiry || 'Unknown') + '</p>';
                } else if (!sec.dtls_available) {
                    infoEl.textContent = 'DTLS not available (build without OpenSSL)';
                } else {
                    infoEl.textContent = 'No certificate loaded';
                }
            }
        } catch (e) {}
    });
}

document.getElementById('btn-security-apply').addEventListener('click', function() {
    var enabled = document.getElementById('sec-enable-dtls').checked;
    var certPath = document.getElementById('sec-cert-path').value.trim();
    var keyPath = document.getElementById('sec-key-path').value.trim();

    var params = {
        enabled: enabled ? 'true' : 'false'
    };
    if (certPath) params.cert_path = certPath;
    if (keyPath) params.key_path = keyPath;

    sendCommand('security.set_dtls', params, function(resp) {
        if (resp.success) {
            refreshSecurityStatus();
            alert('Security settings applied');
        } else {
            alert('Error: ' + (resp.error || 'Unknown'));
        }
    });
});

// --- Refresh All ---
function refreshAll() {
    refreshDevices();
    refreshStreams();
    refreshRoutes();
    refreshPresets();
    refreshSecurityStatus();
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
