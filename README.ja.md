# Remote Audio Aggregation (RAA)

[English version here](README.md)

複数の Windows PC のシステム音声をキャプチャし、混合した音声を WebSocket 経由でブラウザにストリーミングする低遅延音声集約システムです。SIP・PBX 不要、ポートフォワード 2 本だけで NAT を越えられます。

<table>
  <tr>
    <td align="center"><b>Win32 クライアント</b><br><img src="docs/screenshot-client.png" alt="Win32 クライアント" width="380"></td>
    <td align="center"><b>サーバー管理 WebUI</b><br><img src="docs/screenshot-webui-ja.png" alt="サーバー管理 WebUI" width="380"></td>
  </tr>
</table>

## アーキテクチャ

```
[Windows PC-A] ──UDP:4010──┐
[Windows PC-B] ──UDP:4010──┤──→ [Linux サーバー] ──WebSocket:4011──→ [ブラウザ]
[Windows PC-C] ──UDP:4010──┘           │
                                    WebUI :4011
```

- **クライアント → サーバー**: UDP 一方向（fire & forget）。クライアント側への着信ポートは不要。
- **ブラウザ → サーバー**: WebSocket アウトバウンド接続。ポートフォワード 2 本（UDP 4010・TCP 4011）で NAT 越え可能。
- **音声フォーマット**: Opus 48 kHz モノラル、20 ms フレーム、64 kbps、DTX 有効

## コンポーネント

| コンポーネント | 状態 | 概要 |
|---|---|---|
| [Win32 クライアント](client/) | ✅ 完成 | システムトレイアプリ、WASAPI プロセスループバックキャプチャ、RTP/UDP 送信 |
| [Node.js サーバー](server/) | ✅ 完成 | UDP 受信、ミキサー Worker Thread、WebSocket ブロードキャスト、REST 管理 API |
| Zabbix モジュール | 🔲 未着手 | ブラウザプレイヤーを組み込んだ Zabbix ダッシュボードウィジェット |

## 動作の仕組み

### 音声パイプライン（20 ms サイクル）

1. **Win32 クライアント**が WASAPI Process Loopback でシステム音声をキャプチャ（マスターボリューム非依存）し、libopus でエンコードして RTP パケットを UDP で送信。
2. **サーバーの UDP 受信部**が RTP ヘッダーを解析し、SSRC をクライアント ID として取得。Opus を PCM にデコードし、クライアントごとの RTP タイムスタンプ索引ジッターバッファにエンキュー。
3. **ミキサー Worker Thread** が精密な 20 ms タイマーで動作。各クライアントのジッターバッファからフレームを取り出し（欠損時は PLC）、クライアントごとの音量スケーリングを適用して PCM を混合し、Opus に再エンコードしてメインスレッドに送信。
4. **メインスレッド**がエンコード済みフレームをすべての WebSocket リスナーにブロードキャスト。
5. **ブラウザ**が WebAssembly で Opus をデコードし、Web Audio API でスケジュール再生。

### ジッターバッファ

クライアントごとに RTP タイムスタンプ索引のジッターバッファをミキサー Worker 内に保持:

- 再生開始前に 2 フレーム（40 ms）を事前バッファリング
- フレーム欠損時: 最後のフレームを PLC として最大 2 サイクル（40 ms）繰り返し
- `playoutTs` は PLC 時には進めず、実フレーム受信時にのみ前進。遅延到着フレームを有効活用可能
- バッファオーバーフロー時は `playoutTs` より古いフレームを優先的に破棄。それでも不足する場合は最古フレームを破棄し `playoutTs` をリセットして枯渇連鎖を防止
- ミュート/ミュート解除時: 滞留フレームをクリアし `playoutTs` をリセット

## パフォーマンス

### 検証済みの性能特性

**テスト環境**: 1 vCPU（Intel i5-6500T 2.50 GHz）、2 GB RAM、Ubuntu 24.04 LTS（KVM VM）

**シナリオ: 40 接続クライアント、10 クライアント同時発話**

| 指標 | 値 |
|------|----|
| ミキサーサイクル時間 — 平均 | 0.79 ms |
| ミキサーサイクル時間 — p99 | < 1.0 ms |
| ミキサーサイクル時間 — 最大 | 1.28 ms |
| サーバー CPU 使用率 | ~30 % |
| ロードアベレージ（サーバー＋負荷テストを同一 VM で実行） | ~0.5 |

ミキサーの処理予算は 1 サイクル 20 ms。同時発話 10 クライアントでその予算の 4 % 以下しか消費しない。

### なぜクライアント増に対して安定しているか

3 つの要因が重なっています。

