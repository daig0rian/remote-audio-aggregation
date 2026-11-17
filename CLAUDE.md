# Remote Audio Aggregation — ソリューション仕様概要

作成日: 2026-05-14  
最終更新: 2026-05-22

---

## 既知の問題 / TODO

| # | 問題名 | 優先度 | 状態 |
|---|--------|--------|------|
| B-01 | Mixer以遠低頻度ポップノイズ | 🔵 低 | ✅ 解決済み |
| B-02 | 音声開始時の単発ノイズ | 🔵 低 | 🔲 未解決 |

### B-01: Mixer以遠低頻度ポップノイズ

**現象:** ブラウザ再生中に約30〜60秒に1回の頻度で単発のポップノイズが発生する。

**切り分け済みの範囲（原因でないことが確認済み）:**
- Win32クライアント側: FFmpegで受信したWAVがクリア → クライアントは問題なし
- Opusデコード段（`clients.js`）: チャープをバイパスして rawPCM をミキサーに直接送っても発生
- フレーム上書き（mixer.js の単一スロット問題）: FIFOキューに変更後も発生
- デコーダリセット（`_lastDecoded > 300ms` 条件）: バイパス後も発生

**根本原因（解決済み）:** `mixer.js` のジッターバッファ実装における `playoutTs` の誤った進め方。PLC（パケットロス補間）のサイクルでも `playoutTs` を960進めていたため、遅延到着した実フレームが「古いエントリ」として即座に削除されるフィードバックループが発生。1回のPLCが次のPLCを引き起こし、最終的にバッファが完全崩壊していた。

**修正内容:** PLC時は `playoutTs` を据え置き、次サイクルで同スロットの実フレームを再試行できるようにした。`playoutTs` の前進と古エントリの削除は実フレーム再生時のみ実施。スターベーション時は `plcCount` をリセットして次スロットでPLCを再試行できるようにした（`mixer.js` の `mix()` 関数）。

**関連コード:**
- `server/src/mixer.js`: `mix()` 関数内のPLC/スターベーション分岐

### B-02: 音声開始時の単発ノイズ

**現象:** talkspurt 開始直後に「ジ」「グ」といった短いデジタルアーティファクト音が乗ることがある。毎回発生するわけではなく、発生時も音の性質が一定でない。位相依存のクリックではない。

**切り分け済みの範囲（原因でないことが確認済み）:**
- サーバー側のデコーダリセット・ジッターバッファリセット: 正常動作確認済み（3回鳴らして3ペアのリセットログ）
- ブラウザの `nextPlay` スケジューリング: サーバーは無音中も 50fps で配信するため underrun は発生しない
- mixer.js での 10ms PCM フェードイン: 実装したが聴取上の改善なしで revert（位相起因でないため当然）

**根本原因候補（未特定）:**
1. サーバー側 Opus エンコーダ（`mixer.js` の `encoder` は共有・連続）の内部状態が無音期間中に適応し、talkspurt 再開の最初のフレームで予測誤差が大きくなりアーティファクトが混入する
2. ブラウザ Opus WASM デコーダが無音→音声の境界で内部状態遷移時にアーティファクトを出す
3. ジッターバッファ pre-buffer 中に届いた最初の 1〜2 フレームに Opus デコーダ初期化アーティファクトが含まれている（クライアント側デコーダリセット直後の最初フレーム）

**未試行の対処案:**
- talkspurt 開始時（reset=true）にサーバー側 Opus エンコーダ（`encoder`）を再生成してリセットする
- ブラウザ側 decoder の `reset()` を音声開始（フレームサイズが閾値を超えた瞬間）に呼ぶ

---

## 現在の状況

| フェーズ | 状態 | 備考 |
|---------|------|------|
| Phase 0A: Node.js音声PoC | ✅ 完了 | @evan/opus, Float32Array mix, Worker Thread timing 確認済み |
| Phase 0B: WASAPI PoC | ✅ 完了 | IAgileObject 必須, GetMixFormat は別デバイスから取得, マスターボリューム独立を確認 |
| Phase 1: サーバー音声コア | ✅ 完了 | UDP受信・Opusミックス・WebSocket配信・REST API・RTPジッターバッファ・ログ基盤 動作確認済み |
| Phase 2: Win32クライアント | ✅ 完了 | トレイUI・INI設定・.ico・音声パイプライン完成。インストーラー不要（exe単体をGitHub Releasesから配布）。Classic Loopback フォールバック実装済み |
| Phase 3: サーバー管理WebUI | ✅ 完了 | クライアント一覧・音量・ミュート・削除・名前編集・3状態表示 動作確認済み |
| クライアント無音帯域削減 | ✅ 完了 | DTX heartbeat ~5pps、Markerビット RFC 7587準拠、isSilence閾値修正 |
| Phase 4: Zabbixモジュール | ✅ 完了 | ARI不要のシンプル構成。単一トグルボタン・メーターバー・Mixer Settingsリンク。動作確認済み |
| Phase 5: Win32プレイヤー（raa-player.exe） | 🔲 実機未確認 | モダンブラウザのない Windows（Server 2019 等）向けのトレイ常駐プレイヤー。WebSocket 受信・遅延上限つきジッターバッファ・自動再接続 |

