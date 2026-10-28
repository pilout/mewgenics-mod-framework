// MewCore mod menu: a small native window listing the loaded mods with a checkbox each.
// Opened/closed with a key (menu_key in Mods\config\core.cfg, default F10) while the game window has focus.
// Toggles are applied on the game thread and saved to core.cfg ("disabled=" list).
// Included by core.c (uses its Mod table, set_enabled, clog, g_cs, g_dir).
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdi32.lib")

#define WM_MENU_SHOW    (WM_APP + 1)   // wParam: 0 hide, 1 show+activate, 2 show without activating, 3 toggle+activate
#define WM_MENU_REFRESH (WM_APP + 2)
enum { IDC_LIST = 100, IDC_DESC, IDC_OPENCFG, IDC_RELOAD, IDC_CLOSE, IDC_HINT, IDC_SCROLL, IDC_ROW0 = 200 };   // rows use IDC_ROW0 + 2*i (label) and + 2*i+1 (input)
#define MENU_MAX_ROWS 64   // settings of one mod that can be listed; MENU_VIS_ROWS show at once, the rest scrolls
#define MENU_VIS_ROWS 8
#define MENU_MAX_KV 256
#define MENU_ROWS_Y 226

static HWND g_menu, g_menuList, g_menuDesc;
static volatile LONG g_menuStarted;
static HANDLE g_menuReady;
static int g_menuFilling;
static UINT g_menuVk = VK_F10;
static HFONT g_menuFont;
typedef struct { int idx, op; char key[64]; } MenuReq;   // op: 0 disable, 1 enable, 2 reload config, 3 button action (key)
static MenuReq g_menuReq[64]; static int g_nmenuReq;
static HWND menu_child(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int hh, int id);

static void menu_rows_build(int mi);

static void utf8_to_w(const char *s, wchar_t *out, int cap) { if (!MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, out, cap)) out[0] = 0; }

static UINT parse_vk(const char *k) {
    if (!k || !*k || !_stricmp(k, "none")) return 0;
    if ((k[0] == 'F' || k[0] == 'f') && atoi(k + 1) >= 1 && atoi(k + 1) <= 24) return VK_F1 + atoi(k + 1) - 1;
    if (!_stricmp(k, "insert")) return VK_INSERT;
    if (!_stricmp(k, "home")) return VK_HOME;
    if (!_stricmp(k, "pause")) return VK_PAUSE;
    if (!_strnicmp(k, "0x", 2)) return (UINT)strtoul(k, NULL, 16);
    return VK_F10;
}

// Rewrites Mods\config\core.cfg from the current state (keeps agent / menu_key).
static void save_cfg(void) {
    char p[MAX_PATH], dis[2048] = ""; size_t n = 0;
    for (int i = 0; i < g_nmods; i++) if (!g_mods[i].enabled && g_mods[i].id && n + strlen(g_mods[i].id) + 2 < sizeof dis) n += (size_t)snprintf(dis + n, sizeof dis - n, "%s%s", n ? "," : "", g_mods[i].id);
    strncpy(g_cfg_disabled, dis, sizeof g_cfg_disabled - 1);
    snprintf(p, sizeof p, "%s\\Mods\\config\\core.cfg", g_dir);
    FILE *f = fopen(p, "w"); if (!f) { clog("[Menu] cannot write %s", p); return; }
    fprintf(f, "agent=%d\nmenu_key=%s\ndisabled=%s\n", g_cfg_agent, g_cfg_menu_key, dis);
    fclose(f);
}

static void menu_queue(int idx, int op) {
    EnterCriticalSection(&g_cs); if (g_nmenuReq < 64) { memset(&g_menuReq[g_nmenuReq], 0, sizeof(MenuReq)); g_menuReq[g_nmenuReq].idx = idx; g_menuReq[g_nmenuReq].op = op; g_nmenuReq++; } LeaveCriticalSection(&g_cs);
}

static void menu_queue_action(int idx, const char *key) {
    EnterCriticalSection(&g_cs);
    if (g_nmenuReq < 64) { MenuReq *q = &g_menuReq[g_nmenuReq++]; memset(q, 0, sizeof *q); q->idx = idx; q->op = 3; strncpy(q->key, key, sizeof q->key - 1); }
    LeaveCriticalSection(&g_cs);
}

