/**
 * Soluna Transport Abstraction Layer
 *
 * Provides a unified interface for audio packet transport with automatic
 * fallback: WebTransport (QUIC datagrams) → WebSocket (TCP).
 *
 * WebTransport uses QUIC unreliable datagrams for audio (no head-of-line
 * blocking) and reliable streams for control messages.
 */
(function() {
    'use strict';

    // ── Transport Interface ─────────────────────────────────
    //
    // Every transport implements:
    //   connect(url)              — start connection
    //   disconnect()              — close connection
    //   send(data)                — send binary audio packet (unreliable if possible)
    //   sendControl(data)         — send control message (reliable)
    //   onPacket = fn(Uint8Array) — callback for received packets
    //   onStateChange = fn(state) — 'connected' | 'reconnecting' | 'disconnected'
    //   readonly type             — 'webtransport' | 'websocket'

    // ── WebTransportTransport ───────────────────────────────

    function WebTransportTransport() {
        this.type = 'webtransport';
        this.onPacket = null;
        this.onStateChange = null;
        this._wt = null;
        this._url = null;
        this._reconnectDelay = 1000;
        this._reconnectTimer = null;
        this._intentionalClose = false;
        this._controlWriter = null;
        this._datagramWriter = null;
    }

    /**
     * Check if WebTransport is available in this browser.
     */
    WebTransportTransport.isSupported = function() {
        return typeof WebTransport !== 'undefined';
    };

    WebTransportTransport.prototype.connect = function(url) {
        this.disconnect();
        this._url = url;
        this._reconnectDelay = 1000;
        this._intentionalClose = false;
        this._openSession(url);
    };

    WebTransportTransport.prototype._openSession = function(url) {
        var self = this;

        try {
            this._wt = new WebTransport(url);
        } catch (e) {
            console.warn('[WebTransport] creation failed:', e.message);
            self._scheduleReconnect();
            return;
        }

        this._wt.ready.then(function() {
            self._reconnectDelay = 1000;
            if (self.onStateChange) self.onStateChange('connected');
            console.log('[WebTransport] session ready');

            // Start reading datagrams (audio packets — unreliable)
            self._readDatagrams();

            // Start reading incoming unidirectional streams (control responses)
            self._readIncomingStreams();

            // Cache the datagram writer for send()
            if (self._wt.datagrams && self._wt.datagrams.writable) {
                self._datagramWriter = self._wt.datagrams.writable.getWriter();
            }
        }).catch(function(e) {
            console.warn('[WebTransport] ready failed:', e.message);
            self._scheduleReconnect();
        });

        this._wt.closed.then(function() {
            self._cleanup();
            if (self._intentionalClose) {
                if (self.onStateChange) self.onStateChange('disconnected');
            } else {
                self._scheduleReconnect();
            }
        }).catch(function(e) {
            self._cleanup();
            if (!self._intentionalClose) {
                self._scheduleReconnect();
            }
        });
    };

    WebTransportTransport.prototype._readDatagrams = function() {
        var self = this;
        if (!this._wt || !this._wt.datagrams) return;

        var reader = this._wt.datagrams.readable.getReader();
        (function pump() {
            reader.read().then(function(result) {
                if (result.done) return;
                if (self.onPacket) {
                    self.onPacket(new Uint8Array(result.value));
                }
                pump();
            }).catch(function() {
                // Stream closed — handled by wt.closed
            });
        })();
    };

    WebTransportTransport.prototype._readIncomingStreams = function() {
        var self = this;
        if (!this._wt || !this._wt.incomingUnidirectionalStreams) return;

        var reader = this._wt.incomingUnidirectionalStreams.getReader();
        (function acceptLoop() {
            reader.read().then(function(result) {
                if (result.done) return;
                var stream = result.value;
                self._readStreamFully(stream);
                acceptLoop();
            }).catch(function() {
                // Session closed
            });
        })();
    };

    WebTransportTransport.prototype._readStreamFully = function(stream) {
        var self = this;
        var reader = stream.getReader();
        var chunks = [];

        (function read() {
            reader.read().then(function(result) {
                if (result.value) chunks.push(new Uint8Array(result.value));
                if (result.done) {
                    // Merge chunks and deliver
                    if (chunks.length === 1) {
                        if (self.onPacket) self.onPacket(chunks[0]);
                    } else if (chunks.length > 1) {
                        var total = 0;
                        for (var i = 0; i < chunks.length; i++) total += chunks[i].length;
                        var merged = new Uint8Array(total);
                        var offset = 0;
                        for (var j = 0; j < chunks.length; j++) {
                            merged.set(chunks[j], offset);
                            offset += chunks[j].length;
                        }
                        if (self.onPacket) self.onPacket(merged);
                    }
                    return;
                }
                read();
            }).catch(function() {
                // Stream error — ignore
            });
        })();
    };

    /**
     * Send audio data as an unreliable QUIC datagram.
     * No ordering, no retransmit — perfect for real-time audio.
     */
    WebTransportTransport.prototype.send = function(data) {
        if (!this._datagramWriter) return;
        var u8 = data instanceof Uint8Array ? data : new Uint8Array(data);
        this._datagramWriter.write(u8).catch(function() {
            // Datagram send failed (session closing) — ignore
        });
    };

    /**
     * Send a control message over a reliable QUIC stream.
     * Used for JOIN, META, SYNC, etc.
     */
    WebTransportTransport.prototype.sendControl = function(data) {
        if (!this._wt) return;
        var self = this;
        var u8 = data instanceof Uint8Array ? data : new Uint8Array(data);

        this._wt.createUnidirectionalStream().then(function(stream) {
            var writer = stream.getWriter();
            writer.write(u8).then(function() {
                writer.close();
            }).catch(function() {});
        }).catch(function() {
            // Session closed
        });
    };

    WebTransportTransport.prototype.disconnect = function() {
        this._intentionalClose = true;
        if (this._reconnectTimer) {
            clearTimeout(this._reconnectTimer);
            this._reconnectTimer = null;
        }
        if (this._wt) {
            try { this._wt.close(); } catch (e) {}
        }
        this._cleanup();
        this._url = null;
    };

    WebTransportTransport.prototype._cleanup = function() {
        this._datagramWriter = null;
        this._controlWriter = null;
        this._wt = null;
    };

    WebTransportTransport.prototype._scheduleReconnect = function() {
        var self = this;
        if (self._intentionalClose || !self._url) return;
        if (self.onStateChange) self.onStateChange('reconnecting');
        console.log('[WebTransport] reconnecting in ' + self._reconnectDelay + 'ms');
        self._reconnectTimer = setTimeout(function() {
            self._reconnectTimer = null;
            if (!self._intentionalClose && self._url) {
                self._openSession(self._url);
            }
        }, self._reconnectDelay);
        self._reconnectDelay = Math.min(self._reconnectDelay * 2, 30000);
    };

    // ── WebSocketTransport ──────────────────────────────────

    function WebSocketTransport() {
        this.type = 'websocket';
        this.onPacket = null;
        this.onStateChange = null;
        this._ws = null;
        this._url = null;
        this._reconnectDelay = 1000;
        this._reconnectTimer = null;
        this._intentionalClose = false;
    }

    WebSocketTransport.isSupported = function() {
        return typeof WebSocket !== 'undefined';
    };

    WebSocketTransport.prototype.connect = function(url) {
        this.disconnect();
        this._url = url;
        this._reconnectDelay = 1000;
        this._intentionalClose = false;
        this._openWs(url);
    };

    WebSocketTransport.prototype._openWs = function(url) {
        var self = this;
        try {
            this._ws = new WebSocket(url);
        } catch (e) {
            console.warn('[WebSocket] creation failed:', e.message);
            self._scheduleReconnect();
            return;
        }
        this._ws.binaryType = 'arraybuffer';
        this._ws.onopen = function() {
            self._reconnectDelay = 1000;
            if (self.onStateChange) self.onStateChange('connected');
        };
        this._ws.onmessage = function(evt) {
            if (evt.data instanceof ArrayBuffer) {
                if (self.onPacket) self.onPacket(new Uint8Array(evt.data));
            }
        };
        this._ws.onclose = function() {
            if (self._intentionalClose) {
                if (self.onStateChange) self.onStateChange('disconnected');
                return;
            }
            self._scheduleReconnect();
        };
        this._ws.onerror = function() {};
    };

    /**
     * Send audio data. WebSocket is TCP — reliable but may cause HoL blocking.
     */
    WebSocketTransport.prototype.send = function(data) {
        if (!this._ws || this._ws.readyState !== WebSocket.OPEN) return;
        this._ws.send(data);
    };

    /**
     * Send control message. Same as send() for WebSocket (all goes over TCP).
     */
    WebSocketTransport.prototype.sendControl = function(data) {
        this.send(data);
    };

    WebSocketTransport.prototype.disconnect = function() {
        this._intentionalClose = true;
        if (this._reconnectTimer) {
            clearTimeout(this._reconnectTimer);
            this._reconnectTimer = null;
        }
        if (this._ws) {
            this._ws.close();
            this._ws = null;
        }
        this._url = null;
    };

    WebSocketTransport.prototype._scheduleReconnect = function() {
        var self = this;
        if (self._intentionalClose || !self._url) return;
        if (self.onStateChange) self.onStateChange('reconnecting');
        console.log('[WebSocket] reconnecting in ' + self._reconnectDelay + 'ms');
        self._reconnectTimer = setTimeout(function() {
            self._reconnectTimer = null;
            if (!self._intentionalClose && self._url) {
                self._openWs(self._url);
            }
        }, self._reconnectDelay);
        self._reconnectDelay = Math.min(self._reconnectDelay * 2, 30000);
    };

    // ── Auto-selection Factory ──────────────────────────────

    /**
     * Create the best available transport.
     *
     * @param {Object} opts
     * @param {boolean} [opts.preferWebSocket=false]  Force WebSocket even if WebTransport is available
     * @returns {WebTransportTransport|WebSocketTransport}
     */
    function createTransport(opts) {
        opts = opts || {};
        if (!opts.preferWebSocket && WebTransportTransport.isSupported()) {
            console.log('[SolunaTransport] using WebTransport (QUIC datagrams)');
            return new WebTransportTransport();
        }
        console.log('[SolunaTransport] using WebSocket (TCP fallback)');
        return new WebSocketTransport();
    }

    /**
     * Convert a WebSocket URL to a WebTransport URL.
     * wss://relay.solun.art/ws/audio?channel=foo
     *   → https://relay.solun.art/wt?channel=foo
     *
     * @param {string} wsUrl
     * @returns {string}
     */
    function wsUrlToWtUrl(wsUrl) {
        return wsUrl
            .replace(/^wss:\/\//, 'https://')
            .replace(/^ws:\/\//, 'http://')
            .replace(/\/ws\/audio\b/, '/wt');
    }

    // Expose globally
    window.SolunaTransport = {
        create: createTransport,
        wsUrlToWtUrl: wsUrlToWtUrl,
        WebTransportTransport: WebTransportTransport,
        WebSocketTransport: WebSocketTransport,
    };
})();
