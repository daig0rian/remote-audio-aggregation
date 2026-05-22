'use strict';

class CWidgetRaaMonitor extends CWidget {

	onInitialize() {
		this._player = null;
		this._config = null;
	}

	onActivate() {
	}

	onDeactivate() {
		if (this._player?.running) {
			this._stopListening();
		}
	}

	onDestroy() {
		if (this._player) {
			this._player.stop().catch(() => {});
			this._player = null;
		}
	}

	processUpdateResponse(response) {
		if (response.fields) {
			const prev = this._config;
			this._config = response.fields;

			// If config changed while player is running, stop to force reconnect.
			if (this._player?.running && prev !== null) {
				const changed = ['server_host', 'ws_port']
					.some(k => prev[k] !== response.fields[k]);
				if (changed) {
					this._stopListening();
				}
			}
		}

		// Only re-render the body when the player is not active to avoid interrupting audio.
		if (!this._player?.running) {
			super.processUpdateResponse(response);
			this._bindUI();
		} else {
			this._setHeaderName(response.name);
		}
	}

	// ── Private ─────────────────────────────────────────────────────────────

	_bindUI() {
		const btn  = this._body.querySelector('.raa-play-btn');
		const link = this._body.querySelector('.raa-admin-link');

		if (link && this._config) {
			link.href = `http://${this._config.server_host}:${this._config.ws_port}/`;
		}

		if (!btn) return;

		btn.addEventListener('click', () => {
			if (this._player?.running) {
				this._stopListening();
			} else {
				this._startListening();
			}
		});
	}

	_startListening() {
		if (!this._config) return;

		const btn = this._body.querySelector('.raa-play-btn');
		if (btn) btn.disabled = true;

		this._player = new RaaPlayer({
			serverHost: this._config.server_host,
			wsPort:     this._config.ws_port,
			bufferMs:   this._config.buffer_ms,
			onStatus:   (msg) => this._setStatus(msg),
			onMeter:    (pct) => this._setMeter(pct),
		});

		this._player.start().then(() => {
			if (btn) {
				btn.textContent = '⏹  Stop';
				btn.classList.add('playing');
				btn.disabled = false;
			}
		}).catch((e) => {
			this._setStatus('Error: ' + e.message);
			console.error('[raa]', e);
			this._player = null;
			if (btn) {
				btn.textContent = '▶  Monitor';
				btn.classList.remove('playing');
				btn.disabled = false;
			}
		});
	}

	_stopListening() {
		const btn = this._body.querySelector('.raa-play-btn');
		if (btn) btn.disabled = true;

		const player = this._player;
		this._player = null;

		(player ? player.stop() : Promise.resolve()).finally(() => {
			if (btn) {
				btn.textContent = '▶  Monitor';
				btn.classList.remove('playing');
				btn.disabled = false;
			}
			this._setMeter(0);
		});
	}

	_setStatus(msg) {
		const el = this._body.querySelector('.raa-status');
		if (el) el.textContent = msg;
	}

	_setMeter(pct) {
		const bar = this._body.querySelector('.raa-bar');
		if (bar) bar.style.width = pct + '%';
	}
}
