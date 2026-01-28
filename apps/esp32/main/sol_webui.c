/**
 * Soluna ESP32 — Built-in Web UI Implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include "sol_webui.h"

#ifdef ESP_PLATFORM

#include <esp_log.h>
#include <esp_http_server.h>
#include <cJSON.h>
#include <string.h>
#include <stdio.h>

static const char* TAG = "sol_webui";

static httpd_handle_t s_server = NULL;
static sol_webui_config_t s_webui_config = {0};

// Embedded HTML (minified, gzipped in production)
static const char* INDEX_HTML = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Soluna</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;background:#1a1a2e;color:#eef;line-height:1.6;padding:16px}
.container{max-width:600px;margin:0 auto}
h1{font-size:24px;margin-bottom:16px;color:#00d9ff}
h2{font-size:18px;margin:24px 0 12px;color:#a0a0ff;border-bottom:1px solid #333;padding-bottom:8px}
.card{background:#252547;border-radius:8px;padding:16px;margin-bottom:16px}
.status-grid{display:grid;grid-template-columns:repeat(2,1fr);gap:12px}
.stat{text-align:center}
.stat-value{font-size:24px;font-weight:bold;color:#00d9ff}
.stat-label{font-size:12px;color:#888}
.synced{color:#0f0}.unsynced{color:#f55}
label{display:block;margin-bottom:4px;color:#aaa;font-size:14px}
input,select{width:100%;padding:10px;margin-bottom:12px;border:1px solid #444;border-radius:4px;background:#1a1a2e;color:#eef;font-size:16px}
input:focus,select:focus{outline:none;border-color:#00d9ff}
.btn{display:inline-block;padding:12px 24px;border:none;border-radius:4px;font-size:16px;cursor:pointer;margin-right:8px;margin-bottom:8px}
.btn-primary{background:#00d9ff;color:#000}
.btn-danger{background:#ff4444;color:#fff}
.btn-secondary{background:#444;color:#fff}
.btn:hover{opacity:0.9}
.hidden{display:none}
.alert{padding:12px;border-radius:4px;margin-bottom:16px}
.alert-success{background:#0a4;color:#fff}
.alert-error{background:#a00;color:#fff}
#msg{position:fixed;top:16px;right:16px;z-index:100}
</style>
</head>
<body>
<div class="container">
<h1>Soluna</h1>
<div id="msg"></div>

<div class="card">
<h2>Status</h2>
<div class="status-grid">
<div class="stat"><div class="stat-value" id="ptp-status">--</div><div class="stat-label">PTP Sync</div></div>
<div class="stat"><div class="stat-value" id="ptp-offset">-- ns</div><div class="stat-label">Clock Offset</div></div>
<div class="stat"><div class="stat-value" id="packets">0</div><div class="stat-label">Packets RX</div></div>
<div class="stat"><div class="stat-value" id="lost">0</div><div class="stat-label">Lost</div></div>
<div class="stat"><div class="stat-value" id="buffer">-- ms</div><div class="stat-label">Buffer</div></div>
<div class="stat"><div class="stat-value" id="rssi">-- dBm</div><div class="stat-label">WiFi RSSI</div></div>
</div>
</div>

<div class="card">
<h2>Device Settings</h2>
<form id="config-form">
<label>Device Name</label>
<input type="text" id="device_name" maxlength="31">

<label>Mode</label>
<select id="mode">
<option value="0">Receiver (RX)</option>
<option value="1">Transmitter (TX)</option>
</select>

<label>Channels</label>
<select id="channels">
<option value="1">Mono</option>
<option value="2">Stereo</option>
</select>

<label>Sample Rate</label>
<select id="sample_rate">
<option value="44100">44.1 kHz</option>
<option value="48000">48 kHz</option>
<option value="96000">96 kHz</option>
</select>

<label>FEC Level</label>
<select id="fec_level">
<option value="0">Off</option>
<option value="1">Low</option>
<option value="2">Medium</option>
<option value="3">High</option>
</select>

<label>Buffer (ms)</label>
<input type="number" id="buffer_ms" min="5" max="200">
</form>
</div>

<div class="card">
<h2>WiFi Settings</h2>
<label>SSID</label>
<input type="text" id="wifi_ssid" maxlength="31">

<label>Password</label>
<input type="password" id="wifi_pass" maxlength="63">
</div>

<div class="card">
<h2>Actions</h2>
<button class="btn btn-primary" onclick="saveConfig()">Save Settings</button>
<button class="btn btn-secondary" onclick="reboot()">Reboot</button>
<button class="btn btn-danger" onclick="factoryReset()">Factory Reset</button>
</div>

<div class="card">
<h2>Firmware Update</h2>
<input type="file" id="ota-file" accept=".bin">
<button class="btn btn-primary" onclick="uploadOta()">Upload Firmware</button>
<div id="ota-progress" class="hidden">
<progress id="ota-bar" value="0" max="100" style="width:100%;height:20px"></progress>
<span id="ota-percent">0%</span>
</div>
</div>

<p style="text-align:center;color:#555;margin-top:24px;font-size:12px">
Soluna v1.0 &bull; <span id="uptime">--</span> uptime &bull; <span id="ip">--</span>
</p>
</div>

<script>
let config = {};

function msg(text, isError) {
  const m = document.getElementById('msg');
  m.innerHTML = '<div class="alert '+(isError?'alert-error':'alert-success')+'">'+text+'</div>';
  setTimeout(() => m.innerHTML = '', 3000);
}

async function fetchStatus() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    document.getElementById('ptp-status').textContent = d.ptp_synced ? 'Synced' : 'Not Synced';
    document.getElementById('ptp-status').className = 'stat-value ' + (d.ptp_synced ? 'synced' : 'unsynced');
    document.getElementById('ptp-offset').textContent = d.ptp_offset_ns + ' ns';
    document.getElementById('packets').textContent = d.packets_received;
    document.getElementById('lost').textContent = d.packets_lost;
    document.getElementById('buffer').textContent = d.buffer_level_ms + ' ms';
    document.getElementById('rssi').textContent = d.rssi + ' dBm';
    document.getElementById('uptime').textContent = formatUptime(d.uptime_sec);
    document.getElementById('ip').textContent = d.ip_address;
  } catch(e) {}
}

function formatUptime(sec) {
  const h = Math.floor(sec/3600);
  const m = Math.floor((sec%3600)/60);
  const s = sec%60;
  return h+'h '+m+'m '+s+'s';
}

async function loadConfig() {
  try {
    const r = await fetch('/api/config');
    config = await r.json();
    document.getElementById('device_name').value = config.device_name || '';
    document.getElementById('mode').value = config.mode || 0;
    document.getElementById('channels').value = config.channels || 2;
    document.getElementById('sample_rate').value = config.sample_rate || 48000;
    document.getElementById('fec_level').value = config.fec_level || 1;
    document.getElementById('buffer_ms').value = config.buffer_ms || 20;
    document.getElementById('wifi_ssid').value = config.wifi_ssid || '';
    document.getElementById('wifi_pass').value = '';
  } catch(e) { msg('Failed to load config', true); }
}

async function saveConfig() {
  const data = {
    device_name: document.getElementById('device_name').value,
    mode: parseInt(document.getElementById('mode').value),
    channels: parseInt(document.getElementById('channels').value),
    sample_rate: parseInt(document.getElementById('sample_rate').value),
    fec_level: parseInt(document.getElementById('fec_level').value),
    buffer_ms: parseInt(document.getElementById('buffer_ms').value),
    wifi_ssid: document.getElementById('wifi_ssid').value,
    wifi_pass: document.getElementById('wifi_pass').value,
  };
  try {
    const r = await fetch('/api/config', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(data)});
    if(r.ok) msg('Settings saved'); else msg('Save failed', true);
  } catch(e) { msg('Save failed', true); }
}

async function reboot() {
  if(!confirm('Reboot device?')) return;
  try {
    await fetch('/api/reboot', {method:'POST'});
    msg('Rebooting...');
  } catch(e) {}
}

async function factoryReset() {
  if(!confirm('Factory reset? All settings will be erased.')) return;
  try {
    await fetch('/api/factory-reset', {method:'POST'});
    msg('Factory reset complete, rebooting...');
  } catch(e) {}
}

async function uploadOta() {
  const file = document.getElementById('ota-file').files[0];
  if(!file) { msg('Select firmware file', true); return; }

  document.getElementById('ota-progress').classList.remove('hidden');
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota');
  xhr.upload.onprogress = (e) => {
    const pct = Math.round(e.loaded/e.total*100);
    document.getElementById('ota-bar').value = pct;
    document.getElementById('ota-percent').textContent = pct+'%';
  };
  xhr.onload = () => {
    if(xhr.status === 200) msg('Update complete, rebooting...');
    else msg('Update failed: '+xhr.responseText, true);
  };
  xhr.onerror = () => msg('Upload failed', true);
  xhr.send(file);
}

loadConfig();
fetchStatus();
setInterval(fetchStatus, 2000);
</script>
</body>
</html>
)rawliteral";

// GET / - Serve main page
static esp_err_t handler_index(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
    return ESP_OK;
}

// GET /api/status - Get device status
static esp_err_t handler_status(httpd_req_t* req) {
    sol_webui_status_t status = {0};

    if (s_webui_config.status_cb) {
        s_webui_config.status_cb(&status);
    }

    cJSON* json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ptp_synced", status.ptp_synced);
    cJSON_AddNumberToObject(json, "ptp_offset_ns", (double)status.ptp_offset_ns);
    cJSON_AddNumberToObject(json, "packets_received", status.packets_received);
    cJSON_AddNumberToObject(json, "packets_lost", status.packets_lost);
    cJSON_AddNumberToObject(json, "buffer_level_ms", status.buffer_level_ms);
    cJSON_AddNumberToObject(json, "rssi", status.rssi);
    cJSON_AddNumberToObject(json, "uptime_sec", status.uptime_sec);
    cJSON_AddStringToObject(json, "ip_address", status.ip_address ? status.ip_address : "");

    char* response = cJSON_PrintUnformatted(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));

    free(response);
    cJSON_Delete(json);
    return ESP_OK;
}

// GET /api/config - Get configuration
static esp_err_t handler_config_get(httpd_req_t* req) {
    if (!s_webui_config.config) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No config");
        return ESP_FAIL;
    }

    sol_config_t* cfg = s_webui_config.config;

    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "device_name", cfg->device_name);
    cJSON_AddNumberToObject(json, "mode", cfg->mode);
    cJSON_AddNumberToObject(json, "channels", cfg->channels);
    cJSON_AddNumberToObject(json, "rtp_port", cfg->rtp_port);
    cJSON_AddBoolToObject(json, "fec_enabled", cfg->fec_enabled);
    cJSON_AddNumberToObject(json, "target_latency_ms", cfg->target_latency_ms);
    cJSON_AddStringToObject(json, "wifi_ssid", cfg->wifi_ssid);
    // Don't expose password

    char* response = cJSON_PrintUnformatted(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));

    free(response);
    cJSON_Delete(json);
    return ESP_OK;
}

// POST /api/config - Save configuration
static esp_err_t handler_config_post(httpd_req_t* req) {
    char buf[512];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON* json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    sol_config_t* cfg = s_webui_config.config;

    cJSON* item;
    if ((item = cJSON_GetObjectItem(json, "device_name")) && cJSON_IsString(item)) {
        strncpy(cfg->device_name, item->valuestring, sizeof(cfg->device_name) - 1);
    }
    if ((item = cJSON_GetObjectItem(json, "mode")) && cJSON_IsNumber(item)) {
        cfg->mode = (sol_mode_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, "channels")) && cJSON_IsNumber(item)) {
        cfg->channels = (uint8_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, "rtp_port")) && cJSON_IsNumber(item)) {
        cfg->rtp_port = (uint16_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, "fec_enabled")) && cJSON_IsBool(item)) {
        cfg->fec_enabled = cJSON_IsTrue(item) ? 1 : 0;
    }
    if ((item = cJSON_GetObjectItem(json, "target_latency_ms")) && cJSON_IsNumber(item)) {
        cfg->target_latency_ms = (float)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItem(json, "wifi_ssid")) && cJSON_IsString(item)) {
        strncpy(cfg->wifi_ssid, item->valuestring, sizeof(cfg->wifi_ssid) - 1);
    }
    if ((item = cJSON_GetObjectItem(json, "wifi_pass")) && cJSON_IsString(item) &&
        strlen(item->valuestring) > 0) {
        strncpy(cfg->wifi_pass, item->valuestring, sizeof(cfg->wifi_pass) - 1);
    }

    cJSON_Delete(json);

    // Trigger save action
    if (s_webui_config.action_cb) {
        s_webui_config.action_cb(SOL_WEBUI_ACTION_SAVE_CONFIG, cfg);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /api/reboot - Reboot device
static esp_err_t handler_reboot(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    if (s_webui_config.action_cb) {
        s_webui_config.action_cb(SOL_WEBUI_ACTION_REBOOT, NULL);
    }
    return ESP_OK;
}

// POST /api/factory-reset - Factory reset
static esp_err_t handler_factory_reset(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    if (s_webui_config.action_cb) {
        s_webui_config.action_cb(SOL_WEBUI_ACTION_FACTORY_RESET, NULL);
    }
    return ESP_OK;
}

// POST /api/ota - OTA firmware update
static esp_err_t handler_ota(httpd_req_t* req) {
    ESP_LOGI(TAG, "OTA update request, size=%d", req->content_len);

    if (s_webui_config.action_cb) {
        int err = s_webui_config.action_cb(SOL_WEBUI_ACTION_START_OTA, req);
        if (err != 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA failed");
            return ESP_FAIL;
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

int sol_webui_start(const sol_webui_config_t* webui_config) {
    if (s_server) {
        ESP_LOGW(TAG, "WebUI already running");
        return 0;
    }

    memcpy(&s_webui_config, webui_config, sizeof(s_webui_config));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = webui_config->port ? webui_config->port : 80;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 10;
    config.stack_size = 8192;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return -1;
    }

    // Register URI handlers
    httpd_uri_t uri_index = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handler_index,
    };
    httpd_register_uri_handler(s_server, &uri_index);

    httpd_uri_t uri_status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = handler_status,
    };
    httpd_register_uri_handler(s_server, &uri_status);

    httpd_uri_t uri_config_get = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = handler_config_get,
    };
    httpd_register_uri_handler(s_server, &uri_config_get);

    httpd_uri_t uri_config_post = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = handler_config_post,
    };
    httpd_register_uri_handler(s_server, &uri_config_post);

    httpd_uri_t uri_reboot = {
        .uri = "/api/reboot",
        .method = HTTP_POST,
        .handler = handler_reboot,
    };
    httpd_register_uri_handler(s_server, &uri_reboot);

    httpd_uri_t uri_factory_reset = {
        .uri = "/api/factory-reset",
        .method = HTTP_POST,
        .handler = handler_factory_reset,
    };
    httpd_register_uri_handler(s_server, &uri_factory_reset);

    httpd_uri_t uri_ota = {
        .uri = "/api/ota",
        .method = HTTP_POST,
        .handler = handler_ota,
    };
    httpd_register_uri_handler(s_server, &uri_ota);

    ESP_LOGI(TAG, "WebUI started on port %d", config.server_port);
    return 0;
}

void sol_webui_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "WebUI stopped");
    }
}

bool sol_webui_is_running(void) {
    return s_server != NULL;
}

#else
// Stub implementations for non-ESP32 builds

int sol_webui_start(const sol_webui_config_t* webui_config) {
    (void)webui_config;
    return 0;
}

void sol_webui_stop(void) {}

bool sol_webui_is_running(void) { return false; }

#endif
