'use strict';

// Worker Thread: 20ms ごとに全クライアントの PCM フレームをミックスして
// エンコード済みバッファを main thread に送る。
//
// Opus デコードもここで行う（main thread のイベントループをブロックしないため）。
// IPC は PCM(1920B) ではなく Opus 生データ(~150B) を受け取るので転送量も約1/10。
//
// 各クライアントの再生タイミングは送信側の RTP タイムスタンプ基準で管理する。
// jitterBuf に rtpTs → PCM を蓄積し、playoutTs が指す値を順番に取り出す。

const { parentPort } = require('worker_threads');
const { Encoder, Decoder } = require('@evan/opus');
const log            = require('./logger').child({ name: 'mixer' });

const SAMPLE_RATE       = 48000;
const CHANNELS          = 1;
const FRAME_SAMPLES     = 960;         // 20ms @ 48kHz
const RTP_FRAME_TICKS   = 960;         // RTP timestamp increment per frame
const JITTER_BUF_FRAMES = 2;           // 初回バッファリング深度: 40ms (pre-buffer)
const MAX_PLC           = 2;           // PLC: 最大 2 フレーム (40ms) 繰り返す
const JITTER_BUF_MAX    = 8;           // ジッターバッファ上限 (160ms)

const encoder     = new Encoder({ channels: CHANNELS, sample_rate: SAMPLE_RATE });
const accumulator = new Float32Array(FRAME_SAMPLES * CHANNELS);
const pcmOut      = new Int16Array(FRAME_SAMPLES * CHANNELS);  // reused each cycle, no GC

// 32ビット符号付き差分 (RTP タイムスタンプのラップアラウンド対応)
function rtpDiff(a, b) { return (a - b) | 0; }

// --- タイミング統計 (5秒毎にログ出力) -----------------------------------
let _sN = 0, _sSum = 0, _sMax = 0, _sActiveSum = 0;
const _sBuckets = new Uint32Array(40); // 0.5ms 刻みバケット [0, 19.5ms]

function _recordCycle(ms, active) {
    _sN++;
    _sSum += ms;
    _sActiveSum += active;
    if (ms > _sMax) _sMax = ms;
    _sBuckets[Math.min(39, Math.floor(ms * 2))]++;
}

setInterval(() => {
    if (_sN === 0) return;
    const mean = _sSum / _sN;
    const p99target = _sN * 0.99;
    let cum = 0, p99 = 0;
    for (let i = 0; i < 40; i++) {
        cum += _sBuckets[i];
        if (cum >= p99target) { p99 = i * 0.5; break; }
    }
    log.info({
        n:          _sN,
        mean_ms:    mean.toFixed(2),
        p99_ms:     p99.toFixed(1),
        max_ms:     _sMax.toFixed(2),
        avg_active: (_sActiveSum / _sN).toFixed(1),
    }, 'mix cycle stats');
    _sN = 0; _sSum = 0; _sMax = 0; _sActiveSum = 0; _sBuckets.fill(0);
}, 5000);
// -------------------------------------------------------------------------

// uuid -> {
//   decoder   : Decoder | null          Opus デコーダ (raw PCM クライアントは null)
//   jitterBuf : Map<rtpTs, Buffer>
//   playoutTs : number | null
//   lastFrame : Buffer | null
//   plcCount  : number
//   volume    : number
//   muted     : boolean
//   ts        : number                  最終フレーム受信時刻 (wall clock ms)
// }
let clientMap = {};

parentPort.on('message', (msg) => {
    if (msg.type === 'frame') {
        if (!clientMap[msg.uuid]) {
            clientMap[msg.uuid] = {
                decoder:   msg.opusData ? new Decoder({ channels: CHANNELS, sample_rate: SAMPLE_RATE }) : null,
                jitterBuf: new Map(),
                playoutTs: null,
                lastFrame: null,
                plcCount:  0,
                volume:    msg.volume ?? 1.0,
                muted:     msg.muted  ?? false,
                ts:        Date.now(),
            };
        }
        const c = clientMap[msg.uuid];
        c.volume = msg.volume;
        c.muted  = msg.muted;
        c.ts     = Date.now();

        if (msg.reset) {
            // ストリーム再開 (marker bit / gap) — バッファをクリアして再プライム
            log.debug({ uuid: msg.uuid, rtpTs: msg.rtpTs }, 'jitter buf reset');
            if (c.decoder) c.decoder = new Decoder({ channels: CHANNELS, sample_rate: SAMPLE_RATE });
            c.jitterBuf.clear();
            c.playoutTs = null;
            c.lastFrame = null;
            c.plcCount  = 0;
        }

        // Opus デコード or 生 PCM (チャープ診断信号) を受け入れる
        let pcmData;
        if (msg.opusData) {
            if (!c.decoder) c.decoder = new Decoder({ channels: CHANNELS, sample_rate: SAMPLE_RATE });
            try {
                const raw = c.decoder.decode(Buffer.from(msg.opusData));
                pcmData = Buffer.from(raw);
            } catch {
                return;
            }
        } else if (msg.pcmData) {
            pcmData = msg.pcmData;
        } else {
            return;
        }

        c.jitterBuf.set(msg.rtpTs >>> 0, pcmData);

        // バッファ上限超過時の eviction
        if (c.jitterBuf.size > JITTER_BUF_MAX) {
            const sorted = [...c.jitterBuf.keys()].sort((a, b) => rtpDiff(a, b));
            for (const ts of sorted) {
                if (c.jitterBuf.size <= JITTER_BUF_MAX - 2) break;
                if (c.playoutTs !== null && rtpDiff(ts, c.playoutTs) >= 0) break;
                c.jitterBuf.delete(ts);
            }
            if (c.jitterBuf.size > JITTER_BUF_MAX - 2) {
                const remaining = [...c.jitterBuf.keys()].sort((a, b) => rtpDiff(a, b));
                while (c.jitterBuf.size > JITTER_BUF_MAX - 2) c.jitterBuf.delete(remaining.shift());
                if (c.playoutTs !== null && remaining.length > 0) {
                    c.playoutTs = remaining[0] >>> 0;
                    c.lastFrame = null;
                    c.plcCount  = 0;
                }
            }
        }

    } else if (msg.type === 'update') {
        if (clientMap[msg.uuid]) {
            const c = clientMap[msg.uuid];
            const wasMuted = c.muted;
            c.volume = msg.volume;
            c.muted  = msg.muted;
            if (wasMuted && !c.muted) {
                c.jitterBuf.clear();
                c.playoutTs = null;
                c.lastFrame = null;
                c.plcCount  = 0;
            }
        }
    } else if (msg.type === 'remove') {
        delete clientMap[msg.uuid];
    }
});

