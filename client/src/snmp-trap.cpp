// snmp-trap.cpp - SNMPv1 / SNMPv2c trap sender (see snmp-trap.h)
//
// Message layout: RFC 1157 (v1 Trap-PDU) and RFC 3416 (SNMPv2-Trap-PDU), BER per X.690.
// Messages are encoded back to front, so every length is known when its header is written.

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
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "snmp-trap.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

#define OID_ARCS_MAX 128   // RFC 2578 §3.5: at most 128 sub-identifiers

#define TAG_INTEGER   0x02
#define TAG_OCTETS    0x04
#define TAG_NULL      0x05
#define TAG_OID       0x06
#define TAG_SEQUENCE  0x30
#define TAG_IPADDRESS 0x40
#define TAG_COUNTER32 0x41
#define TAG_GAUGE32   0x42
#define TAG_TIMETICKS 0x43
#define TAG_COUNTER64 0x46
#define TAG_TRAP_V1   0xA4
#define TAG_TRAP_V2   0xA7

// Writes a message in the UI language. Both formats must take the same arguments.
static void say(wchar_t* out, int cap, BOOL ja, const wchar_t* fmt_ja, const wchar_t* fmt_en, ...) {
    if (!out || cap <= 0) return;
    va_list ap;
    va_start(ap, fmt_en);
    _vsnwprintf_s(out, cap, _TRUNCATE, ja ? fmt_ja : fmt_en, ap);
    va_end(ap);
}

// ============================================================
// BER writer (back to front)
// ============================================================

struct Ber {
    BYTE* buf;
    int   cap;
    int   pos;        // encoded data is buf[pos..cap)
    bool  overflow;
};

static void ber_init(Ber* b, BYTE* buf, int cap) {
    b->buf = buf; b->cap = cap; b->pos = cap; b->overflow = false;
}

static int ber_size(const Ber* b) { return b->cap - b->pos; }

static void ber_byte(Ber* b, BYTE v) {
    if (b->pos <= 0) { b->overflow = true; return; }
    b->buf[--b->pos] = v;
}

static void ber_bytes(Ber* b, const BYTE* p, int n) {
    if (n > b->pos) { b->overflow = true; return; }
    b->pos -= n;
    if (n > 0) memcpy(b->buf + b->pos, p, n);
}

// Puts tag and length in front of everything written since mark (an earlier ber_size()).
static void ber_wrap(Ber* b, BYTE tag, int mark) {
    int len = ber_size(b) - mark;
    if (len < 0x80) {
        ber_byte(b, (BYTE)len);
    } else {
        int n = 0;
        for (int v = len; v > 0; v >>= 8) { ber_byte(b, (BYTE)v); n++; }
        ber_byte(b, (BYTE)(0x80 | n));
    }
    ber_byte(b, tag);
}

// Two's complement, minimum number of bytes.
static void ber_int(Ber* b, BYTE tag, int64_t v) {
    int mark = ber_size(b);
    for (;;) {
        BYTE lo = (BYTE)(v & 0xFF);
        ber_byte(b, lo);
        v >>= 8;
        if ((v == 0 && !(lo & 0x80)) || (v == -1 && (lo & 0x80))) break;
    }
    ber_wrap(b, tag, mark);
}

// Unsigned types (Counter32, Gauge32, TimeTicks, Counter64) are INTEGER content too:
// a leading 0x00 keeps values with the top bit set positive.
static void ber_uint(Ber* b, BYTE tag, uint64_t v) {
    int mark = ber_size(b);
    BYTE lo;
    do { lo = (BYTE)(v & 0xFF); ber_byte(b, lo); v >>= 8; } while (v);
    if (lo & 0x80) ber_byte(b, 0);
    ber_wrap(b, tag, mark);
}

static void ber_subid(Ber* b, uint64_t v) {
    ber_byte(b, (BYTE)(v & 0x7F));
    for (v >>= 7; v; v >>= 7) ber_byte(b, (BYTE)(0x80 | (v & 0x7F)));
}

// arcs must have passed parse_oid (n >= 2, valid first two arcs).
static void ber_oid(Ber* b, const uint32_t* arcs, int n) {
    int mark = ber_size(b);
    for (int i = n - 1; i >= 2; i--) ber_subid(b, arcs[i]);
    ber_subid(b, (uint64_t)arcs[0] * 40 + arcs[1]);
    ber_wrap(b, TAG_OID, mark);
}

