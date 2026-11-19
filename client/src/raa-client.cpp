// raa-client.cpp - RAA Phase 2: Win32 tray audio client
// WASAPI Process Loopback -> libopus (DTX) -> RTP/UDP (RFC 3550 + RFC 7587)
//
// Payload Type : 111 (Opus, dynamic, 48kHz mono)
// Clock rate   : 48000 Hz
// SSRC         : persistent 32-bit ID (stored in %APPDATA%\raa-client\raa-client.ini)

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <commdlg.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <initguid.h>
#include <roapi.h>
#include <ksmedia.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <rpc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "opus.h"
#include "snmp-trap.h"
#include "../resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "advapi32.lib")

// ComCtl32 v6 (visual styles, required for property sheet tabs)
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Window messages and control IDs are in resource.h (shared with raa-client.rc).
#define RTP_PT_OPUS 111

#define NOTIFY_BATCH 0   // [Notify] mode=batch : run start / stop batch files
#define NOTIFY_SNMP  1   // [Notify] mode=snmp  : send SNMP traps directly

// ============================================================
// Localization
// ============================================================

struct Strings {
    const wchar_t* tray_active;
    const wchar_t* tray_silent;
    const wchar_t* tray_error;
    const wchar_t* status_active;
    const wchar_t* status_silent;
    const wchar_t* status_error;
    const wchar_t* status_fmt;
    const wchar_t* menu_settings;
    const wchar_t* menu_open_log;
    const wchar_t* menu_reload;
    const wchar_t* menu_exit;
    const wchar_t* sheet_caption;
    const wchar_t* tab_connection;
    const wchar_t* tab_audio;
    const wchar_t* tab_detection;
    const wchar_t* tab_notify;
    const wchar_t* tab_general;
    const wchar_t* file_filter;
    const wchar_t* loopback_process;
    const wchar_t* loopback_classic;
    const wchar_t* loopback_note;
    // Notify page
    const wchar_t* notify_label;
    const wchar_t* notify_batch;
    const wchar_t* notify_snmp;
    const wchar_t* on_start;
    const wchar_t* on_stop;
    const wchar_t* batch_timeout;
    const wchar_t* batch_note;
    const wchar_t* snmp_start_label;
    const wchar_t* snmp_stop_label;
    const wchar_t* snmp_test;
    const wchar_t* snmp_charset;
    const wchar_t* snmp_hint;
    const wchar_t* snmp_invalid_fmt;   // %ls = on_start / on_stop, %ls = parser message
    const wchar_t* snmp_test_empty;
    const wchar_t* snmp_test_ok_fmt;   // %ls = destination and size
    const wchar_t* snmp_test_fail_fmt; // %ls = error
};

static const Strings STR_JA = {
    L"RAA Client - 送信中",
    L"RAA Client - 無音",
    L"RAA Client - エラー",
    L"送信中",
    L"無音",
    L"エラー",
    L"状態: %s  (SSRC: %08x)",
    L"設定...",
    L"ログフォルダを開く",
    L"設定ファイルを再読み込み",
    L"終了",
    L"RAA Client 設定",
    L"接続",
    L"オーディオ",
    L"無音検知",
    L"通知",
    L"全般",
    L"スクリプト (*.bat;*.cmd;*.exe;*.ps1)\0*.bat;*.cmd;*.exe;*.ps1\0すべて (*.*)\0*.*\0",
    L"ボリューム/ミュートの影響を受けない方式 *",
    L"ボリューム/ミュートの影響を受ける方式",
    L"* Windows 10 ビルド 19041 (2004) 以降が必要",
    L"通知方法:",
    L"バッチファイルを実行",
    L"SNMP Trap を送信",
    L"音声開始時",
    L"音声停止時",
    L"タイムアウト (ms):",
    L"時間内に終わらないバッチは、そこから起動したプロセスごと終了させます（0 = 無制限）。",
    L"音声開始時に送る Trap:",
    L"音声停止時に送る Trap:",
    L"テスト送信",
    L"文字コード:",
    L"snmptrap.exe の引数と同じ書式です（-v 1 / -v 2c）。バッチファイルの中身をそのまま貼り付けても構いません。",
    L"%ls の Trap の設定に誤りがあります。\n\n%ls",
    L"Trap の引数が入力されていません。",
    L"Trap を送信しました（%ls）。\n\nUDP のため、届いたかどうかはここではわかりません。マネージャー側で受信を確認してください。",
    L"Trap を送信できませんでした。\n\n%ls",
};

static const Strings STR_EN = {
    L"RAA Client - Sending",
    L"RAA Client - Silent",
    L"RAA Client - Error",
    L"Sending",
    L"Silent",
    L"Error",
    L"Status: %s  (SSRC: %08x)",
    L"Settings...",
    L"Open log folder",
    L"Reload settings",
    L"Exit",
    L"RAA Client Settings",
    L"Connection",
    L"Audio",
    L"Detection",
    L"Notify",
    L"General",
    L"Scripts (*.bat;*.cmd;*.exe;*.ps1)\0*.bat;*.cmd;*.exe;*.ps1\0All files (*.*)\0*.*\0",
    L"Independent of master volume *",
    L"Affected by master volume",
    L"* Requires Windows 10 build 19041 (2004) or later",
    L"Method:",
    L"Run batch file",
    L"Send SNMP trap",
    L"On audio start",
    L"On audio stop",
    L"Timeout (ms):",
    L"A script still running after the timeout is terminated together with the processes it started (0 = no limit).",
    L"Trap on audio start:",
    L"Trap on audio stop:",
    L"Send test",
    L"Charset:",
    L"Same syntax as the snmptrap.exe arguments (-v 1 / -v 2c). You can paste the contents of your batch file as is.",
    L"The trap for \"%ls\" is invalid.\n\n%ls",
    L"No trap arguments entered.",
    L"Trap sent (%ls).\n\nUDP gives no delivery confirmation; check that the manager received it.",
    L"Could not send the trap.\n\n%ls",
};

static const Strings* S = &STR_JA;

static BOOL ui_ja(void) { return S == &STR_JA; }

// ============================================================
// Config
// ============================================================

struct Config {
    wchar_t server_ip[64];
    int     server_port;
    int     bitrate_kbps;
    int     silence_guard_ms;   // audio→silence: how long silence must persist before batch_stop
    int     audio_guard_ms;     // silence→audio: how long audio must persist before batch_start
    wchar_t batch_start[MAX_PATH];
    wchar_t batch_stop[MAX_PATH];
    int     batch_timeout_ms;
    BOOL    autostart;
    int     loopback_mode;  // 0 = Process Loopback (build >= 19041), 1 = Classic Loopback
    int     notify_mode;    // NOTIFY_BATCH / NOTIFY_SNMP
    wchar_t snmp_start[SNMP_ARGS_MAX];   // snmptrap-style arguments, canonical one-line form
    wchar_t snmp_stop[SNMP_ARGS_MAX];
    UINT    snmp_charset;   // SNMP_CHARSET_SJIS / SNMP_CHARSET_UTF8
};

static Config g_cfg = {
    L"10.0.0.1", 4010,
    64,
    200, 100,
    L"", L"", 5000,
    FALSE,
    0,  // loopback_mode: 0 = Process Loopback
    NOTIFY_BATCH, L"", L"", SNMP_CHARSET_SJIS
};

static uint32_t g_ssrc = 0;
static HWND     g_hwnd = NULL;

static DWORD g_os_build = 0;
static BOOL  g_process_loopback_supported = FALSE;

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
// SSRC persistence
// ============================================================

static wchar_t g_appdata_dir[MAX_PATH];
static wchar_t g_ini_path[MAX_PATH];
static wchar_t g_log_path[MAX_PATH];

// ============================================================
// Config INI  (Windows WritePrivateProfileString / GetPrivateProfileInt)
// ============================================================

