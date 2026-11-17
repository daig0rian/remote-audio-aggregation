// raa-player.cpp - RAA Win32 tray player
// WebSocket /stream (Opus, 20ms frames, 48kHz mono) -> libopus -> jitter buffer -> WASAPI shared render
//
// Latency is bounded by design:
//   - The network thread always drains the socket, so nothing queues up on the server side.
//   - The jitter buffer steers itself toward buffer_ms by dropping or duplicating silent frames
//     (server and sound card clocks differ by a few ppm), and skips ahead when it overflows by
//     more than LATENCY_CAP_EXTRA_MS (e.g. a burst after a network stall during continuous audio).
//
// The WebSocket client is a minimal RFC 6455 implementation on Winsock so that every blocking
// point has a timeout: a half-open connection is detected within STALL_TIMEOUT_MS (the server
// sends 50 frames/s even during silence) and the player reconnects on its own.

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <initguid.h>
#include <ksmedia.h>
#include <bcrypt.h>
#include <stdio.h>
#include <share.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "opus.h"
#include "../resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

#define WM_TRAY          (WM_USER + 1)

#define IDM_STATUS       1001
#define IDM_TOGGLE       1002
#define IDM_WEBUI        1003
#define IDM_SETTINGS     1004
#define IDM_OPEN_LOG     1005
#define IDM_EXIT         1006

#define TIMER_STATUS     1
#define TIMER_STATS      2

static const int      SAMPLE_RATE          = 48000;
static const int      SAMPLES_PER_MS       = SAMPLE_RATE / 1000;
static const int      MAX_FRAME_SAMPLES    = 5760;              // 120 ms, largest Opus frame
static const int      RING_CAP             = SAMPLE_RATE * 3;    // > max buffer + cap + 1 frame
static const int      BUFFER_MS_MIN        = 40;
static const int      BUFFER_MS_MAX        = 2000;
static const int      STEER_BAND_MS        = 60;    // hysteresis around the target level
static const int      LATENCY_CAP_EXTRA_MS = 500;   // skip ahead beyond target + this
static const float    SILENCE_PEAK         = 1e-3f; // -60 dBFS: a frame this quiet can be dropped/duplicated
static const ULONGLONG AUDIO_HOLD_MS       = 600;   // tray stays "playing" this long after audible audio
static const DWORD    STALL_TIMEOUT_MS     = 3000;
static const DWORD    CONNECT_TIMEOUT_MS   = 5000;
static const DWORD    BACKOFF_MAX_MS       = 10000;
static const int      MAX_PAYLOAD          = 65536;
static const int      RX_CAP               = MAX_PAYLOAD + 64;
static const REFERENCE_TIME RENDER_BUFFER_HNS = 400000;     // 40 ms WASAPI buffer
static const UINT     STATS_INTERVAL_MS    = 10 * 60 * 1000;
static const LONGLONG LOG_ROTATE_BYTES     = 1024 * 1024;

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

// ============================================================
// Localization
// ============================================================

struct Strings {
    const wchar_t* st_playing;
    const wchar_t* st_idle;
    const wchar_t* st_stopped;
    const wchar_t* st_connecting;
    const wchar_t* st_retrying;
    const wchar_t* st_no_device;
    const wchar_t* tip_fmt;          // %s = state
    const wchar_t* tip_latency_fmt;  // %s = state, %d = latency (ms)
    const wchar_t* menu_status_fmt;  // %s = state, %s = server
    const wchar_t* menu_play;
    const wchar_t* menu_stop;
    const wchar_t* menu_webui;
    const wchar_t* menu_settings;
    const wchar_t* menu_open_log;
    const wchar_t* menu_exit;
    const wchar_t* dlg_caption;
    const wchar_t* lbl_host;
    const wchar_t* lbl_port;
    const wchar_t* lbl_buffer;
    const wchar_t* lbl_buffer_note;
    const wchar_t* chk_autostart;
    const wchar_t* btn_cancel;
    const wchar_t* err_host;
    const wchar_t* err_port;
    const wchar_t* err_buffer;
};

static const Strings STR_JA = {
    L"再生中",
    L"待機中（無音）",
    L"停止中",
    L"サーバーに接続しています…",
    L"サーバーに接続できません（再試行中）",
    L"音声出力デバイスを開けません",
    L"RAA Player - %s",
    L"RAA Player - %s（遅延 約 %d ms）",
    L"%s  (%s)",
    L"再生",
    L"停止",
    L"管理画面を開く",
    L"設定...",
    L"ログフォルダを開く",
    L"終了",
    L"RAA Player 設定",
    L"サーバー（IP アドレスまたはホスト名）:",
    L"ポート:",
    L"バッファ (ms):",
    L"大きくすると途切れにくく、小さくすると遅れが短くなります（40〜2000 ms）。",
    L"サインイン時に自動で起動する",
    L"キャンセル",
    L"サーバーを入力してください。",
    L"ポートは 1〜65535 の範囲で入力してください。",
    L"バッファは 40〜2000 ms の範囲で入力してください。",
};

static const Strings STR_EN = {
    L"Playing",
    L"Idle (silence)",
    L"Stopped",
    L"Connecting to server...",
    L"Cannot reach server (retrying)",
    L"Cannot open audio output device",
    L"RAA Player - %s",
    L"RAA Player - %s (latency ~%d ms)",
    L"%s  (%s)",
    L"Play",
    L"Stop",
    L"Open mixer settings",
    L"Settings...",
    L"Open log folder",
    L"Exit",
    L"RAA Player Settings",
    L"Server (IP address or host name):",
    L"Port:",
    L"Buffer (ms):",
    L"Larger values survive network hiccups; smaller values reduce delay (40-2000 ms).",
    L"Start automatically at sign-in",
    L"Cancel",
    L"Please enter the server.",
    L"Port must be between 1 and 65535.",
    L"Buffer must be between 40 and 2000 ms.",
};

static const Strings* S = &STR_JA;

// ============================================================
// Config
// ============================================================

struct Config {
    wchar_t server_host[256];
    int     server_port;
    int     buffer_ms;
    BOOL    autostart;
};

static Config g_cfg = { L"", 4011, 200, FALSE };

static wchar_t g_appdata_dir[MAX_PATH];
static wchar_t g_ini_path[MAX_PATH];
static wchar_t g_log_path[MAX_PATH];

