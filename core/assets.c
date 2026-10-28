// Sprite compiler (included by core.c).
//
//   Mods\<id>\sprites\<Clip>\<Label>.png            a frame of the game's clip <Clip> (AbilityIcon, PassiveIcon, Furniture, ...)
//   Mods\<id>\sprites\<Clip>\<Label>\01.png 02.png  an animation (frames in name order, 12 fps unless the name has @fpsN)
//   Mods\<id>\sprites\Items\<kind>\<Label>.png      the picture of an item (kind = trinket, weapon, head, face, neck)
//   Name modifiers (after the label, separated by @): @center, @topleft, @fpsN   e.g. Sword@center@fps8
//
// Everything is compiled at launch into SWF files in Mods\.cache and handed to the game's mod overlay:
// every <Clip> becomes the library symbol "_Append_<Clip>" whose frames the game appends to the existing clip.
// Clips addressed by frame NUMBER (item pictures) get the next free numbers, written to Mods\.cache\frames.map and
// substituted for @frame(kind/Label) in Mods\<id>\data\**\*.tpl files. See re/NOTES.md.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb/stb_image.h"

#define ASSETS_VERSION 2u
#define SWF_FPS 60

typedef struct { uint8_t *p; size_t n, cap; } Buf;
static void bput(Buf *b, const void *d, size_t n) {
    if (!n) return;
    if (b->n + n > b->cap) { size_t c = b->cap ? b->cap : 4096; while (c < b->n + n) c *= 2; b->p = (uint8_t *)realloc(b->p, c); b->cap = c; }
    if (d) memcpy(b->p + b->n, d, n); else memset(b->p + b->n, 0, n);
    b->n += n;
}
static void bu8(Buf *b, unsigned v) { uint8_t x = (uint8_t)v; bput(b, &x, 1); }
static void bu16(Buf *b, unsigned v) { bu8(b, v & 255); bu8(b, (v >> 8) & 255); }
static void bu32(Buf *b, uint32_t v) { bu16(b, v & 0xFFFF); bu16(b, v >> 16); }

typedef struct { Buf b; unsigned cur, n; } BitW;
static void bw_bits(BitW *w, uint64_t v, int n) {
    for (int i = n - 1; i >= 0; i--) { w->cur = (w->cur << 1) | (unsigned)((v >> i) & 1); if (++w->n == 8) { bu8(&w->b, w->cur); w->cur = 0; w->n = 0; } }
}
static void bw_signed(BitW *w, int64_t v, int n) { bw_bits(w, (uint64_t)v & ((n >= 64) ? ~0ull : ((1ull << n) - 1)), n); }
static void bw_align(BitW *w) { if (w->n) { bu8(&w->b, w->cur << (8 - w->n)); w->cur = 0; w->n = 0; } }
static int sbits1(int64_t v) { int k = 1; while (!(v >= -(1ll << (k - 1)) && v < (1ll << (k - 1)))) k++; return k; }
static int sbits4(int64_t a, int64_t b, int64_t c, int64_t d) { int m = sbits1(a), k; if ((k = sbits1(b)) > m) m = k; if ((k = sbits1(c)) > m) m = k; if ((k = sbits1(d)) > m) m = k; return m; }

static void put_tag(Buf *out, int type, const uint8_t *body, size_t len) {
    if (len < 63) bu16(out, ((unsigned)type << 6) | (unsigned)len);
    else { bu16(out, ((unsigned)type << 6) | 63); bu32(out, (uint32_t)len); }
    bput(out, body, len);
}
static void put_rect(Buf *out, int x0, int x1, int y0, int y1) {
    BitW w = {0}; int n = sbits4(x0, x1, y0, y1); bw_bits(&w, (uint64_t)n, 5);
    bw_signed(&w, x0, n); bw_signed(&w, x1, n); bw_signed(&w, y0, n); bw_signed(&w, y1, n); bw_align(&w);
    bput(out, w.b.p, w.b.n); free(w.b.p);
}

