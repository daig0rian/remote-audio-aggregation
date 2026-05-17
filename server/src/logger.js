'use strict';

const pino = require('pino');

// LOG_LEVEL 環境変数でレベルを制御。未設定時は 'info'。
// 例: LOG_LEVEL=debug node src/main.js
module.exports = pino({ level: process.env.LOG_LEVEL || 'info' });