### 開発環境
- **Windowsホスト**: 192.168.11.33（KVMホスト）
- **サーバーVM**: 192.168.11.38（Ubuntu 24.04, Node.js v24 via nvm, ~/raa-server）
- **ZabbixVM**: 192.168.11.34（`virsh -c qemu:///system start zabbix70` で起動）
- **開発中のデプロイ**: `server\deploy.bat` でSCP転送＋自動再起動（サービス経由）
- **Zabbixモジュールデプロイ**: `scp -r module/. root@192.168.11.34:/usr/share/zabbix/modules/raa_monitor/`
- **本番インストール**: `install.sh` ワンライナー（GitHub Releases から取得）
- **サーバーログ**: `ssh 192.168.11.38 'journalctl --user -u raa-server -f'`（JSON形式、pino）
- **ログレベル切り替え**: 環境変数 `LOG_LEVEL=debug`（デフォルト: `info`）
- **systemdサービス**: `systemctl --user` で管理（`~/.config/systemd/user/raa-server.service`）

### Phase 0B で判明した重要事項（Win32クライアント実装時に必要）
- `IActivateAudioInterfaceCompletionHandler` は `IAgileObject` も実装必須
- `GetMixFormat()` はProcessLoopbackクライアントでは `E_NOTIMPL` → デフォルト出力デバイスから別途取得
- アプリマニフェストに Windows 10 互換宣言（`{8e0f7a12-...}`）が必要
- `RoInitialize(RO_INIT_MULTITHREADED)` を使うこと（Process Loopback パス）。Classic Loopback パスは従来の `CoInitializeEx(NULL, COINIT_MULTITHREADED)` + `IMMDeviceEnumerator`。
- MSVC: VS Build Tools 2026 v14.51 + Windows 11 SDK 10.0.26100.0
- ビルドスクリプト: `poc\phase0b\build.bat`

### サーバー実装の重要事項
- `@evan/opus`: `Encoder`/`Decoder` クラス、`decode()` は `Uint8Array` を返す
- Worker Thread で 20ms タイマーによりミックス→エンコード→postMessage
- **RTPタイムスタンプ駆動ジッターバッファ** (`mixer.js`): PLC時は `playoutTs` を据え置き、実フレームが届いたサイクルで前進させる。スターベーション後は jitter buffer 内の最古フレームに自動 resync。
- **ミュート解除時のリセット**: ミュート中に溜まった stale フレームと `playoutTs` をクリアして再プライム。
- **DTX漏れフレーム対策**: 無音中の散発的ノイズフロアフレームはデコーダリセット（`gap 300ms`）を発火させるが、`_audioStreak < 3` の場合はログ抑制。
- **WebSocket 送信の上限**: `ws.send()` は相手の受信速度を見ずに送信待ちを積むため、送信待ち（`ws.bufferedAmount`）が 32KB（約 4 秒分）を超えたリスナーにはフレームを送らない（`main.js`）。受信の遅いリスナー 1 つでメモリが増え続けるのを防ぐ。捨て始めと追いついたときに warn / info ログが出る。
- **ブラウザバッファ**: `BUFFER_MS = 200`（`public/index.html`）
- **ログ**: `pino`（`src/logger.js`）、`LOG_LEVEL` 環境変数で制御。`info`（デフォルト）/ `debug` / `warn`
  - 5秒ごとの統計（`mix cycle stats` / `event loop delay`）は情報量が多いため `debug` レベル。既定の `info` では出ない

#### systemd ユーザーサービス（実施済み）

`install.sh` が `~/.config/systemd/user/raa-server.service` を自動生成・登録する。
手動で操作する場合のコマンド:

```bash
systemctl --user status raa-server
systemctl --user restart raa-server
journalctl --user -u raa-server -f
```

`deploy.bat` による開発デプロイ後の再起動も `systemctl --user restart raa-server` を使うこと。

**設計上の重要事項（ユーザースコープ採用の理由）:**
- Node.js は nvm でユーザースコープにインストールされているため、`sudo npm install` では nvm の node が見えない → ネイティブアドオン（`@evan/opus`）が別バージョンでビルドされ `NODE_MODULE_VERSION mismatch` でクラッシュする
- `~/raa-server` + `systemctl --user` + `loginctl enable-linger` の構成で、ログイン不要のブート時起動を `sudo` なしで実現できる
- `sudo` が必要なのは `apt-get`（ビルドツール導入）と `loginctl enable-linger`（初回のみ）の2回だけ

### サーバーデプロイの重要事項（SSH 経由）
- **nvm は非インタラクティブシェルで初期化されない** → `bash -lc` も動作しない場合がある。node は絶対パスで指定すること:  
  `/home/daigo/.nvm/versions/node/v24.15.0/bin/node`（サーバーのユーザーは `daigo`、`/root` ではない）
