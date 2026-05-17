'use strict';

/**
 * Phase 0A PoC: Opus encode/decode roundtrip test
 *
 * Windows 環境では opusscript（WASM）を使用。
 * Linux サーバー VM では @discordjs/opus（N-API）で同じテストを実施すること。
 *
 * opusscript と @discordjs/opus は API が異なるが、
 * Opus アルゴリズムの動作・フレームサイズ・エンコード品質は共通。
 * このテストはアルゴリズムの正しさを検証する。
 */

const OpusScript = require('opusscript');

const SAMPLE_RATE  = 48000;
const CHANNELS     = 2;
const FRAME_SAMPLES = 960;                          // 20ms @ 48kHz per channel
const FRAME_BYTES   = FRAME_SAMPLES * CHANNELS * 2; // s16le

// ---- サイン波 PCM を生成（440Hz、s16le interleaved stereo）----
function makeSineWave(freq = 440) {
    const buf = Buffer.alloc(FRAME_BYTES);
    for (let i = 0; i < FRAME_SAMPLES; i++) {
        const t   = i / SAMPLE_RATE;
        const val = Math.round(Math.sin(2 * Math.PI * freq * t) * 16383); // 50% full scale
        buf.writeInt16LE(val, i * 4);       // L
        buf.writeInt16LE(val, i * 4 + 2);   // R
    }
    return buf;
}

// ---- RMS を計算（誤差の比較用）----
function rms(buf) {
    let sum = 0;
    const n = buf.length / 2;
    for (let i = 0; i < n; i++) {
        const s = buf.readInt16LE(i * 2) / 32768;
        sum += s * s;
    }
    return Math.sqrt(sum / n);
}

function main() {
    // opusscript: Application.AUDIO = 音楽・音声混合向け（VOIP より高品質）
    const encoder = new OpusScript(SAMPLE_RATE, CHANNELS, OpusScript.Application.AUDIO);

    const original = makeSineWave(440);
    console.log(`使用ライブラリ : opusscript (WASM)`);
    console.log(`原信号 : ${FRAME_BYTES} bytes, RMS = ${rms(original).toFixed(6)}`);

    // encode（opusscript は第2引数にサンプル数/ch を受け取る）
    const t0 = performance.now();
    const encoded = encoder.encode(original, FRAME_SAMPLES);
    const t1 = performance.now();
    console.log(`エンコード後 : ${encoded.length} bytes  (${(t1 - t0).toFixed(3)} ms)`);

    // decode
    const t2 = performance.now();
    const decoded = encoder.decode(encoded);
    const t3 = performance.now();
    console.log(`デコード後 : ${decoded.length} bytes, RMS = ${rms(decoded).toFixed(6)}  (${(t3 - t2).toFixed(3)} ms)`);

    // 誤差検証
    if (decoded.length !== FRAME_BYTES) {
        console.error(`ERROR: デコード後のサイズが不一致 (期待値 ${FRAME_BYTES})`);
        process.exit(1);
    }

    let maxDiff = 0;
    for (let i = 0; i < FRAME_BYTES; i += 2) {
        const diff = Math.abs(original.readInt16LE(i) - decoded.readInt16LE(i));
        if (diff > maxDiff) maxDiff = diff;
    }
    console.log(`最大サンプル誤差 : ${maxDiff} (16bit整数 / 最大値 32767)`);

    // Opus は非可逆圧縮なので多少の誤差は正常。ゼロ出力なら異常。
    const decodedRms = rms(decoded);
    if (decodedRms < 0.001) {
        console.error('ERROR: デコード出力がほぼ無音 → Opus 処理が機能していない可能性あり');
        process.exit(1);
    }

    console.log('\n✓ Opus エンコード / デコード ラウンドトリップ 正常');
    console.log('  ※ @discordjs/opus (N-API) の確認は Linux サーバー VM で実施すること');
}

main();
