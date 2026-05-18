'use strict';

// RAA サーバー負荷テスト
//
// Usage: node bench/load-test.js [NUM_CLIENTS] [NUM_ACTIVE] [DEST_IP]
//
// Example (サーバー上でループバック実行):
//   node bench/load-test.js 40 10 127.0.0.1
//
// 出力: 送信統計 + サーバーログの "mix cycle stats" / "event loop delay" を併せて確認
//
// NUM_CLIENTS: 登録クライアント総数（デフォルト: 40）
// NUM_ACTIVE:  同時発話クライアント数（デフォルト: 10）
// DEST_IP:     送信先 IP（デフォルト: 127.0.0.1）

const dgram   = require('dgram');
const { Encoder } = require('@evan/opus');

const NUM_CLIENTS = parseInt(process.argv[2] || '40');
const NUM_ACTIVE  = parseInt(process.argv[3] || '10');
const DEST_IP     = process.argv[4] || '127.0.0.1';
const DEST_PORT   = parseInt(process.env.UDP_PORT || '4010');

const SAMPLE_RATE   = 48000;
const CHANNELS      = 1;
const FRAME_SAMPLES = 960;     // 20ms

// 各クライアントの状態
const clients = Array.from({ length: NUM_CLIENTS }, (_, i) => ({
    ssrc:    (0xBEEF0000 + i) >>> 0,
    seq:     Math.floor(Math.random() * 65536),
    rtpTs:   Math.floor(Math.random() * 0xFFFFFFFF),
    encoder: i < NUM_ACTIVE ? new Encoder({ channels: CHANNELS, sample_rate: SAMPLE_RATE }) : null,
    active:  i < NUM_ACTIVE,
}));

const sock = dgram.createSocket('udp4');

// Int16 PCM から RTP+Opus パケットを生成（ホワイトノイズ）
function makePacket(c, isFirst) {
    const pcm = new Int16Array(FRAME_SAMPLES);
    for (let i = 0; i < FRAME_SAMPLES; i++) {
        pcm[i] = Math.round((Math.random() * 2 - 1) * 16000);
    }

    const opus = c.encoder.encode(Buffer.from(pcm.buffer));

    const hdr = Buffer.alloc(12);
    hdr[0] = 0x80;                                 // V=2, P=0, X=0, CC=0
    hdr[1] = (isFirst ? 0x80 : 0x00) | 111;        // marker + PT=111
    hdr.writeUInt16BE(c.seq & 0xFFFF, 2);
    hdr.writeUInt32BE(c.rtpTs >>> 0, 4);
    hdr.writeUInt32BE(c.ssrc, 8);

    c.seq   = (c.seq + 1) & 0xFFFF;
    c.rtpTs = (c.rtpTs + 960) >>> 0;

    return Buffer.concat([hdr, opus]);
}

// 非アクティブクライアントは DTX 無音パケット (len=1) を heartbeat として送る
function makeSilencePacket(c) {
    const hdr = Buffer.alloc(12);
    hdr[0] = 0x80;
    hdr[1] = 111;
    hdr.writeUInt16BE(c.seq & 0xFFFF, 2);
    hdr.writeUInt32BE(c.rtpTs >>> 0, 4);
    hdr.writeUInt32BE(c.ssrc, 8);
    c.seq   = (c.seq + 1) & 0xFFFF;
    c.rtpTs = (c.rtpTs + 960) >>> 0;
    return Buffer.concat([hdr, Buffer.from([0x78])]);  // Opus DTX silence byte
}

// 統計
let frameCount = 0, txBytes = 0;
const startMs  = Date.now();
const REPORT_INTERVAL = 500; // 10秒毎 (500 frames × 20ms)

const START_NS  = process.hrtime.bigint();
const FRAME_NS  = 20_000_000n;
let totalFrames = 0;

// 非アクティブクライアントは 5fps でハートビートを送る (50fps の 1/10)
let silenceTick = 0;

function sendFrame() {
    const isFirst = totalFrames === 0;

    // アクティブクライアント: 50fps でOpus送信
    for (const c of clients) {
        if (!c.active) continue;
        const pkt = makePacket(c, isFirst);
        sock.send(pkt, DEST_PORT, DEST_IP);
        txBytes += pkt.length;
    }

    // 非アクティブクライアント: 5fps でハートビート
    if (silenceTick % 10 === 0) {
        for (const c of clients) {
            if (c.active) continue;
            const pkt = makeSilencePacket(c);
            sock.send(pkt, DEST_PORT, DEST_IP);
        }
    }
    silenceTick++;

    frameCount++;
    totalFrames++;

    if (frameCount >= REPORT_INTERVAL) {
        const elapsedSec = (Date.now() - startMs) / 1000;
        const kbps = (txBytes * 8 / 1000 / elapsedSec).toFixed(0);
        console.log(
            `[${new Date().toISOString()}] ` +
            `total=${totalFrames} frames  active=${NUM_ACTIVE}/${NUM_CLIENTS}  ` +
            `tx=${kbps}kbps  elapsed=${elapsedSec.toFixed(0)}s`
        );
        frameCount = 0;
        txBytes    = 0;
    }

    const targetNs = START_NS + BigInt(totalFrames) * FRAME_NS;
    const nowNs    = process.hrtime.bigint();
    const delayMs  = Number(targetNs - nowNs) / 1_000_000;
    setTimeout(sendFrame, delayMs > 0.5 ? delayMs : 0);
}

console.log(`RAA load-test: ${NUM_CLIENTS} clients, ${NUM_ACTIVE} active → ${DEST_IP}:${DEST_PORT}`);
console.log('Ctrl+C to stop. Watch server log for "mix cycle stats" and "event loop delay".\n');

// 最初のフレームを発火
setTimeout(sendFrame, 20);