static void menu_fill(void) {
    g_menuFilling = 1;
    if (ListView_GetItemCount(g_menuList) != g_nmods) {
        ListView_DeleteAllItems(g_menuList);
        for (int i = 0; i < g_nmods; i++) {
            wchar_t w[256]; LVITEMW it = {0}; it.mask = LVIF_TEXT; it.iItem = i;
            utf8_to_w(g_mods[i].name, w, 256); it.pszText = w; SendMessageW(g_menuList, LVM_INSERTITEMW, 0, (LPARAM)&it);
            utf8_to_w(g_mods[i].version, w, 256); it.iSubItem = 1; SendMessageW(g_menuList, LVM_SETITEMTEXTW, i, (LPARAM)&it);
            utf8_to_w(g_mods[i].id, w, 256); it.iSubItem = 2; SendMessageW(g_menuList, LVM_SETITEMTEXTW, i, (LPARAM)&it);
        }
    }
    for (int i = 0; i < g_nmods; i++) ListView_SetCheckState(g_menuList, i, g_mods[i].enabled ? TRUE : FALSE);
    g_menuFilling = 0;
}

static int menu_selected(void) { return ListView_GetNextItem(g_menuList, -1, LVNI_SELECTED); }

static void menu_show_desc(int i) {
    wchar_t w[1024] = L"Select a mod. Tick or untick it to enable or disable it (saved to Mods\\config\\core.cfg).";
    if (i >= 0 && i < g_nmods) { utf8_to_w(*g_mods[i].description ? g_mods[i].description : "(no description)", w, 1024); }
    SetWindowTextW(g_menuDesc, w);
    menu_rows_build(i);
}

static void menu_cfg_path(int i, wchar_t *out) {
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\Mods\\config\\%s.cfg", g_dir, g_mods[i].id); utf8_to_w(p, out, MAX_PATH);
}

static HWND menu_child(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int hh, int id);
// ---- per-mod settings: one input per key of Mods\config\<id>.cfg (written by the mod through ModConfig) ----
// "<key>__hint" holds the label shown for <key>; "<key>__action" turns <key> into a button (label = its value);
// Mods\config\<id>.<key>.opts ("value|label" per line) turns <key> into a drop-down (searchable when long);
// "__status" is shown on the bottom line; "__opts" is ignored. True/False values become a checkbox.
typedef struct { char key[64]; char val[256]; } CfgKV;
typedef struct { char val[96]; wchar_t disp[160]; } CfgOpt;
enum { K_EDIT, K_BOOL, K_COMBO, K_ACTION };
typedef struct { char key[64]; char val[256]; int kind; HWND lbl, ctl; CfgOpt *opt; int nopt; int editable; } CfgRow;
static CfgRow g_rows[MENU_MAX_ROWS]; static int g_nrows, g_rowsMod = -1;
static wchar_t g_menuHint[200];
static HWND g_scroll; static int g_rowOff;

static int cfg_load(int mi, CfgKV *kv, int cap) {
    char p[MAX_PATH], line[512]; int n = 0;
    snprintf(p, sizeof p, "%s\\Mods\\config\\%s.cfg", g_dir, g_mods[mi].id);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    while (n < cap && fgets(line, sizeof line, f)) {
        char *e = strchr(line, '='); if (!e || e == line) continue;
        size_t kl = (size_t)(e - line), vl = strlen(e + 1);
        while (vl && (e[vl] == '\n' || e[vl] == '\r' || e[vl] == ' ')) vl--;
        if (kl >= sizeof kv[n].key) continue; if (vl >= sizeof kv[n].val) vl = sizeof kv[n].val - 1;
        memcpy(kv[n].key, line, kl); kv[n].key[kl] = 0; memcpy(kv[n].val, e + 1, vl); kv[n].val[vl] = 0; n++;
    }
    fclose(f); return n;
}
static void cfg_store(int mi, const CfgKV *kv, int n) {
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\Mods\\config\\%s.cfg", g_dir, g_mods[mi].id);
    FILE *f = fopen(p, "w"); if (!f) { clog("[Menu] cannot write %s", p); return; }
    for (int i = 0; i < n; i++) fprintf(f, "%s=%s\n", kv[i].key, kv[i].val);
    fclose(f);
}
static int ends_with(const char *k, const char *suf) { size_t l = strlen(k), s = strlen(suf); return l > s && !strcmp(k + l - s, suf); }
static int key_is_meta(const char *k) { return ends_with(k, "__opts") || ends_with(k, "__hint") || ends_with(k, "__action") || !strcmp(k, "__status"); }
static int is_boolstr(const char *v) { return !_stricmp(v, "true") || !_stricmp(v, "false"); }
static const char *kv_find(const CfgKV *kv, int n, const char *key) { for (int i = 0; i < n; i++) if (!strcmp(kv[i].key, key)) return kv[i].val; return NULL; }

