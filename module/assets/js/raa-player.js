'use strict';

/**
 * RaaPlayer — Remote Audio Aggregation WebSocket receiver for Zabbix widget.
 *
 * Connects directly to the RAA server's WebSocket endpoint (/stream) and
 * plays the mixed Opus audio stream via the Web Audio API.
 * No PHP proxy needed — the connection is a plain outbound WebSocket from the browser.
 */
class RaaPlayer {

	constructor({ serverHost, wsPort, bufferMs, onStatus, onMeter } = {}) {
		this._host     = serverHost;
		this._port     = wsPort;
		this._bufferMs = bufferMs ?? 200;
		this._onStatus = onStatus ?? (() => {});
		this._onMeter  = onMeter  ?? (() => {});

		this._ws       = null;
		this._audioCtx = null;
		this._decoder  = null;
		this._nextPlay = 0;

		this.running = false;
	}

	// ── Public ──────────────────────────────────────────────────────────────

	async start() {
		this.running   = true;
		this._nextPlay = 0;

		try {
			// 1. WASM Opus decoder (channels:1 — server mixes to mono)
			this._status('Initializing decoder…');
			const { OpusDecoder } = OpusDecoderLib;
			this._decoder = new OpusDecoder({ channels: 1 });
			await this._decoder.ready;

			// 2. AudioContext (must be inside a user gesture)
			this._audioCtx = new AudioContext({ sampleRate: 48000 });
			await this._audioCtx.resume();

			// 3. WebSocket to RAA server /stream
			this._status('Connecting…');
			const url = `ws://${this._host}:${this._port}/stream`;
			this._ws = new WebSocket(url);
			this._ws.binaryType = 'arraybuffer';

			await new Promise((resolve, reject) => {
				this._ws.onopen  = () => resolve();
				this._ws.onerror = () => reject(new Error(`Connection failed: ${url}`));
			});

			this._ws.onmessage = (e) => this._onFrame(e);
			this._ws.onclose   = () => {
				if (this.running) this._status('Disconnected');
			};

			this._status('Monitoring');

		} catch (e) {
			await this.stop();
			throw e;
		}
	}

	async stop() {
		this.running = false;

		this._ws?.close();
		this._ws = null;

		await this._audioCtx?.close();
		this._audioCtx = null;

		try { this._decoder?.reset(); } catch {}
		this._decoder = null;

		this._onMeter(0);
		this._status('Idle');
	}

	// ── Private ─────────────────────────────────────────────────────────────

	_onFrame(event) {
		if (!this.running) return;
		if (!(event.data instanceof ArrayBuffer)) return;

		const frame = new Uint8Array(event.data);

		// Meter: map typical 64kbps frame size (~160B) to ~80% width
		this._onMeter(Math.min(100, frame.byteLength / 2));

		let decoded;
		try {
			decoded = this._decoder.decodeFrame(frame);
		} catch {
			return;
		}

		const { channelData, samplesDecoded } = decoded;
		if (!samplesDecoded) return;

		// Mix down to mono (server sends mono, but handle stereo just in case)
		const nch = channelData.length;
		const pcm = channelData[0].slice();
		for (let c = 1; c < nch; c++) {
			for (let i = 0; i < pcm.length; i++) pcm[i] += channelData[c][i];
		}
		if (nch > 1) for (let i = 0; i < pcm.length; i++) pcm[i] /= nch;

		const buf = this._audioCtx.createBuffer(1, samplesDecoded, 48000);
		buf.copyToChannel(pcm, 0);

		const src = this._audioCtx.createBufferSource();
		src.buffer = buf;
		src.connect(this._audioCtx.destination);

		const now = this._audioCtx.currentTime;
		if (this._nextPlay < now) this._nextPlay = now + this._bufferMs / 1000;
		src.start(this._nextPlay);
		this._nextPlay += buf.duration;
	}

	_status(msg) {
		this._onStatus(msg);
	}
}