- **pkill は対象プロセスが存在しない場合に exit 1 を返す** → SSH がそれをエラーとして伝播する。  
  起動前に `pgrep -f main.js` で生死確認してから kill するか、`kill $(pgrep -f main.js) 2>/dev/null; true` のように逃がす。  
- **`pkill -f main.js` の危険な罠** → SSH 経由で実行すると、bash プロセス自身のコマンドライン引数に `main.js` が含まれるため、bash ごと kill して SSH が 255 で終了する。PID を直接指定して kill すること: `kill $(ps aux | grep 'node src/main.js' | grep -v grep | awk '{print $2}')`
- **`pgrep` は非インタラクティブ SSH シェルでは PATH に含まれない** → `/usr/bin/pgrep` と絶対パスで呼ぶこと。
- **バックグラウンド起動は `& disown` で行う** → `nohup ... >> /tmp/raa.log 2>&1 & disown` の形が安定。
- **`deploy.bat` 内の再起動コマンド** は上記を踏まえて絶対パスを使うこと（現状の `deploy.bat` は `node` を PATH 経由で呼んでいるため失敗することがある）。

### 仮想BGMクライアント（再生テスト用）

サーバー起動時から常時動作する内蔵の仮想クライアント。Win32 クライアントが1台もない状態でもブラウザやZabbixモジュールの再生テストができるようにするための機能。

**音源ファイル**
- `server/assets/goldberg-var1.opus`（Ogg Opus、48kHz モノラル 64kbps、1分54秒 / 約1MB）
- Bach《ゴルトベルク変奏曲 BWV 988 – 第1変奏》、演奏: Shelley Katz
- 出典: musopen.org、ライセンス: CC Public Domain Mark 1.0（商用利用・改変・再配布自由）

**コード構成**
- `server/src/ogg-reader.js`: 外部ライブラリ不使用のミニマルな Ogg ページパーサー。起動時に全フレームをメモリ上の `Buffer[]` として展開する
- `server/src/main.js` 内の `sendBgmFrame()`: 20ms タイマーで1フレームずつ `clients.setLastFrame()` 経由でミキサーに送信

**旧チャープとの違い**
| 項目 | 旧チャープ（chirp000） | 新BGMクライアント（bgmtest0） |
|------|----------------------|------------------------------|
| 音源 | その場で生成するサイン波チャープ | 事前エンコード済み Opus フレーム |
| 送信パス | `postRawPCM`（Opusデコード段バイパス） | `setLastFrame`（実クライアントと同一パス） |
| 目的 | ミキサー以降の診断 | エンドツーエンド再生テスト |

**設計上の注意点**
- ループ折り返し時（`bgmFrameIndex === 0`）に `marker=true` を立てて mixer 側のデコーダリセットを発火させる。音声内容が不連続なため正しい動作。
- WebUI フレンドリーネーム `"Test BGM (Goldberg Var.1)"` は起動時に `clients.update()` で書き込む。ただし config.json に名前が既に存在する場合（ユーザーがWebUIでリネーム済み）は上書きしない。
- BGM クライアントは毎 20ms に `clients.touch()` を呼ぶため、cleanup による自動削除は発生しない。
- Ogg Opus の先頭2ページ（OpusHead + OpusTags）はヘッダーなのでスキップする。ラッピング（255バイトセグメント連結）の再組立ても ogg-reader.js が処理する。

### Win32クライアント WASAPI チャンネル変換の重要事項
- `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY` フラグを指定すれば、  
  `WAVEFORMATEXTENSIBLE` で `nChannels=1` / `dwChannelMask=SPEAKER_FRONT_CENTER` を要求するだけで  
  WASAPI が自動的にステレオ→モノラルのダウンミックスを行う。アプリ側で L+R 平均を取る処理は不要。
- Opus エンコーダも `opus_encoder_create(48000, 1, ...)` に合わせること（チャンネル数の不整合はクラッシュする）。
- サーバー側 `mixer.js` の `CHANNELS` も同じ値にすること。クライアントとサーバーで不一致があると無音になる。

### Win32プレイヤー（raa-player）の重要事項

ソースは `player/src/raa-player.cpp`。libopus は `client/deps` を共用（`player/build.bat` が無ければ `client/get_opus.bat` を呼ぶ）。

- **受信のあらゆる待ちにタイムアウトを持たせる。** 半開きの TCP 接続で無音のまま止まると、監視用途では警告音を聞き逃す。そのため WebSocket（RFC 6455 受信側）を Winsock 上に最小実装し、select の 200ms 刻みで停止要求と無受信を確認している。サーバーは無音中も 50fps で送るので、3 秒受信がなければ切断とみなして再接続する（バックオフ 1→10 秒）。
- **遅延に上限を持たせる。** ①受信は常に読み切り、サーバー側に送信待ちを溜めない（溜まるとサーバーがフレームを捨て始める）。②ジッターバッファは無音フレーム（ピーク < -60 dBFS）を間引き/複製して `buffer_ms` ±60ms に寄せ、時計のずれを吸収する。③ `buffer_ms` + 500ms を超えたら先へ飛ばす。いずれもログの `Stats:` 行（10 分ごと）で回数を確認できる。
- **出力**: WASAPI 共有モード + `EVENTCALLBACK | AUTOCONVERTPCM`、48kHz モノラル float を渡し、デバイス形式への変換は WASAPI に任せる。既定デバイスの変更は `IMMNotificationClient` で検知して開き直す。デバイスが使えない間は受信フレームを溜めない。
- **raa-client と同じ PC では使わない。** Process Loopback は自プロセスしか除外できないため、raa-player の出力を拾って送り返しループする。
- 設定: `%APPDATA%\raa-player\raa-player.ini`（`[Connection] server_host / server_port`、`[Playback] buffer_ms`、`[General] autostart`）。音量設定は持たない（Windows の音量ミキサーとサーバー WebUI の 2 か所で足りる）。

