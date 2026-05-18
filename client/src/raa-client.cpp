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
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "opus.h"
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

#define RTP_PT_OPUS 111
#define WM_TRAY     (WM_USER + 1)
#define WM_AUDIO_STATE (WM_USER + 2)  // wParam = AudioState

#define IDM_STATUS  1001
#define IDM_SETTINGS 1002
#define IDM_OPEN_LOG 1003
#define IDM_RELOAD  1004
#define IDM_EXIT    1005

#define IDD_PAGE_CONNECTION 2001
#define IDD_PAGE_AUDIO      2002
#define IDD_PAGE_DETECTION  2003
#define IDD_PAGE_BATCH      2004
#define IDD_PAGE_GENERAL    2005

#define IDC_SERVER_IP           3001
#define IDC_SERVER_PORT         3002
#define IDC_SSRC_DISPLAY        3003
#define IDC_BITRATE             3010
#define IDC_BITRATE_SPIN        3011
#define IDC_DTX_THRESHOLD       3020
#define IDC_DTX_THRESHOLD_SPIN  3021
#define IDC_ENTER_GUARD         3022
#define IDC_ENTER_GUARD_SPIN    3023
#define IDC_EXIT_GUARD          3024
#define IDC_EXIT_GUARD_SPIN     3025
#define IDC_BATCH_START         3030
#define IDC_BATCH_START_BROWSE  3031
#define IDC_BATCH_STOP          3032
#define IDC_BATCH_STOP_BROWSE   3033
#define IDC_BATCH_TIMEOUT       3034
#define IDC_BATCH_TIMEOUT_SPIN  3035
#define IDC_AUTOSTART           3040

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
};

static Config g_cfg = {
    L"192.168.11.38", 4010,
    64,
    200, 100,
    L"", L"", 5000,
    FALSE
};

static uint32_t g_ssrc = 0;
static HWND     g_hwnd = NULL;

// ============================================================
// SSRC persistence
// ============================================================

static wchar_t g_appdata_dir[MAX_PATH];
static wchar_t g_ini_path[MAX_PATH];
static wchar_t g_log_path[MAX_PATH];

// ============================================================
// Config INI  (Windows WritePrivateProfileString / GetPrivateProfileInt)
// ============================================================

static void save_config(void) {
    wchar_t buf[32];

    WritePrivateProfileStringW(L"Connection", L"server_ip",   g_cfg.server_ip,  g_ini_path);
    swprintf_s(buf, 32, L"%d", g_cfg.server_port);
    WritePrivateProfileStringW(L"Connection", L"server_port", buf,              g_ini_path);
    swprintf_s(buf, 32, L"%08x", g_ssrc);
    WritePrivateProfileStringW(L"Connection", L"ssrc",        buf,              g_ini_path);

    swprintf_s(buf, 32, L"%d", g_cfg.bitrate_kbps);
    WritePrivateProfileStringW(L"Audio", L"bitrate_kbps",     buf,              g_ini_path);

    swprintf_s(buf, 32, L"%d", g_cfg.silence_guard_ms);
    WritePrivateProfileStringW(L"Detection", L"silence_guard_ms", buf,          g_ini_path);
    swprintf_s(buf, 32, L"%d", g_cfg.audio_guard_ms);
    WritePrivateProfileStringW(L"Detection", L"audio_guard_ms",   buf,          g_ini_path);

    WritePrivateProfileStringW(L"Batch", L"start_script",     g_cfg.batch_start, g_ini_path);
    WritePrivateProfileStringW(L"Batch", L"stop_script",      g_cfg.batch_stop,  g_ini_path);
    swprintf_s(buf, 32, L"%d", g_cfg.batch_timeout_ms);
    WritePrivateProfileStringW(L"Batch", L"timeout_ms",       buf,              g_ini_path);

    WritePrivateProfileStringW(L"General", L"autostart", g_cfg.autostart ? L"1" : L"0", g_ini_path);
}

