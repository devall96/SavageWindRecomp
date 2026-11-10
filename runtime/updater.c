// Updates of Savage Wind itself (runtime/updater.h): Windows release folders and Linux release AppImages.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE  // (pipe2)
#endif
#include "updater.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *rt_version(void);  // runtime/ui/launcher.mm

// Version strings as the releases name them: 0.5.0-alpha.5. Numbers compared as numbers; a version without a "-..." part
// is newer than the same one with it (0.5.0 > 0.5.0-rc.1); the parts after "-" compared one dot-separated piece at a
// time, numbers below words.
static int cmp_piece(const char *a, size_t na, const char *b, size_t nb) {
    bool da = na > 0, db = nb > 0;
    for (size_t i = 0; i < na; i++) da &= isdigit((unsigned char)a[i]) != 0;
    for (size_t i = 0; i < nb; i++) db &= isdigit((unsigned char)b[i]) != 0;
    if (da && db) {
        while (na > 1 && *a == '0') a++, na--;
        while (nb > 1 && *b == '0') b++, nb--;
        if (na != nb) return na < nb ? -1 : 1;
        int c = memcmp(a, b, na);
        return c < 0 ? -1 : c > 0;
    }
    if (da != db) return da ? -1 : 1;
    int c = memcmp(a, b, na < nb ? na : nb);
    if (c) return c < 0 ? -1 : 1;
    return na == nb ? 0 : na < nb ? -1 : 1;
}

int updater_version_cmp(const char *a, const char *b) {
    if (*a == 'v') a++;
    if (*b == 'v') b++;
    const char *pa = strchr(a, '-'), *pb = strchr(b, '-');
    const char *ea = pa ? pa : a + strlen(a), *eb = pb ? pb : b + strlen(b);
    for (int i = 0; i < 3; i++) {  // major.minor.patch
        size_t na = strcspn(a, ".-"), nb = strcspn(b, ".-");
        if (a + na > ea) na = (size_t)(ea - a);
        if (b + nb > eb) nb = (size_t)(eb - b);
        int c = cmp_piece(a, na, b, nb);
        if (c) return c;
        a += na, b += nb;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    if (!pa || !pb) return pa == pb ? 0 : pa ? -1 : 1;
    a = pa + 1, b = pb + 1;
    for (;;) {
        size_t na = strcspn(a, "."), nb = strcspn(b, ".");
        int c = cmp_piece(a, na, b, nb);
        if (c) return c;
        a += na, b += nb;
        bool ma = *a == '.', mb = *b == '.';
        if (ma != mb) return ma ? 1 : -1;  // (more pieces: newer, alpha.5.1 > alpha.5)
        if (!ma) return 0;
        a++, b++;
    }
}

#if !defined(_WIN32) && !defined(__linux__)  // (macOS: releases are updated by hand)
bool updater_available(void) { return false; }
const char *updater_managed_by(void) { return NULL; }
void updater_status(UpdStatus *out) {
    memset(out, 0, sizeof *out);
    snprintf(out->current, sizeof out->current, "%s", rt_version());
}
void updater_check(void) {}
void updater_download(void) {}
void updater_cancel(void) {}
bool updater_apply(bool then_play, char *why, size_t why_size) {
    (void)then_play;
    snprintf(why, why_size, "not available here");
    return false;
}
bool updater_start_update(void) { return false; }
bool updater_wait_pid(unsigned long pid, int timeout_ms) { (void)pid, (void)timeout_ms; return true; }
void updater_cleanup(void) {}
int updater_test(const char *steps) { (void)steps; fprintf(stderr, "[update] not available here\n"); return 1; }
#else

#include <pthread.h>
#include <stdint.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
// (after windows.h: they use its types)
#include <bcrypt.h>
#include <winhttp.h>
#include <zlib.h>
#else
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#include "gamelib.h"
#include "paths.h"
#include "platform.h"
#include "rt.h"

const char *maxwell_translator_id(void);  // runtime/nvn/maxwell/translate.c (TRANSLATOR_ID)
const char *vk_spirv_id(void);            // runtime/nvn/vulkan/spirv.c (SPIRV_ID)

// The project's releases. SAVAGEWIND_UPDATE_REPO=<owner>/<repo> (tests: a fork's releases), SAVAGEWIND_UPDATE_AS=<version>
// (tests: this build taken for that version, so that a check finds the latest release).
#define UPD_REPO "Revivedx/SavageWindRecomp"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static UpdStatus g_st;
static bool g_init, g_busy;
static char g_asset_url[1024], g_asset_sha[65];  // the release file to download (Windows: the zip; Linux: the AppImage)

static void st_init(void) {  // (under g_mu)
    if (g_init) return;
    g_init = true;
    const char *as = getenv("SAVAGEWIND_UPDATE_AS");
    snprintf(g_st.current, sizeof g_st.current, "%s", as && *as ? as : rt_version());
    g_st.gamecode = g_st.shaders = UPD_UNKNOWN;
}

void updater_status(UpdStatus *out) {
    pthread_mutex_lock(&g_mu);
    st_init();
    *out = g_st;
    pthread_mutex_unlock(&g_mu);
}

static void fail(const char *fmt, ...) {
    char m[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    LOG("[update] %s\n", m);
    pthread_mutex_lock(&g_mu);
    g_st.state = UPD_FAILED;
    snprintf(g_st.error, sizeof g_st.error, "%s", m);
    pthread_mutex_unlock(&g_mu);
}

static void set_state(UpdState s) {
    pthread_mutex_lock(&g_mu);
    g_st.state = s;
    pthread_mutex_unlock(&g_mu);
}

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

// ------------------------------------------------------------------ what each system provides (sections below)
typedef bool (*Sink)(const void *p, size_t n, void *user);  // false: stop
static bool http_get(const char *url, bool api, Sink sink, void *user, char *err, size_t en);
static bool release_asset(const char *name);  // this system's file among a release's
static bool local_release(void);              // SAVAGEWIND_UPDATE_FILE: a release file on this computer instead
static void sleep_ms(int ms);

typedef struct { char *p; size_t n, cap; } Buf;

static bool buf_sink(const void *p, size_t n, void *user) {
    Buf *b = (Buf *)user;
    if (b->n + n + 1 > 8u << 20) return false;  // (a release list is tens of KB)
    if (b->n + n + 1 > b->cap) {
        size_t cap = (b->n + n + 1) * 2;
        char *q = (char *)realloc(b->p, cap);
        if (!q) return false;
        b->p = q, b->cap = cap;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
    return true;
}

// ------------------------------------------------------------------ JSON (GitHub's release list)
typedef struct J {
    char type;           // o a s n (numbers, true / false / null: their text)
    char *key, *str;     // key: in an object; str: s decoded, n as written
    struct J *kid, *next;
} J;

static void jfree(J *j) {
    while (j) {
        J *n = j->next;
        jfree(j->kid);
        free(j->key);
        free(j->str);
        free(j);
        j = n;
    }
}

static const char *jws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static void put_utf8(char **o, unsigned c) {
    char *q = *o;
    if (c < 0x80) *q++ = (char)c;
    else if (c < 0x800) *q++ = (char)(0xC0 | c >> 6), *q++ = (char)(0x80 | (c & 63));
    else if (c < 0x10000) *q++ = (char)(0xE0 | c >> 12), *q++ = (char)(0x80 | (c >> 6 & 63)), *q++ = (char)(0x80 | (c & 63));
    else *q++ = (char)(0xF0 | c >> 18), *q++ = (char)(0x80 | (c >> 12 & 63)), *q++ = (char)(0x80 | (c >> 6 & 63)),
         *q++ = (char)(0x80 | (c & 63));
    *o = q;
}

static unsigned hex4(const char *p) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v = v << 4 | (unsigned)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
    }
    return v;
}

static char *jstring(const char **pp) {  // at the opening quote; NULL: malformed
    const char *p = *pp + 1, *e = p;
    while (*e && *e != '"') e += *e == '\\' && e[1] ? 2 : 1;
    if (*e != '"') return NULL;
    char *out = (char *)malloc((size_t)(e - p) * 2 + 1), *o = out;  // (\uXXXX: 6 bytes in, at most 4 out)
    if (!out) return NULL;
    while (p < e) {
        if (*p != '\\') { *o++ = *p++; continue; }
        char c = p[1];
        p += 2;
        switch (c) {
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            if (e - p < 4) { free(out); return NULL; }
            unsigned u = hex4(p);
            p += 4;
            if (u >= 0xD800 && u < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                unsigned lo = hex4(p + 2);
                if (lo >= 0xDC00 && lo < 0xE000) u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00), p += 6;
            }
            put_utf8(&o, u);
            break;
        }
        default: *o++ = c;  // \" \\ \/
        }
    }
    *o = 0;
    *pp = e + 1;
    return out;
}

static J *jparse(const char **pp, int depth) {
    const char *p = jws(*pp);
    if (depth > 64 || !*p) return NULL;
    J *j = (J *)calloc(1, sizeof *j);
    if (!j) return NULL;
    if (*p == '{' || *p == '[') {
        bool obj = *p == '{';
        j->type = obj ? 'o' : 'a';
        p = jws(p + 1);
        J **tail = &j->kid;
        if (*p == (obj ? '}' : ']')) { *pp = p + 1; return j; }
        for (;;) {
            char *key = NULL;
            if (obj) {
                p = jws(p);
                if (*p != '"' || !(key = jstring(&p))) goto bad;
                p = jws(p);
                if (*p++ != ':') { free(key); goto bad; }
            }
            J *k = jparse(&p, depth + 1);
            if (!k) { free(key); goto bad; }
            k->key = key;
            *tail = k, tail = &k->next;
            p = jws(p);
            if (*p == ',') { p++; continue; }
            if (*p == (obj ? '}' : ']')) { *pp = p + 1; return j; }
            goto bad;
        }
    }
    if (*p == '"') {
        j->type = 's';
        if (!(j->str = jstring(&p))) goto bad;
        *pp = p;
        return j;
    }
    {
        const char *e = p;
        while (*e && (isalnum((unsigned char)*e) || *e == '-' || *e == '+' || *e == '.')) e++;
        if (e == p) goto bad;
        j->type = 'n';
        j->str = (char *)malloc((size_t)(e - p) + 1);
        if (!j->str) goto bad;
        memcpy(j->str, p, (size_t)(e - p));
        j->str[e - p] = 0;
        *pp = e;
        return j;
    }
bad:
    jfree(j);
    return NULL;
}

