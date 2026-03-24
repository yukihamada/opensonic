# Soluna Web Audio Receiver — 5 Improvements

## 概要
ブラウザ音声受信パイプラインに5つの改善を実施する。
ワンタップ再生ページ、SharedArrayBuffer零コピーバッファ、Opus WASMフォールバック、
WASM-in-AudioWorklet統合、WebTransportサポート。

## 調査結果

### 現行アーキテクチャ

```
WebSocket (wss://relay.solun.art/ws/audio?channel=xxx)
  ↓ ArrayBuffer (OSTP/RTP binary)
Main Thread (soluna-audio.js)
  ├─ SolunaPlayer (WASM) — push_packet → pull_audio (L/R Float32Array)
  ├─ SolunaOpusBridge (WebCodecs) — PT_OPUS → AudioDecoder → push_decoded_opus
  └─ postMessage({ type:'audio', l, r }) ← structured clone (コピー発生)
       ↓
AudioWorklet (soluna-worklet.js / wasm/soluna-worklet.js)
  └─ 内部 Float32Array リングバッファ → process() で出力
       ↓
GainNode → AnalyserNode → destination
```

### 関連ファイル

| ファイル | 役割 | 行数 |
|---------|------|------|
| `web/wasm/soluna-audio.js` | WASM ローダー + SolunaAudio API | 481 |
| `web/wasm/soluna-worklet.js` | AudioWorklet (WASM dashboard用, stereo) | 64 |
| `web/soluna-worklet.js` | AudioWorklet (listen.js用, mono) | 59 |
| `web/wasm/soluna-opus-bridge.js` | WebCodecs Opus encode/decode | 281 |
| `web/listen.js` | ラジオプレーヤー (S24 直デコード, 非WASM) | 475 |
| `web/listen.html` | ラジオ UI (タップオーバーレイ付き) | ~500 |
| `web/app.js` | ダッシュボード (WASM使用) | 154K |
| `crates/soluna-wasm/src/lib.rs` | Rust WASM コア (RX/TX + ring buffer) | ~600 |
| `crates/soluna-wasm/Cargo.toml` | wasm-bindgen + soluna-core | 20 |
| `crates/soluna-quic-relay/` | QUIC↔UDP ブリッジ (quinn) | ~300 |
| `apps/relay/main.cpp` | C++ リレーサーバー (WebSocket `/ws/audio`) | ~10K |

### 既存パターン・注意点

- **2つのワークレット**: `web/soluna-worklet.js`(listen.js用, mono, `soluna-processor`)と `web/wasm/soluna-worklet.js`(app.js用, stereo, `soluna-wasm`)が別々に存在
- **listen.js は WASM 非使用**: S24-in-S32LE を JS で直接デコード。app.js は WASM (SolunaPlayer) 経由
- **Opus デコード**: WebCodecs API (Chrome 94+/Safari 16.4+) 依存。非対応ブラウザでは Opus 無効
- **データ転送**: postMessage + structured clone でバッファコピーが発生 (毎フレーム)
- **QUIC bridge**: `soluna-quic-relay` が quinn ベースで :5101 に QUIC front-end を提供済み。ただし WebTransport 未対応
- **deploy 手順**: `cp -r web/* deploy/web/ && cd deploy && fly deploy -a soluna-web`

---

## 実装ステップ

### Feature 1: `/c/<channel>` ワンタップ再生ページ（推定: 小）

既存の `listen.html` + `listen.js` に `/c/<channel>` パスが既に一部対応している
(`getChannelFromURL()` で `/c/jazz` パースあり)。専用の軽量ページを作成する。

- [ ] **1-1**: `web/c.html` を作成 — 最小 HTML (インラインCSS/JS、外部依存なし)
  - チャンネル名表示 + 再生/停止ボタン + 音量スライダー + ビジュアライザー
  - URL: `/c/jazz` → nginx/Fly.io で `c.html` にルーティング
  - iOS Safari の autoplay 制約対応: タップオーバーレイ必須
  - `<meta name="apple-mobile-web-app-capable" content="yes">` でホーム画面対応
  - Open Graph タグ: チャンネル名動的埋め込み
