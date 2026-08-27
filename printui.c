/*
 * PrintUI — Win32 GUI wrapper for rundll32 printui.dll,PrintUIEntry
 *
 * MinGW (Linux cross):
 *   x86_64-w64-mingw32-windres printui.rc -O coff -o printui.res && x86_64-w64-mingw32-gcc -O2 -s -static -static-libgcc -mwindows -municode -o PrintUI.exe printui.c printui.res -lcomctl32 -lcomdlg32 -lshell32 -luser32 -lkernel32 -lgdi32
 *
 * MSVC:
 *   rc printui.rc && cl /nologo /O2 /MT /DUNICODE /D_UNICODE printui.c printui.res user32.lib comctl32.lib comdlg32.lib shell32.lib kernel32.lib gdi32.lib /Fe:PrintUI.exe
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <wchar.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "printui.h"

#ifndef ERROR_CANCELLED
#define ERROR_CANCELLED 1223L
#endif

#define FIELD_CAP   (MAX_PATH * 2)
#define CMD_CAP     4096
#define STATUS_CAP  640

typedef enum {
    OP_NONE = 0,
    OP_ADD_NET,
    OP_DEL_NET,
    OP_DEL_LOCAL,
    OP_IF,
    OP_IA,
    OP_DD,
    OP_Y,
    OP_K,
    OP_P,
    OP_O,
    OP_SS,
    OP_SR,
    OP_RAW
} Op;

static Op g_last_op = OP_NONE;

static void trim(wchar_t *s)
{
    wchar_t *a = s;
    size_t n;
    if (!s)
        return;
    while (*a == L' ' || *a == L'\t')
        a++;
    if (a != s)
        memmove(s, a, (wcslen(a) + 1) * sizeof(wchar_t));
    n = wcslen(s);
    while (n && (s[n - 1] == L' ' || s[n - 1] == L'\t'))
        s[--n] = 0;
}

static void get_field(HWND dlg, int id, wchar_t *buf, int cap)
{
    GetDlgItemTextW(dlg, id, buf, cap);
    buf[cap - 1] = 0;
    trim(buf);
}

static BOOL is_unc(const wchar_t *name)
{
    const wchar_t *slash;
    if (!name || name[0] != L'\\' || name[1] != L'\\')
        return FALSE;
    slash = wcschr(name + 2, L'\\');
    return slash && slash[1] != 0;
}

static const wchar_t *bad_named_char(const wchar_t *s)
{
    for (; *s; s++) {
        if (*s == L'"' || *s == L'&' || *s == L'|' ||
            *s == L'\n' || *s == L'\r' || *s == L'%')
            return s;
    }
    return NULL;
}

static const wchar_t *bad_raw_char(const wchar_t *s)
{
    for (; *s; s++) {
        if (*s == L'&' || *s == L'|' || *s == L'\n' || *s == L'\r')
            return s;
    }
    return NULL;
}

static void describe_char(wchar_t c, wchar_t *out, size_t cap)
{
    if (c == L'\n')
        _snwprintf(out, cap, L"newline");
    else if (c == L'\r')
        _snwprintf(out, cap, L"carriage return");
    else if (c == L'"')
        _snwprintf(out, cap, L"double-quote (\")");
    else
        _snwprintf(out, cap, L"'%c'", c);
    out[cap - 1] = 0;
}

/* Returns the field label if a named field contains a forbidden character. */
static const wchar_t *invalid_named_field(HWND dlg, wchar_t *badch)
{
    static const struct { int id; const wchar_t *name; } fields[] = {
        { IDC_PRINTER,  L"printer name" },
        { IDC_SERVER,   L"server" },
        { IDC_MODEL,    L"driver model" },
        { IDC_PORT,     L"port" },
        { IDC_INF,      L"INF file" },
        { IDC_SETTINGS, L"settings file" },
    };
    size_t i;
    wchar_t buf[FIELD_CAP];
    const wchar_t *p;

    if (badch)
        *badch = 0;
    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        get_field(dlg, fields[i].id, buf, FIELD_CAP);
        p = bad_named_char(buf);
        if (p) {
            if (badch)
                *badch = *p;
            return fields[i].name;
        }
    }
    return NULL;
}

static BOOL append_str(wchar_t *buf, size_t cap, const wchar_t *s)
{
    size_t used = wcslen(buf);
    size_t add = wcslen(s);
    if (used + add + 1 > cap)
        return FALSE;
    memcpy(buf + used, s, (add + 1) * sizeof(wchar_t));
    return TRUE;
}

