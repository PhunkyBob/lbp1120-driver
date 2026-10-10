/*
 *  lbp1120lm.dll - "LBP1120 CAPT Monitor", a Windows x64 print LANGUAGE MONITOR for the
 *                  Canon LASER SHOT LBP-1120 / LBP-810 (CAPT host-based laser printers).
 *
 *  The render plugin (unidrv\) emits exactly the byte stream that captsend --print writes:
 *  CAPT packets [op_lo][op_hi][len_lo][len_hi][payload], status commands and a0 c0 data
 *  packets, in order. This monitor sits between the spooler and the port monitor (usbmon.dll),
 *  reassembles those packets across WritePort calls and replays captsend's conversation with
 *  the printer: per-opcode status reads, the a1 a1 identity check, the paper wait, the
 *  "a0 e0 until ready" gate before every data packet, 4096-byte data chunks and the timing.
 *
 *  Protocol logic derived from:
 *    Linux Canon CAPT driver (capt-0.1, capt.c)
 *    Copyright (C) 2004 Nicolas Boichat <nicolas@boichat.ch>
 *  via its faithful C# port lbp1120-x64\captsend\captsend.cs.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details (file COPYING).
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winspool.h>
#include <winsplp.h>
#include <sddl.h>
#include <aclapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

/* ------------------------------------------------------------------ constants (captsend.cs) */
#define LM_VERSION          "lbp1120lm 1.0"
#define LM_MAX_PACKET       0x10000     /* CAPT length field is 16 bits: a packet is <= 65535 bytes   */
#define LM_REPLY_SIZE       256         /* cmdbuffer[j] = new byte[256]                               */
#define LM_NREPLY           2           /* at most 2 reads per command (6-byte header, then body)     */
#define LM_CHUNK            4096        /* a0 c0 data is written in 4096-byte chunks                  */
#define WAIT_MS             8           /* capt.c WAIT = 8000 us: after every command write and read  */
#define WAIT_CHUNK_MS       16          /* 2*WAIT after each data chunk write and after the gate      */
#define WAIT_PACKET_MS      40          /* 5*WAIT after each complete data packet                     */
#define GATE_POLL_MS        100         /* Ssleep(100000) between a0 e0 gate polls                    */
#define PAPER_RETRY_MS      1000        /* Usleep(1000000) between paper checks                       */
#define CANCEL_CHECK_MS     250         /* GetJob() rate limit while data flows                       */

#define JS_NONE      0   /* no document */
#define JS_ACTIVE    1
#define JS_FAILED    2   /* protocol/IO failure: rest of the job is drained */
#define JS_CANCELLED 3   /* job deleted / port closing                       */

/* ------------------------------------------------------------------ configuration (registry) */
typedef struct LMCONFIG {
    DWORD logLevel;          /* LogLevel       0 = off (default), 1 = protocol, 2 = + WritePort calls */
    WCHAR logFile[MAX_PATH]; /* LogFile        default %ProgramData%\LBP1120\lm.log                    */
    DWORD maxLogKB;          /* MaxLogKB       truncate in place above this size (default 4096)       */
    DWORD captureSent;       /* CaptureSent    1 = copy the bytes sent to job<N>-...sent.bin (needs LogLevel) */
    DWORD readTimeoutMs;     /* ReadTimeoutMs  per status read (captsend --read-timeout, 2000)        */
    DWORD writeTimeoutMs;    /* WriteTimeoutMs passed to SetPortTimeOuts (captsend --write-timeout)   */
    DWORD gateTimeoutSec;    /* GateTimeoutSec a0 e0 ready wait before a data packet (120)            */
    DWORD paperWaitTries;    /* PaperWaitTries a0 a0 + a1 a0 attempts, 1 s apart (0 = no limit, default) */
    DWORD writeRetries;      /* WriteRetries   data chunk retries when nothing was written (3)        */
} LMCONFIG;

/* ------------------------------------------------------------------ spooler job control (spoolss.dll, resolved at run time) */
typedef BOOL (WINAPI *PFN_OPENPRINTERW)(LPWSTR, LPHANDLE, LPPRINTER_DEFAULTSW);
typedef BOOL (WINAPI *PFN_CLOSEPRINTER)(HANDLE);
typedef BOOL (WINAPI *PFN_GETJOBW)(HANDLE, DWORD, DWORD, LPBYTE, DWORD, LPDWORD);
typedef BOOL (WINAPI *PFN_SETJOBW)(HANDLE, DWORD, DWORD, LPBYTE, DWORD);
typedef BOOL (WINAPI *PFN_SETPORTW)(LPWSTR, LPWSTR, DWORD, LPBYTE);

static PFN_OPENPRINTERW pOpenPrinterW;
static PFN_CLOSEPRINTER pClosePrinter;
static PFN_GETJOBW      pGetJobW;
static PFN_SETJOBW      pSetJobW;
static PFN_SETPORTW     pSetPortW;      /* optional: port status ("paper out") */

/* ------------------------------------------------------------------ per-port state */
typedef struct LMPORT {
    DWORD            sig;
    struct LMPORT   *next;
    LONG             refs;
    BOOL             closing;
    CRITICAL_SECTION cs;           /* serialises every call on this port */
    HANDLE           hAbort;       /* manual-reset: ClosePort / Shutdown */
    HANDLE           hTimer;       /* high-resolution waitable timer (NULL -> fallback) */
    MONITOR2         fn;           /* the port monitor's function table (usbmon) */
    HANDLE           hPort;        /* the port monitor's handle */
    WCHAR            portName[64];
    WCHAR            printerName[260];
    LMCONFIG         cfg;          /* snapshot taken at StartDocPort */

    /* job */
    int              state;
    DWORD            jobId;
    HANDLE           hPrinter;     /* spooler handle for GetJob/SetJob, NULL if unavailable */
    DWORD            failErr;
    BOOL             drainLogged;
    BOOL             statusShown;
    ULONGLONG        lastCancelCheck;
    HANDLE           hCapture;
    BYTE             pkt[LM_MAX_PACKET];
    DWORD            pktHave;      /* bytes of the current packet received so far */
    DWORD            pktNeed;      /* its total length (valid once pktHave >= 4)   */
    BYTE             reply[LM_NREPLY][LM_REPLY_SIZE];
    BYTE             lastOp[2];    /* opcode of the last packet sent to the printer */
    BOOL             paperPending; /* armed at job start and by a2 d0: next a0 a0 | a1 a0 = paper check */
    BOOL             paperOutSet;  /* port status PAPER_OUT set by this job, to be cleared           */
    DWORD            pagesDone;    /* a2 d0 sent in this job                                         */
    BOOL             paperWaited;  /* the last WaitForPaper() had to wait                            */
    BOOL             engineNoPaper;/* a0 a0 body[5] & 0x02 seen while waiting (job dropped, see below)  */
    BYTE             pageParams[64];   /* the current page's a0 d0 packet, replayed to re-open the page */
    DWORD            pageParamsLen;
    BOOL             midPage;      /* a1 d0 sent, a2 d0 not yet                     */
    BOOL             jobOpen;      /* a0 a2 (job init) sent: a cancel must stop the printer         */
    DWORD            pageIndex;    /* data packet index within the page (captsend ccbm) */
    DWORD            nCmd, nPoll, nChunk, nPacket, nWritePort;
    ULONGLONG        bytesIn, bytesOut;
} LMPORT;

#define LMPORT_SIG 0x4d4c5043u /* 'CPLM' */

static CRITICAL_SECTION g_cs;       /* port list, configuration */
static CRITICAL_SECTION g_logCs;
static LMPORT          *g_ports;
static LMCONFIG         g_cfg;
static HANDLE           g_hSpooler;
static HKEYMONITOR      g_hckRoot;
static PMONITORREG      g_reg;
static LARGE_INTEGER    g_qpf, g_t0;
/* logging state, all under g_logCs */
#define LOGST_OFF      0   /* not configured / not yet opened  */
#define LOGST_ACTIVE   1
#define LOGST_DISABLED 2   /* a safety check failed: no logging until the next job re-validates */
static int              g_logState;
static char             g_logWhy[300];
static HANDLE           g_hLogDir = INVALID_HANDLE_VALUE; /* validated directory, held open WITHOUT FILE_SHARE_DELETE */
static HANDLE           g_hLog = INVALID_HANDLE_VALUE;
static WCHAR            g_logDirFinal[MAX_PATH];        /* its final (resolved) path */
static WCHAR            g_openLogPath[MAX_PATH];        /* LogFile value the state above belongs to */
static DWORD            g_logLines;
static BYTE             g_selfSid[SECURITY_MAX_SID_SIZE];/* user SID of this process: SYSTEM inside spoolsv.exe */
static BOOL             g_haveSelfSid;
static DWORD            g_monitorToken = 0x4c424d31; /* the hMonitor we hand out */

/* ------------------------------------------------------------------ time */
static double NowSec(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_t0.QuadPart) / (double)g_qpf.QuadPart;
}

static ULONGLONG NowMs(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (ULONGLONG)((t.QuadPart * 1000) / g_qpf.QuadPart);
}

static BOOL Aborted(LMPORT *p)
{
    return WaitForSingleObject(p->hAbort, 0) == WAIT_OBJECT_0;
}

/* Precise, NON-abortable short sleep (8/16/40 ms), like captsend's Ssleep(): these are protocol
 * timing and always bounded, so they are honoured even while the port is being closed. */
