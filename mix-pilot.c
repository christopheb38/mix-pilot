/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Mix Pilot: next-track suggestions on the XDJ-AZ.
 *
 * Watches what the decks play, finds the playing track in the rekordbox library on the USB
 * sticks, and shows the tracks that follow it best, as a panel on the BROWSE and SEARCH
 * screens and as a page in the Mod Menu. Two modes:
 *
 *   Next tracks   the best K tracks to play next, best first
 *   Chain         an ordered plan of K tracks, each mixing well into the next
 *
 * Signals (engine.c): Camelot key, tempo (half/double time included), style (Last.fm tags and
 * shared playlists), Last.fm track similarity, and transitions actually played: yours (learned
 * on the deck as you mix) and the community's (a pairings file in the package, plus an
 * optional one fetched from a URL).
 *
 * Threads (docs/GUIDELINES.md, rule 3):
 *   UI thread    tick, rows, the page and the panel: reads deck_info, draws a snapshot. No I/O.
 *   "mp worker"  reads the library, loads and appends the files, scores, builds the snapshot.
 *   "mp net"     Last.fm and the pairings download (HTTPS through the deck's libcurl).
 * They meet in short mutexes (copies only, never held across I/O) and atomics.
 *
 * Files, all on the USB in data_dir(): config.txt (API key, pairings URL), my-transitions.tsv
 * (what you mixed), community.tsv (downloaded pairings), lastfm-cache.tsv (web answers).
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "azmod-mod.h"
#include "engine.h"
#include "online.h"

#define MOD_ID "mix-pilot"
#define NDECKS 4
#define MAX_EX 96               /* tracks excluded: loaded on other decks + played this session */
#ifndef SETTLE_S
#define SETTLE_S 40.0           /* a track that played this long counts as played */
#endif
#ifndef LIB_RETRY_S
#define LIB_RETRY_S 10.0        /* an empty library is read again this often */
#endif
#ifndef REF_STABLE_S
#define REF_STABLE_S 6.0        /* a deck must play this long before it becomes the reference */
#endif
#define CACHE_MAX (8u << 20)    /* lastfm-cache.tsv is reset past this size */

static const az_core_v1 *C;

/* ---- settings ---------------------------------------------------------------------------
 * Kept on the mod's own page (one row in the Mod Menu opens it), saved with cfg_set. Read from
 * any thread as atomics; changed on the UI thread only. */

#include "i18n.h"

enum { S_PANEL, S_MODE, S_COUNT, S_FLOW, S_REF, S_ARTIST, S_LEARN, S_ONLINE, S_LANG, S_N };
static const struct {
    const char *key;
    int label, def, n;
} SET[S_N] = {
    { "panel", T_PANEL, 0, 3 }, { "mode", T_MODE, 0, 2 },     { "count", T_COUNT, 1, 4 },
    { "flow", T_FLOW, 0, 3 },   { "ref", T_REF, 0, 5 },       { "artist", T_ARTIST, 1, 2 },
    { "learn", T_LEARN, 1, 2 }, { "online", T_ONLINE, 1, 2 }, { "lang", T_LANG, 0, LANG_N },
};
static atomic_int g_set[S_N];
static const int COUNTS[] = { 4, 5, 8, 10 };
static const int FLOW_T[3] = { T_SMOOTH, T_UP, T_DOWN };

static int S(int i)
{
    return atomic_load(&g_set[i]);
}

/* A text in the chosen language (English when a translation is missing). */
static const char *tr(int id)
{
    int l = S(S_LANG);
    if (l < 0 || l >= LANG_N) l = 0;
    return TR[id][l] ? TR[id][l] : TR[id][0];
}

/* The label of value v of setting i, in the chosen language. */
static void choice_text(int i, int v, char *buf, size_t n)
{
    static const int panel_t[] = { T_RIGHT, T_LEFT, T_HIDDEN };
    switch (i) {
    case S_PANEL: snprintf(buf, n, "%s", tr(panel_t[v % 3])); break;
    case S_MODE: snprintf(buf, n, "%s", tr(v ? T_CHAIN : T_NEXT)); break;
    case S_COUNT: snprintf(buf, n, "%d", COUNTS[v & 3]); break;
    case S_FLOW: snprintf(buf, n, "%s", tr(FLOW_T[v % 3])); break;
    case S_REF:
        if (v == 0) snprintf(buf, n, "%s", tr(T_AUTO));
        else snprintf(buf, n, tr(T_DECK), v);
        break;
    case S_LANG: snprintf(buf, n, "%s", LANG_NAMES[v % LANG_N]); break;
    default: snprintf(buf, n, "%s", tr(v ? T_ON : T_OFF)); break;
    }
}

static void load_settings(void)
{
    for (int i = 0; i < S_N; i++) {
        int v = AZ_HAS(C, cfg_get) ? C->cfg_get(MOD_ID, SET[i].key, SET[i].def) : SET[i].def;
        if (v < 0 || v >= SET[i].n) v = SET[i].def;
        atomic_store(&g_set[i], v);
    }
}

/* ---- UI -> worker -------------------------------------------------------------------- */

struct ident_ref {
    uint32_t id;
    char title[128], artist[128];
};
struct learn_ev {
    char fa[128], ft[128], ta[128], tt[128];
};

static pthread_mutex_t g_in_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int deck;                   /* -1 = no reference track */
    struct ident_ref t;
    double bpm;                 /* the deck's current tempo */
} g_ref = { -1, { 0, "", "" }, 0 };
static struct ident_ref g_ex[MAX_EX];
static int g_nex;
static struct learn_ev g_learn[8];
static int g_nlearn;

static atomic_int want_reload = 1, want_compute = 1, settings_changed = 1;

/* ---- worker -> UI: what the panel and the page draw ------------------------------------ */

typedef struct {
    char title[128], artist[96], meta[24], why[96], where[64];
    float score;
    int known;                  /* a transition someone actually played */
} vrow;

static pthread_mutex_t g_view_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int n[3], chain;                 /* one list per energy tab: Smooth, Build up, Cool down */
    vrow r[3][NT_MAX_RESULTS];
    char head[160], status[128];
} g_view;
static atomic_int view_fresh = 1;

/* ---- the library: worker and net threads ----------------------------------------------- */

static pthread_mutex_t g_lib_mu = PTHREAD_MUTEX_INITIALIZER;
static nt_lib g_lib;
static int g_lib_ready;

/* worker -> net: what to look up */
static pthread_mutex_t g_net_mu = PTHREAD_MUTEX_INITIALIZER;
struct net_job {
    uint32_t ident;
    char artist[128], title[128];
    int sim;                    /* 1 = similar tracks + tags, 0 = tags only */
};
static struct net_job g_jobs[24];
static int g_njobs;
static char g_cfg_key[64], g_cfg_url[256];
static atomic_int net_state;    /* 0 idle, 1 no key, 2 no libcurl, 3 ok, 4 failing */
static atomic_int community_pending = 1;

/* ---- small helpers --------------------------------------------------------------------- */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void nap_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* The data and assets folders, resolved once in init(). Before each use we check the folder
 * is still there: after an eject only the empty mount point stays, so the check fails and the
 * mod carries on without its files (it never creates these folders). */
static char g_data_dir[384], g_assets_dir[384];

static const char *usable(const char *dir)
{
    struct stat st;
    return dir[0] && stat(dir, &st) == 0 && S_ISDIR(st.st_mode) ? dir : NULL;
}
static const char *data_dir(void) { return usable(g_data_dir); }
static const char *assets_dir(void) { return usable(g_assets_dir); }

static void path_in(char *out, size_t n, const char *dir, const char *name)
{
    snprintf(out, n, "%s/%s", dir, name);
}

static void chomp(char *s)
{
    size_t l = strlen(s);
    while (l && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' ')) s[--l] = '\0';
}

