/*
 * PrintUI — Win32 GUI wrapper for rundll32 printui.dll,PrintUIEntry
 *
 * MinGW (Linux cross):
 *   x86_64-w64-mingw32-windres printui.rc -O coff -o printui.res && x86_64-w64-mingw32-gcc -O2 -s -static -static-libgcc -mwindows -municode -o PrintUI.exe printui.c printui.res -lcomctl32 -lcomdlg32 -lshell32 -luser32 -lkernel32 -lgdi32 -lwinspool
 *
 * MSVC:
 *   rc printui.rc && cl /nologo /O2 /MT /DUNICODE /D_UNICODE printui.c printui.res user32.lib comctl32.lib comdlg32.lib shell32.lib kernel32.lib gdi32.lib winspool.lib /Fe:PrintUI.exe
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0600
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <winspool.h>
#include <wchar.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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
static wchar_t g_last_enum_server[FIELD_CAP];
static BOOL g_filling_from_list = FALSE;

/* Campus buckets — a printer is placed by the prefix of its (share) name.
 *   UN..    -> Union
 *   MSMS..  -> Maxine Smith
 *   WH..    -> Whitehaven
 *   MA..    -> Macon   (MAAB, MAAC, MAAA, MAFR, MATH, ...)
 * Anything else falls into Other. */
#define GRP_UNION       1
#define GRP_MAXINE      2
#define GRP_WHITEHAVEN  3
#define GRP_MACON       4
#define GRP_OTHER       5
#define GRP_COUNT       5
static int g_group_counts[GRP_COUNT + 1];

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

static void canon_server(wchar_t *out, size_t cap, const wchar_t *in)
{
    const wchar_t *s = in ? in : L"";
    wchar_t host[FIELD_CAP];
    size_t n;

    while (*s == L'\\')
        s++;
    wcsncpy(host, s, FIELD_CAP - 1);
    host[FIELD_CAP - 1] = 0;
    n = wcslen(host);
    while (n && host[n - 1] == L'\\')
        host[--n] = 0;
    if (!n)
        wcsncpy(out, L"\\\\pcut1", cap - 1);
    else
        _snwprintf(out, cap, L"\\\\%s", host);
    out[cap - 1] = 0;
}


#define PC_MODEL L"PaperCut Global PostScript - NTNS"