- [ ] **1-2**: JS 部分 — `listen.js` から OSTP デコード + WebSocket 接続を抽出して `c.html` にインライン
  - AudioWorklet (`soluna-worklet.js`) をインラインで `Blob URL` から読み込み
  - S24-in-S32LE デコードロジックをそのまま移植
  - 200ms prefill → 自動再生開始
- [ ] **1-3**: Fly.io nginx 設定更新 — `/c/<channel>` → `c.html` にルーティング
  - `deploy/` の Dockerfile / nginx.conf を確認・更新
- [ ] **1-4**: `listen.html` から `/c/<channel>` へのリンクを各チャンネルボタンに追加

### Feature 2: SharedArrayBuffer 零コピーリングバッファ（推定: 中）

現在 postMessage で Float32Array をコピー転送している。SharedArrayBuffer を使い
メインスレッド ↔ AudioWorklet 間で零コピーにする。

- [ ] **2-1**: COOP/COEP ヘッダー追加
  - `Cross-Origin-Opener-Policy: same-origin`
  - `Cross-Origin-Embedder-Policy: require-corp`
  - Fly.io nginx 設定 or HTML `<meta>` で設定
  - **注意**: COEP により外部リソース (CDN画像等) が読めなくなる。`credentialless` も検討
- [ ] **2-2**: `web/wasm/soluna-worklet-sab.js` を新規作成
  - SharedArrayBuffer ベースの SPSC リングバッファ
  - 構造: `[writeIdx: Int32 | readIdx: Int32 | L[131072]: Float32 | R[131072]: Float32]`
  - `Atomics.load/store` で writeIdx/readIdx を同期
  - `process()`: Atomics.load(readIdx) で読み取り可能量を判定 → 直接 SharedArrayBuffer から出力にコピー
- [ ] **2-3**: `soluna-audio.js` を更新
  - AudioContext 生成時に SharedArrayBuffer を確保 (crossOriginIsolated チェック)
  - AudioWorkletNode 生成時に `processorOptions.sharedBuffer` で SAB を渡す
  - `_handlePacket()`: pull_audio 結果を SAB に直接書き込み (postMessage 不要)
  - フォールバック: `crossOriginIsolated === false` の場合は従来の postMessage パス
- [ ] **2-4**: `listen.js` にも SAB パスを追加 (同じ原理)

### Feature 3: Opus WASM フォールバックデコーダ（推定: 大）

WebCodecs 非対応ブラウザ (Firefox等) で Opus デコードを有効にする。
libopus を WASM にコンパイルし、`soluna-opus-bridge.js` のフォールバックとして使う。

- [ ] **3-1**: libopus を Emscripten でビルド
  - `web/wasm/opus/` ディレクトリ作成
  - libopus 1.5.x ソースから `emcc` でビルド
  - エクスポート関数: `opus_decoder_create`, `opus_decode_float`, `opus_decoder_destroy`
  - 出力: `opus-decoder.wasm` + `opus-decoder.js` (ES module)
  - ビルドスクリプト: `scripts/build-opus-wasm.sh`
- [ ] **3-2**: `web/wasm/soluna-opus-fallback.js` を新規作成
  - WASM libopus をロードし、`soluna-opus-bridge.js` と同じインターフェースを提供
  - `on_opus_packet(data, seq, timestamp, channels)` → `opus_decode_float` → `push_decoded_opus`
  - デコーダ状態管理 (チャンネル数変更時に再生成)
- [ ] **3-3**: `soluna-opus-bridge.js` を更新
  - `isSupported()` が `false` の場合、動的に `soluna-opus-fallback.js` をロード
  - フォールバックが有効になったら `set_opus_bridge_available(1)` を呼ぶ
  - ログ: `[SolunaOpusBridge] Using WASM Opus fallback (no WebCodecs)`