// The *PrivateProfile*W functions keep Unicode only when the file is UTF-16LE with a BOM;
// otherwise they read and write the ANSI code page. Trap texts contain Japanese, so the INI
// is kept in UTF-16. An existing ANSI file (older versions) or UTF-8 file (edited with
// Notepad) is converted once; a missing file is created empty with a BOM.
static void ini_ensure_unicode(void) {
    char*  data = NULL;
    DWORD  size = 0;
    HANDLE h = CreateFileW(g_ini_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER li = {};
        if (!GetFileSizeEx(h, &li) || li.QuadPart > 1024 * 1024) { CloseHandle(h); return; }
        size = (DWORD)li.QuadPart;
        data = (char*)malloc(size + 1);
        DWORD rd = 0;
        if (!data || !ReadFile(h, data, size, &rd, NULL)) { CloseHandle(h); free(data); return; }
        CloseHandle(h);
        size = rd;
        if (size >= 2 && (BYTE)data[0] == 0xFF && (BYTE)data[1] == 0xFE) { free(data); return; }
    }

    const char* src = data;
    int  n  = (int)size;
    UINT cp = CP_ACP;
    if (n >= 3 && (BYTE)src[0] == 0xEF && (BYTE)src[1] == 0xBB && (BYTE)src[2] == 0xBF) {
        cp = CP_UTF8; src += 3; n -= 3;
    } else if (n > 0 && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, n, NULL, 0) > 0) {
        cp = CP_UTF8;   // plain ASCII converts the same either way
    }
    int wn = (n > 0) ? MultiByteToWideChar(cp, 0, src, n, NULL, 0) : 0;
    wchar_t* w = (wchar_t*)malloc((size_t)(wn + 1) * sizeof(wchar_t));
    if (w) {
        w[0] = 0xFEFF;
        if (wn > 0) MultiByteToWideChar(cp, 0, src, n, w + 1, wn);
        // Write a temporary file and swap it in, so a crash cannot leave a half-written INI.
        wchar_t tmp[MAX_PATH + 8];
        swprintf_s(tmp, MAX_PATH + 8, L"%s.tmp", g_ini_path);
        HANDLE o = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (o != INVALID_HANDLE_VALUE) {
            DWORD bytes = (DWORD)((wn + 1) * sizeof(wchar_t)), wr = 0;
            BOOL ok = WriteFile(o, w, bytes, &wr, NULL) && wr == bytes;
            CloseHandle(o);
            if (!ok || !MoveFileExW(tmp, g_ini_path, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp);
        }
        free(w);
    }
    free(data);
}

// GetPrivateProfileString strips one pair of quotes around a value. Values that start or end
// with a quote get an extra pair so that they read back unchanged.
static void ini_write_verbatim(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    size_t n = wcslen(value);
    BOOL edge_quote = n > 0 && (value[0] == L'"' || value[0] == L'\'' ||
                                value[n - 1] == L'"' || value[n - 1] == L'\'');
    if (!edge_quote) {
        WritePrivateProfileStringW(section, key, value, g_ini_path);
        return;
    }
    wchar_t buf[SNMP_ARGS_MAX + 2];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"\"%ls\"", value);
    WritePrivateProfileStringW(section, key, buf, g_ini_path);
}

static void save_config(void) {
    wchar_t buf[32];
    ini_ensure_unicode();

    WritePrivateProfileStringW(L"Connection", L"server_ip",   g_cfg.server_ip,  g_ini_path);
    swprintf_s(buf, 32, L"%d", g_cfg.server_port);
    WritePrivateProfileStringW(L"Connection", L"server_port", buf,              g_ini_path);
    swprintf_s(buf, 32, L"%08x", g_ssrc);
    WritePrivateProfileStringW(L"Connection", L"ssrc",        buf,              g_ini_path);

    swprintf_s(buf, 32, L"%d", g_cfg.bitrate_kbps);
    WritePrivateProfileStringW(L"Audio", L"bitrate_kbps",     buf,              g_ini_path);
    swprintf_s(buf, 32, L"%d", g_cfg.loopback_mode);
    WritePrivateProfileStringW(L"Audio", L"loopback_mode",    buf,              g_ini_path);

    swprintf_s(buf, 32, L"%d", g_cfg.silence_guard_ms);
    WritePrivateProfileStringW(L"Detection", L"silence_guard_ms", buf,          g_ini_path);
    swprintf_s(buf, 32, L"%d", g_cfg.audio_guard_ms);
    WritePrivateProfileStringW(L"Detection", L"audio_guard_ms",   buf,          g_ini_path);

    WritePrivateProfileStringW(L"Batch", L"start_script",     g_cfg.batch_start, g_ini_path);
    WritePrivateProfileStringW(L"Batch", L"stop_script",      g_cfg.batch_stop,  g_ini_path);
    swprintf_s(buf, 32, L"%d", g_cfg.batch_timeout_ms);
    WritePrivateProfileStringW(L"Batch", L"timeout_ms",       buf,              g_ini_path);

    WritePrivateProfileStringW(L"Notify", L"mode",
                               g_cfg.notify_mode == NOTIFY_SNMP ? L"snmp" : L"batch", g_ini_path);
    ini_write_verbatim(L"SnmpTrap", L"start_args", g_cfg.snmp_start);
    ini_write_verbatim(L"SnmpTrap", L"stop_args",  g_cfg.snmp_stop);
    WritePrivateProfileStringW(L"SnmpTrap", L"charset",
                               g_cfg.snmp_charset == SNMP_CHARSET_UTF8 ? L"utf-8" : L"shift_jis", g_ini_path);

    WritePrivateProfileStringW(L"General", L"autostart", g_cfg.autostart ? L"1" : L"0", g_ini_path);
}

// Returns TRUE if the INI file existed and was loaded (FALSE = first run)
static BOOL load_config(void) {
    if (GetFileAttributesW(g_ini_path) == INVALID_FILE_ATTRIBUTES)
        return FALSE;
    ini_ensure_unicode();

    GetPrivateProfileStringW(L"Connection", L"server_ip", L"10.0.0.1",
                             g_cfg.server_ip, 64, g_ini_path);
    g_cfg.server_port = (int)GetPrivateProfileIntW(L"Connection", L"server_port", 4010, g_ini_path);

    wchar_t ssrc_hex[16] = {};
    GetPrivateProfileStringW(L"Connection", L"ssrc", L"", ssrc_hex, 16, g_ini_path);
    if (ssrc_hex[0]) g_ssrc = wcstoul(ssrc_hex, NULL, 16);

    g_cfg.bitrate_kbps     = (int)GetPrivateProfileIntW(L"Audio", L"bitrate_kbps", 64, g_ini_path);
    g_cfg.loopback_mode    = (int)GetPrivateProfileIntW(L"Audio", L"loopback_mode", 0, g_ini_path);

    g_cfg.silence_guard_ms = (int)GetPrivateProfileIntW(L"Detection", L"silence_guard_ms", 200, g_ini_path);
    g_cfg.audio_guard_ms   = (int)GetPrivateProfileIntW(L"Detection", L"audio_guard_ms",   100, g_ini_path);

    GetPrivateProfileStringW(L"Batch", L"start_script", L"", g_cfg.batch_start, MAX_PATH, g_ini_path);
    GetPrivateProfileStringW(L"Batch", L"stop_script",  L"", g_cfg.batch_stop,  MAX_PATH, g_ini_path);
    g_cfg.batch_timeout_ms = (int)GetPrivateProfileIntW(L"Batch", L"timeout_ms", 5000, g_ini_path);

    wchar_t word[16];
    GetPrivateProfileStringW(L"Notify", L"mode", L"batch", word, 16, g_ini_path);
    g_cfg.notify_mode = (_wcsicmp(word, L"snmp") == 0) ? NOTIFY_SNMP : NOTIFY_BATCH;
    GetPrivateProfileStringW(L"SnmpTrap", L"start_args", L"", g_cfg.snmp_start, SNMP_ARGS_MAX, g_ini_path);
    GetPrivateProfileStringW(L"SnmpTrap", L"stop_args",  L"", g_cfg.snmp_stop,  SNMP_ARGS_MAX, g_ini_path);
    GetPrivateProfileStringW(L"SnmpTrap", L"charset", L"shift_jis", word, 16, g_ini_path);
    g_cfg.snmp_charset = (_wcsicmp(word, L"utf-8") == 0 || _wcsicmp(word, L"utf8") == 0)
                       ? SNMP_CHARSET_UTF8 : SNMP_CHARSET_SJIS;

    g_cfg.autostart = (GetPrivateProfileIntW(L"General", L"autostart", 0, g_ini_path) != 0);
    return TRUE;
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
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        RegSetValueExW(hk, L"raa-client", 0, REG_SZ,
                       (BYTE*)exe, (DWORD)((wcslen(exe)+1)*sizeof(wchar_t)));
    } else {
        RegDeleteValueW(hk, L"raa-client");
    }
    RegCloseKey(hk);
}

// ============================================================
// Logging
// ============================================================

static FILE* g_log_file = NULL;
static CRITICAL_SECTION g_log_cs;

static void log_init(void) {
    InitializeCriticalSection(&g_log_cs);
    _wfopen_s(&g_log_file, g_log_path, L"a");
}

