//
//  DashboardServer.swift
//  SolunaReceiverMac
//
//  Built-in web dashboard accessible at http://{mac-ip}:8400/
//  Provides real-time status, channel switching, and playback control.
//

import Foundation
import Network

@MainActor
final class DashboardServer {
    static let shared = DashboardServer()
    static let port: UInt16 = 8400

    private var listener: NWListener?

    private let channels = ["soluna", "jazz", "lofi", "chill", "dance", "bjj", "yuki"]
    private let startTime = Date()

    func start() {
        guard listener == nil else { return }
        do {
            let params = NWParameters.tcp
            params.allowLocalEndpointReuse = true
            listener = try NWListener(using: params, on: NWEndpoint.Port(rawValue: Self.port)!)
            listener?.newConnectionHandler = { [weak self] conn in
                conn.start(queue: .main)
                Task { @MainActor in self?.handleConnection(conn) }
            }
            listener?.start(queue: .main)
            print("[Dashboard] Server started on port \(Self.port)")
        } catch {
            print("[Dashboard] Failed to start: \(error)")
        }
    }

    func stop() {
        listener?.cancel()
        listener = nil
    }

    /// Returns the local IP address for display
    static func localIPAddress() -> String {
        var address = "127.0.0.1"
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0, let firstAddr = ifaddr else { return address }
        defer { freeifaddrs(ifaddr) }
        for ptr in sequence(first: firstAddr, next: { $0.pointee.ifa_next }) {
            let sa = ptr.pointee.ifa_addr.pointee
            guard sa.sa_family == UInt8(AF_INET) else { continue }
            let name = String(cString: ptr.pointee.ifa_name)
            guard name == "en0" || name == "en1" else { continue }
            var hostname = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            getnameinfo(ptr.pointee.ifa_addr, socklen_t(sa.sa_len),
                        &hostname, socklen_t(hostname.count), nil, 0, NI_NUMERICHOST)
            address = String(cString: hostname)
            break
        }
        return address
    }

    // MARK: - Connection handling