static J *jget(const J *o, const char *key) {
    for (J *k = o && o->type == 'o' ? o->kid : NULL; k; k = k->next)
        if (k->key && !strcmp(k->key, key)) return k;
    return NULL;
}

static const char *jstr(const J *o, const char *key) {
    J *k = jget(o, key);
    return k && (k->type == 's' || k->type == 'n') ? k->str : NULL;
}

// ------------------------------------------------------------------ check
static bool ends_with(const char *s, const char *end) {
    size_t a = strlen(s), b = strlen(end);
    return a >= b && !strcmp(s + a - b, end);
}

static bool is_hex64(const char *s) {
    for (int i = 0; i < 64; i++)
        if (!isxdigit((unsigned char)s[i])) return false;
    return s[64] == 0;
}

static void *check_thread(void *arg) {
    (void)arg;
    if (local_release()) return NULL;
    const char *repo = getenv("SAVAGEWIND_UPDATE_REPO");
    if (!repo || !*repo || strspn(repo, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_./") != strlen(repo))
        repo = UPD_REPO;
    char url[300], err[200] = "";
    snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases?per_page=20", repo);
    Buf b = {0};
    bool ok = http_get(url, true, buf_sink, &b, err, sizeof err);
    const char *p = b.p;
    J *list = ok && p ? jparse(&p, 0) : NULL;
    if (!ok) fail("Could not reach GitHub: %s", err);
    else if (!list || list->type != 'a') fail("GitHub's release list could not be read");
    else {
        char cur[64];
        pthread_mutex_lock(&g_mu);
        snprintf(cur, sizeof cur, "%s", g_st.current);
        pthread_mutex_unlock(&g_mu);
        const J *best = NULL, *best_zip = NULL;
        const char *best_ver = NULL;
        bool cur_pre = strchr(cur, '-') != NULL;  // (a test version - alpha, beta, rc - takes the test releases too)
        for (J *r = list->kid; r; r = r->next) {
            const char *tag = jstr(r, "tag_name"), *draft = jstr(r, "draft"), *pre = jstr(r, "prerelease");
            if (!tag || (draft && !strcmp(draft, "true"))) continue;
            if (!cur_pre && pre && !strcmp(pre, "true")) continue;  // (a full release isn't offered a test one)
            const char *ver = tag[0] == 'v' ? tag + 1 : tag;
            if (!isdigit((unsigned char)ver[0]) || strlen(ver) >= sizeof g_st.latest) continue;
            if (updater_version_cmp(ver, cur) <= 0 || (best_ver && updater_version_cmp(ver, best_ver) <= 0)) continue;
            J *assets = jget(r, "assets");
            for (J *a = assets && assets->type == 'a' ? assets->kid : NULL; a; a = a->next) {
                const char *name = jstr(a, "name"), *dl = jstr(a, "browser_download_url"), *dg = jstr(a, "digest");
                // this system's file, with the SHA-256 GitHub publishes for every asset, from GitHub itself
                if (name && dl && dg && release_asset(name) &&
                    !strncmp(dl, "https://github.com/", 19) && strlen(dl) < sizeof g_asset_url && !strncmp(dg, "sha256:", 7) &&
                    is_hex64(dg + 7)) {
                    best = r, best_zip = a, best_ver = ver;
                    break;
                }
            }
        }
        pthread_mutex_lock(&g_mu);
        if (best) {
            const char *page = jstr(best, "html_url");
            snprintf(g_st.latest, sizeof g_st.latest, "%s", best_ver);
            snprintf(g_st.notes_url, sizeof g_st.notes_url, "%s", page && !strncmp(page, "https://github.com/", 19) ? page : "");
            const char *size = jstr(best_zip, "size");
            g_st.total = size ? strtoull(size, NULL, 10) : 0;
            g_st.done = 0;
            snprintf(g_asset_url, sizeof g_asset_url, "%s", jstr(best_zip, "browser_download_url"));
            snprintf(g_asset_sha, sizeof g_asset_sha, "%s", jstr(best_zip, "digest") + 7);
            for (char *c = g_asset_sha; *c; c++) *c = (char)tolower((unsigned char)*c);
            g_st.state = UPD_AVAILABLE;
        } else
            g_st.state = UPD_UP_TO_DATE;
        pthread_mutex_unlock(&g_mu);
        LOG("[update] %s: %s%s\n", cur, best ? "version " : "up to date", best ? best_ver : "");
    }
    jfree(list);
    free(b.p);
    pthread_mutex_lock(&g_mu);
    g_busy = false;
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

static bool start(void *(*fn)(void *), UpdState need1, UpdState need2, UpdState now) {
    pthread_mutex_lock(&g_mu);
    st_init();
    bool go = !g_busy && (g_st.state == need1 || g_st.state == need2 || need1 == UPD_IDLE);
    if (go) g_busy = true, g_st.state = now, g_st.error[0] = 0;
    pthread_mutex_unlock(&g_mu);
    if (!go) return false;
    pthread_t t;
    if (pthread_create(&t, NULL, fn, NULL)) {
        pthread_mutex_lock(&g_mu);
        g_busy = false;
        pthread_mutex_unlock(&g_mu);
        fail("Could not start");
        return false;
    }
    pthread_detach(t);
    return true;
}

void updater_check(void) {
    if (!updater_available()) return;
    pthread_mutex_lock(&g_mu);
    bool ready = g_st.state == UPD_READY;  // (unpacked already: a check would only forget that)
    pthread_mutex_unlock(&g_mu);
    if (!ready) start(check_thread, UPD_IDLE, UPD_IDLE, UPD_CHECKING);
}

#ifdef _WIN32  // ------------------------------------------------------------------ Windows: the release folder
#define UPD_MAX_ZIP (1024ull << 20)  // (a release zip is ~90 MB: anything near this is not one)
#define UPD_MAX_FILE (512u << 20)

static volatile LONG g_cancel;

static void mkdirs_for(const char *file) {  // the folders above file
    char p[2048];
    snprintf(p, sizeof p, "%s", file);
    for (char *c = p + 3; *c; c++)  // (past "C:\")
        if (*c == '/' || *c == '\\') { char s = *c; *c = 0; CreateDirectoryA(p, NULL); *c = s; }
}

static void upd_path(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s\\update%s%s", rt_exe_dir(), *name ? "\\" : "", name);
}

// A launcher that installs and updates games from their GitHub releases owns this folder: its updates, not ours (two
// updaters on one folder would fight over the files). Each leaves its own sign (checked 2026-11):
//   Quiver Launcher (tgeorgiadis/quiver-launcher)   .quiver-version in the game's folder (install.rs VERSION_FILE)
//   GitHub Launcher (SirDiabo/N64RecompLauncher)    version.txt in the game's folder (+ LastPlayed.txt /
//                                                   selected_executable.txt); Quiver's older versions wrote it too
//   RecompLauncher (elripalda/RecompLauncher)      RECOMPLAUNCHER=1 for every game it starts; its library is
//                                                   <system drive>\Recomp Library by default
// "The game's folder" is this exe's, or the one above it when the launcher kept the zip's top folder. Savage Wind's
// releases ship none of these names. SAVAGEWIND_UPDATE_MANAGED=<name> (test): as if that launcher installed it.
const char *updater_managed_by(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;  // (worked out once, whichever thread asks first; the others wait)
    static const char *who;
    BOOL pending;
    if (!InitOnceBeginInitialize(&once, 0, &pending, NULL) || !pending) return who;
    const char *dir = rt_exe_dir();
    char p[1300], up[1200];
    snprintf(up, sizeof up, "%s", dir);
    char *cut = strrchr(up, '\\');
    if (cut && cut > up + 2) *cut = 0; else up[0] = 0;  // (the folder above; none at a drive's top)
    #define HAS(d, name) (*(d) && (snprintf(p, sizeof p, "%s\\%s", d, name), file_exists(p)))
    const char *test = getenv("SAVAGEWIND_UPDATE_MANAGED");
    const char *rl = getenv("RECOMPLAUNCHER");
    if (test && *test) who = test;
    else if (rl && *rl && strcmp(rl, "0")) who = "RecompLauncher";
    else if (HAS(dir, ".quiver-version") || HAS(up, ".quiver-version")) who = "Quiver Launcher";
    else if (HAS(dir, "version.txt") ||
             (HAS(up, "version.txt") && (HAS(up, "LastPlayed.txt") || HAS(up, "selected_executable.txt"))))
        who = "GitHub Launcher";
    else {
        char low[1200];
        snprintf(low, sizeof low, "%s\\", dir);
        for (char *c = low; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (strstr(low, "\\recomp library\\")) who = "RecompLauncher";
    }
    #undef HAS
    if (who) LOG("[update] installed by %s: updates come from it, Savage Wind's own updater is off\n", who);
    InitOnceComplete(&once, 0, NULL);
    return who;
}

bool updater_available(void) {
    if (getenv("SAVAGEWIND_NO_UPDATER") || !rt_exe_dir()[0] || updater_managed_by()) return false;
    // a release folder: the configuration program is there under its release name (a build has
    // build/savagewind-config.exe; a source checkout never updates itself)
    char p[1200];
    snprintf(p, sizeof p, "%s\\Savage Wind Configuration.exe", rt_exe_dir());
    return file_exists(p);
}

// ------------------------------------------------------------------ HTTPS (WinHTTP)
static bool http_get(const char *url, bool api, Sink sink, void *user, char *err, size_t en) {
    if (strncmp(url, "https://", 8)) { snprintf(err, en, "not an https address"); return false; }
    wchar_t wurl[2048], host[256], path[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048)) { snprintf(err, en, "address too long"); return false; }
    URL_COMPONENTS uc = {.dwStructSize = sizeof uc, .lpszHostName = host, .dwHostNameLength = 256,
                         .lpszUrlPath = path, .dwUrlPathLength = 2048, .dwExtraInfoLength = (DWORD)-1};
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) { snprintf(err, en, "bad address"); return false; }
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) wcsncat(path, uc.lpszExtraInfo, 2047 - wcslen(path));
    wchar_t agent[96];
    swprintf(agent, 96, L"SavageWind/%hs", rt_version());  // (nothing of the player: the program and its version)
    HINTERNET s = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE) : NULL;
    bool ok = false;
    DWORD code = 0;
    if (r) {
        WinHttpSetTimeouts(r, 15000, 15000, 20000, 30000);
        const wchar_t *hdr = api ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28" : NULL;
        DWORD sz = sizeof code;
        if (WinHttpSendRequest(r, hdr ? hdr : WINHTTP_NO_ADDITIONAL_HEADERS, hdr ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA,
                               0, 0, 0) &&
            WinHttpReceiveResponse(r, NULL) &&
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &code, &sz, WINHTTP_NO_HEADER_INDEX)) {
            if (code != 200) snprintf(err, en, "the server answered %lu", (unsigned long)code);
            else {
                static __thread char buf[1 << 16];
                ok = true;
                for (;;) {
                    DWORD got = 0;
                    if (!WinHttpReadData(r, buf, sizeof buf, &got)) { ok = false; snprintf(err, en, "the connection broke"); break; }
                    if (!got) break;
                    if (!sink(buf, got, user)) { ok = false; snprintf(err, en, "stopped"); break; }
                }
            }
        } else
            snprintf(err, en, "no connection to %ls (error %lu)", host, (unsigned long)GetLastError());
    } else
        snprintf(err, en, "no connection (error %lu)", (unsigned long)GetLastError());
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