static BOOL append_flag(wchar_t *buf, size_t cap, const wchar_t *flag)
{
    if (buf[0] && !append_str(buf, cap, L" "))
        return FALSE;
    return append_str(buf, cap, flag);
}

static BOOL append_quoted_switch(wchar_t *buf, size_t cap, const wchar_t *sw, const wchar_t *value)
{
    if (!value || !value[0])
        return TRUE;
    if (buf[0] && !append_str(buf, cap, L" "))
        return FALSE;
    if (!append_str(buf, cap, sw))
        return FALSE;
    if (!append_str(buf, cap, L"\""))
        return FALSE;
    if (!append_str(buf, cap, value))
        return FALSE;
    return append_str(buf, cap, L"\"");
}

static void make_unc(wchar_t *out, size_t cap, const wchar_t *server, const wchar_t *name)
{
    const wchar_t *s = server;
    if (name[0] == L'\\' && name[1] == L'\\') {
        wcsncpy(out, name, cap - 1);
        out[cap - 1] = 0;
        return;
    }
    while (*s == L'\\')
        s++;
    if (*s)
        _snwprintf(out, cap, L"\\\\%s\\%s", s, name);
    else {
        wcsncpy(out, name, cap - 1);
        out[cap - 1] = 0;
    }
    out[cap - 1] = 0;
}

static void set_status(HWND dlg, const wchar_t *text)
{
    SetDlgItemTextW(dlg, IDC_STATUS, text);
}

static void get_rundll32(wchar_t *path, size_t cap)
{
    UINT n = GetSystemDirectoryW(path, (UINT)cap);
    if (n == 0 || n + 14 >= cap) {
        wcsncpy(path, L"C:\\Windows\\System32\\rundll32.exe", cap - 1);
        path[cap - 1] = 0;
        return;
    }
    wcscat(path, L"\\rundll32.exe");
}

static void report_fail(HWND dlg, DWORD err)
{
    wchar_t msg[400];
    wchar_t line[STATUS_CAP];
    DWORD n;

    if (err == ERROR_CANCELLED) {
        wcscpy(line, L"UAC cancelled (ERROR_CANCELLED 1223)");
        set_status(dlg, line);
        MessageBoxW(dlg, line, L"PrintUI", MB_OK | MB_ICONWARNING);
        return;
    }
    n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, err, 0, msg, 400, NULL);
    if (n) {
        trim(msg);
        _snwprintf(line, STATUS_CAP, L"Error %lu: %s", (unsigned long)err, msg);
    } else {
        _snwprintf(line, STATUS_CAP, L"Error %lu", (unsigned long)err);
    }
    line[STATUS_CAP - 1] = 0;
    set_status(dlg, line);
    MessageBoxW(dlg, line, L"PrintUI", MB_OK | MB_ICONERROR);
}