static void save_config(void) {
    wchar_t buf[32];
    WritePrivateProfileStringW(L"Connection", L"server_host", g_cfg.server_host, g_ini_path);
    swprintf_s(buf, 32, L"%d", g_cfg.server_port);
    WritePrivateProfileStringW(L"Connection", L"server_port", buf, g_ini_path);
    swprintf_s(buf, 32, L"%d", g_cfg.buffer_ms);
    WritePrivateProfileStringW(L"Playback", L"buffer_ms", buf, g_ini_path);
    WritePrivateProfileStringW(L"General", L"autostart", g_cfg.autostart ? L"1" : L"0", g_ini_path);
}

// Returns TRUE if the INI file existed and names a server (FALSE = first run)
static BOOL load_config(void) {
    if (GetFileAttributesW(g_ini_path) == INVALID_FILE_ATTRIBUTES)
        return FALSE;
    GetPrivateProfileStringW(L"Connection", L"server_host", L"", g_cfg.server_host, 256, g_ini_path);
    g_cfg.server_port = (int)GetPrivateProfileIntW(L"Connection", L"server_port", 4011, g_ini_path);
    g_cfg.buffer_ms   = (int)GetPrivateProfileIntW(L"Playback", L"buffer_ms", 200, g_ini_path);
    g_cfg.autostart   = (GetPrivateProfileIntW(L"General", L"autostart", 0, g_ini_path) != 0);

    if (g_cfg.server_port < 1 || g_cfg.server_port > 65535) g_cfg.server_port = 4011;
    g_cfg.buffer_ms = imax(BUFFER_MS_MIN, imin(BUFFER_MS_MAX, g_cfg.buffer_ms));
    return g_cfg.server_host[0] != L'\0';
}

// "host:port", with brackets around IPv6 literals
static void format_server(wchar_t* out, size_t outsz) {
    if (wcschr(g_cfg.server_host, L':'))
        swprintf_s(out, outsz, L"[%s]:%d", g_cfg.server_host, g_cfg.server_port);
    else
        swprintf_s(out, outsz, L"%s:%d", g_cfg.server_host, g_cfg.server_port);
}

// ============================================================
// Autostart registry
// ============================================================

static void set_autostart(BOOL enable) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS) return;
    if (enable) {
        wchar_t exe[MAX_PATH], cmd[MAX_PATH + 2];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        swprintf_s(cmd, MAX_PATH + 2, L"\"%s\"", exe);
        RegSetValueExW(hk, L"raa-player", 0, REG_SZ,
                       (BYTE*)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(hk, L"raa-player");
    }
    RegCloseKey(hk);
}

// ============================================================
// Logging (UTF-8)
// ============================================================

static FILE* g_log_file = NULL;
static CRITICAL_SECTION g_log_cs;

static void log_init(void) {
    InitializeCriticalSection(&g_log_cs);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(g_log_path, GetFileExInfoStandard, &fa)) {
        LONGLONG size = ((LONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        if (size > LOG_ROTATE_BYTES) {
            wchar_t old_path[MAX_PATH];
            swprintf_s(old_path, MAX_PATH, L"%s.old", g_log_path);
            MoveFileExW(g_log_path, old_path, MOVEFILE_REPLACE_EXISTING);
        }
    }
    // Shared so the log can be opened (e.g. in Notepad) while the player is running
    g_log_file = _wfsopen(g_log_path, L"a", _SH_DENYNO);
}

static void logf(const char* fmt, ...) {
    if (!g_log_file) return;
    EnterCriticalSection(&g_log_cs);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log_file, "[%04d-%02d-%02d %02d:%02d:%02d] ",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log_file, fmt, ap);
    va_end(ap);
    fprintf(g_log_file, "\n");
    fflush(g_log_file);
    LeaveCriticalSection(&g_log_cs);
}

static DWORD get_os_build(void) {
    OSVERSIONINFOW ovi = { sizeof(ovi) };
    typedef LONG (WINAPI *Fn)(OSVERSIONINFOW*);
    HMODULE h = GetModuleHandleW(L"ntdll.dll");
    if (h) {
        auto fn = (Fn)GetProcAddress(h, "RtlGetVersion");
        if (fn) { fn(&ovi); return ovi.dwBuildNumber; }
    }
    return 0;
}

// ============================================================
// Shared state between the UI, network and audio threads
// ============================================================

static HANDLE g_stop_evt      = NULL;  // manual reset: set = threads must exit
static HANDLE g_devchange_evt = NULL;  // auto reset: default render device changed
static HANDLE g_net_thread    = NULL;
static HANDLE g_audio_thread  = NULL;

static volatile LONG     g_net_connected    = 0;
static volatile LONG     g_connect_failures = 0;  // consecutive failed attempts
static volatile LONG     g_audio_running    = 0;  // render stream started
static volatile LONG     g_audio_failed     = 0;  // render device could not be opened
static volatile LONG     g_padding_frames   = 0;  // queued in WASAPI (48 kHz frames)
static volatile LONGLONG g_last_audio_tick  = 0;  // GetTickCount64() of the last audible frame
static volatile LONG     g_decode_errors    = 0;

static bool stop_requested(void) {
    return WaitForSingleObject(g_stop_evt, 0) == WAIT_OBJECT_0;
}

// ============================================================
// Jitter buffer (PCM ring, 48 kHz mono float)
// ============================================================

static CRITICAL_SECTION g_ring_cs;
static float g_ring[RING_CAP];
static int   g_ring_r   = 0;
static int   g_ring_n   = 0;
static bool  g_priming  = true;   // output silence until the buffer reaches the target
static int   g_target   = 200 * SAMPLES_PER_MS;

struct JitterStats {
    unsigned underruns;   // buffer ran dry while playing
    unsigned dropped;     // silent frames dropped (buffer above target)
    unsigned inserted;    // silent frames duplicated (buffer below target)
    unsigned skips;       // skip-aheads (buffer above target + cap)
    unsigned skipped_ms;
};
static JitterStats g_stats = {};

static void ring_discard(int n) {
    g_ring_r = (g_ring_r + n) % RING_CAP;
    g_ring_n -= n;
}

static void ring_write(const float* src, int n) {
    int over = g_ring_n + n - RING_CAP;
    if (over > 0) ring_discard(over);   // guard only; the latency cap keeps us far below RING_CAP
    int w = (g_ring_r + g_ring_n) % RING_CAP;
    int first = imin(n, RING_CAP - w);
    memcpy(g_ring + w, src, first * sizeof(float));
    memcpy(g_ring, src + first, (n - first) * sizeof(float));
    g_ring_n += n;
}

