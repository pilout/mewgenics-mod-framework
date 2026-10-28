// Furniture merge (included by core.c).
//
// The game reads data\furniture_info.data (binary, one row per furniture) from its packed files and does not merge it itself.
// MewCore hooks the packed-file loader (0x99FF50) and appends the rows of every enabled mod, so no extra framework is needed:
//
//   Mods\<id>\data\furniture_info.data.append    rows captured with the game's furniture editor (same layout as MewFurnitureFramework)
//   Mods\<id>\furniture\<Name>.txt                a furniture drawn as text, compiled here (format below)
//
// Row layout (588 bytes + name): u32 name length, u32 flags (0), name, then 580 bytes = u32 flags (0) + 24x24 cells (row-major).
// Cell values (from the game's furniture editor): 1 black = solid collision, 2 blue = surface (things can be put on it),
// 3 red = support (must overlap a surface), 4 yellow = extra clickable area, 5 brown = poop. The editor maps the pointer with
// cell = floor(position + 12): the grid is centred on the furniture's origin, x to the right, y downwards.
//
// Text file: ';' or '//' comment lines, then optionally "@at X Y" (tile of the first character, relative to the origin;
// default -12 -12 = the full 24x24 grid), then one line per row: '.' or ' ' empty, '#' solid, 'B' surface, 'R' support,
// 'Y' clickable, 'P' poop. Rows are matched by name: a name already in the game (or in an earlier mod) is skipped.
#define RVA_PACKED_LOAD 0x99FF50
#define FURN_PAYLOAD 580u
#define FURN_HEAD 8u
#define FURN_GRID 24
#define FURN_MAX_NAME 512u

typedef struct { uint8_t *p; size_t n; char src[MAX_PATH]; } FRow;
static FRow *g_frows; static int g_nfrows, g_capfrows, g_fscanned;
static CRITICAL_SECTION g_furncs;