static CfgOpt *opts_load(int mi, const char *key, int *count) {
    char p[MAX_PATH], line[512]; int cap = 64, n = 0; CfgOpt *o = NULL;
    snprintf(p, sizeof p, "%s\\Mods\\config\\%s.%s.opts", g_dir, g_mods[mi].id, key);
    FILE *f = fopen(p, "r"); *count = 0; if (!f) return NULL;
    o = (CfgOpt *)malloc(sizeof(CfgOpt) * (size_t)cap);
    while (o && fgets(line, sizeof line, f)) {
        char *bar = strchr(line, '|'); size_t l = strlen(line); while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!bar || bar == line) continue; *bar = 0;
        if (n == cap) { cap *= 2; CfgOpt *t = (CfgOpt *)realloc(o, sizeof(CfgOpt) * (size_t)cap); if (!t) break; o = t; }
        strncpy(o[n].val, line, sizeof o[n].val - 1); o[n].val[sizeof o[n].val - 1] = 0; utf8_to_w(bar + 1, o[n].disp, 160); n++;
    }
    fclose(f); *count = n; if (!n) { free(o); o = NULL; } return o;
}
static const wchar_t *opt_disp_for(const CfgRow *r, const char *val) { for (int i = 0; i < r->nopt; i++) if (!strcmp(r->opt[i].val, val)) return r->opt[i].disp; return NULL; }

static int row_has_focus(const CfgRow *r) { HWND f = GetFocus(); return f && (f == r->ctl || GetParent(f) == r->ctl); }

static void menu_rows_clear(void) {
    for (int i = 0; i < g_nrows; i++) { if (g_rows[i].lbl) DestroyWindow(g_rows[i].lbl); if (g_rows[i].ctl) DestroyWindow(g_rows[i].ctl); free(g_rows[i].opt); g_rows[i].opt = NULL; }
    g_nrows = 0; g_rowsMod = -1; g_rowOff = 0;
}

static void row_set_value_ui(CfgRow *r, const char *val) {
    wchar_t w[260]; utf8_to_w(val, w, 260);
    if (r->kind == K_BOOL) SendMessageW(r->ctl, BM_SETCHECK, !_stricmp(val, "true") ? BST_CHECKED : BST_UNCHECKED, 0);
    else if (r->kind == K_COMBO) {
        const wchar_t *d = opt_disp_for(r, val);
        if (r->editable) SetWindowTextW(r->ctl, d ? d : w);
        else { int sel = -1; for (int k = 0; k < r->nopt; k++) if (!strcmp(r->opt[k].val, val)) sel = k; SendMessageW(r->ctl, CB_SETCURSEL, (WPARAM)sel, 0); }
    }
    else if (r->kind == K_EDIT) SetWindowTextW(r->ctl, w);
}