/* Splits a TSV line in place; returns the number of fields. */
static int split_tabs(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        f[n++] = p;
        char *t = strchr(p, '\t');
        if (!t) break;
        *t = '\0';
        p = t + 1;
    }
    return n;
}

/* ---- files (worker and net threads only) ----------------------------------------------- */

static void read_config(void)
{
    const char *dir = data_dir();
    if (!dir) return;
    char p[512], line[400];
    path_in(p, sizeof p, dir, "config.txt");
    FILE *f = fopen(p, "re");
    if (!f) {
        f = fopen(p, "we");
        if (f) {
            fputs("# Mix Pilot settings. Edit on your computer, then Reload in the Mod Menu.\n"
                  "#\n"
                  "# Free Last.fm API key (style tags and similar tracks):\n"
                  "#   https://www.last.fm/api/account/create\n"
                  "lastfm_api_key=\n"
                  "#\n"
                  "# Optional shared pairings file, https:// only, same format as my-transitions.tsv:\n"
                  "pairings_url=\n",
                  f);
            fclose(f);
        }
        return;
    }
    char key[64] = "", url[256] = "";
    while (fgets(line, sizeof line, f)) {
        chomp(line);
        if (strncmp(line, "lastfm_api_key=", 15) == 0) snprintf(key, sizeof key, "%s", line + 15);
        else if (strncmp(line, "pairings_url=", 13) == 0) snprintf(url, sizeof url, "%s", line + 13);
    }
    fclose(f);
    pthread_mutex_lock(&g_net_mu);
    snprintf(g_cfg_key, sizeof g_cfg_key, "%s", key);
    snprintf(g_cfg_url, sizeof g_cfg_url, "%s", url);
    pthread_mutex_unlock(&g_net_mu);
}

/* "fromArtist \t fromTitle \t toArtist \t toTitle [\t count [\t when]]", '#' = comment. */
static int load_pairings(nt_lib *L, const char *path, float weight)
{
    FILE *f = fopen(path, "re");
    if (!f) return 0;
    char line[1024], *fl[6];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        chomp(line);
        if (!line[0] || line[0] == '#') continue;
        int k = split_tabs(line, fl, 6);
        if (k < 4 || !fl[1][0] || !fl[3][0]) continue;
        float c = k >= 5 ? (float)atof(fl[4]) : 1.0f;
        if (c <= 0) c = 1;
        if (c > 50) c = 50;
        nt_pair_add(&L->pairs, nt_ident(fl[0], fl[1]), nt_ident(fl[2], fl[3]), c * weight);
        n++;
    }
    fclose(f);
    return n;
}

static void load_cache(nt_lib *L, const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && (size_t)st.st_size > CACHE_MAX) {
        remove(path);                                     /* it rebuilds itself */
        return;
    }
    FILE *f = fopen(path, "re");
    if (!f) return;
    char line[1024], *fl[2 + 2 * NT_TRACK_TAGS];
    while (fgets(line, sizeof line, f)) {
        chomp(line);
        int k = split_tabs(line, fl, 2 + 2 * NT_TRACK_TAGS);
        if (k < 2) continue;
        uint32_t a = (uint32_t)strtoul(fl[1], NULL, 16);
        if (!a) continue;
        if (fl[0][0] == 'S') nt_pair_set(&L->sim, a, 0, 1);           /* similar: fetched */
        else if (fl[0][0] == 'P' && k >= 4) {
            uint32_t b = (uint32_t)strtoul(fl[2], NULL, 16);
            if (b) nt_pair_set(&L->sim, a, b, (float)atof(fl[3]));
        } else if (fl[0][0] == 'T') {
            int tag[NT_TRACK_TAGS], w[NT_TRACK_TAGS], n = 0;
            for (int i = 2; i + 1 < k && n < NT_TRACK_TAGS; i += 2) {
                int ix = nt_tag_index(L, fl[i]);
                if (ix < 0) continue;
                tag[n] = ix;
                w[n++] = atoi(fl[i + 1]);
            }
            if (n) nt_set_tags(L, a, tag, w, n);
            nt_pair_set(&L->sim, 0, a, 1);                            /* tags: fetched */
        }
    }
    fclose(f);
}

static void append_line(const char *name, const char *text)
{
    const char *dir = data_dir();
    if (!dir) return;
    char p[512];
    path_in(p, sizeof p, dir, name);
    FILE *f = fopen(p, "ae");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

/* ---- reading the library (worker) ------------------------------------------------------ */

struct pl_list {
    int id[4096], folder[4096];
    char name[4096][48];
    int n;
};
struct add_ctx {
    nt_lib *L;
    int stick, pl;
    const char *name;
};

static int on_playlist(int pl_id, int parent, const char *name, int folder, void *user)
{
    (void)parent;
    struct pl_list *p = user;
    if (p->n >= 4096) return 1;
    p->id[p->n] = pl_id;
    p->folder[p->n] = folder;
    snprintf(p->name[p->n], sizeof p->name[0], "%s", name ? name : "");
    p->n++;
    return 0;
}

static int on_track_all(const az_track *t, void *user)
{
    struct add_ctx *a = user;
    nt_lib_add(a->L, a->stick, t->id, t->title, t->artist, t->album, t->key, t->bpm, t->len_s);
    return a->L->n >= a->L->cap;
}

static int on_track_pl(const az_track *t, void *user)
{
    struct add_ctx *a = user;
    int i = nt_find(a->L, a->stick, t->id, 0);
    if (i < 0) i = nt_lib_add(a->L, a->stick, t->id, t->title, t->artist, t->album, t->key, t->bpm, t->len_s);
    nt_lib_in_playlist(a->L, i, a->pl, a->name);
    return 0;
}

static atomic_int g_rc[2][2];     /* per stick: lib_tracks(all) and lib_playlists results, for the screen */
static pthread_mutex_t g_diag_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_diag[96];          /* what we found on the sticks when the core gave no library */
static atomic_int g_diag_code;   /* 0 none, 1 no stick, 2 export.pdb only, 3 port not recognized, 4 refused */

/* The USB port of a block device, by the same rule as the core (sdk_lib.c): 1, 2, or 0 unknown. */
static int port_of_sysfs(const char *real)
{
    const char *h = real;
    while ((h = strstr(h, "/host")) && !(h[5] >= '0' && h[5] <= '9')) h++;
    if (!h) return 0;
    const char *b = h, *a;
    do b--;
    while (b > real && *b != '/');
    if (b <= real) return 0;
    a = b;
    do a--;
    while (a > real && *a != '/');
    if (*a != '/') return 0;
    size_t n = (size_t)(b - a - 1);
    char bus[16];
    if (n == 0 || n >= sizeof bus) return 0;
    memcpy(bus, a + 1, n);
    bus[n] = 0;
    if (!strcmp(bus, "1-1") || !strcmp(bus, "3-1")) return 1;
    if (!strcmp(bus, "2-1") || !strcmp(bus, "4-1")) return 2;
    return 0;
}

/* When the core reads no library: look at the sticks ourselves and say why, in plain words. */
static void diagnose(void)
{
    char out[96] = "";
    int code = 1, o = 0;
    FILE *f = fopen("/proc/mounts", "re");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char dev[128], mp[400];
            if (sscanf(line, "%127s %399s", dev, mp) != 2 || strncmp(dev, "/dev/", 5) != 0 ||
                strncmp(mp, "/media/usb", 10) != 0)
                continue;
            char p[600], sys[300], real[600];
            snprintf(p, sizeof p, "%s/PIONEER/rekordbox/exportLibrary.db", mp);
            int db = access(p, R_OK) == 0;
            snprintf(p, sizeof p, "%s/PIONEER/rekordbox/export.pdb", mp);
            int pdb = access(p, R_OK) == 0;
            snprintf(sys, sizeof sys, "/sys/class/block/%s", dev + 5);
            int port = realpath(sys, real) ? port_of_sysfs(real) : 0;
            int c = db ? (port ? 4 : 3) : pdb ? 2 : 1;
            if (c > code || code == 1) code = c > code ? c : code;
            if (o < (int)sizeof out - 1)
                o += snprintf(out + o, sizeof out - (size_t)o, "%s%s:%s%s p%d", o ? " " : "", dev + 5,
                              db ? "db" : "", pdb ? (db ? "+pdb" : "pdb") : (db ? "" : "-"), port);
        }
        fclose(f);
    }
    pthread_mutex_lock(&g_diag_mu);
    snprintf(g_diag, sizeof g_diag, "%s", out);
    pthread_mutex_unlock(&g_diag_mu);
    atomic_store(&g_diag_code, code);
    C->log(MOD_ID, "sticks: %s (code %d)", out[0] ? out : "none under /media/usb", code);
}

