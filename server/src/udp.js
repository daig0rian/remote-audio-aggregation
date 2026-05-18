'use strict';

const dgram   = require('dgram');
const log     = require('./logger').child({ name: 'udp' });
const clients = require('./clients');

// RFC 3550: RTP minimum fixed header = 12 bytes
// RFC 7587: Opus payload type = 111 (dynamic, de-facto standard)
const RTP_HDR_MIN = 12;
const RTP_VERSION = 2;

function start(port) {
    const sock = dgram.createSocket('udp4');

    sock.on('message', (buf) => {
        if (buf.length < RTP_HDR_MIN) return;

        // Verify RTP version field (top 2 bits of byte 0)
        if ((buf[0] >> 6) !== RTP_VERSION) return;

        // Compute payload offset: skip fixed header, CSRC list, and optional extension
        const cc           = buf[0] & 0x0F;           // CSRC count
        const hasExtension = (buf[0] & 0x10) !== 0;
        let   payloadStart = RTP_HDR_MIN + cc * 4;

        if (hasExtension) {
            if (buf.length < payloadStart + 4) return;
            const extWords = buf.readUInt16BE(payloadStart + 2); // extension length in 32-bit words
            payloadStart  += 4 + extWords * 4;
        }

        if (buf.length <= payloadStart) return;

        // SSRC (bytes 8-11) is used as the persistent client identifier
        const rtpTs    = buf.readUInt32BE(4);  // RFC 3550: sender timestamp (48kHz clock for Opus)
        const ssrc     = buf.readUInt32BE(8);
        const clientId = ssrc.toString(16).padStart(8, '0'); // e.g. "a1b2c3d4"
        const marker   = (buf[1] & 0x80) !== 0;  // RFC 7587 §4.1: first packet of new talkspurt
        const opusData = buf.subarray(payloadStart);
        // Client sends DTX silence (len=1) and CN updates (len=3) as rate-limited heartbeats.
        // Both are non-audio: match client's AUDIO_LEN_MIN=8 so CN updates don't trigger
        // spurious gap resets (which would unnecessarily reset the decoder and jitter buffer).
        const isSilence = opusData.length <= 8;

        clients.touch(clientId, isSilence);
        if (!isSilence) clients.setLastFrame(clientId, opusData, marker, rtpTs);
    });

    sock.on('error', (err) => {
        log.error(err, 'socket error');
    });

    sock.bind(port, () => {
        log.info({ port }, 'listening');
    });

    return sock;
}

module.exports = { start };