static bool release_asset(const char *name) {  // the Windows zip
    return !strncmp(name, "SavageWind-", 11) && ends_with(name, "-windows.zip");
}

// SAVAGEWIND_UPDATE_FILE=<zip> (test): a release zip on this PC is "the newest release", as this build's version -
// with SAVAGEWIND_UPDATE_AS, the whole update (offer, install, the new version's own rebuild) without GitHub. Its
// SHA-256 is worked out and logged, not checked against anything.
static bool local_release(void) {
    const char *zip = getenv("SAVAGEWIND_UPDATE_FILE");
    if (!zip || !*zip) return false;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    bool there = GetFileAttributesExA(zip, GetFileExInfoStandard, &fa) && !(fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    pthread_mutex_lock(&g_mu);
    char cur[64];
    snprintf(cur, sizeof cur, "%s", g_st.current);
    bool newer = there && updater_version_cmp(rt_version(), cur) > 0;
    if (newer) {
        snprintf(g_st.latest, sizeof g_st.latest, "%s", rt_version());
        g_st.notes_url[0] = 0;
        g_st.total = (uint64_t)fa.nFileSizeHigh << 32 | fa.nFileSizeLow, g_st.done = 0;
        snprintf(g_asset_url, sizeof g_asset_url, "file:%s", zip);
        g_asset_sha[0] = 0;
        g_st.state = UPD_AVAILABLE;
    } else
        g_st.state = there ? UPD_UP_TO_DATE : UPD_FAILED;
    if (!there) snprintf(g_st.error, sizeof g_st.error, "SAVAGEWIND_UPDATE_FILE: no file %s", zip);
    g_busy = false;
    pthread_mutex_unlock(&g_mu);
    LOG("[update] %s: %s (local test zip %s)\n", cur, newer ? rt_version() : there ? "up to date" : "no such file", zip);
    return true;
}

// ------------------------------------------------------------------ download
typedef struct {
    FILE *f;
    BCRYPT_HASH_HANDLE h;
    uint64_t n;
} Dl;

static bool dl_sink(const void *p, size_t n, void *user) {
    Dl *d = (Dl *)user;
    if (InterlockedCompareExchange(&g_cancel, 0, 0) || d->n + n > UPD_MAX_ZIP) return false;
    if (fwrite(p, 1, n, d->f) != n || BCryptHashData(d->h, (PUCHAR)p, (ULONG)n, 0)) return false;
    d->n += n;
    pthread_mutex_lock(&g_mu);
    g_st.done = d->n;
    pthread_mutex_unlock(&g_mu);
    return true;
}

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

// A path inside the release folder: relative, '/'-separated, no "." / ".." / empty parts, no drive or stream names,
// no part Windows would read as another (a device - CON, NUL, COM1, "aux.txt" - or one ending in a dot or a space,
// which Windows drops)
static bool safe_rel(const char *rel) {
    if (!*rel || *rel == '/' || strlen(rel) > 400 || strpbrk(rel, "\\:*?\"<>|")) return false;
    static const char *const dev[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
    for (const char *p = rel; *p;) {
        size_t n = strcspn(p, "/");
        if (!n || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return false;
        if (p[n - 1] == '.' || p[n - 1] == ' ') return false;
        size_t base = strcspn(p, "./");  // (a device name counts with any extension: "nul.txt" is NUL)
        for (size_t i = 0; i < sizeof dev / sizeof *dev; i++)
            if (base == strlen(dev[i]) && !_strnicmp(p, dev[i], base)) return false;
        if (base == 4 && (!_strnicmp(p, "COM", 3) || !_strnicmp(p, "LPT", 3)) && p[3] >= '0' && p[3] <= '9') return false;
        p += n;
        if (*p == '/') p++;
    }
    return rel[strlen(rel) - 1] != '/';
}

// The release zip into dst: every entry under its one top folder ("Savage Wind/"), stored or deflated.
static bool unzip(const char *zip, const char *dst, char *err, size_t en) {
    FILE *f = fopen(zip, "rb");
    uint8_t *z = NULL, *out = NULL;
    long size = -1;
    if (f && !fseek(f, 0, SEEK_END)) size = ftell(f), fseek(f, 0, SEEK_SET);
    bool ok = size > 22 && (uint64_t)size <= UPD_MAX_ZIP && (z = (uint8_t *)malloc((size_t)size)) &&
              fread(z, 1, (size_t)size, f) == (size_t)size;
    if (f) fclose(f);
    if (!ok) { free(z); snprintf(err, en, "the download could not be read"); return false; }
    const uint8_t *eocd = NULL;
    for (long i = size - 22; i >= 0 && i >= size - 22 - 65535; i--)
        if (rd32(z + i) == 0x06054b50) { eocd = z + i; break; }
    uint32_t count = eocd ? rd16(eocd + 10) : 0, cd_size = eocd ? rd32(eocd + 12) : 0, cd_off = eocd ? rd32(eocd + 16) : 0;
    if (!eocd || count == 0xFFFF || cd_off == 0xFFFFFFFF || (uint64_t)cd_off + cd_size > (uint64_t)size) {
        free(z);
        snprintf(err, en, "the download is not a zip Savage Wind can read");
        return false;
    }
    char top[256] = "";
    int files = 0;
    bool exe = false;
    const uint8_t *c = z + cd_off, *cend = z + cd_off + cd_size;
    for (uint32_t i = 0; ok && i < count; i++) {
        if (c + 46 > cend || rd32(c) != 0x02014b50) { snprintf(err, en, "damaged zip directory"); ok = false; break; }
        uint32_t flags = rd16(c + 8), method = rd16(c + 10), crc = rd32(c + 16), csize = rd32(c + 20), usize = rd32(c + 24);
        uint32_t nlen = rd16(c + 28), xlen = rd16(c + 30), clen = rd16(c + 32), lofs = rd32(c + 42);
        if (c + 46 + nlen > cend || nlen >= 1024) { snprintf(err, en, "damaged zip directory"); ok = false; break; }
        char name[1024];
        memcpy(name, c + 46, nlen);
        name[nlen] = 0;
        c += 46 + nlen + xlen + clen;
        for (char *q = name; *q; q++) if (*q == '\\') *q = '/';
        if (!top[0]) {  // the top folder, from the first entry
            size_t t = strcspn(name, "/");
            if (!name[t] || t >= sizeof top - 1) { snprintf(err, en, "the zip has no top folder"); ok = false; break; }
            memcpy(top, name, t + 1);
            top[t + 1] = 0;
        }
        if (strncmp(name, top, strlen(top))) { snprintf(err, en, "a file outside the zip's top folder"); ok = false; break; }
        const char *rel = name + strlen(top);
        if (!*rel || name[nlen - 1] == '/') continue;  // (folders: made for the files in them)
        if (!safe_rel(rel)) { snprintf(err, en, "a file with an unsafe name in the zip: %s", rel); ok = false; break; }
        if ((flags & 1) || (method != 0 && method != 8) || usize > UPD_MAX_FILE || csize == 0xFFFFFFFF) {
            snprintf(err, en, "an entry Savage Wind can't unpack: %s", rel);
            ok = false;
            break;
        }
        const uint8_t *lh = z + lofs;
        if ((uint64_t)lofs + 30 > (uint64_t)size || rd32(lh) != 0x04034b50 ||
            (uint64_t)lofs + 30 + rd16(lh + 26) + rd16(lh + 28) + csize > (uint64_t)size) {
            snprintf(err, en, "damaged zip entry: %s", rel);
            ok = false;
            break;
        }
        const uint8_t *data = lh + 30 + rd16(lh + 26) + rd16(lh + 28);
        out = (uint8_t *)realloc(out, usize ? usize : 1);
        if (!out) { snprintf(err, en, "out of memory"); ok = false; break; }
        if (method == 0) {
            if (csize != usize) { snprintf(err, en, "damaged zip entry: %s", rel); ok = false; break; }
            memcpy(out, data, usize);
        } else {
            z_stream zs = {0};
            zs.next_in = (Bytef *)data, zs.avail_in = csize, zs.next_out = out, zs.avail_out = usize;
            int r = inflateInit2(&zs, -MAX_WBITS);
            if (r == Z_OK) r = inflate(&zs, Z_FINISH);
            inflateEnd(&zs);
            if (r != Z_STREAM_END || zs.total_out != usize) { snprintf(err, en, "damaged zip entry: %s", rel); ok = false; break; }
        }
        if (crc32(0, out, usize) != crc) { snprintf(err, en, "damaged zip entry (CRC): %s", rel); ok = false; break; }
        char path[2048];
        snprintf(path, sizeof path, "%s\\%s", dst, rel);
        for (char *q = path; *q; q++) if (*q == '/') *q = '\\';
        mkdirs_for(path);
        FILE *o = fopen(path, "wb");
        bool w = o && fwrite(out, 1, usize, o) == usize;
        if (o && fclose(o)) w = false;
        if (!w) { snprintf(err, en, "could not write %s", path); ok = false; break; }
        files++;
        exe |= !strcmp(rel, "savagewind.exe");
    }
    free(out);
    free(z);
    if (ok && !exe) { snprintf(err, en, "the zip has no savagewind.exe"); ok = false; }
    if (ok) LOG("[update] %d files unpacked into %s\n", files, dst);
    return ok;
}

// build-ids.txt (make dist): "<name> <id>" lines - what the release's game code and shaders are made from
static bool read_id(const char *file, const char *name, char *out, size_t n) {
    FILE *f = fopen(file, "r");
    char line[256];
    bool got = false;
    while (f && !got && fgets(line, sizeof line, f)) {
        size_t k = strlen(name);
        if (!strncmp(line, name, k) && line[k] == ' ') {
            snprintf(out, n, "%s", line + k + 1);
            out[strcspn(out, "\r\n")] = 0;
            got = *out != 0;
        }
    }
    if (f) fclose(f);
    return got;
}

static void *download_thread(void *arg) {
    (void)arg;
    char url[1024], sha[65], dir[1200], part[1300], zip[1300], fresh[1300], err[300] = "";
    pthread_mutex_lock(&g_mu);
    snprintf(url, sizeof url, "%s", g_asset_url);
    snprintf(sha, sizeof sha, "%s", g_asset_sha);
    g_st.done = 0;
    pthread_mutex_unlock(&g_mu);
    InterlockedExchange(&g_cancel, 0);
    upd_path(dir, sizeof dir, "");
    upd_path(part, sizeof part, "download.zip.part");
    upd_path(zip, sizeof zip, "download.zip");
    upd_path(fresh, sizeof fresh, "new");
    CreateDirectoryA(dir, NULL);
    plat_remove_tree(fresh);  // (an earlier, unfinished one)
    remove(zip);
    BCRYPT_ALG_HANDLE alg = NULL;
    Dl d = {fopen(part, "wb"), NULL, 0};
    bool ok = d.f && !BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) &&
              !BCryptCreateHash(alg, &d.h, NULL, 0, NULL, 0, 0);
    if (!ok) snprintf(err, sizeof err, d.f ? "SHA-256 is not available" : "Savage Wind's folder can't be written to");
    else if (!strncmp(url, "file:", 5)) {  // (SAVAGEWIND_UPDATE_FILE: copied in chunks, as a download)
        FILE *in = fopen(url + 5, "rb");
        static char chunk[1 << 18];
        size_t n;
        ok = in != NULL;
        while (ok && in && (n = fread(chunk, 1, sizeof chunk, in)) > 0) ok = dl_sink(chunk, n, &d);
        if (in) fclose(in);
        if (!ok) snprintf(err, sizeof err, in ? "the local zip could not be copied" : "no local zip %s", url + 5);
    } else ok = http_get(url, false, dl_sink, &d, err, sizeof err);
    if (d.f && fclose(d.f)) ok = false;
    uint8_t h[32];
    char hex[65] = "";
    if (d.h) {
        if (!BCryptFinishHash(d.h, h, 32, 0))
            for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", h[i]);
        BCryptDestroyHash(d.h);
    }
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    bool cancelled = InterlockedCompareExchange(&g_cancel, 0, 0) != 0;
    if (ok && sha[0] && strcmp(hex, sha)) {  // (sha empty: the local test zip, nothing to check it against)
        ok = false;
        snprintf(err, sizeof err, "the download doesn't match the release's SHA-256 (%.12s..., expected %.12s...)", hex, sha);
    }
    if (ok && !MoveFileExA(part, zip, MOVEFILE_REPLACE_EXISTING)) ok = false, snprintf(err, sizeof err, "could not keep the download");
    if (!ok) remove(part);
    if (ok) {
        LOG("[update] downloaded %llu bytes, SHA-256 %s %s\n", (unsigned long long)d.n, hex,
            sha[0] ? "matches" : "(local test zip: nothing to check it against)");
        CreateDirectoryA(fresh, NULL);
        ok = unzip(zip, fresh, err, sizeof err);
        remove(zip);
        if (!ok) plat_remove_tree(fresh);
    }
    if (ok) {  // what the update means for the game code and the shaders (unknown without build-ids.txt)
        char ids[1400], v[128], v2[128];
        snprintf(ids, sizeof ids, "%s\\build-ids.txt", fresh);
        int gc = UPD_UNKNOWN, sh = UPD_UNKNOWN;
        if (read_id(ids, gamelib_compat() ? "gamelib_compat" : "gamelib", v, sizeof v)) gc = strcmp(v, gamelib_id()) != 0;
        if (read_id(ids, "translator", v, sizeof v) && read_id(ids, "spirv", v2, sizeof v2))
            sh = strcmp(v, maxwell_translator_id()) || strcmp(v2, vk_spirv_id());
        LOG("[update] after it: game code %s, shaders %s\n", gc < 0 ? "unknown" : gc ? "built again" : "kept",
            sh < 0 ? "unknown" : sh ? "made again" : "kept");
        pthread_mutex_lock(&g_mu);
        g_st.gamecode = gc, g_st.shaders = sh;
        g_st.state = UPD_READY;
        pthread_mutex_unlock(&g_mu);
    } else if (cancelled) {
        LOG("[update] download stopped\n");
        set_state(UPD_AVAILABLE);
    } else
        fail("Download failed: %s", err);
    pthread_mutex_lock(&g_mu);
    g_busy = false;
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

void updater_download(void) {
    if (updater_available()) start(download_thread, UPD_AVAILABLE, UPD_FAILED, UPD_DOWNLOADING);
}

void updater_cancel(void) { InterlockedExchange(&g_cancel, 1); }

// ------------------------------------------------------------------ apply
// The data the player brings or Savage Wind makes, next to the program: never moved, whatever a file list says
static bool protected_rel(const char *rel) {
    static const char *const keep[] = {"extracted/", "save/", "save-profiles/", "cache/", "fonts/", "lang/", "luts/",
                                       "update/", "profile/", "aoc"};
    for (size_t i = 0; i < sizeof keep / sizeof *keep; i++)
        if (!_strnicmp(rel, keep[i], strlen(keep[i]))) return true;
    return !_stricmp(rel, "savagewind.ini") || !_stricmp(rel, "savagewind.log") || !_stricmp(rel, "launch-flags.txt") ||
           !_strnicmp(rel, "save_profile", 12) || !_strnicmp(rel, "splash.", 7);
}

typedef struct { char **v; int n, cap; } List;

static void list_add(List *l, const char *s) {
    if (l->n == l->cap) l->v = (char **)realloc(l->v, sizeof *l->v * (l->cap = l->cap ? l->cap * 2 : 256));
    l->v[l->n++] = _strdup(s);
}

static void list_free(List *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

static bool list_has(const List *l, const char *s) {
    for (int i = 0; i < l->n; i++)
        if (!_stricmp(l->v[i], s)) return true;
    return false;
}

static void walk(const char *root, const char *sub, List *out) {  // every file under root/sub (sub and the names: '/')
    char pat[2048];
    snprintf(pat, sizeof pat, "%s\\%s%s*", root, sub, *sub ? "\\" : "");
    for (char *q = pat; *q; q++) if (*q == '/') *q = '\\';
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char rel[1400];
        snprintf(rel, sizeof rel, "%s%s%s", sub, *sub ? "/" : "", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) walk(root, rel, out);
        else list_add(out, rel);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void read_list(const char *file, List *out) {  // files.txt: one path per line
    FILE *f = fopen(file, "r");
    char line[1100];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (*line) list_add(out, line);
    }
    if (f) fclose(f);
}

static void join(char *out, size_t n, const char *dir, const char *rel) {
    snprintf(out, n, "%s\\%s", dir, rel);
    for (char *q = out; *q; q++) if (*q == '/') *q = '\\';
}

typedef struct { const char *rel; bool had, added; } Step;  // had: a file was there (now in old/); added: new one in

bool updater_apply(bool then_play, char *why, size_t why_size) {
    UpdStatus st;
    updater_status(&st);
    if (st.state != UPD_READY) { snprintf(why, why_size, "nothing to install"); return false; }
    const char *exe_dir = rt_exe_dir();
    char fresh[1200], old[1200], m_old[1300], m_new[1300];
    upd_path(fresh, sizeof fresh, "new");
    upd_path(old, sizeof old, "old");
    snprintf(m_old, sizeof m_old, "%s\\files.txt", exe_dir);
    snprintf(m_new, sizeof m_new, "%s\\files.txt", fresh);
    // (an earlier update's old/ still there - its files were in use at that start - would mix into a rollback, and its
    // applied.txt would stop updater_cleanup from undoing a cut-short install: both have to go first)
    char applied[1300];
    upd_path(applied, sizeof applied, "applied.txt");
    if ((file_exists(old) && !plat_remove_tree(old)) || (file_exists(applied) && remove(applied))) {
        snprintf(why, why_size, "the previous update's files are still in use: start Savage Wind again, then install");
        return false;
    }
    CreateDirectoryA(old, NULL);
    List add = {0}, prev = {0}, next = {0};
    walk(fresh, "", &add);
    read_list(m_old, &prev);  // (none before the first release that ships it: nothing is removed then)
    read_list(m_new, &next);
    // the files this release has and the new one doesn't: moved away too (only names the release itself listed)
    List gone = {0};
    for (int i = 0; i < prev.n; i++)
        if (safe_rel(prev.v[i]) && !protected_rel(prev.v[i]) && !list_has(&add, prev.v[i])) list_add(&gone, prev.v[i]);
    int nsteps = add.n + gone.n, done = 0;
    Step *steps = (Step *)calloc((size_t)nsteps + 1, sizeof *steps);
    bool ok = steps && add.n > 0;
    if (!ok) snprintf(why, why_size, "the update's files are missing");
    // (installing.txt: there while files move - a start that finds it, the install cut short by a crash or a power
    // cut, puts the previous version back from old/: updater_cleanup)
    char busy_mark[1300];
    upd_path(busy_mark, sizeof busy_mark, "installing.txt");
    FILE *bm = ok ? fopen(busy_mark, "w") : NULL;
    if (bm) fprintf(bm, "%s -> %s\n", st.current, st.latest), fclose(bm);
    else if (ok) ok = false, snprintf(why, why_size, "Savage Wind's folder can't be written to");
    char a[2048], b[2048], c[2048];
    for (int i = 0; ok && i < add.n; i++) {
        Step *s = &steps[done];
        s->rel = add.v[i];
        if (!safe_rel(s->rel) || protected_rel(s->rel)) {  // (unzip lets only safe names through; the player's data stays)
            snprintf(why, why_size, "the update has a file Savage Wind won't put in place: %s", s->rel);
            ok = false;
            break;
        }
        join(a, sizeof a, exe_dir, s->rel);
        join(b, sizeof b, old, s->rel);
        join(c, sizeof c, fresh, s->rel);
        mkdirs_for(a);
        if (file_exists(a)) {
            mkdirs_for(b);
            if (!MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING)) {
                snprintf(why, why_size, "%s is in use (error %lu): close other Savage Wind windows", s->rel, (unsigned long)GetLastError());
                ok = false;
                break;
            }
            s->had = true;
        }
        if (!MoveFileExA(c, a, 0)) {
            snprintf(why, why_size, "could not put %s in place (error %lu)", s->rel, (unsigned long)GetLastError());
            if (s->had) MoveFileExA(b, a, 0);
            ok = false;
            break;
        }
        s->added = true;
        done++;
    }
    for (int i = 0; ok && i < gone.n; i++) {
        Step *s = &steps[done];
        s->rel = gone.v[i];
        join(a, sizeof a, exe_dir, s->rel);
        join(b, sizeof b, old, s->rel);
        if (!file_exists(a)) continue;
        mkdirs_for(b);
        if (!MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING)) {
            snprintf(why, why_size, "%s is in use (error %lu)", s->rel, (unsigned long)GetLastError());
            ok = false;
            break;
        }
        s->had = true;
        done++;
    }
    if (!ok) {  // everything back as it was, last step first
        for (int i = done - 1; i >= 0; i--) {
            Step *s = &steps[i];
            join(a, sizeof a, exe_dir, s->rel);
            join(b, sizeof b, old, s->rel);
            join(c, sizeof c, fresh, s->rel);
            if (s->added) { mkdirs_for(c); MoveFileExA(a, c, MOVEFILE_REPLACE_EXISTING); }
            if (s->had) MoveFileExA(b, a, MOVEFILE_REPLACE_EXISTING);
        }
        LOG("[update] install failed, %d step(s) undone: %s\n", done, why);
    } else {
        LOG("[update] %s -> %s: %d file(s) in place, %d old one(s) removed\n", st.current, st.latest, add.n, gone.n);
        char mark[1300];
        upd_path(mark, sizeof mark, "applied.txt");
        FILE *f = fopen(mark, "w");
        if (f) fprintf(f, "%s -> %s\n", st.current, st.latest), fclose(f);
    }
    remove(busy_mark);  // (done either way: in place, or every file back)
    free(steps);
    list_free(&add), list_free(&prev), list_free(&next), list_free(&gone);
    if (!ok) return false;
    char exe[1300];
    snprintf(exe, sizeof exe, "%s\\savagewind.exe", exe_dir);
    // (the new version, on the configuration screen: it builds the game code again, then_play: and starts the game)
    char *av[] = {exe, (char *)"--config", (char *)"--updated", then_play ? (char *)"--then-play" : NULL, NULL};
    // (SAVAGEWIND_UPDATE_FILE: the local zip is this same build, so its game code would match; marked out of date
    // here, as a release that changes the game code finds it, for the new version to build again by itself)
    const char *local = getenv("SAVAGEWIND_UPDATE_FILE");
    if (local && *local) {
        char stamp[1300];
        snprintf(stamp, sizeof stamp, "%s\\extracted\\" GAMELIB_NAME ".id", rt_data_root());
        FILE *s = file_exists(stamp) ? fopen(stamp, "w") : NULL;
        if (s) fputs("local-test-older\n", s), fclose(s), LOG("[update] local test zip: the game code marked out of date\n");
    }
    // (the tests' stand-ins - an older version, a local zip, a test run - are this run's: the installed version is
    // itself, and goes on to its own update page)
    _putenv("SAVAGEWIND_UPDATE_AS=");
    _putenv("SAVAGEWIND_UPDATE_FILE=");
    _putenv("SAVAGEWIND_UPDATE_TEST=");
    plat_exec(av);  // (returns only when the new one didn't start)
    snprintf(why, why_size, "the update is installed: start Savage Wind again");
    return true;
}

// The configuration screen on its update, once this game has ended (its own process: nothing of the game's inherited)
bool updater_start_update(void) {
    if (!updater_available()) return false;
    wchar_t exe[1200], cmd[1500];
    if (!MultiByteToWideChar(CP_UTF8, 0, rt_exe_dir(), -1, exe, 1100)) return false;
    wcscat(exe, L"\\savagewind.exe");
    swprintf(cmd, 1500, L"\"%ls\" --config --update --then-play --wait-pid %lu", exe, (unsigned long)GetCurrentProcessId());
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        LOG("[update] could not start the configuration screen (error %lu)\n", (unsigned long)GetLastError());
        return false;
    }
    AllowSetForegroundWindow(pi.dwProcessId);  // (its window comes up in front once the game's is gone)
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LOG("[update] the configuration screen takes the update over\n");
    return true;
}

bool updater_wait_pid(unsigned long pid, int timeout_ms) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) return true;  // (gone already)
    bool ended = WaitForSingleObject(h, (DWORD)timeout_ms) == WAIT_OBJECT_0;
    CloseHandle(h);
    return ended;
}

