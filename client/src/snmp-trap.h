// snmp-trap.h - SNMPv1 / SNMPv2c trap sender configured with net-snmp snmptrap arguments
//
// A trap is written exactly like the arguments of net-snmp's snmptrap command, e.g.
//   -v 1 -c public 192.0.2.1 .1.3.6.1.4.1.99999 "" 6 1 "" .1.3.6.1.4.1.99999.1 s "text"
// so the contents of an existing batch file (snmptrap.exe ... with ^ continuation lines)
// can be pasted as is.
//
// All parsing and most of the BER encoding happen once, when the settings are loaded.
// Sending a trap only fills in the uptime and agent address, encodes into a stack buffer
// and calls send(): no child process and no heap allocation per trap.

#pragma once

#include <windows.h>
#include <stdint.h>

#define SNMP_ARGS_MAX     4096    // max length of the argument text (wchar_t, incl. NUL)
#define SNMP_MSG_MAX      4096    // max size of one encoded trap message (bytes)
#define SNMP_OID_TLV_MAX  700     // one encoded OID incl. tag and length (128 arcs x 5 bytes + 4)

// Character set for 's' values and the community string.
// snmptrap.exe sends its command line in the ANSI code page, i.e. Shift_JIS on Japanese Windows.
#define SNMP_CHARSET_SJIS 932
#define SNMP_CHARSET_UTF8 65001

struct SnmpTrap {
    int      version;                                   // version field: 0 = SNMPv1, 1 = SNMPv2c
    wchar_t  host[256];
    wchar_t  port[8];
    BYTE     community[256];            int community_len;
    BYTE     oid_tlv[SNMP_OID_TLV_MAX]; int oid_tlv_len;  // v1: enterprise, v2c: snmpTrapOID.0 value
    BOOL     agent_auto;  BYTE agent_addr[4];           // v1 only. auto = local address towards host
    int32_t  generic_trap, specific_trap;               // v1 only
    BOOL     uptime_auto; uint32_t uptime;              // auto = Windows uptime (1/100 s)
    BYTE     varbinds[SNMP_MSG_MAX];    int varbinds_len; // the user's variable bindings, encoded
};

// TRUE if args holds nothing but blanks / ^ continuations (= no trap configured).
BOOL snmp_args_blank(const wchar_t* args);

// Parses args into t. On failure returns FALSE and writes a message to err
// (Japanese if ja, otherwise English).
BOOL snmp_trap_parse(const wchar_t* args, UINT charset, BOOL ja,
                     SnmpTrap* t, wchar_t* err, int errcap);

// Rewrites args in canonical form: lines before snmptrap.exe, rem lines and ^ continuations
// are dropped and the arguments are re-quoted. multiline puts each variable binding on its
// own line (for the settings dialog); otherwise the result is one line (for the INI file).
void snmp_args_canonical(const wchar_t* args, BOOL multiline, wchar_t* out, int outcap);

// Sends the trap once over UDP. info receives "address:port, N bytes" or an error message.
BOOL snmp_trap_send(const SnmpTrap* t, BOOL ja, wchar_t* info, int infocap);