static void jitter_reset(void) {
    EnterCriticalSection(&g_ring_cs);
    g_ring_r = 0; g_ring_n = 0; g_priming = true;
    LeaveCriticalSection(&g_ring_cs);
}

// Network thread: one decoded frame
static void jitter_push(const float* pcm, int n, bool silent) {
    // Nothing is consuming: do not let audio pile up while the output device is unavailable.
    if (!InterlockedCompareExchange(&g_audio_running, 0, 0)) return;

    EnterCriticalSection(&g_ring_cs);
    const int band = imin(STEER_BAND_MS * SAMPLES_PER_MS, g_target / 2);
    int copies = 1;
    if (!g_priming) {
        if (g_ring_n > g_target + LATENCY_CAP_EXTRA_MS * SAMPLES_PER_MS) {
            int skip = g_ring_n - g_target;
            ring_discard(skip);
            g_stats.skips++;
            g_stats.skipped_ms += skip / SAMPLES_PER_MS;
        } else if (silent && g_ring_n > g_target + band) {
            copies = 0; g_stats.dropped++;
        } else if (silent && g_ring_n < g_target - band) {
            copies = 2; g_stats.inserted++;
        }
    }
    for (int c = 0; c < copies; c++) ring_write(pcm, n);
    if (g_priming && g_ring_n >= g_target) g_priming = false;
    LeaveCriticalSection(&g_ring_cs);
}

// Audio thread: fill n samples (silence while priming or on underrun)
static void jitter_pull(float* out, int n) {
    int got = 0;
    EnterCriticalSection(&g_ring_cs);
    if (!g_priming) {
        got = imin(n, g_ring_n);
        int first = imin(got, RING_CAP - g_ring_r);
        memcpy(out, g_ring + g_ring_r, first * sizeof(float));
        memcpy(out + first, g_ring, (got - first) * sizeof(float));
        ring_discard(got);
        if (got < n) { g_priming = true; g_stats.underruns++; }
    }
    LeaveCriticalSection(&g_ring_cs);
    if (got < n) memset(out + got, 0, (n - got) * sizeof(float));
}

static int jitter_level_ms(void) {
    EnterCriticalSection(&g_ring_cs);
    int n = g_ring_n;
    LeaveCriticalSection(&g_ring_cs);
    return n / SAMPLES_PER_MS;
}

// ============================================================
// Minimal WebSocket client (RFC 6455, receive side)
// ============================================================

static void base64_encode(const BYTE* in, int n, char* out) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? T[v & 63] : '=';
    }
    out[o] = 0;
}

static bool sha1(const BYTE* data, ULONG len, BYTE out[20]) {
    BCRYPT_ALG_HANDLE  alg = NULL;
    BCRYPT_HASH_HANDLE h   = NULL;
    bool ok = false;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0))) {
        if (BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0))) {
            ok = BCRYPT_SUCCESS(BCryptHashData(h, (PUCHAR)data, len, 0)) &&
                 BCRYPT_SUCCESS(BCryptFinishHash(h, out, 20, 0));
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return ok;
}

static void random_bytes(BYTE* out, ULONG n) {
    BCryptGenRandom(NULL, out, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

// Waits until the socket is readable (or, for a pending connect, writable/failed).
// Polls in 200 ms slices so a stop request is noticed promptly.
// Returns 1 = ready, 0 = timeout, -1 = stop requested or select() error.
static int wait_socket(SOCKET s, bool connecting, DWORD timeout_ms) {
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        if (stop_requested()) return -1;
        fd_set rf, wf, ef;
        FD_ZERO(&rf); FD_ZERO(&wf); FD_ZERO(&ef);
        if (connecting) { FD_SET(s, &wf); FD_SET(s, &ef); }
        else            { FD_SET(s, &rf); }
        ULONGLONG now = GetTickCount64();
        DWORD slice = (DWORD)((deadline > now) ? imin((int)(deadline - now), 200) : 0);
        timeval tv = { 0, (long)slice * 1000 };
        int r = select(0, &rf, &wf, &ef, &tv);
        if (r < 0)  return -1;
        if (r > 0)  return 1;
        if (GetTickCount64() >= deadline) return 0;
    }
}

static bool send_all(SOCKET s, const char* p, int len) {
    while (len > 0) {
        int n = send(s, p, len, 0);
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}

static SOCKET tcp_connect(const char* host, int port, char* err, size_t errsz) {
    char port_s[16];
    sprintf_s(port_s, sizeof(port_s), "%d", port);
    addrinfo hints = {};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = NULL;
    int rc = getaddrinfo(host, port_s, &hints, &res);
    if (rc != 0) { sprintf_s(err, errsz, "name resolution failed (%d)", rc); return INVALID_SOCKET; }

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* ai = res; ai && s == INVALID_SOCKET && !stop_requested(); ai = ai->ai_next) {
        SOCKET t = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (t == INVALID_SOCKET) { sprintf_s(err, errsz, "socket() failed (%d)", WSAGetLastError()); continue; }
        u_long nb = 1; ioctlsocket(t, FIONBIO, &nb);
        if (connect(t, ai->ai_addr, (int)ai->ai_addrlen) == 0 || WSAGetLastError() == WSAEWOULDBLOCK) {
            int w = wait_socket(t, true, CONNECT_TIMEOUT_MS);
            if (w > 0) {
                int soerr = 0; int len = sizeof(soerr);
                getsockopt(t, SOL_SOCKET, SO_ERROR, (char*)&soerr, &len);
                if (soerr == 0) s = t;
                else sprintf_s(err, errsz, "connect failed (%d)", soerr);
            } else if (w == 0) {
                sprintf_s(err, errsz, "connect timed out");
            } else {
                sprintf_s(err, errsz, "connect cancelled");
            }
        } else {
            sprintf_s(err, errsz, "connect failed (%d)", WSAGetLastError());
        }
        if (s != t) closesocket(t);
    }
    freeaddrinfo(res);

    if (s != INVALID_SOCKET) {
        u_long nb = 0; ioctlsocket(s, FIONBIO, &nb);
        BOOL nodelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));
    }
    return s;
}