// Returns TRUE if the INI file existed and was loaded (FALSE = first run)
static BOOL load_config(void) {
    if (GetFileAttributesW(g_ini_path) == INVALID_FILE_ATTRIBUTES)
        return FALSE;

    GetPrivateProfileStringW(L"Connection", L"server_ip", L"192.168.11.38",
                             g_cfg.server_ip, 64, g_ini_path);
    g_cfg.server_port = (int)GetPrivateProfileIntW(L"Connection", L"server_port", 4010, g_ini_path);

    wchar_t ssrc_hex[16] = {};
    GetPrivateProfileStringW(L"Connection", L"ssrc", L"", ssrc_hex, 16, g_ini_path);
    if (ssrc_hex[0]) g_ssrc = wcstoul(ssrc_hex, NULL, 16);

    g_cfg.bitrate_kbps     = (int)GetPrivateProfileIntW(L"Audio", L"bitrate_kbps", 64, g_ini_path);

    g_cfg.silence_guard_ms = (int)GetPrivateProfileIntW(L"Detection", L"silence_guard_ms", 200, g_ini_path);
    g_cfg.audio_guard_ms   = (int)GetPrivateProfileIntW(L"Detection", L"audio_guard_ms",   100, g_ini_path);

    GetPrivateProfileStringW(L"Batch", L"start_script", L"", g_cfg.batch_start, MAX_PATH, g_ini_path);
    GetPrivateProfileStringW(L"Batch", L"stop_script",  L"", g_cfg.batch_stop,  MAX_PATH, g_ini_path);
    g_cfg.batch_timeout_ms = (int)GetPrivateProfileIntW(L"Batch", L"timeout_ms", 5000, g_ini_path);

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

static void logf(const char* fmt, ...) {
    if (!g_log_file) return;
    EnterCriticalSection(&g_log_cs);
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
    const wchar_t* tip = (state == AS_ACTIVE) ? L"RAA Client - 送信中"
                       : (state == AS_ERROR)  ? L"RAA Client - エラー"
                                              : L"RAA Client - 無音";
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
    const wchar_t* s = (g_audio_state == AS_ACTIVE) ? L"送信中"
                     : (g_audio_state == AS_ERROR)  ? L"エラー" : L"無音";
    swprintf_s(status, 64, L"状態: %s  (SSRC: %08x)", s, g_ssrc);
    AppendMenuW(hm, MF_STRING | MF_DISABLED | MF_GRAYED, IDM_STATUS, status);
    AppendMenuW(hm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hm, MF_STRING, IDM_SETTINGS, L"設定...");
    AppendMenuW(hm, MF_STRING, IDM_OPEN_LOG, L"ログフォルダを開く");
    AppendMenuW(hm, MF_STRING, IDM_RELOAD,   L"設定ファイルを再読み込み");
    AppendMenuW(hm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hm, MF_STRING, IDM_EXIT, L"終了");

    SetForegroundWindow(hwnd);
    POINT pt; GetCursorPos(&pt);
    TrackPopupMenu(hm, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hm);
}

// ============================================================
// Batch script execution
// ============================================================

static void run_batch(const wchar_t* path, int timeout_ms) {
    if (!path || path[0] == L'\0') return;
    wchar_t cmd[MAX_PATH + 16];
    swprintf_s(cmd, MAX_PATH + 16, L"\"%s\"", path);
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, timeout_ms > 0 ? timeout_ms : INFINITE);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
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
    wchar_t batch_start[MAX_PATH];
    wchar_t batch_stop[MAX_PATH];
    int     batch_timeout_ms;
};

static DWORD WINAPI audio_thread_proc(LPVOID param) {
    ThreadConfig* tc = (ThreadConfig*)param;
    InterlockedExchange(&g_thread_running, 1);

    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr)) {
        logf("RoInitialize failed: 0x%08lX", hr);
        PostMessage(g_hwnd, WM_AUDIO_STATE, AS_ERROR, 0);
        InterlockedExchange(&g_thread_running, 0);
        delete tc; return 1;
    }

    // UDP socket
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        logf("socket() failed"); PostMessage(g_hwnd, WM_AUDIO_STATE, AS_ERROR, 0);
        RoUninitialize(); InterlockedExchange(&g_thread_running, 0); delete tc; return 1;
    }
    sockaddr_in srv = {};
    srv.sin_family = AF_INET; srv.sin_port = htons((u_short)tc->server_port);
    inet_pton(AF_INET, tc->server_ip, &srv.sin_addr);

    // WASAPI activation
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
    IAudioClient* ac = handler->m_client; handler->m_client = NULL;
    asyncOp->Release(); handler->Release();

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

    hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                        AUDCLNT_STREAMFLAGS_LOOPBACK |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                        1000000, 0, (WAVEFORMATEX*)&wfex, NULL);
    if (FAILED(hr)) {
        logf("Initialize failed: 0x%08lX", hr);
        PostMessage(g_hwnd, WM_AUDIO_STATE, AS_ERROR, 0);
        ac->Release(); closesocket(sock); RoUninitialize();
        InterlockedExchange(&g_thread_running, 0); delete tc; return 1;
    }

    IAudioCaptureClient* cap = NULL;
    ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cap);

    int opus_err;
    OpusEncoder* enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_AUDIO, &opus_err);
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(tc->bitrate_kbps * 1000));
    opus_encoder_ctl(enc, OPUS_SET_DTX(1));

    ac->Start();
    logf("Audio started -> %s:%d  SSRC=%08x", tc->server_ip, tc->server_port, tc->ssrc);

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

    // DTX guard state (ms-based; frames are 20ms each)
    int silence_frames = tc->silence_guard_ms / 20;  // silent frames needed to trigger batch_stop
    int audio_frames   = tc->audio_guard_ms   / 20;  // active frames needed to trigger batch_start
    int silent_count = 0, active_count = 0;
    BOOL sending = TRUE;  // start in active mode, batch_start fires immediately
    run_batch(tc->batch_start, tc->batch_timeout_ms);

    BYTE opus_buf[4000];
    BYTE pkt_buf[12 + 4000];

    while (!InterlockedCompareExchange(&g_thread_stop, 0, 0)) {
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
                // DTX leak suppression: treat WASAPI-silent frames as silent regardless of Opus output
                BOOL frame_silent = !frame_has_audio || (len <= 2);

                if (frame_silent) {
                    silent_count++; active_count = 0;
                    if (sending && silent_count >= silence_frames) {
                        sending = FALSE;
                        was_silent = 1;  // next talkspurt needs Marker bit
                        PostMessage(g_hwnd, WM_AUDIO_STATE, AS_SILENT, 0);
                        run_batch(tc->batch_stop, tc->batch_timeout_ms);
                        logf("Silence detected -> batch_stop");
                    }
                } else {
                    active_count++; silent_count = 0;
                    if (!sending && active_count >= audio_frames) {
                        sending = TRUE;
                        PostMessage(g_hwnd, WM_AUDIO_STATE, AS_ACTIVE, 0);
                        run_batch(tc->batch_start, tc->batch_timeout_ms);
                        logf("Audio resumed -> batch_start");
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
                }
                ts48k += RTP_TS_PER_FRAME; pcm_fill = 0; frame_has_audio = false;
            }
        }
        cap->ReleaseBuffer(nf);
    }

    logf("Audio thread stopping");
    run_batch(tc->batch_stop, tc->batch_timeout_ms);
    ac->Stop();
    opus_encoder_destroy(enc);
    cap->Release(); ac->Release();
    closesocket(sock);
    RoUninitialize();
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
    wcscpy_s(tc->batch_start, g_cfg.batch_start);
    wcscpy_s(tc->batch_stop,  g_cfg.batch_stop);
    tc->batch_timeout_ms = g_cfg.batch_timeout_ms;

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
static wchar_t s_tmp_batch_start[MAX_PATH], s_tmp_batch_stop[MAX_PATH];
static BOOL    s_tmp_autostart;

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

    set_autostart(g_cfg.autostart);
    save_config();
    stop_audio_thread();
    start_audio_thread();
}

