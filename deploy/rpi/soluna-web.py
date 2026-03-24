#!/usr/bin/env python3
"""Soluna RPi Web UI — channel / codec / volume control on port 8080.
Volume/mute goes via Python→WS8400 (no direct browser↔WS needed).
Channel/codec changes restart soluna-rx service.
Status is HTTP-polled every 2s."""

import http.server
import json
import socket
import subprocess
import urllib.parse
from pathlib import Path

CONFIG_FILE = "/etc/soluna-rx.conf"
SERVICE_NAME = "soluna-rx"
WS_HOST = "127.0.0.1"
WS_PORT = 8400

# ── Config ──────────────────────────────────────────────────────────────────

def read_config():
    cfg = {"CHANNEL": "jazz", "CODEC": "pcm",
           "RELAY": "52.194.128.180:5100", "ALSA_DEVICE": "plughw:1,0"}
    try:
        for line in Path(CONFIG_FILE).read_text().splitlines():
            line = line.strip()
            if "=" in line and not line.startswith("#"):
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip()
    except Exception:
        pass
    return cfg

def write_config(cfg):
    Path(CONFIG_FILE).write_text(
        "\n".join(f"{k}={v}" for k, v in cfg.items()) + "\n"
    )

def service_running():
    return subprocess.run(
        ["systemctl", "is-active", "--quiet", SERVICE_NAME]
    ).returncode == 0

def restart_service():
    subprocess.run(["systemctl", "restart", SERVICE_NAME], check=True)

# ── Minimal WebSocket client (one-shot message to port 8400) ────────────────