1. **エンコードが O(1)** — 何人が同時発話しても、1 サイクルあたりの Opus エンコードはちょうど 1 回。
2. **デコードの定数が極めて小さい** — libopus は 20 ms モノラルフレームを ~0.3 ms でデコード（N-API ネイティブ呼び出し）。10 クライアントで ~3 ms のデコード処理が 20 ms 窓に分散して発生。
3. **ミックスループが軽微** — N × 960 サンプルの加算処理は 10 クライアントでも ~0.05 ms。

結果として CPU 使用率は `固定コスト + N × 1.5 %` 程度の増加に収まる（固定のエンコードコストが低クライアント数での底上げになるため、クライアント数に正比例はしない）。

### 想定される性能限界

| 同時発話クライアント数 | ミキサー予算消費 | 備考 |
|----------------------|----------------|------|
| 10 | ~4 % | ✅ 1 vCPU / 2 GB で検証済み |
| 25 | ~10 % | 十分な余裕あり |
| 40 | ~70 % | デコード ~12 ms、限界に近づく |
| 50 以上 | > 100 % | フレーム落ちが発生する見込み |

WebSocket 聴取クライアント数の影響は軽微で、~200 同時ブラウザまでほぼ問題なし（WS 送信 1 回あたりの追加コストはフレームヘッダー ~10 バイトのみ、~150 バイトの Opus ペイロードは全クライアントで共有）。

### パフォーマンス監視

サーバーは 5 秒ごとに `info` レベルでタイミング統計をログ出力します。

```json
{"msg":"mix cycle stats","mean_ms":"0.79","p99_ms":"1.0","max_ms":"1.28","avg_active":"10.9"}
{"msg":"event loop delay","mean_ms":"0.12","p99_ms":"0.8","max_ms":"1.1"}
```

`avg_active` は 1 サイクルで実際にミックスされたクライアントの平均数。`event loop delay` はメインスレッドの応答性（UDP 受信・WebSocket ブロードキャスト・HTTP）を反映します。

負荷テストの再現手順:

```bash
# 40 クライアント登録、10 クライアントが音声送信、ローカルホスト宛
node bench/load-test.js 40 10 127.0.0.1
```

## 技術スタック