// An install cut short (installing.txt still there, no applied.txt): every file in old/ back in place, the files the
// update had put there moved aside first (a running program can be renamed, not replaced). When savagewind.exe was
// one, this process is the half-installed version: the previous one starts again in its place, with the same arguments.
static void undo_cut_install(const char *busy_mark) {
    char old[1200], aside[1200], a[2048], b[2048], c[2048];
    upd_path(old, sizeof old, "old");
    upd_path(aside, sizeof aside, "aside");
    List back = {0};
    walk(old, "", &back);
    int put = 0, failed = 0;
    bool exe = false;
    for (int i = 0; i < back.n; i++) {
        if (!safe_rel(back.v[i]) || protected_rel(back.v[i])) continue;
        join(a, sizeof a, rt_exe_dir(), back.v[i]);
        join(b, sizeof b, old, back.v[i]);
        join(c, sizeof c, aside, back.v[i]);
        if (file_exists(a)) { mkdirs_for(c); MoveFileExA(a, c, MOVEFILE_REPLACE_EXISTING); }
        mkdirs_for(a);
        if (MoveFileExA(b, a, MOVEFILE_REPLACE_EXISTING)) put++, exe |= !_stricmp(back.v[i], "savagewind.exe");
        else failed++;
    }
    list_free(&back);
    remove(busy_mark);
    LOG("[update] an install was cut short: %d file(s) of the previous version put back%s\n", put,
        failed ? ", some could not be" : "");
    if (exe && !failed) plat_exec(NULL);  // (the same command line, now the previous program; returns only on failure)
}

