/*
 * tlsr_serial.c - Win32 serial transport for the Blue Pill bridge.
 *
 * Copyright (C) 2026  telink_project contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The bridge enumerates as a USB CDC virtual COM port, so the "baud rate" is
 * meaningless -- the data never touches a UART.  We still set a line
 * configuration because Windows requires a valid DCB, but changing it has no
 * effect on throughput.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "tlsr_core.h"
#include "tlsr_internal.h"

/* One error string per process is enough for a single-threaded CLI and for the
 * GUI, which serialises every bridge operation onto one worker thread. */
static char g_error[512] = "";

void tlsr_set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof g_error, fmt, ap);
    va_end(ap);
}

const char *tlsr_last_error(void)
{
    return g_error[0] ? g_error : "no error";
}

/* Turn a Windows error code into something a user can act on. */
static void set_win_error(const char *what, DWORD err)
{
    char *msg = NULL;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err,
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   (LPSTR)&msg, 0, NULL);
    if (msg) {
        char *nl = strpbrk(msg, "\r\n");
        if (nl) *nl = 0;
        tlsr_set_error("%s: %s (error %lu)", what, msg, (unsigned long)err);
        LocalFree(msg);
    } else {
        tlsr_set_error("%s: Windows error %lu", what, (unsigned long)err);
    }
}

tlsr_dev *tlsr_open(const char *port)
{
    char path[64];
    tlsr_dev *d;
    HANDLE h;
    DCB dcb;
    COMMTIMEOUTS to;

    if (!port || !*port) {
        tlsr_set_error("no COM port given");
        return NULL;
    }

    /* "COM10" and above must be opened through the \\.\ namespace; without the
     * prefix CreateFile fails with a confusing "file not found". */
    if (port[0] == '\\')
        snprintf(path, sizeof path, "%s", port);
    else
        snprintf(path, sizeof path, "\\\\.\\%s", port);

    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED)
            tlsr_set_error("%s is already open in another program "
                           "(close the GUI, a terminal, or Device Manager)", port);
        else if (e == ERROR_FILE_NOT_FOUND)
            tlsr_set_error("%s does not exist - is the bridge plugged in?", port);
        else
            set_win_error(port, e);
        return NULL;
    }

    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb)) {
        set_win_error("GetCommState", GetLastError());
        CloseHandle(h);
        return NULL;
    }
    dcb.BaudRate = CBR_115200;        /* ignored by a CDC device */
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = dcb.fInX = FALSE;
    if (!SetCommState(h, &dcb)) {
        set_win_error("SetCommState", GetLastError());
        CloseHandle(h);
        return NULL;
    }

    /* Read timeouts are set per call in tlsr_read_exact(); start with a value
     * that cannot hang if something goes wrong before then. */
    to.ReadIntervalTimeout         = 0;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.ReadTotalTimeoutConstant    = 2000;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 2000;
    SetCommTimeouts(h, &to);

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    d = (tlsr_dev *)calloc(1, sizeof *d);
    if (!d) {
        tlsr_set_error("out of memory");
        CloseHandle(h);
        return NULL;
    }
    d->h = h;
    snprintf(d->port, sizeof d->port, "%s", port);
    return d;
}

void tlsr_close(tlsr_dev *d)
{
    if (!d)
        return;
    if (d->h && d->h != INVALID_HANDLE_VALUE)
        CloseHandle(d->h);
    free(d);
}

int tlsr_write_all(tlsr_dev *d, const uint8_t *data, uint32_t n)
{
    DWORD done = 0;
    while (done < n) {
        DWORD wrote = 0;
        if (!WriteFile(d->h, data + done, n - done, &wrote, NULL)) {
            set_win_error("write to bridge", GetLastError());
            return TLSR_E_PORT;
        }
        if (wrote == 0) {
            tlsr_set_error("write to bridge stalled");
            return TLSR_E_TIMEOUT;
        }
        done += wrote;
    }
    return TLSR_OK;
}

int tlsr_read_exact(tlsr_dev *d, uint8_t *out, uint32_t n, unsigned timeout_ms)
{
    COMMTIMEOUTS to;
    DWORD done = 0;

    /* A total timeout for the whole read is what we want: the bridge either
     * answers a command promptly or something is wrong. */
    to.ReadIntervalTimeout         = 0;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.ReadTotalTimeoutConstant    = timeout_ms;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 2000;
    SetCommTimeouts(d->h, &to);

    while (done < n) {
        DWORD got = 0;
        if (!ReadFile(d->h, out + done, n - done, &got, NULL)) {
            set_win_error("read from bridge", GetLastError());
            return TLSR_E_PORT;
        }
        if (got == 0) {
            tlsr_set_error("bridge did not answer: wanted %u bytes, got %u "
                           "after %u ms", n, (unsigned)done, timeout_ms);
            return TLSR_E_TIMEOUT;
        }
        done += got;
    }
    return TLSR_OK;
}

void tlsr_purge(tlsr_dev *d)
{
    PurgeComm(d->h, PURGE_RXCLEAR);
}

int tlsr_list_ports(char names[][16], int max_names)
{
    HKEY key;
    int count = 0;
    DWORD i;

    /* HARDWARE\DEVICEMAP\SERIALCOMM is the cheapest enumeration available and
     * needs no SetupAPI.  Entries can be stale if a USB device was removed
     * uncleanly, so a listed port is not a guarantee that it will open. */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0,
                      KEY_READ, &key) != ERROR_SUCCESS)
        return 0;

    for (i = 0; count < max_names; i++) {
        char value_name[256];
        BYTE data[256];
        DWORD name_len = sizeof value_name, data_len = sizeof data, type = 0;

        if (RegEnumValueA(key, i, value_name, &name_len, NULL, &type,
                          data, &data_len) != ERROR_SUCCESS)
            break;
        if (type != REG_SZ)
            continue;
        data[sizeof data - 1] = 0;
        snprintf(names[count], 16, "%s", (char *)data);
        count++;
    }
    RegCloseKey(key);
    return count;
}