const START_NS  = process.hrtime.bigint();
const FRAME_NS  = 20_000_000n;
let   frameCount = 0;

function mix() {
    const cycleStart = process.hrtime.bigint();
    const now = Date.now();
    accumulator.fill(0);
    let activeCount = 0;

    for (const [uuid, c] of Object.entries(clientMap)) {
        if (c.muted || now - c.ts > 1000) continue;

        // 再生ポインタ未設定: JITTER_BUF_FRAMES 分溜まるまで待機 (pre-buffer)
        if (c.playoutTs === null) {
            if (c.jitterBuf.size < JITTER_BUF_FRAMES) continue;
            const sorted = [...c.jitterBuf.keys()].sort((a, b) => rtpDiff(a, b));
            c.playoutTs = sorted[0] >>> 0;
        }

        let toPlay = null;
        const scheduled = c.jitterBuf.get(c.playoutTs);
        if (scheduled !== undefined) {
            c.jitterBuf.delete(c.playoutTs);
            c.lastFrame = scheduled;
            c.plcCount  = 0;
            toPlay = scheduled;

            c.playoutTs = (c.playoutTs + RTP_FRAME_TICKS) >>> 0;
            for (const ts of c.jitterBuf.keys()) {
                if (rtpDiff(ts, c.playoutTs) < 0) c.jitterBuf.delete(ts);
            }
        } else if (c.lastFrame !== null && c.plcCount < MAX_PLC) {
            c.plcCount++;
            toPlay = c.lastFrame;
        } else {
            if (now - c.ts < (MAX_PLC + 1) * 20 + 5) {
                log.warn({ uuid }, 'starvation');
            }
            c.plcCount  = 0;
            c.playoutTs = (c.playoutTs + RTP_FRAME_TICKS) >>> 0;
            if (c.jitterBuf.size > 0) {
                const oldest = [...c.jitterBuf.keys()].sort((a, b) => rtpDiff(a, b))[0];
                if (rtpDiff(oldest, c.playoutTs) > RTP_FRAME_TICKS) {
                    c.playoutTs = oldest >>> 0;
                    c.lastFrame = null;
                    log.warn({ uuid, ts: oldest }, 'resync');
                }
            }
            continue;
        }

        if (!toPlay || toPlay.byteLength < FRAME_SAMPLES * CHANNELS * 2) continue;
        activeCount++;

        const src   = new Int16Array(toPlay.buffer, toPlay.byteOffset, FRAME_SAMPLES * CHANNELS);
        const scale = c.volume / 32768;
        for (let i = 0; i < src.length; i++) {
            accumulator[i] += src[i] * scale;
        }
    }

    // Float32 → Int16 (clip): 事前確保バッファを使い回す
    for (let i = 0; i < accumulator.length; i++) {
        const v = accumulator[i];
        pcmOut[i] = v >  1 ?  32767 :
                    v < -1 ? -32767 :
                    (v * 32767) | 0;
    }

    let encoded;
    try {
        encoded = encoder.encode(Buffer.from(pcmOut.buffer));
    } catch {
        // encode error: still schedule next frame
    }

    if (encoded) {
        parentPort.postMessage({ type: 'frame', data: Buffer.from(encoded) });
    }

    // サイクル時間を計測して統計に記録
    const cycleMs = Number(process.hrtime.bigint() - cycleStart) / 1e6;
    _recordCycle(cycleMs, activeCount);

    frameCount++;
    const targetNs = START_NS + BigInt(frameCount) * FRAME_NS;
    const nowNs    = process.hrtime.bigint();
    const delayMs  = Number(targetNs - nowNs) / 1_000_000;

    if (delayMs > 0.5) {
        setTimeout(mix, delayMs);
    } else {
        setImmediate(mix);
    }
}

setTimeout(mix, 20);