static uint32_t fu32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void furn_add(const uint8_t *row, size_t n, const char *src) {
    if (g_nfrows == g_capfrows) { g_capfrows = g_capfrows ? g_capfrows * 2 : 16; g_frows = (FRow *)realloc(g_frows, (size_t)g_capfrows * sizeof(FRow)); }
    FRow *r = &g_frows[g_nfrows++]; r->p = (uint8_t *)malloc(n); memcpy(r->p, row, n); r->n = n; strncpy(r->src, src, MAX_PATH - 1); r->src[MAX_PATH - 1] = 0;
}
// Walks a furniture_info.data layout; returns the row count or -1 when the buffer is not valid.
static int furn_validate(const uint8_t *d, size_t n) {
    if (n < 8 || fu32(d) != 1) return -1;
    uint32_t cnt = fu32(d + 4); size_t off = 8;
    for (uint32_t i = 0; i < cnt; i++) {
        if (off + FURN_HEAD > n) return -1;
        uint32_t nl = fu32(d + off); if (!nl || nl > FURN_MAX_NAME || off + FURN_HEAD + nl + FURN_PAYLOAD > n) return -1;
        off += FURN_HEAD + nl + FURN_PAYLOAD;
    }
    return off == n ? (int)cnt : -1;
}
static int furn_cell(char c) {
    switch (c) { case '.': case ' ': return 0; case '#': return 1; case 'B': case 'b': return 2; case 'R': case 'r': return 3; case 'Y': case 'y': return 4; case 'P': case 'p': return 5; }
    return -1;
}
static void furn_compile_txt(const char *name, const uint8_t *txt, size_t n, const char *file) {
    uint8_t cells[FURN_GRID * FURN_GRID]; memset(cells, 0, sizeof cells);
    int col0 = 0, row0 = 0, row = 0, ntiles = 0, started = 0, err = 0; size_t i = 0;
    while (i < n && !err) {
        size_t e = i; while (e < n && txt[e] != '\n') e++;
        size_t len = e - i; const char *ln = (const char *)txt + i; if (len && ln[len - 1] == '\r') len--;
        i = e + 1;
        if ((len >= 1 && ln[0] == ';') || (len >= 2 && ln[0] == '/' && ln[1] == '/')) continue;
        if (len >= 1 && ln[0] == '@') {
            int x, y; char t[256]; snprintf(t, sizeof t, "%.*s", (int)len, ln);
            if (started || sscanf(t, "@at %d %d", &x, &y) != 2) { clog("[Furniture] %s: bad or misplaced directive '%s'", file, t); err = 1; break; }
            col0 = x + FURN_GRID / 2; row0 = y + FURN_GRID / 2; continue;
        }
        started = 1;
        for (size_t k = 0; k < len; k++) {
            int v = furn_cell(ln[k]); if (v < 0) { clog("[Furniture] %s: unknown character '%c' on row %d", file, ln[k], row + 1); err = 1; break; }
            if (!v) continue;
            int cx = col0 + (int)k, cy = row0 + row;
            if (cx < 0 || cx >= FURN_GRID || cy < 0 || cy >= FURN_GRID) { clog("[Furniture] %s: tile outside the %dx%d grid (row %d, column %d)", file, FURN_GRID, FURN_GRID, row + 1, (int)k + 1); err = 1; break; }
            cells[cy * FURN_GRID + cx] = (uint8_t)v; ntiles++;
        }
        row++;
    }
    if (err) return;
    if (!ntiles) { clog("[Furniture] %s: no tile drawn, ignored", file); return; }
    size_t nl = strlen(name), tot = FURN_HEAD + nl + FURN_PAYLOAD; uint8_t *r = (uint8_t *)calloc(1, tot);
    uint32_t l32 = (uint32_t)nl; memcpy(r, &l32, 4); memcpy(r + FURN_HEAD, name, nl); memcpy(r + FURN_HEAD + nl + 4, cells, sizeof cells);
    furn_add(r, tot, file); free(r);
}
static void furn_scan_dir(const char *dir) {
    static const char *bins[] = { "data\\furniture_info.data.append", "furniture_info.data.append" };
    for (int b = 0; b < 2; b++) {
        char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s", dir, bins[b]); size_t n; uint8_t *d = read_file(p, &n); if (!d) continue;
        int cnt = furn_validate(d, n);
        if (cnt < 0) clog("[Furniture] %s is not a valid furniture_info append file, ignored", p);
        else { size_t off = 8; for (int i = 0; i < cnt; i++) { size_t rs = FURN_HEAD + fu32(d + off) + FURN_PAYLOAD; furn_add(d + off, rs, p); off += rs; } clog("[Furniture] %s: %d row(s)", p, cnt); }
        free(d);
    }
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\furniture\\*.txt", dir); WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char name[MAX_PATH]; strncpy(name, fd.cFileName, MAX_PATH - 1); name[MAX_PATH - 1] = 0; char *dot = strrchr(name, '.'); if (dot) *dot = 0;
        char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\furniture\\%s", dir, fd.cFileName);
        if (!name[0] || strlen(name) > 100) { clog("[Furniture] %s: bad furniture name", p); continue; }
        size_t n; uint8_t *d = read_file(p, &n); if (!d) continue;
        furn_compile_txt(name, d, n, p); free(d);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
#define MAX_FURN_PATHS 32
static char g_furn_paths[MAX_FURN_PATHS][MAX_PATH]; static int g_nfurn_paths;
// Folders passed to the game with -modpaths (Mewtator mods): scanned like the Mods folder.
static void furn_note_modpaths(int argc, char **argv) {
    int mp = -1; for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-modpaths")) { mp = i; break; }
    if (mp < 0) return;
    char *end = NULL; long count = -1;
    if (mp + 1 < argc) { count = strtol(argv[mp + 1], &end, 0); if (!end || *end || end == argv[mp + 1]) count = -1; }
    int first = count >= 0 ? mp + 2 : mp + 1, last = count >= 0 ? mp + 2 + (int)count : argc; if (last > argc) last = argc;
    for (int i = first; i < last && g_nfurn_paths < MAX_FURN_PATHS; i++) { if (argv[i][0] == '-') break; strncpy(g_furn_paths[g_nfurn_paths++], argv[i], MAX_PATH - 1); }
}
static void furn_scan_all(void) {
    char root[MAX_PATH]; snprintf(root, sizeof root, "%s\\Mods", g_dir);
    DirEnt *es; int ne = list_dir(root, 1, &es);
    for (int i = 0; i < ne; i++) {
        if (!_stricmp(es[i].name, "config") || is_disabled(es[i].name)) continue;
        char d[MAX_PATH]; snprintf(d, sizeof d, "%s\\%s", root, es[i].name); furn_scan_dir(d);
    }
    free(es);
    for (int i = 0; i < g_nfurn_paths; i++) { char r[MAX_PATH]; snprintf(r, sizeof r, "%s\\Mods\\", g_dir); if (_strnicmp(g_furn_paths[i], r, strlen(r))) furn_scan_dir(g_furn_paths[i]); }
}
static void furn_prepare(void) {   // scans the mods once; the merge hook and the placeholder step share the result
    if (g_fscanned) return;
    g_fscanned = 1; furn_scan_all(); clog("[Furniture] %d mod row(s) found", g_nfrows);
}

// Placeholders for removed mods. A save that holds a furniture whose mod is gone makes the game stop on "GON ERROR: Field name does
// not exist" when the house loads (verified). MewCore remembers the furniture names it has merged (Mods\.cache\furniture_known.txt)
// and, for the names that no enabled mod provides any more, generates a neutral "Removed furniture" entry (effects + text through the
// resource overlay, plus an info row) so the save keeps loading; when the mod comes back the real data wins. Saves are never edited.
static int furn_valid_name(const char *s) { size_t n = strlen(s); if (!n || n > 100) return 0; for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '_') return 0; return 1; }
static char g_fknown[512][104]; static int g_nfknown;
static int fknown_add(const char *s) {
    if (!furn_valid_name(s)) return 0;
    for (int i = 0; i < g_nfknown; i++) if (!strcmp(g_fknown[i], s)) return 0;
    if (g_nfknown >= 512) return 0;
    strcpy(g_fknown[g_nfknown++], s); return 1;
}
static int furn_row_named(const char *nm) {
    size_t n = strlen(nm);
    for (int i = 0; i < g_nfrows; i++) if (fu32(g_frows[i].p) == n && !memcmp(g_frows[i].p + FURN_HEAD, nm, n)) return 1;
    return 0;
}
// Fills <outdir> (a resource folder) and returns 1 when it has content.
static int furn_ghosts(const char *outdir) {
    furn_prepare();
    char reg[MAX_PATH], dd[MAX_PATH], pa[MAX_PATH], cs[MAX_PATH]; snprintf(reg, sizeof reg, "%s\\Mods\\.cache\\furniture_known.txt", g_dir);
    snprintf(dd, sizeof dd, "%s\\data\\text", outdir); snprintf(pa, sizeof pa, "%s\\data\\furniture_effects.gon.patch", outdir); snprintf(cs, sizeof cs, "%s\\data\\text\\combined.csv.append", outdir);
    size_t n; uint8_t *d = read_file(reg, &n);
    if (d) { char *t = (char *)malloc(n + 1); memcpy(t, d, n); t[n] = 0; free(d); for (char *l = strtok(t, "\r\n"); l; l = strtok(NULL, "\r\n")) fknown_add(l); free(t); }
    int before = g_nfknown;
    for (int i = 0; i < g_nfrows; i++) { char nm[104]; uint32_t nl = fu32(g_frows[i].p); if (nl < sizeof nm) { memcpy(nm, g_frows[i].p + FURN_HEAD, nl); nm[nl] = 0; fknown_add(nm); } }
    if (g_nfknown != before) {
        char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s\\Mods\\.cache", g_dir); mkdirs(dir);
        Buf b = {0}; for (int i = 0; i < g_nfknown; i++) { bput(&b, g_fknown[i], strlen(g_fknown[i])); bput(&b, "\n", 1); }
        write_file(reg, b.p, b.n); free(b.p);
    }
    Buf pt = {0}; int ng = 0;
    for (int i = 0; i < g_nfknown; i++) {
        if (furn_row_named(g_fknown[i])) continue;
        char e[512]; int k = snprintf(e, sizeof e, "%s {\r\n    name FURNITURE_GHOST_NAME\r\n    desc FURNITURE_GHOST_DESC\r\n}\r\n", g_fknown[i]); bput(&pt, e, (size_t)k);
        uint8_t row[FURN_HEAD + 104 + FURN_PAYLOAD] = {0}; size_t nl = strlen(g_fknown[i]); uint32_t l32 = (uint32_t)nl;
        memcpy(row, &l32, 4); memcpy(row + FURN_HEAD, g_fknown[i], nl); row[FURN_HEAD + nl + 4 + 12 * FURN_GRID + 11] = 1;   // one solid tile at the origin
        furn_add(row, FURN_HEAD + nl + FURN_PAYLOAD, "removed furniture"); ng++;
    }
    if (!ng) { free(pt.p); DeleteFileA(pa); DeleteFileA(cs); return 0; }
    static const char t1[] = "Removed furniture", t2[] = "Added by a mod that is not installed.";
    char csv[1024]; int k = snprintf(csv, sizeof csv, "FURNITURE_GHOST_NAME,%s,,%s,%s,%s,%s,%s,%s,%s,%s,%s\r\nFURNITURE_GHOST_DESC,%s,,%s,%s,%s,%s,%s,%s,%s,%s,%s\r\n", t1, t1, t1, t1, t1, t1, t1, t1, t1, t1, t2, t2, t2, t2, t2, t2, t2, t2, t2, t2);
    mkdirs(dd); int ok = write_file(pa, pt.p, pt.n) && write_file(cs, csv, (size_t)k); free(pt.p);
    clog("[Furniture] %d furniture of removed mods kept as placeholders", ng);
    return ok;
}