static int build_library(nt_lib *L)
{
    if (nt_lib_init(L, NT_MAX_TRACKS)) return -1;
    struct pl_list *pls = malloc(sizeof *pls);
    if (!pls) return -1;
    int plbase = 0;
    for (int stick = 0; stick < 2; stick++) {
        struct add_ctx a = { L, stick, 0, "" };
        int rt = C->lib_tracks(MOD_ID, stick, 0, on_track_all, &a);
        pls->n = 0;
        int rp = C->lib_playlists(MOD_ID, stick, on_playlist, pls);
        atomic_store(&g_rc[stick][0], rt);
        atomic_store(&g_rc[stick][1], rp);
        C->log(MOD_ID, "USB %d: all tracks -> %d (%d so far), playlists -> %d (%d)", stick + 1, rt, L->n, rp, pls->n);
        if (rp < 0) continue;                             /* tracks only, or no library there */
        for (int i = 0; i < pls->n && plbase + i < 4096; i++) {
            if (pls->folder[i]) continue;
            a.pl = plbase + i;
            a.name = pls->name[i];
            C->lib_tracks(MOD_ID, stick, pls->id[i], on_track_pl, &a);
        }
        plbase += pls->n;
    }
    free(pls);
    if (!L->n) diagnose();
    const char *dir = data_dir(), *assets = assets_dir();
    char p[512];
    if (assets) {
        path_in(p, sizeof p, assets, "pairings.tsv");
        load_pairings(L, p, 1.0f);
    }
    if (dir) {
        path_in(p, sizeof p, dir, "community.tsv");
        load_pairings(L, p, 1.0f);
        path_in(p, sizeof p, dir, "my-transitions.tsv");
        load_pairings(L, p, 1.0f);
        path_in(p, sizeof p, dir, "lastfm-cache.tsv");
        load_cache(L, p);
    }
    return 0;
}

/* ---- scoring and the snapshot (worker) ------------------------------------------------- */

static void mark_ident(const nt_lib *L, uint8_t *ex, uint32_t ident)
{
    for (int i = 0; i < L->n; i++)
        if (L->t[i].ident == ident) ex[i] = 1;
}

static int find_ref(const nt_lib *L, const struct ident_ref *r)
{
    uint32_t ident = nt_ident(r->artist, r->title);
    if (r->id) {                                          /* id on either stick, same song */
        for (int i = 0; i < L->n; i++)
            if (L->t[i].id == r->id && L->t[i].ident == ident) return i;
    }
    int i = nt_find(L, -1, 0, ident);
    if (i >= 0) return i;
    return r->id && !r->title[0] ? nt_find(L, -1, r->id, 0) : -1;
}

static void queue_job(uint32_t ident, const char *artist, const char *title, int sim)
{
    pthread_mutex_lock(&g_net_mu);
    int dup = 0;
    for (int i = 0; i < g_njobs; i++)
        if (g_jobs[i].ident == ident && g_jobs[i].sim >= sim) dup = 1;
    if (!dup) {
        if (sim && g_njobs) {                             /* the playing track goes first */
            if (g_njobs == (int)(sizeof g_jobs / sizeof g_jobs[0])) g_njobs--;
            memmove(&g_jobs[1], &g_jobs[0], sizeof g_jobs[0] * (size_t)g_njobs);
            g_njobs++;
            struct net_job *j = &g_jobs[0];
            j->ident = ident;
            j->sim = sim;
            snprintf(j->artist, sizeof j->artist, "%s", artist);
            snprintf(j->title, sizeof j->title, "%s", title);
        } else if (g_njobs < (int)(sizeof g_jobs / sizeof g_jobs[0])) {
            struct net_job *j = &g_jobs[g_njobs++];
            j->ident = ident;
            j->sim = sim;
            snprintf(j->artist, sizeof j->artist, "%s", artist);
            snprintf(j->title, sizeof j->title, "%s", title);
        }
    }
    pthread_mutex_unlock(&g_net_mu);
}

static void status_line(char *s, size_t n, const nt_lib *L)
{
    int ns = atomic_load(&net_state);
    const char *net = ns == 1 ? tr(T_NET_KEY) : ns == 2 ? tr(T_NET_OFFLINE) : ns == 3 ? tr(T_NET_ON)
                    : ns == 4 ? tr(T_NET_FAIL) : "";
    if (!S(S_ONLINE)) net = tr(T_NET_DISABLED);
    if (!L->n) {                                          /* what the core answered, and what we saw */
        char diag[96];
        pthread_mutex_lock(&g_diag_mu);
        snprintf(diag, sizeof diag, "%s", g_diag);
        pthread_mutex_unlock(&g_diag_mu);
        snprintf(s, n, "USB1 %d/%d  USB2 %d/%d  %s", atomic_load(&g_rc[0][0]), atomic_load(&g_rc[0][1]),
                 atomic_load(&g_rc[1][0]), atomic_load(&g_rc[1][1]), diag);
        return;
    }
    char a[48], b[64];
    snprintf(a, sizeof a, tr(T_TRACKS), L->n);
    snprintf(b, sizeof b, tr(T_KNOWN), L->pairs.n);
    snprintf(s, n, "%s  -  %s%s%s", a, b, net[0] ? "  -  " : "", net);
}

/* One line of reasons, in the chosen language: "8A>9A  -0.8%  house  mixed 3x". */
static void reason_text(const nt_lib *L, const nt_reason *r, int from, int to, char *buf, size_t n)
{
    char ka[4], kb[4], part[64];
    size_t o = 0;
    buf[0] = '\0';
    nt_camelot_str(L->t[from].camelot, ka);
    nt_camelot_str(L->t[to].camelot, kb);
    if (ka[0] && kb[0]) o += (size_t)snprintf(buf + o, n - o, "%s>%s", ka, kb);
    else if (kb[0]) o += (size_t)snprintf(buf + o, n - o, "%s", kb);
    if (o < n && L->t[to].bpm > 0)
        o += (size_t)snprintf(buf + o, n - o, "%s%+.1f%%%s", o ? "  " : "", r->pct, r->halfdouble ? " x2" : "");
    if (o < n && r->shared_tag >= 0 && r->shared_tag < L->ntags)
        o += (size_t)snprintf(buf + o, n - o, "  %s", L->tags[r->shared_tag]);
    if (o < n && r->pair_count > 0) {
        snprintf(part, sizeof part, tr(T_MIXED), r->pair_count);
        o += (size_t)snprintf(buf + o, n - o, "  %s", part);
    } else if (o < n && r->sim >= 0.15f) {
        snprintf(part, sizeof part, tr(T_SIMILAR), (int)(r->sim * 100 + 0.5f));
        o += (size_t)snprintf(buf + o, n - o, "  %s", part);
    }
}