static BOOL file_exists(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL looks_like_papercut_inf(const wchar_t *path)
{
    const wchar_t *base;
    if (!path || !path[0])
        return TRUE;
    base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    return _wcsicmp(base, L"PCGlobal.inf") == 0;
}

static BOOL find_papercut_inf(HWND dlg, wchar_t *out, size_t cap)
{
    wchar_t typed[FIELD_CAP], host[FIELD_CAP];
    const wchar_t *s;
    static const wchar_t *local[] = {
        L"C:\\Program Files\\PaperCut NG\\providers\\print\\drivers\\global\\win\\PC-Global-Print-Driver\\PCGlobal.inf",
        L"C:\\Program Files\\PaperCut MF\\providers\\print\\drivers\\global\\win\\PC-Global-Print-Driver\\PCGlobal.inf",
        L"C:\\Program Files\\PaperCut NG\\providers\\print\\drivers\\global\\win\\PCGlobal.inf",
        L"C:\\Program Files\\PaperCut MF\\providers\\print\\drivers\\global\\win\\PCGlobal.inf",
        L"C:\\Program Files (x86)\\PaperCut NG\\providers\\print\\drivers\\global\\win\\PC-Global-Print-Driver\\PCGlobal.inf",
        L"C:\\Program Files (x86)\\PaperCut MF\\providers\\print\\drivers\\global\\win\\PC-Global-Print-Driver\\PCGlobal.inf",
        L"C:\\Program Files\\PaperCut Print Deploy Client\\PC-Global-Print-Driver\\PCGlobal.inf",
    };
    size_t i;
    wchar_t cand[FIELD_CAP];

    get_field(dlg, IDC_SERVER, typed, FIELD_CAP);
    canon_server(host, FIELD_CAP, typed[0] ? typed : L"pcut1");
    s = host;
    while (*s == L'\\')
        s++;

    _snwprintf(cand, FIELD_CAP, L"\\\\%s\\PCClient\\win\\PC-Global-Print-Driver\\PCGlobal.inf", s);
    cand[FIELD_CAP - 1] = 0;
    if (file_exists(cand)) {
        wcsncpy(out, cand, cap - 1);
        out[cap - 1] = 0;
        return TRUE;
    }
    _snwprintf(cand, FIELD_CAP, L"\\\\%s\\PCClient\\win\\PCGlobal.inf", s);
    cand[FIELD_CAP - 1] = 0;
    if (file_exists(cand)) {
        wcsncpy(out, cand, cap - 1);
        out[cap - 1] = 0;
        return TRUE;
    }
    for (i = 0; i < sizeof(local) / sizeof(local[0]); i++) {
        if (file_exists(local[i])) {
            wcsncpy(out, local[i], cap - 1);
            out[cap - 1] = 0;
            return TRUE;
        }
    }
    _snwprintf(out, cap, L"\\\\%s\\PCClient\\win\\PC-Global-Print-Driver\\PCGlobal.inf", s);
    out[cap - 1] = 0;
    return FALSE;
}

static void apply_papercut_defaults(HWND dlg, BOOL force_model, BOOL force_inf)
{
    wchar_t model[FIELD_CAP], inf[FIELD_CAP], found[FIELD_CAP];

    get_field(dlg, IDC_MODEL, model, FIELD_CAP);
    get_field(dlg, IDC_INF, inf, FIELD_CAP);
    if (force_model || !model[0])
        SetDlgItemTextW(dlg, IDC_MODEL, PC_MODEL);
    find_papercut_inf(dlg, found, FIELD_CAP);
    if (force_inf || !inf[0] || looks_like_papercut_inf(inf))
        SetDlgItemTextW(dlg, IDC_INF, found);
}

static const wchar_t *printer_leaf(const wchar_t *name)
{
    const wchar_t *slash;
    if (!name || !name[0])
        return L"";
    if (name[0] == L'\\' && name[1] == L'\\') {
        slash = wcsrchr(name, L'\\');
        if (slash && slash[1])
            return slash + 1;
    }
    return name;
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
        if (!append_quoted_switch(args, cap, L"/h", L"x64"))
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
        if (!append_quoted_switch(args, cap, L"/h", L"x64"))
            goto overflow;
        if (!append_flag(args, cap, L"/u"))
            goto overflow;
        break;
    case OP_DD:
        used_m = TRUE;
        extra_mr = FALSE;
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
    default:
        if (printer[0] && !append_quoted_switch(args, cap, L"/n", printer))
            goto overflow;
        break;
    }

    if (!skip_c && !append_quoted_switch(args, cap, L"/c", server))
        goto overflow;
    if (extra_mr && !used_m && !append_quoted_switch(args, cap, L"/m", model))
        goto overflow;
    if (extra_mr && !used_r && !append_quoted_switch(args, cap, L"/r", port))
        goto overflow;
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

static int campus_of(const wchar_t *name)
{
    if (!name)
        return GRP_OTHER;
    while (*name == L' ' || *name == L'\t')
        name++;
    if (_wcsnicmp(name, L"MSMS", 4) == 0)
        return GRP_MAXINE;
    if (_wcsnicmp(name, L"UN", 2) == 0)
        return GRP_UNION;
    if (_wcsnicmp(name, L"WH", 2) == 0)
        return GRP_WHITEHAVEN;
    if (_wcsnicmp(name, L"MA", 2) == 0)
        return GRP_MACON;
    return GRP_OTHER;
}

static const wchar_t *group_name(int id)
{
    switch (id) {
    case GRP_UNION:      return L"Union";
    case GRP_MAXINE:     return L"Maxine Smith";
    case GRP_WHITEHAVEN: return L"Whitehaven";
    case GRP_MACON:      return L"Macon";
    default:             return L"Other";
    }
}

static void lv_add_group(HWND lv, int id)
{
    LVGROUP g;
    memset(&g, 0, sizeof(g));
    g.cbSize = sizeof(g);
    g.mask = LVGF_HEADER | LVGF_GROUPID;
    g.pszHeader = (LPWSTR)group_name(id);
    g.iGroupId = id;
    SendMessageW(lv, LVM_INSERTGROUP, (WPARAM)-1, (LPARAM)&g);
}

static void reset_groups(HWND lv)
{
    int id;
    SendMessageW(lv, LVM_REMOVEALLGROUPS, 0, 0);
    memset(g_group_counts, 0, sizeof(g_group_counts));
    for (id = 1; id <= GRP_COUNT; id++)
        lv_add_group(lv, id);
}

static void prune_empty_groups(HWND lv)
{
    int id;
    for (id = 1; id <= GRP_COUNT; id++)
        if (g_group_counts[id] == 0)
            SendMessageW(lv, LVM_REMOVEGROUP, (WPARAM)id, 0);
}

static void add_lv_col(HWND lv, int i, const wchar_t *title, int cx)
{
    LVCOLUMNW col;
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = (wchar_t *)title;
    col.cx = cx;
    col.iSubItem = i;
    SendMessageW(lv, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
}

static int lv_add_row(HWND lv, const wchar_t *name, const wchar_t *driver,
                      const wchar_t *port, const wchar_t *comment)
{
    LVITEMW item;
    int i, group;
    group = campus_of(name);
    memset(&item, 0, sizeof(item));
    item.mask = LVIF_TEXT | LVIF_GROUPID;
    item.iItem = 0x7fffffff;
    item.iGroupId = group;
    item.pszText = (wchar_t *)(name && name[0] ? name : L"");
    i = (int)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&item);
    if (i < 0)
        return -1;
    ListView_SetItemText(lv, i, 1, (wchar_t *)(driver && driver[0] ? driver : L""));
    ListView_SetItemText(lv, i, 2, (wchar_t *)(port && port[0] ? port : L""));
    ListView_SetItemText(lv, i, 3, (wchar_t *)(comment && comment[0] ? comment : L""));
    g_group_counts[group]++;
    return i;
}

static void init_printer_list(HWND dlg)
{
    HWND lv = GetDlgItem(dlg, IDC_PRINTER_LIST);
    ListView_SetExtendedListViewStyle(lv,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_GRIDLINES);
    SendMessageW(lv, LVM_ENABLEGROUPVIEW, TRUE, 0);
    add_lv_col(lv, 0, L"Printer", 160);
    add_lv_col(lv, 1, L"Driver", 140);
    add_lv_col(lv, 2, L"Port", 80);
    add_lv_col(lv, 3, L"Comment", 140);
}

/* Copy one list row into the Selected-printer fields (name + port) and refresh
 * the PaperCut model/INF defaults. Guarded so the EN_CHANGE handlers stay quiet. */
static void fill_fields_from_row(HWND dlg, int index)
{
    HWND lv = GetDlgItem(dlg, IDC_PRINTER_LIST);
    wchar_t name[FIELD_CAP], port[FIELD_CAP];

    if (index < 0)
        return;
    name[0] = port[0] = 0;
    ListView_GetItemText(lv, index, 0, name, FIELD_CAP);
    ListView_GetItemText(lv, index, 2, port, FIELD_CAP);
    g_filling_from_list = TRUE;
    SetDlgItemTextW(dlg, IDC_PRINTER, printer_leaf(name));
    if (port[0])
        SetDlgItemTextW(dlg, IDC_PORT, port);
    apply_papercut_defaults(dlg, TRUE, TRUE);
    g_filling_from_list = FALSE;
}

static void apply_list_selection(HWND dlg, int index)
{
    if (index < 0)
        return;
    fill_fields_from_row(dlg, index);
    g_last_op = OP_ADD_NET;
    refresh(dlg);
}

static DWORD enum_level(HWND lv, const wchar_t *server, DWORD level)
{
    DWORD needed = 0, returned = 0, i;
    BYTE *buf;
    DWORD err;

    EnumPrintersW(PRINTER_ENUM_NAME, (LPWSTR)server, level, NULL, 0, &needed, &returned);
    err = GetLastError();
    if (!needed)
        return err ? err : ERROR_INVALID_PARAMETER;
    buf = (BYTE *)malloc(needed);
    if (!buf)
        return ERROR_OUTOFMEMORY;
    if (!EnumPrintersW(PRINTER_ENUM_NAME, (LPWSTR)server, level, buf, needed, &needed, &returned)) {
        err = GetLastError();
        free(buf);
        return err;
    }
    if (level == 2) {
        PRINTER_INFO_2W *p = (PRINTER_INFO_2W *)buf;
        for (i = 0; i < returned; i++) {
            const wchar_t *nm = p[i].pShareName && p[i].pShareName[0]
                ? p[i].pShareName : printer_leaf(p[i].pPrinterName);
            lv_add_row(lv, nm, p[i].pDriverName, p[i].pPortName, p[i].pComment);
        }
    } else {
        PRINTER_INFO_1W *p = (PRINTER_INFO_1W *)buf;
        for (i = 0; i < returned; i++)
            lv_add_row(lv, printer_leaf(p[i].pName), L"", L"", p[i].pComment);
    }
    free(buf);
    return 0;
}

static void refresh_printers(HWND dlg, BOOL force)
{
    wchar_t typed[FIELD_CAP], server[FIELD_CAP], line[STATUS_CAP];
    HWND lv = GetDlgItem(dlg, IDC_PRINTER_LIST);
    DWORD err;

    get_field(dlg, IDC_SERVER, typed, FIELD_CAP);
    if (!typed[0])
        wcscpy(typed, L"pcut1");
    canon_server(server, FIELD_CAP, typed);

    if (!force && g_last_enum_server[0] && _wcsicmp(server, g_last_enum_server) == 0)
        return;

    SetDlgItemTextW(dlg, IDC_SERVER, server);
    ListView_DeleteAllItems(lv);
    reset_groups(lv);
    _snwprintf(line, STATUS_CAP, L"Querying %s ...", server);
    line[STATUS_CAP - 1] = 0;
    set_status(dlg, line);
    UpdateWindow(GetDlgItem(dlg, IDC_STATUS));

    err = enum_level(lv, server, 2);
    if (err)
        err = enum_level(lv, server, 1);

    prune_empty_groups(lv);
    wcsncpy(g_last_enum_server, server, FIELD_CAP - 1);
    g_last_enum_server[FIELD_CAP - 1] = 0;
    apply_papercut_defaults(dlg, FALSE, FALSE);

    if (err) {
        wchar_t msg[400];
        DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 NULL, err, 0, msg, 400, NULL);
        if (n) {
            trim(msg);
            _snwprintf(line, STATUS_CAP, L"%s: %s", server, msg);
        } else {
            _snwprintf(line, STATUS_CAP, L"%s: error %lu (list fills on Windows against that server)",
                       server, (unsigned long)err);
        }
        line[STATUS_CAP - 1] = 0;
        set_status(dlg, line);
    } else {
        int count = ListView_GetItemCount(lv);
        _snwprintf(line, STATUS_CAP, L"%s — %d printer%s", server, count, count == 1 ? L"" : L"s");
        line[STATUS_CAP - 1] = 0;
        set_status(dlg, line);
    }
    refresh(dlg);
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

/* Operations that act on a printer queue and so can be applied to a whole
 * multi-selection, one rundll32 invocation per selected row. Driver-level and
 * file-based ops (IF/IA/DD/SS/SR) and RAW stay single-shot. */
static BOOL op_is_batchable(Op op)
{
    switch (op) {
    case OP_ADD_NET:
    case OP_DEL_NET:
    case OP_DEL_LOCAL:
    case OP_Y:
    case OP_K:
    case OP_P:
    case OP_O:
        return TRUE;
    default:
        return FALSE;
    }
}

/* Compose from the current fields and run one rundll32 invocation.
 * Returns TRUE if the process launched (its exit code is stored in *code).
 * On failure returns FALSE: *launch_err holds the Win32 error for a launch
 * failure (0 if the failure was compose-time), and err[] holds the message. */
static BOOL run_current(HWND dlg, Op op, BOOL elevate, DWORD *code,
                        DWORD *launch_err, wchar_t *err, size_t ecap)
{
    wchar_t rundll[MAX_PATH], args[CMD_CAP];
    wchar_t cmdline[CMD_CAP], params[CMD_CAP];

    *code = 0;
    *launch_err = 0;

    if (!compose(dlg, op, args, CMD_CAP, err, ecap))
        return FALSE;

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
            *launch_err = GetLastError();
            return FALSE;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return TRUE;
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
            *launch_err = GetLastError();
            return FALSE;
        }
        if (!sei.hProcess) {
            *launch_err = ERROR_INVALID_HANDLE;
            return FALSE;
        }
        WaitForSingleObject(sei.hProcess, INFINITE);
        GetExitCodeProcess(sei.hProcess, code);
        CloseHandle(sei.hProcess);
        return TRUE;
    }
}