### リリースワークフロー（GitHub Actions）

タグ `v*` を push すると `.github/workflows/release.yml` が自動実行される。

```
git tag v0.x.y && git push origin v0.x.y
  ├─ server-pack (ubuntu-latest)
  │    npm version $TAG  # package.json バージョンをタグに合わせる
  │    npm pack          # raa-server-0.x.y.tgz を生成
  │    → GitHub Release 作成 + raa-server-0.x.y.tgz / install.sh をアップロード
  └─ client-build (windows-latest) ← server-pack 完了後に実行
       client\build.bat  # libopus 自動ビルド → raa-client.exe
       player\build.bat  # 同じ libopus を使って → raa-player.exe
       → 同リリースに raa-client.exe / raa-player.exe をアップロード
```

**注意点:**
- `npm pack` はファイル名を `package.json` の `version` から取る → ワークフロー内で `npm version $TAG` を先に実行してタグと一致させる
- `client-build` は `needs: server-pack` でリリースが先に作成されることを保証してから exe をアップロード
- `install.sh` の `releases/latest/download/install.sh` URL は常に最新タグを指す

---

## 背景・動機

### 廃止した旧ソリューション
- **loopcast** (C#/.NET Windows クライアント) + **ConfBridgeMonitor** (Zabbix モジュール) + **FreePBX/Asterisk** (サーバー)
- SIP中心の設計。Asteriskに音声ミキシングを任せることで自前実装を避けた
- **廃止理由:** 複雑なNATが絡むネットワークでSIPのNAT透過性が低く、安定動作しなかった

### 新ソリューションの方針
- 通信プロトコルを「パケットが届けば鳴る」シンプルなUDPベースに変更
- クライアント→サーバーはUDP送りっぱなし（一方向、fire & forget）
- リスナー→サーバーはWebSocket（ブラウザからのアウトバウンド接続）
- NAT透過性: 両方向ともクライアント起点のアウトバウンド通信のみ

---

## アーキテクチャ概要

```
[Windows PC-A] ──UDP:5004──┐
[Windows PC-B] ──UDP:5004──┤──→ [Linux サーバー] ──WebSocket:8080──→ [Zabbix ブラウザ]
[Windows PC-C] ──UDP:5004──┘          │ HTTP:8080
                                    WebUI (管理画面)
```

### ネットワーク要件（サーバーがNAT内の場合）
- UDP 5004: ポート転送（クライアントからの音声受信）
- TCP 8080: ポート転送（WebUI + WebSocket配信）
- 計2本のポート転送のみ

---

## Part 1: 音声収集クライアント

### 技術スタック
- 言語: **C++ / Win32 API ネイティブ** (MSVC)
- 音声キャプチャ: **WASAPI COM直接** (`IAudioClient` + `AUDCLNT_STREAMFLAGS_LOOPBACK`)
  - **Process Loopback**（デフォルト、ビルド 19041 以上）: `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK` + `ActivateAudioInterfaceAsync` でマスターボリューム非依存キャプチャ。`RoInitialize(RO_INIT_MULTITHREADED)` 使用。
  - **Classic Loopback**（フォールバック、全 Windows 10）: `IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender)` でデフォルト出力デバイスをキャプチャ。マスターボリュームの影響を受ける。`CoInitializeEx(COINIT_MULTITHREADED)` 使用。
  - ビルド 19041 未満では Classic Loopback に自動切り替え。設定画面の「オーディオ」タブでユーザーが手動選択も可能（非対応ビルドでは Process Loopback ラジオをグレーアウト）。
  - 参照実装: `loopcast/client/LoopcastUA/src/Audio/ProcessLoopbackCapturer.cs`
- コーデック: **libopus** (静的リンク、外部DLL不要)
  - DTX有効 (`OPUS_SET_DTX(1)`) → 無音時はパケット送信をスキップ（帯域99%削減）
  - パラメータ: **48kHz, 20ms フレーム, モノラル, 64kbps**
  - 用途はNMS警告音（人の声より広い帯域）のため48kHzを維持。Opusの対応レートは 8k/12k/16k/24k/48kHz のみ（44.1kHz は非対応）
- ネットワーク: Winsock2 UDP送信（一方向のみ）
- UI: `Shell_NotifyIcon` システムトレイ（.icoファイル使用、active/silent/error の3状態）
- 設定: `%APPDATA%\raa-client\raa-client.ini`（Windows INI API）

### 無音抑制 (DTX)
```cpp
opus_encoder_ctl(encoder, OPUS_SET_DTX(1));
int len = opus_encode_float(encoder, pcm, 960, buf, buf_size);
if (len > 2) send_packet(buf, len);  // 無音時はlen<=2なので送らない
```

### UX フロー
1. GitHub Releases から `raa-client.exe` をダウンロードして直接実行（インストーラー不要）
2. 初回起動 → 設定ダイアログ自動表示
   - **サーバーIPアドレス** 入力欄
   - (詳細設定) サーバーポート（デフォルト: 5004）
   - OK → 即座に音声送信開始
3. 以後はシステムトレイに常駐、PC起動時に自動起動
4. トレイ右クリック: 設定を開く / 終了
5. **設定ダイアログにSSRCを表示**（サーバー管理画面との照合用）

### 接続状態フィードバック
**なし（fire & forget）。** UDPの性質上、サーバーへの到達可否はクライアントから検出不可。ユーザーはサーバー管理WebUIで疎通を確認する。（旧世代の実装と同じ方針）

### パケットフォーマット（RTP — RFC 3550 + RFC 7587）

標準RTPパケットなので **FFmpeg・Wireshark・VLC などの汎用ツールがそのまま使える**。

```
[1B]  0x80          (V=2, P=0, X=0, CC=0)
[1B]  M | PT=111    (Marker bit + Payload Type 111 = Opus)
[2B]  シーケンス番号 (big-endian)
[4B]  タイムスタンプ (48kHz ticks, big-endian)
[4B]  SSRC          (big-endian, クライアント識別子)
[NB]  Opusペイロード (20ms, 48kHz, mono)
```

- Marker bit: 無音後の最初のパケットで 1 にセット（RFC 7587 §4.1）
- ペイロード長フィールドなし — UDPパケット長 − 12 がペイロード長
- **RTPタイムスタンプは RFC 7587 により Opus は常に 48kHz クロック固定。** エンコーダのサンプリングレートに関わらず 20ms ごとに +960。コード上は `FRAME_SAMPLES` と別定数 `RTP_TS_PER_FRAME=960` に分離済み（将来レートを変えても timestamp が壊れないように）

**FFmpegでの受信例（SDP経由）:**
```
ffmpeg -protocol_whitelist file,udp,rtp -i raa.sdp -f wav output.wav
```
```ini
; raa.sdp
v=0
o=- 0 0 IN IP4 0.0.0.0
s=RAA
c=IN IP4 0.0.0.0
t=0 0
m=audio 4010 RTP/AVP 111
a=rtpmap:111 opus/48000/2
a=fmtp:111 stereo=0
```
※ `rtpmap` の `/2` は RFC 7587 の規定で mono/stereo 問わず常に 2 と書く。実際のチャンネル数は `fmtp` の `stereo=0`（mono）で示す。

**Wireshark**: Analyze > Decode As > RTP（UDP port 4010）

### 設定ファイル
- `%APPDATA%\raa-client\raa-client.ini`（Windows INI API: `WritePrivateProfileString` / `GetPrivateProfileInt`）
- セクション: `[Connection]` server_ip / server_port / ssrc、`[Audio]` bitrate_kbps / loopback_mode（0=Process/1=Classic）、`[Detection]` silence_guard_ms / audio_guard_ms、`[Batch]` start_script / stop_script / timeout_ms、`[General]` autostart
- **初回起動フロー**: INI 未存在 → SSRC 生成 → 設定ダイアログ自動表示 → OK で INI 初回保存 → 音声スレッド開始。Cancel → アプリ終了。
- SSRC は INI に保存。アンインストールしない限り変わらない。

### メモリフットプリント実績
| 構成 | RSS |
|------|-----|
| 旧MFC版（生PCM送信、Opusなし） | 4MB 未満 |
| 旧.NET版 loopcast | 10MB 強 |
| 本実装 stereo / 128kbps | 8.1MB |
| 本実装 **mono / 64kbps**（現在） | **2.4MB** |

モノラル化によりOpusエンコーダ内部バッファとWASAPIフォーマット変換バッファが半減し約70%削減。

---

## Part 2: サーバー

### 開発・検証環境（実績）

| 項目 | 値 |
|------|----|
| ホスト | 192.168.11.33（KVM/libvirt ホスト） |
| VM IP | 192.168.11.38 |
| VM 名 | raa-server |
| OS | Ubuntu 24.04.4 LTS（Noble Numbat） |
| Kernel | 6.8.0-106-generic |
| vCPU | 1（Intel i5-6500T 2.50GHz） |
| メモリ | 2 GB |
| ディスク | 20 GB |
| 自動起動 | 有効（virsh autostart raa-server） |
| Node.js | v24.15.0（nvm 管理、LTS 2028年4月まで） |
| `@evan/opus` | Node 24 にて動作確認済み |

> Phase 0A・Phase 1 の開発はこの VM 上で実施。
> 本番環境への移行時は同じ Ubuntu 24.04 + nvm 構成を踏襲すること。

### 技術スタック
- 言語: **Node.js**
- OS: **Linux**
- 音声処理: `@evan/opus`（`@discordjs/opus` の後継、Node.js v22以降対応）
- Web: `fastify`（REST API + 静的ファイル）+ `ws`（WebSocket）
- 設定永続化: JSON ファイル (`config.json`)

### 主要パッケージ
```json
{
  "dependencies": {
    "@evan/opus":      "^1.x",
    "ws":              "^8.x",
    "fastify":         "^4.x",
    "@fastify/static": "^6.x",
    "uuid":            "^9.x"
  }
}
```

### ディレクトリ構成
```
server/
├── package.json
├── src/
│   ├── main.js       # 起動・各モジュールの接続
│   ├── udp.js        # dgram UDP受信・パケット解析・クライアント登録
│   ├── clients.js    # クライアントレジストリ・config.json 永続化
│   ├── mixer.js      # 20ms ミキシングループ（Worker Thread）
│   ├── stream.js     # WebSocket /stream ブロードキャスト
│   └── api.js        # fastify REST API + 静的WebUI配信
├── static/
│   ├── index.html
│   ├── app.js
│   └── style.css
└── config.json       # クライアント設定の永続化
```

### 音声パイプライン（20msサイクル）

```
[main thread]
  dgram UDP受信 → パケット解析 → クライアント毎の直近フレームを更新
  fastify HTTP/REST
  ws WebSocket管理

[Worker Thread: mixer.js]
  setInterval 20ms:
    各クライアントの直近Opusフレームを取得（なければ無音）
    → opus.decode() × N  … libopus C呼び出し（N-API、高速）
    → Float32Array で PCM合算 + 音量スケーリング + クリップ
    → opus.encode() × 1
    → postMessage(encodedBuffer) → main thread → ws broadcast
```

**Worker Thread を使う理由:** Opus decode/encode はN-API経由でも同期呼び出しのため、main threadで実行するとイベントループを数ms ブロックする。Worker に分離することでWebSocket/HTTP応答性を維持する。

**クライアントからパケットが来ないフレーム:** 無音（Float32Array のゼロ）として混合。DTXによるスキップと通常のパケットロスを同じ方法で扱う。

**ノイズフロアによる誤検知対策:** Opus エンコーダは主観的無音時でも ~5% の確率で DTX 非発動フレーム（`len > 2`）を出力する（WASAPI が `AUDCLNT_BUFFERFLAGS_SILENT` を付けないケースが散発的にあるため）。これを `last_audio` の更新に使うと、ブラウザの「音声受信中」表示が消えなくなる。対策: **連続 3 フレーム以上** 非無音が続いた場合のみ `last_audio` を更新する「ストリークフィルタ」を `clients.js` に実装済み。無音になった瞬間にストリークはリセット。

### パケット解析（Node.js Buffer API）
```javascript
// RFC 3550 RTP header parse
if (buf.length < 12) return;
if ((buf[0] >> 6) !== 2) return;         // version must be 2

const cc           = buf[0] & 0x0F;
const hasExtension = (buf[0] & 0x10) !== 0;
let   payloadStart = 12 + cc * 4;
if (hasExtension) {
    const extWords = buf.readUInt16BE(payloadStart + 2);
    payloadStart  += 4 + extWords * 4;
}

const ssrc     = buf.readUInt32BE(8);
const clientId = ssrc.toString(16).padStart(8, '0');  // e.g. "a1b2c3d4"
const opusData = buf.subarray(payloadStart);
```

### クライアント管理データモデル
```json
{
  "clients": {
    "a1b2c3d4": {
      "friendly_name": "会議室A-PC",
      "volume": 0.7,
      "muted": false,
      "created_at": "2026-05-14T10:00:00Z",
      "last_seen": "2026-05-14T12:34:56Z"
    }
  }
}
```

- キーは SSRC を 8桁 lowercase hex にした文字列（例: `"a1b2c3d4"`）
- 新 SSRC からの初パケット到着時に自動エントリ生成（volume=1.0, muted=false）
- 設定変更は即時ファイルに書き込み（クライアントオフライン後も保持）

### WebUI 機能
| 要素 | 内容 |
|------|------|
| フレンドリーネーム | 編集可能、SSRC より目立つ位置 |
| SSRC | フレンドリーネームの下に小さく表示（8桁 hex） |
| ステータス | 🟢 受信中 / ⚫ 未受信（最終受信時刻表示） |
| 音量スライダー | 0〜200%（100%に中央マーカー） |
| ミュートトグル | クライアント個別 |

### REST API
| エンドポイント | 機能 |
|---------------|------|
| `GET /api/clients` | クライアント一覧（ステータス含む） |
| `PATCH /api/clients/{ssrc}` | friendly_name / volume / muted 更新 |
| `DELETE /api/clients/{ssrc}` | エントリ削除 |
| `WS /stream` | 混合済みOpusフレーム配信（バイナリ） |

### サーバーインストール手順

**方針: 素のVM + nvm + systemd --user**
Dockerは使わない。専用VMに1サービスだけ動かす構成ではDockerの隔離メリットが薄い。
Node.js は nvm でユーザースコープ管理し、systemd ユーザーサービスで起動する。

#### 手順（Ubuntu 24.04 LTS）

```bash
# 1. Node.js ≥ 22（OS パッケージが 22 以上なら sudo apt install nodejs でよい。Ubuntu 24.04 は 18 なので nvm）
curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.39.0/install.sh | bash
source ~/.bashrc
nvm install 22
nvm alias default 22

# 2. raa-server インストール（展開・サービス登録まで一括）
curl -fsSL https://github.com/daig0rian/remote-audio-aggregation/releases/latest/download/install.sh | bash
```

`install.sh` が行う処理:
- Node.js ≥ 22 チェック（なければエラー終了）。`package.json` の `engines` も `>=22`（OS パッケージの Node.js 22 で動かすため。依存パッケージの要求は最大でも `20 || >=22`）
- `~/raa-server` にパッケージを展開（既存の `node_modules` は削除してから展開、`config.json` は保持）
  - tgz に `node_modules` を同梱（`package.json` の `bundleDependencies: true`、CI で `npm ci --omit=dev` 後に `npm pack`）。インストール時の `npm install`・registry.npmjs.org 接続・`build-essential`/`libopus-dev` は不要
  - `@evan/opus` は全プラットフォーム分のプリビルド N-API バイナリ（libopus 静的リンク）を同梱しているため、Node.js メジャーバージョンが変わっても再ビルド不要
- `~/.config/systemd/user/raa-server.service` を生成・登録・起動
- `loginctl enable-linger`（ブート時起動許可、sudo、初回のみ）

#### 日常的なトラブルシューティング
```bash
systemctl --user status raa-server
journalctl --user -u raa-server -f
journalctl --user -u raa-server --since "1 hour ago"
systemctl --user restart raa-server
```

### 長期メンテナンス（5〜10年運用）

#### 依存関係の階層
```
VM (Ubuntu 24.04 LTS)       ← 2029年に次のLTSへ dist-upgrade
  └─ nvm                    ← シェルスクリプト、非常に安定
       └─ Node.js LTS       ← 3年ごとに nvm で切り替え
            └─ npm packages ← package-lock.json で管理、年1回更新
                 └─ libopus ← @evan/opus に静的リンク済み、コーデック仕様は2012年以来不変
```

#### 年次パッケージ更新（所要30分程度）
```bash
cd ~/raa-server
npm outdated              # 更新確認
npm update                # マイナー・パッチ更新
systemctl --user restart raa-server
```

#### Node.js メジャーバージョン更新（3年ごと）
```bash
nvm install 26            # 新しいLTSをインストール
nvm alias default 26
systemctl --user restart raa-server   # @evan/opus は N-API のため再ビルド不要
# install.sh を再実行するとサービスファイルの ExecStart パスも更新される
```

---

## Part 3: Zabbix 音声聴取モジュール

### 技術スタック
- ConfBridgeMonitor と同一の Zabbix モジュール構造（PHP + JS）
- ARI不要、PHPプロキシ不要、SIP不要

### ディレクトリ構成
```
module/
├── manifest.json
├── Widget.php
├── actions/WidgetView.php
├── includes/WidgetForm.php
├── views/
│   ├── widget.edit.php
│   └── widget.view.php
└── assets/
    ├── css/widget.css
    └── js/
        ├── class.widget.js
        ├── raa-player.js          # 新規実装（cbm-player.js の大幅簡略版）
        └── opus-decoder.bundle.js # ConfBridgeMonitor から流用
```

### raa-player.js の接続ロジック
```javascript
// ConfBridgeMonitorのcbm-player.jsと比べてはるかにシンプル
const ws = new WebSocket(`ws://${serverHost}:${serverPort}/stream`);
ws.binaryType = 'arraybuffer';
ws.onmessage = (e) => this._onFrame(e);  // _onFrame()はcbm-player.jsから流用
```

### ウィジェット設定項目
- サーバーホスト（IP / FQDN）
- WebSocket ポート（デフォルト: 8080）
- バッファサイズ（ms、デフォルト: 100）

---

## 既存資産の再利用

| 資産 | 出典プロジェクト | 再利用方法 |
|------|----------------|-----------|
| `ProcessLoopbackCapturer.cs` | loopcast | C++ WASAPI実装の参照 |
| `LoopbackCapturer.cs` | loopcast | 同上 |
| `OpusEncoder.cs` | loopcast | libopus C API使用の参照 |
| `cbm-player.js` の `_onFrame()` | ConfBridgeMonitor | raa-player.js にほぼそのままコピー |
| `opus-decoder.bundle.js` | ConfBridgeMonitor | そのまま流用 |
| モジュール構造全体 | ConfBridgeMonitor | Widget.php 等を改修ベースとして使用 |

---

## 開発フェーズ

技術リスクの高い箇所を先に潰し、音が出ることをできるだけ早い段階で確認する順序とする。

### Phase 0A: Node.js 音声 PoC ✅ 完了
**結果:**
- `@discordjs/opus` v0.9.0 は Node.js v22/v24 で使用不可（プリビルドなし、ソースビルドも失敗）
- **`@evan/opus`** に変更。Node.js v22 で正常動作確認済み（API: `Encoder` / `Decoder`、`decode()` は Uint8Array を返す）
- 40ストリーム同時処理: **15.1ms**（JITウォームアップ後）— 20ms予算内
- Worker Thread `setInterval(20ms)` ジッター: 平均20.6ms、標準偏差0.19ms、外れ値0% ← 良好
- サーバーVM: `raa-server` (192.168.11.38), Ubuntu 24.04 LTS, Node.js v22.22.3, 自動起動設定済み

### Phase 0B: Win32 WASAPI PoC
**目的:** C++ での WASAPI Process Loopback がマスターボリューム非依存で動くことを確認  
**完了の定義:** マスターボリュームを変化させても PCM レベルが変化しないことをコンソール出力で確認

- C++ コンソールアプリで WASAPI loopback キャプチャのみ実装
- `AUDCLNT_STREAMFLAGS_PROCESS_LOOPBACK` の挙動確認
- 参照: `loopcast/client/LoopcastUA/src/Audio/ProcessLoopbackCapturer.cs`

### Phase 1: サーバー音声コア ✅
**目的:** WebUI なしで音声パイプラインを完成させ、ブラウザで音が聞こえることを確認  
**完了の定義:** 疑似クライアント（FFmpeg または Python スクリプト）からの音声がブラウザで再生できる  
**結果:** 疑似クライアント（fake-client.js）から RAA1 パケットを送信し、WebSocket 経由でブラウザへの Opus フレーム配信を確認。音声デコード・再生は Phase 4 で実装。

- UDP 受信・パケット解析
- Worker Thread で decode × N → PCM 混合 → encode
- WebSocket `/stream` 配信
- コンソールで UUID・パケット受信状況を表示
- テスト用の簡易 HTML ページ（`raa-player.js` の骨格）で音声確認

*Win32 クライアント完成前は FFmpeg やスクリプトで RAA1 パケットを生成して代用する*

### Phase 2: Win32 クライアント
**目的:** 実 PC の音声をサーバー経由でブラウザに届ける  
**完了の定義:** Windows PC で再生した音声が Phase 1 のブラウザページで聞こえる

**実装済み:**
- WASAPI Process Loopback（ビルド 19041+、マスターボリューム非依存）+ Classic Loopback フォールバック（全 Windows 10）
- モノラル、48kHz、32bit float、`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` による自動ダウンミックス
- libopus encode（64kbps、DTX）+ RTP/UDP 送信（RFC 3550 + RFC 7587）
- システムトレイ（.ico 3状態）・5タブ設定ダイアログ・INI永続化・初回起動フロー
- オーディオタブ: キャプチャ API 選択ラジオボタン（ビルド非対応時は Process Loopback をグレーアウト）
- バッチスクリプト実行（音声開始/終了時）・自動起動設定（レジストリ）
- シングルインスタンス制御（Mutex）・ログファイル

**配布:**
- インストーラー不要。GitHub Releases に `raa-client.exe` を置き、ダウンロードして直接実行。
- タグ push（`v*`）で GitHub Actions が自動ビルド・リリースアセットとして添付。

### Phase 3: サーバー管理 WebUI
**目的:** 音量・名前などの設定を管理画面から操作できるようにする  
**完了の定義:** 音量スライダー操作がミキシング結果に反映される

- クライアント一覧（フレンドリーネーム・ステータス・最終受信時刻）
- 音量スライダー（0〜200%）・ミュートトグル・フレンドリーネーム編集
- REST API の完成

### Phase 4: Zabbix モジュール ✅
**目的:** Phase 1 の簡易 HTML ページを Zabbix ダッシュボードパネルに格上げする  
**完了の定義:** Zabbix ダッシュボードの再生ボタンで音声が流れる  
**結果:** Zabbix ダッシュボードウィジェットとして動作確認済み。

**実装済み:**
- `raa-player.js`: WebSocket `/stream` に直接接続するシンプルなプレーヤー（ARI・PHPプロキシ不要）
- PHP スキャフォールド（Widget.php・WidgetForm.php 等）、ConfBridgeMonitor の構造を改修ベースに使用
- 単一トグルボタン（▶ Monitor / ⏹ Stop）・ステータステキスト・音量メーターバー・Mixer Settingsリンク
- `opus-decoder.bundle.js` は ConfBridgeMonitor から流用

**ウィジェット設定項目:** RAA Server Host / WebSocket Port（デフォルト: 4011）/ Buffer ms（デフォルト: 200）

**インストール手順:**
1. `module/` ディレクトリを `/usr/share/zabbix/modules/raa_monitor/` に配置
2. Zabbix → Administration → General → Modules → Scan directory → Enable

**CBM との主な違い:**
- ARI接続・StasisStart待機・PHPプロキシがすべて不要（WebSocket直接接続）
- 2ボタン（Listen/Stop）→ 1ボタントグル（サーバーWebUIと同じ操作感）