// zlib stream made of stored (uncompressed) deflate blocks: valid, simple, only used for the local cache
static void zlib_store(Buf *out, const uint8_t *d, size_t n) {
    bu8(out, 0x78); bu8(out, 0x01);
    size_t pos = 0; uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
    do {
        size_t k = n - pos > 65535 ? 65535 : n - pos;
        bu8(out, pos + k >= n ? 1 : 0); bu16(out, (unsigned)k); bu16(out, (~(unsigned)k) & 0xFFFF);
        bput(out, d + pos, k); pos += k;
    } while (pos < n);
    bu8(out, (b >> 8) & 255); bu8(out, b & 255); bu8(out, (a >> 8) & 255); bu8(out, a & 255);   // Adler-32, big endian
}

// ---- file helpers ----
static uint64_t fnv(uint64_t h, const void *d, size_t n) { const uint8_t *p = (const uint8_t *)d; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }
static void mkdirs(const char *path) {
    char t[MAX_PATH]; strncpy(t, path, MAX_PATH - 1); t[MAX_PATH - 1] = 0;
    for (char *p = t + 3; *p; p++) if (*p == '\\' || *p == '/') { char c = *p; *p = 0; CreateDirectoryA(t, NULL); *p = c; }
    CreateDirectoryA(t, NULL);
}
static int write_file(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb"); if (!f) return 0; size_t w = n ? fwrite(d, 1, n, f) : 0; fclose(f); return w == n;
}
static uint8_t *read_file(const char *path, size_t *n) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL); if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL); uint8_t *b = (uint8_t *)malloc(sz ? sz : 1); DWORD got = 0;
    if (!b || !ReadFile(h, b, sz, &got, NULL) || got != sz) { free(b); CloseHandle(h); return NULL; }
    CloseHandle(h); *n = sz; return b;
}
static void clean_id(const char *in, char *out, size_t cap) {
    size_t i = 0; for (; in[i] && i + 1 < cap; i++) out[i] = (isalnum((unsigned char)in[i]) || in[i] == '_') ? in[i] : '_'; out[i] = 0;
}
typedef struct { char name[MAX_PATH]; int dir; uint64_t stamp; } DirEnt;
static int cmp_ent(const void *a, const void *b) { return _stricmp(((const DirEnt *)a)->name, ((const DirEnt *)b)->name); }
// Entries of a folder sorted by name (ASCII case-insensitive); filter: 1 = folders only, 2 = *.png files only, 3 = both
static int list_dir(const char *dir, int filter, DirEnt **out) {
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*", dir); WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    int n = 0, cap = 0; DirEnt *l = NULL; *out = NULL; if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == '.') continue;
        int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        size_t ln = strlen(fd.cFileName); int png = !isdir && ln > 4 && !_stricmp(fd.cFileName + ln - 4, ".png");
        if (!((isdir && (filter & 1)) || (png && (filter & 2)))) continue;
        if (n == cap) { cap = cap ? cap * 2 : 32; l = (DirEnt *)realloc(l, (size_t)cap * sizeof(DirEnt)); }
        strncpy(l[n].name, fd.cFileName, MAX_PATH - 1); l[n].name[MAX_PATH - 1] = 0; l[n].dir = isdir;
        l[n].stamp = ((uint64_t)fd.nFileSizeLow << 32) ^ fd.ftLastWriteTime.dwLowDateTime ^ ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 17); n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h); if (n > 1) qsort(l, (size_t)n, sizeof(DirEnt), cmp_ent); *out = l; return n;
}