static void ber_octets(Ber* b, BYTE tag, const BYTE* p, int n) {
    int mark = ber_size(b);
    ber_bytes(b, p, n);
    ber_wrap(b, tag, mark);
}

// ============================================================
// Value parsing
// ============================================================

static BOOL is_digit(wchar_t c) { return c >= L'0' && c <= L'9'; }

static BOOL parse_int(const wchar_t* s, int64_t lo, int64_t hi, int64_t* out) {
    const wchar_t* p = (*s == L'-') ? s + 1 : s;
    if (!is_digit(*p)) return FALSE;
    wchar_t* end = NULL;
    errno = 0;
    long long v = wcstoll(s, &end, 10);
    if (errno == ERANGE || *end || v < lo || v > hi) return FALSE;
    *out = v;
    return TRUE;
}

static BOOL parse_uint(const wchar_t* s, uint64_t hi, uint64_t* out) {
    if (!is_digit(*s)) return FALSE;
    wchar_t* end = NULL;
    errno = 0;
    unsigned long long v = wcstoull(s, &end, 10);
    if (errno == ERANGE || *end || v > hi) return FALSE;
    *out = v;
    return TRUE;
}

// Numeric OID such as .1.3.6.1.4.1.119 (leading dot optional). MIB names are not supported.
static BOOL parse_oid(const wchar_t* s, uint32_t* arcs, int* n) {
    const wchar_t* p = (*s == L'.') ? s + 1 : s;
    int k = 0;
    for (;;) {
        if (!is_digit(*p)) return FALSE;
        uint64_t v = 0;
        while (is_digit(*p)) {
            v = v * 10 + (uint64_t)(*p - L'0');
            if (v > 0xFFFFFFFFull) return FALSE;
            p++;
        }
        if (k >= OID_ARCS_MAX) return FALSE;
        arcs[k++] = (uint32_t)v;
        if (*p == 0) break;
        if (*p != L'.') return FALSE;
        p++;
    }
    if (k < 2 || arcs[0] > 2 || (arcs[0] < 2 && arcs[1] >= 40)) return FALSE;
    *n = k;
    return TRUE;
}

static BOOL encode_oid_tlv(const wchar_t* s, BYTE* out, int* len) {
    uint32_t arcs[OID_ARCS_MAX];
    int n;
    if (!parse_oid(s, arcs, &n)) return FALSE;
    BYTE tmp[SNMP_OID_TLV_MAX];
    Ber b;
    ber_init(&b, tmp, sizeof(tmp));
    ber_oid(&b, arcs, n);
    if (b.overflow) return FALSE;
    memcpy(out, tmp + b.pos, ber_size(&b));
    *len = ber_size(&b);
    return TRUE;
}

// Returns the byte count, -1 if a character cannot be represented, -2 if out is too small.
static int to_charset(const wchar_t* s, UINT charset, BYTE* out, int cap) {
    int n = (int)wcslen(s);
    if (n == 0) return 0;
    BOOL lossy = FALSE;
    int r = (charset == CP_UTF8)
        ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, n, (char*)out, cap, NULL, NULL)
        : WideCharToMultiByte(charset, WC_NO_BEST_FIT_CHARS, s, n, (char*)out, cap, NULL, &lossy);
    if (r <= 0) return (GetLastError() == ERROR_INSUFFICIENT_BUFFER) ? -2 : -1;
    return lossy ? -1 : r;
}

static int hex_digit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

// "0a 1B ff", "0a1bff" or "0x0a1bff". Returns the byte count, -1 if malformed, -2 if too long.
static int parse_hex(const wchar_t* s, BYTE* out, int cap) {
    const wchar_t* p = s;
    if (p[0] == L'0' && (p[1] == L'x' || p[1] == L'X')) p += 2;
    int n = 0;
    for (;;) {
        while (*p == L' ') p++;
        if (!*p) break;
        int hi = hex_digit(p[0]);
        int lo = (hi >= 0) ? hex_digit(p[1]) : -1;
        if (lo < 0) return -1;
        if (n >= cap) return -2;
        out[n++] = (BYTE)(hi * 16 + lo);
        p += 2;
    }
    return n;
}

// Bytes in decimal separated by '.' or ' ' ("192.168.0.1"). Same return values as parse_hex.
static int parse_dec_bytes(const wchar_t* s, BYTE* out, int cap) {
    const wchar_t* p = s;
    int n = 0;
    while (*p) {
        if (!is_digit(*p)) return -1;
        int v = 0;
        while (is_digit(*p)) {
            v = v * 10 + (*p - L'0');
            if (v > 255) return -1;
            p++;
        }
        if (n >= cap) return -2;
        out[n++] = (BYTE)v;
        if (*p == L'.' || *p == L' ') {
            p++;
            if (!*p) return -1;
        } else if (*p) {
            return -1;
        }
    }
    return n;
}