static BOOL compose(HWND dlg, Op op, wchar_t *args, size_t cap, wchar_t *err, size_t ecap)
{
    wchar_t printer[FIELD_CAP], server[FIELD_CAP], model[FIELD_CAP], port[FIELD_CAP];
    wchar_t inf[FIELD_CAP], settings[FIELD_CAP], raw[CMD_CAP], unc[FIELD_CAP];
    BOOL quiet, used_m = FALSE, used_r = FALSE, skip_q = FALSE, skip_c = FALSE;
    BOOL extra_mr = TRUE;
    wchar_t badch = 0;
    const wchar_t *badfield;

    args[0] = 0;
    if (err)
        err[0] = 0;

    get_field(dlg, IDC_PRINTER, printer, FIELD_CAP);
    get_field(dlg, IDC_SERVER, server, FIELD_CAP);
    get_field(dlg, IDC_MODEL, model, FIELD_CAP);
    get_field(dlg, IDC_PORT, port, FIELD_CAP);
    get_field(dlg, IDC_INF, inf, FIELD_CAP);
    get_field(dlg, IDC_SETTINGS, settings, FIELD_CAP);
    get_field(dlg, IDC_RAW, raw, CMD_CAP);
    quiet = (IsDlgButtonChecked(dlg, IDC_QUIET) == BST_CHECKED);

    badfield = invalid_named_field(dlg, &badch);
    if (badfield) {
        if (err)
            _snwprintf(err, ecap, L"invalid character in %s", badfield);
        return FALSE;
    }

    if (op == OP_RAW) {
        if (bad_raw_char(raw)) {
            if (err)
                _snwprintf(err, ecap, L"invalid character in raw arguments");
            return FALSE;
        }
        return append_str(args, cap, raw);
    }

    switch (op) {
    case OP_ADD_NET:
        skip_c = TRUE;
        if (!append_flag(args, cap, L"/in"))
            goto overflow;
        make_unc(unc, FIELD_CAP, server, printer);
        if (!append_quoted_switch(args, cap, L"/n", unc))
            goto overflow;
        break;
    case OP_DEL_NET:
        skip_c = TRUE;
        if (!append_flag(args, cap, L"/dn"))
            goto overflow;
        make_unc(unc, FIELD_CAP, server, printer);
        if (!append_quoted_switch(args, cap, L"/n", unc))
            goto overflow;
        break;
    case OP_DEL_LOCAL:
        if (!append_flag(args, cap, L"/dl"))
            goto overflow;
        if (!append_quoted_switch(args, cap, L"/n", printer))
            goto overflow;
        break;
    case OP_IF:
        used_m = TRUE;
        used_r = TRUE;
        if (!append_flag(args, cap, L"/if"))
            goto overflow;
        if (!append_quoted_switch(args, cap, L"/b", printer))
            goto overflow;
        if (!append_quoted_switch(args, cap, L"/f", inf))
            goto overflow;
        if (!append_quoted_switch(args, cap, L"/r", port))
            goto overflow;
        if (!append_quoted_switch(args, cap, L"/m", model))
            goto overflow;
        break;
    case OP_IA:
        used_m = TRUE;
        if (!append_flag(args, cap, L"/ia"))
            goto overflow;
        if (!append_quoted_switch(args, cap, L"/m", model))
            goto overflow;
        if (!append_quoted_switch(args, cap, L"/f", inf))
            goto overflow;
        break;
    case OP_DD:
        used_m = TRUE;
        if (!append_flag(args, cap, L"/dd"))
            goto overflow;
        if (!append_quoted_switch(args, cap, L"/m", model))
            goto overflow;
        break;
    case OP_Y:
        if (!append_flag(args, cap, L"/y"))
            goto overflow;
        if (!append_quoted_switch(args, cap, L"/n", printer))
            goto overflow;
        break;
    case OP_K:
        if (!append_flag(args, cap, L"/k"))
            goto overflow;
        if (!append_quoted_switch(args, cap, L"/n", printer))
            goto overflow;
        break;
    case OP_P:
        skip_q = TRUE;
        if (!append_flag(args, cap, L"/p"))
            goto overflow;
        if (!append_quoted_switch(args, cap, L"/n", printer))
            goto overflow;
        break;
    case OP_O:
        skip_q = TRUE;
        if (!append_flag(args, cap, L"/o"))
            goto overflow;
        if (!append_quoted_switch(args, cap, L"/n", printer))
            goto overflow;
        break;
    case OP_SS:
        if (!append_flag(args, cap, L"/Ss"))
            goto overflow;
        if (!append_quoted_switch(args, cap, L"/n", printer))
            goto overflow;
        if (!append_quoted_switch(args, cap, L"/a", settings))
            goto overflow;
        break;
    case OP_SR:
        if (!append_flag(args, cap, L"/Sr"))
            goto overflow;
        if (!append_quoted_switch(args, cap, L"/n", printer))
            goto overflow;
        if (!append_quoted_switch(args, cap, L"/a", settings))
            goto overflow;
        break;
    case OP_NONE:
    default:
        extra_mr = FALSE;
        skip_c = TRUE;
        if (printer[0] && !append_quoted_switch(args, cap, L"/n", printer))
            goto overflow;
        if (server[0] && !append_quoted_switch(args, cap, L"/c", server))
            goto overflow;
        if (model[0] && !append_quoted_switch(args, cap, L"/m", model))
            goto overflow;
        if (port[0] && !append_quoted_switch(args, cap, L"/r", port))
            goto overflow;
        if (inf[0] && !append_quoted_switch(args, cap, L"/f", inf))
            goto overflow;
        if (settings[0] && !append_quoted_switch(args, cap, L"/a", settings))
            goto overflow;
        break;
    }

    if (!skip_c && server[0] && !append_quoted_switch(args, cap, L"/c", server))
        goto overflow;
    if (extra_mr) {
        if (!used_m && model[0] && !append_quoted_switch(args, cap, L"/m", model))
            goto overflow;
        if (!used_r && port[0] && !append_quoted_switch(args, cap, L"/r", port))
            goto overflow;
    }
    if (!skip_q && quiet && !append_flag(args, cap, L"/q"))
        goto overflow;
    return TRUE;

overflow:
    if (err)
        _snwprintf(err, ecap, L"command line too long");
    return FALSE;
}