static void LmSleep(LMPORT *p, DWORD ms)
{
    if (ms == 0) return;
    if (p->hTimer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)ms * 10000;
        if (SetWaitableTimer(p->hTimer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(p->hTimer, ms + 1000);
            return;
        }
    }
    if (ms >= 20) { Sleep(ms); return; }
    {   /* < 20 ms without a high-resolution timer: spin on QPC (captsend Ssleep does the same) */
        ULONGLONG end = NowMs() + ms;
        while (NowMs() < end) YieldProcessor();
    }
}

/* Abortable wait (read polling); FALSE if the port is being closed. */
static BOOL LmWaitAbortable(LMPORT *p, DWORD ms)
{
    if (p->hTimer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)ms * 10000;
        if (SetWaitableTimer(p->hTimer, &due, 0, NULL, NULL, FALSE)) {
            HANDLE h[2];
            h[0] = p->hAbort; h[1] = p->hTimer;
            if (WaitForMultipleObjects(2, h, FALSE, ms + 1000) == WAIT_OBJECT_0) {
                CancelWaitableTimer(p->hTimer);
                return FALSE;
            }
            return TRUE;
        }
    }
    return WaitForSingleObject(p->hAbort, ms) != WAIT_OBJECT_0;
}

/* ------------------------------------------------------------------ logging
 *
 * The monitor runs as SYSTEM inside spoolsv.exe, so the log and capture files must never be
 * redirectable by an unprivileged user (junction / symlink / hardlink planted in a directory that
 * user pre-created, e.g. under %ProgramData%, where Users may create sub-folders they own):
 *   - logging and capture are OFF unless an administrator sets LogLevel (and CaptureSent) in the
 *     monitor's own registry key (HKLM\SYSTEM\CurrentControlSet\Control\Print\Monitors\<name>,
 *     writable by administrators only);
 *   - the directory is created with a protected DACL (SYSTEM, Administrators, and this process's
 *     own user = SYSTEM again inside the spooler); an existing directory is accepted only if it is
 *     not a reparse point, is owned by SYSTEM / Administrators / this process's user, and its
 *     final path equals the configured one (no junction in a parent). It is then held open
 *     without FILE_SHARE_DELETE so it cannot be renamed or replaced while in use;
 *   - files are opened with FILE_FLAG_OPEN_REPARSE_POINT and rejected if they are a reparse point,
 *     have more than one link, or resolve elsewhere; capture files use CREATE_NEW + unique names;
 *   - no rename-based rotation: the validated handle is truncated in place at MaxLogKB.
 * Any failed check disables logging (it never tries to repair the directory). */
static void LogCloseLocked(void)
{
    if (g_hLog != INVALID_HANDLE_VALUE) { CloseHandle(g_hLog); g_hLog = INVALID_HANDLE_VALUE; }
    if (g_hLogDir != INVALID_HANDLE_VALUE) { CloseHandle(g_hLogDir); g_hLogDir = INVALID_HANDLE_VALUE; }
}

static void LogDisableLocked(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(g_logWhy, sizeof(g_logWhy), _TRUNCATE, fmt, ap);
    va_end(ap);
    LogCloseLocked();
    g_logState = LOGST_DISABLED;
    OutputDebugStringA("lbp1120lm: logging disabled: ");
    OutputDebugStringA(g_logWhy);
    OutputDebugStringA("\n");
}

static void InitSelfSid(void)
{
    HANDLE tok;
    BYTE buf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    DWORD cb = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &cb)) {
        PSID sid = ((TOKEN_USER *)buf)->User.Sid;
        if (IsValidSid(sid) && GetLengthSid(sid) <= sizeof(g_selfSid) && CopySid(sizeof(g_selfSid), g_selfSid, sid))
            g_haveSelfSid = TRUE;
    }
    CloseHandle(tok);
}

static BOOL TrustedOwner(PSID owner)
{
    return owner && IsValidSid(owner) &&
           (IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid) ||
            (g_haveSelfSid && EqualSid(owner, (PSID)g_selfSid)));
}

/* D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)[(A;OICI;FA;;;<self>)]  - the caller LocalFree()s *psd */
static BOOL ProtectedSd(PSECURITY_DESCRIPTOR *psd)
{
    WCHAR sddl[256];
    LPWSTR self = NULL;
    BOOL ok;
    lstrcpyW(sddl, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
    if (g_haveSelfSid && !IsWellKnownSid((PSID)g_selfSid, WinLocalSystemSid) && ConvertSidToStringSidW((PSID)g_selfSid, &self)) {
        if (lstrlenW(sddl) + lstrlenW(self) + 20 < 256) {
            lstrcatW(sddl, L"(A;OICI;FA;;;");
            lstrcatW(sddl, self);
            lstrcatW(sddl, L")");
        }
        LocalFree(self);
    }
    ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, psd, NULL);
    if (!ok) *psd = NULL;
    return ok;
}