static void log_close(void) {
    EnterCriticalSection(&g_log_cs);
    if (g_log_file) { fclose(g_log_file); g_log_file = NULL; }
    LeaveCriticalSection(&g_log_cs);
}

// For logging wide strings (paths, SNMP messages) as UTF-8.
static const char* utf8(const wchar_t* s, char* buf, int cap) {
    if (WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, cap, NULL, NULL) <= 0) buf[0] = 0;
    return buf;
}

static void logf(const char* fmt, ...) {
    EnterCriticalSection(&g_log_cs);
    if (!g_log_file) { LeaveCriticalSection(&g_log_cs); return; }
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log_file, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log_file, fmt, ap);
    va_end(ap);
    fprintf(g_log_file, "\n");
    fflush(g_log_file);
    LeaveCriticalSection(&g_log_cs);
}

// ============================================================
// Tray icon (GDI speaker icon, 3 states)
// ============================================================

enum AudioState { AS_ACTIVE, AS_SILENT, AS_ERROR };
static AudioState g_audio_state = AS_SILENT;

static HICON load_tray_icon(AudioState state) {
    int id = (state == AS_ACTIVE) ? IDI_TRAY_ACTIVE
           : (state == AS_SILENT) ? IDI_TRAY_SILENT
                                  : IDI_TRAY_ERROR;
    return (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(id),
                             IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
}

// ============================================================
// Tray management
// ============================================================

static NOTIFYICONDATAW g_nid = {};
static HICON g_icons[3] = {};

static void tray_init(HWND hwnd) {
    g_icons[AS_ACTIVE] = load_tray_icon(AS_ACTIVE);
    g_icons[AS_SILENT] = load_tray_icon(AS_SILENT);
    g_icons[AS_ERROR]  = load_tray_icon(AS_ERROR);

    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon            = g_icons[AS_SILENT];
    wcscpy_s(g_nid.szTip, L"RAA Client");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void tray_update(AudioState state) {
    g_audio_state = state;
    g_nid.hIcon = g_icons[state];
    const wchar_t* tip = (state == AS_ACTIVE) ? S->tray_active
                       : (state == AS_ERROR)  ? S->tray_error
                                              : S->tray_silent;
    wcscpy_s(g_nid.szTip, tip);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void tray_remove(void) {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    for (int i = 0; i < 3; i++) if (g_icons[i]) DestroyIcon(g_icons[i]);
}

static void show_tray_menu(HWND hwnd) {
    HMENU hm = CreatePopupMenu();

    wchar_t status[64];
    const wchar_t* s = (g_audio_state == AS_ACTIVE) ? S->status_active
                     : (g_audio_state == AS_ERROR)  ? S->status_error : S->status_silent;
    swprintf_s(status, 64, S->status_fmt, s, g_ssrc);
    AppendMenuW(hm, MF_STRING | MF_DISABLED | MF_GRAYED, IDM_STATUS, status);
    AppendMenuW(hm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hm, MF_STRING, IDM_SETTINGS, S->menu_settings);
    AppendMenuW(hm, MF_STRING, IDM_OPEN_LOG, S->menu_open_log);
    AppendMenuW(hm, MF_STRING, IDM_RELOAD,   S->menu_reload);
    AppendMenuW(hm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hm, MF_STRING, IDM_EXIT, S->menu_exit);

    SetForegroundWindow(hwnd);
    POINT pt; GetCursorPos(&pt);
    TrackPopupMenu(hm, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hm);
}

// ============================================================
// Start / stop notification (batch file or SNMP trap)
// ============================================================
//
// The audio thread only queues events (notify_post); a single worker thread runs the batch
// files / sends the traps in order. Running a batch takes from tens of milliseconds up to
// the timeout, far longer than the 100 ms WASAPI capture buffer, so doing it on the audio
// thread made the audio choppy. One worker (rather than a thread per event) keeps start and
// stop in order, so the receiver never sees them swapped.

enum { NE_START = 0, NE_STOP = 1 };

// Settings for the notifications. Immutable once published and shared by reference count:
// each queued event holds a reference to the settings in effect when it was posted.
struct NotifyConfig {
    volatile LONG refs;
    int      mode;                       // NOTIFY_BATCH / NOTIFY_SNMP
    wchar_t  batch[2][MAX_PATH];         // [NE_START], [NE_STOP]
    int      batch_timeout_ms;
    BOOL     trap_enabled[2];
    SnmpTrap trap[2];
};

#define NOTIFY_QUEUE_MAX 16

struct NotifyItem { int ev; NotifyConfig* cfg; };

static CRITICAL_SECTION   g_nq_cs;
static CONDITION_VARIABLE g_nq_cv;
static NotifyItem         g_nq[NOTIFY_QUEUE_MAX];
static int                g_nq_head  = 0;
static int                g_nq_count = 0;
static BOOL               g_nq_quit  = FALSE;
static NotifyConfig*      g_notify_cfg = NULL;   // current settings, guarded by g_nq_cs
static HANDLE             g_notify_thread = NULL;

static const char* notify_name(int ev) { return ev == NE_START ? "start" : "stop"; }

static void notify_cfg_release(NotifyConfig* c) {
    if (c && InterlockedDecrement(&c->refs) == 0) delete c;
}

// Builds the notification settings from g_cfg. SNMP traps are parsed and encoded here,
// once, so that sending one later is just a send() call.
static NotifyConfig* notify_config_from_cfg(void) {
    NotifyConfig* c = new NotifyConfig();   // value-initialized (all zero)
    c->refs = 1;
    c->mode = g_cfg.notify_mode;
    wcscpy_s(c->batch[NE_START], g_cfg.batch_start);
    wcscpy_s(c->batch[NE_STOP],  g_cfg.batch_stop);
    c->batch_timeout_ms = g_cfg.batch_timeout_ms;
    if (c->mode == NOTIFY_SNMP) {
        const wchar_t* args[2] = { g_cfg.snmp_start, g_cfg.snmp_stop };
        for (int ev = 0; ev < 2; ev++) {
            if (snmp_args_blank(args[ev])) continue;
            wchar_t err[512];
            if (snmp_trap_parse(args[ev], g_cfg.snmp_charset, FALSE, &c->trap[ev], err, 512)) {
                c->trap_enabled[ev] = TRUE;
            } else {
                char u[1600];
                logf("SNMP trap (%s) settings are invalid, not sending it: %s", notify_name(ev), utf8(err, u, sizeof(u)));
            }
        }
    }
    return c;
}

// Takes ownership of c. Events already queued keep the settings they were posted with.
static void notify_set_config(NotifyConfig* c) {
    EnterCriticalSection(&g_nq_cs);
    NotifyConfig* old = g_notify_cfg;
    g_notify_cfg = c;
    LeaveCriticalSection(&g_nq_cs);
    notify_cfg_release(old);
}

// Called from the audio thread. Only takes a short lock; never waits for the action.
static void notify_post(int ev) {
    NotifyItem dropped = { -1, NULL };
    EnterCriticalSection(&g_nq_cs);
    if (!g_notify_cfg) { LeaveCriticalSection(&g_nq_cs); return; }
    if (g_nq_count == NOTIFY_QUEUE_MAX) {
        // The worker is stuck (e.g. a batch with no timeout). Drop the oldest event so the
        // latest state still goes out once it recovers.
        dropped = g_nq[g_nq_head];
        g_nq_head = (g_nq_head + 1) % NOTIFY_QUEUE_MAX;
        g_nq_count--;
    }
    NotifyItem* it = &g_nq[(g_nq_head + g_nq_count) % NOTIFY_QUEUE_MAX];
    it->ev  = ev;
    it->cfg = g_notify_cfg;
    InterlockedIncrement(&it->cfg->refs);
    g_nq_count++;
    LeaveCriticalSection(&g_nq_cs);
    WakeConditionVariable(&g_nq_cv);

    if (dropped.cfg) {
        logf("Notify queue full: dropped a pending %s event", notify_name(dropped.ev));
        notify_cfg_release(dropped.cfg);
    }
}

// Runs a batch file, waiting at most timeout_ms (0 = no limit). On timeout the script and
// everything it started (cmd.exe -> snmptrap.exe ...) are terminated via a job object, so
// hung scripts cannot pile up.
static void run_batch(const wchar_t* path, int timeout_ms, const char* what) {
    if (!path || path[0] == L'\0') return;
    char u[MAX_PATH * 3];
    wchar_t cmd[MAX_PATH + 16];
    swprintf_s(cmd, MAX_PATH + 16, L"\"%s\"", path);
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    DWORD t0 = GetTickCount();
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        NULL, NULL, &si, &pi)) {
        logf("Batch %s: cannot start %s (error %lu)", what, utf8(path, u, sizeof(u)), GetLastError());
        return;
    }
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) { CloseHandle(job); job = NULL; }
    ResumeThread(pi.hThread);

    DWORD wr = WaitForSingleObject(pi.hProcess, timeout_ms > 0 ? (DWORD)timeout_ms : INFINITE);
    if (wr == WAIT_TIMEOUT) {
        if (job) TerminateJobObject(job, 1); else TerminateProcess(pi.hProcess, 1);
        logf("Batch %s: still running after %d ms, terminated", what, timeout_ms);
    } else {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        logf("Batch %s: exit code %lu (%lu ms)", what, code, GetTickCount() - t0);
    }
    if (job) CloseHandle(job);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

static void notify_run(int ev, const NotifyConfig* c) {
    if (c->mode == NOTIFY_SNMP) {
        if (!c->trap_enabled[ev]) return;
        wchar_t info[256];
        char u[800];
        BOOL ok = snmp_trap_send(&c->trap[ev], FALSE, info, 256);
        logf("SNMP trap %s: %s %s", notify_name(ev), ok ? "sent," : "FAILED:", utf8(info, u, sizeof(u)));
    } else {
        run_batch(c->batch[ev], c->batch_timeout_ms, notify_name(ev));
    }
}

static DWORD WINAPI notify_thread_proc(LPVOID) {
    for (;;) {
        EnterCriticalSection(&g_nq_cs);
        while (g_nq_count == 0 && !g_nq_quit)
            SleepConditionVariableCS(&g_nq_cv, &g_nq_cs, INFINITE);
        if (g_nq_count == 0) {          // quit requested and nothing left to send
            LeaveCriticalSection(&g_nq_cs);
            break;
        }
        NotifyItem it = g_nq[g_nq_head];
        g_nq_head = (g_nq_head + 1) % NOTIFY_QUEUE_MAX;
        g_nq_count--;
        LeaveCriticalSection(&g_nq_cs);

        notify_run(it.ev, it.cfg);
        notify_cfg_release(it.cfg);
    }
    return 0;
}

static void notify_init(void) {
    InitializeCriticalSection(&g_nq_cs);
    InitializeConditionVariable(&g_nq_cv);
    g_notify_thread = CreateThread(NULL, 0, notify_thread_proc, NULL, 0, NULL);
}

// Lets the worker finish the queued events (e.g. the final stop) and waits up to wait_ms.
static void notify_shutdown(DWORD wait_ms) {
    if (!g_notify_thread) return;
    EnterCriticalSection(&g_nq_cs);
    g_nq_quit = TRUE;
    LeaveCriticalSection(&g_nq_cs);
    WakeConditionVariable(&g_nq_cv);
    if (WaitForSingleObject(g_notify_thread, wait_ms) == WAIT_TIMEOUT)
        logf("Notify worker still busy after %lu ms, exiting anyway", wait_ms);
    CloseHandle(g_notify_thread);
    g_notify_thread = NULL;
}

// ============================================================
// RTP + audio thread
// ============================================================

static volatile LONG g_thread_running = 0;
static volatile LONG g_thread_stop    = 0;
static HANDLE g_audio_thread = NULL;

static uint16_t g_rtp_seq = 0;

static int build_rtp(BYTE* out, uint32_t ts48k, uint32_t ssrc,
                     const BYTE* payload, int payload_len, int marker) {
    out[0] = 0x80;
    out[1] = (BYTE)((marker ? 0x80u : 0x00u) | RTP_PT_OPUS);
    out[2] = (BYTE)(g_rtp_seq >> 8); out[3] = (BYTE)(g_rtp_seq); g_rtp_seq++;
    out[4]=(BYTE)(ts48k>>24); out[5]=(BYTE)(ts48k>>16);
    out[6]=(BYTE)(ts48k>>8);  out[7]=(BYTE)(ts48k);
    out[8]=(BYTE)(ssrc>>24);  out[9]=(BYTE)(ssrc>>16);
    out[10]=(BYTE)(ssrc>>8);  out[11]=(BYTE)(ssrc);
    memcpy(out + 12, payload, payload_len);
    return 12 + payload_len;
}

// ---- ActivationHandler ----

class ActivationHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    HANDLE        m_event;
    IAudioClient* m_client;
    HRESULT       m_hr;
    LONG          m_refCount;

    ActivationHandler() : m_event(NULL), m_client(NULL), m_hr(E_PENDING), m_refCount(1) {
        m_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    }
    ~ActivationHandler() {
        if (m_event)  CloseHandle(m_event);
        if (m_client) m_client->Release();
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0) delete this;
        return ref;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            { *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this); AddRef(); return S_OK; }
        if (riid == __uuidof(IAgileObject))
            { *ppv = static_cast<IAgileObject*>(this); AddRef(); return S_OK; }
        *ppv = NULL; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT hr = E_FAIL; IUnknown* pAudio = NULL;
        op->GetActivateResult(&hr, &pAudio);
        m_hr = hr;
        if (SUCCEEDED(hr) && pAudio) {
            pAudio->QueryInterface(__uuidof(IAudioClient), (void**)&m_client);
            pAudio->Release();
        }
        SetEvent(m_event);
        return S_OK;
    }
};