- [ ] **3-4**: `soluna-audio.js` の `init()` を更新
  - WebCodecs 失敗時に fallback を試行するフロー追加
- [ ] **3-5**: Cargo.toml に `opus` feature を追加検討
  - Rust 側で libopus を静的リンクする方が効率的かもしれないが、WASM バイナリサイズが増大する
  - 判断: JS 側フォールバックの方が既存 WASM を壊さないため安全 → JS 側で実装

### Feature 4: WASM を AudioWorklet スレッドに移動（推定: 大）

現在: Main Thread で WASM パケット処理 → postMessage → AudioWorklet で再生。
改善: AudioWorklet 内で WASM をロードし、パケット受信 → デコード → 再生を audio thread で完結させる。

- [ ] **4-1**: `web/wasm/soluna-worklet-wasm.js` を新規作成
  - AudioWorkletProcessor 内で `WebAssembly.instantiate()` を呼び WASM をロード
  - `port.onmessage` で生パケット (Uint8Array) を受け取り、WASM `push_packet()` を呼ぶ
  - `process()` で WASM `pull_audio()` を呼び出力バッファに直接書き込み
  - WASM メモリから直接 Float32Array ビューを作成 → 出力にコピー (1回のみ)
- [ ] **4-2**: WASM バイナリの WorkerScope でのロード
  - AudioWorklet は `fetch()` 可能だが制約あり
  - `processorOptions.wasmBytes` で ArrayBuffer を渡す方式 (メインスレッドで fetch → worklet に転送)
  - または `processorOptions.wasmUrl` で URL を渡し worklet 内で fetch
- [ ] **4-3**: Opus デコード統合
  - AudioWorklet 内では WebCodecs API が使えない (Worker scope 制限)
  - 選択肢A: Feature 3 の WASM libopus を worklet 内でもロード → 完全 audio thread 内デコード
  - 選択肢B: Opus パケットだけ postMessage でメインスレッドに投げ、WebCodecs デコード後に戻す
  - → 選択肢A が理想。Feature 3 と組み合わせて WASM libopus を worklet 内で使う
- [ ] **4-4**: `soluna-audio.js` を更新
  - 新しい worklet モード: `_handlePacket()` で生パケットを postMessage (デコード済み audio ではなく)
  - worklet 側が全処理を担当
  - フォールバック: WASM worklet ロード失敗時は従来の main-thread デコードに戻る
- [ ] **4-5**: Feature 2 (SAB) との統合
  - SAB 対応時: メインスレッドが WebSocket からパケットを受け取り SAB に書き込み → worklet が SAB から読み取り
  - worklet 内 WASM が SAB から直接パケットを読む設計も可能だが、WebSocket は main thread 専用なので main→SAB→worklet の流れは変わらない

### Feature 5: WebTransport サポート（推定: 大）

WebSocket の代わりに HTTP/3 + QUIC unreliable datagrams でブラウザに音声配信。
HOL blocking 回避、低レイテンシ。

- [ ] **5-1**: リレーサーバーに WebTransport エンドポイント追加
  - 既存 `soluna-quic-relay` (Rust/quinn) を拡張
  - quinn の `Connection::accept_datagram()` / `send_datagram()` はすでに対応
  - WebTransport は HTTP/3 CONNECT over QUIC。`h3-quinn` + `h3-webtransport` crate を使用
  - エンドポイント: `https://relay.solun.art:5102/wt/audio?channel=<name>`
  - TLS 証明書: Let's Encrypt (自己署名だとブラウザが拒否)
- [ ] **5-2**: `crates/soluna-quic-relay/Cargo.toml` に依存追加
  ```toml
  h3 = "0.0.6"
  h3-quinn = "0.0.7"
  h3-webtransport = "0.1"
  ```
- [ ] **5-3**: `crates/soluna-quic-relay/src/webtransport.rs` を新規作成
  - WebTransport セッション受け入れ → channel 名パース → UDP relay に転送
  - Unreliable datagram: 音声パケット (OSTP/RTP)
  - Reliable stream: 制御メッセージ (JOIN/LEAVE/SYNC)
  - 既存の `main.rs` の QUIC datagram ハンドリングパターンを踏襲
