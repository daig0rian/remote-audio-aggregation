'use strict';
const dgram = require('dgram');
const sock = dgram.createSocket('udp4');
const counts = { 'silence(<=2)': 0, 'near(3-10)': 0, 'audio(>10)': 0 };
sock.on('message', (buf) => {
    if (buf.length < 12) return;
    if ((buf[0] >> 6) !== 2) return;
    const payloadLen = buf.length - 12;
    if      (payloadLen <= 2)  counts['silence(<=2)']++;
    else if (payloadLen <= 10) counts['near(3-10)']++;
    else                       counts['audio(>10)']++;
});
sock.bind(4098);
console.log('Sniffing on 4098 for 5 seconds...');
setTimeout(() => {
    console.log(JSON.stringify(counts, null, 2));
    process.exit();
}, 5000);