static void build_preview(HWND dlg, Op op)
{
    wchar_t rundll[MAX_PATH], args[CMD_CAP], err[STATUS_CAP], cmd[CMD_CAP];

    get_rundll32(rundll, MAX_PATH);
    if (!compose(dlg, op, args, CMD_CAP, err, STATUS_CAP)) {
        SetDlgItemTextW(dlg, IDC_PREVIEW, err[0] ? err : L"invalid input");
        if (err[0])
            set_status(dlg, err);
        return;
    }
    if (args[0])
        _snwprintf(cmd, CMD_CAP, L"\"%s\" printui.dll,PrintUIEntry %s", rundll, args);
    else
        _snwprintf(cmd, CMD_CAP, L"\"%s\" printui.dll,PrintUIEntry", rundll);
    cmd[CMD_CAP - 1] = 0;
    SetDlgItemTextW(dlg, IDC_PREVIEW, cmd);
}

static void update_buttons(HWND dlg)
{
    wchar_t printer[FIELD_CAP], server[FIELD_CAP], model[FIELD_CAP], port[FIELD_CAP];
    wchar_t inf[FIELD_CAP], settings[FIELD_CAP], raw[CMD_CAP];
    BOOL has_printer, has_server, has_model, has_port, has_inf, has_settings, has_raw;
    BOOL valid, net, raw_ok;
    wchar_t badch = 0;

    get_field(dlg, IDC_PRINTER, printer, FIELD_CAP);
    get_field(dlg, IDC_SERVER, server, FIELD_CAP);
    get_field(dlg, IDC_MODEL, model, FIELD_CAP);
    get_field(dlg, IDC_PORT, port, FIELD_CAP);
    get_field(dlg, IDC_INF, inf, FIELD_CAP);
    get_field(dlg, IDC_SETTINGS, settings, FIELD_CAP);
    get_field(dlg, IDC_RAW, raw, CMD_CAP);

    valid = (invalid_named_field(dlg, &badch) == NULL);
    has_printer = printer[0] != 0;
    has_server = server[0] != 0;
    has_model = model[0] != 0;
    has_port = port[0] != 0;
    has_inf = inf[0] != 0;
    has_settings = settings[0] != 0;
    has_raw = raw[0] != 0;
    net = valid && (is_unc(printer) || (has_printer && has_server));
    raw_ok = valid && has_raw && !bad_raw_char(raw);

    EnableWindow(GetDlgItem(dlg, IDC_ADD_NET), net);
    EnableWindow(GetDlgItem(dlg, IDC_DEL_NET), net);
    EnableWindow(GetDlgItem(dlg, IDC_DEL_LOCAL), valid && has_printer);
    EnableWindow(GetDlgItem(dlg, IDC_Y), valid && has_printer);
    EnableWindow(GetDlgItem(dlg, IDC_K), valid && has_printer);
    EnableWindow(GetDlgItem(dlg, IDC_P), valid && has_printer);
    EnableWindow(GetDlgItem(dlg, IDC_O), valid && has_printer);
    EnableWindow(GetDlgItem(dlg, IDC_SS), valid && has_printer && has_settings);
    EnableWindow(GetDlgItem(dlg, IDC_SR), valid && has_printer && has_settings);
    EnableWindow(GetDlgItem(dlg, IDC_IF), valid && has_printer && has_inf && has_port && has_model);
    EnableWindow(GetDlgItem(dlg, IDC_IA), valid && has_model && has_inf);
    EnableWindow(GetDlgItem(dlg, IDC_DD), valid && has_model);
    EnableWindow(GetDlgItem(dlg, IDC_RAW_RUN), raw_ok);
}

static void refresh(HWND dlg)
{
    update_buttons(dlg);
    build_preview(dlg, g_last_op);
}