static const wchar_t* charset_name(UINT charset) {
    return (charset == CP_UTF8) ? L"UTF-8" : L"Shift_JIS";
}

// ============================================================
// Tokenizing (same rules as snmptrap.exe's C runtime)
// ============================================================

static BOOL is_comment_line(const wchar_t* p) {   // p = first non-blank character of a line
    if (*p == L'@') p++;
    if (p[0] == L':' && p[1] == L':') return TRUE;
    return _wcsnicmp(p, L"rem", 3) == 0 &&
           (p[3] == 0 || p[3] == L' ' || p[3] == L'\t' || p[3] == L'\r' || p[3] == L'\n');
}

// Joins the lines of text pasted from a batch file into one command line: rem / :: lines
// are dropped, ^ line continuations and line breaks become spaces. Never grows the text.
static void flatten(const wchar_t* in, wchar_t* out, int cap) {
    int o = 0;
    BOOL line_start = TRUE;
    const wchar_t* p = in;
    while (*p && o < cap - 1) {
        if (line_start) {
            const wchar_t* q = p;
            while (*q == L' ' || *q == L'\t') q++;
            if (is_comment_line(q)) {
                while (*q && *q != L'\n') q++;
                p = q;
                if (!*p) break;
            }
            line_start = FALSE;
        }
        wchar_t c = *p;
        if (c == L'^') {
            const wchar_t* q = p + 1;
            while (*q == L' ' || *q == L'\t') q++;
            if (*q == L'\r' || *q == L'\n' || *q == 0) {   // continuation: drop the caret
                out[o++] = L' ';
                p = q;
                continue;
            }
        }
        if (c == L'\n') line_start = TRUE;
        out[o++] = (c == L'\r' || c == L'\n' || c == L'\t') ? L' ' : c;
        p++;
    }
    out[o] = 0;
}

static BOOL is_snmptrap_program(const wchar_t* tok) {
    const wchar_t* name = tok;
    for (const wchar_t* p = tok; *p; p++)
        if (*p == L'\\' || *p == L'/') name = p + 1;
    return _wcsicmp(name, L"snmptrap") == 0 || _wcsicmp(name, L"snmptrap.exe") == 0;
}

struct Tokens {
    LPWSTR* argv;    // LocalFree when done
    int     argc;
    int     first;   // argv[first..argc) are the snmptrap arguments
};

static BOOL tokenize(const wchar_t* args, Tokens* tk) {
    // CommandLineToArgvW parses argv[0] (the program name) with different rules,
    // so put a dummy program name in front.
    wchar_t buf[SNMP_ARGS_MAX + 4];
    buf[0] = L'x';
    buf[1] = L' ';
    flatten(args, buf + 2, SNMP_ARGS_MAX + 2);
    tk->argv = CommandLineToArgvW(buf, &tk->argc);
    if (!tk->argv) return FALSE;
    // Skip whatever precedes the snmptrap program (@echo off, cd C:\tools\, ...).
    // Only tokens before the first option are looked at, so a value can never match.
    tk->first = 1;
    for (int i = 1; i < tk->argc && tk->argv[i][0] != L'-'; i++)
        if (is_snmptrap_program(tk->argv[i])) tk->first = i + 1;
    return TRUE;
}

struct Header {
    int            version;     // 0 = v1, 1 = v2c
    const wchar_t* community;
    int            host;        // index of the destination argument
    int            fields;      // index of the first version-specific argument
    int            varbinds;    // index of the first variable binding
};