/* final path of a handle, without the \\?\ prefix */
static BOOL FinalPath(HANDLE h, WCHAR *out, DWORD cch)
{
    WCHAR tmp[MAX_PATH + 16];
    DWORD n = GetFinalPathNameByHandleW(h, tmp, MAX_PATH + 16, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    out[0] = 0;
    if (n == 0 || n >= MAX_PATH + 16) return FALSE;
    if (wcsncmp(tmp, L"\\\\?\\UNC\\", 8) == 0) { out[0] = L'\\'; lstrcpynW(out + 1, tmp + 7, (int)cch - 1); }
    else if (wcsncmp(tmp, L"\\\\?\\", 4) == 0) lstrcpynW(out, tmp + 4, (int)cch);
    else lstrcpynW(out, tmp, (int)cch);
    return TRUE;
}

static const char *CheckHandle(HANDLE h, BOOL wantDir)
{
    BY_HANDLE_FILE_INFORMATION fi;
    if (!GetFileInformationByHandle(h, &fi)) return "GetFileInformationByHandle failed";
    if (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return "is a reparse point (junction/symlink)";
    if (wantDir && !(fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return "is not a directory";
    if (!wantDir && (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return "is a directory";
    if (!wantDir && fi.nNumberOfLinks != 1) return "has more than one hard link";
    return NULL;
}

static BOOL OpenLogDirLocked(const WCHAR *dirIn)
{
    WCHAR dir[MAX_PATH], fin[MAX_PATH];
    SECURITY_ATTRIBUTES sa;
    PSECURITY_DESCRIPTOR sd = NULL, osd = NULL;
    PSID owner = NULL;
    HANDLE h;
    const char *why;
    DWORD n = GetFullPathNameW(dirIn, MAX_PATH, dir, NULL);
    size_t len;

    if (n == 0 || n >= MAX_PATH) { LogDisableLocked("log directory path invalid or too long"); return FALSE; }
    len = wcslen(dir);
    if (len > 3 && dir[len - 1] == L'\\') dir[--len] = 0;
    if (!ProtectedSd(&sd)) { LogDisableLocked("cannot build the log directory DACL (error %lu)", GetLastError()); return FALSE; }
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    if (!CreateDirectoryW(dir, &sa) && GetLastError() != ERROR_ALREADY_EXISTS) {
        DWORD e = GetLastError();
        LocalFree(sd);
        LogDisableLocked("cannot create log directory %ls (error %lu)", dir, e);
        return FALSE;
    }
    LocalFree(sd);
    /* no FILE_SHARE_DELETE: while held, the directory cannot be renamed, deleted or swapped */
    h = CreateFileW(dir, READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) { LogDisableLocked("cannot open log directory %ls (error %lu)", dir, GetLastError()); return FALSE; }
    if ((why = CheckHandle(h, TRUE)) != NULL) { CloseHandle(h); LogDisableLocked("log directory %ls %s", dir, why); return FALSE; }
    if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, NULL, NULL, NULL, &osd) != ERROR_SUCCESS) {
        CloseHandle(h);
        LogDisableLocked("cannot read the owner of log directory %ls", dir);
        return FALSE;
    }
    if (!TrustedOwner(owner)) {
        LPWSTR so = NULL;
        ConvertSidToStringSidW(owner, &so);
        LogDisableLocked("log directory %ls is owned by %ls, not by SYSTEM/Administrators/this service", dir, so ? so : L"?");
        if (so) LocalFree(so);
        LocalFree(osd);
        CloseHandle(h);
        return FALSE;
    }
    LocalFree(osd);
    if (!FinalPath(h, fin, MAX_PATH) || lstrcmpiW(fin, dir) != 0) {
        CloseHandle(h);
        LogDisableLocked("log directory %ls resolves to %ls (junction/symlink in the path?)", dir, fin);
        return FALSE;
    }
    g_hLogDir = h;
    lstrcpynW(g_logDirFinal, fin, MAX_PATH);
    return TRUE;
}

/* Opens <validated dir>\leaf safely (OPEN_ALWAYS for the log, CREATE_NEW for captures). */
static HANDLE OpenSafeFileLocked(const WCHAR *leaf, DWORD disposition, const char **why)
{
    WCHAR path[MAX_PATH], fin[MAX_PATH];
    SECURITY_ATTRIBUTES sa;
    PSECURITY_DESCRIPTOR sd = NULL;
    HANDLE h;
    *why = NULL;
    if (g_hLogDir == INVALID_HANDLE_VALUE) { *why = "no validated log directory"; return INVALID_HANDLE_VALUE; }
    if (!leaf[0] || wcspbrk(leaf, L"\\/:") || lstrlenW(g_logDirFinal) + lstrlenW(leaf) + 2 > MAX_PATH) {
        *why = "invalid file name";
        return INVALID_HANDLE_VALUE;
    }
    swprintf_s(path, MAX_PATH, L"%s\\%s", g_logDirFinal, leaf);
    if (!ProtectedSd(&sd)) { *why = "cannot build DACL"; return INVALID_HANDLE_VALUE; }
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    h = CreateFileW(path, FILE_READ_ATTRIBUTES | FILE_WRITE_DATA | FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ, &sa,
                    disposition, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    LocalFree(sd);
    if (h == INVALID_HANDLE_VALUE) { *why = "CreateFile failed"; return INVALID_HANDLE_VALUE; }
    *why = CheckHandle(h, FALSE);
    if (!*why && !FinalPath(h, fin, MAX_PATH)) *why = "no final path";
    if (!*why && lstrcmpiW(fin, path) != 0) *why = "resolves elsewhere";
    if (*why) { CloseHandle(h); return INVALID_HANDLE_VALUE; }
    return h;
}

/* TRUE when g_hLog is usable for g_cfg.logFile */
static BOOL LogEnsureLocked(void)
{
    WCHAR dir[MAX_PATH], *slash;
    const char *why;
    if (g_cfg.logLevel == 0) return FALSE;
    if (g_logState != LOGST_OFF && lstrcmpiW(g_openLogPath, g_cfg.logFile) == 0) return g_logState == LOGST_ACTIVE;
    LogCloseLocked();
    g_logState = LOGST_OFF;
    lstrcpynW(g_openLogPath, g_cfg.logFile, MAX_PATH);
    lstrcpynW(dir, g_cfg.logFile, MAX_PATH);
    slash = wcsrchr(dir, L'\\');
    if (!slash || !slash[1]) { LogDisableLocked("LogFile must be a full path to a file"); return FALSE; }
    *slash = 0;
    if (!OpenLogDirLocked(dir)) return FALSE;
    g_hLog = OpenSafeFileLocked(slash + 1, OPEN_ALWAYS, &why);
    if (g_hLog == INVALID_HANDLE_VALUE) {
        LogDisableLocked("log file %ls rejected: %s (error %lu)", g_cfg.logFile, why, GetLastError());
        return FALSE;
    }
    g_logState = LOGST_ACTIVE;
    g_logWhy[0] = 0;
    return TRUE;
}

static void LogWriteLocked(const char *line, DWORD n)
{
    LARGE_INTEGER zero, size;
    DWORD w;
    zero.QuadPart = 0;
    SetFilePointerEx(g_hLog, zero, NULL, FILE_END);
    WriteFile(g_hLog, line, n, &w, NULL);
    if ((++g_logLines & 255) == 0 && GetFileSizeEx(g_hLog, &size) && size.QuadPart > (LONGLONG)g_cfg.maxLogKB * 1024) {
        static const char msg[] = "--- log truncated (MaxLogKB reached) ---\r\n";
        SetFilePointerEx(g_hLog, zero, NULL, FILE_BEGIN);   /* truncate through the validated handle */
        SetEndOfFile(g_hLog);
        WriteFile(g_hLog, msg, sizeof(msg) - 1, &w, NULL);
    }
}

/* ------------------------------------------------------------------ file worker
 * The spooler calls a language monitor while impersonating the user who submitted the job.
 * Files and directories created on such a thread are owned by that user, which the owner check
 * in OpenLogDirLocked() rightly rejects, so the log never opened. All log and capture file
 * creation therefore runs on one thread created here: a new thread does not inherit its
 * creator's impersonation token, so it runs as the spooler service itself. Requests are
 * synchronous (the caller waits), which keeps log lines in order. */
typedef void (*FILEWORKFN)(void *ctx);
static CRITICAL_SECTION g_fwCs;      /* one request at a time; never taken by the worker */
static HANDLE           g_fwThread, g_fwGo, g_fwDone;
static FILEWORKFN       g_fwFn;
static void            *g_fwCtx;
static volatile LONG    g_fwStop;

static DWORD WINAPI FileWorker(LPVOID unused)
{
    UNREFERENCED_PARAMETER(unused);
    for (;;) {
        WaitForSingleObject(g_fwGo, INFINITE);
        if (g_fwStop) return 0;
        g_fwFn(g_fwCtx);
        SetEvent(g_fwDone);
    }
}

/* Runs fn(ctx) on the worker thread and waits for it. If the worker cannot be started, fn is
 * NOT run here (that would create files as the impersonated user): callers then behave as if
 * logging were unavailable. */
static void RunOnFileWorker(FILEWORKFN fn, void *ctx)
{
    EnterCriticalSection(&g_fwCs);
    if (!g_fwThread && !g_fwStop) {
        if (!g_fwGo) g_fwGo = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!g_fwDone) g_fwDone = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (g_fwGo && g_fwDone) g_fwThread = CreateThread(NULL, 0, FileWorker, NULL, 0, NULL);
    }
    if (g_fwThread && !g_fwStop) {
        g_fwFn = fn;
        g_fwCtx = ctx;
        SetEvent(g_fwGo);
        WaitForSingleObject(g_fwDone, INFINITE);
    }
    LeaveCriticalSection(&g_fwCs);
}

static void FileWorkerStop(void)
{
    EnterCriticalSection(&g_fwCs);
    g_fwStop = 1;
    if (g_fwThread) {
        SetEvent(g_fwGo);
        WaitForSingleObject(g_fwThread, 5000);
        CloseHandle(g_fwThread);
        g_fwThread = NULL;
    }
    LeaveCriticalSection(&g_fwCs);
}

typedef struct { const char *line; DWORD n; } LOGWORK;

static void LogLineWork(void *ctx)
{
    LOGWORK *w = (LOGWORK *)ctx;
    EnterCriticalSection(&g_logCs);
    if (LogEnsureLocked()) LogWriteLocked(w->line, w->n);
    LeaveCriticalSection(&g_logCs);
}

typedef struct { const WCHAR *leaf; HANDLE h; const char *why; } CAPWORK;

static void CaptureOpenWork(void *ctx)
{
    CAPWORK *w = (CAPWORK *)ctx;
    EnterCriticalSection(&g_logCs);
    if (LogEnsureLocked()) {
        w->h = OpenSafeFileLocked(w->leaf, CREATE_NEW, &w->why);
        if (w->h == INVALID_HANDLE_VALUE) w->h = NULL;
    }
    LeaveCriticalSection(&g_logCs);
}

static void LogLineV(LMPORT *p, DWORD level, const char *fmt, va_list ap)
{
    char line[2048];
    int k, m;
    LOGWORK lw;

    if (g_cfg.logLevel < level) return;
    k = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%10.3f] %ls j%lu ", NowSec(),
                    p ? p->portName : L"-", p ? (unsigned long)p->jobId : 0ul);
    if (k < 0) k = 0;
    m = _vsnprintf_s(line + k, sizeof(line) - k - 2, _TRUNCATE, fmt, ap);
    k = (m < 0) ? (int)strlen(line) : k + m;
    line[k++] = '\r';
    line[k++] = '\n';

    lw.line = line;
    lw.n = (DWORD)k;
    RunOnFileWorker(LogLineWork, &lw);
}

/* Diagnostic export: 0 = logging off, 1 = active, 2 = disabled by a safety check (reason in buf). */
DWORD WINAPI LbpLmLogStatus(LPSTR buf, DWORD cch)
{
    DWORD st;
    EnterCriticalSection(&g_logCs);
    st = (DWORD)g_logState;
    if (buf && cch) lstrcpynA(buf, st == LOGST_ACTIVE ? "active" : st == LOGST_DISABLED ? g_logWhy : "off", (int)cch);
    LeaveCriticalSection(&g_logCs);
    return st;
}

static void LogP(LMPORT *p, DWORD level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LogLineV(p, level, fmt, ap);
    va_end(ap);
}

#define LOG(p, ...)  LogP((p), 1, __VA_ARGS__)
#define LOG2(p, ...) LogP((p), 2, __VA_ARGS__)

/* "a0 e0 04 00" */
static const char *Hex(char *out, size_t cap, const BYTE *b, int n)
{
    static const char hx[] = "0123456789abcdef";
    size_t k = 0;
    int i;
    for (i = 0; i < n && k + 4 < cap; i++) {
        if (i) out[k++] = ' ';
        out[k++] = hx[b[i] >> 4];
        out[k++] = hx[b[i] & 15];
    }
    if (i < n && k + 4 < cap) { out[k++] = '.'; out[k++] = '.'; }
    out[k] = 0;
    return out;
}

/* ------------------------------------------------------------------ configuration */
static DWORD RegDword(LPCWSTR name, DWORD def)
{
    DWORD type = 0, v = 0, cb = sizeof(v);
    if (!g_reg || !g_reg->fpQueryValue || !g_hckRoot) return def;
    if (g_reg->fpQueryValue(g_hckRoot, name, &type, (PBYTE)&v, &cb, g_hSpooler) == ERROR_SUCCESS &&
        type == REG_DWORD && cb == sizeof(v))
        return v;
    return def;
}

static void LoadConfig(void)
{
    LMCONFIG c;
    WCHAR raw[MAX_PATH], pd[MAX_PATH];
    DWORD type = 0, cb = sizeof(raw);

    ZeroMemory(&c, sizeof(c));
    c.logLevel       = RegDword(L"LogLevel", 0);    /* off unless an administrator enables it */
    c.maxLogKB       = RegDword(L"MaxLogKB", 4096);
    c.captureSent    = RegDword(L"CaptureSent", 0);
    c.readTimeoutMs  = RegDword(L"ReadTimeoutMs", 2000);
    c.writeTimeoutMs = RegDword(L"WriteTimeoutMs", 30000);
    c.gateTimeoutSec = RegDword(L"GateTimeoutSec", 120);
    c.paperWaitTries = RegDword(L"PaperWaitTries", 0);     /* 0: wait for paper until cancelled */
    c.writeRetries   = RegDword(L"WriteRetries", 3);
    if (c.maxLogKB < 64) c.maxLogKB = 64;
    if (c.readTimeoutMs < 50) c.readTimeoutMs = 50;

    raw[0] = 0;
    if (g_reg && g_reg->fpQueryValue && g_hckRoot &&
        g_reg->fpQueryValue(g_hckRoot, L"LogFile", &type, (PBYTE)raw, &cb, g_hSpooler) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && cb >= 2 * sizeof(WCHAR)) {
        raw[MAX_PATH - 1] = 0;
        if (!ExpandEnvironmentStringsW(raw, c.logFile, MAX_PATH)) lstrcpynW(c.logFile, raw, MAX_PATH);
    }
    if (!c.logFile[0]) {
        if (!GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) lstrcpyW(pd, L"C:\\ProgramData");
        swprintf_s(c.logFile, MAX_PATH, L"%s\\LBP1120\\lm.log", pd);
    }
    EnterCriticalSection(&g_logCs);
    EnterCriticalSection(&g_cs);
    g_cfg = c;
    LeaveCriticalSection(&g_cs);
    if (g_logState == LOGST_DISABLED) g_logState = LOGST_OFF;   /* re-validate once per job */
    if (c.logLevel == 0) { LogCloseLocked(); g_logState = LOGST_OFF; }
    LeaveCriticalSection(&g_logCs);
}

/* spoolss.dll exports OpenPrinterW/GetJobW/SetJobW/ClosePrinter for code running inside the
 * spooler (pjlmon links spoolss.lib). Resolved dynamically so that the DLL loads in a test
 * process without a spooler: there they are simply absent (or provided by a test double). */
static void ResolveSpooler(void)
{
    HMODULE h = GetModuleHandleW(L"spoolss.dll");
    if (!h) {
        WCHAR exe[MAX_PATH], *base;
        if (GetModuleFileNameW(NULL, exe, MAX_PATH)) {
            base = wcsrchr(exe, L'\\');
            base = base ? base + 1 : exe;
            if (lstrcmpiW(base, L"spoolsv.exe") == 0)
                h = LoadLibraryExW(L"spoolss.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
    }
    if (!h) return;
    pOpenPrinterW = (PFN_OPENPRINTERW)GetProcAddress(h, "OpenPrinterW");
    pClosePrinter = (PFN_CLOSEPRINTER)GetProcAddress(h, "ClosePrinter");
    pGetJobW      = (PFN_GETJOBW)GetProcAddress(h, "GetJobW");
    pSetJobW      = (PFN_SETJOBW)GetProcAddress(h, "SetJobW");
    if (!pOpenPrinterW || !pClosePrinter || !pGetJobW || !pSetJobW)
        pOpenPrinterW = NULL, pClosePrinter = NULL, pGetJobW = NULL, pSetJobW = NULL;
    else
        pSetPortW = (PFN_SETPORTW)GetProcAddress(h, "SetPortW");
}

/* ------------------------------------------------------------------ job control helpers */
static JOB_INFO_1W *GetJob1(LMPORT *p)
{
    DWORD need = 0;
    JOB_INFO_1W *ji;
    if (!p->hPrinter || !pGetJobW) return NULL;
    pGetJobW(p->hPrinter, p->jobId, 1, NULL, 0, &need);
    if (need < sizeof(JOB_INFO_1W) || need > 1 << 20) return NULL;
    ji = (JOB_INFO_1W *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, need);
    if (!ji) return NULL;
    if (!pGetJobW(p->hPrinter, p->jobId, 1, (LPBYTE)ji, need, &need)) {
        HeapFree(GetProcessHeap(), 0, ji);
        return NULL;
    }
    return ji;
}

/* Status text shown in the queue ("Waiting for paper"...). Best effort; NULL clears it. */
static void SetJobStatusText(LMPORT *p, LPCWSTR text)
{
    JOB_INFO_1W *ji;
    if (!text && !p->statusShown) return;
    if (!pSetJobW || !(ji = GetJob1(p))) return;
    ji->pStatus = (LPWSTR)text;
    ji->Position = JOB_POSITION_UNSPECIFIED;
    if (pSetJobW(p->hPrinter, p->jobId, 1, (LPBYTE)ji, 0))
        p->statusShown = text != NULL;
    else
        LOG2(p, "SetJob(status) failed, error %lu", GetLastError());
    HeapFree(GetProcessHeap(), 0, ji);
}

/* Port status "paper out" (on = TRUE) or cleared (FALSE), shown by Windows on the printers using the
 * port. SetPort level 3 (PORT_INFO_3) is how a monitor reports this; SetPrinter(PRINTER_CONTROL_
 * SET_STATUS) was refused with error 1803 inside the spooler. Run on the worker thread (service
 * identity): the job owner the spooler impersonates is usually not an administrator.
 * Best effort: a failure only loses the indication, never the job. */
typedef struct { LPWSTR port; PORT_INFO_3W pi; BOOL ok; DWORD err; } PORTSTATWORK;

static void PortStatusWork(void *ctx)
{
    PORTSTATWORK *w = (PORTSTATWORK *)ctx;
    w->ok = pSetPortW(NULL, w->port, 3, (LPBYTE)&w->pi);
    if (!w->ok) w->err = GetLastError();
}

static void SetPaperOutStatus(LMPORT *p, BOOL on)
{
    PORTSTATWORK w;
    if (!pSetPortW || !p->portName[0]) return;
    w.port = p->portName;
    w.pi.dwStatus = on ? PORT_STATUS_PAPER_OUT : 0;
    w.pi.pszStatus = NULL;
    w.pi.dwSeverity = on ? PORT_STATUS_TYPE_ERROR : PORT_STATUS_TYPE_INFO;
    w.ok = FALSE;
    w.err = ERROR_SERVICE_NOT_ACTIVE;   /* unchanged if the worker could not run it */
    RunOnFileWorker(PortStatusWork, &w);
    if (w.ok) p->paperOutSet = on;
    else LOG(p, "SetPort(%ls, paper out %s) failed, error %lu", p->portName, on ? "on" : "off", w.err);
}

/* TRUE if the port is closing or the spooler marked the job deleted/restarted. */
static BOOL CancelRequested(LMPORT *p, BOOL force)
{
    JOB_INFO_1W *ji;
    BOOL cancel = FALSE;
    ULONGLONG now;
    if (Aborted(p)) return TRUE;
    now = NowMs();
    if (!force && now - p->lastCancelCheck < CANCEL_CHECK_MS) return FALSE;
    p->lastCancelCheck = now;
    if ((ji = GetJob1(p)) != NULL) {
        cancel = (ji->Status & (JOB_STATUS_DELETING | JOB_STATUS_DELETED | JOB_STATUS_RESTART)) != 0;
        if (cancel) LOG(p, "spooler job status 0x%08lx: job deleted/restarted", ji->Status);
        HeapFree(GetProcessHeap(), 0, ji);
    }
    return cancel;
}

/* Waits ms in slices, watching for cancellation (paper wait, gate polls). */
static BOOL WaitCancellable(LMPORT *p, DWORD ms)
{
    while (ms > 0) {
        DWORD s = ms > 100 ? 100 : ms;
        if (!LmWaitAbortable(p, s)) return FALSE;
        ms -= s;
        if (CancelRequested(p, TRUE)) return FALSE;
    }
    return TRUE;
}

static BOOL JobFail(LMPORT *p, DWORD err, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (p->state == JS_ACTIVE) {
        p->state = (err == ERROR_PRINT_CANCELLED) ? JS_CANCELLED : JS_FAILED;
        p->failErr = err;
    }
    LOG(p, "! %s: %s (error %lu)", err == ERROR_PRINT_CANCELLED ? "CANCEL" : "FAIL", msg, err);
    if (err != ERROR_PRINT_CANCELLED) {
        WCHAR w[200];
        MultiByteToWideChar(CP_ACP, 0, msg, -1, w, 200);
        w[199] = 0;
        SetJobStatusText(p, w);
    }
    return FALSE;
}

static BOOL JobCancelled(LMPORT *p)
{
    return JobFail(p, ERROR_PRINT_CANCELLED, Aborted(p) ? "port closing" : "job cancelled in the spooler");
}

/* ------------------------------------------------------------------ port monitor I/O */
static void Capture(LMPORT *p, const BYTE *b, int n)
{
    DWORD w;
    if (p->hCapture && n > 0) WriteFile(p->hCapture, b, (DWORD)n, &w, NULL);
}

/* DeviceTransport.Write: loop until everything is written; -1 if nothing could be written. */
static int PortWrite(LMPORT *p, const BYTE *b, DWORD len, DWORD *err)
{
    DWORD tot = 0;
    *err = 0;
    while (tot < len) {
        DWORD n = 0;
        if (!p->fn.pfnWritePort(p->hPort, (LPBYTE)b + tot, len - tot, &n)) {
            *err = GetLastError();
            if (n == 0) break;
        }
        if (n == 0) { if (!*err) *err = ERROR_WRITE_FAULT; break; }
        tot += n;
        if (*err) break;
    }
    p->bytesOut += tot;
    Capture(p, b, (int)tot);
    return tot > 0 ? (int)tot : -1;
}

/* One status transfer. usbmon may block in ReadPort (timeouts from SetPortTimeOuts) or return
 * 0 bytes immediately: either way, poll until data or ReadTimeoutMs (captsend: 2000 ms, then -1). */
static int PortRead(LMPORT *p, BYTE *b, DWORD cap, char *why, size_t whyCap)
{
    ULONGLONG deadline = NowMs() + p->cfg.readTimeoutMs;
    DWORD lastErr = 0, calls = 0;
    for (;;) {
        DWORD n = 0;
        BOOL ok = p->fn.pfnReadPort(p->hPort, b, cap, &n);
        calls++;
        if (ok && n > 0) return (int)(n > cap ? cap : n);
        if (!ok) lastErr = GetLastError();
        if (NowMs() >= deadline) {
            _snprintf_s(why, whyCap, _TRUNCATE, "no reply within %lu ms (%lu ReadPort calls, last error %lu)",
                        p->cfg.readTimeoutMs, calls, lastErr);
            return -1;
        }
        if (!LmWaitAbortable(p, 10)) {
            _snprintf_s(why, whyCap, _TRUNCATE, "aborted (port closing)");
            return -1;
        }
    }
}

/* ------------------------------------------------------------------ CAPT conversation */

/* Number of status transfers captsend reads after each command (WriteCommandPacket nread). */
static int ReadsFor(BYTE a, BYTE b, BOOL *known)
{
    *known = TRUE;
    if (a == 0xa1 && b == 0xa1) return 2;   /* identity: must answer a1 a1                     */
    if (a == 0xa0 && b == 0xa0) return 2;   /* status                                          */
    if (a == 0xa1 && b == 0xa0) return 2;   /* status, body[10] = 0xfd paper / 0x00 tray empty */
    if (a == 0xa0 && b == 0xa2) return 1;
    if (a == 0xa0 && b == 0xe0) return 1;   /* status, header[4] & 0x08 = busy                 */
    if (a == 0xa2 && b == 0xe0) return 1;   /* attention clear (Canon cap3emn)                   */
    if (a == 0xa3 && b == 0xe0) return 1;   /* Canon cap3emn, cancel only                         */
    if (a == 0xa6 && b == 0xe0) return 1;   /* stop (Canon cap3emn), cancel only                  */
    if (a == 0xa9 && b == 0xe0) return 1;   /* job end (Canon cap3emn), cancel only               */
    if (a == 0xa4 && b == 0xe0) return 1;
    if (a == 0xa5 && b == 0xe0) return 1;   /* + 8-byte payload                                */
    if (a == 0xa0 && b == 0xd0) return 0;   /* page parameters, 34-byte payload                */
    if (a == 0xa1 && b == 0xd0) return 0;   /* start of page                                   */
    if (a == 0xa2 && b == 0xd0) return 0;   /* end of page                                     */
    *known = FALSE;
    return 0;
}

/* WriteCommandPacketBuf(): write, 8 ms, then nread x (read, 8 ms). */
static BOOL SendCommand(LMPORT *p, const BYTE *buf, DWORD len)
{
    char hx[800], why[160];
    BOOL known, split = FALSE;
    int reads = ReadsFor(buf[0], buf[1], &known), n, j;
    DWORD err;

    if (!known) LOG(p, "warning: unknown opcode %02x %02x, forwarded without reads", buf[0], buf[1]);
    n = PortWrite(p, buf, len, &err);
    p->nCmd++;
    if (n == -1) LOG(p, "w: n=%d : %s   (WritePort error %lu)", n, Hex(hx, sizeof(hx), buf, (int)len), err);
    else         LOG(p, "w: n=%d : %s", n, Hex(hx, sizeof(hx), buf, (int)len));
    if (n != (int)len)
        return JobFail(p, err ? err : ERROR_WRITE_FAULT, "command write failed (%d/%lu bytes), device gone ?", n, len);
    LmSleep(p, WAIT_MS);

    for (j = 0; j < reads; j++) {
        if (j == 1 && split) continue;
        ZeroMemory(p->reply[j], LM_REPLY_SIZE);
        n = PortRead(p, p->reply[j], LM_REPLY_SIZE, why, sizeof(why));
        if (n == -1) LOG(p, "r: n=-1 %s: ", why);
        else         LOG(p, "r: n=%d : %s", n, Hex(hx, sizeof(hx), p->reply[j], n));
        LmSleep(p, WAIT_MS);
        if (j == 0 && reads == 2 && n > 6) {
            /* header and body arrived in ONE transfer (not seen on the LBP-1120 or usbprint, but
             * possible through another port monitor): split instead of waiting 2 s for a body. */
            int body = n - 6;
            CopyMemory(p->reply[1], p->reply[0] + 6, body);
            ZeroMemory(p->reply[0] + 6, body);
            split = TRUE;
            LOG(p, "r: (header+body in one transfer, split: body %d bytes)", body);
        }
    }
    p->lastOp[0] = buf[0];
    p->lastOp[1] = buf[1];
    if (Aborted(p)) return JobCancelled(p);
    return TRUE;
}

static BOOL SendCommand2(LMPORT *p, BYTE a, BYTE b)
{
    BYTE c[4];
    c[0] = a; c[1] = b; c[2] = 4; c[3] = 0;
    return SendCommand(p, c, 4);
}

/* WaitForPaper(): an a0 a0 + a1 a0 pair has just been sent; repeat it once a second until
 * a1 a0 body byte[10] == 0xfd. PaperWaitTries attempts in all, or until the job is cancelled when
 * PaperWaitTries is 0 (default). While waiting, the printer shows "paper out". */
/* a0 a0 status, then note the engine's "no paper" flag (body[5] & 0x02): on the LBP-1120 it was set
 * 3 s after the previous sheet left, once the printer tried to feed the already started page with
 * an empty tray, and cleared when paper was loaded. */
static BOOL StatusA0A0(LMPORT *p)
{
    if (!SendCommand2(p, 0xa0, 0xa0)) return FALSE;
    if (p->reply[1][5] & 0x02) {
        if (!p->engineNoPaper) LOG(p, "printer reports a paper feed error (a0 a0 body[5] = %02x)", p->reply[1][5]);
        p->engineNoPaper = TRUE;
    }
    return TRUE;
}

static BOOL WaitForPaper(LMPORT *p, const char *when)
{
    DWORD i;
    p->paperWaited = FALSE;
    LOG(p, "paper check (%s): a1 a0 body[10] = %02x", when, p->reply[1][10]);
    for (i = 0;; i++) {
        if (p->reply[1][10] == 0xfd) {
            if (i > 0) {
                LOG(p, "paper present");
                SetJobStatusText(p, NULL);
                if (p->paperOutSet) SetPaperOutStatus(p, FALSE);
                p->paperWaited = TRUE;
            }
            return TRUE;
        }
        if (i == 0) {
            LOG(p, "Waiting for paper...");
            SetJobStatusText(p, L"Plus de papier : chargez du papier (LBP-1120)");
            SetPaperOutStatus(p, TRUE);
        }
        if (!WaitCancellable(p, PAPER_RETRY_MS)) return JobCancelled(p);
        if (p->cfg.paperWaitTries && i + 1 >= p->cfg.paperWaitTries)
            return JobFail(p, ERROR_OUT_OF_PAPER, "Timeout out while waiting for paper.");
        if (!StatusA0A0(p)) return FALSE;
        if (!SendCommand2(p, 0xa1, 0xa0)) return FALSE;
    }
}

/* The gate before each data packet: a0 e0 until reply[4] & 0x08 == 0 (and a reply came). */
static BOOL Gate(LMPORT *p)
{
    ULONGLONG start;
    BOOL shown = FALSE;
    if (!(p->lastOp[0] == 0xa0 && p->lastOp[1] == 0xe0)) {
        LOG(p, "warning: data packet not preceded by a0 e0 in the stream; sending one for the gate");
        if (!SendCommand2(p, 0xa0, 0xe0)) return FALSE;
    }
    start = NowMs();
    while ((p->reply[0][4] & 0x08) == 0x08 || p->reply[0][0] == 0x00) {
        if (NowMs() - start > (ULONGLONG)p->cfg.gateTimeoutSec * 1000)
            return JobFail(p, ERROR_TIMEOUT,
                           "Printer never reported ready (a0 e0 reply byte[4] & 0x08 still set or no reply) after %lu s. "
                           "Aborting before data packet %lu.", p->cfg.gateTimeoutSec, p->pageIndex);
        if (!WaitCancellable(p, GATE_POLL_MS)) return JobCancelled(p);
        p->nPoll++;
        if (!shown && NowMs() - start > 3000) {
            SetJobStatusText(p, L"Imprimante occup\x00e9" L"e : attente (LBP-1120)");
            shown = TRUE;
        }
        if (!SendCommand2(p, 0xa0, 0xe0)) return FALSE;
    }
    if (shown) SetJobStatusText(p, NULL);
    LmSleep(p, WAIT_CHUNK_MS);
    return TRUE;
}

/* SendChunk(): retry up to WriteRetries times when nothing was written; a partial write aborts. */
static BOOL SendChunk(LMPORT *p, const BYTE *pk, DWORD i, DWORD len, DWORD size)
{
    DWORD c = 0, err;
    int w;
    do {
        w = PortWrite(p, pk + i, len, &err);
        if (w == -1) LOG(p, "w: n=%d/%lu WritePort error %lu (i=%lu/%lu)", w, len, err, i, size);
        else         LOG(p, "w: n=%d/%lu  (i=%lu/%lu)", w, len, i, size);
        LmSleep(p, WAIT_CHUNK_MS);
        if (w == -1 && c >= p->cfg.writeRetries)
            return JobFail(p, err ? err : ERROR_WRITE_FAULT, "Error while sending data, timeout.");
        c++;
    } while (w == -1);
    if (w != (int)len)
        return JobFail(p, ERROR_WRITE_FAULT, "Partial data write (%d/%lu), aborting: the stream would be corrupt.", w, len);
    p->nChunk++;
    return TRUE;
}

/* Paper check right before a page's first data packet, once the printer is ready (gate open).
 * capt.c checks paper only at page start, while the previous sheet may still be in the tray. On
 * hardware (3 pages, 2 sheets) page 3 passed that check with the last sheet still in the tray; the
 * sheet was picked 1.4 s later (a0 e0 reply byte[5] 0x0d -> 0x0f), page 3 was then sent to an
 * empty printer and lost, and loading paper did not bring it back. So pages after the first, and
 * any page whose last a0 e0 reply has byte[5] & 0x02 (seen with an empty tray), re-check here.
 * Status commands only; a single-page job keeps captsend's exact byte stream.
 *
 * The page was already started (a1 d0) when this check runs. On hardware the printer raised its
 * feed error (a0 a0 body[5] & 0x02) while waiting, and after paper was loaded it took the page's
 * data without printing it, even with the page re-opened: after the feed error the a0 a0 body is
 * 02 40 80 .., the printer's idle state before a job, i.e. it dropped the whole job. So when that
 * error was seen, the job is restarted with print_page(0)'s init (a0 a2 | a0 e0 | a1 a0 | a4 e0 |
 * a5 e0 + ee db ea ad 00 00 00 00 | a0 e0 | a0 a0 | a1 a0 | a0 e0) plus, right after a0 a2, the
 * a2 e0 that Canon's own engine (cap3emn) sends there and to clear the a0 a0 body[1] & 0x4f
 * attention state before re-sending a page after a paper-out; then the page is re-opened with the
 * stream's own page-start sequence (a0 d0 + params | a0 e0 | a1 d0 | a0 e0 | a0 a0 | a1 a0),
 * before its data. */
static BOOL PaperBeforeData(LMPORT *p)
{
    p->engineNoPaper = FALSE;
    if (!StatusA0A0(p)) return FALSE;
    if (!SendCommand2(p, 0xa1, 0xa0)) return FALSE;
    if (!WaitForPaper(p, "before page data")) return FALSE;
    if (p->paperWaited && p->engineNoPaper && p->pageParamsLen) {
        static const BYTE a5e0[12] = { 0xa5, 0xe0, 0x0c, 0x00, 0xee, 0xdb, 0xea, 0xad, 0, 0, 0, 0 };
        LOG(p, "job dropped by the printer while out of paper: restarting it (a0 a2 | a2 e0 .. a5 e0) and re-opening the page (a0 d0 | a1 d0)");
        if (!SendCommand2(p, 0xa0, 0xa2)) return FALSE;
        if (!SendCommand2(p, 0xa2, 0xe0)) return FALSE;
        if (!SendCommand2(p, 0xa0, 0xe0)) return FALSE;
        if (!SendCommand2(p, 0xa1, 0xa0)) return FALSE;
        if (!SendCommand2(p, 0xa4, 0xe0)) return FALSE;
        if (!SendCommand(p, a5e0, sizeof(a5e0))) return FALSE;
        if (!SendCommand2(p, 0xa0, 0xe0)) return FALSE;
        if (!SendCommand2(p, 0xa0, 0xa0)) return FALSE;
        if (!SendCommand2(p, 0xa1, 0xa0)) return FALSE;
        if (!SendCommand2(p, 0xa0, 0xe0)) return FALSE;
        if (!SendCommand(p, p->pageParams, p->pageParamsLen)) return FALSE;
        if (!SendCommand2(p, 0xa0, 0xe0)) return FALSE;
        if (!SendCommand2(p, 0xa1, 0xd0)) return FALSE;
        if (!SendCommand2(p, 0xa0, 0xe0)) return FALSE;
        if (!SendCommand2(p, 0xa0, 0xa0)) return FALSE;
        if (!SendCommand2(p, 0xa1, 0xa0)) return FALSE;
    }
    if (!SendCommand2(p, 0xa0, 0xe0)) return FALSE;   /* the printer may be busy again: re-gate */
    return Gate(p);
}

static BOOL SendData(LMPORT *p)
{
    DWORD size = p->pktNeed, i;
    if (!Gate(p)) return FALSE;
    if (p->pageIndex == 0 && (p->pagesDone > 0 || (p->reply[0][5] & 0x02)) && !PaperBeforeData(p))
        return FALSE;
    LOG(p, "size: %lu, index: %lu", size, p->pageIndex);
    /* once the first chunk is out, the packet is always completed (never leave the printer
     * with a truncated data frame), even if the port is being closed meanwhile */
    for (i = 0; i + LM_CHUNK < size; i += LM_CHUNK)
        if (!SendChunk(p, p->pkt, i, LM_CHUNK, size)) return FALSE;
    if (!SendChunk(p, p->pkt, i, size - i, size)) return FALSE;
    p->nPacket++;
    p->pageIndex++;
    p->lastOp[0] = 0xa0;
    p->lastOp[1] = 0xc0;
    LmSleep(p, WAIT_PACKET_MS);
    if (Aborted(p)) return JobCancelled(p);
    return TRUE;
}

static BOOL ProcessPacket(LMPORT *p)
{
    BYTE a = p->pkt[0], b = p->pkt[1];
    BOOL afterA0A0 = (p->lastOp[0] == 0xa0 && p->lastOp[1] == 0xa0);

    if (CancelRequested(p, FALSE)) return JobCancelled(p);

    if (a == 0xa0 && b == 0xc0) return SendData(p);

    if (!SendCommand(p, p->pkt, p->pktNeed)) return FALSE;

    if (a == 0xa1 && b == 0xa1) {
        /* print_page(0) only */
        if (p->reply[0][0] != 0xa1 || p->reply[0][1] != 0xa1)
            return JobFail(p, ERROR_NOT_READY, "Invalid printer state, printer not connected ?");
    } else if (a == 0xa1 && b == 0xa0 && afterA0A0 && p->paperPending) {
        /* waitforpaper(): every print_page(n) opens with an a0 a0 | a1 a0 pair (after a1 a1 on
         * page 0, right after the previous page's a2 d0 .. a0 e0 | a1 a0 tail on pages n > 0).
         * Armed at StartDocPort and by each a2 d0; the first a0 a0 | a1 a0 pair consumes it, so the
         * first-page init pair and the post-a1 d0 pair are never taken for paper checks. */
        p->paperPending = FALSE;
        if (!WaitForPaper(p, "page start")) return FALSE;
    } else if (a == 0xa0 && b == 0xa2) {
        p->jobOpen = TRUE;
    } else if (a == 0xa0 && b == 0xd0) {
        /* page parameters: kept to re-open the page after a paper-out (PaperBeforeData) */
        p->pageParamsLen = p->pktNeed <= sizeof(p->pageParams) ? p->pktNeed : 0;
        if (p->pageParamsLen) CopyMemory(p->pageParams, p->pkt, p->pageParamsLen);
    } else if (a == 0xa1 && b == 0xd0) {
        p->midPage = TRUE;
        p->pageIndex = 0;
        p->paperPending = FALSE;   /* a page started without a paper check (contract deviation) */
    } else if (a == 0xa2 && b == 0xd0) {
        p->midPage = FALSE;
        p->pagesDone++;
        p->paperPending = TRUE;    /* the next page, if any, starts with its paper check */
    }
    return TRUE;
}

/* ------------------------------------------------------------------ port handle management */
static LMPORT *PortRef(HANDLE h)
{
    LMPORT *p;
    EnterCriticalSection(&g_cs);
    for (p = g_ports; p; p = p->next)
        if (p == (LMPORT *)h && p->sig == LMPORT_SIG && !p->closing) { p->refs++; break; }
    LeaveCriticalSection(&g_cs);
    return p;
}

static void PortFree(LMPORT *p)
{
    if (p->hTimer) CloseHandle(p->hTimer);
    if (p->hAbort) CloseHandle(p->hAbort);
    DeleteCriticalSection(&p->cs);
    p->sig = 0;
    HeapFree(GetProcessHeap(), 0, p);
}

static void PortUnref(LMPORT *p)
{
    LONG r;
    EnterCriticalSection(&g_cs);
    r = --p->refs;
    LeaveCriticalSection(&g_cs);
    if (r == 0) PortFree(p);   /* only reachable after ClosePort unlinked it */
}

static void EndJobLocked(LMPORT *p, const char *why)
{
    const char *res = p->state == JS_ACTIVE ? "DONE" : p->state == JS_CANCELLED ? "CANCELLED" : "FAILED";
    if (p->pktHave) LOG(p, "incomplete packet discarded at end of job (%lu of %lu bytes, opcode %02x %02x)",
                        p->pktHave, p->pktHave >= 4 ? p->pktNeed : 0, p->pkt[0], p->pktHave > 1 ? p->pkt[1] : 0);
    if (p->midPage) LOG(p, "warning: job ended between a1 d0 and a2 d0 (page not closed on the printer)");
    LOG(p, "%s (%s): %lu commands (%lu gate polls), %lu data packets / %lu data writes, %I64u bytes in, %I64u bytes sent, %lu WritePort calls",
        res, why, p->nCmd, p->nPoll, p->nPacket, p->nChunk, p->bytesIn, p->bytesOut, p->nWritePort);
    if (p->hCapture) { CloseHandle(p->hCapture); p->hCapture = NULL; }
    if (p->paperOutSet) SetPaperOutStatus(p, FALSE);   /* cancelled or failed while waiting for paper */
    if (p->hPrinter) {
        if (p->state == JS_ACTIVE) SetJobStatusText(p, NULL);
        /* no bidi job tracking: tell the spooler the job is finished (pjlmon does the same) */
        if (pSetJobW && !pSetJobW(p->hPrinter, p->jobId, 0, NULL, JOB_CONTROL_LAST_PAGE_EJECTED))
            LOG2(p, "SetJob(LAST_PAGE_EJECTED) failed, error %lu", GetLastError());
        pClosePrinter(p->hPrinter);
        p->hPrinter = NULL;
    }
    p->state = JS_NONE;
    p->pktHave = p->pktNeed = 0;
}

/* ------------------------------------------------------------------ MONITOR2 entry points */
static BOOL WINAPI LmOpenPortEx(HANDLE hMonitor, HANDLE hMonitorPort, LPWSTR pPortName, LPWSTR pPrinterName,
                                PHANDLE pHandle, struct _MONITOR2 *pMonitor2)
{
    LMPORT *p;
    DWORD cb;
    UNREFERENCED_PARAMETER(hMonitor);

    if (!pHandle || !pPortName || !pMonitor2) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *pHandle = NULL;
    if (!pMonitor2->pfnOpenPort || !pMonitor2->pfnStartDocPort || !pMonitor2->pfnWritePort ||
        !pMonitor2->pfnReadPort || !pMonitor2->pfnEndDocPort || !pMonitor2->pfnClosePort) {
        LOG(NULL, "OpenPortEx(%ls): port monitor lacks Open/StartDoc/Write/Read/EndDoc/ClosePort", pPortName);
        SetLastError(ERROR_INVALID_PRINT_MONITOR);
        return FALSE;
    }
    p = (LMPORT *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(LMPORT));
    if (!p) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    p->sig = LMPORT_SIG;
    p->refs = 1;
    InitializeCriticalSection(&p->cs);
    p->hAbort = CreateEventW(NULL, TRUE, FALSE, NULL);
    p->hTimer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!p->hTimer) p->hTimer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    cb = pMonitor2->cbSize;
    if (cb < MONITOR2_SIZE_WIN2K) cb = MONITOR2_SIZE_WIN2K;
    if (cb > sizeof(MONITOR2)) cb = sizeof(MONITOR2);
    CopyMemory(&p->fn, pMonitor2, cb);
    lstrcpynW(p->portName, pPortName, 64);
    if (pPrinterName) lstrcpynW(p->printerName, pPrinterName, 260);
    EnterCriticalSection(&g_cs);
    p->cfg = g_cfg;
    LeaveCriticalSection(&g_cs);
    if (!p->hAbort) { PortFree(p); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }

    if (!p->fn.pfnOpenPort(hMonitorPort, pPortName, &p->hPort)) {
        DWORD e = GetLastError();
        LOG(p, "OpenPortEx: port monitor OpenPort(%ls) failed, error %lu", pPortName, e);
        PortFree(p);
        SetLastError(e);
        return FALSE;
    }
    EnterCriticalSection(&g_cs);
    p->next = g_ports;
    g_ports = p;
    LeaveCriticalSection(&g_cs);
    LOG(p, "OpenPortEx: port %ls, printer %ls, high-res timer %s", pPortName,
        pPrinterName ? pPrinterName : L"(null)", p->hTimer ? "yes" : "no");
    *pHandle = (HANDLE)p;
    return TRUE;
}

static BOOL WINAPI LmStartDocPort(HANDLE hPort, LPWSTR pPrinterName, DWORD JobId, DWORD Level, LPBYTE pDocInfo)
{
    LMPORT *p = PortRef(hPort);
    BOOL ok;
    DWORD e = 0;
    if (!p) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    LoadConfig();   /* registry changes (e.g. LogLevel) apply from the next job */
    EnterCriticalSection(&p->cs);
    if (p->state != JS_NONE) EndJobLocked(p, "StartDocPort without EndDocPort");
    EnterCriticalSection(&g_cs);
    p->cfg = g_cfg;
    LeaveCriticalSection(&g_cs);

    p->jobId = JobId;
    p->failErr = 0;
    p->drainLogged = p->statusShown = p->midPage = p->jobOpen = FALSE;
    p->paperPending = TRUE;     /* the job's first a0 a0 | a1 a0 pair is page 0's paper check */
    p->paperOutSet = FALSE;
    p->pagesDone = 0;
    p->pageParamsLen = 0;
    p->pktHave = p->pktNeed = 0;
    p->lastOp[0] = p->lastOp[1] = 0;
    ZeroMemory(p->reply, sizeof(p->reply));
    p->pageIndex = p->nCmd = p->nPoll = p->nChunk = p->nPacket = p->nWritePort = 0;
    p->bytesIn = p->bytesOut = 0;
    p->lastCancelCheck = 0;
    ResetEvent(p->hAbort);

    ok = p->fn.pfnStartDocPort(p->hPort, pPrinterName, JobId, Level, pDocInfo);
    if (!ok) {
        e = GetLastError();
        LOG(p, "StartDocPort: port monitor StartDocPort failed, error %lu", e);
    } else {
        COMMTIMEOUTS cto;
        if (p->fn.pfnSetPortTimeOuts) {
            ZeroMemory(&cto, sizeof(cto));
            cto.ReadTotalTimeoutConstant = p->cfg.readTimeoutMs;
            cto.WriteTotalTimeoutConstant = p->cfg.writeTimeoutMs;
            if (!p->fn.pfnSetPortTimeOuts(p->hPort, &cto, 0))
                LOG(p, "SetPortTimeOuts failed, error %lu (reads rely on the port monitor's own timeout)", GetLastError());
        } else {
            LOG(p, "port monitor has no SetPortTimeOuts (reads rely on its own timeout)");
        }
        if (pOpenPrinterW && pPrinterName && !pOpenPrinterW(pPrinterName, &p->hPrinter, NULL)) {
            LOG(p, "OpenPrinter(%ls) failed, error %lu: no cancel detection / status text", pPrinterName, GetLastError());
            p->hPrinter = NULL;
        }
        if (p->cfg.captureSent) {
            /* only into the validated log directory, never overwriting: CREATE_NEW + unique name */
            WCHAR leaf[96];
            CAPWORK cw;
            swprintf_s(leaf, 96, L"job%lu-%lu-%I64u.sent.bin", JobId, GetCurrentProcessId(), GetTickCount64());
            cw.leaf = leaf;
            cw.h = NULL;
            cw.why = "logging is not active";
            RunOnFileWorker(CaptureOpenWork, &cw);   /* created as the service, not as the job owner */
            p->hCapture = cw.h;
            if (p->hCapture) LOG(p, "capturing the bytes sent to %ls\\%ls", g_logDirFinal, leaf);
            else LOG(p, "CaptureSent ignored: %s", cw.why);
        }
        p->state = JS_ACTIVE;
        LOG(p, "StartDocPort: %s, printer %ls, job %lu (read timeout %lu ms, gate %lu s, paper %lu tries, spooler job control %s)",
            LM_VERSION, pPrinterName ? pPrinterName : L"(null)", JobId, p->cfg.readTimeoutMs, p->cfg.gateTimeoutSec,
            p->cfg.paperWaitTries, p->hPrinter ? "yes" : "no");
    }
    LeaveCriticalSection(&p->cs);
    PortUnref(p);
    if (!ok) SetLastError(e);
    return ok;
}

static BOOL WINAPI LmWritePort(HANDLE hPort, LPBYTE pBuffer, DWORD cbBuf, LPDWORD pcbWritten)
{
    LMPORT *p = PortRef(hPort);
    BOOL ok = TRUE;
    DWORD off = 0, e = 0;

    if (!p) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (pcbWritten) *pcbWritten = 0;
    EnterCriticalSection(&p->cs);
    p->nWritePort++;
    LOG2(p, "WritePort(%lu bytes)", cbBuf);

    if (p->state == JS_NONE) {
        /* outside StartDocPort/EndDocPort (not expected from the spooler): plain pass-through */
        ok = p->fn.pfnWritePort(p->hPort, pBuffer, cbBuf, pcbWritten);
        e = ok ? 0 : GetLastError();
    } else if (p->state == JS_CANCELLED) {
        ok = FALSE;
        e = ERROR_PRINT_CANCELLED;
    } else if (p->state == JS_FAILED) {
        /* the job already failed (and returned an error once): swallow the rest so that the
         * spooler finishes the job instead of retrying forever; nothing more reaches the printer */
        if (!p->drainLogged) { LOG(p, "job failed earlier: draining the remaining data"); p->drainLogged = TRUE; }
        p->bytesIn += cbBuf;
        if (pcbWritten) *pcbWritten = cbBuf;
    } else {
        while (off < cbBuf) {
            DWORD take;
            if (p->pktHave < 4) {
                take = min(4 - p->pktHave, cbBuf - off);
                CopyMemory(p->pkt + p->pktHave, pBuffer + off, take);
                p->pktHave += take;
                off += take;
                if (p->pktHave < 4) break;
                p->pktNeed = p->pkt[2] | ((DWORD)p->pkt[3] << 8);
                if (p->pktNeed < 4) {
                    char hx[16];
                    ok = JobFail(p, ERROR_INVALID_DATA, "stream out of sync: packet header %s has length %lu < 4",
                                 Hex(hx, sizeof(hx), p->pkt, 4), p->pktNeed);
                    break;
                }
            }
            take = min(p->pktNeed - p->pktHave, cbBuf - off);
            CopyMemory(p->pkt + p->pktHave, pBuffer + off, take);
            p->pktHave += take;
            off += take;
            if (p->pktHave == p->pktNeed) {
                BOOL r = ProcessPacket(p);
                p->pktHave = p->pktNeed = 0;
                if (!r) { ok = FALSE; break; }
            }
        }
        p->bytesIn += off;
        if (ok) {
            if (pcbWritten) *pcbWritten = cbBuf;
        } else {
            e = p->failErr ? p->failErr : ERROR_GEN_FAILURE;
            if (pcbWritten) *pcbWritten = off;
        }
    }
    LeaveCriticalSection(&p->cs);
    PortUnref(p);
    if (!ok) SetLastError(e);
    return ok;
}

static BOOL WINAPI LmReadPort(HANDLE hPort, LPBYTE pBuffer, DWORD cbBuffer, LPDWORD pcbRead)
{
    LMPORT *p = PortRef(hPort);
    BOOL ok;
    DWORD e;
    if (!p) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    EnterCriticalSection(&p->cs);   /* never interleaves with the job's own status reads */
    ok = p->fn.pfnReadPort(p->hPort, pBuffer, cbBuffer, pcbRead);
    e = GetLastError();
    LeaveCriticalSection(&p->cs);
    PortUnref(p);
    SetLastError(e);
    return ok;
}

/* A job cancelled in the spooler leaves the printer inside it (often between a1 d0 and a2 d0),
 * and capt.c has no way to stop it. Canon's engine (cap3emn) cancels with a6 e0, polls a0 a0 until
 * the engine is idle (body[0] & 0x84 == 0), clears its conditions (a3 e0 if body[4] & 0xc0 and not
 * body[0] & 0x04; a2 e0 if body[1] & 0x4f) and ends the job with a9 e0: do the same, at EndDocPort,
 * while the port is still ours. Not done when the port is closing (device gone or spooler stop). */
static void CancelOnPrinter(LMPORT *p)
{
    ULONGLONG start;
    if (p->state != JS_CANCELLED || !p->jobOpen || Aborted(p)) return;
    LOG(p, "job cancelled: stopping the printer (a6 e0 | a0 a0 until idle | a2 e0 | a9 e0)");
    if (!SendCommand2(p, 0xa6, 0xe0)) return;
    start = NowMs();
    for (;;) {
        if (!SendCommand2(p, 0xa0, 0xa0)) return;
        if (!(p->reply[1][0] & 0x84)) break;
        if (NowMs() - start > (ULONGLONG)p->cfg.gateTimeoutSec * 1000) {
            LOG(p, "printer still busy after %lu s (a0 a0 body[0] = %02x), ending the job anyway",
                p->cfg.gateTimeoutSec, p->reply[1][0]);
            break;
        }
        if (!LmWaitAbortable(p, GATE_POLL_MS * 5)) return;
    }
    if ((p->reply[1][4] & 0xc0) && !(p->reply[1][0] & 0x04) && !SendCommand2(p, 0xa3, 0xe0)) return;
    if ((p->reply[1][1] & 0x4f) && !SendCommand2(p, 0xa2, 0xe0)) return;
    if (!SendCommand2(p, 0xa9, 0xe0)) return;
    p->midPage = p->jobOpen = FALSE;
}

static BOOL WINAPI LmEndDocPort(HANDLE hPort)
{
    LMPORT *p = PortRef(hPort);
    BOOL ok;
    DWORD e;
    if (!p) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    EnterCriticalSection(&p->cs);
    CancelOnPrinter(p);
    ok = p->fn.pfnEndDocPort(p->hPort);
    e = GetLastError();
    if (p->state != JS_NONE) EndJobLocked(p, "EndDocPort");
    LeaveCriticalSection(&p->cs);
    PortUnref(p);
    SetLastError(e);
    return ok;
}

static BOOL WINAPI LmClosePort(HANDLE hPort)
{
    LMPORT *p, **pp;
    BOOL ok;
    EnterCriticalSection(&g_cs);
    for (pp = &g_ports; *pp; pp = &(*pp)->next)
        if (*pp == (LMPORT *)hPort) break;
    p = *pp;
    if (!p || p->sig != LMPORT_SIG || p->closing) {
        LeaveCriticalSection(&g_cs);
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    p->closing = TRUE;
    *pp = p->next;        /* no new calls can find it */
    p->refs++;
    LeaveCriticalSection(&g_cs);

    SetEvent(p->hAbort);  /* unblocks a WritePort waiting for paper / ready / a reply */
    EnterCriticalSection(&p->cs);
    if (p->state != JS_NONE) {
        p->fn.pfnEndDocPort(p->hPort);
        EndJobLocked(p, "ClosePort during a job");
    }
    ok = p->fn.pfnClosePort(p->hPort);
    LOG(p, "ClosePort");
    LeaveCriticalSection(&p->cs);
    PortUnref(p);         /* our temporary reference */
    PortUnref(p);         /* the OpenPortEx reference: frees once in-flight calls have left */
    return ok;
}

static BOOL WINAPI LmGetPrinterDataFromPort(HANDLE hPort, DWORD ControlID, LPWSTR pValueName, LPWSTR lpInBuffer,
                                            DWORD cbInBuffer, LPWSTR lpOutBuffer, DWORD cbOutBuffer, LPDWORD lpcbReturned)
{
    LMPORT *p = PortRef(hPort);
    BOOL ok = FALSE;
    DWORD e = ERROR_NOT_SUPPORTED;
    if (!p) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (p->fn.pfnGetPrinterDataFromPort) {   /* e.g. IEEE-1284 device ID queries: pass through */
        EnterCriticalSection(&p->cs);
        ok = p->fn.pfnGetPrinterDataFromPort(p->hPort, ControlID, pValueName, lpInBuffer, cbInBuffer,
                                             lpOutBuffer, cbOutBuffer, lpcbReturned);
        e = GetLastError();
        LeaveCriticalSection(&p->cs);
    }
    PortUnref(p);
    SetLastError(e);
    return ok;
}

static BOOL WINAPI LmSetPortTimeOuts(HANDLE hPort, LPCOMMTIMEOUTS lpCTO, DWORD reserved)
{
    LMPORT *p = PortRef(hPort);
    BOOL ok = FALSE;
    DWORD e = ERROR_NOT_SUPPORTED;
    if (!p) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (p->fn.pfnSetPortTimeOuts) {
        EnterCriticalSection(&p->cs);
        ok = p->fn.pfnSetPortTimeOuts(p->hPort, lpCTO, reserved);
        e = GetLastError();
        LeaveCriticalSection(&p->cs);
    }
    PortUnref(p);
    SetLastError(e);
    return ok;
}

static VOID WINAPI LmShutdown(HANDLE hMonitor)
{
    UNREFERENCED_PARAMETER(hMonitor);
    for (;;) {
        HANDLE h;
        EnterCriticalSection(&g_cs);
        h = (HANDLE)g_ports;
        LeaveCriticalSection(&g_cs);
        if (!h) break;
        LmClosePort(h);
    }
    LOG(NULL, "Shutdown");
    FileWorkerStop();
    EnterCriticalSection(&g_logCs);
    LogCloseLocked();
    LeaveCriticalSection(&g_logCs);
}

static MONITOR2 g_mon2 = {
    sizeof(MONITOR2),
    NULL,                       /* EnumPorts      - language monitor: none */
    NULL,                       /* OpenPort       */
    LmOpenPortEx,
    LmStartDocPort,
    LmWritePort,
    LmReadPort,
    LmEndDocPort,
    LmClosePort,
    NULL,                       /* AddPort        */
    NULL,                       /* AddPortEx      */
    NULL,                       /* ConfigurePort  */
    NULL,                       /* DeletePort     */
    LmGetPrinterDataFromPort,
    LmSetPortTimeOuts,
    NULL,                       /* XcvOpenPort    */
    NULL,                       /* XcvDataPort    */
    NULL,                       /* XcvClosePort   */
    LmShutdown,
    NULL,                       /* SendRecvBidiDataFromPort */
    NULL,                       /* NotifyUsedPorts   */
    NULL,                       /* NotifyUnusedPorts */
    NULL                        /* PowerEvent        */
};

LPMONITOR2 WINAPI InitializePrintMonitor2(PMONITORINIT pMonitorInit, PHANDLE phMonitor)
{
    WCHAR exe[MAX_PATH];
    if (!pMonitorInit || !phMonitor) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    g_hSpooler = pMonitorInit->hSpooler;
    g_hckRoot = pMonitorInit->hckRegistryRoot;
    g_reg = (pMonitorInit->pMonitorReg && pMonitorInit->pMonitorReg->cbSize >= sizeof(MONITORREG))
            ? pMonitorInit->pMonitorReg : NULL;
    InitSelfSid();
    EnterCriticalSection(&g_fwCs);
    g_fwStop = 0;                   /* the spooler may re-initialize the monitor after Shutdown */
    LeaveCriticalSection(&g_fwCs);
    LoadConfig();
    ResolveSpooler();
    *phMonitor = (HANDLE)&g_monitorToken;
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) exe[0] = 0;
    LOG(NULL, "InitializePrintMonitor2: %s (built " __DATE__ " " __TIME__ "), host %ls, local %d, registry %s, spooler job control %s",
        LM_VERSION, exe, pMonitorInit->bLocal, g_reg ? "yes" : "no", pGetJobW ? "yes" : "no");
    return &g_mon2;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        InitializeCriticalSection(&g_cs);
        InitializeCriticalSection(&g_logCs);
        InitializeCriticalSection(&g_fwCs);
        QueryPerformanceFrequency(&g_qpf);
        QueryPerformanceCounter(&g_t0);
        g_cfg.logLevel = 0;
    }
    return TRUE;
}
