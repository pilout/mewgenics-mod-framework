// Mewjector API v3 compatibility (included by core.c).
//
// MewCore is a version.dll proxy like Mewjector (github.com/githubuser508/mewjector, MIT licence), so mods written for Mewjector
// find "version.dll" in the process and resolve the MJ_* exports below. The API behaves as documented in Mewjector's API.md:
//   MJ_InstallHook      chainable entry-point hook (per RVA, ordered by priority, lower = called first)
//   MJ_QueryHook        number of hooks at an RVA
//   MJ_AllocTypeIdPair  unique type index pair, from 0x1000
//   MJ_RegisterName / MJ_LookupName   collision-detected names (512 entries)
//   MJ_GetGameBase, MJ_Log, MJ_VerifyHooks, MJ_GetVersion (3)
// MewCore's own hooks and the hooks of SDK mods go through the same chain, so a Mewjector mod and an SDK mod can hook the
// same game function. MinHook does the actual patching; each hook gets a small stable relay ("next" stub) so that chains can be
// re-ordered while mods keep the pointer they were given.
#define MJ_API_VERSION 3
#define MJ_MAX_SITES 256
#define MJ_MAX_CHAIN 16
#define MJ_MAX_NAMES 512

typedef struct { void *fn; int pri; char owner[64]; uint8_t *next; } MjHook;      // next = relay called by this hook to continue the chain
typedef struct { uint64_t rva; void *target; void *orig; uint8_t *entry; int n; MjHook hk[MJ_MAX_CHAIN]; } MjSite;
static MjSite g_sites[MJ_MAX_SITES]; static int g_nsites;
static uint8_t *g_stubpool; static size_t g_stubused, g_stubcap;
static struct { char cat[32], name[128], owner[64]; } g_names[MJ_MAX_NAMES]; static int g_nnames;
static volatile LONG g_typeid = 0x1000;

// jmp qword ptr [rip+0] ; <8 byte address>
static uint8_t *stub_new(void *to) {
    if (g_stubused + 16 > g_stubcap) {
        g_stubpool = (uint8_t *)VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); g_stubused = 0; g_stubcap = g_stubpool ? 65536 : 0;
        if (!g_stubpool) return NULL;
    }
    uint8_t *s = g_stubpool + g_stubused; g_stubused += 16;
    s[0] = 0xFF; s[1] = 0x25; s[2] = s[3] = s[4] = s[5] = 0; memcpy(s + 6, &to, 8); return s;
}
static void stub_set(uint8_t *s, void *to) { InterlockedExchangePointer((void *volatile *)(s + 6), to); }
static void site_rewire(MjSite *st) {
    stub_set(st->entry, st->n ? st->hk[0].fn : st->orig);
    for (int i = 0; i < st->n; i++) stub_set(st->hk[i].next, i + 1 < st->n ? st->hk[i + 1].fn : st->orig);
}
static MjSite *site_find(uint64_t rva) { for (int i = 0; i < g_nsites; i++) if (g_sites[i].rva == rva) return &g_sites[i]; return NULL; }