static BOOL scan_header(const Tokens* tk, BOOL ja, Header* h, wchar_t* err, int errcap) {
    h->version = -1;
    h->community = NULL;
    int i = tk->first;
    while (i < tk->argc && tk->argv[i][0] == L'-') {
        const wchar_t* a = tk->argv[i];
        wchar_t opt = a[1];
        if (opt != L'v' && opt != L'c') {
            say(err, errcap, ja, L"対応していないオプションです: %ls（使えるのは -v と -c です）",
                L"Unsupported option: %ls (only -v and -c are supported)", a);
            return FALSE;
        }
        const wchar_t* val;
        if (a[2]) {                       // -v1, -cpublic
            val = a + 2;
            i++;
        } else {
            if (i + 1 >= tk->argc) {
                say(err, errcap, ja, L"%ls の値がありません。", L"%ls needs a value.", a);
                return FALSE;
            }
            val = tk->argv[i + 1];
            i += 2;
        }
        if (opt == L'v') {
            if (wcscmp(val, L"1") == 0)          h->version = 0;
            else if (_wcsicmp(val, L"2c") == 0)  h->version = 1;
            else {
                say(err, errcap, ja, L"対応していない SNMP バージョンです: %ls（1 または 2c）",
                    L"Unsupported SNMP version: %ls (use 1 or 2c)", val);
                return FALSE;
            }
        } else {
            h->community = val;
        }
    }
    if (h->version < 0) {
        say(err, errcap, ja, L"SNMP バージョンを -v 1 または -v 2c で指定してください。",
            L"Specify the SNMP version with -v 1 or -v 2c.");
        return FALSE;
    }
    if (!h->community) {
        say(err, errcap, ja, L"コミュニティ名を -c で指定してください。",
            L"Specify the community with -c.");
        return FALSE;
    }
    if (i >= tk->argc) {
        say(err, errcap, ja, L"送信先（Trap を受けるマネージャー）のアドレスがありません。",
            L"The destination (trap receiver) address is missing.");
        return FALSE;
    }
    h->host = i++;
    h->fields = i;
    int nfields = (h->version == 0) ? 5 : 2;
    if (tk->argc - i < nfields) {
        if (h->version == 0)
            say(err, errcap, ja,
                L"SNMPv1 では送信先のあとに enterprise-oid agent generic-trap specific-trap uptime の 5 つが必要です（省略するものは \"\"）。",
                L"SNMPv1 needs enterprise-oid agent generic-trap specific-trap uptime after the destination (\"\" for defaults).");
        else
            say(err, errcap, ja,
                L"SNMPv2c では送信先のあとに uptime trap-oid の 2 つが必要です（uptime を省略するときは \"\"）。",
                L"SNMPv2c needs uptime trap-oid after the destination (\"\" for the default uptime).");
        return FALSE;
    }
    h->varbinds = i + nfields;
    return TRUE;
}

// ============================================================
// Parsing
// ============================================================

// [udp:]host[:port]   (IPv4 only)
static BOOL parse_host(const wchar_t* spec, SnmpTrap* t, BOOL ja, wchar_t* err, int errcap) {
    const wchar_t* s = spec;
    if (_wcsnicmp(s, L"udp:", 4) == 0) s += 4;
    const wchar_t* colon = wcschr(s, L':');
    size_t hlen = colon ? (size_t)(colon - s) : wcslen(s);
    uint64_t port = 162;
    BOOL ok = hlen > 0 && hlen < _countof(t->host) &&
              (!colon || (!wcschr(colon + 1, L':') && parse_uint(colon + 1, 65535, &port) && port > 0));
    if (!ok) {
        say(err, errcap, ja,
            L"送信先の指定が正しくありません: %ls（ホスト名か IPv4 アドレス。ポートを変えるときは :ポート番号）",
            L"Invalid destination: %ls (host name or IPv4 address, optionally followed by :port)", spec);
        return FALSE;
    }
    wmemcpy(t->host, s, hlen);
    t->host[hlen] = 0;
    _snwprintf_s(t->port, _countof(t->port), _TRUNCATE, L"%u", (unsigned)port);
    return TRUE;
}

