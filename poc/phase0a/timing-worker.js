'use strict';

/**
 * Worker Thread 側: 20ms setInterval のタイミングを計測して親に報告する。
 */

const { parentPort } = require('worker_threads');

const TARGET_MS   = 20;
const WARMUP      = 5;   // 最初の数回は無視（JIT ウォームアップ）
const MEASURE     = 250; // 計測サンプル数（5秒分）

let count = 0;
let lastTime = process.hrtime.bigint();
const intervals = [];

const timer = setInterval(() => {
    const now  = process.hrtime.bigint();
    const diff = Number(now - lastTime) / 1_000_000; // ns → ms
    lastTime   = now;
    count++;

    if (count > WARMUP) {
        intervals.push(diff);
    }

    if (intervals.length >= MEASURE) {
        clearInterval(timer);

        const avg    = intervals.reduce((a, b) => a + b, 0) / intervals.length;
        const min    = Math.min(...intervals);
        const max    = Math.max(...intervals);
        const stddev = Math.sqrt(
            intervals.reduce((a, b) => a + (b - avg) ** 2, 0) / intervals.length
        );

        // 外れ値カウント（±5ms 超）
        const outliers = intervals.filter(v => Math.abs(v - TARGET_MS) > 5).length;

        parentPort.postMessage({ avg, min, max, stddev, outliers, total: intervals.length });
    }
}, TARGET_MS);
