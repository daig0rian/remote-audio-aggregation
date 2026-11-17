'use strict';

const path          = require('path');
const { Worker }    = require('worker_threads');
const { monitorEventLoopDelay } = require('perf_hooks');
const Fastify       = require('fastify');
const { WebSocketServer } = require('ws');
const log           = require('./logger');
const clients       = require('./clients');
const udp           = require('./udp');
const { readOggOpusFrames } = require('./ogg-reader');
const UDP_PORT  = parseInt(process.env.UDP_PORT  || '4010');
const HTTP_PORT = parseInt(process.env.HTTP_PORT || '4011');

// ---- 起動 ----
clients.load();

// イベントループ遅延を5秒毎に記録（メインスレッドの詰まり検知）
// 情報量が多いため debug レベル（LOG_LEVEL=debug で出力）
const eldMonitor = monitorEventLoopDelay({ resolution: 10 });
eldMonitor.enable();
setInterval(() => {
    log.debug({
        mean_ms: (eldMonitor.mean    / 1e6).toFixed(2),
        p99_ms:  (eldMonitor.percentile(99) / 1e6).toFixed(2),
        max_ms:  (eldMonitor.max     / 1e6).toFixed(2),
    }, 'event loop delay');
    eldMonitor.reset();
}, 5000);
udp.start(UDP_PORT);
setInterval(clients.cleanup, 30000);

// ---- Mixer Worker ----
const mixer = new Worker(path.join(__dirname, 'mixer.js'));
clients.setMixer(mixer);

// ---- 仮想BGMクライアント（再生テスト用）----
// 音源: Bach "Goldberg Variations BWV 988 – Variation 1", perf. Shelley Katz
// ライセンス: Public Domain (https://creativecommons.org/publicdomain/mark/1.0/)
// 出典: musopen.org
const BGM_UUID   = 'bgmtest0';
const bgmFrames  = readOggOpusFrames(path.join(__dirname, '..', 'assets', 'goldberg-var1.opus'));
log.info({ frames: bgmFrames.length }, 'bgm loaded');

clients.touch(BGM_UUID);
// デフォルト名が未設定の場合のみ設定（ユーザーがWebUIで変更した名前を上書きしない）
const _bgmEntry = clients.get(BGM_UUID);
if (_bgmEntry && !_bgmEntry.friendly_name) {
    clients.update(BGM_UUID, { friendly_name: 'Test BGM (Goldberg Var.1)' });
}

const BGM_START_NS = process.hrtime.bigint();
const BGM_FRAME_NS = 20_000_000n;
let   bgmFrameCount = 0;
let   bgmFrameIndex = 0;
let   bgmRtpTs      = 0;

function sendBgmFrame() {
    const marker = bgmFrameIndex === 0; // 先頭（ループ折り返し含む）でデコーダリセット
    clients.touch(BGM_UUID);
    clients.setLastFrame(BGM_UUID, bgmFrames[bgmFrameIndex], marker, bgmRtpTs);

    bgmRtpTs      = (bgmRtpTs + 960) >>> 0;
    bgmFrameIndex = (bgmFrameIndex + 1) % bgmFrames.length;

    bgmFrameCount++;
    const targetNs = BGM_START_NS + BigInt(bgmFrameCount) * BGM_FRAME_NS;
    const nowNs    = process.hrtime.bigint();
    const delayMs  = Number(targetNs - nowNs) / 1_000_000;
    setTimeout(sendBgmFrame, delayMs > 0.5 ? delayMs : 0);
}

setTimeout(sendBgmFrame, 20);
// ---- 仮想BGMクライアントここまで ----

// ---- WebSocket ----
// ws.send() は相手の受信速度を見ずに送信待ちを積むため、受信の遅いリスナーが 1 つあるだけで
// メモリと遅延が伸び続ける。送信待ちがこの量を超えている間は、そのリスナーへのフレームを捨てる。
// 64 kbps 前後のストリームで約 4 秒分。
const WS_MAX_BUFFERED_BYTES = 32 * 1024;

const wss = new WebSocketServer({ noServer: true });
const wsClients = new Map();   // ws -> { remote, dropped }

wss.on('connection', (ws, req) => {
    const state = { remote: req.socket.remoteAddress, dropped: 0 };
    wsClients.set(ws, state);
    log.info({ remote: state.remote, total: wsClients.size }, 'ws client connected');
    ws.on('close', () => {
        wsClients.delete(ws);
        log.info({ remote: state.remote, total: wsClients.size, dropped: state.dropped || undefined },
                 'ws client disconnected');
    });
});

mixer.on('message', (msg) => {
    if (msg.type !== 'frame') return;
    const buf = msg.data;
    for (const [ws, state] of wsClients) {
        if (ws.readyState !== 1) continue;
        if (ws.bufferedAmount > WS_MAX_BUFFERED_BYTES) {
            if (state.dropped++ === 0) {
                log.warn({ remote: state.remote, buffered: ws.bufferedAmount },
                         'ws client too slow, dropping frames');
            }
            continue;
        }
        if (state.dropped > 0) {
            log.info({ remote: state.remote, dropped: state.dropped }, 'ws client caught up');
            state.dropped = 0;
        }
        ws.send(buf, { binary: true });
    }
});

// ---- Fastify (HTTP + static) ----
const app = Fastify({ loggerInstance: log, disableRequestLogging: true });

app.register(require('@fastify/static'), {
    root: path.join(__dirname, '..', 'public'),
    prefix: '/'
});

app.get('/api/clients', async () => {
    const all = clients.getAll();
    return {
        server_time: new Date().toISOString(),
        clients: Object.entries(all).map(([uuid, c]) => ({
            uuid,
            friendly_name: c.friendly_name,
            volume: c.volume,
            muted: c.muted,
            last_seen: c.last_seen,
            last_audio: c.last_audio
        }))
    };
});

app.patch('/api/clients/:uuid', async (req, reply) => {
    const ok = clients.update(req.params.uuid, req.body);
    return ok ? { ok: true } : reply.code(404).send({ error: 'not found' });
});

app.delete('/api/clients/:uuid', async (req, reply) => {
    const ok = clients.remove(req.params.uuid);
    return ok ? { ok: true } : reply.code(404).send({ error: 'not found' });
});

app.listen({ port: HTTP_PORT, host: '0.0.0.0' }, (err) => {
    if (err) { log.fatal(err, 'http listen failed'); process.exit(1); }
    log.info({ port: HTTP_PORT }, 'http listening');
});

app.server.on('upgrade', (req, socket, head) => {
    if (req.url === '/stream') {
        wss.handleUpgrade(req, socket, head, (ws) => wss.emit('connection', ws, req));
    } else {
        socket.destroy();
    }
});

log.info('RAA server starting');