// Returns the relay the hook must call to continue the chain (NULL on failure).
static void *chain_install(uint64_t rva, void *fn, int pri, const char *owner) {
    EnterCriticalSection(&g_cs);
    MjSite *st = site_find(rva); void *ret = NULL;
    if (!st) {
        if (g_nsites >= MJ_MAX_SITES) goto done;
        void *target = (void *)(g_base + rva), *tramp = NULL;
        st = &g_sites[g_nsites]; memset(st, 0, sizeof *st); st->rva = rva; st->target = target;
        st->entry = stub_new(NULL); if (!st->entry) goto done;
        MH_STATUS ms = MH_CreateHook(target, st->entry, &tramp);
        if (ms == MH_OK) ms = MH_EnableHook(target);
        if (ms != MH_OK) { clog("[Hook] RVA 0x%llX failed: %s", (unsigned long long)rva, MH_StatusToString(ms)); goto done; }
        st->orig = tramp; g_nsites++;
    }
    if (st->n >= MJ_MAX_CHAIN) goto done;
    int pos = st->n; while (pos > 0 && st->hk[pos - 1].pri > pri) pos--;     // after hooks of the same or lower priority
    memmove(&st->hk[pos + 1], &st->hk[pos], (size_t)(st->n - pos) * sizeof(MjHook));
    MjHook *h = &st->hk[pos]; memset(h, 0, sizeof *h); h->fn = fn; h->pri = pri; strncpy(h->owner, owner ? owner : "?", sizeof h->owner - 1);
    h->next = stub_new(NULL); if (!h->next) { memmove(&st->hk[pos], &st->hk[pos + 1], (size_t)(st->n - pos) * sizeof(MjHook)); goto done; }
    st->n++; site_rewire(st); ret = h->next;
done:
    LeaveCriticalSection(&g_cs); return ret;
}
// Removes the hook that was given <next>; the game function is restored when the last hook goes.
static void chain_remove(void *next) {
    EnterCriticalSection(&g_cs);
    for (int s = 0; s < g_nsites; s++) {
        MjSite *st = &g_sites[s];
        for (int i = 0; i < st->n; i++) if (st->hk[i].next == (uint8_t *)next) {
            memmove(&st->hk[i], &st->hk[i + 1], (size_t)(st->n - i - 1) * sizeof(MjHook)); st->n--; site_rewire(st);
            if (st->n == 0) { MH_DisableHook(st->target); MH_RemoveHook(st->target); g_sites[s] = g_sites[--g_nsites]; }
            LeaveCriticalSection(&g_cs); return;
        }
    }
    LeaveCriticalSection(&g_cs);
}
static int chain_count(void) { int n = 0; for (int i = 0; i < g_nsites; i++) n += g_sites[i].n; return n; }

__declspec(dllexport) int __cdecl MJ_InstallHook(UINT_PTR rva, int stolenBytes, void *hookFn, void **outTrampoline, int priority, const char *owner) {
    (void)stolenBytes;
    if (!hookFn || !outTrampoline) return 0;
    void *next = chain_install((uint64_t)rva, hookFn, priority, owner ? owner : "?");
    if (!next) return 0;
    *outTrampoline = next; clog("[MJ] %s hooked 0x%llX (priority %d)", owner ? owner : "?", (unsigned long long)rva, priority); return 1;
}
__declspec(dllexport) int __cdecl MJ_QueryHook(UINT_PTR rva) { EnterCriticalSection(&g_cs); MjSite *s = site_find((uint64_t)rva); int n = s ? s->n : 0; LeaveCriticalSection(&g_cs); return n; }
__declspec(dllexport) UINT_PTR __cdecl MJ_AllocTypeIdPair(const char *owner) { LONG v = InterlockedExchangeAdd(&g_typeid, 2); clog("[MJ] %s got type ids 0x%lX/0x%lX", owner ? owner : "?", (unsigned long)v, (unsigned long)v + 1); return (UINT_PTR)v; }
__declspec(dllexport) int __cdecl MJ_RegisterName(const char *category, const char *name, const char *owner) {
    if (!category || !name) return 0; if (!owner) owner = "?";
    EnterCriticalSection(&g_cs); int ret = 0;
    for (int i = 0; i < g_nnames; i++) if (!strncmp(g_names[i].cat, category, 31) && !strncmp(g_names[i].name, name, 127)) {
        ret = !strncmp(g_names[i].owner, owner, 63);
        if (!ret) clog("[MJ] name collision in '%s': '%s' is owned by %s, requested by %s", category, name, g_names[i].owner, owner);
        goto done;
    }
    if (g_nnames < MJ_MAX_NAMES) { strncpy(g_names[g_nnames].cat, category, 31); strncpy(g_names[g_nnames].name, name, 127); strncpy(g_names[g_nnames].owner, owner, 63); g_nnames++; ret = 1; }
    else clog("[MJ] name registry is full (%d)", MJ_MAX_NAMES);
done:
    LeaveCriticalSection(&g_cs); return ret;
}
__declspec(dllexport) const char *__cdecl MJ_LookupName(const char *category, const char *name) {
    const char *r = NULL; if (!category || !name) return NULL;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_nnames; i++) if (!strncmp(g_names[i].cat, category, 31) && !strncmp(g_names[i].name, name, 127)) { r = g_names[i].owner; break; }
    LeaveCriticalSection(&g_cs); return r;
}
__declspec(dllexport) UINT_PTR __cdecl MJ_GetGameBase(void) { return (UINT_PTR)g_base; }
__declspec(dllexport) void __cdecl MJ_Log(const char *owner, const char *fmt, ...) {
    char buf[1500]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt ? fmt : "", ap); va_end(ap);
    clog("[%s] %s", owner ? owner : "mod", buf);
}
__declspec(dllexport) int __cdecl MJ_VerifyHooks(void) {
    int bad = 0; EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_nsites; i++) if (g_sites[i].n && *(uint8_t *)g_sites[i].target != 0xE9) { clog("[MJ] hook site 0x%llX was overwritten by another patch", (unsigned long long)g_sites[i].rva); bad++; }
    LeaveCriticalSection(&g_cs); return bad;
}
__declspec(dllexport) int __cdecl MJ_GetVersion(void) { return MJ_API_VERSION; }