// Case-insensitive header lookup in a NUL-terminated response header block.
static bool header_value(const char* hdr, const char* name, char* out, size_t outsz) {
    size_t nlen = strlen(name);
    for (const char* line = strstr(hdr, "\r\n"); line; line = strstr(line, "\r\n")) {
        line += 2;
        if (_strnicmp(line, name, nlen) == 0 && line[nlen] == ':') {
            const char* v = line + nlen + 1;
            while (*v == ' ' || *v == '\t') v++;
            const char* e = strstr(v, "\r\n");
            if (!e) e = v + strlen(v);
            while (e > v && (e[-1] == ' ' || e[-1] == '\t')) e--;
            size_t len = (size_t)(e - v);
            if (len >= outsz) return false;
            memcpy(out, v, len); out[len] = 0;
            return true;
        }
    }
    return false;
}

// Sends the upgrade request and validates the 101 response.
// Bytes received after the response header (the first frames) are left in rx[0..*rx_len).
static bool ws_handshake(SOCKET s, const char* host, int port,
                         BYTE* rx, int* rx_len, char* err, size_t errsz) {
    BYTE nonce[16];
    random_bytes(nonce, (ULONG)sizeof(nonce));
    char key[32];
    base64_encode(nonce, (int)sizeof(nonce), key);

    bool v6 = strchr(host, ':') != NULL;
    char req[768];
    int len = sprintf_s(req, sizeof(req),
        "GET /stream HTTP/1.1\r\n"
        "Host: %s%s%s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "User-Agent: raa-player\r\n"
        "\r\n",
        v6 ? "[" : "", host, v6 ? "]" : "", port, key);
    if (len <= 0 || !send_all(s, req, len)) { sprintf_s(err, errsz, "send failed"); return false; }

    *rx_len = 0;
    ULONGLONG deadline = GetTickCount64() + CONNECT_TIMEOUT_MS;
    char* end = NULL;
    while (!end) {
        if (*rx_len >= 8192) { sprintf_s(err, errsz, "response header too large"); return false; }
        ULONGLONG now = GetTickCount64();
        int w = (now < deadline) ? wait_socket(s, false, (DWORD)(deadline - now)) : 0;
        if (w == 0) { sprintf_s(err, errsz, "handshake timed out"); return false; }
        if (w < 0)  { sprintf_s(err, errsz, "handshake cancelled"); return false; }
        int n = recv(s, (char*)rx + *rx_len, RX_CAP - *rx_len - 1, 0);
        if (n <= 0) { sprintf_s(err, errsz, "connection closed during handshake"); return false; }
        *rx_len += n;
        rx[*rx_len] = 0;
        end = strstr((char*)rx, "\r\n\r\n");   // header is text; binary frames only follow it
    }
    int hdr_len = (int)(end - (char*)rx) + 4;

    char hdr[8200];
    if (hdr_len >= (int)sizeof(hdr)) { sprintf_s(err, errsz, "response header too large"); return false; }
    memcpy(hdr, rx, hdr_len);
    hdr[hdr_len] = 0;

    if (strncmp(hdr, "HTTP/1.1 101", 12) != 0) {
        const char* eol = strstr(hdr, "\r\n");
        int l = imin((int)(eol - hdr), 80);
        sprintf_s(err, errsz, "unexpected response: %.*s", l, hdr);
        return false;
    }

    char accept_src[96];
    sprintf_s(accept_src, sizeof(accept_src), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    BYTE digest[20];
    char expect[32], got[64];
    if (!sha1((const BYTE*)accept_src, (ULONG)strlen(accept_src), digest)) {
        sprintf_s(err, errsz, "SHA-1 unavailable"); return false;
    }
    base64_encode(digest, 20, expect);
    if (!header_value(hdr, "sec-websocket-accept", got, sizeof(got)) || strcmp(got, expect) != 0) {
        sprintf_s(err, errsz, "invalid Sec-WebSocket-Accept");
        return false;
    }

    memmove(rx, rx + hdr_len, *rx_len - hdr_len);
    *rx_len -= hdr_len;
    return true;
}

// Parses one server frame. Returns bytes consumed, 0 if incomplete, -1 on protocol error.
static int ws_parse(const BYTE* p, int len, int* op, bool* fin, const BYTE** payload, int* plen) {
    if (len < 2) return 0;
    if (p[0] & 0x70) return -1;              // RSV bits: no extensions were negotiated
    if (p[1] & 0x80) return -1;              // server frames must not be masked
    *fin = (p[0] & 0x80) != 0;
    *op  = p[0] & 0x0F;
    uint64_t l = p[1] & 0x7F;
    int h = 2;
    if (l == 126) {
        if (len < 4) return 0;
        l = ((uint64_t)p[2] << 8) | p[3];
        h = 4;
    } else if (l == 127) {
        if (len < 10) return 0;
        l = 0;
        for (int i = 0; i < 8; i++) l = (l << 8) | p[2 + i];
        h = 10;
    }
    if (l > (uint64_t)MAX_PAYLOAD) return -1;
    if (*op >= 0x8 && (l > 125 || !*fin)) return -1;   // control frames: short and unfragmented
    if (len < h + (int)l) return 0;
    *payload = p + h;
    *plen    = (int)l;
    return h + (int)l;
}

// Client-to-server frames must be masked (RFC 6455 5.3). Only control frames are sent.
static bool ws_send_control(SOCKET s, int op, const BYTE* payload, int len) {
    BYTE f[2 + 4 + 125];
    len = imin(len, 125);
    f[0] = (BYTE)(0x80 | op);
    f[1] = (BYTE)(0x80 | len);
    random_bytes(f + 2, 4);
    for (int i = 0; i < len; i++) f[6 + i] = payload[i] ^ f[2 + (i & 3)];
    return send_all(s, (const char*)f, 6 + len);
}

// ============================================================
// Network thread
// ============================================================

static void on_packet(OpusDecoder* dec, const BYTE* data, int len, float* pcm) {
    int n = opus_decode_float(dec, data, len, pcm, MAX_FRAME_SAMPLES, 0);
    if (n <= 0) { InterlockedIncrement(&g_decode_errors); return; }
    float peak = 0.0f;
    for (int i = 0; i < n; i++) {
        float a = fabsf(pcm[i]);
        if (a > peak) peak = a;
    }
    if (peak >= SILENCE_PEAK)
        InterlockedExchange64(&g_last_audio_tick, (LONGLONG)GetTickCount64());
    jitter_push(pcm, n, peak < SILENCE_PEAK);
}

// Receives frames until the connection ends or a stop is requested.
static void ws_session(SOCKET s, BYTE* rx, int rx_len, char* reason, size_t reasonsz) {
    int err = 0;
    OpusDecoder* dec = opus_decoder_create(SAMPLE_RATE, 1, &err);
    BYTE* msg = (BYTE*)malloc(MAX_PAYLOAD);
    float* pcm = (float*)malloc(MAX_FRAME_SAMPLES * sizeof(float));
    if (!dec || !msg || !pcm) {
        sprintf_s(reason, reasonsz, "initialization failed (opus %d)", err);
        if (dec) opus_decoder_destroy(dec);
        free(msg); free(pcm);
        return;
    }

    int  msg_len = 0;
    bool in_msg = false, msg_binary = false;
    ULONGLONG last_rx = GetTickCount64();
    reason[0] = 0;

    while (!reason[0]) {
        // 1) Handle every complete frame in the buffer
        int off = 0;
        while (!reason[0]) {
            int op = 0, plen = 0; bool fin = false; const BYTE* pl = NULL;
            int used = ws_parse(rx + off, rx_len - off, &op, &fin, &pl, &plen);
            if (used == 0) break;
            if (used < 0) { sprintf_s(reason, reasonsz, "protocol error"); break; }
            off += used;
            switch (op) {
            case 0x0:  // continuation
                if (!in_msg || msg_len + plen > MAX_PAYLOAD) { sprintf_s(reason, reasonsz, "protocol error"); break; }
                memcpy(msg + msg_len, pl, plen); msg_len += plen;
                if (fin) { in_msg = false; if (msg_binary) on_packet(dec, msg, msg_len, pcm); }
                break;
            case 0x1:  // text (not used by the server)
            case 0x2:  // binary: one Opus packet per message
                if (in_msg) { sprintf_s(reason, reasonsz, "protocol error"); break; }
                if (fin) {
                    if (op == 0x2) on_packet(dec, pl, plen, pcm);
                } else {
                    in_msg = true; msg_binary = (op == 0x2);
                    memcpy(msg, pl, plen); msg_len = plen;
                }
                break;
            case 0x8:  // close: echo the status code, then reconnect
                ws_send_control(s, 0x8, pl, plen >= 2 ? 2 : 0);
                sprintf_s(reason, reasonsz, "closed by server");
                break;
            case 0x9:  // ping
                ws_send_control(s, 0xA, pl, plen);
                break;
            case 0xA:  // pong
                break;
            default:
                sprintf_s(reason, reasonsz, "protocol error (opcode %d)", op);
                break;
            }
        }
        if (off > 0) { memmove(rx, rx + off, rx_len - off); rx_len -= off; }
        if (reason[0]) break;

        // 2) Read more
        int w = wait_socket(s, false, 200);
        if (w < 0) { sprintf_s(reason, reasonsz, stop_requested() ? "stopped" : "select failed"); break; }
        if (w == 0) {
            if (GetTickCount64() - last_rx > STALL_TIMEOUT_MS)
                sprintf_s(reason, reasonsz, "no data for %lu ms", STALL_TIMEOUT_MS);
            continue;
        }
        int n = recv(s, (char*)rx + rx_len, RX_CAP - rx_len - 1, 0);
        if (n == 0) { sprintf_s(reason, reasonsz, "connection closed"); break; }
        if (n < 0)  { sprintf_s(reason, reasonsz, "recv failed (%d)", WSAGetLastError()); break; }
        rx_len += n;
        last_rx = GetTickCount64();
    }

    opus_decoder_destroy(dec);
    free(msg); free(pcm);
}

struct NetConfig {
    char host[256];
    int  port;
};

static DWORD WINAPI net_thread_proc(LPVOID param) {
    NetConfig* nc = (NetConfig*)param;
    BYTE* rx = (BYTE*)malloc(RX_CAP);
    DWORD backoff = 1000;
    char last_err[128] = "";

    while (rx && !stop_requested()) {
        char err[128] = "";
        int rx_len = 0;
        SOCKET s = tcp_connect(nc->host, nc->port, err, sizeof(err));
        if (s != INVALID_SOCKET && !ws_handshake(s, nc->host, nc->port, rx, &rx_len, err, sizeof(err))) {
            closesocket(s);
            s = INVALID_SOCKET;
        }
        if (s == INVALID_SOCKET) {
            if (stop_requested()) break;
            LONG fails = InterlockedIncrement(&g_connect_failures);
            // Log when the cause changes, and then periodically while it persists
            if (strcmp(err, last_err) != 0 || fails % 100 == 0) {
                logf("Connect to %s:%d failed: %s (attempt %ld)", nc->host, nc->port, err, fails);
                strcpy_s(last_err, sizeof(last_err), err);
            }
            WaitForSingleObject(g_stop_evt, backoff);
            backoff = (DWORD)imin((int)backoff * 2, (int)BACKOFF_MAX_MS);
            continue;
        }

        logf("Connected to %s:%d", nc->host, nc->port);
        InterlockedExchange(&g_connect_failures, 0);
        last_err[0] = 0;
        jitter_reset();
        InterlockedExchange(&g_net_connected, 1);

        ULONGLONG t0 = GetTickCount64();
        char reason[128];
        ws_session(s, rx, rx_len, reason, sizeof(reason));

        InterlockedExchange(&g_net_connected, 0);
        closesocket(s);
        if (stop_requested()) break;

        ULONGLONG lasted = GetTickCount64() - t0;
        logf("Disconnected: %s (connected for %llu s)", reason, lasted / 1000);
        // A session that held for a while gets a quick retry; flapping ones back off.
        backoff = (lasted > 10000) ? 1000 : (DWORD)imin((int)backoff * 2, (int)BACKOFF_MAX_MS);
        WaitForSingleObject(g_stop_evt, backoff);
    }

    free(rx);
    delete nc;
    return 0;
}

// ============================================================
// Audio thread (WASAPI shared-mode render, event driven)
// ============================================================

// Follows changes of the default output device (headset plugged in, HDMI switched, ...)
class DeviceNotifier : public IMMNotificationClient {
    LONG   m_ref;
    HANDLE m_evt;
public:
    explicit DeviceNotifier(HANDLE evt) : m_ref(1), m_evt(evt) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK;
        }
        *ppv = NULL; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) SetEvent(m_evt);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