### サーバー
- **ランタイム**: Node.js ≥ 24（LTS）
- **HTTP/REST**: [Fastify](https://fastify.dev/)
- **WebSocket**: [ws](https://github.com/websockets/ws)
- **Opus コーデック**: [@evan/opus](https://github.com/evan/opus)（N-API ネイティブバインディング）
- **ロギング**: [pino](https://getpino.io/)（構造化 JSON、`LOG_LEVEL` 環境変数で制御）
- **スレッド**: Worker Threads（ミキサーが HTTP/WS イベントループとは独立して動作）

### Win32 クライアント
- **言語**: C++ / Win32 API（MSVC）
- **音声キャプチャ**: WASAPI `AUDCLNT_STREAMFLAGS_PROCESS_LOOPBACK`（マスターボリュームと独立してプロセスの音声をキャプチャ）
- **コーデック**: libopus 1.3.1（静的リンク、外部 DLL 不要）
- **ネットワーク**: Winsock2 UDP、標準 RTP フレーミング（RFC 3550 + RFC 7587）
- **UI**: `Shell_NotifyIcon` システムトレイ（アクティブ / 無音 / エラーの 3 状態アイコン）
- **設定**: `%APPDATA%\raa-client\raa-client.ini`

## ディレクトリ構成

```
remote-audio-aggregation/
├── client/                  # Win32 C++ クライアント
│   ├── src/
│   │   └── raa-client.cpp   # メインソース（WASAPI + libopus + RTP + トレイ UI）
│   ├── deps/                # libopus スタティックライブラリとヘッダー
│   ├── icons/               # active.ico / silent.ico / error.ico
│   ├── build.bat            # MSVC ビルドスクリプト
│   ├── get_opus.bat         # libopus のソースからのビルド
│   ├── app.manifest         # Windows 10 以降互換マニフェスト
│   └── raa-client.rc        # リソースファイル（アイコン、バージョン情報）
└── server/                  # Node.js サーバー
    ├── src/
    │   ├── main.js          # エントリーポイント: UDP・HTTP・WS の接続
    │   ├── udp.js           # RTP パケット受信・解析
    │   ├── clients.js       # クライアントレジストリ、Opus デコード、設定永続化
    │   ├── mixer.js         # Worker Thread: ジッターバッファ・PLC・混合・エンコード
    │   ├── ogg-reader.js    # Ogg ページパーサー（BGM クライアント用）
    │   └── logger.js        # 各モジュール共有の pino インスタンス
    ├── assets/
    │   └── goldberg-var1.opus  # 内蔵テスト音源（パブリックドメイン、~1 MB）
    ├── public/
    │   └── index.html       # 管理 WebUI + ブラウザ音声プレイヤー
    ├── package.json
    ├── deploy.bat           # SCP デプロイ + リモート再起動ヘルパー
    └── raa-server.service   # systemd ユニットファイル
```

## サーバーセットアップ（Ubuntu 24.04 LTS）

### 前提パッケージ

```bash
sudo apt-get update
sudo apt-get install -y build-essential libopus-dev
```

### nvm で Node.js をインストール

```bash
curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.39.0/install.sh | bash
source ~/.bashrc
nvm install 24
nvm alias default 24
```

### インストール

```bash
sudo mkdir /opt/raa-server
sudo chown $USER /opt/raa-server
cp -r server/* /opt/raa-server/
cd /opt/raa-server
npm ci
```

### 起動（フォアグラウンド）

```bash
node src/main.js
# デバッグログを有効にする場合:
LOG_LEVEL=debug node src/main.js
```

デフォルトポート: UDP `4010`（音声入力）、HTTP/WS `4011`（Web インターフェース）。  
環境変数で変更可能: `UDP_PORT=5004 HTTP_PORT=8080 node src/main.js`

### systemd サービスとして起動

```bash
sudo cp /opt/raa-server/raa-server.service /etc/systemd/system/
# ExecStart のパスを実際の nvm node バージョンに合わせて編集
sudo systemctl daemon-reload
sudo systemctl enable --now raa-server
```

```bash
# ログ確認
journalctl -u raa-server -f
journalctl -u raa-server --since "1 hour ago"
```

## Win32 クライアントのビルド

Visual Studio Build Tools 2022 以降（"C++ によるデスクトップ開発" ワークロードと Windows SDK）が必要です。

```bat
cd client
build.bat
```

`deps\libopus.lib` が存在しない場合、`build.bat` が自動的に libopus をソースからビルドします。出力は `client\raa-client.exe` です。

### 初回起動

設定ファイルがない状態で初回起動すると、設定ダイアログが自動的に表示されます。サーバーの IP アドレスを入力して OK を押すと、即座に音声キャプチャと送信が始まります。

SSRC（管理 WebUI に表示されるクライアント識別子）は初回起動時に生成され、`%APPDATA%\raa-client\raa-client.ini` に保存されます。

## 管理 WebUI

ブラウザで `http://<server>:4011/` を開きます。

- アクティブ・既知のすべてのクライアントをフレンドリーネーム・SSRC・状態付きで一覧表示
- クライアントごとの音量スライダー（0〜200%）・ミュートトグル・名前編集
- 混合ストリームの音声プレイヤー（再生ボタンをクリック）
- 右上の EN/JA ボタンで表示言語を切り替え可能（ブラウザ言語を自動検出）

## 内蔵テストストリーム

サーバーには仮想 BGM クライアント（`bgmtest0`）が組み込まれており、起動直後からパブリックドメインの楽曲をループ再生します。WebUI には **"Test BGM (Goldberg Var.1)"** として表示されます。Win32 クライアントを接続しなくても、ブラウザ → WebSocket → デコーダ → 再生までのエンドツーエンドの疎通確認がすぐに行えます。

音源: バッハ《ゴルトベルク変奏曲 BWV 988 – 第1変奏》、演奏: Shelley Katz  
出典: [musopen.org](https://musopen.org/) · ライセンス: [パブリックドメイン](https://creativecommons.org/publicdomain/mark/1.0/)

## RTP パケットフォーマット

Opus ペイロードタイプ 111（RFC 7587）を使用した標準 RTP（RFC 3550）。Wireshark・VLC・FFmpeg で診断可能。

```
バイト 0:    0x80  (V=2, P=0, X=0, CC=0)
バイト 1:    M | 111  (Marker ビット + PT=111)
バイト 2-3:  シーケンス番号（big-endian）
バイト 4-7:  タイムスタンプ（48 kHz クロック、big-endian）
バイト 8-11: SSRC（big-endian、クライアント識別子）
バイト 12+:  Opus ペイロード（20 ms、48 kHz、モノラル）
```

## ポートフォワード設定（サーバーが NAT 内の場合）

| ポート | プロトコル | 方向 | 用途 |
|---|---|---|---|
| 4010 | UDP | 外部 → サーバー | Win32 クライアントからの音声 |
| 4011 | TCP | 外部 → サーバー | WebUI + WebSocket ブラウザリスナー |

## ログレベル

| レベル | ログ内容 |
|---|---|
| `info`（デフォルト） | サーバー起動/停止、クライアント接続/切断 |
| `debug` | デコーダリセット、フレーム単位イベント |
| `warn` | ジッターバッファ枯渇、リシンクイベント |

`LOG_LEVEL=debug` 環境変数、または systemd ユニットファイルで設定。