/* Apply op to every selected row in turn, filling the Selected-printer fields
 * from each row before composing. Stops early if the user cancels a UAC prompt. */
static void launch_selected(HWND dlg, Op op, BOOL elevate, HWND lv, int sel)
{
    wchar_t save_printer[FIELD_CAP], save_port[FIELD_CAP], line[STATUS_CAP];
    int ok = 0, fail = 0, done = 0, i = -1;
    BOOL cancelled = FALSE;

    get_field(dlg, IDC_PRINTER, save_printer, FIELD_CAP);
    get_field(dlg, IDC_PORT, save_port, FIELD_CAP);

    for (;;) {
        DWORD code = 0, lerr = 0;
        wchar_t err[STATUS_CAP];

        i = (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)i, LVNI_SELECTED);
        if (i < 0)
            break;

        fill_fields_from_row(dlg, i);
        done++;
        _snwprintf(line, STATUS_CAP, L"Working %d of %d ...", done, sel);
        line[STATUS_CAP - 1] = 0;
        set_status(dlg, line);
        UpdateWindow(GetDlgItem(dlg, IDC_STATUS));

        if (!run_current(dlg, op, elevate, &code, &lerr, err, STATUS_CAP)) {
            fail++;
            if (lerr == ERROR_CANCELLED) {
                cancelled = TRUE;
                break;
            }
        } else if (code == 0) {
            ok++;
        } else {
            fail++;
        }
    }

    g_filling_from_list = TRUE;
    SetDlgItemTextW(dlg, IDC_PRINTER, save_printer);
    SetDlgItemTextW(dlg, IDC_PORT, save_port);
    g_filling_from_list = FALSE;

    if (cancelled)
        _snwprintf(line, STATUS_CAP, L"Cancelled at UAC — %d ok, %d failed of %d selected",
                   ok, fail, sel);
    else
        _snwprintf(line, STATUS_CAP, L"%d printers: %d ok, %d failed", sel, ok, fail);
    line[STATUS_CAP - 1] = 0;
    set_status(dlg, line);
    refresh(dlg);
}