static void compute(void)
{
    struct { int deck; struct ident_ref t; double bpm; } ref;
    struct ident_ref ex[MAX_EX];
    int nex;
    pthread_mutex_lock(&g_in_mu);
    ref.deck = g_ref.deck;
    ref.t = g_ref.t;
    ref.bpm = g_ref.bpm;
    nex = g_nex;
    memcpy(ex, g_ex, sizeof ex[0] * (size_t)nex);
    pthread_mutex_unlock(&g_in_mu);

    int chain = S(S_MODE) == 1;
    int k = COUNTS[S(S_COUNT) & 3];
    int online = S(S_ONLINE), cur = S(S_FLOW);

    static vrow rows[3][NT_MAX_RESULTS];
    int nr[3] = { 0, 0, 0 };
    char head[160], status[128];

    pthread_mutex_lock(&g_lib_mu);
    nt_lib *L = &g_lib;
    status_line(status, sizeof status, L);
    int from = ref.deck >= 0 ? find_ref(L, &ref.t) : -1;
    if (!g_lib_ready) snprintf(head, sizeof head, "%s", tr(T_READING));
    else if (!L->n) {
        static const int dt[] = { T_NOLIB, T_D_NOUSB, T_D_PDB, T_D_HUB, T_D_REFUSED };
        int c = atomic_load(&g_diag_code);
        snprintf(head, sizeof head, "%s", tr(dt[c >= 0 && c <= 4 ? c : 0]));
    } else if (ref.deck < 0) snprintf(head, sizeof head, "%s", tr(T_PLAY));
    else if (from < 0) snprintf(head, sizeof head, tr(T_NOTIN), ref.deck + 1);
    else {
        char kc[4];
        nt_camelot_str(L->t[from].camelot, kc);
        snprintf(head, sizeof head, "%s D%d: %s  %.1f %s", tr(chain ? T_PLAN : T_AFTER), ref.deck + 1,
                 nt_str(L, L->t[from].title), ref.bpm > 0 ? ref.bpm : L->t[from].bpm, kc);
        uint8_t *exb = calloc((size_t)L->n, 1);
        if (exb) {
            mark_ident(L, exb, L->t[from].ident);
            for (int i = 0; i < nex; i++) mark_ident(L, exb, nt_ident(ex[i].artist, ex[i].title));
            for (int f = 0; f < 3; f++) {                 /* one list per energy tab */
                nt_ctx ctx = { f, S(S_ARTIST), exb };
                int out[NT_MAX_RESULTS];
                nt_reason why[NT_MAX_RESULTS];
                int n = chain ? nt_chain(L, &ctx, from, ref.bpm, k, out, why)
                              : nt_rank(L, &ctx, from, ref.bpm, k, out, why);
                for (int i = 0; i < n; i++) {
                    const nt_track *t = &L->t[out[i]];
                    vrow *r = &rows[f][i];
                    int prev = chain && i ? out[i - 1] : from;
                    char kc2[4];
                    nt_camelot_str(t->camelot, kc2);
                    snprintf(r->title, sizeof r->title, "%s", nt_str(L, t->title));
                    snprintf(r->artist, sizeof r->artist, "%s", nt_str(L, t->artist));
                    if (t->bpm > 0) snprintf(r->meta, sizeof r->meta, "%.1f  %s", t->bpm, kc2);
                    else snprintf(r->meta, sizeof r->meta, "%s", kc2);
                    reason_text(L, &why[i], prev, out[i], r->why, sizeof r->why);
                    snprintf(r->where, sizeof r->where, "%s%s", t->stick ? "USB2 " : "", nt_str(L, t->where));
                    r->score = why[i].total;
                    r->known = why[i].pair_count > 0;
                    /* ask the web about the shown tracks we know nothing about yet */
                    if (online && f == cur && !nt_pair_get(&L->sim, 0, t->ident))
                        queue_job(t->ident, nt_str(L, t->artist), nt_str(L, t->title), 0);
                }
                nr[f] = n;
            }
            free(exb);
        }
        if (online && !nt_pair_get(&L->sim, L->t[from].ident, 0))
            queue_job(L->t[from].ident, nt_str(L, L->t[from].artist), nt_str(L, L->t[from].title), 1);
    }
    pthread_mutex_unlock(&g_lib_mu);

    pthread_mutex_lock(&g_view_mu);
    g_view.chain = chain;
    for (int f = 0; f < 3; f++) {
        g_view.n[f] = nr[f];
        memcpy(g_view.r[f], rows[f], sizeof rows[0][0] * (size_t)nr[f]);
    }
    snprintf(g_view.head, sizeof g_view.head, "%s", head);
    snprintf(g_view.status, sizeof g_view.status, "%s", status);
    pthread_mutex_unlock(&g_view_mu);
    atomic_store(&view_fresh, 1);
}

static void take_learning(void)
{
    struct learn_ev ev[8];
    int n;
    pthread_mutex_lock(&g_in_mu);
    n = g_nlearn;
    memcpy(ev, g_learn, sizeof ev[0] * (size_t)n);
    g_nlearn = 0;
    pthread_mutex_unlock(&g_in_mu);
    for (int i = 0; i < n; i++) {
        char line[700], when[32] = "";
        time_t t = time(NULL);
        struct tm tm;
        if (gmtime_r(&t, &tm)) strftime(when, sizeof when, "%Y-%m-%dT%H:%MZ", &tm);
        snprintf(line, sizeof line, "%s\t%s\t%s\t%s\t1\t%s\n", ev[i].fa, ev[i].ft, ev[i].ta, ev[i].tt, when);
        append_line("my-transitions.tsv", line);
        pthread_mutex_lock(&g_lib_mu);
        nt_pair_add(&g_lib.pairs, nt_ident(ev[i].fa, ev[i].ft), nt_ident(ev[i].ta, ev[i].tt), 1);
        pthread_mutex_unlock(&g_lib_mu);
    }
    if (n) atomic_store(&want_compute, 1);
}

static void *worker(void *arg)
{
    (void)arg;
    int loops = 0;                                        /* the loop naps 120 ms: count, no clock */
    for (;;) {
        /* the stick may not be readable yet at start, or plugged in later: try again */
        if (g_lib_ready && ++loops >= (int)(LIB_RETRY_S / 0.12)) {
            loops = 0;
            pthread_mutex_lock(&g_lib_mu);
            int empty = g_lib.n == 0;
            pthread_mutex_unlock(&g_lib_mu);
            if (empty) atomic_store(&want_reload, 1);
        }
        if (atomic_exchange(&want_reload, 0)) {
            read_config();
            nt_lib fresh, old;
            if (build_library(&fresh) == 0) {
                pthread_mutex_lock(&g_lib_mu);
                old = g_lib;
                g_lib = fresh;
                g_lib_ready = 1;
                pthread_mutex_unlock(&g_lib_mu);
                nt_lib_free(&old);
                C->log(MOD_ID, "library: %d tracks, %u transitions, %u web pairs", fresh.n, fresh.pairs.n,
                       fresh.sim.n);
            } else {
                nt_lib_free(&fresh);
            }
            atomic_store(&want_compute, 1);
        }
        take_learning();
        if (atomic_exchange(&want_compute, 0)) compute();
        nap_ms(120);
    }
    return NULL;
}

/* ---- the network thread ---------------------------------------------------------------- */

struct sim_hit {
    uint32_t dst;
    float match;
};
struct sim_acc {
    struct sim_hit v[160];
    int n;
    char artists[50][128];
    float amatch[50];
    int na;
};

static int on_sim(const char *artist, const char *title, double match, void *user)
{
    struct sim_acc *a = user;
    if (a->n >= 160) return 1;
    a->v[a->n].dst = nt_ident(artist, title);
    a->v[a->n].match = (float)(match < 0 ? 0 : match > 1 ? 1 : match);
    a->n++;
    return 0;
}

static int on_sim_artist(const char *artist, double match, void *user)
{
    struct sim_acc *a = user;
    if (a->na >= 50) return 1;
    snprintf(a->artists[a->na], sizeof a->artists[0], "%s", artist);
    a->amatch[a->na++] = (float)match;
    return 0;
}

struct tag_acc {
    char tag[NT_TRACK_TAGS][24];
    int w[NT_TRACK_TAGS], n;
};