- [ ] **5-4**: `web/wasm/soluna-webtransport.js` を新規作成
  - `WebTransport` API (Chrome 97+) を使った接続マネージャー
  - `transport.datagrams.readable` → ReadableStream → パケット取得
  - `soluna-audio.js` の `SolunaAudio.connect()` と同じインターフェース
  - フォールバック: WebTransport 非対応時は WebSocket にフォールバック
- [ ] **5-5**: `soluna-audio.js` に WebTransport パスを追加
  - `connect(url)` で `wss://` なら WebSocket、`https://` なら WebTransport を試行
  - transport 層の抽象化: `{ send(data), close(), onmessage, onclose }` インターフェース
  - `_handlePacket()` はどちらの transport でも同じパスを通る
- [ ] **5-6**: Let's Encrypt 証明書の設定
  - relay.solun.art に certbot 設定 (既存のHTTPサーバーで ACME challenge)
  - QUIC bridge に証明書パスを環境変数で渡す: `TLS_CERT_PATH`, `TLS_KEY_PATH`
  - 現在の自己署名証明書 (`generate_self_signed_cert`) からの切り替え

---

## 実装順序 (依存関係考慮)

```
Feature 1 (/c/ page)  ← 独立、最初に実装可能
    ↓ (なし)
Feature 2 (SAB)       ← 独立、Feature 1 と並行可能
    ↓
Feature 3 (Opus WASM) ← 独立、Feature 2 と並行可能
    ↓
Feature 4 (WASM in Worklet) ← Feature 2 + Feature 3 に依存
    ↓
Feature 5 (WebTransport)    ← 独立だがサーバー側変更が大きい、最後に実施
```

推奨順: **1 → 2 → 3 → 4 → 5**

## テスト方針

- [ ] Feature 1: `/c/jazz` にアクセス → タップ → 音声再生確認 (iOS Safari, Chrome, Firefox)
- [ ] Feature 2: Chrome DevTools Performance タブで postMessage コピーが消えていることを確認。`crossOriginIsolated === true` を確認
- [ ] Feature 3: Firefox (WebCodecs 未対応) で Opus チャンネルが再生されることを確認
- [ ] Feature 4: Chrome DevTools → Performance → Main thread の audio 処理負荷が減少していることを確認
- [ ] Feature 5: Chrome 97+ で WebTransport 接続 → 音声再生。WebSocket フォールバックも確認

## リスク

1. **COOP/COEP ヘッダー (Feature 2)**: 外部リソース (画像CDN等) がブロックされる可能性。`/c/` ページは自己完結なので影響小だが、`app.html` には慎重に適用
2. **AudioWorklet 内の WASM (Feature 4)**: ブラウザによって WorkletGlobalScope での WebAssembly サポートが異なる。Chrome は対応、Safari は要確認
3. **WebTransport (Feature 5)**: h3-webtransport crate がまだ不安定 (0.x)。API 変更リスクあり。Let's Encrypt 証明書の自動更新も必要
4. **libopus WASM (Feature 3)**: Emscripten ビルド環境のセットアップが必要。WASM バイナリサイズ ~300KB 追加
5. **Firefox の SharedArrayBuffer (Feature 2)**: COOP/COEP が正しく設定されていないと `SharedArrayBuffer is not defined` になる

## 完了条件

1. `/c/jazz` でワンタップ再生ができる (iOS Safari + Chrome + Firefox)
2. SharedArrayBuffer 有効時に postMessage コピーが発生しない
3. Firefox (WebCodecs 非対応) で Opus 音声が再生できる
4. AudioWorklet 内で WASM パケット処理 + デコードが完結する
5. WebTransport 経由で音声受信できる (Chrome 97+)
6. 全 Feature でフォールバックパスが動作する (レガシーブラウザ対応)