// ---- sprite descriptions ----
typedef struct { char clip[96], label[160]; int anchor /* -1 default, 0 top-left, 1 center */, fps, nimg; char (*img)[MAX_PATH]; uint64_t stamp; char mod[96]; } Spr;
static void free_sprs(Spr *l, int n) { for (int i = 0; i < n; i++) free(l[i].img); free(l); }
static void parse_name(const char *file, char *label, size_t cap, int *anchor, int *fps) {
    char t[MAX_PATH]; strncpy(t, file, MAX_PATH - 1); t[MAX_PATH - 1] = 0;
    char *dot = strrchr(t, '.'); if (dot && !_stricmp(dot, ".png")) *dot = 0;
    *anchor = -1; *fps = 12;
    char *at = strchr(t, '@'); if (!at) { strncpy(label, t, cap - 1); label[cap - 1] = 0; return; }
    *at = 0; strncpy(label, t, cap - 1); label[cap - 1] = 0;
    for (char *tok = strtok(at + 1, "@"); tok; tok = strtok(NULL, "@")) {
        if (!_stricmp(tok, "center") || !_stricmp(tok, "c")) *anchor = 1;
        else if (!_stricmp(tok, "topleft") || !_stricmp(tok, "tl")) *anchor = 0;
        else if (!_strnicmp(tok, "fps", 3) && atoi(tok + 3) > 0) { *fps = atoi(tok + 3); if (*fps > SWF_FPS) *fps = SWF_FPS; }
    }
}
// Adds the sprites found in <dir> (one folder of the sprites tree) under the clip name <clip>
static void gather_clip(const char *id, const char *dir, const char *clip, Spr **list, int *n, int *cap) {
    DirEnt *es; int ne = list_dir(dir, 3, &es);
    for (int i = 0; i < ne; i++) {
        if (*n == *cap) { *cap = *cap ? *cap * 2 : 64; *list = (Spr *)realloc(*list, (size_t)*cap * sizeof(Spr)); }
        Spr *s = &(*list)[(*n)++]; memset(s, 0, sizeof *s);
        strncpy(s->clip, clip, sizeof s->clip - 1); strncpy(s->mod, id, sizeof s->mod - 1);
        parse_name(es[i].name, s->label, sizeof s->label, &s->anchor, &s->fps); s->stamp = es[i].stamp;
        char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s", dir, es[i].name);
        if (!es[i].dir) { s->nimg = 1; s->img = (char (*)[MAX_PATH])malloc(MAX_PATH); strcpy(s->img[0], p); continue; }
        DirEnt *fr; int nf = list_dir(p, 2, &fr);
        s->img = (char (*)[MAX_PATH])malloc((size_t)(nf ? nf : 1) * MAX_PATH);
        for (int k = 0; k < nf; k++) { snprintf(s->img[s->nimg++], MAX_PATH, "%s\\%s", p, fr[k].name); s->stamp = fnv(s->stamp, &fr[k].stamp, 8); }
        free(fr);
        if (!nf) { (*n)--; free(s->img); }
    }
    free(es);
}
static uint64_t sig_sprs(uint64_t sig, const Spr *l, int n) {
    for (int i = 0; i < n; i++) { sig = fnv(sig, l[i].clip, strlen(l[i].clip)); sig = fnv(sig, l[i].label, strlen(l[i].label)); sig = fnv(sig, &l[i].stamp, 8); sig = fnv(sig, &l[i].anchor, 4); sig = fnv(sig, &l[i].fps, 4); sig = fnv(sig, &l[i].nimg, 4); }
    return sig;
}