static int on_tag(const char *tag, int count, void *user)
{
    struct tag_acc *a = user;
    if (count < 5 && a->n) return 1;                      /* the tail says little */
    snprintf(a->tag[a->n], sizeof a->tag[0], "%s", tag);
    a->w[a->n++] = count < 1 ? 1 : count;
    return a->n >= 6;
}

static void fetch_tags(const char *key, const struct net_job *j)
{
    struct tag_acc ta = { .n = 0 };
    int r = lfm_track_tags(key, j->artist, j->title, on_tag, &ta);
    if (r <= 0 && j->artist[0]) {
        nap_ms(220);
        r = lfm_artist_tags(key, j->artist, on_tag, &ta);
    }
    if (r < 0 && ta.n == 0) {
        atomic_store(&net_state, 4);
        return;
    }
    atomic_store(&net_state, 3);
    char line[600];
    int o = snprintf(line, sizeof line, "T\t%08x", j->ident);
    for (int i = 0; i < ta.n && o < (int)sizeof line - 40; i++) o += snprintf(line + o, sizeof line - (size_t)o, "\t%s\t%d", ta.tag[i], ta.w[i]);
    snprintf(line + o, sizeof line - (size_t)o, "\n");
    append_line("lastfm-cache.tsv", line);
    pthread_mutex_lock(&g_lib_mu);
    int tag[NT_TRACK_TAGS], w[NT_TRACK_TAGS], n = 0;
    for (int i = 0; i < ta.n; i++) {
        int ix = nt_tag_index(&g_lib, ta.tag[i]);
        if (ix >= 0) { tag[n] = ix; w[n++] = ta.w[i]; }
    }
    if (n) nt_set_tags(&g_lib, j->ident, tag, w, n);
    nt_pair_set(&g_lib.sim, 0, j->ident, 1);
    pthread_mutex_unlock(&g_lib_mu);
}

static void fetch_similar(const char *key, const struct net_job *j)
{
    struct sim_acc *a = calloc(1, sizeof *a);
    if (!a) return;
    int r = lfm_similar_tracks(key, j->artist, j->title, 150, on_sim, a);
    if (r < 0) {
        atomic_store(&net_state, 4);
        free(a);
        return;
    }
    atomic_store(&net_state, 3);
    if (a->n < 8 && j->artist[0]) {                       /* little known track: similar artists */
        nap_ms(220);
        lfm_similar_artists(key, j->artist, 50, on_sim_artist, a);
    }
    /* turn similar artists into pairs with the library tracks by them */
    static struct sim_hit extra[400];
    int nextra = 0;
    pthread_mutex_lock(&g_lib_mu);
    for (int k = 0; k < a->na; k++) {
        uint32_t aid = nt_ident(a->artists[k], "");
        for (int i = 0; i < g_lib.n && nextra < 400; i++)
            if (g_lib.t[i].aident == aid) {
                extra[nextra].dst = g_lib.t[i].ident;
                extra[nextra++].match = a->amatch[k] * 0.5f;
            }
    }
    for (int i = 0; i < a->n; i++) nt_pair_set(&g_lib.sim, j->ident, a->v[i].dst, a->v[i].match);
    for (int i = 0; i < nextra; i++) nt_pair_set(&g_lib.sim, j->ident, extra[i].dst, extra[i].match);
    nt_pair_set(&g_lib.sim, j->ident, 0, 1);
    pthread_mutex_unlock(&g_lib_mu);

    const char *dir = data_dir();
    if (dir) {
        char p[512];
        path_in(p, sizeof p, dir, "lastfm-cache.tsv");
        FILE *f = fopen(p, "ae");
        if (f) {
            for (int i = 0; i < a->n; i++) fprintf(f, "P\t%08x\t%08x\t%.3f\n", j->ident, a->v[i].dst, a->v[i].match);
            for (int i = 0; i < nextra; i++) fprintf(f, "P\t%08x\t%08x\t%.3f\n", j->ident, extra[i].dst, extra[i].match);
            fprintf(f, "S\t%08x\n", j->ident);
            fclose(f);
        }
    }
    free(a);
}

static void fetch_community(const char *url)
{
    char *body = NULL;
    size_t len = 0;
    int code = net_get(url, &body, &len, 4u << 20, 30);
    if (code == 200 && body) {
        const char *dir = data_dir();
        if (dir) {
            char p[512], tmp[520];
            path_in(p, sizeof p, dir, "community.tsv");
            snprintf(tmp, sizeof tmp, "%s.tmp", p);
            FILE *f = fopen(tmp, "we");
            if (f) {
                int ok = fwrite(body, 1, len, f) == len;
                ok = fclose(f) == 0 && ok;
                if (ok && rename(tmp, p) == 0) {
                    C->log(MOD_ID, "community pairings: %zu bytes", len);
                    atomic_store(&want_reload, 1);
                } else {
                    remove(tmp);
                }
            }
        }
    } else {
        C->log(MOD_ID, "community pairings: download failed (%d)", code);
    }
    free(body);
}

static void *net_thread(void *arg)
{
    (void)arg;
    int backoff = 0;                                      /* naps of 500 ms left before trying again */
    for (;;) {
        if (!S(S_ONLINE)) {
            nap_ms(1000);
            continue;
        }
        char key[64], url[256];
        pthread_mutex_lock(&g_net_mu);
        snprintf(key, sizeof key, "%s", g_cfg_key);
        snprintf(url, sizeof url, "%s", g_cfg_url);
        pthread_mutex_unlock(&g_net_mu);
        if (net_available() != 0) {
            if (atomic_exchange(&net_state, 2) != 2) atomic_store(&want_compute, 1);
            nap_ms(5000);
            continue;
        }
        if (url[0] && atomic_exchange(&community_pending, 0)) fetch_community(url);
        if (!key[0]) {
            if (atomic_exchange(&net_state, 1) != 1) atomic_store(&want_compute, 1);
            nap_ms(2000);
            continue;
        }
        if (backoff > 0) {
            backoff--;
            nap_ms(500);
            continue;
        }
        struct net_job j;
        int have = 0;
        pthread_mutex_lock(&g_net_mu);
        if (g_njobs) {
            j = g_jobs[0];
            memmove(&g_jobs[0], &g_jobs[1], sizeof g_jobs[0] * (size_t)(g_njobs - 1));
            g_njobs--;
            have = 1;
        }
        pthread_mutex_unlock(&g_net_mu);
        if (!have) {
            nap_ms(300);
            continue;
        }
        int before = atomic_load(&net_state);
        if (j.sim) {
            fetch_similar(key, &j);
            nap_ms(220);
        }
        pthread_mutex_lock(&g_lib_mu);
        int need_tags = !nt_pair_get(&g_lib.sim, 0, j.ident);
        pthread_mutex_unlock(&g_lib_mu);
        if (need_tags) fetch_tags(key, &j);
        if (atomic_load(&net_state) == 4) backoff = 60;  /* the WiFi may be down: 30 s */
        if (atomic_load(&net_state) != before || j.sim) atomic_store(&want_compute, 1);
        else {
            pthread_mutex_lock(&g_net_mu);
            int left = g_njobs;
            pthread_mutex_unlock(&g_net_mu);
            if (!left) atomic_store(&want_compute, 1);    /* a batch of tags is in: rescore once */
        }
        nap_ms(220);                                      /* Last.fm: at most ~5 calls a second */
    }
    return NULL;
}

/* ---- watching the decks (UI thread) ---------------------------------------------------- */

static struct {
    uint32_t id;
    char title[128], artist[128];
    int loaded, playing, counted;
    double play_since, played_s;
    double bpm;
} dk[NDECKS];
static struct ident_ref played[MAX_EX];
static int nplayed;
static struct ident_ref settled;   /* the last track that played SETTLE_S: the "from" of a transition */
static int have_settled;
static double last_poll;
static double last_reload_ask;