typedef struct { const uint8_t *p; uint32_t len; } FName;
static int name_known(const FName *l, int n, const uint8_t *nm, uint32_t len) {
    for (int i = 0; i < n; i++) if (l[i].len == len && !memcmp(l[i].p, nm, len)) return 1;
    return 0;
}
// Returns a new buffer with the mod rows appended to <base> (NULL when there is nothing to add).
static uint8_t *furn_merge(const uint8_t *base, size_t bn, size_t *outn) {
    int cnt = furn_validate(base, bn); if (cnt < 0) { clog("[Furniture] the game's furniture_info.data has an unexpected layout, not merged"); return NULL; }
    FName *names = (FName *)malloc(((size_t)cnt + (size_t)g_nfrows + 1) * sizeof(FName)); int nn = 0; size_t off = 8;
    for (int i = 0; i < cnt; i++) { uint32_t nl = fu32(base + off); names[nn].p = base + off + FURN_HEAD; names[nn++].len = nl; off += FURN_HEAD + nl + FURN_PAYLOAD; }
    size_t add = 0; int *pick = (int *)malloc(((size_t)g_nfrows + 1) * sizeof(int)); int np = 0;
    for (int i = 0; i < g_nfrows; i++) {
        const uint8_t *r = g_frows[i].p; uint32_t nl = fu32(r);
        if (name_known(names, nn, r + FURN_HEAD, nl)) { clog("[Furniture] %.*s already exists, skipped (%s)", (int)nl, r + FURN_HEAD, g_frows[i].src); continue; }
        names[nn].p = r + FURN_HEAD; names[nn++].len = nl; pick[np++] = i; add += g_frows[i].n;
    }
    uint8_t *out = NULL;
    if (np) {
        out = (uint8_t *)malloc(bn + add); memcpy(out, base, bn); size_t o = bn;
        for (int k = 0; k < np; k++) { memcpy(out + o, g_frows[pick[k]].p, g_frows[pick[k]].n); o += g_frows[pick[k]].n; clog("[Furniture] added %.*s", (int)fu32(g_frows[pick[k]].p), g_frows[pick[k]].p + FURN_HEAD); }
        uint32_t total = (uint32_t)(cnt + np); memcpy(out + 4, &total, 4); *outn = o;
        clog("[Furniture] furniture_info.data: %d + %d = %d rows", cnt, np, cnt + np);
    }
    free(names); free(pick); return out;
}