// Shows the rows from g_rowOff on (MENU_VIS_ROWS of them) and hides the others; the scroll bar follows.
static void menu_rows_layout(void) {
    int maxoff = g_nrows > MENU_VIS_ROWS ? g_nrows - MENU_VIS_ROWS : 0;
    if (g_rowOff > maxoff) g_rowOff = maxoff; if (g_rowOff < 0) g_rowOff = 0;
    for (int i = 0; i < g_nrows; i++) {
        CfgRow *r = &g_rows[i]; int vis = i >= g_rowOff && i < g_rowOff + MENU_VIS_ROWS, y = MENU_ROWS_Y + (i - g_rowOff) * 28;
        HWND w[2] = { r->lbl, r->ctl };
        for (int k = 0; k < 2; k++) if (w[k]) {
            int x = (k == 0 || r->kind == K_BOOL) ? 10 : 270;
            if (vis) SetWindowPos(w[k], NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            ShowWindow(w[k], vis ? SW_SHOWNA : SW_HIDE);
        }
    }
    if (g_scroll) {
        SCROLLINFO si = { sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, g_nrows > 0 ? g_nrows - 1 : 0, MENU_VIS_ROWS, g_rowOff, 0 };
        SetScrollInfo(g_scroll, SB_CTL, &si, TRUE); ShowWindow(g_scroll, g_nrows > MENU_VIS_ROWS ? SW_SHOWNA : SW_HIDE);
    }
}
// Builds the inputs for mod mi, or only refreshes their values when the same keys are already shown.
static void menu_rows_build(int mi) {
    CfgKV kv[MENU_MAX_KV]; int n = (mi >= 0 && mi < g_nmods) ? cfg_load(mi, kv, MENU_MAX_KV) : 0;
    CfgKV shown[MENU_MAX_ROWS]; int ns = 0;
    for (int i = 0; i < n && ns < MENU_MAX_ROWS; i++) if (!key_is_meta(kv[i].key)) shown[ns++] = kv[i];
    const char *st = kv_find(kv, n, "__status");
    if (g_menuHint[0]) { if (st && *st) { wchar_t w[200]; utf8_to_w(st, w, 200); SetWindowTextW(GetDlgItem(g_menu, IDC_HINT), w); } else SetWindowTextW(GetDlgItem(g_menu, IDC_HINT), g_menuHint); }
    int same = (g_rowsMod == mi && ns == g_nrows);
    for (int i = 0; same && i < ns; i++) if (strcmp(shown[i].key, g_rows[i].key)) same = 0;
    if (same) {
        for (int i = 0; i < ns; i++) {
            if (g_rows[i].kind == K_ACTION || row_has_focus(&g_rows[i])) continue;
            strcpy(g_rows[i].val, shown[i].val); row_set_value_ui(&g_rows[i], shown[i].val);
        }
        return;
    }
    menu_rows_clear(); g_rowsMod = mi;
    for (int i = 0; i < ns; i++) {
        CfgRow *r = &g_rows[i]; memset(r, 0, sizeof *r); strcpy(r->key, shown[i].key); strcpy(r->val, shown[i].val);
        char hk[80]; snprintf(hk, sizeof hk, "%s__hint", r->key); const char *hint = kv_find(kv, n, hk);
        snprintf(hk, sizeof hk, "%s__action", r->key); const char *act = kv_find(kv, n, hk);
        r->opt = act ? NULL : opts_load(mi, r->key, &r->nopt);
        r->kind = act ? K_ACTION : r->opt ? K_COMBO : is_boolstr(r->val) ? K_BOOL : K_EDIT;
        wchar_t wl[200]; utf8_to_w(hint ? hint : r->key, wl, 200);
        int y = MENU_ROWS_Y + (i < MENU_VIS_ROWS ? i : 0) * 28, id = IDC_ROW0 + 2 * i;
        if (r->kind == K_ACTION) {
            wchar_t wa[200]; utf8_to_w(act, wa, 200);
            r->ctl = menu_child(L"BUTTON", wa, WS_TABSTOP | BS_PUSHBUTTON, 270, y, 240, 24, id + 1);
        } else if (r->kind == K_BOOL) {
            r->ctl = menu_child(L"BUTTON", wl, WS_TABSTOP | BS_AUTOCHECKBOX, 10, y, 500, 22, id + 1);
            row_set_value_ui(r, r->val);
        } else if (r->kind == K_COMBO) {
            r->editable = r->nopt > 12;
            r->lbl = menu_child(L"STATIC", wl, SS_LEFT | SS_CENTERIMAGE, 10, y, 250, 22, id);
            r->ctl = menu_child(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | (r->editable ? CBS_DROPDOWN | CBS_AUTOHSCROLL : CBS_DROPDOWNLIST), 270, y, 240, 260, id + 1);
            for (int k = 0; k < r->nopt; k++) SendMessageW(r->ctl, CB_ADDSTRING, 0, (LPARAM)r->opt[k].disp);
            row_set_value_ui(r, r->val);
        } else {
            r->lbl = menu_child(L"STATIC", wl, SS_LEFT | SS_CENTERIMAGE, 10, y, 250, 22, id);
            r->ctl = menu_child(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 270, y, 240, 22, id + 1);
            row_set_value_ui(r, r->val);
        }
        g_nrows++;
    }
    menu_rows_layout();
}

// Typing in a long drop-down: keep only the choices containing the typed text.
static void combo_filter(int row) {
    CfgRow *r = &g_rows[row]; wchar_t t[160]; int len = GetWindowTextW(r->ctl, t, 160), shown = 0;
    SendMessageW(r->ctl, WM_SETREDRAW, FALSE, 0);
    SendMessageW(r->ctl, CB_RESETCONTENT, 0, 0);
    for (int k = 0; k < r->nopt && shown < 300; k++) if (!t[0] || StrStrIW(r->opt[k].disp, t)) { SendMessageW(r->ctl, CB_ADDSTRING, 0, (LPARAM)r->opt[k].disp); shown++; }
    SetWindowTextW(r->ctl, t); SendMessageW(r->ctl, CB_SETEDITSEL, 0, MAKELPARAM(len, len));
    SendMessageW(r->ctl, WM_SETREDRAW, TRUE, 0);
    SendMessageW(r->ctl, CB_SHOWDROPDOWN, shown > 0, 0); SetCursor(LoadCursor(NULL, IDC_ARROW));
}

// Maps what is in a drop-down (a label, a value, or a unique partial label) to the stored value; NULL when unknown.
static const char *combo_resolve(const CfgRow *r, const wchar_t *text) {
    const char *partial = NULL; int np = 0;
    for (int k = 0; k < r->nopt; k++) {
        if (!_wcsicmp(r->opt[k].disp, text)) return r->opt[k].val;
        wchar_t v[96]; utf8_to_w(r->opt[k].val, v, 96); if (!_wcsicmp(v, text)) return r->opt[k].val;
        if (text[0] && StrStrIW(r->opt[k].disp, text)) { partial = r->opt[k].val; np++; }
    }
    return np == 1 ? partial : NULL;
}

// Writes the row's current value to the cfg file and asks the mod to reload it (game thread).
static void menu_row_apply(int row) {
    if (row < 0 || row >= g_nrows || g_rowsMod < 0) return;
    CfgRow *r = &g_rows[row]; char nv[256];
    if (r->kind == K_ACTION) return;
    if (r->kind == K_BOOL) strcpy(nv, SendMessageW(r->ctl, BM_GETCHECK, 0, 0) == BST_CHECKED ? "True" : "False");
    else {
        wchar_t w[260]; GetWindowTextW(r->ctl, w, 260);
        if (r->kind == K_COMBO) {
            const char *v = combo_resolve(r, w);
            if (!v) { row_set_value_ui(r, r->val); return; }   // unknown text: put the stored choice back
            strcpy(nv, v);
        } else {
            if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, nv, sizeof nv, NULL, NULL)) return;
            char *s = nv; while (*s == ' ') s++; memmove(nv, s, strlen(s) + 1);
            for (size_t l = strlen(nv); l && nv[l - 1] == ' '; ) nv[--l] = 0;
        }
    }
    if (r->kind == K_COMBO) row_set_value_ui(r, nv);
    if (!strcmp(nv, r->val)) return;
    CfgKV kv[MENU_MAX_KV]; int n = cfg_load(g_rowsMod, kv, MENU_MAX_KV), found = 0;
    for (int i = 0; i < n; i++) if (!strcmp(kv[i].key, r->key)) { strcpy(kv[i].val, nv); found = 1; }
    if (!found && n < MENU_MAX_KV) { strcpy(kv[n].key, r->key); strcpy(kv[n].val, nv); n++; }
    cfg_store(g_rowsMod, kv, n); strcpy(r->val, nv);
    menu_queue(g_rowsMod, 2);
}
static void menu_place(void) {
    // center on the game window (same process, visible SDL window)
    RECT gr = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)}, mr;
    HWND gw = g_hwnd; if (!gw) gw = FindWindowA("SDL_app", NULL);
    if (gw) { DWORD pid = 0; GetWindowThreadProcessId(gw, &pid); if (pid == GetCurrentProcessId()) GetWindowRect(gw, &gr); }
    GetWindowRect(g_menu, &mr);
    int w = mr.right - mr.left, h = mr.bottom - mr.top;
    SetWindowPos(g_menu, HWND_TOPMOST, gr.left + (gr.right - gr.left - w) / 2, gr.top + (gr.bottom - gr.top - h) / 2, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

static LRESULT CALLBACK menu_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MENU_SHOW: {
        int vis = IsWindowVisible(h), mode = (int)wp;
        if (mode == 3) mode = vis ? 0 : 1;
        if (mode == 0) { ShowWindow(h, SW_HIDE); return 0; }
        menu_fill(); if (!vis) menu_place();
        ShowWindow(h, mode == 1 ? SW_SHOW : SW_SHOWNOACTIVATE);
        if (mode == 1) { SetForegroundWindow(h); SetFocus(g_menuList); }
        return 0;
    }
    case WM_MENU_REFRESH: menu_fill(); menu_rows_build(g_rowsMod); return 0;
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == IDC_LIST && nh->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *lv = (NMLISTVIEW *)lp;
            if (lv->uChanged & LVIF_STATE) {
                if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED)) menu_show_desc(lv->iItem);
                UINT ns = (lv->uNewState & LVIS_STATEIMAGEMASK) >> 12, os = (lv->uOldState & LVIS_STATEIMAGEMASK) >> 12;
                if (!g_menuFilling && ns && os && ns != os && lv->iItem >= 0 && lv->iItem < g_nmods) menu_queue(lv->iItem, ns == 2);
            }
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_OPENCFG: {
            int i = menu_selected(); if (i < 0) { SetWindowTextW(g_menuDesc, L"Select a mod first."); break; }
            wchar_t p[MAX_PATH]; menu_cfg_path(i, p);
            if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) { SetWindowTextW(g_menuDesc, L"This mod has no config file yet (it is created the first time the mod saves a setting)."); break; }
            ShellExecuteW(h, L"open", L"notepad.exe", p, NULL, SW_SHOWNORMAL);
            SetWindowTextW(g_menuDesc, L"Config opened in Notepad. Save it, then press \"Reload config\".");
            break;
        }
        case IDC_RELOAD: { int i = menu_selected(); if (i >= 0) { menu_queue(i, 2); SetWindowTextW(g_menuDesc, L"Config reload requested."); } break; }
        case IDC_CLOSE: case IDCANCEL: ShowWindow(h, SW_HIDE); break;
        case IDOK: { for (int i = 0; i < g_nrows; i++) if (row_has_focus(&g_rows[i])) menu_row_apply(i); break; }
        default: {
            int id = LOWORD(wp) - IDC_ROW0, row = id / 2;
            if (id >= 0 && (id & 1) && row < g_nrows) {
                CfgRow *r = &g_rows[row]; UINT code = HIWORD(wp);
                if (r->kind == K_ACTION) { if (code == BN_CLICKED) menu_queue_action(g_rowsMod, r->key); }
                else if (r->kind == K_BOOL) { if (code == BN_CLICKED) menu_row_apply(row); }
                else if (r->kind == K_COMBO) {
                    if (code == CBN_EDITCHANGE) combo_filter(row);
                    else if (code == CBN_SELENDOK) { wchar_t t[200]; LRESULT s = SendMessageW(r->ctl, CB_GETCURSEL, 0, 0); if (s >= 0 && SendMessageW(r->ctl, CB_GETLBTEXTLEN, s, 0) < 200) { SendMessageW(r->ctl, CB_GETLBTEXT, s, (LPARAM)t); SetWindowTextW(r->ctl, t); menu_row_apply(row); } }
                    else if (code == CBN_KILLFOCUS) menu_row_apply(row);
                }
                else if (code == EN_KILLFOCUS) menu_row_apply(row);
            }
        }
        }
        return 0;
    case WM_VSCROLL:
        if ((HWND)lp == g_scroll) {
            SCROLLINFO si = { sizeof si, SIF_ALL }; GetScrollInfo(g_scroll, SB_CTL, &si); int pos = si.nPos;
            switch (LOWORD(wp)) {
            case SB_LINEUP: pos--; break; case SB_LINEDOWN: pos++; break;
            case SB_PAGEUP: pos -= MENU_VIS_ROWS; break; case SB_PAGEDOWN: pos += MENU_VIS_ROWS; break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
            }
            g_rowOff = pos; menu_rows_layout();
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (g_nrows > MENU_VIS_ROWS) { g_rowOff -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 2; menu_rows_layout(); }
        return 0;
    case WM_CLOSE: ShowWindow(h, SW_HIDE); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND menu_child(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int hh, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, hh, g_menu, (HMENU)(intptr_t)id, g_self, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_menuFont, TRUE); return c;
}