static void push_learn(const struct ident_ref *from, const struct ident_ref *to)
{
    pthread_mutex_lock(&g_in_mu);
    if (g_nlearn < 8) {
        struct learn_ev *e = &g_learn[g_nlearn++];
        snprintf(e->fa, sizeof e->fa, "%s", from->artist);
        snprintf(e->ft, sizeof e->ft, "%s", from->title);
        snprintf(e->ta, sizeof e->ta, "%s", to->artist);
        snprintf(e->tt, sizeof e->tt, "%s", to->title);
    }
    pthread_mutex_unlock(&g_in_mu);
}

static int same_track(const struct ident_ref *a, const char *artist, const char *title)
{
    return strcmp(a->title, title) == 0 && strcmp(a->artist, artist) == 0;
}

static void poll_decks(void)
{
    double t = now_s(), dt = last_poll ? t - last_poll : 0;
    last_poll = t;
    if (dt > 2) dt = 0;                                   /* a long gap: don't count it as play */
    int ndecks = C->deck_count();
    if (ndecks != 2 && ndecks != 4) return;               /* unknown: do nothing this round */
    for (int d = 0; d < NDECKS; d++) {
        az_deck_info di;
        memset(&di, 0, sizeof di);
        di.size = sizeof di;
        if (d >= ndecks || C->deck_info(d, &di) != 0 || di.loaded != 1 || !di.title[0]) {
            dk[d].loaded = 0;
            dk[d].playing = 0;
            continue;
        }
        if (!dk[d].loaded || dk[d].id != di.content_id || strcmp(dk[d].title, di.title) != 0 ||
            strcmp(dk[d].artist, di.artist) != 0) {
            memset(&dk[d], 0, sizeof dk[d]);
            dk[d].loaded = 1;
            dk[d].id = di.content_id;
            snprintf(dk[d].title, sizeof dk[d].title, "%s", di.title);
            snprintf(dk[d].artist, sizeof dk[d].artist, "%s", di.artist);
        }
        dk[d].bpm = di.bpm;
        int playing = di.playing == 1;
        if (playing && !dk[d].playing) dk[d].play_since = t;
        dk[d].playing = playing;
        if (playing) dk[d].played_s += dt;
        if (dk[d].played_s >= SETTLE_S && !dk[d].counted) {
            dk[d].counted = 1;
            struct ident_ref me;
            me.id = dk[d].id;
            snprintf(me.title, sizeof me.title, "%s", dk[d].title);
            snprintf(me.artist, sizeof me.artist, "%s", dk[d].artist);
            int seen = 0;
            for (int i = 0; i < nplayed; i++)
                if (same_track(&played[i], me.artist, me.title)) seen = 1;
            if (!seen) {
                if (nplayed == MAX_EX - NDECKS) {         /* keep the most recent */
                    memmove(&played[0], &played[1], sizeof played[0] * (size_t)(nplayed - 1));
                    nplayed--;
                }
                played[nplayed++] = me;
            }
            if (have_settled && !same_track(&settled, me.artist, me.title) && S(S_LEARN))
                push_learn(&settled, &me);
            settled = me;
            have_settled = 1;
        }
    }

    /* the reference deck */
    static int ref = -1;
    int mode = S(S_REF), pick = -1;
    if (mode >= 1 && mode <= 4) {
        if (mode - 1 < ndecks && dk[mode - 1].loaded) pick = mode - 1;
    } else {
        double best = -1;
        for (int d = 0; d < ndecks; d++)
            if (dk[d].loaded && dk[d].playing && t - dk[d].play_since >= REF_STABLE_S && dk[d].play_since > best) {
                best = dk[d].play_since;
                pick = d;
            }
        if (pick < 0 && ref >= 0 && ref < ndecks && dk[ref].loaded) pick = ref;   /* nothing new: keep it */
        if (pick < 0) {
            int only = -1, count = 0;
            for (int d = 0; d < ndecks; d++)
                if (dk[d].loaded) { only = d; count++; }
            if (count == 1) pick = only;                  /* one track loaded: plan after it */
        }
    }
    ref = pick;

    /* hand the worker what changed */
    pthread_mutex_lock(&g_in_mu);
    int changed = 0;
    if (pick != g_ref.deck || (pick >= 0 && (g_ref.t.id != dk[pick].id || strcmp(g_ref.t.title, dk[pick].title) ||
                                             strcmp(g_ref.t.artist, dk[pick].artist)))) {
        changed = 1;
        g_ref.deck = pick;
        if (pick >= 0) {
            g_ref.t.id = dk[pick].id;
            snprintf(g_ref.t.title, sizeof g_ref.t.title, "%s", dk[pick].title);
            snprintf(g_ref.t.artist, sizeof g_ref.t.artist, "%s", dk[pick].artist);
            g_ref.bpm = dk[pick].bpm;
        }
    } else if (pick >= 0 && dk[pick].bpm > 0 && (g_ref.bpm - dk[pick].bpm > 0.3 || dk[pick].bpm - g_ref.bpm > 0.3)) {
        changed = 1;
        g_ref.bpm = dk[pick].bpm;
    }
    struct ident_ref ex[MAX_EX];
    int nex = 0;
    for (int d = 0; d < ndecks; d++)
        if (dk[d].loaded && d != pick) {
            ex[nex].id = dk[d].id;
            snprintf(ex[nex].title, sizeof ex[nex].title, "%s", dk[d].title);
            snprintf(ex[nex].artist, sizeof ex[nex].artist, "%s", dk[d].artist);
            nex++;
        }
    for (int i = 0; i < nplayed && nex < MAX_EX; i++) ex[nex++] = played[i];
    if (nex != g_nex || memcmp(ex, g_ex, sizeof ex[0] * (size_t)nex) != 0) {
        changed = 1;
        memcpy(g_ex, ex, sizeof ex[0] * (size_t)nex);
        g_nex = nex;
    }
    pthread_mutex_unlock(&g_in_mu);
    if (changed) atomic_store(&want_compute, 1);
}

static void on_deck(int deck, int what, void *user)
{
    (void)user;
    if (deck < 0 || deck >= NDECKS) return;
    /* a track the library does not know may come from a stick plugged in since: re-read, at
     * most once a minute (the worker reports "not in the USB library" until then) */
    if (what == AZ_DECK_LOADED && now_s() - last_reload_ask > 60) {
        pthread_mutex_lock(&g_view_mu);
        int missing = strstr(g_view.head, "not in the USB library") != NULL;
        pthread_mutex_unlock(&g_view_mu);
        if (missing) {
            last_reload_ask = now_s();
            atomic_store(&want_reload, 1);
        }
    }
    last_poll = last_poll ? last_poll : now_s();
    poll_decks();
}

/* ---- drawing (UI / overlay thread) ----------------------------------------------------- */

static az_surface *panel;
static int panel_small;          /* long press folds the panel to its header */

static void fit(char *out, size_t n, const char *s, int px, int w)
{
    snprintf(out, n, "%s", s);
    size_t len = strlen(out);
    while (len > 0 && C->text_width(out, px) > w) {
        do len--;
        while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80);
        snprintf(out, n, "%.*s...", (int)len, s);
    }
    if (C->text_width(out, px) > w) out[0] = '\0';
}

static void text(az_draw *d, int x, int y, int w, const char *s, int px, uint32_t rgb)
{
    char f[256];
    if (w <= 0) return;
    fit(f, sizeof f, s, px, w);
    if (f[0]) C->draw_text(d, x, y, f, px, rgb);
}

static uint32_t score_color(float s, int known)
{
    if (known) return 0x3fa9ff;                           /* a played transition: blue */
    if (s >= 0.62f) return 0x2ecc71;
    if (s >= 0.45f) return 0xf1c40f;
    return 0xe67e22;
}

/* Where the tab bar was drawn, for taps (UI thread). */
static int tabs_y0 = -1, tabs_h, tabs_x0, tabs_w;