// ---- SWF writer ----
typedef struct { Buf tags, sym; int nid, nsym; struct { char path[MAX_PATH]; int anchor, shape; } img[256]; int nimg; } Sw;
static void sw_init(Sw *w) { memset(w, 0, sizeof *w); w->nid = 1; uint8_t fa[4] = { 8, 0, 0, 0 }, bg[3] = { 255, 255, 255 }; put_tag(&w->tags, 69, fa, 4); put_tag(&w->tags, 9, bg, 3); }
// One picture = DefineBitsLossless2 (premultiplied ARGB) + DefineShape2 (a rectangle filled with the bitmap, 1 pixel = 1 stage pixel)
static int sw_image(Sw *w, const char *path, int anchor, const char *id) {
    for (int i = 0; i < w->nimg; i++) if (w->img[i].anchor == anchor && !_stricmp(w->img[i].path, path)) return w->img[i].shape;
    size_t fn = 0; uint8_t *fb = read_file(path, &fn); int pw = 0, ph = 0, ch = 0; uint8_t *px = fb ? stbi_load_from_memory(fb, (int)fn, &pw, &ph, &ch, 4) : NULL; free(fb);
    if (!px || pw <= 0 || ph <= 0 || pw > 4096 || ph > 4096) { clog("[Assets] %s: cannot use %s (%s)", id, path, px ? "size must be 1..4096" : stbi_failure_reason()); if (px) stbi_image_free(px); return 0; }
    int bid = w->nid++, sid = w->nid++; size_t npx = (size_t)pw * (size_t)ph;
    Buf raw = {0}; bput(&raw, NULL, npx * 4);
    for (size_t i = 0; i < npx; i++) { unsigned a = px[i * 4 + 3]; raw.p[i * 4] = (uint8_t)a; raw.p[i * 4 + 1] = (uint8_t)(px[i * 4] * a / 255); raw.p[i * 4 + 2] = (uint8_t)(px[i * 4 + 1] * a / 255); raw.p[i * 4 + 3] = (uint8_t)(px[i * 4 + 2] * a / 255); }
    Buf body = {0}; bu16(&body, (unsigned)bid); bu8(&body, 5); bu16(&body, (unsigned)pw); bu16(&body, (unsigned)ph); zlib_store(&body, raw.p, raw.n);
    put_tag(&w->tags, 36, body.p, body.n); free(raw.p); free(body.p); stbi_image_free(px);
    int W = pw * 20, H = ph * 20, ox = anchor == 1 ? -W / 2 : 0, oy = anchor == 1 ? -H / 2 : 0;
    Buf sh = {0}; bu16(&sh, (unsigned)sid); put_rect(&sh, ox, ox + W, oy, oy + H);
    bu8(&sh, 1); bu8(&sh, 0x41); bu16(&sh, (unsigned)bid);
    BitW m = {0}; int tb = sbits4(ox, oy, 0, 0);
    bw_bits(&m, 1, 1); bw_bits(&m, 22, 5); bw_signed(&m, 20 * 65536, 22); bw_signed(&m, 20 * 65536, 22); bw_bits(&m, 0, 1); bw_bits(&m, (uint64_t)tb, 5); bw_signed(&m, ox, tb); bw_signed(&m, oy, tb); bw_align(&m);
    bput(&sh, m.b.p, m.b.n); free(m.b.p);
    bu8(&sh, 0); bu8(&sh, 0x10);
    BitW s = {0}; int mb = sbits4(ox, oy, 0, 0);
    bw_bits(&s, 0, 1); bw_bits(&s, 0, 1); bw_bits(&s, 0, 1); bw_bits(&s, 1, 1); bw_bits(&s, 0, 1); bw_bits(&s, 1, 1);
    bw_bits(&s, (uint64_t)mb, 5); bw_signed(&s, ox, mb); bw_signed(&s, oy, mb); bw_bits(&s, 1, 1);
    int edges[4][2] = { { W, 0 }, { 0, H }, { -W, 0 }, { 0, -H } };
    for (int i = 0; i < 4; i++) { int nb = sbits4(edges[i][0], edges[i][1], 0, 0); bw_bits(&s, 1, 1); bw_bits(&s, 1, 1); bw_bits(&s, (uint64_t)(nb - 2), 4); bw_bits(&s, 1, 1); bw_signed(&s, edges[i][0], nb); bw_signed(&s, edges[i][1], nb); }
    bw_bits(&s, 0, 6); bw_align(&s); bput(&sh, s.b.p, s.b.n); free(s.b.p);
    put_tag(&w->tags, 22, sh.p, sh.n); free(sh.p);
    if (w->nimg < 256) { strcpy(w->img[w->nimg].path, path); w->img[w->nimg].anchor = anchor; w->img[w->nimg].shape = sid; w->nimg++; }
    return sid;
}
static void put_place(Buf *b, int ch) { uint8_t po[5] = { 0x02, 1, 0, (uint8_t)(ch & 255), (uint8_t)(ch >> 8) }; put_tag(b, 26, po, 5); }
static void put_remove(Buf *b) { uint8_t rm[2] = { 1, 0 }; put_tag(b, 28, rm, 2); }
// Character shown by a sprite description: its picture, or a looping animation clip for several pictures. 0 = nothing usable.
static int sw_sprite_char(Sw *w, const Spr *s, int defanchor, const char *id) {
    int anchor = s->anchor >= 0 ? s->anchor : defanchor;
    if (s->nimg == 1) return sw_image(w, s->img[0], anchor, id);
    int ids[512], n = 0; for (int i = 0; i < s->nimg && n < 512; i++) { int sh = sw_image(w, s->img[i], anchor, id); if (sh) ids[n++] = sh; }
    if (!n) return 0;
    int hold = SWF_FPS / (s->fps > 0 ? s->fps : 12); if (hold < 1) hold = 1;
    Buf inner = {0}; int frames = 0;
    for (int i = 0; i < n; i++) { if (i > 0) put_remove(&inner); put_place(&inner, ids[i]); for (int h = 0; h < hold; h++) { put_tag(&inner, 1, NULL, 0); frames++; } }
    put_tag(&inner, 0, NULL, 0);
    Buf sp = {0}; int cid = w->nid++; bu16(&sp, (unsigned)cid); bu16(&sp, (unsigned)frames); bput(&sp, inner.p, inner.n); put_tag(&w->tags, 39, sp.p, sp.n); free(sp.p); free(inner.p);
    return cid;
}
typedef struct { const char *label; int ch; } Frm;   // label NULL = unlabelled, ch 0 = blank frame
static void sw_clip(Sw *w, const char *clip, const Frm *f, int n) {
    if (n <= 0) return;
    Buf inner = {0}; int shown = 0;
    for (int i = 0; i < n; i++) {
        if (shown) put_remove(&inner);
        if (f[i].label && f[i].label[0]) { Buf lb = {0}; bput(&lb, f[i].label, strlen(f[i].label) + 1); put_tag(&inner, 43, lb.p, lb.n); free(lb.p); }
        if (f[i].ch) put_place(&inner, f[i].ch);
        shown = f[i].ch != 0; put_tag(&inner, 1, NULL, 0);
    }
    put_tag(&inner, 0, NULL, 0);
    int cid = w->nid++; Buf sp = {0}; bu16(&sp, (unsigned)cid); bu16(&sp, (unsigned)n); bput(&sp, inner.p, inner.n); put_tag(&w->tags, 39, sp.p, sp.n); free(sp.p); free(inner.p);
    bu16(&w->sym, (unsigned)cid); char nm[200]; snprintf(nm, sizeof nm, "_Append_%s", clip); bput(&w->sym, nm, strlen(nm) + 1); w->nsym++;
}
// Finishes the SWF and writes it with a swflist.gon.append next to it: <cachedir>\swfs\mewgen_<cid>.swf
static int sw_write(Sw *w, const char *cachedir, const char *cid) {
    if (w->nsym == 0) return 0;
    Buf sc = {0}; bu16(&sc, (unsigned)w->nsym); bput(&sc, w->sym.p, w->sym.n); put_tag(&w->tags, 76, sc.p, sc.n); free(sc.p);
    put_tag(&w->tags, 1, NULL, 0); put_tag(&w->tags, 0, NULL, 0);
    Buf body = {0}; put_rect(&body, 0, 25600, 0, 14400); bu16(&body, SWF_FPS * 256); bu16(&body, 1); bput(&body, w->tags.p, w->tags.n);
    Buf swf = {0}; bput(&swf, "FWS", 3); bu8(&swf, 15); bu32(&swf, (uint32_t)(body.n + 8)); bput(&swf, body.p, body.n);
    char dir[MAX_PATH], sp[MAX_PATH], lp[MAX_PATH], gl[512]; snprintf(dir, sizeof dir, "%s\\swfs", cachedir); mkdirs(dir);
    snprintf(sp, sizeof sp, "%s\\mewgen_%s.swf", dir, cid); snprintf(lp, sizeof lp, "%s\\swflist.gon.append", dir); snprintf(gl, sizeof gl, "game [\n    mewgen_%s.swf\n]\n", cid);
    int ok = write_file(sp, swf.p, swf.n) && write_file(lp, gl, strlen(gl));
    clog("[Assets] wrote %s (%u bytes)%s", sp, (unsigned)swf.n, ok ? "" : " - FAILED");
    free(body.p); free(swf.p); free(w->tags.p); free(w->sym.p); return ok;
}
static int cache_fresh(const char *cachedir, const char *cid, uint64_t sig) {
    char sg[32], p[MAX_PATH], sp[MAX_PATH]; snprintf(sg, sizeof sg, "%016llx", (unsigned long long)sig);
    snprintf(p, sizeof p, "%s\\.sig", cachedir); snprintf(sp, sizeof sp, "%s\\swfs\\mewgen_%s.swf", cachedir, cid);
    size_t n = 0; uint8_t *o = read_file(p, &n); int ok = o && n == 16 && !memcmp(o, sg, 16) && GetFileAttributesA(sp) != INVALID_FILE_ATTRIBUTES; free(o); return ok;
}
static void cache_stamp(const char *cachedir, uint64_t sig) {
    char sg[32], p[MAX_PATH]; snprintf(sg, sizeof sg, "%016llx", (unsigned long long)sig); snprintf(p, sizeof p, "%s\\.sig", cachedir); write_file(p, sg, 16);
}