static BOOL encode_varbind(Ber* b, int index, const wchar_t* oid_s, const wchar_t* type,
                           const wchar_t* val, UINT charset, BOOL ja, wchar_t* err, int errcap) {
    uint32_t arcs[OID_ARCS_MAX];
    int n;
    if (!parse_oid(oid_s, arcs, &n)) {
        say(err, errcap, ja, L"%d 番目の変数の OID が正しくありません: %ls（数値の OID のみ。MIB 名は使えません）",
            L"Variable %d: invalid OID: %ls (numeric OIDs only, MIB names are not supported)", index, oid_s);
        return FALSE;
    }
    if (!type[0] || type[1] || !wcschr(L"iucCtaosxdn", type[0])) {
        say(err, errcap, ja, L"%d 番目の変数の型 %ls には対応していません（i u c C t a o s x d n）",
            L"Variable %d: unsupported type %ls (i u c C t a o s x d n)", index, type);
        return FALSE;
    }

    int mark = ber_size(b);
    BOOL bad_value = FALSE;
    BYTE tmp[SNMP_MSG_MAX];
    int len = 0;
    switch (type[0]) {
    case L'i': {
        int64_t v;
        if (parse_int(val, INT32_MIN, INT32_MAX, &v)) ber_int(b, TAG_INTEGER, v);
        else bad_value = TRUE;
        break;
    }
    case L'u': case L'c': case L't': {
        uint64_t v;
        BYTE tag = (type[0] == L'u') ? TAG_GAUGE32 : (type[0] == L'c') ? TAG_COUNTER32 : TAG_TIMETICKS;
        if (parse_uint(val, 0xFFFFFFFFull, &v)) ber_uint(b, tag, v);
        else bad_value = TRUE;
        break;
    }
    case L'C': {
        uint64_t v;
        if (parse_uint(val, UINT64_MAX, &v)) ber_uint(b, TAG_COUNTER64, v);
        else bad_value = TRUE;
        break;
    }
    case L'a': {
        IN_ADDR a;
        if (InetPtonW(AF_INET, val, &a) == 1) ber_octets(b, TAG_IPADDRESS, (const BYTE*)&a, 4);
        else bad_value = TRUE;
        break;
    }
    case L'o': {
        uint32_t v[OID_ARCS_MAX];
        int vn;
        if (parse_oid(val, v, &vn)) ber_oid(b, v, vn);
        else bad_value = TRUE;
        break;
    }
    case L's':
        len = to_charset(val, charset, tmp, sizeof(tmp));
        if (len == -1) {
            say(err, errcap, ja, L"%d 番目の変数の値に %ls で表せない文字があります: %ls",
                L"Variable %d: the value has characters that %ls cannot represent: %ls",
                index, charset_name(charset), val);
            return FALSE;
        }
        if (len >= 0) ber_octets(b, TAG_OCTETS, tmp, len);
        break;
    case L'x':
        len = parse_hex(val, tmp, sizeof(tmp));
        if (len == -1) bad_value = TRUE;
        else if (len >= 0) ber_octets(b, TAG_OCTETS, tmp, len);
        break;
    case L'd':
        len = parse_dec_bytes(val, tmp, sizeof(tmp));
        if (len == -1) bad_value = TRUE;
        else if (len >= 0) ber_octets(b, TAG_OCTETS, tmp, len);
        break;
    case L'n':
        ber_octets(b, TAG_NULL, NULL, 0);
        break;
    }
    if (bad_value) {
        say(err, errcap, ja, L"%d 番目の変数の値が型 %ls として正しくありません: %ls",
            L"Variable %d: invalid value for type %ls: %ls", index, type, val);
        return FALSE;
    }
    if (len == -2) b->overflow = true;   // value alone exceeds the message size

    ber_oid(b, arcs, n);
    ber_wrap(b, TAG_SEQUENCE, mark);
    return TRUE;
}

// Encodes the complete message. Returns a pointer into buf, or NULL if it does not fit.
static const BYTE* build_message(const SnmpTrap* t, const BYTE agent[4], uint32_t uptime,
                                 int32_t request_id, BYTE* buf, int cap, int* len) {
    static const uint32_t SYS_UPTIME_0[]    = { 1, 3, 6, 1, 2, 1, 1, 3, 0 };
    static const uint32_t SNMP_TRAP_OID_0[] = { 1, 3, 6, 1, 6, 3, 1, 1, 4, 1, 0 };

    Ber b;
    ber_init(&b, buf, cap);

    // VarBindList (v2c: sysUpTime.0 and snmpTrapOID.0 come first, RFC 3416 §4.2.6)
    ber_bytes(&b, t->varbinds, t->varbinds_len);
    if (t->version == 1) {
        int m = ber_size(&b);
        ber_bytes(&b, t->oid_tlv, t->oid_tlv_len);
        ber_oid(&b, SNMP_TRAP_OID_0, (int)_countof(SNMP_TRAP_OID_0));
        ber_wrap(&b, TAG_SEQUENCE, m);
        m = ber_size(&b);
        ber_uint(&b, TAG_TIMETICKS, uptime);
        ber_oid(&b, SYS_UPTIME_0, (int)_countof(SYS_UPTIME_0));
        ber_wrap(&b, TAG_SEQUENCE, m);
    }
    ber_wrap(&b, TAG_SEQUENCE, 0);

    if (t->version == 0) {
        // Trap-PDU ::= { enterprise, agent-addr, generic-trap, specific-trap, time-stamp, variable-bindings }
        ber_uint(&b, TAG_TIMETICKS, uptime);
        ber_int(&b, TAG_INTEGER, t->specific_trap);
        ber_int(&b, TAG_INTEGER, t->generic_trap);
        ber_octets(&b, TAG_IPADDRESS, agent, 4);
        ber_bytes(&b, t->oid_tlv, t->oid_tlv_len);
        ber_wrap(&b, TAG_TRAP_V1, 0);
    } else {
        // SNMPv2-Trap-PDU ::= { request-id, error-status, error-index, variable-bindings }
        ber_int(&b, TAG_INTEGER, 0);
        ber_int(&b, TAG_INTEGER, 0);
        ber_int(&b, TAG_INTEGER, request_id);
        ber_wrap(&b, TAG_TRAP_V2, 0);
    }

    // Message ::= { version, community, data }
    ber_octets(&b, TAG_OCTETS, t->community, t->community_len);
    ber_int(&b, TAG_INTEGER, t->version);
    ber_wrap(&b, TAG_SEQUENCE, 0);

    if (b.overflow) return NULL;
    *len = ber_size(&b);
    return buf + b.pos;
}

