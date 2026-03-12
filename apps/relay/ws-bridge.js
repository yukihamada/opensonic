// ws-bridge.js — WebSocket to UDP bridge for Soluna relay
// Run: node ws-bridge.js
// Bridges ws://0.0.0.0:5101 <-> udp://127.0.0.1:5100
//
// Each WebSocket client gets its own UDP socket so the relay can
// distinguish peers by source port. Text frames (JOIN/HELLO) are
// forwarded as-is; binary frames (audio) are forwarded raw.
// UDP responses from the relay are forwarded back to the WebSocket.

const WebSocket = require('ws');
const dgram = require('dgram');

const WS_PORT = parseInt(process.env.WS_PORT || '5101', 10);
const RELAY_HOST = process.env.RELAY_HOST || '127.0.0.1';
const RELAY_PORT = parseInt(process.env.RELAY_PORT || '5100', 10);
const MAX_CLIENTS = parseInt(process.env.MAX_CLIENTS || '200', 10);

const wss = new WebSocket.Server({ port: WS_PORT });
console.log(`[ws-bridge] Listening on ws://0.0.0.0:${WS_PORT}`);
console.log(`[ws-bridge] Relay target: udp://${RELAY_HOST}:${RELAY_PORT}`);
console.log(`[ws-bridge] Max clients: ${MAX_CLIENTS}`);

let clientCount = 0;
let activeClients = 0;

wss.on('connection', (ws, req) => {
    if (activeClients >= MAX_CLIENTS) {
        ws.close(1013, 'Server at capacity');
        return;
    }
    activeClients++;
    const clientId = ++clientCount;
    const remoteAddr = req.headers['x-forwarded-for'] || req.socket.remoteAddress;
    const udp = dgram.createSocket('udp4');
    let udpClosed = false;

    // Forward WS -> UDP
    ws.on('message', (data) => {
        const buf = Buffer.isBuffer(data) ? data : Buffer.from(data);
        udp.send(buf, RELAY_PORT, RELAY_HOST);
    });

    // Forward UDP -> WS
    udp.on('message', (msg) => {
        if (ws.readyState === WebSocket.OPEN) {
            // Detect text vs binary: OSTP text messages start with ASCII
            // letters (JOIN, HELLO, META, etc). Audio is RTP (first byte
            // has version bits 0b10xxxxxx = 0x80+).
            if (msg.length > 0 && msg[0] < 0x80) {
                ws.send(msg.toString('utf8'));
            } else {
                ws.send(msg);
            }
        }
    });

    // Bind to ephemeral port
    udp.bind(0, () => {
        const addr = udp.address();
        console.log(`[ws-bridge] #${clientId} connected from ${remoteAddr}, UDP port ${addr.port}`);
    });

    const closeUdp = () => {
        if (!udpClosed) {
            udpClosed = true;
            activeClients--;
            udp.close();
        }
    };

    udp.on('error', (err) => {
        console.error(`[ws-bridge] #${clientId} UDP error:`, err.message);
        closeUdp();
        ws.close();
    });

    ws.on('close', () => {
        closeUdp();
        console.log(`[ws-bridge] #${clientId} disconnected (active: ${activeClients})`);
    });

    ws.on('error', (err) => {
        console.error(`[ws-bridge] #${clientId} WS error:`, err.message);
        closeUdp();
    });
});

// Graceful shutdown
process.on('SIGINT', () => {
    console.log('\n[ws-bridge] Shutting down...');
    wss.close(() => process.exit(0));
});