// PKEY_Device_FriendlyName
static const PROPERTYKEY kDeviceFriendlyName =
    { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

static void device_name(IMMDevice* dev, char* out, int outsz) {
    strcpy_s(out, outsz, "(unknown)");
    IPropertyStore* ps = NULL;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(kDeviceFriendlyName, &pv)) && pv.vt == VT_LPWSTR)
            WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, out, outsz, NULL, NULL);
        PropVariantClear(&pv);
        ps->Release();
    }
}

// Opens the default output device and renders until stop, device change or device error.
// Returns S_OK for stop / device change, a failure code if the device could not be used.
static HRESULT render_session(IMMDeviceEnumerator* en) {
    IMMDevice*          dev = NULL;
    IAudioClient*       ac  = NULL;
    IAudioRenderClient* rc  = NULL;
    HANDLE evt = CreateEventW(NULL, FALSE, FALSE, NULL);
    UINT32 buf_frames = 0;

    // 48 kHz mono float; AUTOCONVERTPCM lets WASAPI convert to the device's mix format
    WAVEFORMATEXTENSIBLE wfex = {};
    wfex.Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
    wfex.Format.nChannels            = 1;
    wfex.Format.nSamplesPerSec       = SAMPLE_RATE;
    wfex.Format.wBitsPerSample       = 32;
    wfex.Format.nBlockAlign          = 4;
    wfex.Format.nAvgBytesPerSec      = SAMPLE_RATE * 4;
    wfex.Format.cbSize               = (WORD)(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    wfex.Samples.wValidBitsPerSample = 32;
    wfex.dwChannelMask               = SPEAKER_FRONT_CENTER;
    wfex.SubFormat                   = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    HRESULT hr = evt ? en->GetDefaultAudioEndpoint(eRender, eConsole, &dev) : E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&ac);
    if (SUCCEEDED(hr)) hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                           AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                           AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                           AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                           RENDER_BUFFER_HNS, 0, (WAVEFORMATEX*)&wfex, NULL);
    if (SUCCEEDED(hr)) hr = ac->SetEventHandle(evt);
    if (SUCCEEDED(hr)) hr = ac->GetBufferSize(&buf_frames);
    if (SUCCEEDED(hr)) hr = ac->GetService(__uuidof(IAudioRenderClient), (void**)&rc);
    if (SUCCEEDED(hr)) {
        BYTE* p = NULL;
        hr = rc->GetBuffer(buf_frames, &p);
        if (SUCCEEDED(hr)) hr = rc->ReleaseBuffer(buf_frames, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    if (SUCCEEDED(hr)) hr = ac->Start();

    if (SUCCEEDED(hr)) {
        char name[256];
        device_name(dev, name, sizeof(name));
        logf("Audio output opened: %s (WASAPI buffer %u ms)", name, buf_frames / SAMPLES_PER_MS);
        InterlockedExchange(&g_audio_failed, 0);
        jitter_reset();
        InterlockedExchange(&g_audio_running, 1);

        HANDLE hs[3] = { evt, g_stop_evt, g_devchange_evt };
        for (;;) {
            DWORD w = WaitForMultipleObjects(3, hs, FALSE, 1000);
            if (w == WAIT_OBJECT_0 + 1) { hr = S_OK; break; }
            if (w == WAIT_OBJECT_0 + 2) { logf("Default audio output changed"); hr = S_OK; break; }
            // Buffer event, or a timeout (checks whether the device is still there)
            UINT32 padding = 0;
            hr = ac->GetCurrentPadding(&padding);
            if (FAILED(hr)) break;
            InterlockedExchange(&g_padding_frames, (LONG)padding);
            UINT32 avail = buf_frames - padding;
            if (avail == 0) continue;
            BYTE* p = NULL;
            hr = rc->GetBuffer(avail, &p);
            if (FAILED(hr)) break;
            jitter_pull((float*)p, (int)avail);
            hr = rc->ReleaseBuffer(avail, 0);
            if (FAILED(hr)) break;
        }

        InterlockedExchange(&g_audio_running, 0);
        InterlockedExchange(&g_padding_frames, 0);
        ac->Stop();
        if (FAILED(hr)) logf("Audio output lost: 0x%08lX", hr);
    }

    if (rc)  rc->Release();
    if (ac)  ac->Release();
    if (dev) dev->Release();
    if (evt) CloseHandle(evt);
    return hr;
}

static DWORD WINAPI audio_thread_proc(LPVOID) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool com = SUCCEEDED(hr);

    IMMDeviceEnumerator* en = NULL;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr)) {
        logf("CoCreateInstance(MMDeviceEnumerator) failed: 0x%08lX", hr);
        InterlockedExchange(&g_audio_failed, 1);
    } else {
        DeviceNotifier* notifier = new DeviceNotifier(g_devchange_evt);
        en->RegisterEndpointNotificationCallback(notifier);

        HRESULT last_fail = S_OK;
        while (!stop_requested()) {
            hr = render_session(en);
            if (stop_requested()) break;
            if (FAILED(hr)) {
                InterlockedExchange(&g_audio_failed, 1);
                if (hr != last_fail) {
                    logf("Audio output unavailable: 0x%08lX (retrying every 2 s)", hr);
                    last_fail = hr;
                }
                WaitForSingleObject(g_stop_evt, 2000);
            } else {
                last_fail = S_OK;
                WaitForSingleObject(g_stop_evt, 200);   // let a device switch settle
            }
        }

        en->UnregisterEndpointNotificationCallback(notifier);
        notifier->Release();
        en->Release();
    }

    if (com) CoUninitialize();
    return 0;
}

