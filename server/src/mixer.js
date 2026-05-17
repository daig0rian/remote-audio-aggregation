'use strict';

// Worker Thread: 20ms ごとに全クライアントの PCM フレームをミックスして
// エンコード済みバッファを main thread に送る。
//
// 各クライアントの再生タイミングは送信側の RTP タイムスタンプ基準で管理する。
// jitterBuf に rtpTs → PCM を蓄積し、playoutTs が指す値を順番に取り出す。
// これにより mixer の独立クロックと送信側クロックのずれを吸収する。

const { parentPort } = require('worker_threads');
const { Encoder }    = require('@evan/opus');
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

// 32ビット符号付き差分 (RTP タイムスタンプのラップアラウンド対応)
function rtpDiff(a, b) { return (a - b) | 0; }

// uuid -> {
//   jitterBuf : Map<rtpTs(number), Buffer>  送信側 rtpTs でインデックスされた PCM
//   playoutTs : number | null               現在の再生ポインタ（送信側 RTP クロック基準）
//   lastFrame : Buffer | null               PLC 用: 直前に再生した PCM
//   plcCount  : number                      連続 PLC フレーム数
//   volume    : number
//   muted     : boolean
//   ts        : number                      最終フレーム受信時刻 (wall clock ms)
// }
let clientMap = {};

parentPort.on('message', (msg) => {
    if (msg.type === 'frame') {
        if (!clientMap[msg.uuid]) {
            clientMap[msg.uuid] = {
                jitterBuf: new Map(), playoutTs: null,
                lastFrame: null, plcCount: 0,
                volume: msg.volume, muted: msg.muted, ts: Date.now(),
            };
        }
        const c = clientMap[msg.uuid];
        c.volume = msg.volume;
        c.muted  = msg.muted;
        c.ts     = Date.now();

        if (msg.reset) {
            // ストリーム再開 (marker bit / gap) — バッファをクリアして再プライム
            c.jitterBuf.clear();
            c.playoutTs = null;
            c.lastFrame = null;
            c.plcCount  = 0;
        }

        c.jitterBuf.set(msg.rtpTs >>> 0, msg.pcmData);

        // バッファ上限超過時の eviction:
        // まず playoutTs より古い（再生済み/通過済み）エントリを優先削除する。
        // playoutTs 以降のフレームしかない場合は最古を強制削除し、
        // playoutTs を残存最古フレームに resync してスターベーション連鎖を防ぐ。
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
            // ミュート解除時: ミュート中に溜まった古いフレームと stale な playoutTs をリセット。
            // そのまま再開すると playoutTs がバッファより大幅に遅れてスターベーション連鎖する。
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
    const now = Date.now();
    accumulator.fill(0);

    for (const [uuid, c] of Object.entries(clientMap)) {
        if (c.muted || now - c.ts > 1000) continue;

        // 再生ポインタ未設定: JITTER_BUF_FRAMES 分溜まるまで待機 (pre-buffer)
        if (c.playoutTs === null) {
            if (c.jitterBuf.size < JITTER_BUF_FRAMES) continue;
            // バッファ内で最も古い RTP タイムスタンプから再生開始
            const sorted = [...c.jitterBuf.keys()].sort((a, b) => rtpDiff(a, b));
            c.playoutTs = sorted[0] >>> 0;
        }

        // 現在の再生ポインタに対応するフレームを取得
        let toPlay = null;
        const scheduled = c.jitterBuf.get(c.playoutTs);
        if (scheduled !== undefined) {
            c.jitterBuf.delete(c.playoutTs);
            c.lastFrame = scheduled;
            c.plcCount  = 0;
            toPlay = scheduled;

            // 実フレーム再生時のみ再生ポインタを進め、古いエントリを除去する。
            // PLC 中は playoutTs を据え置くことで、遅延到着した実フレームを
            // 次サイクルで拾えるようにする (拾う前に evict しない)。
            c.playoutTs = (c.playoutTs + RTP_FRAME_TICKS) >>> 0;
            for (const ts of c.jitterBuf.keys()) {
                if (rtpDiff(ts, c.playoutTs) < 0) c.jitterBuf.delete(ts);
            }
        } else if (c.lastFrame !== null && c.plcCount < MAX_PLC) {
            // フレーム未着 — 直前フレームを PLC として使用
            // playoutTs は変えない: 次サイクルで同スロットの実フレームを再試行する
            c.plcCount++;
            toPlay = c.lastFrame;
        } else {
            // PLC 上限超過 — このスロットを諦めて次へ (starvation ログは枯渇発生瞬間のみ)
            if (now - c.ts < (MAX_PLC + 1) * 20 + 5) {
                log.warn({ uuid }, 'starvation');
            }
            c.plcCount  = 0;  // リセット: 次スロットで PLC を再試行できるようにする
            c.playoutTs = (c.playoutTs + RTP_FRAME_TICKS) >>> 0;
            // 自動 resync: バッファにフレームがあるのに playoutTs が大幅に先行している場合
            // (バッファ eviction 後のドリフト等)、最古フレームに即座にジャンプして復旧する。
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

        const src   = new Int16Array(toPlay.buffer, toPlay.byteOffset, FRAME_SAMPLES * CHANNELS);
        const scale = c.volume / 32768;
        for (let i = 0; i < src.length; i++) {
            accumulator[i] += src[i] * scale;
        }
    }

    // Float32 → Int16 (clip)
    const pcm = new Int16Array(FRAME_SAMPLES * CHANNELS);
    for (let i = 0; i < accumulator.length; i++) {
        const v = accumulator[i];
        pcm[i] = v >  1 ?  32767 :
                 v < -1 ? -32767 :
                 (v * 32767) | 0;
    }

    let encoded;
    try {
        encoded = encoder.encode(Buffer.from(pcm.buffer));
    } catch {
        // encode error: still schedule next frame
    }

    if (encoded) {
        parentPort.postMessage({ type: 'frame', data: Buffer.from(encoded) });
    }

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