struct ThreadConfig {
    char    server_ip[64];
    int     server_port;
    uint32_t ssrc;
    int     bitrate_kbps;
    int     silence_guard_ms;
    int     audio_guard_ms;
    BOOL    use_process_loopback;
};

static DWORD WINAPI audio_thread_proc(LPVOID param) {
    ThreadConfig* tc = (ThreadConfig*)param;
    InterlockedExchange(&g_thread_running, 1);

    // UDP socket (common to both loopback paths)
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        logf("socket() failed"); PostMessage(g_hwnd, WM_AUDIO_STATE, AS_ERROR, 0);
        InterlockedExchange(&g_thread_running, 0); delete tc; return 1;
    }
    sockaddr_in srv = {};
    srv.sin_family = AF_INET; srv.sin_port = htons((u_short)tc->server_port);
    inet_pton(AF_INET, tc->server_ip, &srv.sin_addr);

    IAudioClient* ac = NULL;
    bool use_ro = false;

    if (tc->use_process_loopback) {
        // ---- Process Loopback (Windows 10 build 19041+, master-volume independent) ----
        HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
        if (FAILED(hr)) {
            logf("RoInitialize failed: 0x%08lX", hr);
            PostMessage(g_hwnd, WM_AUDIO_STATE, AS_ERROR, 0);
            closesocket(sock); InterlockedExchange(&g_thread_running, 0); delete tc; return 1;
        }
        use_ro = true;

        AUDIOCLIENT_ACTIVATION_PARAMS params = {};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId  = GetCurrentProcessId();
        params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT pv = {}; PropVariantInit(&pv);
        pv.vt = VT_BLOB; pv.blob.cbSize = sizeof(params); pv.blob.pBlobData = (BYTE*)&params;

        ActivationHandler* handler = new ActivationHandler();
        IActivateAudioInterfaceAsyncOperation* asyncOp = NULL;
        hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                         __uuidof(IAudioClient), &pv, handler, &asyncOp);
        if (FAILED(hr)) {
            logf("ActivateAudioInterfaceAsync failed: 0x%08lX", hr);
            PostMessage(g_hwnd, WM_AUDIO_STATE, AS_ERROR, 0);
            handler->Release(); closesocket(sock); RoUninitialize();
            InterlockedExchange(&g_thread_running, 0); delete tc; return 1;
        }
        WaitForSingleObject(handler->m_event, 5000);
        if (FAILED(handler->m_hr) || !handler->m_client) {
            logf("Audio activation failed: 0x%08lX", handler->m_hr);
            PostMessage(g_hwnd, WM_AUDIO_STATE, AS_ERROR, 0);
            asyncOp->Release(); handler->Release(); closesocket(sock); RoUninitialize();
            InterlockedExchange(&g_thread_running, 0); delete tc; return 1;
        }
        ac = handler->m_client; handler->m_client = NULL;
        asyncOp->Release(); handler->Release();
    } else {
        // ---- Classic Loopback (all Windows 10 builds, affected by master volume) ----
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            logf("CoInitializeEx failed: 0x%08lX", hr);
            PostMessage(g_hwnd, WM_AUDIO_STATE, AS_ERROR, 0);
            closesocket(sock); InterlockedExchange(&g_thread_running, 0); delete tc; return 1;
        }

        IMMDeviceEnumerator* pEnum = NULL;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
        if (FAILED(hr)) {
            logf("CoCreateInstance(MMDeviceEnumerator) failed: 0x%08lX", hr);
            PostMessage(g_hwnd, WM_AUDIO_STATE, AS_ERROR, 0);
            CoUninitialize(); closesocket(sock);
            InterlockedExchange(&g_thread_running, 0); delete tc; return 1;
        }
        IMMDevice* pDev = NULL;
        hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev);
        pEnum->Release();
        if (FAILED(hr)) {
            logf("GetDefaultAudioEndpoint failed: 0x%08lX", hr);
            PostMessage(g_hwnd, WM_AUDIO_STATE, AS_ERROR, 0);
            CoUninitialize(); closesocket(sock);
            InterlockedExchange(&g_thread_running, 0); delete tc; return 1;
        }
        hr = pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&ac);
        pDev->Release();
        if (FAILED(hr)) {
            logf("IMMDevice::Activate failed: 0x%08lX", hr);
            PostMessage(g_hwnd, WM_AUDIO_STATE, AS_ERROR, 0);
            CoUninitialize(); closesocket(sock);
            InterlockedExchange(&g_thread_running, 0); delete tc; return 1;
        }
    }

    WAVEFORMATEXTENSIBLE wfex = {};
    wfex.Format.wFormatTag       = WAVE_FORMAT_EXTENSIBLE;
    wfex.Format.nChannels        = 1;
    wfex.Format.nSamplesPerSec   = 48000;
    wfex.Format.wBitsPerSample   = 32;
    wfex.Format.nBlockAlign      = 4;
    wfex.Format.nAvgBytesPerSec  = 48000 * 4;
    wfex.Format.cbSize           = (WORD)(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    wfex.Samples.wValidBitsPerSample = 32;
    wfex.dwChannelMask           = SPEAKER_FRONT_CENTER;
    wfex.SubFormat               = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    HRESULT hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                        AUDCLNT_STREAMFLAGS_LOOPBACK |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                        1000000, 0, (WAVEFORMATEX*)&wfex, NULL);
    if (FAILED(hr)) {
        logf("Initialize failed: 0x%08lX", hr);
        PostMessage(g_hwnd, WM_AUDIO_STATE, AS_ERROR, 0);
        ac->Release(); closesocket(sock);
        if (use_ro) RoUninitialize(); else CoUninitialize();
        InterlockedExchange(&g_thread_running, 0); delete tc; return 1;
    }

    IAudioCaptureClient* cap = NULL;
    ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cap);

    int opus_err;
    OpusEncoder* enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_AUDIO, &opus_err);
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(tc->bitrate_kbps * 1000));
    opus_encoder_ctl(enc, OPUS_SET_DTX(1));

    ac->Start();
    logf("Audio started (%s) -> %s:%d  SSRC=%08x",
         tc->use_process_loopback ? "ProcessLoopback" : "ClassicLoopback",
         tc->server_ip, tc->server_port, tc->ssrc);

    const int FRAME_SAMPLES     = 960;   // 48000Hz × 20ms
    const int RTP_TS_PER_FRAME  = 960;   // RFC 7587: Opus RTP clock is 48kHz regardless of encoder rate
    float    pcm[FRAME_SAMPLES] = {};
    int      pcm_fill        = 0;
    uint32_t ts48k           = 0;
    int      was_silent      = 1;
    bool     frame_has_audio = false;

    // Silence heartbeat: send at most one packet every 19 frames (~380ms) during silence.
    // Genuine audio frames (len > AUDIO_LEN_MIN) are sent immediately and reset the counter.
    // DTX silence (len<=2) and CN updates (len≈3) are both rate-limited to avoid the
    // synchronization artifact where the heartbeat and CN update cycle align and produce
    // paired packets. AUDIO_LEN_MIN=8 safely separates CN updates (~3B) from audio at
    // 64 kbps (~160B); no valid Opus audio frame at any supported bitrate falls below 8B.
    static const int AUDIO_LEN_MIN = 8;
    int hb_counter = 0;
    DWORD last_send_tick = GetTickCount();

    // DTX guard state (ms-based; frames are 20ms each)
    int silence_frames = tc->silence_guard_ms / 20;  // silent frames needed to trigger the stop notification
    int audio_frames   = tc->audio_guard_ms   / 20;  // active frames needed to trigger the start notification
    int silent_count = 0, active_count = 0;
    BOOL sending = TRUE;  // start in active mode, the start notification fires immediately
    // Notifications are only queued here; the notify worker runs them (see notify_post).
    notify_post(NE_START);

    BYTE opus_buf[4000];
    BYTE pkt_buf[12 + 4000];

    while (!InterlockedCompareExchange(&g_thread_stop, 0, 0)) {
        // Wall-clock heartbeat fallback: if WASAPI stops delivering data (e.g. render engine
        // semi-dormant after sleep/wake), send a silence packet to keep server status stable.
        // Without this, hb_counter can't advance and the server flips between idle/disconnected.
        if (GetTickCount() - last_send_tick > 800) {
            float zeros[FRAME_SAMPLES] = {};
            int len = opus_encode_float(enc, zeros, FRAME_SAMPLES, opus_buf, sizeof(opus_buf));
            int plen = build_rtp(pkt_buf, ts48k, tc->ssrc, opus_buf, len, 0);
            sendto(sock, (char*)pkt_buf, plen, 0, (sockaddr*)&srv, sizeof(srv));
            ts48k += RTP_TS_PER_FRAME;
            pcm_fill = 0; frame_has_audio = false; hb_counter = 0; was_silent = 1;
            last_send_tick = GetTickCount();
        }

        BYTE* pData; UINT32 nf; DWORD flags;
        hr = cap->GetBuffer(&pData, &nf, &flags, NULL, NULL);
        if (hr == AUDCLNT_S_BUFFER_EMPTY || nf == 0) { Sleep(5); continue; }
        if (FAILED(hr)) { logf("GetBuffer: 0x%08lX", hr); break; }

        float* src = (float*)pData;
        UINT32 offset = 0;
        while (offset < nf) {
            int need = FRAME_SAMPLES - pcm_fill;
            int take = (int)(nf - offset); if (take > need) take = need;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                memset(pcm + pcm_fill, 0, take * sizeof(float));
            else {
                memcpy(pcm + pcm_fill, src + offset, take * sizeof(float));
                frame_has_audio = true;
            }
            pcm_fill += take; offset += take;

            if (pcm_fill == FRAME_SAMPLES) {
                int len = opus_encode_float(enc, pcm, FRAME_SAMPLES, opus_buf, sizeof(opus_buf));
                // DTX leak suppression: treat WASAPI-silent frames as silent regardless of Opus output.
                // Use AUDIO_LEN_MIN (same threshold as send logic) so CN frames (len≈3) are also
                // treated as silence here, keeping the DTX guard consistent with the send path.
                BOOL frame_silent = !frame_has_audio || (len <= AUDIO_LEN_MIN);

                if (frame_silent) {
                    silent_count++; active_count = 0;
                    if (sending && silent_count >= silence_frames) {
                        sending = FALSE;
                        was_silent = 1;  // next talkspurt needs Marker bit
                        PostMessage(g_hwnd, WM_AUDIO_STATE, AS_SILENT, 0);
                        notify_post(NE_STOP);
                        logf("Silence detected -> notify stop");
                    }
                } else {
                    active_count++; silent_count = 0;
                    if (!sending && active_count >= audio_frames) {
                        sending = TRUE;
                        PostMessage(g_hwnd, WM_AUDIO_STATE, AS_ACTIVE, 0);
                        notify_post(NE_START);
                        logf("Audio resumed -> notify start");
                    }
                }

                // Send logic (len-based; AUDCLNT_BUFFERFLAGS_SILENT is unreliable on some devices):
                //   len>AUDIO_LEN_MIN → genuine audio: send immediately, reset counter.
                //   len<=AUDIO_LEN_MIN → silence or CN update: rate-limited to 1 per 19 frames.
                //   Treating CN updates (~3B) the same as silence prevents the sync artifact
                //   where the heartbeat and libopus CN cycle lock step and produce paired packets.
                bool send_frame = false;
                if (len > AUDIO_LEN_MIN) {
                    send_frame = true;
                    hb_counter = 0;
                } else {
                    hb_counter++;
                    if (hb_counter >= 19) {
                        send_frame = true;
                        hb_counter = 0;
                    }
                }
                if (send_frame) {
                    bool is_audio = (len > AUDIO_LEN_MIN);
                    // Marker set only on the first audio packet of each talkspurt (RFC 7587 §4.1).
                    // Silence/heartbeat frames never carry Marker.
                    int plen = build_rtp(pkt_buf, ts48k, tc->ssrc, opus_buf, len, is_audio && was_silent);
                    sendto(sock, (char*)pkt_buf, plen, 0, (sockaddr*)&srv, sizeof(srv));
                    // Update was_silent based on what was actually sent, not on DTX guard state.
                    was_silent = is_audio ? 0 : 1;
                    last_send_tick = GetTickCount();
                }
                ts48k += RTP_TS_PER_FRAME; pcm_fill = 0; frame_has_audio = false;
            }
        }
        cap->ReleaseBuffer(nf);
    }

    logf("Audio thread stopping");
    if (sending) notify_post(NE_STOP);   // only if a start was notified and not yet stopped
    ac->Stop();
    opus_encoder_destroy(enc);
    cap->Release(); ac->Release();
    closesocket(sock);
    if (use_ro) RoUninitialize(); else CoUninitialize();
    InterlockedExchange(&g_thread_running, 0);
    delete tc;
    return 0;
}

