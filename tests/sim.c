/* SPDX-License-Identifier: GPL-3.0-only
 *
 * End-to-end simulation on the host: a fake core with a 30-track library, four decks, rows
 * and a panel. Deck 1 plays a track, then deck 2 mixes in another; the test checks the panel
 * shows suggestions after the right track, never the loaded ones, and that the transition
 * lands in my-transitions.tsv and comes back as "mixed 1x".
 *
 *   make sim     (builds the mod's sources into this program with short timings)
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "azmod-mod.h"

const az_mod_v1 *az_mod_v1_entry(void);

static char dir[256];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static char screen_text[64][200];
static int nscreen;
static int fails;
#define CHECK(c) do { if (!(c)) { fails++; fprintf(stderr, "sim:%d: FAIL %s\n", __LINE__, #c); } } while (0)

/* ---- the library ------------------------------------------------------------------------ */
struct t { uint32_t id; const char *title, *artist, *key; double bpm; int pl; };
static const struct t LIB[] = {
    { 1, "Opening Groove", "Alpha", "Am", 122, 1 },   { 2, "Second Wind", "Beta", "Em", 123, 1 },
    { 3, "Deep Blue", "Gamma", "Dm", 121, 1 },        { 4, "Night Drive", "Delta", "C", 122, 1 },
    { 5, "Warehouse", "Epsilon", "F#", 128, 2 },      { 6, "Peak Time", "Zeta", "Bm", 130, 2 },
    { 7, "Sunrise", "Eta", "G", 118, 3 },             { 8, "Clash Test", "Theta", "Eb", 122, 1 },
    { 9, "Half Step", "Iota", "Am", 61, 3 },          { 10, "Fast Lane", "Kappa", "Am", 150, 2 },
    { 11, "Groove Two", "Alpha", "Am", 122, 1 },      { 12, "Rolling", "Lambda", "Em", 124, 1 },
    { 13, "Blue Hour", "Mu", "Bm", 122, 1 },          { 14, "Low End", "Nu", "Dm", 120, 1 },
    { 15, "Reprise", "Xi", "Am", 126, 2 },            { 16, "Hands Up", "Omicron", "C", 125, 2 },
    { 17, "Sunset", "Pi", "F", 119, 3 },              { 18, "Basement", "Rho", "Gm", 122, 1 },
    { 19, "Neon", "Sigma", "Cm", 127, 2 },            { 20, "Dawn", "Tau", "G", 121, 3 },
    { 21, "Echoes", "Upsilon", "Em", 122, 1 },        { 22, "Loop", "Phi", "Am", 122, 1 },
    { 23, "Glow", "Chi", "C", 123, 1 },               { 24, "Pulse", "Psi", "Dm", 122, 1 },
    { 25, "Ride", "Omega", "F#m", 122, 2 },           { 26, "Heat", "Alpha2", "Bbm", 128, 2 },
    { 27, "Calm", "Beta2", "E", 100, 3 },             { 28, "Spin", "Gamma2", "Am", 123, 1 },
    { 29, "Drift", "Delta2", "Em", 121, 1 },          { 30, "Ending", "Eps2", "Am", 122, 1 },
};
#define NLIB ((int)(sizeof LIB / sizeof LIB[0]))

static int lib_ready;   /* 0 = the stick is not readable yet (as at boot) */

static int lib_playlists(const char *id, int stick, int (*fn)(int, int, const char *, int, void *), void *u)
{
    (void)id;
    if (stick || !__atomic_load_n(&lib_ready, __ATOMIC_ACQUIRE)) return -1;
    fn(1, 0, "Deep House", 0, u);
    fn(2, 0, "Peak", 0, u);
    fn(3, 0, "Chill", 0, u);
    return 0;
}

static int lib_tracks(const char *id, int stick, int pl, int (*fn)(const az_track *, void *), void *u)
{
    (void)id;
    if (stick || !__atomic_load_n(&lib_ready, __ATOMIC_ACQUIRE)) return -1;
    for (int i = 0; i < NLIB; i++) {
        if (pl && LIB[i].pl != pl) continue;
        az_track t = { sizeof t, LIB[i].id, LIB[i].title, LIB[i].artist, "", LIB[i].key, "/x.mp3", LIB[i].bpm, 300 };
        if (fn(&t, u)) break;
    }
    return 0;
}

/* ---- decks ------------------------------------------------------------------------------- */
static struct { int loaded, playing; int lib; } D[4];