/* The energy tabs: Smooth | Build up | Cool down, the current one filled. */
static void draw_tabs(az_draw *d, int x, int y, int w, int h, int px)
{
    const uint32_t hi = C->color(AZ_COL_HI), fg = C->color(AZ_COL_TEXT), dim = C->color(AZ_COL_TEXT_DIM);
    int cur = S(S_FLOW), tw = w / 3;
    for (int f = 0; f < 3; f++) {
        int tx = x + f * tw;
        if (f == cur) C->fill_rect(d, tx + 2, y, tw - 4, h, hi);
        else C->draw_rect(d, tx + 2, y, tw - 4, h, 1, dim);
        const char *t = tr(FLOW_T[f]);
        int lw = C->text_width(t, px);
        if (lw > tw - 8) lw = tw - 8;
        text(d, tx + (tw - lw) / 2, y + (h - px * 3 / 2) / 2, tw - 8, t, px,
             f == cur ? C->color(AZ_COL_HI_TEXT) : fg);
    }
    tabs_y0 = y;
    tabs_h = h;
    tabs_x0 = x;
    tabs_w = w;
}

/* The list, in area: used by both the panel and the Mod Menu page. */
static void draw_list(az_draw *d, az_rect a, int px, int header)
{
    const uint32_t fg = C->color(AZ_COL_TEXT), dim = C->color(AZ_COL_TEXT_DIM), hi = C->color(AZ_COL_HI);
    const int pad = px / 2, lh = px * 3 / 2, small = px * 4 / 5 > 8 ? px * 4 / 5 : 8, slh = small * 3 / 2;
    int y = a.y, f = S(S_FLOW) % 3;
    pthread_mutex_lock(&g_view_mu);
    if (header) {
        C->fill_rect(d, a.x, a.y, a.w, 3, hi);
        char h[64];
        snprintf(h, sizeof h, "MIX PILOT  -  %s", tr(g_view.chain ? T_HDR_CHAIN : T_HDR_NEXT));
        text(d, a.x + pad, y + 6, a.w - 2 * pad, h, small, hi);
        y += 6 + slh;
    }
    text(d, a.x + pad, y, a.w - 2 * pad, g_view.head, small, fg);
    y += slh + 4;
    draw_tabs(d, a.x + pad, y, a.w - 2 * pad, slh + 8, small);
    y += slh + 8 + 6;
    int bar = px / 3 > 3 ? px / 3 : 3;
    for (int i = 0; i < g_view.n[f] && y + lh + slh <= a.y + a.h - slh; i++) {
        const vrow *r = &g_view.r[f][i];
        int metaw = C->text_width(r->meta, small) + pad;
        C->fill_rect(d, a.x + pad, y + 2, bar, lh + slh - 6, score_color(r->score, r->known));
        int x = a.x + pad + bar + pad;
        char num[8], line[240];
        snprintf(num, sizeof num, "%d", i + 1);
        C->draw_text(d, x, y, num, px, dim);
        x += C->text_width("10", px) + pad / 2;
        snprintf(line, sizeof line, "%s%s%s", r->title, r->artist[0] ? " - " : "", r->artist);
        text(d, x, y, a.x + a.w - pad - metaw - x, line, px, fg);
        C->draw_text(d, a.x + a.w - pad - metaw + pad, y + (lh - slh) / 2, r->meta, small, fg);
        snprintf(line, sizeof line, "%s%s%s%s", r->why, r->where[0] ? "   " : "", r->where[0] ? tr(T_IN) : "", "");
        if (r->where[0]) snprintf(line + strlen(line), sizeof line - strlen(line), " %s", r->where);
        text(d, x, y + lh, a.x + a.w - pad - x, line, small, dim);
        y += lh + slh + 4;
    }
    text(d, a.x + pad, a.y + a.h - slh, a.w - 2 * pad, g_view.status, small, dim);
    pthread_mutex_unlock(&g_view_mu);
}

static void panel_paint(az_draw *d, az_rect a, void *user)
{
    (void)user;
    if (panel_small) {
        const uint32_t hi = C->color(AZ_COL_HI);
        C->fill_rect(d, a.x, a.y, a.w, 3, hi);
        pthread_mutex_lock(&g_view_mu);
        char s[200];
        int f = S(S_FLOW) % 3;
        snprintf(s, sizeof s, "MIX PILOT  %s", g_view.n[f] ? g_view.r[f][0].title : "");
        pthread_mutex_unlock(&g_view_mu);
        text(d, a.x + 8, a.y + 8, a.w - 16, s, 15, C->color(AZ_COL_TEXT));
        tabs_y0 = -1;
        return;
    }
    draw_list(d, a, 18, 1);
}

/* A setting changed on the UI thread: store it, save it, rescore, re-place the panel. */
static void set_value(int i, int v)
{
    if (i < 0 || i >= S_N) return;
    if (v < 0 || v >= SET[i].n) v = 0;
    atomic_store(&g_set[i], v);
    if (AZ_HAS(C, cfg_set)) C->cfg_set(MOD_ID, SET[i].key, v);
    atomic_store(&settings_changed, 1);
    atomic_store(&want_compute, 1);
}

static int panel_input(const az_event *ev, void *user)
{
    (void)user;
    if (ev->kind == AZ_EV_TAP) {
        if (tabs_y0 >= 0 && ev->y >= tabs_y0 && ev->y < tabs_y0 + tabs_h && tabs_w > 0) {
            int f = (ev->x - tabs_x0) * 3 / tabs_w;     /* a tab: that energy */
            if (f >= 0 && f < 3) set_value(S_FLOW, f);
        } else {
            set_value(S_MODE, S(S_MODE) ? 0 : 1);         /* elsewhere: Next <-> Chain */
        }
        return 1;
    }
    if (ev->kind == AZ_EV_LONG) {                         /* long press: fold / unfold */
        panel_small = !panel_small;
        atomic_store(&settings_changed, 1);
        return 1;
    }
    return 0;
}

static void place_panel(void)
{
    if (!panel) return;
    int where = S(S_PANEL);                           /* 0 right, 1 left, 2 off */
    if (where == 2) {
        C->surface_show(panel, 0);
        return;
    }
    int k = COUNTS[S(S_COUNT) & 3];
    int w = 470, h = panel_small ? 40 : 6 + 22 + 26 + 36 + k * (27 + 21 + 4) + 26;
    if (h > 700) h = 700;
    az_rect r = { where == 1 ? 10 : 1280 - w - 10, 90, w, h };
    C->surface_place(panel, r);
    C->surface_show(panel, 1);
    C->surface_redraw(panel);
}

/* ---- the Mix Pilot page: settings, the suggestions, credits --------------------------- */

enum { IT_SUGGEST = 0, IT_FIRST_SET = 1, IT_RELOAD = IT_FIRST_SET + S_N, IT_N };
static int page_sel, page_top, page_list;       /* UI thread only; page_list = showing the suggestions */
static int page_y0, page_row, page_lines;       /* where page_draw put the rows, for taps */

static void reload_now(void)
{
    atomic_store(&want_reload, 1);
    atomic_store(&community_pending, 1);
    C->banner(MOD_ID, tr(T_BANNER), 1500);
}

static void item_text(int it, char *label, size_t ln, char *value, size_t vn)
{
    value[0] = '\0';
    if (it == IT_SUGGEST) snprintf(label, ln, "%s", tr(T_SHOW));
    else if (it == IT_RELOAD) snprintf(label, ln, "%s", tr(T_RELOAD));
    else {
        int i = it - IT_FIRST_SET;
        snprintf(label, ln, "%s", tr(SET[i].label));
        choice_text(i, S(i), value, vn);
    }
}

static void draw_credits(az_draw *d, az_rect a, int px)
{
    const int lh = px * 3 / 2, pad = px;
    int y = a.y + a.h - 2 * lh - pad / 2;
    C->fill_rect(d, a.x + pad, y - pad / 2, a.w - 2 * pad, 1, C->color(AZ_COL_TEXT_DIM));
    text(d, a.x + pad, y, a.w - 2 * pad, tr(T_LOVE), px, C->color(AZ_COL_TEXT));
    text(d, a.x + pad, y + lh, a.w - 2 * pad, "@blinkevent38    @christopheb_38", px, C->color(AZ_COL_HI));
}