static BOOL parse_tokens(const Tokens* tk, UINT charset, BOOL ja, SnmpTrap* t, wchar_t* err, int errcap) {
    Header h;
    if (!scan_header(tk, ja, &h, err, errcap)) return FALSE;
    LPWSTR* av = tk->argv;
    t->version = h.version;

    int clen = to_charset(h.community, charset, t->community, sizeof(t->community));
    if (clen < 0) {
        say(err, errcap, ja, L"コミュニティ名が長すぎるか、%ls で表せない文字があります。",
            L"The community is too long or has characters that %ls cannot represent.", charset_name(charset));
        return FALSE;
    }
    t->community_len = clen;

    if (!parse_host(av[h.host], t, ja, err, errcap)) return FALSE;

    const wchar_t* uptime_s;
    if (t->version == 0) {
        const wchar_t* ent   = av[h.fields];
        const wchar_t* agent = av[h.fields + 1];
        const wchar_t* gen   = av[h.fields + 2];
        const wchar_t* spec  = av[h.fields + 3];
        uptime_s             = av[h.fields + 4];

        // net-snmp's default enterprise when "" is given
        if (!encode_oid_tlv(ent[0] ? ent : L".1.3.6.1.4.1.3.1.1", t->oid_tlv, &t->oid_tlv_len)) {
            say(err, errcap, ja, L"enterprise-oid が正しくありません: %ls（.1.3.6.1.4.1.… のような数値の OID）",
                L"Invalid enterprise-oid: %ls (numeric OID such as .1.3.6.1.4.1....)", ent);
            return FALSE;
        }
        if (!agent[0]) {
            t->agent_auto = TRUE;
        } else {
            IN_ADDR a;
            if (InetPtonW(AF_INET, agent, &a) != 1) {
                say(err, errcap, ja, L"agent アドレスが正しくありません: %ls（IPv4 アドレス。自動にするときは \"\"）",
                    L"Invalid agent address: %ls (IPv4 address, or \"\" for automatic)", agent);
                return FALSE;
            }
            memcpy(t->agent_addr, &a, 4);
        }
        int64_t v;
        if (!parse_int(gen, 0, 6, &v)) {
            say(err, errcap, ja, L"generic-trap は 0〜6 の数値で指定してください: %ls",
                L"generic-trap must be a number from 0 to 6: %ls", gen);
            return FALSE;
        }
        t->generic_trap = (int32_t)v;
        if (!parse_int(spec, INT32_MIN, INT32_MAX, &v)) {
            say(err, errcap, ja, L"specific-trap が正しくありません: %ls（32 ビットの整数）",
                L"Invalid specific-trap: %ls (32-bit integer)", spec);
            return FALSE;
        }
        t->specific_trap = (int32_t)v;
    } else {
        uptime_s = av[h.fields];
        const wchar_t* trap_oid = av[h.fields + 1];
        if (!encode_oid_tlv(trap_oid, t->oid_tlv, &t->oid_tlv_len)) {
            say(err, errcap, ja, L"trap-oid が正しくありません: %ls（数値の OID）",
                L"Invalid trap-oid: %ls (numeric OID)", trap_oid);
            return FALSE;
        }
    }

    if (!uptime_s[0]) {
        t->uptime_auto = TRUE;
    } else {
        uint64_t v;
        if (!parse_uint(uptime_s, 0xFFFFFFFFull, &v)) {
            say(err, errcap, ja, L"uptime が正しくありません: %ls（1/100 秒単位の数値。自動にするときは \"\"）",
                L"Invalid uptime: %ls (hundredths of a second, or \"\" for automatic)", uptime_s);
            return FALSE;
        }
        t->uptime = (uint32_t)v;
    }

    int nargs = tk->argc - h.varbinds;
    if (nargs % 3 != 0) {
        say(err, errcap, ja, L"変数の指定が途中で終わっています。変数は「OID 型 値」の 3 つ組で書いてください。",
            L"Incomplete variable binding. Each variable needs three arguments: OID TYPE VALUE.");
        return FALSE;
    }

    // Encode back to front, last variable first, so the list ends up in the given order.
    Ber b;
    ber_init(&b, t->varbinds, sizeof(t->varbinds));
    for (int k = nargs / 3 - 1; k >= 0 && !b.overflow; k--) {
        int i = h.varbinds + k * 3;
        if (!encode_varbind(&b, k + 1, av[i], av[i + 1], av[i + 2], charset, ja, err, errcap))
            return FALSE;
    }

    // Also make sure the complete message fits, with the longest uptime / request-id encodings.
    BYTE probe[SNMP_MSG_MAX];
    int probe_len;
    static const BYTE AGENT_MAX[4] = { 255, 255, 255, 255 };
    if (!b.overflow) {
        t->varbinds_len = ber_size(&b);
        memmove(t->varbinds, t->varbinds + b.pos, t->varbinds_len);
    }
    if (b.overflow || !build_message(t, AGENT_MAX, 0xFFFFFFFFu, 0x7FFFFFFF, probe, sizeof(probe), &probe_len)) {
        say(err, errcap, ja, L"Trap が大きすぎます（最大 %d バイト）。",
            L"The trap is too large (max %d bytes).", SNMP_MSG_MAX);
        return FALSE;
    }
    return TRUE;
}