void updater_cleanup(void) {
    if (!rt_exe_dir()[0]) return;
    char mark[1300], old[1200], dir[1200], busy_mark[1300];
    upd_path(mark, sizeof mark, "applied.txt");
    upd_path(busy_mark, sizeof busy_mark, "installing.txt");
    if (!file_exists(mark) && file_exists(busy_mark)) undo_cut_install(busy_mark);
    upd_path(dir, sizeof dir, "");
    if (!file_exists(mark)) {  // (a download in progress, or one waiting to be installed, stays; what an undone one
        char aside[1200];     //  moved aside, and the old/ it emptied, go once they aren't in use)
        upd_path(aside, sizeof aside, "aside");
        if (file_exists(aside)) {
            plat_remove_tree(aside);
            upd_path(old, sizeof old, "old");
            plat_remove_tree(old);
            RemoveDirectoryA(dir);  // (only when nothing else is in it)
        }
        return;
    }
    upd_path(old, sizeof old, "old");
    char line[160] = "";
    FILE *f = fopen(mark, "r");
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
    line[strcspn(line, "\r\n")] = 0;
    // (the previous program may take a moment to end: what is still in use goes at the next start)
    if (!file_exists(old) || plat_remove_tree(old)) {
        plat_remove_tree(dir);
        LOG("[update] updated (%s); the previous version's files removed\n", line);
    } else
        LOG("[update] updated (%s); the previous version's files are still in use, removed at the next start\n", line);
}