static void launch(HWND dlg, Op op, BOOL elevate)
{
    wchar_t err[STATUS_CAP];
    wchar_t msg[512], chdesc[64], badch = 0;
    const wchar_t *badfield;
    HWND lv = GetDlgItem(dlg, IDC_PRINTER_LIST);
    DWORD code = 0, lerr = 0;
    int sel;

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

    sel = (int)SendMessageW(lv, LVM_GETSELECTEDCOUNT, 0, 0);
    if (op_is_batchable(op) && sel > 1) {
        launch_selected(dlg, op, elevate, lv, sel);
        return;
    }

    if (!run_current(dlg, op, elevate, &code, &lerr, err, STATUS_CAP)) {
        if (lerr)
            report_fail(dlg, lerr);
        else
            MessageBoxW(dlg, err[0] ? err : L"Failed to compose arguments.",
                        L"PrintUI", MB_OK | MB_ICONWARNING);
        return;
    }
    _snwprintf(err, STATUS_CAP, L"exit code %lu", (unsigned long)code);
    err[STATUS_CAP - 1] = 0;
    set_status(dlg, err);
}

static INT_PTR CALLBACK DlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
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
        init_printer_list(dlg);
        SetDlgItemTextW(dlg, IDC_SERVER, L"\\\\pcut1");
        apply_papercut_defaults(dlg, TRUE, TRUE);
        refresh_printers(dlg, TRUE);
        return TRUE;

    case WM_NOTIFY:
        if (((NMHDR *)lParam)->idFrom == IDC_PRINTER_LIST) {
            NMITEMACTIVATE *nm = (NMITEMACTIVATE *)lParam;
            if (nm->hdr.code == NM_CLICK || nm->hdr.code == NM_DBLCLK ||
                nm->hdr.code == LVN_ITEMACTIVATE) {
                if (nm->iItem >= 0)
                    apply_list_selection(dlg, nm->iItem);
                return TRUE;
            }
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_PRINTER:
        case IDC_MODEL:
        case IDC_PORT:
        case IDC_INF:
        case IDC_SETTINGS:
        case IDC_RAW:
            if (HIWORD(wParam) == EN_CHANGE && !g_filling_from_list)
                refresh(dlg);
            return TRUE;
        case IDC_SERVER:
            if (HIWORD(wParam) == EN_CHANGE && !g_filling_from_list)
                refresh(dlg);
            if (HIWORD(wParam) == EN_KILLFOCUS)
                refresh_printers(dlg, FALSE);
            return TRUE;
        case IDC_REFRESH:
            refresh_printers(dlg, TRUE);
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
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    DialogBoxW(inst, MAKEINTRESOURCEW(IDD_MAIN), NULL, DlgProc);
    return 0;
}