static BOOL browse_file(HWND dlg, int dest_id, BOOL inf, BOOL save)
{
    wchar_t file[FIELD_CAP];
    OPENFILENAMEW ofn;

    get_field(dlg, dest_id, file, FIELD_CAP);
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dlg;
    ofn.lpstrFile = file;
    ofn.nMaxFile = FIELD_CAP;
    ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    if (inf) {
        ofn.lpstrFilter = L"INF files (*.inf)\0*.inf\0All files (*.*)\0*.*\0";
        ofn.lpstrDefExt = L"inf";
        ofn.Flags |= OFN_FILEMUSTEXIST;
        if (!GetOpenFileNameW(&ofn))
            return FALSE;
    } else {
        ofn.lpstrFilter = L"Settings (*.dat)\0*.dat\0All files (*.*)\0*.*\0";
        ofn.lpstrDefExt = L"dat";
        if (save) {
            ofn.Flags |= OFN_OVERWRITEPROMPT;
            if (!GetSaveFileNameW(&ofn))
                return FALSE;
        } else {
            ofn.Flags |= OFN_FILEMUSTEXIST;
            if (!GetOpenFileNameW(&ofn))
                return FALSE;
        }
    }
    SetDlgItemTextW(dlg, dest_id, file);
    return TRUE;
}

static void launch(HWND dlg, Op op, BOOL elevate)
{
    wchar_t rundll[MAX_PATH], args[CMD_CAP], err[STATUS_CAP];
    wchar_t cmdline[CMD_CAP], params[CMD_CAP];
    wchar_t msg[512], chdesc[64], badch = 0;
    const wchar_t *badfield;
    DWORD code;

    g_last_op = op;
    build_preview(dlg, op);

    badfield = invalid_named_field(dlg, &badch);
    if (badfield) {
        describe_char(badch, chdesc, 64);
        _snwprintf(msg, 512,
                   L"The %s field contains a forbidden character: %s\r\n\r\n"
                   L"These characters are not allowed: \"  &  |  %%  and line breaks.",
                   badfield, chdesc);
        msg[511] = 0;
        MessageBoxW(dlg, msg, L"PrintUI", MB_OK | MB_ICONWARNING);
        return;
    }
    if (op == OP_RAW) {
        wchar_t raw[CMD_CAP];
        get_field(dlg, IDC_RAW, raw, CMD_CAP);
        if (bad_raw_char(raw)) {
            MessageBoxW(dlg,
                        L"Raw arguments contain a forbidden character (&, |, or a line break).\r\n"
                        L"Quotes are allowed. Refusing to launch.",
                        L"PrintUI", MB_OK | MB_ICONWARNING);
            return;
        }
    }

    if (!compose(dlg, op, args, CMD_CAP, err, STATUS_CAP)) {
        MessageBoxW(dlg, err[0] ? err : L"Failed to compose arguments.",
                    L"PrintUI", MB_OK | MB_ICONWARNING);
        return;
    }

    get_rundll32(rundll, MAX_PATH);
    if (args[0])
        _snwprintf(cmdline, CMD_CAP, L"\"%s\" printui.dll,PrintUIEntry %s", rundll, args);
    else
        _snwprintf(cmdline, CMD_CAP, L"\"%s\" printui.dll,PrintUIEntry", rundll);
    cmdline[CMD_CAP - 1] = 0;
    SetDlgItemTextW(dlg, IDC_PREVIEW, cmdline);

    if (!elevate) {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        memset(&pi, 0, sizeof(pi));
        if (!CreateProcessW(rundll, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            report_fail(dlg, GetLastError());
            return;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        _snwprintf(err, STATUS_CAP, L"exit code %lu", (unsigned long)code);
        err[STATUS_CAP - 1] = 0;
        set_status(dlg, err);
        return;
    }

    {
        SHELLEXECUTEINFOW sei;
        memset(&sei, 0, sizeof(sei));
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.hwnd = dlg;
        sei.lpVerb = L"runas";
        sei.lpFile = rundll;
        _snwprintf(params, CMD_CAP, L"printui.dll,PrintUIEntry %s", args);
        params[CMD_CAP - 1] = 0;
        sei.lpParameters = params;
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei)) {
            report_fail(dlg, GetLastError());
            return;
        }
        if (!sei.hProcess) {
            set_status(dlg, L"elevated launch returned no process handle");
            return;
        }
        WaitForSingleObject(sei.hProcess, INFINITE);
        code = 0;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        _snwprintf(err, STATUS_CAP, L"exit code %lu", (unsigned long)code);
        err[STATUS_CAP - 1] = 0;
        set_status(dlg, err);
    }
}