typedef void *(__fastcall *PackedLoadFn)(void *ctx, void *path, uint8_t useCache, uint8_t allowFallback);
static PackedLoadFn o_furn_load;
static int furn_is_info(const void *path) {
    const char *s = (const char *)path; size_t len = *(const size_t *)(s + 16), cap = *(const size_t *)(s + 24);
    const char *p = cap > 15 ? *(const char *const *)s : s; static const char suf[] = "furniture_info.data"; size_t sl = sizeof suf - 1;
    if (len < sl || len > 260 || _strnicmp(p + len - sl, suf, sl)) return 0;
    return len == sl || p[len - sl - 1] == '/' || p[len - sl - 1] == '\\';
}
static void *__fastcall h_furn_load(void *ctx, void *path, uint8_t uc, uint8_t af) {
    int info = furn_is_info(path);   // the loader moves the path string away, so it has to be read before the call
    void *res = o_furn_load(ctx, path, uc, af);
    if (!res || !info) return res;
    EnterCriticalSection(&g_furncs);
    __try {
        uint8_t **pdata = (uint8_t **)((uint8_t *)res + 0x20); uint32_t *psize = (uint32_t *)((uint8_t *)res + 0x28); uint8_t *owns = (uint8_t *)res + 0x2C;
        furn_prepare();
        if (g_nfrows && *pdata && *psize) {
            size_t n = 0; uint8_t *m = furn_merge(*pdata, *psize, &n);
            if (m) {
                *pdata = m; *psize = (uint32_t)n; *owns = 0;
                if (getenv("MEW_FURNITURE_DUMP")) { char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\Mods\\.cache", g_dir); mkdirs(p); strcat(p, "\\furniture_info.merged.data"); write_file(p, m, n); }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { clog("[Furniture] merge failed (0x%lX), the game keeps its own data", GetExceptionCode()); }
    LeaveCriticalSection(&g_furncs);
    return res;
}
static void furn_install(void) {
    if (!g_cfg_furniture) { clog("[Furniture] disabled (furniture=0)"); return; }
    InitializeCriticalSection(&g_furncs);
    void *next = chain_install(RVA_PACKED_LOAD, h_furn_load, 30, "MewCore");
    if (next) { o_furn_load = (PackedLoadFn)next; clog("[Furniture] merge hook ready"); } else clog("[Furniture] merge hook failed");
}
