'use strict';

const fs   = require('fs');
const path = require('path');
const { Decoder } = require('@evan/opus');
const log  = require('./logger').child({ name: 'clients' });

const CONFIG_PATH   = path.join(__dirname, '..', 'config.json');
const SAMPLE_RATE   = 48000;
const CHANNELS      = 1;
const FRAME_SAMPLES = 960;

let _clients = {};  // uuid -> { friendly_name, volume, muted, created_at, last_seen, _decoder }
let _mixer   = null;

function setMixer(worker) { _mixer = worker; }

function load() {
    try {
        const raw = fs.readFileSync(CONFIG_PATH, 'utf8');
        const data = JSON.parse(raw);
        for (const [uuid, c] of Object.entries(data.clients || {})) {
            _clients[uuid] = {
                friendly_name: c.friendly_name || '', volume: c.volume ?? 1.0, muted: c.muted ?? false,
                created_at: c.created_at, last_seen: c.last_seen,
                last_audio: null, _audioStreak: 0,
                _decoder: new Decoder({ channels: CHANNELS, sample_rate: SAMPLE_RATE }),
                _lastDecoded: 0,
                _nextRtpTs: 0,
            };
        }
        log.info({ count: Object.keys(_clients).length }, 'loaded from config.json');
    } catch {
        log.info('no config.json, starting fresh');
    }
}

function save() {
    const data = { clients: {} };
    for (const [uuid, c] of Object.entries(_clients)) {
        // Only persist entries the user has explicitly customized.
        // Ephemeral clients (default volume, no name, not muted) are not written.
        if (c.friendly_name || c.volume !== 1.0 || c.muted) {
            data.clients[uuid] = { friendly_name: c.friendly_name, volume: c.volume, muted: c.muted, created_at: c.created_at, last_seen: c.last_seen };
        }
    }
    fs.writeFileSync(CONFIG_PATH, JSON.stringify(data, null, 2));
}

function touch(uuid, isSilence = false) {
    const now = new Date().toISOString();
    if (!_clients[uuid]) {
        _clients[uuid] = {
            friendly_name: '', volume: 1.0, muted: false,
            created_at: now, last_seen: now,
            last_audio: null, _audioStreak: 0,
            _decoder: new Decoder({ channels: CHANNELS, sample_rate: SAMPLE_RATE }),
            _lastDecoded: 0,
            _nextRtpTs: 0,
        };
        log.info({ uuid }, 'new client');
        // Not saved here — persisted only when user customizes (name/volume/mute)
    } else {
        _clients[uuid].last_seen = now;
        if (!isSilence) {
            // Require 3 consecutive non-silence frames before marking audio active.
            // This filters sporadic noise-floor frames (which the Opus encoder produces
            // at ~2-5% rate even during subjective silence) from genuine sustained audio.
            const streak = ++_clients[uuid]._audioStreak;
            if (streak >= 3) _clients[uuid].last_audio = now;
        } else {
            _clients[uuid]._audioStreak = 0;
        }
    }
}

function setLastFrame(uuid, opusData, marker = false, rtpTs = 0) {
    const c = _clients[uuid];
    if (!c || !_mixer) return;

    const now = Date.now();
    const reset = marker || (c._lastDecoded > 0 && now - c._lastDecoded > 300);
    if (reset) {
        const reason = marker ? 'marker' : `gap ${now - c._lastDecoded}ms`;
        // audioStreak < 3 は主観的無音中の DTX 漏れフレーム — リセット自体は必要だがログは抑制
        if (c._audioStreak >= 3) {
            log.debug({ uuid, reason }, 'decoder reset');
        }
        c._decoder = new Decoder({ channels: CHANNELS, sample_rate: SAMPLE_RATE });
    }
    c._lastDecoded = now;

    try {
        const raw     = c._decoder.decode(opusData);
        const pcmData = Buffer.from(raw);
        // reset=true instructs the mixer to clear its jitter buffer and restart playout.
        // The jitter buffer's startup delay (JITTER_BUF_FRAMES) replaces the old _muteFrames logic.
        _mixer.postMessage({ type: 'frame', uuid, pcmData, rtpTs: rtpTs >>> 0, reset, volume: c.volume, muted: c.muted });
    } catch {
        // decode error: skip this frame
    }
}

function getAll() { return _clients; }

function get(uuid) { return _clients[uuid] || null; }

function update(uuid, patch) {
    if (!_clients[uuid]) return false;
    if (patch.friendly_name !== undefined) _clients[uuid].friendly_name = patch.friendly_name;
    if (patch.volume        !== undefined) _clients[uuid].volume        = Math.max(0, Math.min(2, patch.volume));
    if (patch.muted         !== undefined) _clients[uuid].muted         = !!patch.muted;
    if (_mixer) _mixer.postMessage({ type: 'update', uuid, volume: _clients[uuid].volume, muted: _clients[uuid].muted });
    save();
    return true;
}

function remove(uuid) {
    if (!_clients[uuid]) return false;
    delete _clients[uuid];
    if (_mixer) _mixer.postMessage({ type: 'remove', uuid });
    save();
    return true;
}

// Remove ephemeral (uncustomized) clients that have been offline for > 30s.
function cleanup() {
    const cutoff = new Date(Date.now() - 30000).toISOString();
    let removed = 0;
    for (const [uuid, c] of Object.entries(_clients)) {
        const customized = c.friendly_name || c.volume !== 1.0 || c.muted;
        const offline    = !c.last_seen || c.last_seen < cutoff;
        if (!customized && offline) {
            delete _clients[uuid];
            if (_mixer) _mixer.postMessage({ type: 'remove', uuid });
            removed++;
        }
    }
    if (removed > 0) log.debug({ removed }, 'removed ephemeral offline clients');
}

function postRawPCM(uuid, pcmData) {
    const c = _clients[uuid];
    if (!c || !_mixer) return;
    const rtpTs = c._nextRtpTs;
    c._nextRtpTs = (c._nextRtpTs + 960) >>> 0;  // unsigned 32-bit increment
    _mixer.postMessage({ type: 'frame', uuid, pcmData, rtpTs, reset: false, volume: c.volume, muted: c.muted });
}

module.exports = { load, setMixer, touch, setLastFrame, postRawPCM, getAll, get, update, remove, cleanup };