static void start_audio_thread(void) {
    if (InterlockedCompareExchange(&g_thread_running, 0, 0)) return;
    InterlockedExchange(&g_thread_stop, 0);

    ThreadConfig* tc = new ThreadConfig();
    WideCharToMultiByte(CP_UTF8, 0, g_cfg.server_ip, -1, tc->server_ip, 64, NULL, NULL);
    tc->server_port      = g_cfg.server_port;
    tc->ssrc             = g_ssrc;
    tc->bitrate_kbps     = g_cfg.bitrate_kbps;
    tc->silence_guard_ms = g_cfg.silence_guard_ms;
    tc->audio_guard_ms   = g_cfg.audio_guard_ms;
    tc->use_process_loopback = (g_cfg.loopback_mode == 0 && g_process_loopback_supported);

    if (g_audio_thread) { CloseHandle(g_audio_thread); g_audio_thread = NULL; }
    g_audio_thread = CreateThread(NULL, 0, audio_thread_proc, tc, 0, NULL);
}

static void stop_audio_thread(void) {
    InterlockedExchange(&g_thread_stop, 1);
    if (g_audio_thread) {
        WaitForSingleObject(g_audio_thread, 8000);
        CloseHandle(g_audio_thread); g_audio_thread = NULL;
    }
}