static DWORD WINAPI menu_thread(LPVOID p) {
    (void)p;
    INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_LISTVIEW_CLASSES }; InitCommonControlsEx(&ic);
    NONCLIENTMETRICSW ncm = { sizeof ncm }; SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0); g_menuFont = CreateFontIndirectW(&ncm.lfMessageFont);
    WNDCLASSEXW wc = { sizeof wc }; wc.lpfnWndProc = menu_proc; wc.hInstance = g_self; wc.lpszClassName = L"MewCoreMenu";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClassExW(&wc);
    // Not owned by the game window on purpose: a cross-thread owner would attach the input queues of both threads.
    RECT r = { 0, 0, 540, 520 }; AdjustWindowRectEx(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
    g_menu = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"MewCoreMenu", L"MewCore " CORE_VERSION_W L" - Mods", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             100, 100, r.right - r.left, r.bottom - r.top, NULL, NULL, g_self, NULL);
    g_menuList = menu_child(WC_LISTVIEWW, L"", WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 10, 10, 520, 150, IDC_LIST);
    ListView_SetExtendedListViewStyle(g_menuList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col = { 0 }; col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = L"Mod"; col.cx = 240; SendMessageW(g_menuList, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.pszText = L"Version"; col.cx = 80; SendMessageW(g_menuList, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.pszText = L"Id"; col.cx = 175; SendMessageW(g_menuList, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    g_menuDesc = menu_child(L"STATIC", L"", SS_LEFT, 10, 166, 520, 54, IDC_DESC);
    g_scroll = CreateWindowExW(0, L"SCROLLBAR", L"", WS_CHILD | SBS_VERT, 514, MENU_ROWS_Y, 18, MENU_VIS_ROWS * 28 - 4, g_menu, (HMENU)(intptr_t)IDC_SCROLL, g_self, NULL);
    menu_child(L"BUTTON", L"Open config", WS_TABSTOP | BS_PUSHBUTTON, 10, 458, 110, 26, IDC_OPENCFG);
    menu_child(L"BUTTON", L"Reload config", WS_TABSTOP | BS_PUSHBUTTON, 128, 458, 110, 26, IDC_RELOAD);
    menu_child(L"BUTTON", L"Close", WS_TABSTOP | BS_PUSHBUTTON, 420, 458, 110, 26, IDC_CLOSE);
    wchar_t hint[160], key[32]; utf8_to_w(g_cfg_menu_key, key, 32);
    swprintf(hint, 160, L"%s shows/hides this window. Enter or click elsewhere applies a setting.", key);
    wcscpy(g_menuHint, hint);
    menu_child(L"STATIC", hint, SS_LEFT, 10, 492, 520, 20, IDC_HINT);
    menu_show_desc(-1);
    SetEvent(g_menuReady);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { if (!IsDialogMessageW(g_menu, &m)) { TranslateMessage(&m); DispatchMessageW(&m); } }
    return 0;
}

// mode: 0 hide, 1 show+activate, 2 show without activating (dev console), 3 toggle+activate (hotkey)
static void menu_post(int mode) {
    if (InterlockedCompareExchange(&g_menuStarted, 1, 0) == 0) {
        g_menuReady = CreateEventA(NULL, TRUE, FALSE, NULL);
        CreateThread(NULL, 0, menu_thread, NULL, 0, NULL);
    }
    if (WaitForSingleObject(g_menuReady, 3000) == WAIT_OBJECT_0 && g_menu) PostMessageW(g_menu, WM_MENU_SHOW, (WPARAM)mode, 0);
}

// Game thread, every frame: hotkey edge + pending toggles.
static void menu_frame(void) {
    static int was, init;
    if (!init) { init = 1; g_menuVk = parse_vk(g_cfg_menu_key); }
    if (g_menuVk) {
        int down = (GetAsyncKeyState((int)g_menuVk) & 0x8000) != 0;
        if (down && !was) {
            DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            if (pid == GetCurrentProcessId()) menu_post(3);
        }
        was = down;
    }
    MenuReq req[64]; int n;
    EnterCriticalSection(&g_cs); n = g_nmenuReq; memcpy(req, g_menuReq, sizeof(MenuReq) * (size_t)n); g_nmenuReq = 0; LeaveCriticalSection(&g_cs);
    if (!n) return;
    int changed = 0;
    for (int i = 0; i < n; i++) {
        Mod *m = &g_mods[req[i].idx];
        if (req[i].op == 3) {
            if (m->action) __try { m->action(req[i].key); } __except (EXCEPTION_EXECUTE_HANDLER) { clog("[Menu] %s action %s threw", m->id, req[i].key); }
            clog("[Menu] %s action %s", m->id, req[i].key);
        } else if (req[i].op == 2) {
            void (*fn)(void) = m->desc && m->desc->OnConfigReload ? m->desc->OnConfigReload : m->reload;
            if (fn) __try { fn(); } __except (EXCEPTION_EXECUTE_HANDLER) { clog("[Menu] %s config reload threw", m->id); }
            clog("[Menu] %s config reloaded", m->id);
        } else { set_enabled(m, req[i].op); changed = 1; }
    }
    if (changed) save_cfg();
    if (g_menu) PostMessageW(g_menu, WM_MENU_REFRESH, 0, 0);
}

// Dev console: menu show | hide | state | check <index> <0|1>   (check goes through the list view like a click)
static int menu_console(SOCKET s, char *line) {
    char out[2048];
    if (!strncmp(line, "menu show", 9)) { menu_post(2); sendstr(s, "ok"); return 1; }
    if (!strncmp(line, "menu hide", 9)) { menu_post(0); sendstr(s, "ok"); return 1; }
    if (!strncmp(line, "menu state", 10)) {
        size_t n = (size_t)snprintf(out, sizeof out, "ok visible=%d key=%s", g_menu && IsWindowVisible(g_menu), g_cfg_menu_key);
        for (int i = 0; g_menuList && i < g_nmods; i++) n += (size_t)snprintf(out + n, sizeof out - n, " %s:checked=%d,enabled=%d", g_mods[i].id, (int)ListView_GetCheckState(g_menuList, i), g_mods[i].enabled);
        sendstr(s, out); return 1;
    }
    if (!strncmp(line, "menu shot ", 10)) {   // PNG of the menu window itself (the in-engine "shot" only sees the game)
        RECT wr; if (!g_menu || !IsWindowVisible(g_menu) || !GetWindowRect(g_menu, &wr)) { sendstr(s, "err menu not visible (menu show first)"); return 1; }
        int w = wr.right - wr.left, h = wr.bottom - wr.top;
        BITMAPINFO bi = {0}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        HDC sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc); void *bits = NULL;
        HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        HGDIOBJ old = SelectObject(mdc, bmp);
        int ok = PrintWindow(g_menu, mdc, 2);
        uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * 3);
        for (int i = 0; ok && rgb && i < w * h; i++) { const uint8_t *p = (const uint8_t *)bits + i * 4; rgb[i * 3] = p[2]; rgb[i * 3 + 1] = p[1]; rgb[i * 3 + 2] = p[0]; }
        if (ok && rgb && write_png(line + 10, rgb, w, h)) sendstr(s, "ok"); else sendstr(s, "err capture failed");
        free(rgb); SelectObject(mdc, old); DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(NULL, sdc); return 1;
    }
    if (!strncmp(line, "menu select ", 12)) {
        int i = atoi(line + 12); if (i < 0 || i >= g_nmods || !g_menuList) { sendstr(s, "err usage: menu select <index> (menu show first)"); return 1; }
        ListView_SetItemState(g_menuList, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        sendstr(s, "ok"); return 1;
    }
    if (!strncmp(line, "menu rows", 9)) {   // shown inputs of the selected mod: key=value ...
        size_t n = (size_t)snprintf(out, sizeof out, "ok mod=%d", g_rowsMod);
        for (int i = 0; i < g_nrows; i++) n += (size_t)snprintf(out + n, sizeof out - n, " %s[%d]=%s", g_rows[i].key, g_rows[i].kind, g_rows[i].val);
        sendstr(s, out); return 1;
    }
    if (!strncmp(line, "menu set ", 9)) {   // menu set <row> <text>: types into the input and applies it (as Enter would)
        int i = -1, off = 0; if (sscanf(line + 9, "%d %n", &i, &off) < 1 || i < 0 || i >= g_nrows) { sendstr(s, "err usage: menu set <row> <text>"); return 1; }
        const char *t = line + 9 + off; wchar_t w[160];
        if (g_rows[i].kind == K_ACTION) { SendMessageW(g_menu, WM_COMMAND, MAKEWPARAM(IDC_ROW0 + 2 * i + 1, BN_CLICKED), (LPARAM)g_rows[i].ctl); sendstr(s, "ok"); return 1; }
        if (g_rows[i].kind == K_BOOL) { SendMessageW(g_rows[i].ctl, BM_SETCHECK, !_stricmp(t, "true") || !strcmp(t, "1") ? BST_CHECKED : BST_UNCHECKED, 0); SendMessageW(g_menu, WM_COMMAND, MAKEWPARAM(IDC_ROW0 + 2 * i + 1, BN_CLICKED), (LPARAM)g_rows[i].ctl); sendstr(s, "ok"); return 1; }
        utf8_to_w(t, w, 160);
        if (g_rows[i].kind == K_COMBO && !g_rows[i].editable) {   // a list-only combo has no text to type into: select the matching choice
            const char *v = combo_resolve(&g_rows[i], w); int sel = -1;
            for (int k = 0; v && k < g_rows[i].nopt; k++) if (!strcmp(g_rows[i].opt[k].val, v)) sel = k;
            if (sel < 0) { sendstr(s, "err unknown choice"); return 1; }
            SendMessageW(g_rows[i].ctl, CB_SETCURSEL, (WPARAM)sel, 0);
            SendMessageW(g_menu, WM_COMMAND, MAKEWPARAM(IDC_ROW0 + 2 * i + 1, CBN_SELENDOK), (LPARAM)g_rows[i].ctl);
            sendstr(s, "ok"); return 1;
        }
        SetWindowTextW(g_rows[i].ctl, w);
        SendMessageW(g_menu, WM_COMMAND, MAKEWPARAM(IDC_ROW0 + 2 * i + 1, g_rows[i].kind == K_COMBO ? CBN_KILLFOCUS : EN_KILLFOCUS), (LPARAM)g_rows[i].ctl);
        sendstr(s, "ok"); return 1;
    }
    if (!strncmp(line, "menu check ", 11)) {
        int i = -1, on = 0;
        if (sscanf(line + 11, "%d %d", &i, &on) != 2 || i < 0 || i >= g_nmods || !g_menuList) { sendstr(s, "err usage: menu check <index> <0|1> (menu show first)"); return 1; }
        ListView_SetCheckState(g_menuList, i, on ? TRUE : FALSE);
        sendstr(s, "ok"); return 1;
    }
    return 0;
}