// ---- item pictures: families of clips indexed by the same frame number ----
// Frame numbers start at 1. The pictures of all mods get the next free numbers after the game's own frames.
typedef struct { const char *name; int len; int icon; } FamClip;
typedef struct { const char *kind; int ncl; FamClip cl[5]; } Family;
static const Family g_fam[] = {   // frame counts of build 1.1.21239 (swfs/catparts.swf)
    { "trinket", 4, { { "TrinketIcon", 555, 1 }, { "TrinketIcon_Worn", 555, 1 }, { "TrinketIcon_Broken", 555, 1 }, { "Trinket", 400, 0 } } },
    { "weapon",  4, { { "WeaponIcon", 342, 1 }, { "WeaponIcon_Worn", 342, 1 }, { "WeaponIcon_Broken", 342, 1 }, { "Weapon", 400, 0 } } },
    { "head",    5, { { "HeadItemIcon", 340, 1 }, { "HeadItemIcon_Worn", 340, 1 }, { "HeadItemIcon_Broken", 340, 1 }, { "HeadItemF", 302, 0 }, { "HeadItemB", 303, 0 } } },
    { "face",    5, { { "FaceItemIcon", 340, 1 }, { "FaceItemIcon_Worn", 400, 1 }, { "FaceItemIcon_Broken", 340, 1 }, { "FaceItemF", 212, 0 }, { "FaceItemB", 225, 0 } } },
    { "neck",    5, { { "NeckItemIcon", 340, 1 }, { "NeckItemIcon_Worn", 340, 1 }, { "NeckItemIcon_Broken", 340, 1 }, { "NeckItemF", 215, 0 }, { "NeckItemB", 210, 0 } } },
};
#define NFAM ((int)(sizeof g_fam / sizeof g_fam[0]))
static struct { char key[260]; int frame; } g_frames[2048]; static int g_nframes;   // "kind/Label" -> frame number
static int frame_of(const char *key) { for (int i = 0; i < g_nframes; i++) if (!_stricmp(g_frames[i].key, key)) return g_frames[i].frame; return 0; }