// ============================================================
// Property sheet pages
// ============================================================

static wchar_t s_tmp_ip[64];
static int     s_tmp_port, s_tmp_bitrate, s_tmp_silence, s_tmp_audio, s_tmp_timeout;
static int     s_tmp_loopback_mode;
static wchar_t s_tmp_batch_start[MAX_PATH], s_tmp_batch_stop[MAX_PATH];
static BOOL    s_tmp_autostart;
static int     s_tmp_notify_mode;
static wchar_t s_tmp_snmp_start[SNMP_ARGS_MAX], s_tmp_snmp_stop[SNMP_ARGS_MAX];
static UINT    s_tmp_snmp_charset;

// Restarts capture with the current g_cfg. The notification settings are swapped while
// no audio thread runs, so the final stop of the old thread still uses the old settings.
static void restart_audio(void) {
    stop_audio_thread();
    notify_set_config(notify_config_from_cfg());
    start_audio_thread();
}

static void apply_settings(void) {
    wcscpy_s(g_cfg.server_ip,    s_tmp_ip);
    g_cfg.server_port         = s_tmp_port;
    g_cfg.bitrate_kbps        = s_tmp_bitrate;
    g_cfg.silence_guard_ms    = s_tmp_silence;
    g_cfg.audio_guard_ms      = s_tmp_audio;
    wcscpy_s(g_cfg.batch_start, s_tmp_batch_start);
    wcscpy_s(g_cfg.batch_stop,  s_tmp_batch_stop);
    g_cfg.batch_timeout_ms    = s_tmp_timeout;
    g_cfg.autostart           = s_tmp_autostart;
    g_cfg.loopback_mode       = s_tmp_loopback_mode;
    g_cfg.notify_mode         = s_tmp_notify_mode;
    wcscpy_s(g_cfg.snmp_start, s_tmp_snmp_start);
    wcscpy_s(g_cfg.snmp_stop,  s_tmp_snmp_stop);
    g_cfg.snmp_charset        = s_tmp_snmp_charset;

    set_autostart(g_cfg.autostart);
    save_config();
    restart_audio();
}

static void browse_batch(HWND hdlg, int edit_id) {
    OPENFILENAMEW ofn = {};
    wchar_t file[MAX_PATH] = {};
    GetDlgItemTextW(hdlg, edit_id, file, MAX_PATH);
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hdlg;
    ofn.lpstrFilter = S->file_filter;
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn))
        SetDlgItemTextW(hdlg, edit_id, file);
}

// ---- Connection page ----
static INT_PTR CALLBACK page_connection(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetDlgItemTextW(hdlg, IDC_SERVER_IP, s_tmp_ip);
        SetDlgItemInt(hdlg, IDC_SERVER_PORT, s_tmp_port, FALSE);
        wchar_t hex[16]; swprintf_s(hex, 16, L"%08x", g_ssrc);
        SetDlgItemTextW(hdlg, IDC_SSRC_DISPLAY, hex);
        return TRUE;
    }
    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lp;
        if (hdr->code == PSN_APPLY) {
            GetDlgItemTextW(hdlg, IDC_SERVER_IP, s_tmp_ip, 64);
            s_tmp_port = (int)GetDlgItemInt(hdlg, IDC_SERVER_PORT, NULL, FALSE);
            if (s_tmp_port <= 0 || s_tmp_port > 65535) s_tmp_port = 4010;
        }
        break;
    }
    }
    return FALSE;
}

// ---- Audio page ----
static INT_PTR CALLBACK page_audio(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        HWND spin = GetDlgItem(hdlg, IDC_BITRATE_SPIN);
        SendMessage(spin, UDM_SETRANGE, 0, MAKELPARAM(512, 6));
        SetDlgItemInt(hdlg, IDC_BITRATE, s_tmp_bitrate, FALSE);

        // Set localized radio button and note text dynamically to avoid RC encoding issues
        SetDlgItemTextW(hdlg, IDC_LOOPBACK_PROCESS, S->loopback_process);
        SetDlgItemTextW(hdlg, IDC_LOOPBACK_CLASSIC, S->loopback_classic);
        SetDlgItemTextW(hdlg, IDC_LOOPBACK_NOTE, S->loopback_note);

        // Gray out Process Loopback radio on unsupported builds; force Classic
        if (!g_process_loopback_supported) {
            EnableWindow(GetDlgItem(hdlg, IDC_LOOPBACK_PROCESS), FALSE);
            s_tmp_loopback_mode = 1;
        }
        CheckRadioButton(hdlg, IDC_LOOPBACK_PROCESS, IDC_LOOPBACK_CLASSIC,
                         (s_tmp_loopback_mode == 0) ? IDC_LOOPBACK_PROCESS : IDC_LOOPBACK_CLASSIC);
        return TRUE;
    }
    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lp;
        if (hdr->code == PSN_APPLY) {
            s_tmp_bitrate = (int)GetDlgItemInt(hdlg, IDC_BITRATE, NULL, FALSE);
            if (s_tmp_bitrate < 6)   s_tmp_bitrate = 6;
            if (s_tmp_bitrate > 512) s_tmp_bitrate = 512;
            s_tmp_loopback_mode =
                (IsDlgButtonChecked(hdlg, IDC_LOOPBACK_PROCESS) == BST_CHECKED) ? 0 : 1;
        }
        break;
    }
    }
    return FALSE;
}

// ---- Detection page ----
static INT_PTR CALLBACK page_detection(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetDlgItemInt(hdlg, IDC_SILENCE_GUARD, s_tmp_silence, FALSE);
        SendMessage(GetDlgItem(hdlg, IDC_SILENCE_GUARD_SPIN), UDM_SETRANGE, 0, MAKELPARAM(5000, 0));
        SetDlgItemInt(hdlg, IDC_AUDIO_GUARD, s_tmp_audio, FALSE);
        SendMessage(GetDlgItem(hdlg, IDC_AUDIO_GUARD_SPIN), UDM_SETRANGE, 0, MAKELPARAM(5000, 0));
        return TRUE;
    }
    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lp;
        if (hdr->code == PSN_APPLY) {
            s_tmp_silence = (int)GetDlgItemInt(hdlg, IDC_SILENCE_GUARD, NULL, FALSE);
            s_tmp_audio   = (int)GetDlgItemInt(hdlg, IDC_AUDIO_GUARD,   NULL, FALSE);
        }
        break;
    }
    }
    return FALSE;
}

