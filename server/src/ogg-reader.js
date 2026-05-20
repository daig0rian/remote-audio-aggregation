'use strict';

const fs = require('fs');

// Minimal Ogg Opus frame extractor.
// Skips the two mandatory header pages (OpusHead + OpusTags) and returns
// an array of Buffer objects, each containing one 20ms Opus audio packet.
function readOggOpusFrames(filePath) {
    const buf = fs.readFileSync(filePath);
    const frames = [];
    let pos = 0;
    let headerPagesSeen = 0;

    while (pos + 27 <= buf.length) {
        // Ogg page capture pattern
        if (buf[pos] !== 0x4F || buf[pos+1] !== 0x67 ||
            buf[pos+2] !== 0x67 || buf[pos+3] !== 0x53) break;

        const numSeg   = buf[pos + 26];
        const tableOff = pos + 27;
        const dataOff  = tableOff + numSeg;

        let totalData = 0;
        for (let i = 0; i < numSeg; i++) totalData += buf[tableOff + i];

        if (headerPagesSeen < 2) {
            // Skip OpusHead (page 0) and OpusTags (page 1)
            headerPagesSeen++;
        } else {
            // Reassemble Opus packets from lacing segments.
            // A segment of 255 bytes means "more to follow"; < 255 ends the packet.
            let segPos = 0;
            let i = 0;
            while (i < numSeg) {
                let pktLen = 0;
                while (i < numSeg) {
                    const s = buf[tableOff + i++];
                    pktLen += s;
                    if (s < 255) break;
                }
                if (pktLen > 0) {
                    frames.push(buf.subarray(dataOff + segPos, dataOff + segPos + pktLen));
                }
                segPos += pktLen;
            }
        }

        pos = dataOff + totalData;
    }

    return frames;
}

module.exports = { readOggOpusFrames };