// ---- chainloader.ini and Mewtator manifest (same files and rules as Mewjector) ----
typedef struct { int enabled, scan_game_dir; char scan_path[MAX_PATH], manifest[MAX_PATH]; char order[32][MAX_PATH]; int norder; } MjConfig;
static MjConfig g_mj;
static void mj_trim(char *s) { size_t n = strlen(s); while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0; char *p = s; while (*p == ' ' || *p == '\t') p++; if (p != s) memmove(s, p, strlen(p) + 1); }
static void mj_default_ini(const char *path) {
    static const char txt[] =
        "[Chainloader]\n; Directory to scan for mod DLLs. Relative to the game folder, or absolute.\nScanPath=mods\n"
        "; Also scan the game folder itself for loose DLLs.\nScanGameDir=0\n"
        "; Mewtator writes its manifest path here when both are installed.\nMewtatorManifest=\n"
        "; Set to 0 to load no community mods.\nEnabled=1\n\n"
        "[LoadOrder]\n; Mod1..Mod9 are reserved for framework DLLs, use Mod10 and up for your own entries.\nMod2=csf_core.dll\n";
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) write_file(path, txt, sizeof txt - 1);
}
static void mj_load_config(void) {
    char ini[MAX_PATH]; snprintf(ini, sizeof ini, "%s\\chainloader.ini", g_dir);
    mj_default_ini(ini);
    memset(&g_mj, 0, sizeof g_mj); g_mj.enabled = 1; strcpy(g_mj.scan_path, "mods");
    g_mj.enabled = GetPrivateProfileIntA("Chainloader", "Enabled", 1, ini) != 0;
    g_mj.scan_game_dir = GetPrivateProfileIntA("Chainloader", "ScanGameDir", 0, ini) != 0;
    GetPrivateProfileStringA("Chainloader", "ScanPath", "mods", g_mj.scan_path, sizeof g_mj.scan_path, ini); mj_trim(g_mj.scan_path);
    GetPrivateProfileStringA("Chainloader", "MewtatorManifest", "", g_mj.manifest, sizeof g_mj.manifest, ini); mj_trim(g_mj.manifest);
    for (int i = 1; i <= 99 && g_mj.norder < 32; i++) {
        char key[16], val[MAX_PATH]; snprintf(key, sizeof key, "Mod%d", i);
        GetPrivateProfileStringA("LoadOrder", key, "", val, sizeof val, ini); mj_trim(val);
        if (val[0]) strcpy(g_mj.order[g_mj.norder++], val);
    }
}
static int mj_is_abs(const char *p) { return p[0] == '\\' || p[0] == '/' || (p[0] && p[1] == ':'); }
static void mj_resolve(const char *p, char *out, size_t cap) { if (mj_is_abs(p)) snprintf(out, cap, "%s", p); else snprintf(out, cap, "%s\\%s", g_dir, p); }