    private func handleConnection(_ conn: NWConnection) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 8192) { [weak self] data, _, _, error in
            guard let self, let data, error == nil,
                  let request = String(data: data, encoding: .utf8) else {
                conn.cancel()
                return
            }
            let lines = request.components(separatedBy: "\r\n")
            guard let firstLine = lines.first else { conn.cancel(); return }
            let parts = firstLine.components(separatedBy: " ")
            guard parts.count >= 2 else { conn.cancel(); return }
            let method = parts[0]
            let path = parts[1]

            // Extract body for POST requests
            let body = request.components(separatedBy: "\r\n\r\n").last ?? ""

            Task { @MainActor in
                let (contentType, responseBody) = self.route(method: method, path: path, body: body)
                let header = "HTTP/1.1 200 OK\r\nContent-Type: \(contentType)\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\nConnection: close\r\n\r\n"
                let full = header + responseBody
                conn.send(content: full.data(using: .utf8), completion: .contentProcessed { _ in
                    conn.cancel()
                })
            }
        }
    }

    // MARK: - Routing

    private func route(method: String, path: String, body: String) -> (String, String) {
        if method == "OPTIONS" {
            return ("application/json", "{\"ok\":true}")
        }

        switch (method, path) {
        case ("GET", "/"):
            return ("text/html; charset=utf-8", Self.dashboardHTML)

        case ("GET", "/api/status"):
            return ("application/json", statusJSON())

        case ("GET", "/api/channels"):
            return ("application/json", channelsJSON())

        case ("POST", "/api/play"):
            let rx = AudioReceiver.shared
            if !rx.isPlaying { rx.start() }
            return ("application/json", "{\"ok\":true}")

        case ("POST", "/api/stop"):
            let rx = AudioReceiver.shared
            if rx.isPlaying { rx.toggle() }
            return ("application/json", "{\"ok\":true}")

        case ("POST", "/api/volume"):
            if let data = body.data(using: .utf8),
               let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
               let vol = json["volume"] as? Double {
                let clamped = Float(max(0, min(1, vol)))
                AudioReceiver.shared.volume = clamped
                return ("application/json", "{\"ok\":true,\"volume\":\(clamped)}")
            }
            return ("application/json", "{\"error\":\"invalid body, expected {\\\"volume\\\": 0.0-1.0}\"}")

        case ("POST", _) where path.hasPrefix("/api/channel/"):
            let name = String(path.dropFirst("/api/channel/".count))
                .removingPercentEncoding ?? ""
            guard !name.isEmpty else {
                return ("application/json", "{\"error\":\"empty channel name\"}")
            }
            UserDefaults.standard.set(name, forKey: "channel")
            SDKAudioReceiver.shared.setChannel(name)
            return ("application/json", "{\"ok\":true,\"channel\":\"\(name)\"}")

        default:
            return ("application/json", "{\"error\":\"not found\"}")
        }
    }

    // MARK: - JSON builders

    private func statusJSON() -> String {
        let sdk = SDKAudioReceiver.shared
        let rx = AudioReceiver.shared
        let channel = UserDefaults.standard.string(forKey: "channel") ?? "soluna"
        let uptime = Int(Date().timeIntervalSince(startTime))
        let volume = rx.volume
        let muted = rx.isMuted
        let outputs = rx.activeOutputs.count

        return """
        {"isPlaying":\(sdk.isPlaying),"channel":"\(channel)","bufferMs":\(sdk.bufferMs),"packetsReceived":\(sdk.packetsReceived),"packetsPerSec":\(sdk.packetsPerSec),"bufferFillMs":\(sdk.bufferFillMs),"volume":\(String(format:"%.2f",volume)),"muted":\(muted),"uptime":\(uptime),"activeOutputs":\(outputs),"state":"\(sdk.state.rawValue)"}
        """
    }

    private func channelsJSON() -> String {
        let current = UserDefaults.standard.string(forKey: "channel") ?? "soluna"
        let items = channels.map { ch in
            "{\"name\":\"\(ch)\",\"active\":\(ch == current)}"
        }
        return "[\(items.joined(separator: ","))]"
    }

    // MARK: - Dashboard HTML

    static let dashboardHTML: String = """
    <!DOCTYPE html>
    <html lang="en">
    <head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <title>Soluna Dashboard</title>
    <style>
    *{margin:0;padding:0;box-sizing:border-box}
    body{background:#0a0a0c;color:#e4e4e7;font-family:-apple-system,BlinkMacSystemFont,'SF Pro',system-ui,sans-serif;min-height:100vh}
    .wrap{max-width:640px;margin:0 auto;padding:20px}
    header{display:flex;align-items:center;gap:10px;margin-bottom:24px}
    header h1{font-size:20px;font-weight:600;letter-spacing:-0.3px}
    .dot{width:10px;height:10px;border-radius:50%;background:#555;flex-shrink:0}
    .dot.on{background:#22c55e;box-shadow:0 0 8px #22c55e80}
    .dot.connecting{background:#f97316;box-shadow:0 0 8px #f9731680}
    .stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:10px;margin-bottom:20px}
    .stat{background:#18181b;border-radius:10px;padding:12px;text-align:center}
    .stat .val{font-size:22px;font-weight:700;color:#f97316;font-variant-numeric:tabular-nums}
    .stat .lbl{font-size:11px;color:#71717a;margin-top:2px;text-transform:uppercase;letter-spacing:0.5px}
    .section{background:#18181b;border-radius:10px;padding:16px;margin-bottom:14px}
    .section h2{font-size:13px;color:#71717a;text-transform:uppercase;letter-spacing:0.5px;margin-bottom:10px}
    .vu{height:8px;background:#27272a;border-radius:4px;overflow:hidden;margin-bottom:4px}
    .vu-fill{height:100%;background:linear-gradient(90deg,#22c55e,#f97316,#ef4444);border-radius:4px;transition:width .3s ease}
    .channels{display:flex;flex-wrap:wrap;gap:8px}
    .ch-btn{padding:8px 16px;border-radius:8px;border:1px solid #27272a;background:#0a0a0c;color:#a1a1aa;cursor:pointer;font-size:13px;transition:all .15s}
    .ch-btn:hover{border-color:#f97316;color:#f97316}
    .ch-btn.active{background:#f97316;color:#fff;border-color:#f97316}
    .custom-ch{display:flex;gap:8px;margin-top:10px}
    .custom-ch input{flex:1;padding:8px 12px;border-radius:8px;border:1px solid #27272a;background:#0a0a0c;color:#e4e4e7;font-size:13px;outline:none}
    .custom-ch input:focus{border-color:#f97316}
    .custom-ch button{padding:8px 16px;border-radius:8px;border:none;background:#f97316;color:#fff;cursor:pointer;font-size:13px;font-weight:600}
    .vol-wrap{display:flex;align-items:center;gap:12px}
    .vol-wrap input[type=range]{flex:1;accent-color:#f97316;height:6px}
    .vol-pct{font-size:14px;font-variant-numeric:tabular-nums;width:40px;text-align:right}
    .play-btn{width:100%;padding:14px;border-radius:10px;border:none;font-size:15px;font-weight:600;cursor:pointer;transition:all .15s}
    .play-btn.playing{background:#27272a;color:#ef4444}
    .play-btn.stopped{background:#f97316;color:#fff}
    .play-btn:hover{opacity:0.9;transform:scale(0.99)}
    .log{max-height:160px;overflow-y:auto;font-family:'SF Mono',Menlo,monospace;font-size:11px;color:#52525b;line-height:1.6}
    .log div{border-bottom:1px solid #1a1a1e;padding:2px 0}
    .devices{font-size:13px;color:#a1a1aa}
    .devices span{color:#f97316;font-weight:600}
    </style>
    </head>
    <body>
    <div class="wrap">
      <header>
        <div class="dot" id="dot"></div>
        <h1>Soluna Dashboard</h1>
      </header>

      <div class="stats">
        <div class="stat"><div class="val" id="s-ch">--</div><div class="lbl">Channel</div></div>
        <div class="stat"><div class="val" id="s-buf">--</div><div class="lbl">Buffer (ms)</div></div>
        <div class="stat"><div class="val" id="s-pps">--</div><div class="lbl">Pkts/sec</div></div>
        <div class="stat"><div class="val" id="s-up">--</div><div class="lbl">Uptime</div></div>
      </div>

      <div class="section">
        <h2>Level</h2>
        <div class="vu"><div class="vu-fill" id="vu" style="width:0%"></div></div>
      </div>

      <div class="section">
        <h2>Channel</h2>
        <div class="channels" id="channels"></div>
        <div class="custom-ch">
          <input id="custom-input" placeholder="Custom channel name...">
          <button onclick="switchCustom()">Join</button>
        </div>
      </div>

      <div class="section">
        <h2>Volume</h2>
        <div class="vol-wrap">
          <input type="range" id="vol" min="0" max="100" value="100" oninput="setVol(this.value)">
          <div class="vol-pct" id="vol-pct">100%</div>
        </div>
      </div>

      <div class="section" id="dev-section" style="display:none">
        <h2>Outputs</h2>
        <div class="devices" id="devices"></div>
      </div>

      <button class="play-btn stopped" id="play-btn" onclick="togglePlay()">Play</button>

      <div class="section" style="margin-top:14px">
        <h2>Event Log</h2>
        <div class="log" id="log"></div>
      </div>
    </div>

    <script>
    const API='';
    let playing=false, currentChannel='';

    function log(msg){
      const d=document.getElementById('log');
      const t=new Date().toLocaleTimeString();
      const el=document.createElement('div');
      el.textContent=`[${t}] ${msg}`;
      d.prepend(el);
      while(d.children.length>50) d.lastChild.remove();
    }

    function fmtUptime(s){
      const h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sec=s%60;
      return h>0?`${h}h${m}m`:`${m}m${sec}s`;
    }

    async function fetchStatus(){
      try{
        const r=await fetch(API+'/api/status');
        const d=await r.json();
        playing=d.isPlaying;
        const dot=document.getElementById('dot');
        dot.className='dot'+(d.isPlaying?' on':d.state==='Connecting...'?' connecting':'');
        document.getElementById('s-ch').textContent=d.channel||'--';
        document.getElementById('s-buf').textContent=d.bufferFillMs;
        document.getElementById('s-pps').textContent=d.packetsPerSec;
        document.getElementById('s-up').textContent=fmtUptime(d.uptime);
        const pct=Math.min(100,Math.max(0,d.bufferFillMs/120*100));
        document.getElementById('vu').style.width=pct+'%';
        document.getElementById('vol').value=Math.round(d.volume*100);
        document.getElementById('vol-pct').textContent=Math.round(d.volume*100)+'%';
        const btn=document.getElementById('play-btn');
        btn.textContent=playing?'Stop':'Play';
        btn.className='play-btn '+(playing?'playing':'stopped');
        currentChannel=d.channel||'';
        if(d.activeOutputs>0){
          document.getElementById('dev-section').style.display='';
          document.getElementById('devices').innerHTML='<span>'+d.activeOutputs+'</span> extra output(s) active';
        }else{
          document.getElementById('dev-section').style.display='none';
        }
        updateChannelButtons();
      }catch(e){}
    }

    async function fetchChannels(){
      try{
        const r=await fetch(API+'/api/channels');
        const chs=await r.json();
        const wrap=document.getElementById('channels');
        wrap.innerHTML='';
        chs.forEach(c=>{
          const b=document.createElement('button');
          b.className='ch-btn'+(c.active?' active':'');
          b.textContent=c.name;
          b.onclick=()=>switchChannel(c.name);
          wrap.appendChild(b);
        });
      }catch(e){}
    }

    function updateChannelButtons(){
      document.querySelectorAll('.ch-btn').forEach(b=>{
        b.className='ch-btn'+(b.textContent===currentChannel?' active':'');
      });
    }

    async function switchChannel(name){
      try{
        await fetch(API+'/api/channel/'+encodeURIComponent(name),{method:'POST'});
        log('Switched to channel: '+name);
        currentChannel=name;
        updateChannelButtons();
      }catch(e){log('Error: '+e.message);}
    }

    function switchCustom(){
      const v=document.getElementById('custom-input').value.trim();
      if(v){switchChannel(v);document.getElementById('custom-input').value='';}
    }

    async function togglePlay(){
      try{
        const ep=playing?'/api/stop':'/api/play';
        await fetch(API+ep,{method:'POST'});
        log(playing?'Stopped playback':'Started playback');
      }catch(e){log('Error: '+e.message);}
    }

    async function setVol(v){
      document.getElementById('vol-pct').textContent=v+'%';
      try{
        await fetch(API+'/api/volume',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({volume:v/100})});
      }catch(e){}
    }

    document.getElementById('custom-input').addEventListener('keydown',e=>{if(e.key==='Enter')switchCustom();});

    fetchChannels();
    fetchStatus();
    setInterval(fetchStatus,1000);
    log('Dashboard connected');
    </script>
    </body>
    </html>
    """
}
