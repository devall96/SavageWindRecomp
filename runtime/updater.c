// Updates of Savage Wind itself (runtime/updater.h): Windows release folders only.
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

#ifndef _WIN32
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
const char *updater_just_updated(void) { return ""; }
void updater_cleanup(void) {}
int updater_test(const char *steps) { (void)steps; fprintf(stderr, "[update] not available here\n"); return 1; }
#else

#include <pthread.h>
#include <sys/stat.h>
#include <windows.h>
// (after windows.h: they use its types)
#include <bcrypt.h>
#include <winhttp.h>
#include <zlib.h>

#include "gamelib.h"
#include "paths.h"
#include "platform.h"
#include "rt.h"

const char *maxwell_translator_id(void);  // runtime/nvn/maxwell/translate.c (TRANSLATOR_ID)
const char *vk_spirv_id(void);            // runtime/nvn/vulkan/spirv.c (SPIRV_ID)

// The project's releases. SAVAGEWIND_UPDATE_REPO=<owner>/<repo> (tests: a fork's releases), SAVAGEWIND_UPDATE_AS=<version>
// (tests: this build taken for that version, so that a check finds the latest release).
#define UPD_REPO "Revivedx/SavageWindRecomp"
#define UPD_MAX_ZIP (1024ull << 20)  // (a release zip is ~90 MB: anything near this is not one)
#define UPD_MAX_FILE (512u << 20)

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static UpdStatus g_st;
static bool g_init, g_busy;
static volatile LONG g_cancel;
static char g_zip_url[1024], g_zip_sha[65];

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
typedef bool (*Sink)(const void *p, size_t n, void *user);  // false: stop

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
        snprintf(g_zip_url, sizeof g_zip_url, "file:%s", zip);
        g_zip_sha[0] = 0;
        g_st.state = UPD_AVAILABLE;
    } else
        g_st.state = there ? UPD_UP_TO_DATE : UPD_FAILED;
    if (!there) snprintf(g_st.error, sizeof g_st.error, "SAVAGEWIND_UPDATE_FILE: no file %s", zip);
    g_busy = false;
    pthread_mutex_unlock(&g_mu);
    LOG("[update] %s: %s (local test zip %s)\n", cur, newer ? rt_version() : there ? "up to date" : "no such file", zip);
    return true;
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
        for (J *r = list->kid; r; r = r->next) {
            const char *tag = jstr(r, "tag_name"), *draft = jstr(r, "draft");
            if (!tag || (draft && !strcmp(draft, "true"))) continue;
            const char *ver = tag[0] == 'v' ? tag + 1 : tag;
            if (!isdigit((unsigned char)ver[0]) || strlen(ver) >= sizeof g_st.latest) continue;
            if (updater_version_cmp(ver, cur) <= 0 || (best_ver && updater_version_cmp(ver, best_ver) <= 0)) continue;
            J *assets = jget(r, "assets");
            for (J *a = assets && assets->type == 'a' ? assets->kid : NULL; a; a = a->next) {
                const char *name = jstr(a, "name"), *dl = jstr(a, "browser_download_url"), *dg = jstr(a, "digest");
                // the Windows zip, with the SHA-256 GitHub publishes for every asset, from GitHub itself
                if (name && dl && dg && !strncmp(name, "SavageWind-", 11) && ends_with(name, "-windows.zip") &&
                    !strncmp(dl, "https://github.com/", 19) && strlen(dl) < sizeof g_zip_url && !strncmp(dg, "sha256:", 7) &&
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
            snprintf(g_zip_url, sizeof g_zip_url, "%s", jstr(best_zip, "browser_download_url"));
            snprintf(g_zip_sha, sizeof g_zip_sha, "%s", jstr(best_zip, "digest") + 7);
            for (char *c = g_zip_sha; *c; c++) *c = (char)tolower((unsigned char)*c);
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

// A path inside the release folder: relative, '/'-separated, no "." / ".." / empty parts, no drive or stream names
static bool safe_rel(const char *rel) {
    if (!*rel || *rel == '/' || strlen(rel) > 400 || strpbrk(rel, "\\:*?\"<>|")) return false;
    for (const char *p = rel; *p;) {
        size_t n = strcspn(p, "/");
        if (!n || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return false;
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
    snprintf(url, sizeof url, "%s", g_zip_url);
    snprintf(sha, sizeof sha, "%s", g_zip_sha);
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
    plat_remove_tree(old);
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
    // (the tests' stand-ins - an older version, a local zip - are this run's: the installed version is itself)
    _putenv("SAVAGEWIND_UPDATE_AS=");
    _putenv("SAVAGEWIND_UPDATE_FILE=");
    plat_exec(av);  // (returns only when the new one didn't start)
    snprintf(why, why_size, "the update is installed: start Savage Wind again");
    return true;
}

static char g_just[160];
const char *updater_just_updated(void) { return g_just; }

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
    snprintf(g_just, sizeof g_just, "%s", line);
    // (the previous program may take a moment to end: what is still in use goes at the next start)
    if (!file_exists(old) || plat_remove_tree(old)) {
        plat_remove_tree(dir);
        LOG("[update] updated (%s); the previous version's files removed\n", line);
    } else
        LOG("[update] updated (%s); the previous version's files are still in use, removed at the next start\n", line);
}

int updater_test(const char *steps) {
    if (!updater_available()) {
        LOG("[update] test: %s\n", updater_managed_by() ? "updates come from the launcher" : "not a release folder");
        return 1;
    }
    UpdStatus u;
    if (start(check_thread, UPD_IDLE, UPD_IDLE, UPD_CHECKING))
        for (updater_status(&u); u.state == UPD_CHECKING; updater_status(&u)) Sleep(50);
    updater_status(&u);
    LOG("[update] test: check -> state %d, latest %s\n", u.state, u.latest);
    if (strstr(steps, "download") || strstr(steps, "apply")) {
        if (u.state != UPD_AVAILABLE) return u.state == UPD_UP_TO_DATE ? 0 : 1;
        updater_download();
        for (updater_status(&u); u.state == UPD_DOWNLOADING; updater_status(&u)) Sleep(100);
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