// names: mod folder names (already filtered and sorted). Returns 1 when the shared item cache holds a SWF.
static int assets_items(const char *modsroot, char (*names)[MAX_PATH], int nmods, const char *cachedir) {
    Spr *all = NULL; int n = 0, cap = 0; g_nframes = 0;
    for (int m = 0; m < nmods; m++) {
        char d[MAX_PATH]; snprintf(d, sizeof d, "%s\\%s\\sprites\\Items", modsroot, names[m]);
        for (int f = 0; f < NFAM; f++) { char kd[MAX_PATH]; snprintf(kd, sizeof kd, "%s\\%s", d, g_fam[f].kind); gather_clip(names[m], kd, g_fam[f].kind, &all, &n, &cap); }
    }
    char mp[MAX_PATH]; snprintf(mp, sizeof mp, "%s\\frames.map", cachedir);
    if (n == 0) { DeleteFileA(mp); free(all); return 0; }
    int ok = 0, usedfam = 0;
    uint64_t sig = fnv(1469598103934665603ull, "items", 5); sig = fnv(sig, &(uint32_t){ ASSETS_VERSION }, 4); sig = sig_sprs(sig, all, n);
    Buf map = {0};
    int base[NFAM]; int count[NFAM];
    for (int f = 0; f < NFAM; f++) {
        int mx = 0; for (int c = 0; c < g_fam[f].ncl; c++) if (g_fam[f].cl[c].len > mx) mx = g_fam[f].cl[c].len;
        base[f] = mx + 1; count[f] = 0;
        for (int i = 0; i < n; i++) if (!_stricmp(all[i].clip, g_fam[f].kind)) {
            char key[260]; snprintf(key, sizeof key, "%s/%s", g_fam[f].kind, all[i].label);
            if (frame_of(key)) { clog("[Assets] item picture %s of %s ignored: %s is already defined by %s", all[i].label, all[i].mod, key, "an earlier mod"); continue; }
            if (g_nframes < 2048) { strcpy(g_frames[g_nframes].key, key); g_frames[g_nframes].frame = base[f] + count[f]; g_nframes++; }
            count[f]++;
        }
        if (count[f]) usedfam++;
    }
    mkdirs(cachedir);
    for (int i = 0; i < g_nframes; i++) { char line[300]; int l = snprintf(line, sizeof line, "%s=%d\n", g_frames[i].key, g_frames[i].frame); bput(&map, line, (size_t)l); }
    write_file(mp, map.p, map.n); free(map.p);
    if (!usedfam) { free_sprs(all, n); return 0; }
    if (cache_fresh(cachedir, "items", sig)) { clog("[Assets] item pictures: %d frames, cache up to date", g_nframes); free_sprs(all, n); return 1; }
    Sw w; sw_init(&w);
    for (int f = 0; f < NFAM; f++) {
        if (!count[f]) continue;
        int ids[2048]; Frm *fr; int nf = 0; const char *seen[2048]; int nseen = 0;
        for (int i = 0; i < n && nf < 2048; i++) if (!_stricmp(all[i].clip, g_fam[f].kind)) {
            int dup = 0; for (int k = 0; k < nseen; k++) if (!_stricmp(seen[k], all[i].label)) dup = 1;
            if (dup) continue; seen[nseen++] = all[i].label; ids[nf++] = sw_sprite_char(&w, &all[i], 1, all[i].mod);
        }
        fr = (Frm *)calloc((size_t)(base[f] + nf), sizeof(Frm));
        for (int c = 0; c < g_fam[f].ncl; c++) {
            int nfr = 0; for (int p = 0; p < base[f] - 1 - g_fam[f].cl[c].len; p++) { fr[nfr].label = NULL; fr[nfr].ch = 0; nfr++; }
            for (int k = 0; k < nf; k++) { fr[nfr].label = NULL; fr[nfr].ch = g_fam[f].cl[c].icon ? ids[k] : 0; nfr++; }
            sw_clip(&w, g_fam[f].cl[c].name, fr, nfr);
        }
        free(fr);
    }
    ok = sw_write(&w, cachedir, "items"); if (ok) cache_stamp(cachedir, sig);
    clog("[Assets] item pictures: %d frames in %d families%s", g_nframes, usedfam, ok ? "" : " - FAILED");
    free_sprs(all, n); return ok;
}