static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else  // ------------------------------------------------------------------ Linux: the release AppImage
// A Linux release is one file, Savage_Wind-<version>-<arch>.AppImage. The new one is downloaded next to it (the same
// filesystem, so rename() is atomic), checked, and renamed over it: the running copy keeps its file (its inode) until
// it ends, and the path stays the same, so Steam shortcuts and menu entries still start it. Nothing else is touched:
// game files, saves, settings and caches are in the data folder (runtime/paths.c), never next to the AppImage.
#if defined(__x86_64__)
#define UPD_ARCH "x86_64"
#elif defined(__aarch64__)
#define UPD_ARCH "aarch64"
#else
#define UPD_ARCH "unknown"
#endif
#define UPD_MAX_IMAGE (1024ull << 20)  // (an AppImage is ~150 MB: anything near this is not one)
// The files next to the AppImage while an update is under way. Not named *.AppImage: AppImageLauncher would take a
// new one in a folder it watches for an app to integrate.
#define UPD_FILE_PART ".savagewind-update.part"   // being downloaded
#define UPD_FILE_NEW ".savagewind-update.new"     // downloaded and checked: what Install puts in place
#define UPD_FILE_PREV ".savagewind-previous.old"  // the version it replaced, until the new one has started
#define UPD_FILE_NOTE ".savagewind-updated.txt"   // "<old> -> <new>": the new version's first start finds it

extern char **environ;

static atomic_int g_cancel;
static char g_image[1100], g_image_dir[1100];  // the AppImage this runs from and its folder ("": not from one)
static bool g_image_ours;  // its folder can be written to and the file is the player's

// The release AppImage this process runs from: $APPIMAGE as its runtime sets it - and only when this program is the
// one inside it (under $APPDIR, the runtime's mount). A launcher that is an AppImage itself passed its own $APPIMAGE to
// the games it started (Quiver Launcher before 3.3.3), and a source build started from such a terminal inherits it.
// Its name isn't checked: AppImage managers rename the file. A symbolic link is followed to the file it names.
static void image_find(void) {
    const char *img = getenv("APPIMAGE"), *dir = getenv("APPDIR");
    if (!img || !*img) return;
    char exe[1100], appdir[1100], real[1100];
    struct stat st;
    size_t n;
    if (!dir || !realpath(dir, appdir) || !plat_exe_path(exe, sizeof exe) || (n = strlen(appdir), strncmp(exe, appdir, n)) ||
        exe[n] != '/' || !realpath(img, real) || stat(real, &st) || !S_ISREG(st.st_mode)) {
        LOG("[update] APPIMAGE=%s is not the AppImage this program runs from: no updates of its own\n", img);
        return;
    }
    snprintf(g_image, sizeof g_image, "%s", real);
    snprintf(g_image_dir, sizeof g_image_dir, "%s", real);
    *strrchr(g_image_dir, '/') = 0;
    if (!g_image_dir[0]) snprintf(g_image_dir, sizeof g_image_dir, "/");
    g_image_ours = !access(g_image_dir, W_OK) && st.st_uid == geteuid();
    if (!g_image_ours) LOG("[update] %s can't be replaced by this user: no updates of its own\n", g_image);
}

static void image_init(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, image_find);
}

static void img_path(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", strcmp(g_image_dir, "/") ? g_image_dir : "", name);
}

// A launcher that installs and updates games from their GitHub releases owns this AppImage: its updates, not ours. The
// same signs as on Windows (checked 2026-11), in the AppImage's folder or the one above it:
//   Quiver Launcher (tgeorgiadis/quiver-launcher)   .quiver-version (install.rs VERSION_FILE; it installs .appimage
//                                                   release files on Linux)
//   GitHub Launcher (SirDiabo/N64RecompLauncher)    version.txt (+ LastPlayed.txt / selected_executable.txt above)
//   RecompLauncher (elripalda/RecompLauncher)      RECOMPLAUNCHER=1 for every game it starts; its library is
//                                                   ~/Games/Recomp Library by default
// SAVAGEWIND_UPDATE_MANAGED=<name> (test): as if that launcher installed it.
static const char *g_managed;

static void managed_find(void) {
    image_init();
    char dir[1100], up[1100], p[1300];
    snprintf(dir, sizeof dir, "%s", g_image_dir[0] ? g_image_dir : rt_exe_dir());
    snprintf(up, sizeof up, "%s", dir);
    char *cut = strrchr(up, '/');
    if (cut && cut > up) *cut = 0; else up[0] = 0;  // (the folder above; none at the top)
    #define HAS(d, name) (*(d) && (snprintf(p, sizeof p, "%s/%s", d, name), file_exists(p)))
    const char *test = getenv("SAVAGEWIND_UPDATE_MANAGED");
    const char *rl = getenv("RECOMPLAUNCHER");
    if (test && *test) g_managed = test;
    else if (rl && *rl && strcmp(rl, "0")) g_managed = "RecompLauncher";
    else if (HAS(dir, ".quiver-version") || HAS(up, ".quiver-version")) g_managed = "Quiver Launcher";
    else if (HAS(dir, "version.txt") ||
             (HAS(up, "version.txt") && (HAS(up, "LastPlayed.txt") || HAS(up, "selected_executable.txt"))))
        g_managed = "GitHub Launcher";
    else {
        char low[1200];
        snprintf(low, sizeof low, "%s/", dir);
        for (char *c = low; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (strstr(low, "/recomp library/")) g_managed = "RecompLauncher";
    }
    #undef HAS
    if (g_managed) LOG("[update] installed by %s: updates come from it, Savage Wind's own updater is off\n", g_managed);
}

const char *updater_managed_by(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;  // (worked out once, whichever thread asks first; the others wait)
    pthread_once(&once, managed_find);
    return g_managed;
}

bool updater_available(void) {
    if (getenv("SAVAGEWIND_NO_UPDATER") || updater_managed_by()) return false;
    // (a source build, an extracted AppImage, one in a folder this user can't write to: no updates of its own)
    image_init();
    return g_image[0] && g_image_ours;
}

// ------------------------------------------------------------------ HTTPS (the system's libcurl)
// The system's libcurl, opened the first time it's needed: every distribution has it (Ubuntu, Debian, Fedora, Arch,
// openSUSE, SteamOS), with its own TLS library and certificate store. The AppImage bundles no TLS stack, and the
// certificate store is in a different place on each distribution, so its own would have to guess. Not a link
// dependency: without it Savage Wind starts the same, and a check says what's missing.
// (curl.h's option numbers: they are libcurl's ABI, so its header isn't needed to build)
enum {
    CURLOPT_WRITEDATA = 10001, CURLOPT_URL = 10002, CURLOPT_ERRORBUFFER = 10010, CURLOPT_LOW_SPEED_LIMIT = 19,
    CURLOPT_LOW_SPEED_TIME = 20, CURLOPT_USERAGENT = 10018, CURLOPT_HTTPHEADER = 10023, CURLOPT_FAILONERROR = 45,
    CURLOPT_FOLLOWLOCATION = 52, CURLOPT_MAXREDIRS = 68, CURLOPT_CONNECTTIMEOUT = 78, CURLOPT_NOSIGNAL = 99,
    CURLOPT_PROTOCOLS = 181, CURLOPT_REDIR_PROTOCOLS = 182, CURLOPT_PROTOCOLS_STR = 10318,
    CURLOPT_REDIR_PROTOCOLS_STR = 10319, CURLOPT_WRITEFUNCTION = 20011, CURLINFO_RESPONSE_CODE = 0x200002,
    CURLPROTO_HTTPS = 2, CURL_GLOBAL_DEFAULT = 3, CURLE_HTTP_RETURNED_ERROR = 22,
};

static struct {
    int (*global_init)(long flags);
    void *(*easy_init)(void);
    int (*easy_setopt)(void *curl, int option, ...);
    int (*easy_perform)(void *curl);
    int (*easy_getinfo)(void *curl, int info, ...);
    void (*easy_cleanup)(void *curl);
    const char *(*easy_strerror)(int code);
    void *(*slist_append)(void *list, const char *s);
    void (*slist_free_all)(void *list);
} g_curl;
static bool g_curl_ok;

static void curl_open(void) {
    // (Debian and Ubuntu have two builds of it, against OpenSSL and GnuTLS: either does)
    static const char *const names[] = {"libcurl.so.4", "libcurl-gnutls.so.4"};
    void *h = NULL;
    for (size_t i = 0; !h && i < sizeof names / sizeof *names; i++) h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!h) { LOG("[update] libcurl is not installed: no update checks\n"); return; }
    #define SYM(f) (*(void **)&g_curl.f = dlsym(h, "curl_" #f))
    bool ok = SYM(global_init) && SYM(easy_init) && SYM(easy_setopt) && SYM(easy_perform) && SYM(easy_getinfo) &&
              SYM(easy_cleanup) && SYM(easy_strerror) && SYM(slist_append) && SYM(slist_free_all);
    #undef SYM
    g_curl_ok = ok && g_curl.global_init(CURL_GLOBAL_DEFAULT) == 0;
    if (!g_curl_ok) LOG("[update] libcurl could not be used: no update checks\n");
}

typedef struct { Sink sink; void *user; bool stopped; } CurlSink;

