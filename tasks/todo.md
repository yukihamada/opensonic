# Soluna iOS — 新機能実装計画

## 概要
4つの機能を優先順に実装する。
1. @username をメインUIに目立つ表示
2. @mention 着信通知（電話ライクなコール）
3. 受信音声の自動録音
4. Apple Speech による自動文字起こし

---

## 調査結果

### 既存コード把握
- `AuthManager.swift`: `username` プロパティ (@Published) 既にあり。`setUsername` / `checkUsername` API 実装済み。
- `EmailLoginView.swift`: ログイン後にusername設定フローあり。ただしSettings画面経由でしか開けない。
- `ContentView.swift` `headerBar`: HStack に globe/qrcode/gear ボタンのみ。@username 表示なし。
- `SettingsView.swift`: `showLogin` state あり、Auth section は settings 内のみ。
- `SolunaReceiverApp.swift`: AVAudioSession は `.playback` のみ。録音 (`.record` or `.playAndRecord`) は未設定。
- relay `main.cpp`: `/api/auth/*` 全実装済み。@mention 用の push notification エンドポイントは**未実装**。
- Push 通知: `SolunaReceiverApp.swift` に APNs 登録コードなし。Info.plist / Entitlements 要確認。

### 注意点
- 録音を追加する場合は `AVAudioSession` category を `.playAndRecord` に変更が必要（SolunaReceiverApp.swift）
- @mention 通知はリレー側に新 API エンドポイント (`POST /api/notify/mention`) + APNs 連携が必要
- Speech framework は iOS 17 以降 on-device 推論が安定。認可 (`NSSpeechRecognitionUsageDescription`) が Info.plist 必要
- CallKit を使うと電話ライクな UI になるが、VoIP entitlement が必要 → 初期は UNUserNotificationCenter の critical alert で代替可能

---

## 優先順位と実装ステップ

---

### Feature 1: @username をメインUIに目立つ表示（優先度: 高・工数: 小）

**目標**: ログイン済みなら headerBar に `@username` バッジを表示。未ログイン/未設定なら「ログイン」ボタンを表示。

#### 変更ファイル
- `ContentView.swift` — `headerBar` に username/login ボタン追加
- `ContentView.swift` — `@StateObject private var auth = AuthManager.shared` を追加
- `ContentView.swift` — `@State private var showLogin = false` + `.sheet(isPresented: $showLogin) { EmailLoginView(auth: auth) }` 追加

#### ステップ
- [ ] Step 1: ContentView に `@StateObject private var auth = AuthManager.shared` を追加（小）
- [ ] Step 2: `headerBar` の先頭（SOLUNA title の右下）に条件分岐ボタンを配置（小）
  - `auth.username != nil` → `@username` capsule ボタン（タップで EmailLoginView ログイン情報表示）
  - `auth.isAuthenticated && auth.username == nil` → `@ユーザー名を設定` ボタン
  - `!auth.isAuthenticated` → `ログイン` ボタン
- [ ] Step 3: `.sheet(isPresented: $showLogin)` で EmailLoginView を呼び出せるようにする（小）

---

### Feature 2: @mention 着信通知（優先度: 高・工数: 大）

**目標**: 誰かが `@yourname` チャンネルに接続すると、iPhoneに電話ライクな通知が届く。

#### 変更ファイル（iOS側）
- `SolunaReceiverApp.swift` — APNs デバイストークン登録処理追加
- `AuthManager.swift` — `registerPushToken(_ token: String)` メソッド追加（`POST /api/auth/register-push`）
- 新規 `NotificationManager.swift` — UNUserNotificationCenter + APNs トークン管理

#### 変更ファイル（リレー側）
- `apps/relay/main.cpp` — 2つの新 API エンドポイント追加:
  1. `POST /api/auth/register-push` — デバイスの APNs トークンを users テーブルに保存
  2. チャンネル接続イベントのフック — channel に `@` prefix があれば対象ユーザーを検索し APNs 送信

#### ステップ
- [ ] Step 1: `NotificationManager.swift` 新規作成（小）
  - `UNUserNotificationCenter.current().requestAuthorization`
  - `UIApplication.shared.registerForRemoteNotifications()`
  - `didRegisterForRemoteNotificationsWithDeviceToken` → hex string 変換
- [ ] Step 2: `SolunaReceiverApp.swift` に `AppDelegate` または `onReceive(NotificationCenter...)` で APNs トークン受信処理追加（小）
- [ ] Step 3: `AuthManager.swift` に `registerPushToken` 追加（小）
- [ ] Step 4: リレー `main.cpp` に `POST /api/auth/register-push` エンドポイント追加（中）
  - UserRecord に `apns_token: string` フィールド追加
  - `users_save` / `users_load` に apns_token を含める
- [ ] Step 5: リレーの channel join ロジックに @mention 検出フック追加（中）
  - チャンネル名が `@` で始まる場合、`g_username_to_email` で検索 → APNs 送信
  - APNs HTTP/2 送信は libcurl + APNs 認証鍵で実装（または既存の libcurl があれば流用）
