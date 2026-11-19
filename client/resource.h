#pragma once

// Icons
#define IDI_APP         100   // Application icon (Explorer, window title bar)
#define IDI_TRAY_ACTIVE 101   // Tray: audio sending
#define IDI_TRAY_SILENT 102   // Tray: silence
#define IDI_TRAY_ERROR  103   // Tray: error

// Window / tray messages
#define WM_TRAY                 (WM_USER + 1)
#define WM_AUDIO_STATE          (WM_USER + 2)   // wParam = AudioState enum

// Tray menu items
#define IDM_STATUS              1001
#define IDM_SETTINGS            1002
#define IDM_OPEN_LOG            1003
#define IDM_RELOAD              1004
#define IDM_EXIT                1005

// Property sheet pages (IDD_PAGE_*)
#define IDD_PAGE_CONNECTION     2001
#define IDD_PAGE_AUDIO          2002
#define IDD_PAGE_DETECTION      2003
#define IDD_PAGE_NOTIFY         2004
#define IDD_PAGE_GENERAL        2005

// Connection page controls
#define IDC_SERVER_IP           3001
#define IDC_SERVER_PORT         3002
#define IDC_SSRC_DISPLAY        3003

// Audio page controls
#define IDC_BITRATE             3010
#define IDC_BITRATE_SPIN        3011
#define IDC_LOOPBACK_PROCESS    3012   // Radio: Process Loopback (build >= 19041)
#define IDC_LOOPBACK_CLASSIC    3013   // Radio: Classic Loopback (master-volume dependent)
#define IDC_LOOPBACK_NOTE       3014   // Static text: build requirement note

// Detection page controls
#define IDC_SILENCE_GUARD       3022
#define IDC_SILENCE_GUARD_SPIN  3023
#define IDC_AUDIO_GUARD         3024
#define IDC_AUDIO_GUARD_SPIN    3025

// Notify page controls (labels are set at runtime for localization)
#define IDC_NOTIFY_LABEL        3026
#define IDC_NOTIFY_BATCH        3027   // Radio: run batch files
#define IDC_NOTIFY_SNMP         3028   // Radio: send SNMP traps directly
// - batch mode
#define IDC_BATCH_START         3030
#define IDC_BATCH_START_BROWSE  3031
#define IDC_BATCH_STOP          3032
#define IDC_BATCH_STOP_BROWSE   3033
#define IDC_BATCH_TIMEOUT       3034
#define IDC_BATCH_TIMEOUT_SPIN  3035
#define IDC_BATCH_START_GROUP   3036
#define IDC_BATCH_STOP_GROUP    3037
#define IDC_BATCH_TIMEOUT_LABEL 3038
#define IDC_BATCH_NOTE          3039
// - SNMP trap mode
#define IDC_SNMP_START_LABEL    3050
#define IDC_SNMP_START          3051
#define IDC_SNMP_TEST_START     3052
#define IDC_SNMP_STOP_LABEL     3053
#define IDC_SNMP_STOP           3054
#define IDC_SNMP_TEST_STOP      3055
#define IDC_SNMP_CHARSET_LABEL  3056
#define IDC_SNMP_CHARSET        3057
#define IDC_SNMP_HINT           3058

// General page controls
#define IDC_AUTOSTART           3040