// ---- templates: Mods\<id>\data\**\<name>.tpl -> <cache>\data\**\<name> with @frame(kind/Label) replaced ----
static int make_template(const char *src, const char *dst, const char *id) {
    size_t n = 0; uint8_t *in = read_file(src, &n); if (!in) return 0;
    Buf out = {0}; size_t i = 0;
    while (i < n) {
        if (n - i > 8 && !memcmp(in + i, "@frame(", 7)) {
            size_t j = i + 7; while (j < n && in[j] != ')' && in[j] != '\n') j++;
            if (j < n && in[j] == ')') {
                char key[260]; size_t kl = j - (i + 7); if (kl >= sizeof key) kl = sizeof key - 1; memcpy(key, in + i + 7, kl); key[kl] = 0;
                int fr = frame_of(key); if (!fr) { clog("[Assets] %s: @frame(%s) has no matching picture (sprites\\Items\\...); using 1", id, key); fr = 1; }
                char num[16]; int l = snprintf(num, sizeof num, "%d", fr); bput(&out, num, (size_t)l); i = j + 1; continue;
            }
        }
        bput(&out, in + i, 1); i++;
    }
    char dir[MAX_PATH]; strncpy(dir, dst, MAX_PATH - 1); dir[MAX_PATH - 1] = 0; char *sl = strrchr(dir, '\\'); if (sl) { *sl = 0; mkdirs(dir); }
    int ok = write_file(dst, out.p, out.n); free(in); free(out.p); return ok;
}
static int templates_in(const char *srcdir, const char *dstdir, const char *id) {
    DirEnt *es; int ne = list_dir(srcdir, 1, &es), made = 0;
    for (int i = 0; i < ne; i++) { char s[MAX_PATH], d[MAX_PATH]; snprintf(s, sizeof s, "%s\\%s", srcdir, es[i].name); snprintf(d, sizeof d, "%s\\%s", dstdir, es[i].name); made += templates_in(s, d, id); }
    free(es);
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*.tpl", srcdir); WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) { do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char s[MAX_PATH], d[MAX_PATH]; snprintf(s, sizeof s, "%s\\%s", srcdir, fd.cFileName); snprintf(d, sizeof d, "%s\\%s", dstdir, fd.cFileName); d[strlen(d) - 4] = 0;
        if (make_template(s, d, id)) made++;
    } while (FindNextFileA(h, &fd)); FindClose(h); }
    return made;
}