// ---- Notify page (batch file / SNMP trap) ----
static const int NOTIFY_BATCH_CTRLS[] = {
    IDC_BATCH_START_GROUP, IDC_BATCH_START, IDC_BATCH_START_BROWSE,
    IDC_BATCH_STOP_GROUP,  IDC_BATCH_STOP,  IDC_BATCH_STOP_BROWSE,
    IDC_BATCH_TIMEOUT_LABEL, IDC_BATCH_TIMEOUT, IDC_BATCH_TIMEOUT_SPIN, IDC_BATCH_NOTE,
};
static const int NOTIFY_SNMP_CTRLS[] = {
    IDC_SNMP_START_LABEL, IDC_SNMP_START, IDC_SNMP_TEST_START,
    IDC_SNMP_STOP_LABEL,  IDC_SNMP_STOP,  IDC_SNMP_TEST_STOP,
    IDC_SNMP_CHARSET_LABEL, IDC_SNMP_CHARSET, IDC_SNMP_HINT,
};

static void notify_page_show(HWND hdlg, int mode) {
    for (int id : NOTIFY_BATCH_CTRLS)
        ShowWindow(GetDlgItem(hdlg, id), mode == NOTIFY_BATCH ? SW_SHOW : SW_HIDE);
    for (int id : NOTIFY_SNMP_CTRLS)
        ShowWindow(GetDlgItem(hdlg, id), mode == NOTIFY_SNMP ? SW_SHOW : SW_HIDE);
}

static UINT notify_page_charset(HWND hdlg) {
    return SendDlgItemMessageW(hdlg, IDC_SNMP_CHARSET, CB_GETCURSEL, 0, 0) == 1
         ? SNMP_CHARSET_UTF8 : SNMP_CHARSET_SJIS;
}

// Parses the trap in an edit box. Shows the problem and returns FALSE if it is invalid.
static BOOL notify_page_parse(HWND hdlg, int edit_id, const wchar_t* args, const wchar_t* which,
                              SnmpTrap* t) {
    wchar_t err[512];
    if (snmp_trap_parse(args, notify_page_charset(hdlg), ui_ja(), t, err, 512)) return TRUE;
    wchar_t msg[1024];
    _snwprintf_s(msg, _countof(msg), _TRUNCATE, S->snmp_invalid_fmt, which, err);
    MessageBoxW(hdlg, msg, S->sheet_caption, MB_OK | MB_ICONWARNING);
    SetFocus(GetDlgItem(hdlg, edit_id));
    return FALSE;
}

// "Send test": sends the trap as currently typed (not yet saved).
static void notify_page_test(HWND hdlg, int edit_id, const wchar_t* which) {
    wchar_t args[SNMP_ARGS_MAX];
    GetDlgItemTextW(hdlg, edit_id, args, SNMP_ARGS_MAX);
    if (snmp_args_blank(args)) {
        MessageBoxW(hdlg, S->snmp_test_empty, S->sheet_caption, MB_OK | MB_ICONINFORMATION);
        return;
    }
    SnmpTrap t;
    if (!notify_page_parse(hdlg, edit_id, args, which, &t)) return;

    wchar_t info[256], msg[1024];
    char u[800];
    BOOL ok = snmp_trap_send(&t, ui_ja(), info, 256);
    logf("SNMP test trap (%s): %s", edit_id == IDC_SNMP_START ? "start" : "stop", utf8(info, u, sizeof(u)));
    _snwprintf_s(msg, _countof(msg), _TRUNCATE, ok ? S->snmp_test_ok_fmt : S->snmp_test_fail_fmt, info);
    MessageBoxW(hdlg, msg, S->sheet_caption, MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
}

static INT_PTR CALLBACK page_notify(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetDlgItemTextW(hdlg, IDC_NOTIFY_LABEL,        S->notify_label);
        SetDlgItemTextW(hdlg, IDC_NOTIFY_BATCH,        S->notify_batch);
        SetDlgItemTextW(hdlg, IDC_NOTIFY_SNMP,         S->notify_snmp);
        SetDlgItemTextW(hdlg, IDC_BATCH_START_GROUP,   S->on_start);
        SetDlgItemTextW(hdlg, IDC_BATCH_STOP_GROUP,    S->on_stop);
        SetDlgItemTextW(hdlg, IDC_BATCH_TIMEOUT_LABEL, S->batch_timeout);
        SetDlgItemTextW(hdlg, IDC_BATCH_NOTE,          S->batch_note);
        SetDlgItemTextW(hdlg, IDC_SNMP_START_LABEL,    S->snmp_start_label);
        SetDlgItemTextW(hdlg, IDC_SNMP_STOP_LABEL,     S->snmp_stop_label);
        SetDlgItemTextW(hdlg, IDC_SNMP_TEST_START,     S->snmp_test);
        SetDlgItemTextW(hdlg, IDC_SNMP_TEST_STOP,      S->snmp_test);
        SetDlgItemTextW(hdlg, IDC_SNMP_CHARSET_LABEL,  S->snmp_charset);
        SetDlgItemTextW(hdlg, IDC_SNMP_HINT,           S->snmp_hint);

        CheckRadioButton(hdlg, IDC_NOTIFY_BATCH, IDC_NOTIFY_SNMP,
                         s_tmp_notify_mode == NOTIFY_SNMP ? IDC_NOTIFY_SNMP : IDC_NOTIFY_BATCH);

        SetDlgItemTextW(hdlg, IDC_BATCH_START, s_tmp_batch_start);
        SetDlgItemTextW(hdlg, IDC_BATCH_STOP,  s_tmp_batch_stop);
        SetDlgItemInt(hdlg, IDC_BATCH_TIMEOUT, s_tmp_timeout, FALSE);
        SendMessage(GetDlgItem(hdlg, IDC_BATCH_TIMEOUT_SPIN), UDM_SETRANGE, 0, MAKELPARAM(60000, 0));

        // Shown with one variable binding per line; stored as one line in the INI file.
        wchar_t disp[SNMP_ARGS_MAX];
        snmp_args_canonical(s_tmp_snmp_start, TRUE, disp, SNMP_ARGS_MAX);
        SetDlgItemTextW(hdlg, IDC_SNMP_START, disp);
        snmp_args_canonical(s_tmp_snmp_stop, TRUE, disp, SNMP_ARGS_MAX);
        SetDlgItemTextW(hdlg, IDC_SNMP_STOP, disp);
        SendDlgItemMessageW(hdlg, IDC_SNMP_START, EM_LIMITTEXT, SNMP_ARGS_MAX - 1, 0);
        SendDlgItemMessageW(hdlg, IDC_SNMP_STOP,  EM_LIMITTEXT, SNMP_ARGS_MAX - 1, 0);

        SendDlgItemMessageW(hdlg, IDC_SNMP_CHARSET, CB_ADDSTRING, 0, (LPARAM)L"Shift_JIS (CP932)");
        SendDlgItemMessageW(hdlg, IDC_SNMP_CHARSET, CB_ADDSTRING, 0, (LPARAM)L"UTF-8");
        SendDlgItemMessageW(hdlg, IDC_SNMP_CHARSET, CB_SETCURSEL,
                            s_tmp_snmp_charset == SNMP_CHARSET_UTF8 ? 1 : 0, 0);

        notify_page_show(hdlg, s_tmp_notify_mode);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BATCH_START_BROWSE: browse_batch(hdlg, IDC_BATCH_START); break;
        case IDC_BATCH_STOP_BROWSE:  browse_batch(hdlg, IDC_BATCH_STOP);  break;
        case IDC_NOTIFY_BATCH:
        case IDC_NOTIFY_SNMP:
            if (HIWORD(wp) == BN_CLICKED)
                notify_page_show(hdlg, LOWORD(wp) == IDC_NOTIFY_SNMP ? NOTIFY_SNMP : NOTIFY_BATCH);
            break;
        case IDC_SNMP_TEST_START: notify_page_test(hdlg, IDC_SNMP_START, S->on_start); break;
        case IDC_SNMP_TEST_STOP:  notify_page_test(hdlg, IDC_SNMP_STOP,  S->on_stop);  break;
        }
        break;
    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lp;
        if (hdr->code == PSN_APPLY) {
            int mode = (IsDlgButtonChecked(hdlg, IDC_NOTIFY_SNMP) == BST_CHECKED) ? NOTIFY_SNMP : NOTIFY_BATCH;
            wchar_t start[SNMP_ARGS_MAX], stop[SNMP_ARGS_MAX];
            GetDlgItemTextW(hdlg, IDC_SNMP_START, start, SNMP_ARGS_MAX);
            GetDlgItemTextW(hdlg, IDC_SNMP_STOP,  stop,  SNMP_ARGS_MAX);

            // In SNMP mode, refuse to close the dialog with a trap that cannot be sent.
            if (mode == NOTIFY_SNMP) {
                SnmpTrap t;
                if ((!snmp_args_blank(start) && !notify_page_parse(hdlg, IDC_SNMP_START, start, S->on_start, &t)) ||
                    (!snmp_args_blank(stop)  && !notify_page_parse(hdlg, IDC_SNMP_STOP,  stop,  S->on_stop,  &t))) {
                    SetWindowLongPtrW(hdlg, DWLP_MSGRESULT, PSNRET_INVALID);
                    return TRUE;
                }
            }
            s_tmp_notify_mode = mode;
            GetDlgItemTextW(hdlg, IDC_BATCH_START, s_tmp_batch_start, MAX_PATH);
            GetDlgItemTextW(hdlg, IDC_BATCH_STOP,  s_tmp_batch_stop,  MAX_PATH);
            s_tmp_timeout = (int)GetDlgItemInt(hdlg, IDC_BATCH_TIMEOUT, NULL, FALSE);
            snmp_args_canonical(start, FALSE, s_tmp_snmp_start, SNMP_ARGS_MAX);
            snmp_args_canonical(stop,  FALSE, s_tmp_snmp_stop,  SNMP_ARGS_MAX);
            s_tmp_snmp_charset = notify_page_charset(hdlg);
        }
        break;
    }
    }
    return FALSE;
}