static INT_PTR CALLBACK DlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        SendDlgItemMessageW(dlg, IDC_PRINTER, EM_SETLIMITTEXT, FIELD_CAP - 1, 0);
        SendDlgItemMessageW(dlg, IDC_SERVER, EM_SETLIMITTEXT, FIELD_CAP - 1, 0);
        SendDlgItemMessageW(dlg, IDC_MODEL, EM_SETLIMITTEXT, FIELD_CAP - 1, 0);
        SendDlgItemMessageW(dlg, IDC_PORT, EM_SETLIMITTEXT, FIELD_CAP - 1, 0);
        SendDlgItemMessageW(dlg, IDC_INF, EM_SETLIMITTEXT, FIELD_CAP - 1, 0);
        SendDlgItemMessageW(dlg, IDC_SETTINGS, EM_SETLIMITTEXT, FIELD_CAP - 1, 0);
        SendDlgItemMessageW(dlg, IDC_RAW, EM_SETLIMITTEXT, CMD_CAP - 1, 0);
        SendDlgItemMessageW(dlg, IDC_DEL_LOCAL, BCM_SETSHIELD, 0, TRUE);
        SendDlgItemMessageW(dlg, IDC_IF, BCM_SETSHIELD, 0, TRUE);
        SendDlgItemMessageW(dlg, IDC_IA, BCM_SETSHIELD, 0, TRUE);
        SendDlgItemMessageW(dlg, IDC_DD, BCM_SETSHIELD, 0, TRUE);
        refresh(dlg);
        set_status(dlg, L"Ready");
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_PRINTER:
        case IDC_SERVER:
        case IDC_MODEL:
        case IDC_PORT:
        case IDC_INF:
        case IDC_SETTINGS:
        case IDC_RAW:
            if (HIWORD(wParam) == EN_CHANGE)
                refresh(dlg);
            return TRUE;
        case IDC_QUIET:
            if (HIWORD(wParam) == BN_CLICKED)
                refresh(dlg);
            return TRUE;
        case IDC_INF_BROWSE:
            if (browse_file(dlg, IDC_INF, TRUE, FALSE))
                refresh(dlg);
            return TRUE;
        case IDC_SETTINGS_BROWSE:
            /* GetOpenFileNameW without FILEMUSTEXIST so a new .dat can be typed. */
            {
                wchar_t file[FIELD_CAP];
                OPENFILENAMEW ofn;
                get_field(dlg, IDC_SETTINGS, file, FIELD_CAP);
                memset(&ofn, 0, sizeof(ofn));
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = dlg;
                ofn.lpstrFile = file;
                ofn.nMaxFile = FIELD_CAP;
                ofn.lpstrFilter = L"Settings (*.dat)\0*.dat\0All files (*.*)\0*.*\0";
                ofn.lpstrDefExt = L"dat";
                ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    SetDlgItemTextW(dlg, IDC_SETTINGS, file);
                    refresh(dlg);
                }
            }
            return TRUE;
        case IDC_ADD_NET:   launch(dlg, OP_ADD_NET, FALSE); return TRUE;
        case IDC_DEL_NET:   launch(dlg, OP_DEL_NET, FALSE); return TRUE;
        case IDC_DEL_LOCAL: launch(dlg, OP_DEL_LOCAL, TRUE); return TRUE;
        case IDC_IF:        launch(dlg, OP_IF, TRUE); return TRUE;
        case IDC_IA:        launch(dlg, OP_IA, TRUE); return TRUE;
        case IDC_DD:        launch(dlg, OP_DD, TRUE); return TRUE;
        case IDC_Y:         launch(dlg, OP_Y, FALSE); return TRUE;
        case IDC_K:         launch(dlg, OP_K, FALSE); return TRUE;
        case IDC_P:         launch(dlg, OP_P, FALSE); return TRUE;
        case IDC_O:         launch(dlg, OP_O, FALSE); return TRUE;
        case IDC_SS:
            if (browse_file(dlg, IDC_SETTINGS, FALSE, TRUE))
                launch(dlg, OP_SS, FALSE);
            return TRUE;
        case IDC_SR:
            if (browse_file(dlg, IDC_SETTINGS, FALSE, FALSE))
                launch(dlg, OP_SR, FALSE);
            return TRUE;
        case IDC_RAW_RUN:   launch(dlg, OP_RAW, FALSE); return TRUE;
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(dlg, 0);
        return TRUE;
    }
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    INITCOMMONCONTROLSEX icc;
    (void)prev;
    (void)cmd;
    (void)show;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);
    DialogBoxW(inst, MAKEINTRESOURCEW(IDD_MAIN), NULL, DlgProc);
    return 0;
}