// ---- named clips of one mod (AbilityIcon, PassiveIcon, Furniture, ...) and templates. Returns 1 when <cachedir> has content. ----
static int assets_mod(const char *moddir, const char *id, const char *cachedir) {
    int has = 0; char cid[96]; clean_id(id, cid, sizeof cid);
    char sdir[MAX_PATH], tdir[MAX_PATH], tdst[MAX_PATH];
    snprintf(tdir, sizeof tdir, "%s\\data", moddir); snprintf(tdst, sizeof tdst, "%s\\data", cachedir);
    if (GetFileAttributesA(tdir) != INVALID_FILE_ATTRIBUTES && templates_in(tdir, tdst, id) > 0) has = 1;
    snprintf(sdir, sizeof sdir, "%s\\sprites", moddir);
    DirEnt *clips; int nc = list_dir(sdir, 1, &clips); Spr *l = NULL; int n = 0, cap = 0;
    for (int c = 0; c < nc; c++) { if (!_stricmp(clips[c].name, "Items")) continue; char cd[MAX_PATH]; snprintf(cd, sizeof cd, "%s\\%s", sdir, clips[c].name); gather_clip(id, cd, clips[c].name, &l, &n, &cap); }
    free(clips); if (n == 0) { free(l); return has; }
    uint64_t sig = fnv(1469598103934665603ull, "mod", 3); sig = fnv(sig, &(uint32_t){ ASSETS_VERSION }, 4); sig = sig_sprs(sig, l, n);
    if (cache_fresh(cachedir, cid, sig)) { clog("[Assets] %s: %d sprites, cache up to date", id, n); free_sprs(l, n); return 1; }
    Sw w; sw_init(&w); int shown = 0;
    for (int i = 0; i < n;) {
        int j = i; while (j < n && !_stricmp(l[j].clip, l[i].clip)) j++;
        Frm *fr = (Frm *)calloc((size_t)(j - i), sizeof(Frm)); int nf = 0;
        for (int k = i; k < j; k++) { int ch = sw_sprite_char(&w, &l[k], 0, id); if (!ch) continue; fr[nf].label = l[k].label; fr[nf].ch = ch; nf++; shown++; }
        sw_clip(&w, l[i].clip, fr, nf); free(fr); i = j;
    }
    int ok = sw_write(&w, cachedir, cid); if (ok) cache_stamp(cachedir, sig);
    clog("[Assets] %s: %d of %d sprites compiled%s", id, shown, n, ok ? "" : " - FAILED");
    free_sprs(l, n); return has || ok;
}
