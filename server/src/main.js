'use strict';

const path          = require('path');
const { Worker }    = require('worker_threads');
const { monitorEventLoopDelay } = require('perf_hooks');
const Fastify       = require('fastify');
const { WebSocketServer } = require('ws');
const log           = require('./logger');
const clients       = require('./clients');
const udp           = require('./udp');
const UDP_PORT  = parseInt(process.env.UDP_PORT  || '4010');
const HTTP_PORT = parseInt(process.env.HTTP_PORT || '4011');

// ---- 起動 ----
clients.load();

// イベントループ遅延を5秒毎に記録（メインスレッドの詰まり検知）
const eldMonitor = monitorEventLoopDelay({ resolution: 10 });
eldMonitor.enable();
setInterval(() => {
    log.info({
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

// ---- 仮想チャープクライアント（ミキサー以降の診断用 — Opusデコード段をバイパス）----
const CHIRP_UUID      = 'chirp000';
const CHIRP_FREQ_MIN  = 300;
const CHIRP_FREQ_MAX  = 3000;
const CHIRP_SWEEP_LEN = 48000 * 8;
const CHIRP_AMPLITUDE = 12000;
let chirpPhase = 0, chirpT = 0;

clients.touch(CHIRP_UUID);

const CHIRP_START_NS = process.hrtime.bigint();
const CHIRP_FRAME_NS = 20_000_000n;
let   chirpFrameCount = 0;

function sendChirpFrame() {
    const pcm = new Int16Array(960);
    for (let i = 0; i < 960; i++) {
        const tMod = chirpT % CHIRP_SWEEP_LEN;
        const freq = CHIRP_FREQ_MIN + (CHIRP_FREQ_MAX - CHIRP_FREQ_MIN) * tMod / CHIRP_SWEEP_LEN;
        pcm[i] = Math.round(CHIRP_AMPLITUDE * Math.sin(chirpPhase));
        chirpPhase += 2 * Math.PI * freq / 48000;
        chirpT++;
    }
    chirpPhase %= 2 * Math.PI;

    clients.touch(CHIRP_UUID);
    clients.postRawPCM(CHIRP_UUID, Buffer.from(pcm.buffer));

    chirpFrameCount++;
    const targetNs = CHIRP_START_NS + BigInt(chirpFrameCount) * CHIRP_FRAME_NS;
    const nowNs    = process.hrtime.bigint();
    const delayMs  = Number(targetNs - nowNs) / 1_000_000;
    setTimeout(sendChirpFrame, delayMs > 0.5 ? delayMs : 0);
}

setTimeout(sendChirpFrame, 20);
// ---- 仮想チャープクライアントここまで ----

// ---- WebSocket ----
const wss = new WebSocketServer({ noServer: true });
const wsClients = new Set();

wss.on('connection', (ws) => {
    wsClients.add(ws);
    log.info({ total: wsClients.size }, 'ws client connected');
    ws.on('close', () => {
        wsClients.delete(ws);
        log.info({ total: wsClients.size }, 'ws client disconnected');
    });
});

mixer.on('message', (msg) => {
    if (msg.type !== 'frame') return;
    const buf = msg.data;
    for (const ws of wsClients) {
        if (ws.readyState === 1) ws.send(buf, { binary: true });
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