def ws_call(message: str) -> str:
    """Send one WS text frame to soluna WS server, return response or ''."""
    import base64, random
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect((WS_HOST, WS_PORT))
        raw_key = bytes(random.getrandbits(8) for _ in range(16))
        key = base64.b64encode(raw_key).decode()
        s.sendall((
            f"GET /ws HTTP/1.1\r\nHost: localhost\r\n"
            f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
        ).encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = s.recv(1024)
            if not chunk:
                break
            resp += chunk
        # Send masked text frame
        data = message.encode()
        mask = bytes(random.getrandbits(8) for _ in range(4))
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        hdr = bytes([0x81, 0x80 | len(data)]) + mask if len(data) < 126 else \
              bytes([0x81, 0xFE, len(data) >> 8, len(data) & 0xFF]) + mask
        s.sendall(hdr + masked)
        # Read one response frame
        s.settimeout(1.0)
        try:
            raw = s.recv(4096)
            if len(raw) >= 4:
                payload_len = raw[1] & 0x7F
                payload = raw[2:2 + payload_len]
                return payload.decode(errors="ignore")
        except Exception:
            pass
        s.close()
    except Exception as e:
        print(f"[ws] {e}")
    return ""

# ── HTML ────────────────────────────────────────────────────────────────────

HTML = """<!DOCTYPE html>
<html lang="ja">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Soluna RPi</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0a0a12;color:#e8e8f0;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
  min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:24px 16px}
h1{font-size:22px;font-weight:700;letter-spacing:.05em;color:#fff;margin-bottom:4px}
.sub{font-size:12px;color:rgba(255,255,255,.4);margin-bottom:28px}
.card{background:rgba(255,255,255,.05);border:1px solid rgba(255,255,255,.1);border-radius:16px;
  padding:20px;width:100%;max-width:400px;margin-bottom:14px}
.label{font-size:11px;font-weight:600;letter-spacing:.08em;text-transform:uppercase;
  color:rgba(255,255,255,.4);margin-bottom:14px}
.status-row{display:flex;align-items:center;gap:10px}
.dot{width:8px;height:8px;border-radius:50%;background:#555;flex-shrink:0;transition:all .3s}
.dot.on{background:#4ade80;box-shadow:0 0 8px #4ade8080}
.dot.off{background:#f87171}
.status-text{font-size:14px;font-weight:500}
.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}
.btn{background:rgba(255,255,255,.06);border:1px solid rgba(255,255,255,.12);border-radius:10px;
  color:rgba(255,255,255,.7);cursor:pointer;font-size:13px;font-weight:500;padding:10px 6px;
  text-align:center;transition:all .15s;-webkit-tap-highlight-color:transparent}
.btn:active{transform:scale(.95)}
.btn:hover{background:rgba(255,255,255,.12);color:#fff}
.btn.active{background:rgba(99,102,241,.3);border-color:rgba(99,102,241,.7);color:#a5b4fc}
.row2{display:flex;gap:8px}
.row2 .btn{flex:1}
.vol-row{display:flex;align-items:center;gap:12px}
.vol-lbl{font-size:13px;color:rgba(255,255,255,.5);min-width:40px;text-align:right}
input[type=range]{flex:1;-webkit-appearance:none;height:4px;border-radius:2px;
  background:rgba(255,255,255,.15);outline:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:20px;
  border-radius:50%;background:#6366f1;cursor:pointer}
.mute{background:none;border:1px solid rgba(255,255,255,.15);border-radius:8px;
  color:rgba(255,255,255,.5);cursor:pointer;font-size:18px;padding:4px 10px;line-height:1.4}
.mute.on{color:#f87171;border-color:#f87171}
.toast{position:fixed;bottom:24px;left:50%;transform:translateX(-50%) translateY(80px);
  background:rgba(99,102,241,.9);color:#fff;font-size:13px;font-weight:500;
  padding:10px 20px;border-radius:20px;transition:transform .3s;pointer-events:none}
.toast.show{transform:translateX(-50%) translateY(0)}
</style>
</head>
<body>
<h1>Soluna</h1>
<p class="sub">Raspberry Pi Receiver</p>

<div class="card">
  <div class="label">Status</div>
  <div class="status-row">
    <div class="dot" id="dot"></div>
    <span class="status-text" id="stxt">Loading...</span>
  </div>
</div>

<div class="card">
  <div class="label">Channel</div>
  <div class="grid" id="chgrid">
    <button class="btn" data-ch="bjj">bjj</button>
    <button class="btn" data-ch="jazz">jazz</button>
    <button class="btn" data-ch="chill">chill</button>
    <button class="btn" data-ch="lofi">lofi</button>
    <button class="btn" data-ch="dance">dance</button>
    <button class="btn" data-ch="soluna">soluna</button>
    <button class="btn" data-ch="yuki">yuki</button>
  </div>
</div>

<div class="card">
  <div class="label">Codec</div>
  <div class="row2">
    <button class="btn" id="cpcm" data-codec="pcm">PCM</button>
    <button class="btn" id="cadpcm" data-codec="adpcm">ADPCM</button>
  </div>
</div>

<div class="card">
  <div class="label">Volume</div>
  <div class="vol-row">
    <button class="mute" id="mbtn">&#128264;</button>
    <input type="range" id="vslider" min="0" max="100" value="80">
    <span class="vol-lbl" id="vlbl">80%</span>
  </div>
</div>

<div class="toast" id="toast"></div>

<script>
let ch='jazz', codec='pcm', muted=false;

function toast(msg){
  const t=document.getElementById('toast');
  t.textContent=msg; t.classList.add('show');
  setTimeout(function(){t.classList.remove('show');},2000);
}
function setStatus(running,c,co){
  ch=c; codec=co;
  document.getElementById('dot').className='dot '+(running?'on':'off');
  document.getElementById('stxt').textContent=running?'Playing \u2014 '+c+' ('+co.toUpperCase()+')'  :'Stopped';
  document.querySelectorAll('#chgrid .btn').forEach(function(b){b.classList.toggle('active',b.dataset.ch===c);});
  document.getElementById('cpcm').classList.toggle('active',co==='pcm');
  document.getElementById('cadpcm').classList.toggle('active',co==='adpcm');
}

function poll(){
  fetch('/api/status').then(function(r){return r.json();}).then(function(d){
    setStatus(d.running,d.channel,d.codec);
    var v=Math.round(d.volume*100);
    document.getElementById('vslider').value=v;
    document.getElementById('vlbl').textContent=v+'%';
  }).catch(function(){});
}
poll();
setInterval(poll,3000);

function setChannel(c){
  if(c===ch)return;
  toast('Switching to '+c+'...');
  fetch('/api/channel',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({channel:c})})
    .then(function(r){return r.json();}).then(function(d){toast(d.ok?'Channel: '+c:'Error');setTimeout(poll,2000);});
}
function setCodec(co){
  if(co===codec)return;
  toast('Codec: '+co.toUpperCase()+'...');
  fetch('/api/codec',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({codec:co})})
    .then(function(r){return r.json();}).then(function(d){toast(d.ok?'Codec: '+co.toUpperCase():'Error');setTimeout(poll,2000);});
}

var vtimer=null;
document.getElementById('vslider').addEventListener('input',function(){
  document.getElementById('vlbl').textContent=this.value+'%';
});
document.getElementById('vslider').addEventListener('change',function(){
  var v=this.value;
  clearTimeout(vtimer);
  vtimer=setTimeout(function(){
    fetch('/api/volume',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({volume:v/100})});
  },150);
});
document.getElementById('mbtn').addEventListener('click',function(){
  muted=!muted;
  this.className='mute'+(muted?' on':'');
  this.textContent=muted?'\uD83D\uDD07':'\uD83D\uDD08';
  fetch('/api/mute',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({muted:muted})});
});
document.querySelectorAll('#chgrid .btn').forEach(function(b){
  b.addEventListener('click',function(){setChannel(b.dataset.ch);});
});
document.getElementById('cpcm').addEventListener('click',function(){setCodec('pcm');});
document.getElementById('cadpcm').addEventListener('click',function(){setCodec('adpcm');});
</script>
</body>
</html>
"""

# ── HTTP handler ────────────────────────────────────────────────────────────

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_): pass

    def send_json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path in ("/", "/index.html"):
            body = HTML.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Content-Security-Policy",
                             "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'")
            self.end_headers()
            self.wfile.write(body)
        elif path == "/api/status":
            cfg = read_config()
            # Get volume from WS
            vol = 0.8
            try:
                r = ws_call('{"method":"monitor.status","id":1}')
                if r:
                    import re
                    m = re.search(r'"monitor_volume"\s*:\s*([\d.]+)', r)
                    if m: vol = float(m.group(1))
            except Exception: pass
            self.send_json(200, {
                "channel": cfg.get("CHANNEL", "jazz"),
                "codec":   cfg.get("CODEC", "pcm"),
                "volume":  vol,
                "running": service_running(),
            })
        else:
            self.send_response(404); self.end_headers()

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length)) if length else {}

        if path == "/api/channel":
            ch = body.get("channel", "jazz")
            cfg = read_config(); cfg["CHANNEL"] = ch; write_config(cfg)
            try: restart_service(); ok = True
            except Exception as e: print(e); ok = False
            self.send_json(200, {"ok": ok, "channel": ch})

        elif path == "/api/codec":
            codec = body.get("codec", "pcm")
            cfg = read_config(); cfg["CODEC"] = codec; write_config(cfg)
            try: restart_service(); ok = True
            except Exception as e: print(e); ok = False
            self.send_json(200, {"ok": ok, "codec": codec})

        elif path == "/api/volume":
            vol = max(0.0, min(1.0, float(body.get("volume", 0.8))))
            ws_call(json.dumps({"method": "monitor.set_volume", "volume": vol, "id": 2}))
            self.send_json(200, {"ok": True, "volume": vol})

        elif path == "/api/mute":
            muted = bool(body.get("muted", False))
            ws_call(json.dumps({"method": "monitor.set_mute", "muted": muted, "id": 3}))
            self.send_json(200, {"ok": True, "muted": muted})

        else:
            self.send_response(404); self.end_headers()

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()


if __name__ == "__main__":
    PORT = 8080
    server = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print(f"[soluna-web] http://0.0.0.0:{PORT}")
    server.serve_forever()
