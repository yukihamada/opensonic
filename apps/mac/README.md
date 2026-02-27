# SolunaControl — macOS Menu Bar App

Mac の menu bar に常駐し、solunad の操作を手軽に行える SwiftUI アプリ。

## 機能

- **接続状態インジケーター** — solunad への WebSocket 接続状態をリアルタイム表示
- **音量コントロール** — スライダー・±10% ボタン・ミュートトグル
- **スピーカー選択** — Mac の出力デバイスを切り替え（複数デバイス時）
- **Buffer 調整** — Mac スピーカーのジッターバッファを 5–100ms で設定
- **iPhone Receiver 接続** — iPhone の IP を指定して音量・バッファをリモートコントロール
- **Tunnel URL 表示** — ngrok などのトンネル URL をワンクリックでコピー（iPhone に渡す用）
- **ホスト設定** — 接続先 solunad のホスト名 / IP を変更

## 必要環境

- macOS 13 (Ventura) 以上
- Xcode 15+ または Swift 5.9+
- 稼働中の solunad（デフォルト: localhost:8400）

## ビルド

### Swift Package Manager（コマンドライン）

```bash
cd apps/mac
swift build -c release
.build/release/SolunaControl
```

### Xcode で開く

```bash
cd apps/mac
open Package.swift
# Xcode でビルド & 実行（⌘R）
```

## 使い方

1. solunad を起動:
   ```bash
   ./build/solunad --tx --device soluna --speaker ""
   # または launchd 自動起動:
   bash apps/daemon/install-service.sh
   ```

2. SolunaControl を起動 → menu bar に波形アイコンが表示される

3. アイコンをクリックしてポップオーバーを開く

4. 接続先が `localhost` 以外の場合は **Connection → Edit** でホストを変更

### iPhone との連携

1. iPhone で Soluna Receiver アプリを起動・受信開始
2. **iPhone Receiver → Edit** で iPhone の IP アドレスを入力
3. 音量・バッファを Mac menu bar から一括操作

ngrok などのトンネルを使う場合は **Internet URL** 欄に URL が表示されるので、そのままコピーして iPhone に貼り付け。

## アーキテクチャ

```
SolunaControlApp.swift  — @main, MenuBarExtra (.window style)
MenuContent.swift       — SwiftUI ポップオーバー UI (280px幅)
DaemonClient.swift      — WebSocket クライアント (ObservableObject)
```

`DaemonClient` は `ws://[host]:8400` に接続し、JSON-RPC コマンドを送受信します。

| コマンド | 説明 |
|---------|------|
| `monitor.set_volume` | Mac スピーカー音量 (0.0–1.0) |
| `monitor.set_mute` | Mac スピーカー ミュート |
| `monitor.start` | Mac スピーカー再生開始 |
| `monitor.stop` | Mac スピーカー再生停止 |
| `monitor.set_buffer` | Mac バッファ (ms) |

## ライセンス

MIT License
