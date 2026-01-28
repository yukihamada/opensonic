# インストールガイド

各プラットフォームへのSolunaのインストール方法を説明します。

## クイックインストール

### Linux (Debian/Ubuntu/Raspberry Pi)

```bash
curl -sSL https://soluna.dev/install.sh | sudo bash
```

これにより以下が実行されます：
1. システムアーキテクチャを検出（x64、ARM64、ARMv7）
2. 依存関係をインストール（libasound2、libssl3）
3. Solunaパッケージをダウンロード・インストール
4. systemdサービスを設定
5. デーモンを起動

### macOS (Homebrew)

```bash
brew tap soluna/tap
brew install soluna
```

### Windows

[Releases](https://github.com/example/soluna/releases)からインストーラーをダウンロード：

1. `soluna-0.1.0-win64.msi`を実行
2. インストールウィザードに従う
3. スタートメニューから「Soluna」を起動

## ソースからビルド

### 必要なもの

**Linux:**
```bash
sudo apt-get install git cmake g++ libasound2-dev libssl-dev
```

**macOS:**
```bash
brew install cmake
# Xcode Command Line Toolsが必要
xcode-select --install
```

**Windows:**
- Visual Studio 2019以降
- CMake 3.16以上
- Git

### ビルド手順

```bash
# リポジトリをクローン
git clone https://github.com/example/soluna.git
cd soluna

# ビルドディレクトリを作成
mkdir build && cd build

# 設定（Releaseビルド）
cmake .. -DCMAKE_BUILD_TYPE=Release

# ビルド
make -j$(nproc)  # Linux/macOS
# または
cmake --build . --config Release  # Windows

# インストール（任意）
sudo make install  # Linux/macOS
```

### ビルドオプション

| オプション | デフォルト | 説明 |
|-----------|---------|------|
| `SOLUNA_BUILD_TESTS` | ON | ユニットテストをビルド |
| `SOLUNA_ENABLE_OPUS` | OFF | Opusコーデックを有効化 |
| `SOLUNA_ENABLE_AES67` | OFF | AES67互換モードを有効化 |
| `SOLUNA_ENABLE_DTLS` | OFF | DTLS暗号化を有効化 |
| `SOLUNA_ENABLE_AAC` | OFF | AACコーデックを有効化（fdk-aac必要）|
| `SOLUNA_ENABLE_FLAC` | OFF | FLACコーデックを有効化（libflac必要）|

オプション指定例：
```bash
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DSOLUNA_ENABLE_DTLS=ON \
         -DSOLUNA_ENABLE_OPUS=ON
```

### テスト実行

```bash
cd build
ctest --output-on-failure
```

## プラットフォーム固有の注意

### Linux

**audioグループ:**
デバイスアクセスのためユーザーを`audio`グループに追加：
```bash
sudo usermod -a -G audio $USER
# 変更を反映するにはログアウト後再ログイン
```

**リアルタイム優先度:**
最低レイテンシのためリアルタイムスケジューリングを有効化：
```bash
# /etc/security/limits.conf を編集
@audio - rtprio 99
@audio - memlock unlimited
```

### macOS

**マイク権限:**
入力キャプチャにはマイクアクセスを許可：
システム環境設定 → セキュリティとプライバシー → プライバシー → マイク → ターミナル/アプリを有効化

**オーディオデバイス:**
`default`を使用するか、以下でデバイス一覧を表示：
```bash
solunad --list-devices
```

### Windows

**ファイアウォール:**
WindowsファイアウォールでSolunaを許可：
- 設定 → 更新とセキュリティ → Windowsセキュリティ → ファイアウォール
- アプリにファイアウォール経由の通信を許可 → `solunad.exe`を追加

**オーディオデバイス:**
デフォルトでWASAPI共有モードを使用。排他モードは設定で変更。

## インストールの確認

```bash
# バージョン確認
solunad --version

# オーディオデバイス一覧
solunad --list-devices

# 送信テスト
solunad --tx --device default --dest 239.69.0.1:5004

# 受信テスト（別ターミナルで）
solunad --rx --device default --port 5004
```

## アップグレード

### Linux

```bash
# インストーラースクリプトを使用
curl -sSL https://soluna.dev/install.sh | sudo bash

# apt使用（リポジトリ設定済みの場合）
sudo apt update && sudo apt upgrade soluna
```

### macOS

```bash
brew upgrade soluna
```

### Windows

最新インストーラーをダウンロードして実行。既存インストールをアップグレードします。

## アンインストール

### Linux

```bash
# アンインストールスクリプトを使用
curl -sSL https://soluna.dev/uninstall.sh | sudo bash

# 手動
sudo systemctl stop soluna
sudo systemctl disable soluna
sudo dpkg -r soluna
sudo rm -rf /etc/soluna
```

### macOS

```bash
brew uninstall soluna
```

### Windows

設定 → アプリ → Soluna → アンインストール

## 次のステップ

- [設定リファレンス](configuration.md) - 用途に合わせてSolunaを設定
- [APIリファレンス](api.md) - プログラムからSolunaを制御
- [トラブルシューティング](troubleshooting.md) - 一般的な問題の解決