// ============================================================
// Player start / stop
// ============================================================

static void start_player(void) {
    if (g_net_thread || g_audio_thread) return;
    ResetEvent(g_stop_evt);

    EnterCriticalSection(&g_ring_cs);
    g_target = g_cfg.buffer_ms * SAMPLES_PER_MS;
    LeaveCriticalSection(&g_ring_cs);
    jitter_reset();

    InterlockedExchange(&g_audio_failed, 0);
    InterlockedExchange(&g_connect_failures, 0);
    InterlockedExchange64(&g_last_audio_tick, 0);

    NetConfig* nc = new NetConfig();
    WideCharToMultiByte(CP_UTF8, 0, g_cfg.server_host, -1, nc->host, sizeof(nc->host), NULL, NULL);
    nc->port = g_cfg.server_port;

    logf("Playback starting: %s:%d, buffer %d ms", nc->host, nc->port, g_cfg.buffer_ms);
    g_audio_thread = CreateThread(NULL, 0, audio_thread_proc, NULL, 0, NULL);
    g_net_thread   = CreateThread(NULL, 0, net_thread_proc, nc, 0, NULL);
}

static void stop_player(void) {
    SetEvent(g_stop_evt);
    HANDLE hs[2]; DWORD n = 0;
    if (g_net_thread)   hs[n++] = g_net_thread;
    if (g_audio_thread) hs[n++] = g_audio_thread;
    if (n > 0 && WaitForMultipleObjects(n, hs, TRUE, 8000) == WAIT_TIMEOUT)
        logf("Worker threads did not stop within 8 s");
    if (g_net_thread)   { CloseHandle(g_net_thread);   g_net_thread = NULL; }
    if (g_audio_thread) { CloseHandle(g_audio_thread); g_audio_thread = NULL; }
    InterlockedExchange(&g_net_connected, 0);
    InterlockedExchange(&g_audio_running, 0);
}