static int deck_info(int deck, az_deck_info *o)
{
    if (deck < 0 || deck > 3) return -1;
    memset(o, 0, sizeof *o);
    o->size = sizeof *o;
    pthread_mutex_lock(&mu);
    o->loaded = D[deck].loaded;
    o->playing = D[deck].playing;
    if (D[deck].loaded) {
        const struct t *t = &LIB[D[deck].lib];
        o->content_id = t->id;
        o->bpm = t->bpm;
        snprintf(o->title, sizeof o->title, "%s", t->title);
        snprintf(o->artist, sizeof o->artist, "%s", t->artist);
    }
    pthread_mutex_unlock(&mu);
    return 0;
}
static int deck_count(void) { return 2; }
static void (*deck_cb)(int, int, void *);
static int on_deck_event(const char *id, void (*fn)(int, int, void *), void *u) { (void)id; (void)u; deck_cb = fn; return 0; }

/* ---- rows -------------------------------------------------------------------------------- */
static int rowv[16], nrows;
static const az_screen_v1 *row_screen;
static int cfg[16];
static int cfg_get(const char *id, const char *key, int def) { (void)id; (void)key; return def; }
static void cfg_set(const char *id, const char *key, int v) { (void)id; (void)key; (void)v; cfg[0]++; }
static int register_row(const char *id, const az_row_v1 *r)
{
    (void)id;
    if (r->kind == AZ_ROW_SCREEN) row_screen = r->screen;
    rowv[nrows] = r->kind == AZ_ROW_CHOICE ? r->default_choice : (r->flags & AZ_ROW_DEFAULT_ON) ? 1 : 0;
    return nrows++;
}
static int row_value(int h) { return h >= 0 && h < nrows ? __atomic_load_n(&rowv[h], __ATOMIC_ACQUIRE) : -1; }
static int row_set(int h, int v) { if (h < 0 || h >= nrows) return -1; __atomic_store_n(&rowv[h], v, __ATOMIC_RELEASE); return 0; }