static size_t curl_write(char *p, size_t size, size_t n, void *user) {
    CurlSink *s = (CurlSink *)user;
    if (!s->sink(p, size * n, s->user)) { s->stopped = true; return 0; }
    return size * n;
}

static bool http_get(const char *url, bool api, Sink sink, void *user, char *err, size_t en) {
    if (strncmp(url, "https://", 8)) { snprintf(err, en, "not an https address"); return false; }
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, curl_open);
    if (!g_curl_ok) { snprintf(err, en, "libcurl is not installed (the libcurl4 package, or libcurl)"); return false; }
    void *c = g_curl.easy_init();
    if (!c) { snprintf(err, en, "libcurl could not start"); return false; }
    char agent[96], ebuf[256] = "";  // (CURL_ERROR_SIZE)
    snprintf(agent, sizeof agent, "SavageWind/%s", rt_version());  // (nothing of the player: the program and its version)
    void *hdr = NULL;
    if (api) {
        hdr = g_curl.slist_append(NULL, "Accept: application/vnd.github+json");
        if (hdr) g_curl.slist_append(hdr, "X-GitHub-Api-Version: 2022-11-28");
    }
    CurlSink cs = {sink, user, false};
    g_curl.easy_setopt(c, CURLOPT_URL, url);
    g_curl.easy_setopt(c, CURLOPT_ERRORBUFFER, ebuf);
    g_curl.easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    g_curl.easy_setopt(c, CURLOPT_WRITEDATA, &cs);
    g_curl.easy_setopt(c, CURLOPT_USERAGENT, agent);
    if (hdr) g_curl.easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    g_curl.easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    g_curl.easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);  // (a release file's address redirects to GitHub's storage)
    g_curl.easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    g_curl.easy_setopt(c, CURLOPT_NOSIGNAL, 1L);  // (on a thread of its own: no SIGALRM for timeouts)
    g_curl.easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    g_curl.easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);  // (as WinHTTP's receive timeout: 30 s without a byte ends it)
    g_curl.easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    // https only, the redirects too (libcurl before 7.85 has only the bit masks)
    if (g_curl.easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https") || g_curl.easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https")) {
        g_curl.easy_setopt(c, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
        g_curl.easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
    }
    int r = g_curl.easy_perform(c);
    long code = 0;
    g_curl.easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    bool ok = r == 0 && code == 200;
    if (cs.stopped) snprintf(err, en, "stopped");
    else if (r == CURLE_HTTP_RETURNED_ERROR || (!r && code != 200)) snprintf(err, en, "the server answered %ld", code);
    else if (r) snprintf(err, en, "%s", ebuf[0] ? ebuf : g_curl.easy_strerror(r));
    g_curl.easy_cleanup(c);
    if (hdr) g_curl.slist_free_all(hdr);
    return ok;
}

static bool release_asset(const char *name) {  // this computer's AppImage
    return !strncmp(name, "Savage_Wind-", 12) && ends_with(name, "-" UPD_ARCH ".AppImage");
}

// SAVAGEWIND_UPDATE_FILE=<AppImage> (test): an AppImage on this computer is "the newest release", as this build's
// version - with SAVAGEWIND_UPDATE_AS, the whole update (offer, install, the new version's own rebuild) without GitHub.
// Its SHA-256 is worked out and logged, not checked against anything.
static bool local_release(void) {
    const char *img = getenv("SAVAGEWIND_UPDATE_FILE");
    if (!img || !*img) return false;
    struct stat st;
    bool there = !stat(img, &st) && S_ISREG(st.st_mode);
    pthread_mutex_lock(&g_mu);
    char cur[64];
    snprintf(cur, sizeof cur, "%s", g_st.current);
    bool newer = there && updater_version_cmp(rt_version(), cur) > 0;
    if (newer) {
        snprintf(g_st.latest, sizeof g_st.latest, "%s", rt_version());
        g_st.notes_url[0] = 0;
        g_st.total = (uint64_t)st.st_size, g_st.done = 0;
        snprintf(g_asset_url, sizeof g_asset_url, "file:%s", img);
        g_asset_sha[0] = 0;
        g_st.state = UPD_AVAILABLE;
    } else
        g_st.state = there ? UPD_UP_TO_DATE : UPD_FAILED;
    if (!there) snprintf(g_st.error, sizeof g_st.error, "SAVAGEWIND_UPDATE_FILE: no file %s", img);
    g_busy = false;
    pthread_mutex_unlock(&g_mu);
    LOG("[update] %s: %s (local test AppImage %s)\n", cur, newer ? rt_version() : there ? "up to date" : "no such file", img);
    return true;
}

// ------------------------------------------------------------------ download
typedef struct {
    int fd;
    struct AVSHA *h;
    uint64_t n;
} Dl;