// ============================================================
// Tray
// ============================================================

enum PlayerState { PS_PLAYING, PS_IDLE, PS_STOPPED, PS_OFFLINE, PS_COUNT };

static HWND            g_hwnd = NULL;
static NOTIFYICONDATAW g_nid = {};
static HICON           g_icons[PS_COUNT] = {};
static PlayerState     g_state = PS_OFFLINE;
static const wchar_t*  g_status_text = L"";
static bool            g_user_stopped = false;
static UINT            g_wm_taskbar_created = 0;

static void tray_add(void) {
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void tray_init(HWND hwnd) {
    HINSTANCE hi = GetModuleHandleW(NULL);
    const int ids[PS_COUNT] = { IDI_TRAY_PLAYING, IDI_TRAY_IDLE, IDI_TRAY_STOPPED, IDI_TRAY_OFFLINE };
    for (int i = 0; i < PS_COUNT; i++)
        LoadIconMetric(hi, MAKEINTRESOURCEW(ids[i]), LIM_SMALL, &g_icons[i]);

    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon            = g_icons[PS_OFFLINE];
    wcscpy_s(g_nid.szTip, L"RAA Player");
    tray_add();
}

static void tray_remove(void) {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    for (int i = 0; i < PS_COUNT; i++) if (g_icons[i]) DestroyIcon(g_icons[i]);
}

static void update_status(void) {
    PlayerState st;
    const wchar_t* text;
    bool connected = InterlockedCompareExchange(&g_net_connected, 0, 0) != 0;
    bool running   = InterlockedCompareExchange(&g_audio_running, 0, 0) != 0;
    ULONGLONG last_audio = (ULONGLONG)InterlockedCompareExchange64(&g_last_audio_tick, 0, 0);

    if (g_user_stopped)                                     { st = PS_STOPPED; text = S->st_stopped; }
    else if (InterlockedCompareExchange(&g_audio_failed, 0, 0)) { st = PS_OFFLINE; text = S->st_no_device; }
    else if (!connected) {
        st = PS_OFFLINE;
        text = InterlockedCompareExchange(&g_connect_failures, 0, 0) ? S->st_retrying : S->st_connecting;
    }
    else if (last_audio && GetTickCount64() - last_audio < AUDIO_HOLD_MS) { st = PS_PLAYING; text = S->st_playing; }
    else                                                    { st = PS_IDLE; text = S->st_idle; }

    wchar_t tip[128];
    if (connected && running) {
        int latency = jitter_level_ms() +
                      (int)InterlockedCompareExchange(&g_padding_frames, 0, 0) / SAMPLES_PER_MS;
        latency = (latency + 5) / 10 * 10;
        swprintf_s(tip, 128, S->tip_latency_fmt, text, latency);
    } else {
        swprintf_s(tip, 128, S->tip_fmt, text);
    }

    g_status_text = text;
    if (st != g_state || wcscmp(tip, g_nid.szTip) != 0) {
        g_state     = st;
        g_nid.hIcon = g_icons[st];
        wcscpy_s(g_nid.szTip, tip);
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
}

static void log_stats(void) {
    JitterStats js;
    EnterCriticalSection(&g_ring_cs);
    js = g_stats;
    LeaveCriticalSection(&g_ring_cs);
    logf("Stats: buffer=%d ms, underruns=%u, dropped=%u, inserted=%u, skips=%u (%u ms), decode_errors=%ld",
         jitter_level_ms(), js.underruns, js.dropped, js.inserted, js.skips, js.skipped_ms,
         InterlockedCompareExchange(&g_decode_errors, 0, 0));
}

static void show_tray_menu(HWND hwnd) {
    HMENU hm = CreatePopupMenu();

    wchar_t server[300], status[400];
    format_server(server, 300);
    swprintf_s(status, 400, S->menu_status_fmt, g_status_text, server);
    AppendMenuW(hm, MF_STRING | MF_GRAYED, IDM_STATUS, status);
    AppendMenuW(hm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hm, MF_STRING, IDM_TOGGLE, g_user_stopped ? S->menu_play : S->menu_stop);
    AppendMenuW(hm, MF_STRING, IDM_WEBUI, S->menu_webui);
    AppendMenuW(hm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hm, MF_STRING, IDM_SETTINGS, S->menu_settings);
    AppendMenuW(hm, MF_STRING, IDM_OPEN_LOG, S->menu_open_log);
    AppendMenuW(hm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hm, MF_STRING, IDM_EXIT, S->menu_exit);

    SetForegroundWindow(hwnd);
    POINT pt; GetCursorPos(&pt);
    TrackPopupMenu(hm, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hm);
}

// ============================================================
// Settings dialog
// ============================================================

static Config s_edit;
static HWND   s_dlg = NULL;

static void trim(wchar_t* s) {
    wchar_t* b = s;
    while (*b == L' ' || *b == L'\t') b++;
    if (b != s) memmove(s, b, (wcslen(b) + 1) * sizeof(wchar_t));
    size_t n = wcslen(s);
    while (n > 0 && (s[n - 1] == L' ' || s[n - 1] == L'\t')) s[--n] = 0;
}

static INT_PTR CALLBACK settings_proc(HWND h, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG:
        s_dlg = h;
        SetWindowTextW(h, S->dlg_caption);
        SetDlgItemTextW(h, IDC_LBL_HOST,        S->lbl_host);
        SetDlgItemTextW(h, IDC_LBL_PORT,        S->lbl_port);
        SetDlgItemTextW(h, IDC_LBL_BUFFER,      S->lbl_buffer);
        SetDlgItemTextW(h, IDC_LBL_BUFFER_NOTE, S->lbl_buffer_note);
        SetDlgItemTextW(h, IDC_AUTOSTART,       S->chk_autostart);
        SetDlgItemTextW(h, IDCANCEL,            S->btn_cancel);
        SendDlgItemMessageW(h, IDC_HOST, EM_LIMITTEXT, 255, 0);
        SetDlgItemTextW(h, IDC_HOST, s_edit.server_host);
        SetDlgItemInt(h, IDC_PORT,   s_edit.server_port, FALSE);
        SetDlgItemInt(h, IDC_BUFFER, s_edit.buffer_ms,   FALSE);
        CheckDlgButton(h, IDC_AUTOSTART, s_edit.autostart ? BST_CHECKED : BST_UNCHECKED);
        SetForegroundWindow(h);
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t host[256];
            GetDlgItemTextW(h, IDC_HOST, host, 256);
            trim(host);
            if (!host[0]) {
                MessageBoxW(h, S->err_host, S->dlg_caption, MB_ICONWARNING);
                SetFocus(GetDlgItem(h, IDC_HOST));
                return TRUE;
            }
            BOOL ok = FALSE;
            UINT port = GetDlgItemInt(h, IDC_PORT, &ok, FALSE);
            if (!ok || port < 1 || port > 65535) {
                MessageBoxW(h, S->err_port, S->dlg_caption, MB_ICONWARNING);
                SetFocus(GetDlgItem(h, IDC_PORT));
                return TRUE;
            }
            UINT buf = GetDlgItemInt(h, IDC_BUFFER, &ok, FALSE);
            if (!ok || buf < (UINT)BUFFER_MS_MIN || buf > (UINT)BUFFER_MS_MAX) {
                MessageBoxW(h, S->err_buffer, S->dlg_caption, MB_ICONWARNING);
                SetFocus(GetDlgItem(h, IDC_BUFFER));
                return TRUE;
            }
            wcscpy_s(s_edit.server_host, host);
            s_edit.server_port = (int)port;
            s_edit.buffer_ms   = (int)buf;
            s_edit.autostart   = (IsDlgButtonChecked(h, IDC_AUTOSTART) == BST_CHECKED);
            EndDialog(h, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(h, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_DESTROY:
        s_dlg = NULL;
        break;
    }
    return FALSE;
}

// Returns TRUE if the user pressed OK (settings saved and playback restarted)
static BOOL show_settings(HWND owner) {
    if (s_dlg) { SetForegroundWindow(s_dlg); return FALSE; }

    s_edit = g_cfg;
    INT_PTR r = DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_SETTINGS),
                                owner, settings_proc, 0);
    if (r != IDOK) return FALSE;

    stop_player();
    g_cfg = s_edit;
    save_config();
    set_autostart(g_cfg.autostart);
    g_user_stopped = false;
    start_player();
    update_status();
    return TRUE;
}

// ============================================================
// Main window (hidden)
// ============================================================

static LRESULT CALLBACK main_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_wm_taskbar_created && msg == g_wm_taskbar_created) {
        tray_add();   // Explorer restarted: the tray icon has to be registered again
        return 0;
    }

    switch (msg) {
    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU)
            show_tray_menu(hwnd);
        else if (lp == WM_LBUTTONDBLCLK)
            PostMessage(hwnd, WM_COMMAND, IDM_SETTINGS, 0);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_STATUS) update_status();
        else if (wp == TIMER_STATS && !g_user_stopped) log_stats();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_TOGGLE:
            if (g_user_stopped) {
                g_user_stopped = false;
                start_player();
            } else {
                stop_player();
                g_user_stopped = true;
                logf("Playback stopped by user");
            }
            update_status();
            break;
        case IDM_WEBUI: {
            wchar_t server[300], url[320];
            format_server(server, 300);
            swprintf_s(url, 320, L"http://%s/", server);
            ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
            break;
        }
        case IDM_SETTINGS:
            show_settings(hwnd);
            break;
        case IDM_OPEN_LOG:
            ShellExecuteW(NULL, L"open", g_appdata_dir, NULL, NULL, SW_SHOW);
            break;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_STATUS);
        KillTimer(hwnd, TIMER_STATS);
        if (!g_user_stopped) log_stats();
        tray_remove();
        stop_player();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================