// ============================================================
// Public API
// ============================================================

BOOL snmp_args_blank(const wchar_t* args) {
    for (const wchar_t* p = args; *p; p++)
        if (*p != L' ' && *p != L'\t' && *p != L'\r' && *p != L'\n' && *p != L'^') return FALSE;
    return TRUE;
}

BOOL snmp_trap_parse(const wchar_t* args, UINT charset, BOOL ja,
                     SnmpTrap* t, wchar_t* err, int errcap) {
    memset(t, 0, sizeof(*t));
    if (err && errcap > 0) err[0] = 0;
    if (wcslen(args) >= SNMP_ARGS_MAX) {
        say(err, errcap, ja, L"引数が長すぎます（最大 %d 文字）。",
            L"The arguments are too long (max %d characters).", SNMP_ARGS_MAX - 1);
        return FALSE;
    }
    Tokens tk;
    if (!tokenize(args, &tk)) {
        say(err, errcap, ja, L"引数を解釈できませんでした。", L"Could not split the arguments.");
        return FALSE;
    }
    BOOL ok = parse_tokens(&tk, charset, ja, t, err, errcap);
    LocalFree(tk.argv);
    return ok;
}

struct Out {
    wchar_t* buf;
    int      cap;
    int      n;
    bool     full;
};

static void out_char(Out* o, wchar_t c) {
    if (o->n >= o->cap - 1) { o->full = true; return; }
    o->buf[o->n++] = c;
    o->buf[o->n] = 0;
}

// Quotes tok so that the C runtime / CommandLineToArgvW gives it back unchanged.
static void out_token(Out* o, const wchar_t* tok, BOOL force_quotes) {
    BOOL quote = force_quotes || !tok[0] || wcspbrk(tok, L" \t\"") != NULL;
    if (!quote) {
        for (const wchar_t* p = tok; *p; p++) out_char(o, *p);
        return;
    }
    out_char(o, L'"');
    for (const wchar_t* p = tok; ; p++) {
        int backslashes = 0;
        while (*p == L'\\') { backslashes++; p++; }
        if (!*p) {
            for (int k = 0; k < backslashes * 2; k++) out_char(o, L'\\');
            break;
        }
        if (*p == L'"') {
            for (int k = 0; k < backslashes * 2 + 1; k++) out_char(o, L'\\');
        } else {
            for (int k = 0; k < backslashes; k++) out_char(o, L'\\');
        }
        out_char(o, *p);
    }
    out_char(o, L'"');
}