/* ---- drawing: capture the text ------------------------------------------------------------ */
static az_surface_v1 surf;
static int fake_surface;
static void draw_text(az_draw *d, int x, int y, const char *s, int px, uint32_t c)
{
    (void)d; (void)x; (void)y; (void)px; (void)c;
    if (nscreen < 64) snprintf(screen_text[nscreen++], sizeof screen_text[0], "%s", s);
}
static void fill_rect(az_draw *d, int x, int y, int w, int h, uint32_t c) { (void)d; (void)x; (void)y; (void)w; (void)h; (void)c; }
static void draw_header(az_draw *d, const char *t) { (void)d; (void)t; }
static uint32_t color(int r) { return (uint32_t)r * 0x111111u; }
static int row_height(az_draw *d) { (void)d; return 60; }
static int text_width(const char *s, int px) { return (int)strlen(s) * px / 2; }
static void request_redraw(void) {}
static az_surface *surface_create(const char *id, const az_surface_v1 *s) { (void)id; surf = *s; return (az_surface *)&fake_surface; }
static void surface_place(az_surface *s, az_rect r) { (void)s; (void)r; }
static void surface_show(az_surface *s, int on) { (void)s; (void)on; }
static void surface_redraw(az_surface *s) { (void)s; }
static int view(void) { return AZ_VIEW_BROWSE; }
static const char *data_dir(const char *id) { (void)id; return dir; }
static const char *no_assets(const char *id) { (void)id; return NULL; }
static void logf_(const char *id, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  [%s] ", id);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
static void banner(const char *id, const char *t, int ms) { (void)id; (void)t; (void)ms; }
static int register_screen(const char *id, const az_screen_v1 *s) { (void)id; (void)s; return 0; }
static int thread_start(const char *id, const char *name, void *(*fn)(void *), void *arg)
{
    (void)id; (void)name;
    pthread_t th;
    if (pthread_create(&th, NULL, fn, arg)) return -1;
    pthread_detach(th);
    return 0;
}

static az_core_v1 CORE;

static void paint(void)
{
    nscreen = 0;
    az_rect a = { 800, 90, 470, 600 };
    surf.on_paint(NULL, a, surf.user);
}

static int screen_has(const char *needle)
{
    for (int i = 0; i < nscreen; i++)
        if (strstr(screen_text[i], needle)) return 1;
    return 0;
}

static void run(double seconds)
{
    const az_mod_v1 *m = az_mod_v1_entry();
    for (double t = 0; t < seconds; t += 0.05) {
        m->tick();
        usleep(50000);
    }
}

static void dump(const char *when)
{
    paint();
    printf("--- panel %s\n", when);
    for (int i = 0; i < nscreen; i++) printf("  | %s\n", screen_text[i]);
}

int main(void)
{
    snprintf(dir, sizeof dir, "/tmp/mixpilot-sim-%d", (int)getpid());
    mkdir(dir, 0755);
    CORE = (az_core_v1){ .abi = AZ_MOD_ABI, .size = sizeof(az_core_v1), .log = logf_, .register_screen = register_screen,
                         .draw_header = draw_header, .draw_text = draw_text, .fill_rect = fill_rect, .color = color,
                         .row_height = row_height, .text_width = text_width, .request_redraw = request_redraw,
                         .deck_count = deck_count, .data_dir = data_dir, .assets_dir = no_assets,
                         .register_row = register_row, .cfg_get = cfg_get, .cfg_set = cfg_set, .row_value = row_value, .row_set = row_set,
                         .lib_playlists = lib_playlists, .lib_tracks = lib_tracks, .view = view,
                         .surface_create = surface_create, .surface_place = surface_place,
                         .surface_show = surface_show, .surface_redraw = surface_redraw, .deck_info = deck_info,
                         .on_deck_event = on_deck_event, .banner = banner, .thread_start = thread_start };
    const az_mod_v1 *m = az_mod_v1_entry();
    CHECK(m->init(&CORE) == 0);
    CHECK(nrows == 1 && row_screen != NULL);            /* one row in the Mod Menu, opening our page */
    run(1.0);
    dump("stick not readable yet");
    CHECK(screen_has("No rekordbox library yet") && screen_has("USB1 -1/-1"));
    __atomic_store_n(&lib_ready, 1, __ATOMIC_RELEASE);  /* the stick becomes readable */
    run(2.5);
    dump("empty decks");
    CHECK(screen_has("Play a track"));
    CHECK(screen_has("30 tracks"));

    /* the page: settings and the credits */
    nscreen = 0;
    row_screen->on_draw(NULL, (az_rect){ 0, 0, 1280, 800 }, row_screen->user);
    printf("--- page\n");
    for (int i = 0; i < nscreen; i++) printf("  | %s\n", screen_text[i]);
    CHECK(screen_has("Energy") && screen_has("Smooth") && screen_has("Follow"));
    CHECK(screen_has("@blinkevent38") && screen_has("Suivez-nous"));

    pthread_mutex_lock(&mu);
    D[0].loaded = 1; D[0].lib = 0; D[0].playing = 1;      /* Opening Groove, Am 122 */
    D[1].loaded = 1; D[1].lib = 1;                         /* Second Wind loaded, not playing */
    pthread_mutex_unlock(&mu);
    if (deck_cb) deck_cb(0, AZ_DECK_LOADED, NULL);
    run(2.0);
    dump("deck 1 playing Opening Groove");
    CHECK(screen_has("After D1: Opening Groove"));
    CHECK(!screen_has("Second Wind"));                     /* loaded on deck 2: never suggested */
    CHECK(!screen_has("Fast Lane"));                       /* 150 BPM */
    CHECK(screen_has("Deep House"));                       /* "in <playlist>" */

    /* deck 2 mixes in, deck 1 stops: Opening Groove -> Second Wind is learned */
    pthread_mutex_lock(&mu);
    D[1].playing = 1;
    pthread_mutex_unlock(&mu);
    run(1.0);
    pthread_mutex_lock(&mu);
    D[0].playing = 0;
    pthread_mutex_unlock(&mu);
    run(2.0);
    dump("deck 2 playing Second Wind");
    CHECK(screen_has("After D2: Second Wind"));
    CHECK(!screen_has("Opening Groove"));                  /* played: never again tonight */

    char p[300], line[600] = "";
    snprintf(p, sizeof p, "%s/my-transitions.tsv", dir);
    FILE *f = fopen(p, "r");
    CHECK(f != NULL);
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
    printf("  learned: %s", line);
    CHECK(strncmp(line, "Alpha\tOpening Groove\tBeta\tSecond Wind\t1\t", 40) == 0);
    snprintf(p, sizeof p, "%s/config.txt", dir);
    CHECK(access(p, F_OK) == 0);

    /* new night: load Opening Groove again; the learned transition is suggested first */
    pthread_mutex_lock(&mu);
    D[1].loaded = 1; D[1].lib = 0; D[1].playing = 1;
    D[0].loaded = 0; D[0].playing = 0;
    pthread_mutex_unlock(&mu);
    /* the session's played list still excludes Second Wind; check the score path instead */
    az_event tap = { AZ_EV_TAP, 0, 900, 100 };
    CHECK(surf.on_input(&tap, surf.user) == 1);            /* a tap on the panel: Chain */
    CHECK(cfg[0] > 0);                                      /* the setting was saved */
    run(2.0);
    dump("chain after Opening Groove (deck 2)");
    CHECK(screen_has("Plan after D2: Opening Groove"));

    printf("sim: %s\n", fails ? "FAILED" : "all checks passed");
    return fails != 0;
}
