'use strict';

/**
 * Phase 0A PoC: Worker Thread 内 setInterval(20ms) のジッター計測
 *
 * 判定基準:
 *   avg    : 18〜22ms なら正常
 *   stddev : 3ms 以下なら良好
 *   outliers (±5ms超) : 全サンプルの 5% 以下なら許容範囲
 *
 * リスナー側は 100ms バッファを持つので多少のジッターは吸収される。
 */

const { Worker } = require('worker_threads');
const path = require('path');

console.log('Worker Thread タイミング計測を開始します（約 5 秒）...\n');

const worker = new Worker(path.join(__dirname, 'timing-worker.js'));

worker.on('message', ({ avg, min, max, stddev, outliers, total }) => {
    console.log('--- 計測結果 ---');
    console.log(`サンプル数   : ${total}`);
    console.log(`平均間隔     : ${avg.toFixed(3)} ms  （目標: 20ms）`);
    console.log(`最小         : ${min.toFixed(3)} ms`);
    console.log(`最大         : ${max.toFixed(3)} ms`);
    console.log(`標準偏差     : ${stddev.toFixed(3)} ms`);
    console.log(`外れ値 (±5ms超) : ${outliers} / ${total}  (${(outliers / total * 100).toFixed(1)}%)`);
    console.log();

    // 判定
    const ok_avg     = avg >= 18 && avg <= 22;
    const ok_stddev  = stddev <= 3;
    const ok_outlier = (outliers / total) <= 0.05;

    if (ok_avg && ok_stddev && ok_outlier) {
        console.log('✓ タイミング精度 良好 — 本番実装に進んで問題なし');
    } else {
        if (!ok_avg)     console.warn(`⚠ 平均間隔が目標から外れています (${avg.toFixed(1)} ms)`);
        if (!ok_stddev)  console.warn(`⚠ 標準偏差が大きい (${stddev.toFixed(1)} ms) — リスナーのバッファを増やす必要があるかもしれない`);
        if (!ok_outlier) console.warn(`⚠ 外れ値が多い (${(outliers / total * 100).toFixed(1)}%) — OS スケジューラの負荷が高い可能性あり`);
    }

    worker.terminate();
});

worker.on('error', (err) => {
    console.error('Worker エラー:', err);
    process.exit(1);
});