static void browse_batch(HWND hdlg, int edit_id) {
    OPENFILENAMEW ofn = {};
    wchar_t file[MAX_PATH] = {};
    GetDlgItemTextW(hdlg, edit_id, file, MAX_PATH);
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hdlg;
    ofn.lpstrFilter = L"スクリプト (*.bat;*.cmd;*.exe;*.ps1)\0*.bat;*.cmd;*.exe;*.ps1\0すべて (*.*)\0*.*\0";
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
        return TRUE;
    }
    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lp;
        if (hdr->code == PSN_APPLY) {
            s_tmp_bitrate = (int)GetDlgItemInt(hdlg, IDC_BITRATE, NULL, FALSE);
            if (s_tmp_bitrate < 6)   s_tmp_bitrate = 6;
            if (s_tmp_bitrate > 512) s_tmp_bitrate = 512;
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

// ---- Batch page ----
static INT_PTR CALLBACK page_batch(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetDlgItemTextW(hdlg, IDC_BATCH_START, s_tmp_batch_start);
        SetDlgItemTextW(hdlg, IDC_BATCH_STOP,  s_tmp_batch_stop);
        SetDlgItemInt(hdlg, IDC_BATCH_TIMEOUT, s_tmp_timeout, FALSE);
        SendMessage(GetDlgItem(hdlg, IDC_BATCH_TIMEOUT_SPIN), UDM_SETRANGE, 0, MAKELPARAM(60000, 0));
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_BATCH_START_BROWSE) browse_batch(hdlg, IDC_BATCH_START);
        if (LOWORD(wp) == IDC_BATCH_STOP_BROWSE)  browse_batch(hdlg, IDC_BATCH_STOP);
        break;
    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lp;
        if (hdr->code == PSN_APPLY) {
            GetDlgItemTextW(hdlg, IDC_BATCH_START, s_tmp_batch_start, MAX_PATH);
            GetDlgItemTextW(hdlg, IDC_BATCH_STOP,  s_tmp_batch_stop,  MAX_PATH);
            s_tmp_timeout = (int)GetDlgItemInt(hdlg, IDC_BATCH_TIMEOUT, NULL, FALSE);
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

    PROPSHEETPAGEW pages[5] = {};
    const wchar_t* titles[] = { L"接続", L"オーディオ", L"無音検知", L"バッチ", L"全般" };
    DLGPROC procs[]  = { page_connection, page_audio, page_detection, page_batch, page_general };
    int     ids[]    = { IDD_PAGE_CONNECTION, IDD_PAGE_AUDIO, IDD_PAGE_DETECTION,
                         IDD_PAGE_BATCH, IDD_PAGE_GENERAL };

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
    hdr.pszCaption  = L"RAA Client 設定";
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
            stop_audio_thread();
            start_audio_thread();
            break;
        case IDM_EXIT:
            PostQuitMessage(0);
            break;
        }
        return 0;

    case WM_DESTROY:
        tray_remove();
        stop_audio_thread();
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

    // Generate a new SSRC; load_config() will override it if INI exists
    UUID u; UuidCreate(&u);
    g_ssrc = u.Data1 ? u.Data1 : 0xA0B0C0D0u;

    BOOL config_exists = load_config();
    logf("SSRC: %08x", g_ssrc);

    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);

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
    if (g_log_file) fclose(g_log_file);
    CloseHandle(mutex);
    return 0;
}