- [ ] Step 6: Info.plist に `UNNotificationDefaultActionIdentifier` と Push Notifications capability 追加（小）
- [ ] Step 7: Xcode で Push Notifications entitlement を有効化（小）

#### リスク
- APNs 証明書/鍵の管理が必要（.p8 ファイル + key_id + team_id を relay secrets に登録）
- リレーは C++ で HTTP/2 の APNs API を叩く必要あり → 複雑。初期は Firebase FCM 経由も検討余地あり

---

### Feature 3: 受信音声の自動録音（優先度: 中・工数: 中）

**目標**: 受信中に自動で m4a / wav ファイルとして Documents に保存。

#### 変更ファイル
- `SolunaReceiverApp.swift` — AVAudioSession を `.playAndRecord` に変更（録音許可も取得）
- `AudioReceiver.swift` — 録音開始/停止 API 追加
- 新規 `AudioRecorder.swift` — AVAudioFile / AVAudioEngine tap による録音ロジック
- `ContentView.swift` — 録音中インジケーター + 設定トグル
- `SettingsView.swift` — 「自動録音を有効にする」トグル追加

#### ステップ
- [ ] Step 1: `AudioRecorder.swift` 新規作成（中）
  - `AVAudioEngine` の output node に `installTap` で PCM バッファをキャプチャ
  - `AVAudioFile` で Documents/Recordings/`YYYYMMDD-HHmmss.m4a` に書き込み
  - `startRecording()` / `stopRecording()` / `@Published var isRecording: Bool`
- [ ] Step 2: `SolunaReceiverApp.swift` の AVAudioSession category を `.playAndRecord` に変更（小）
  - options: `[.defaultToSpeaker, .allowBluetooth]`
  - Info.plist に `NSMicrophoneUsageDescription` 追加（録音許可要求のため必須）
- [ ] Step 3: `AudioReceiver.swift` に `audioRecorder: AudioRecorder?` を持たせ、state が `.receiving` になったら自動録音開始（中）
- [ ] Step 4: `SettingsView.swift` に `@AppStorage("autoRecord")` トグル追加（小）
- [ ] Step 5: `ContentView.swift` に録音中を示すインジケーター（赤い REC バッジ）追加（小）

#### 注意
- `.playAndRecord` に変更すると、バックグラウンド再生時の挙動を再テストが必要
- iOS のサンドボックス制限: 保存先は `FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)` のみ

---

### Feature 4: Apple Speech による自動文字起こし（優先度: 低・工数: 中）

**目標**: 録音ファイルまたはリアルタイム音声ストリームを Speech framework で文字起こし。

#### 変更ファイル
- 新規 `TranscriptionManager.swift` — SFSpeechRecognizer ラッパー
- `AudioReceiver.swift` または `AudioRecorder.swift` — バッファを TranscriptionManager に渡す
- `ContentView.swift` — 文字起こしテキスト表示エリア
- `SettingsView.swift` — 「自動文字起こし」トグル + 言語選択
- `Info.plist` — `NSSpeechRecognitionUsageDescription` 追加

#### ステップ
- [ ] Step 1: `TranscriptionManager.swift` 新規作成（中）
  - `SFSpeechRecognizer(locale: Locale(identifier: "ja-JP"))` をデフォルト
  - `SFSpeechAudioBufferRecognitionRequest` でリアルタイム認識
  - `@Published var transcript: String`
  - `requestAuthorization()` — 初回起動時に許可要求
- [ ] Step 2: Feature 3 の `AudioRecorder` が書き込んだバッファを `TranscriptionManager` にも流す（小）
- [ ] Step 3: `SettingsView.swift` にトグルと言語選択 Picker 追加（小）
- [ ] Step 4: `ContentView.swift` に `transcriptSection` を追加（receiving 中かつ transcription 有効時のみ表示）（小）
- [ ] Step 5: Info.plist に `NSSpeechRecognitionUsageDescription` を追加（小）

#### 注意
- SFSpeechRecognizer はオンデバイス認識 (iOS 13+) だが長時間連続認識には1分ごとのリクエスト再生成が必要
- Feature 3（録音）が前提。Feature 3 実装後に着手する

---

## 完了条件
- [ ] @username が ContentView の headerBar に表示される（未ログイン時は「ログイン」ボタン）
- [ ] @mention チャンネル接続時に iOS 通知が届く（APNs 経由）
- [ ] `autoRecord=true` の場合、受信開始と同時に Documents に録音ファイルが生成される
- [ ] `autoTranscribe=true` の場合、受信中にリアルタイム文字起こしが ContentView に表示される
- [ ] ビルドエラーなし / TestFlight で動作確認済み

## 実装順序
1. Feature 1（小・即効性高）
2. Feature 3（中・Feature 4 の前提）
3. Feature 4（Feature 3 完了後）
4. Feature 2（最も工数大・リレー側変更も必要）
