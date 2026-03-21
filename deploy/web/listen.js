// Soluna Radio — Browser Audio Player
// Decodes S24-in-S32LE PCM frames from WebSocket and plays via WebAudio API.

(function () {
  'use strict';

  // ── Channel definitions ──────────────────────────────────────
  const CHANNELS = [
    { id: 'bjj',    emoji: '🥋', label: 'BJJ' },
    { id: 'soluna', emoji: '🌊', label: 'Soluna' },
    { id: 'jazz',   emoji: '🎷', label: 'Jazz' },
    { id: 'chill',  emoji: '🌙', label: 'Chill' },
    { id: 'lofi',   emoji: '📻', label: 'Lo-Fi' },
    { id: 'dance',  emoji: '💃', label: 'Dance' },
    { id: 'yuki',   emoji: '❄️', label: 'Yuki' },
  ];

  const SAMPLE_RATE = 48000;
  const PREFILL_MS = 200;
  const PREFILL_SAMPLES = Math.ceil(SAMPLE_RATE * PREFILL_MS / 1000);
  const WS_BASE = 'wss://relay.solun.art/ws/audio';

  // ── DOM refs ─────────────────────────────────────────────────
  const $ = (id) => document.getElementById(id);
  const tapOverlay   = $('tapOverlay');
  const tapChannel   = $('tapChannel');
  const statusDot    = $('statusDot');
  const statusText   = $('statusText');
  const channelName  = $('channelName');
  const channelIcon  = $('channelIcon');
  const playBtn      = $('playBtn');
  const iconPlay     = $('iconPlay');
  const iconStop     = $('iconStop');
  const volumeSlider = $('volumeSlider');
  const listenerCount = $('listenerCount');
  const bufferStatus = $('bufferStatus');
  const bufferFill   = $('bufferFill');
  const channelGrid  = $('channelGrid');
  const vizCanvas    = $('vizCanvas');

  // ── State ────────────────────────────────────────────────────
  let currentChannel = 'jazz';
  let ws = null;
  let audioCtx = null;
  let gainNode = null;
  let isPlaying = false;
  let audioStarted = false;
  let userGestured = false;

  // Ring buffer for decoded float samples
  let ringBuffer = new Float32Array(SAMPLE_RATE * 4); // 4 seconds
  let writePos = 0;
  let readPos = 0;
  let bufferedSamples = 0;
  let prefillReached = false;

  // Visualizer
  let analyser = null;
  let vizAnimId = null;

  // Listener count polling
  let listenerPollId = null;

  // ── Parse channel from URL ───────────────────────────────────
  function getChannelFromURL() {
    // Support: /listen?ch=jazz, /listen#jazz, /listen/jazz, /c/jazz
    const params = new URLSearchParams(window.location.search);
    if (params.has('ch')) return params.get('ch');

    const hash = window.location.hash.replace('#', '');
    if (hash) return hash;

    // Path: /listen/jazz or /c/jazz
    const path = window.location.pathname;
    const m = path.match(/\/(?:listen|c)\/([a-zA-Z0-9_-]+)/);
    if (m) return m[1];

    return 'jazz';
  }

  // ── Channel grid ─────────────────────────────────────────────
  function renderChannels() {
    channelGrid.innerHTML = '';
    for (const ch of CHANNELS) {
      const btn = document.createElement('button');
      btn.className = 'ch-btn' + (ch.id === currentChannel ? ' active' : '');
      btn.innerHTML = `<span class="ch-emoji">${ch.emoji}</span>${ch.label}`;
      btn.addEventListener('click', () => switchChannel(ch.id));
      channelGrid.appendChild(btn);
    }
  }

  function updateChannelUI() {
    const ch = CHANNELS.find(c => c.id === currentChannel) || { emoji: '🎵', label: currentChannel };
    channelName.textContent = ch.label;
    channelIcon.textContent = ch.emoji;
    tapChannel.textContent = ch.label + ' channel';
    document.title = `${ch.label} — Soluna Radio`;
    // Update active state
    channelGrid.querySelectorAll('.ch-btn').forEach((btn, i) => {
      btn.classList.toggle('active', CHANNELS[i] && CHANNELS[i].id === currentChannel);
    });
    // Update URL without reload
    const url = new URL(window.location);
    url.searchParams.set('ch', currentChannel);
    history.replaceState(null, '', url);
  }

  // ── Audio context ────────────────────────────────────────────
  function ensureAudioContext() {
    if (audioCtx) {
      if (audioCtx.state === 'suspended') audioCtx.resume().catch(() => {});
      return;
    }
    try {
      audioCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: SAMPLE_RATE });
    } catch (e) {
      console.warn('[Soluna] AudioContext creation deferred until user gesture');
      return;
    }

    gainNode = audioCtx.createGain();
    gainNode.gain.value = volumeSlider.value / 100;

    analyser = audioCtx.createAnalyser();
    analyser.fftSize = 256;

    gainNode.connect(analyser);
    analyser.connect(audioCtx.destination);
  }

  // ── AudioWorklet for audio output ────────────────────────────
  let workletNode = null;
  let workletReady = false;

  async function startAudioOutput() {
    if (workletNode) return;
    ensureAudioContext();
    if (!audioCtx) return;

    try {
      if (!workletReady) {
        await audioCtx.audioWorklet.addModule('/soluna-worklet.js');
        workletReady = true;
      }
      workletNode = new AudioWorkletNode(audioCtx, 'soluna-processor', {
        outputChannelCount: [1]
      });
      workletNode.connect(gainNode);
    } catch (e) {
      console.warn('[Soluna] AudioWorklet failed, audio may not play:', e);
      return;
    }
    audioStarted = true;
    startVisualizer();
  }

  function stopAudioOutput() {
    if (workletNode) {
      workletNode.port.postMessage({ type: 'reset' });
      workletNode.disconnect();
      workletNode = null;
    }
    audioStarted = false;
    stopVisualizer();
  }

  // ── Decode S24-in-S32LE → Float32 ───────────────────────────
  function decodeS24inS32LE(data) {
    // Each sample is 4 bytes (32-bit int), but only 24 bits are significant.
    // S24-in-S32LE: the 24-bit sample is stored in the upper 24 bits of the 32-bit word,
    // OR in some implementations in the lower 24 bits. We handle the standard case:
    // bits [31:8] contain the 24-bit audio, bits [7:0] are zero padding.
    // However, the relay sends samples where the 24-bit value occupies bits [23:0].
    const view = new DataView(data);
    const sampleCount = Math.floor(data.byteLength / 4);
    const scale = 1.0 / 8388608.0; // 1 / 2^23

    const samples = new Float32Array(sampleCount);
    for (let i = 0; i < sampleCount; i++) {
      let val = view.getInt32(i * 4, true);
      if (val > 8388607) val -= 16777216;
      if (val < -8388608) val = -8388608;
      samples[i] = Math.max(-1.0, Math.min(1.0, val * scale));
    }

    // Send to AudioWorklet processor
    if (workletNode) {
      workletNode.port.postMessage({ type: 'samples', samples: samples });
    }

    bufferedSamples += sampleCount;
    if (bufferedSamples > ringBuffer.length) {
      bufferedSamples = ringBuffer.length;
    }

    if (!prefillReached && bufferedSamples >= PREFILL_SAMPLES) {
      prefillReached = true;
      bufferStatus.textContent = 'Playing';
    }

    updateBufferBar();
  }

  function updateBufferBar() {
    const pct = Math.min(100, (bufferedSamples / PREFILL_SAMPLES) * 100);
    bufferFill.style.width = pct + '%';
  }

  // ── WebSocket connection ─────────────────────────────────────
  function connectWS() {
    if (ws) {
      ws.onclose = null;
      ws.close();
      ws = null;
    }

    setStatus('connecting');
    resetBuffer();

    const url = `${WS_BASE}?channel=${encodeURIComponent(currentChannel)}`;
    ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';

    ws.onopen = function () {
      setStatus('connected');
    };

    ws.onmessage = function (event) {
      if (event.data instanceof ArrayBuffer && event.data.byteLength > 0) {
        decodeS24inS32LE(event.data);
      } else if (typeof event.data === 'string') {
        // Possibly JSON control message (listener count, etc.)
        try {
          const msg = JSON.parse(event.data);
          if (msg.listeners !== undefined) {
            listenerCount.textContent = msg.listeners;
          }
        } catch (_) { /* ignore non-JSON text */ }
      }
    };

    ws.onerror = function () {
      setStatus('error');
    };

    ws.onclose = function () {
      setStatus('disconnected');
      // Auto-reconnect if playing
      if (isPlaying) {
        setTimeout(connectWS, 2000);
      }
    };
  }

  function resetBuffer() {
    writePos = 0;
    readPos = 0;
    bufferedSamples = 0;
    prefillReached = false;
    bufferStatus.textContent = 'Buffering...';
    bufferFill.style.width = '0%';
    if (workletNode) workletNode.port.postMessage({ type: 'reset' });
  }

  function setStatus(state) {
    statusDot.className = 'status-dot';
    switch (state) {
      case 'connected':
        statusDot.classList.add('connected');
        statusText.textContent = 'Connected';
        break;
      case 'connecting':
        statusText.textContent = 'Connecting';
        break;
      case 'disconnected':
        statusText.textContent = 'Disconnected';
        break;
      case 'error':
        statusDot.classList.add('error');
        statusText.textContent = 'Error';
        break;
    }
  }

  // ── Listener count polling ───────────────────────────────────
  function startListenerPoll() {
    stopListenerPoll();
    fetchListenerCount();
    listenerPollId = setInterval(fetchListenerCount, 10000);
  }

  function stopListenerPoll() {
    if (listenerPollId) {
      clearInterval(listenerPollId);
      listenerPollId = null;
    }
  }

  function fetchListenerCount() {
    fetch(`https://relay.solun.art/api/channels/${encodeURIComponent(currentChannel)}`)
      .then(r => r.ok ? r.json() : null)
      .then(data => {
        if (data && data.listeners !== undefined) {
          listenerCount.textContent = data.listeners;
        }
      })
      .catch(() => { listenerCount.textContent = '--'; });
  }

  // ── Visualizer ───────────────────────────────────────────────
  function startVisualizer() {
    if (!analyser || vizAnimId) return;
    const ctx = vizCanvas.getContext('2d');
    const w = vizCanvas.width;
    const h = vizCanvas.height;
    const cx = w / 2;
    const cy = h / 2;
    const radius = 60;
    const bars = analyser.frequencyBinCount;
    const dataArray = new Uint8Array(bars);

    function draw() {
      vizAnimId = requestAnimationFrame(draw);
      analyser.getByteFrequencyData(dataArray);
      ctx.clearRect(0, 0, w, h);

      const sliceAngle = (2 * Math.PI) / bars;
      for (let i = 0; i < bars; i++) {
        const val = dataArray[i] / 255;
        const barLen = val * 16 + 2;
        const angle = i * sliceAngle - Math.PI / 2;
        const x1 = cx + Math.cos(angle) * radius;
        const y1 = cy + Math.sin(angle) * radius;
        const x2 = cx + Math.cos(angle) * (radius + barLen);
        const y2 = cy + Math.sin(angle) * (radius + barLen);

        ctx.beginPath();
        ctx.moveTo(x1, y1);
        ctx.lineTo(x2, y2);
        ctx.strokeStyle = `rgba(59, 130, 246, ${0.3 + val * 0.7})`;
        ctx.lineWidth = 1.5;
        ctx.lineCap = 'round';
        ctx.stroke();
      }
    }
    draw();
  }

  function stopVisualizer() {
    if (vizAnimId) {
      cancelAnimationFrame(vizAnimId);
      vizAnimId = null;
    }
    const ctx = vizCanvas.getContext('2d');
    ctx.clearRect(0, 0, vizCanvas.width, vizCanvas.height);
  }

  // ── Play / Stop toggle ───────────────────────────────────────
  async function play() {
    if (isPlaying) return;
    isPlaying = true;
    ensureAudioContext();
    await startAudioOutput();
    connectWS();
    startListenerPoll();
    iconPlay.style.display = 'none';
    iconStop.style.display = 'block';
    playBtn.classList.add('playing');
  }

  function stop() {
    isPlaying = false;
    if (ws) {
      ws.onclose = null;
      ws.close();
      ws = null;
    }
    stopAudioOutput();
    stopListenerPoll();
    setStatus('disconnected');
    resetBuffer();
    iconPlay.style.display = 'block';
    iconStop.style.display = 'none';
    playBtn.classList.remove('playing');
    listenerCount.textContent = '--';
  }

  function switchChannel(ch) {
    if (ch === currentChannel && isPlaying) return;
    currentChannel = ch;
    updateChannelUI();
    if (isPlaying) {
      resetBuffer();
      connectWS();
      startListenerPoll();
    }
  }

  // ── Event listeners ──────────────────────────────────────────
  playBtn.addEventListener('click', () => {
    if (isPlaying) stop(); else play();
  });

  volumeSlider.addEventListener('input', () => {
    if (gainNode) gainNode.gain.value = volumeSlider.value / 100;
  });

  // Tap overlay — required user gesture to create AudioContext
  tapOverlay.addEventListener('click', () => {
    userGestured = true;
    tapOverlay.classList.add('hidden');
    ensureAudioContext();
    play();
  });

  // ── Init ─────────────────────────────────────────────────────
  function init() {
    currentChannel = getChannelFromURL();
    renderChannels();
    updateChannelUI();

    // Always show overlay — AudioContext requires user gesture on all browsers
    tapOverlay.classList.remove('hidden');
  }

  init();
})();