void snmp_args_canonical(const wchar_t* args, BOOL multiline, wchar_t* out, int outcap) {
    if (outcap <= 0) return;
    out[0] = 0;
    Tokens tk;
    if (wcslen(args) >= SNMP_ARGS_MAX || !tokenize(args, &tk)) {
        flatten(args, out, outcap);
        return;
    }
    Header h;
    int vb = scan_header(&tk, FALSE, &h, NULL, 0) ? h.varbinds : -1;

    Out o = { out, outcap, 0, false };
    for (int i = tk.first; i < tk.argc; i++) {
        BOOL in_varbinds = (vb >= 0 && i >= vb);
        if (i > tk.first) {
            if (multiline && in_varbinds && (i - vb) % 3 == 0) {
                out_char(&o, L'\r');
                out_char(&o, L'\n');
            } else {
                out_char(&o, L' ');
            }
        }
        // Keep string values quoted ("1", "TEST") the way they are usually written.
        BOOL string_value = in_varbinds && (i - vb) % 3 == 2 && wcscmp(tk.argv[i - 1], L"s") == 0;
        out_token(&o, tk.argv[i], string_value);
    }
    LocalFree(tk.argv);

    if (o.full) {
        if (multiline) snmp_args_canonical(args, FALSE, out, outcap);
        else           flatten(args, out, outcap);
    }
}

static volatile LONG s_request_id = 0;

BOOL snmp_trap_send(const SnmpTrap* t, BOOL ja, wchar_t* info, int infocap) {
    ADDRINFOW hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    ADDRINFOW* ai = NULL;
    int rc = GetAddrInfoW(t->host, t->port, &hints, &ai);
    if (rc != 0 || !ai) {
        say(info, infocap, ja, L"%ls の名前解決に失敗しました（エラー %d）",
            L"Cannot resolve %ls (error %d)", t->host, rc);
        if (ai) FreeAddrInfoW(ai);
        return FALSE;
    }
    sockaddr_in dst;
    memcpy(&dst, ai->ai_addr, sizeof(dst));
    FreeAddrInfoW(ai);

    wchar_t addr[INET_ADDRSTRLEN] = L"?";
    InetNtopW(AF_INET, &dst.sin_addr, addr, INET_ADDRSTRLEN);
    wchar_t where[300];
    if (wcscmp(addr, t->host) == 0)
        _snwprintf_s(where, _countof(where), _TRUNCATE, L"%ls:%ls", addr, t->port);
    else
        _snwprintf_s(where, _countof(where), _TRUNCATE, L"%ls (%ls):%ls", t->host, addr, t->port);

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        say(info, infocap, ja, L"ソケットを作成できませんでした（エラー %d）",
            L"Cannot create a socket (error %d)", WSAGetLastError());
        return FALSE;
    }

    BOOL ok = FALSE;
    int len = 0;
    // connect() on a UDP socket only fixes the peer. It also makes getsockname() report the
    // local address used towards the manager, which is what agent-addr "" stands for.
    if (connect(s, (const sockaddr*)&dst, sizeof(dst)) != 0) {
        say(info, infocap, ja, L"%ls へ送信できません（エラー %d）",
            L"Cannot send to %ls (error %d)", where, WSAGetLastError());
    } else {
        BYTE agent[4];
        if (t->agent_auto) {
            sockaddr_in local;
            int local_len = sizeof(local);
            memset(&local, 0, sizeof(local));
            getsockname(s, (sockaddr*)&local, &local_len);
            memcpy(agent, &local.sin_addr, 4);
        } else {
            memcpy(agent, t->agent_addr, 4);
        }
        uint32_t uptime = t->uptime_auto ? (uint32_t)(GetTickCount64() / 10) : t->uptime;
        int32_t  reqid  = (int32_t)((uint32_t)InterlockedIncrement(&s_request_id) & 0x7FFFFFFFu);

        BYTE buf[SNMP_MSG_MAX];
        const BYTE* msg = build_message(t, agent, uptime, reqid, buf, sizeof(buf), &len);
        if (!msg) {
            say(info, infocap, ja, L"Trap が大きすぎます", L"The trap is too large");
        } else if (send(s, (const char*)msg, len, 0) != len) {
            say(info, infocap, ja, L"%ls へ送信できません（エラー %d）",
                L"Cannot send to %ls (error %d)", where, WSAGetLastError());
        } else {
            ok = TRUE;
        }
    }
    closesocket(s);

    if (ok) say(info, infocap, ja, L"%ls、%d バイト", L"%ls, %d bytes", where, len);
    return ok;
}