// ---- General page ----
static INT_PTR CALLBACK page_general(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG:
        CheckDlgButton(hdlg, IDC_AUTOSTART, s_tmp_autostart ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lp;
        if (hdr->code == PSN_APPLY)
            s_tmp_autostart = (IsDlgButtonChecked(hdlg, IDC_AUTOSTART) == BST_CHECKED);
        break;
    }
    }
    return FALSE;
}

static BOOL show_settings(HWND hwnd) {
    // Copy current config to scratch
    wcscpy_s(s_tmp_ip,          g_cfg.server_ip);
    s_tmp_port     = g_cfg.server_port;
    s_tmp_bitrate  = g_cfg.bitrate_kbps;
    s_tmp_silence  = g_cfg.silence_guard_ms;
    s_tmp_audio    = g_cfg.audio_guard_ms;
    wcscpy_s(s_tmp_batch_start, g_cfg.batch_start);
    wcscpy_s(s_tmp_batch_stop,  g_cfg.batch_stop);
    s_tmp_timeout  = g_cfg.batch_timeout_ms;
    s_tmp_autostart = g_cfg.autostart;
    s_tmp_loopback_mode = (g_cfg.loopback_mode == 0 && g_process_loopback_supported) ? 0 : 1;
    s_tmp_notify_mode  = g_cfg.notify_mode;
    wcscpy_s(s_tmp_snmp_start, g_cfg.snmp_start);
    wcscpy_s(s_tmp_snmp_stop,  g_cfg.snmp_stop);
    s_tmp_snmp_charset = g_cfg.snmp_charset;

    PROPSHEETPAGEW pages[5] = {};
    const wchar_t* titles[] = { S->tab_connection, S->tab_audio, S->tab_detection, S->tab_notify, S->tab_general };
    DLGPROC procs[]  = { page_connection, page_audio, page_detection, page_notify, page_general };
    int     ids[]    = { IDD_PAGE_CONNECTION, IDD_PAGE_AUDIO, IDD_PAGE_DETECTION,
                         IDD_PAGE_NOTIFY, IDD_PAGE_GENERAL };

    for (int i = 0; i < 5; i++) {
        pages[i].dwSize      = sizeof(PROPSHEETPAGEW);
        pages[i].dwFlags     = PSP_USETITLE;
        pages[i].hInstance   = GetModuleHandleW(NULL);
        pages[i].pszTemplate = MAKEINTRESOURCEW(ids[i]);
        pages[i].pfnDlgProc  = procs[i];
        pages[i].pszTitle    = titles[i];
    }

    PROPSHEETHEADERW hdr = {};
    hdr.dwSize      = sizeof(hdr);
    hdr.dwFlags     = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW | PSH_NOCONTEXTHELP;
    hdr.hwndParent  = hwnd;
    hdr.hInstance   = GetModuleHandleW(NULL);
    hdr.pszCaption  = S->sheet_caption;
    hdr.nPages      = 5;
    hdr.ppsp        = pages;

    INT_PTR result = PropertySheetW(&hdr);
    if (result > 0) apply_settings();
    return (result > 0);
}

// ============================================================
// Main window (message-only)
// ============================================================

static LRESULT CALLBACK main_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU)
            show_tray_menu(hwnd);
        else if (lp == WM_LBUTTONDBLCLK)
            PostMessage(hwnd, WM_COMMAND, IDM_SETTINGS, 0);
        return 0;

    case WM_AUDIO_STATE:
        tray_update((AudioState)wp);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_SETTINGS: show_settings(hwnd); break;
        case IDM_OPEN_LOG:
            ShellExecuteW(NULL, L"open", g_appdata_dir, NULL, NULL, SW_SHOW);
            break;
        case IDM_RELOAD:
            load_config();  // reload from INI (ignores first-run return value here)
            restart_audio();
            break;
        case IDM_EXIT:
            DestroyWindow(hwnd);   // WM_DESTROY stops capture and sends the final notification
            break;
        }
        return 0;

    case WM_DESTROY:
        tray_remove();
        stop_audio_thread();
        notify_shutdown(8000);     // let a pending stop notification go out
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================
// WinMain
// ============================================================

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // Single instance check
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"raa-client-{7E4A2B3C-1234-5678-9ABC-DEF012345678}");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex); return 0;
    }

    // Init appdata dir
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, g_appdata_dir);
    wchar_t* p = g_appdata_dir + wcslen(g_appdata_dir);
    wcscpy_s(p, MAX_PATH - (p - g_appdata_dir), L"\\raa-client");
    CreateDirectoryW(g_appdata_dir, NULL);

    swprintf_s(g_ini_path, MAX_PATH, L"%s\\raa-client.ini", g_appdata_dir);
    swprintf_s(g_log_path, MAX_PATH, L"%s\\raa-client.log", g_appdata_dir);

    log_init();
    logf("raa-client starting");

    g_os_build = get_os_build();
    g_process_loopback_supported = (g_os_build >= 19041);
    logf("OS build: %lu, ProcessLoopback: %s",
         g_os_build, g_process_loopback_supported ? "yes" : "no");

    S = (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE) ? &STR_JA : &STR_EN;

    // Generate a new SSRC; load_config() will override it if INI exists
    UUID u; UuidCreate(&u);
    g_ssrc = u.Data1 ? u.Data1 : 0xA0B0C0D0u;

    BOOL config_exists = load_config();
    logf("SSRC: %08x", g_ssrc);

    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);

    notify_init();
    notify_set_config(notify_config_from_cfg());

    // Common controls
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_WIN95_CLASSES | ICC_UPDOWN_CLASS;
    InitCommonControlsEx(&icc);

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize      = sizeof(wc);
    wc.lpfnWndProc = main_wndproc;
    wc.hInstance   = hInst;
    wc.lpszClassName = L"raa-client-wnd";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, L"raa-client-wnd", L"RAA Client",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1, 1,
                              NULL, NULL, hInst, NULL);
    // Keep window hidden (message-only)

    tray_init(g_hwnd);
    tray_update(AS_SILENT);

    if (!config_exists) {
        // First run: show settings so user can enter server IP.
        // apply_settings() (called on OK) saves INI and starts audio thread.
        logf("First run: opening settings dialog");
        if (!show_settings(g_hwnd)) {
            logf("First run cancelled, exiting");
            DestroyWindow(g_hwnd);  // triggers WM_DESTROY -> PostQuitMessage
        }
    } else {
        start_audio_thread();
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WSACleanup();
    logf("raa-client exiting");
    log_close();
    CloseHandle(mutex);
    return 0;
}