static void page_draw(az_draw *d, az_rect area, void *user)
{
    (void)user;
    int row = C->row_height(d);
    int px = row / 2 > 14 ? row / 2 : 14;
    if (px > 24) px = 24;
    C->draw_header(d, page_list ? tr(T_SUGG_TITLE) : "Mix Pilot");
    az_rect a = { area.x, area.y + row, area.w, area.h - row };
    if (a.h < px * 4) return;
    if (page_list) {
        draw_list(d, a, px, 0);
        return;
    }
    const int small = px * 4 / 5 > 10 ? px * 4 / 5 : 10;
    int credits_h = 2 * (small * 3 / 2) + small * 2;
    int lines = (a.h - credits_h) / row;
    if (lines < 1) lines = 1;
    if (page_sel < page_top) page_top = page_sel;
    if (page_sel >= page_top + lines) page_top = page_sel - lines + 1;
    page_y0 = a.y;
    page_row = row;
    page_lines = lines;
    for (int i = 0; i < lines && page_top + i < IT_N; i++) {
        int it = page_top + i, y = a.y + i * row, on = it == page_sel;
        if (on) C->fill_rect(d, a.x, y, a.w, row, C->color(AZ_COL_HI));
        uint32_t fg = on ? C->color(AZ_COL_HI_TEXT) : C->color(AZ_COL_TEXT);
        char label[48], value[32];
        item_text(it, label, sizeof label, value, sizeof value);
        int ty = y + (row - px * 3 / 2) / 2, vw = value[0] ? C->text_width(value, px) : 0;
        text(d, a.x + px, ty, a.w - 3 * px - vw, label, px, fg);
        if (vw) C->draw_text(d, a.x + a.w - px - vw, ty, value, px, on ? fg : C->color(AZ_COL_HI));
    }
    draw_credits(d, a, small);
}

static void activate(int it, int dir)
{
    if (it == IT_SUGGEST) {
        page_list = 1;
        atomic_store(&want_compute, 1);
    } else if (it == IT_RELOAD) {
        reload_now();
    } else {
        int i = it - IT_FIRST_SET;
        set_value(i, (S(i) + (dir < 0 ? SET[i].n - 1 : 1)) % SET[i].n);
    }
}

static int page_input(const az_event *ev, void *user)
{
    (void)user;
    if (page_list) {
        if (ev->kind == AZ_EV_BACK) {
            page_list = 0;
            C->request_redraw();
            return 1;
        }
        if (ev->kind == AZ_EV_CLICK || ev->kind == AZ_EV_TAP) {   /* Next <-> Chain */
            set_value(S_MODE, S(S_MODE) ? 0 : 1);
            C->request_redraw();
            return 1;
        }
        if (ev->kind == AZ_EV_KNOB) {                     /* the knob changes the energy tab */
            set_value(S_FLOW, (S(S_FLOW) + (ev->dir < 0 ? 2 : 1)) % 3);
            C->request_redraw();
            return 1;
        }
        return 0;
    }
    switch (ev->kind) {
    case AZ_EV_KNOB:
        page_sel += ev->dir < 0 ? -1 : 1;
        if (page_sel < 0) page_sel = 0;
        if (page_sel >= IT_N) page_sel = IT_N - 1;
        C->request_redraw();
        return 1;
    case AZ_EV_CLICK:
        activate(page_sel, 1);
        C->request_redraw();
        return 1;
    case AZ_EV_TAP: {                                      /* a tap changes the row under the finger */
        if (page_row <= 0 || ev->y < page_y0) return 1;
        int i = (ev->y - page_y0) / page_row;
        if (i >= page_lines || page_top + i >= IT_N) return 1;
        page_sel = page_top + i;
        activate(page_sel, 1);
        C->request_redraw();
        return 1;
    }
    default:
        return 0;
    }
}

static void page_enter(void *user)
{
    (void)user;
    page_list = 0;
    atomic_store(&want_compute, 1);
    C->request_redraw();
}

static const az_screen_v1 PAGE = {
    .size = sizeof(az_screen_v1), .on_draw = page_draw, .on_input = page_input, .on_enter = page_enter,
};

/* One row in the Mod Menu: it opens the page above (BACK returns to the row). */
static void register_rows(void)
{
    az_row_v1 r;
    memset(&r, 0, sizeof r);
    r.size = sizeof r;
    r.key = "open";
    r.label = "Mix Pilot";
    r.kind = AZ_ROW_SCREEN;
    r.help = "Next-track suggestions and their settings. Turn the knob, click to change.";
    r.screen = &PAGE;
    if (C->register_row(MOD_ID, &r) < 0) C->log(MOD_ID, "the Mod Menu row was refused");
}

/* ---- lifecycle ------------------------------------------------------------------------- */

static void tick(void)
{
    double t = now_s();
    static double next_poll;
    if (t >= next_poll) {
        next_poll = t + 0.25;
        poll_decks();
    }
    if (atomic_exchange(&settings_changed, 0)) place_panel();
    if (atomic_exchange(&view_fresh, 0)) {
        if (panel) C->surface_redraw(panel);
        C->request_redraw();
    }
}

static int init(const az_core_v1 *core)
{
    if (!core || core->abi != AZ_MOD_ABI) return 1;
    C = core;
    if (!AZ_HAS(C, deck_info) || !AZ_HAS(C, lib_tracks) || !AZ_HAS(C, lib_playlists) ||
        !AZ_HAS(C, thread_start) || !AZ_HAS(C, data_dir) ||
        !AZ_HAS(C, assets_dir) || !AZ_HAS(C, surface_create) || !AZ_HAS(C, banner)) {
        C->log(MOD_ID, "needs AZ-MODS 1.2.3 or newer");
        return 2;
    }
    tzset();
    const char *dd = C->data_dir(MOD_ID), *ad = C->assets_dir(MOD_ID);
    snprintf(g_data_dir, sizeof g_data_dir, "%s", dd ? dd : "");
    snprintf(g_assets_dir, sizeof g_assets_dir, "%s", ad ? ad : "");
    load_settings();
    if (AZ_HAS(C, register_row)) register_rows();
    if (C->register_screen(MOD_ID, &PAGE) != 0) C->log(MOD_ID, "register_screen refused");
    static const az_surface_v1 PANEL = {
        .size = sizeof(az_surface_v1),
        .layer = AZ_LAYER_OVER_EP147,
        .views = AZ_VIEW_BIT(AZ_VIEW_BROWSE) | AZ_VIEW_BIT(AZ_VIEW_SEARCH),
        .on_paint = panel_paint,
        .on_input = panel_input,
    };
    panel = C->surface_create(MOD_ID, &PANEL);
    if (!panel) C->log(MOD_ID, "no panel (surface refused); the Mod Menu page still works");
    if (AZ_HAS(C, on_deck_event)) C->on_deck_event(MOD_ID, on_deck, NULL);
    /* last: threads outlive a refused init */
    if (C->thread_start(MOD_ID, "mp worker", worker, NULL) != 0) {
        C->log(MOD_ID, "thread_start refused");
        return 3;
    }
    if (C->thread_start(MOD_ID, "mp net", net_thread, NULL) != 0)
        C->log(MOD_ID, "no network thread: offline only");
    return 0;
}

static const char *const FW_TAGS[] = { "any", NULL };

static const az_mod_v1 MOD = {
    .abi = AZ_MOD_ABI,
    .size = sizeof(az_mod_v1),
    .id = MOD_ID,
    .fw_tags = FW_TAGS,
    .init = init,
    .tick = tick,
    .on_frame = NULL,
};

AZ_MOD_EXPORT const az_mod_v1 *az_mod_v1_entry(void)
{
    return &MOD;
}