static bool dl_sink(const void *p, size_t n, void *user) {
    Dl *d = (Dl *)user;
    if (atomic_load(&g_cancel) || d->n + n > UPD_MAX_IMAGE) return false;
    for (size_t off = 0; off < n;) {
        ssize_t w = write(d->fd, (const char *)p + off, n - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        off += (size_t)w;
    }
    av_sha_update(d->h, (const uint8_t *)p, n);
    d->n += n;
    pthread_mutex_lock(&g_mu);
    g_st.done = d->n;
    pthread_mutex_unlock(&g_mu);
    return true;
}

// An AppImage for this computer: an ELF program of its architecture with the AppImage type 2 mark ("AI" 2 at byte 8)
static bool is_our_appimage(const char *file) {
    uint8_t h[20];
    FILE *f = fopen(file, "rb");
    bool ok = f && fread(h, 1, sizeof h, f) == sizeof h;
    if (f) fclose(f);
#if defined(__x86_64__)
    const unsigned machine = 62;  // EM_X86_64
#else
    const unsigned machine = 183;  // EM_AARCH64
#endif
    return ok && !memcmp(h, "\x7f" "ELF", 4) && h[4] == 2 && h[8] == 'A' && h[9] == 'I' && h[10] == 2 &&
           (unsigned)(h[18] | h[19] << 8) == machine;
}

static void sync_dir(void) {  // (a rename is on the disk once its folder is)
    int fd = open(g_image_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) fsync(fd), close(fd);
}

static void *download_thread(void *arg) {
    (void)arg;
    char url[1024], sha[65], part[1300], ready[1300], err[300] = "";
    pthread_mutex_lock(&g_mu);
    snprintf(url, sizeof url, "%s", g_asset_url);
    snprintf(sha, sizeof sha, "%s", g_asset_sha);
    g_st.done = 0;
    pthread_mutex_unlock(&g_mu);
    atomic_store(&g_cancel, 0);
    img_path(part, sizeof part, UPD_FILE_PART);
    img_path(ready, sizeof ready, UPD_FILE_NEW);
    remove(ready);  // (an earlier, not installed one)
    Dl d = {open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644), av_sha_alloc(), 0};
    bool ok = d.fd >= 0 && d.h && av_sha_init(d.h, 256) == 0;
    if (!ok) snprintf(err, sizeof err, d.fd < 0 ? "the AppImage's folder can't be written to" : "SHA-256 is not available");
    else if (!strncmp(url, "file:", 5)) {  // (SAVAGEWIND_UPDATE_FILE: copied in chunks, as a download)
        FILE *in = fopen(url + 5, "rb");
        static char chunk[1 << 18];
        size_t n;
        ok = in != NULL;
        while (ok && in && (n = fread(chunk, 1, sizeof chunk, in)) > 0) ok = dl_sink(chunk, n, &d);
        if (in) fclose(in);
        if (!ok) snprintf(err, sizeof err, in ? "the local AppImage could not be copied" : "no local AppImage %s", url + 5);
    } else ok = http_get(url, false, dl_sink, &d, err, sizeof err);
    // (all of it on the disk before it can be renamed into place: a power cut afterwards must not leave half of it)
    if (d.fd >= 0 && fsync(d.fd) && ok) ok = false, snprintf(err, sizeof err, "the download could not be written");
    if (d.fd >= 0) close(d.fd);
    uint8_t h[32];
    char hex[65] = "";
    if (d.h) {
        av_sha_final(d.h, h);
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", h[i]);
        av_free(d.h);
    }
    bool cancelled = atomic_load(&g_cancel) != 0;
    if (ok && sha[0] && strcmp(hex, sha)) {  // (sha empty: the local test AppImage, nothing to check it against)
        ok = false;
        snprintf(err, sizeof err, "the download doesn't match the release's SHA-256 (%.12s..., expected %.12s...)", hex, sha);
    }
    if (ok && !is_our_appimage(part)) ok = false, snprintf(err, sizeof err, "the download is not an AppImage for " UPD_ARCH);
    if (ok && (chmod(part, 0755) || rename(part, ready))) ok = false, snprintf(err, sizeof err, "could not keep the download");
    if (!ok) remove(part);
    if (ok) {
        sync_dir();
        LOG("[update] downloaded %llu bytes, SHA-256 %s %s\n", (unsigned long long)d.n, hex,
            sha[0] ? "matches" : "(local test AppImage: nothing to check it against)");
        // (what the game code and the shaders need is the new version's own finding when it starts: an AppImage's
        // build-ids.txt is inside it, and nothing of it runs before it is installed)
        pthread_mutex_lock(&g_mu);
        g_st.gamecode = g_st.shaders = UPD_UNKNOWN;
        g_st.state = UPD_READY;
        pthread_mutex_unlock(&g_mu);
    } else if (cancelled) {
        LOG("[update] download stopped\n");
        set_state(UPD_AVAILABLE);
    } else
        fail("Download failed: %s", err);
    pthread_mutex_lock(&g_mu);
    g_busy = false;
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

void updater_download(void) {
    if (updater_available()) start(download_thread, UPD_AVAILABLE, UPD_FAILED, UPD_DOWNLOADING);
}

void updater_cancel(void) { atomic_store(&g_cancel, 1); }

// ------------------------------------------------------------------ apply
// The environment for the new version: without what this AppImage's runtime set (the new one sets its own, and an
// old $APPIMAGE would name this file) and, tests=false, without the tests' stand-ins - an older version, a local
// AppImage, a test run - which are this run's: the installed version is itself, and goes on to its own update page.
static char **child_env(bool tests) {
    static const char *const drop[] = {"APPIMAGE=", "APPDIR=", "ARGV0=", "OWD=", "SAVAGEWIND_UPDATE_AS=",
                                       "SAVAGEWIND_UPDATE_FILE=", "SAVAGEWIND_UPDATE_TEST="};
    size_t n = 0, k = 0;
    while (environ[n]) n++;
    char **env = (char **)malloc((n + 1) * sizeof *env);
    if (!env) return environ;
    for (size_t i = 0; i < n; i++) {
        bool keep = true;
        for (size_t j = 0; j < sizeof drop / sizeof *drop && keep; j++)
            keep = (tests && j >= 4) || strncmp(environ[i], drop[j], strlen(drop[j]));
        if (keep) env[k++] = environ[i];
    }
    env[k] = NULL;
    return env;
}

bool updater_apply(bool then_play, char *why, size_t why_size) {
    UpdStatus st;
    updater_status(&st);
    if (st.state != UPD_READY) { snprintf(why, why_size, "nothing to install"); return false; }
    char ready[1300], prev[1300], note[1300];
    img_path(ready, sizeof ready, UPD_FILE_NEW);
    img_path(prev, sizeof prev, UPD_FILE_PREV);
    img_path(note, sizeof note, UPD_FILE_NOTE);
    // (an earlier update's leftovers would be taken for this one's: its previous version for the one to put back, its
    // note for this update's)
    if ((file_exists(prev) && remove(prev)) || (file_exists(note) && remove(note))) {
        snprintf(why, why_size, "the previous update's files next to the AppImage can't be removed");
        return false;
    }
    if (!is_our_appimage(ready)) { snprintf(why, why_size, "the update's file is missing"); return false; }
    // The version this one replaces, kept until the new one has started: a hard link, the same file under a second
    // name (nothing copied). Filesystems without them (exFAT and FAT, on SD cards) go without: if the new version then
    // doesn't start, it can't be put back.
    bool backup = !link(g_image, prev);
    if (!backup) LOG("[update] no copy of the previous version kept (%s)\n", strerror(errno));
    // (written first: a start that finds it with this version still in place knows the install was cut short)
    FILE *f = fopen(note, "w");
    if (f) fprintf(f, "%s -> %s\n", st.current, st.latest), fclose(f);
    if (rename(ready, g_image)) {
        snprintf(why, why_size, "could not put the new version in place (%s)", strerror(errno));
        if (backup) remove(prev);
        remove(note);
        LOG("[update] install failed: %s\n", why);
        return false;
    }
    sync_dir();
    LOG("[update] %s -> %s: %s replaced\n", st.current, st.latest, g_image);
    // (SAVAGEWIND_UPDATE_FILE: the local AppImage is this same build, so its game code would match; marked out of date
    // here, as a release that changes the game code finds it, for the new version to build again by itself)
    const char *local = getenv("SAVAGEWIND_UPDATE_FILE");
    if (local && *local) {
        char stamp[1300];
        snprintf(stamp, sizeof stamp, "%s/extracted/" GAMELIB_NAME ".id", rt_data_root());
        FILE *s = file_exists(stamp) ? fopen(stamp, "w") : NULL;
        if (s) fputs("local-test-older\n", s), fclose(s), LOG("[update] local test AppImage: the game code marked out of date\n");
    }
    // the new version, on the configuration screen: it builds the game code again, then_play: and starts the game.
    // Started from its own file, not this process's program: that is the old AppImage's, mounted from the file it had.
    char *av[] = {g_image, (char *)"--config", (char *)"--updated", then_play ? (char *)"--then-play" : NULL, NULL};
    fflush(stderr);
    char **env = child_env(false);
    execve(g_image, av, env);
    int e = errno;
    if (env != environ) free(env);
    if (backup && !rename(prev, g_image)) {  // (it didn't start: the previous version back in its place)
        remove(note);
        sync_dir();
        snprintf(why, why_size, "the new version didn't start (%s): nothing was changed", strerror(e));
        LOG("[update] %s\n", why);
        return false;
    }
    snprintf(why, why_size, "the update is installed: start Savage Wind again");
    return true;
}

// The configuration screen on its update, once this game has ended: a process of its own (its own session, so the
// game's terminal or launcher closing doesn't take it along), with none of the game's open files - the save folder's
// lock among them, which it would otherwise go on holding after the game has ended.
bool updater_start_update(void) {
    if (!updater_available()) return false;
    char pid[24];
    snprintf(pid, sizeof pid, "%lu", (unsigned long)getpid());
    char *av[] = {g_image, (char *)"--config", (char *)"--update", (char *)"--then-play", (char *)"--wait-pid", pid, NULL};
    char **env = child_env(true);  // (made before fork: only async-signal-safe calls in the child)
    struct rlimit rl;
    int max_fd = !getrlimit(RLIMIT_NOFILE, &rl) && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < 65536 ? (int)rl.rlim_cur : 65536;
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return false;  // (the child's exec closes it: nothing read means it started)
    pid_t c = fork();
    if (c == 0) {
        setsid();
        for (int fd = 3; fd < max_fd; fd++)
            if (fd != p[1]) close(fd);
        execve(g_image, av, env);
        int e = errno;
        if (write(p[1], &e, sizeof e) < 0) e = 0;
        _exit(127);
    }
    close(p[1]);
    int e = 0;
    ssize_t r = -1;
    if (c > 0)
        do r = read(p[0], &e, sizeof e); while (r < 0 && errno == EINTR);
    close(p[0]);
    if (env != environ) free(env);
    if (c < 0 || r > 0) {
        LOG("[update] could not start the configuration screen (%s)\n", strerror(c < 0 ? errno : e));
        if (c > 0) waitpid(c, NULL, 0);
        return false;
    }
    LOG("[update] the configuration screen takes the update over\n");
    return true;
}

bool updater_wait_pid(unsigned long pid, int timeout_ms) {
    if (!pid) return true;
    for (int t = 0;; t += 100) {
        if (kill((pid_t)pid, 0) && errno == ESRCH) return true;
        // (ended, its parent not having collected it yet: a zombie holds nothing)
        char p[64], line[512];
        snprintf(p, sizeof p, "/proc/%lu/stat", pid);
        FILE *f = fopen(p, "r");
        bool zombie = false;
        if (f) {
            if (fgets(line, sizeof line, f)) {
                const char *s = strrchr(line, ')');
                zombie = s && s[1] == ' ' && s[2] == 'Z';
            }
            fclose(f);
        }
        if (zombie) return true;
        if (t >= timeout_ms) return false;
        usleep(100 * 1000);
    }
}

static void remove_if_old(const char *file, int seconds) {  // (another Savage Wind may be downloading it right now)
    struct stat st;
    if (!stat(file, &st) && time(NULL) - st.st_mtime > seconds) remove(file);
}

void updater_cleanup(void) {
    image_init();
    if (!g_image[0] || !g_image_ours) return;
    char note[1300], prev[1300], part[1300], ready[1300];
    img_path(note, sizeof note, UPD_FILE_NOTE);
    img_path(prev, sizeof prev, UPD_FILE_PREV);
    img_path(part, sizeof part, UPD_FILE_PART);
    img_path(ready, sizeof ready, UPD_FILE_NEW);
    char line[160] = "";
    FILE *f = fopen(note, "r");
    if (f) {
        if (!fgets(line, sizeof line, f)) line[0] = 0;
        fclose(f);
        line[strcspn(line, "\r\n")] = 0;
        // the new version started: the one it replaced isn't needed any more. Still the old one here: the install was
        // cut short before the new one was in place (rename() is all or nothing), so nothing changed.
        const char *to = strstr(line, " -> ");
        if (to && !updater_version_cmp(to + 4, rt_version())) LOG("[update] updated (%s); the previous version removed\n", line);
        else LOG("[update] an install was cut short before the new version was in place: nothing changed (%s)\n", line);
        remove(prev);
        remove(note);
    } else if (file_exists(prev))
        remove(prev);
    remove_if_old(part, 24 * 3600);
    remove_if_old(ready, 24 * 3600);
}

static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif  // (Linux)

// ------------------------------------------------------------------ test
int updater_test(const char *steps) {
    if (!updater_available()) {
        LOG("[update] test: %s\n", updater_managed_by() ? "updates come from the launcher" : "not a release folder");
        return 1;
    }
    UpdStatus u;
    if (start(check_thread, UPD_IDLE, UPD_IDLE, UPD_CHECKING))
        for (updater_status(&u); u.state == UPD_CHECKING; updater_status(&u)) sleep_ms(50);
    updater_status(&u);
    LOG("[update] test: check -> state %d, latest %s\n", u.state, u.latest);
    if (strstr(steps, "download") || strstr(steps, "apply")) {
        if (u.state != UPD_AVAILABLE) return u.state == UPD_UP_TO_DATE ? 0 : 1;
        updater_download();
        for (updater_status(&u); u.state == UPD_DOWNLOADING; updater_status(&u)) sleep_ms(100);
        LOG("[update] test: download -> state %d (%s), game code %d, shaders %d\n", u.state, u.error, u.gamecode, u.shaders);
        if (u.state != UPD_READY) return 1;
    }
    if (strstr(steps, "apply")) {
        char why[300] = "";
        bool ok = updater_apply(false, why, sizeof why);
        LOG("[update] test: apply -> %s\n", ok ? why : why[0] ? why : "failed");
        return 1;
    }
    return u.state == UPD_FAILED;
}
#endif