// WinMain
// ============================================================

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // Single instance
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"raa-player-{3F6C1D2A-8B4E-4C7A-9E21-5A0D7B3C9F14}");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex); return 0;
    }

    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, g_appdata_dir);
    wcscat_s(g_appdata_dir, MAX_PATH, L"\\raa-player");
    CreateDirectoryW(g_appdata_dir, NULL);
    swprintf_s(g_ini_path, MAX_PATH, L"%s\\raa-player.ini", g_appdata_dir);
    swprintf_s(g_log_path, MAX_PATH, L"%s\\raa-player.log", g_appdata_dir);

    log_init();
    logf("raa-player starting (OS build %lu)", get_os_build());

    S = (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE) ? &STR_JA : &STR_EN;

    BOOL config_exists = load_config();
    if (config_exists && g_cfg.autostart) set_autostart(TRUE);   // keep the Run entry pointing at this exe

    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    InitializeCriticalSection(&g_ring_cs);
    g_stop_evt      = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_devchange_evt = CreateEventW(NULL, FALSE, FALSE, NULL);

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    g_wm_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = main_wndproc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = L"raa-player-wnd";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, L"raa-player-wnd", L"RAA Player", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1, 1, NULL, NULL, hInst, NULL);
    // Window stays hidden; it only receives tray and timer messages

    tray_init(g_hwnd);
    SetTimer(g_hwnd, TIMER_STATUS, 500, NULL);
    SetTimer(g_hwnd, TIMER_STATS, STATS_INTERVAL_MS, NULL);

    if (!config_exists) {
        // First run: ask for the server. OK saves the INI and starts playback.
        logf("First run: opening settings dialog");
        if (!show_settings(g_hwnd)) {
            logf("First run cancelled, exiting");
            DestroyWindow(g_hwnd);
        }
    } else {
        start_player();
    }
    if (IsWindow(g_hwnd)) update_status();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WSACleanup();
    logf("raa-player exiting");
    if (g_log_file) fclose(g_log_file);
    CloseHandle(mutex);
    return 0;
}
