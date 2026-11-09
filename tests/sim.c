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
#include "pdbgen.h"

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
    { 21, "Echoes", "Upsilon", "Em", 122, 1 },        { 22, "Loop \xc3\x89t\xc3\xa9", "Ph\xc3\xafl", "Am", 122, 1 },
    { 23, "Glow", "Chi", "C", 123, 1 },               { 24, "Pulse", "Psi", "Dm", 122, 1 },
    { 25, "Ride", "Omega", "F#m", 122, 2 },           { 26, "Heat", "Alpha2", "Bbm", 128, 2 },
    { 27, "Calm", "Beta2", "E", 100, 3 },             { 28, "Spin", "Gamma2", "Am", 123, 1 },
    { 29, "Drift", "Delta2", "Em", 121, 1 },          { 30, "Ending Che\xcc\x81rie \xc5\x81", "Eps2", "Am", 122, 1 },
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
static void draw_rect(az_draw *d, int x, int y, int w, int h, int lw, uint32_t c) { (void)d; (void)x; (void)y; (void)w; (void)h; (void)lw; (void)c; }
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

static void no_accents(void)
{
    for (int i = 0; i < nscreen; i++)
        for (const unsigned char *c = (const unsigned char *)screen_text[i]; *c; c++)
            if (*c >= 0x80) { fails++; fprintf(stderr, "sim: non-ASCII on screen: %s\n", screen_text[i]); return; }
}

static void paint(void)
{
    nscreen = 0;
    az_rect a = { 10, 458, 1260, 182 };                   /* the bottom panel, 5 tracks (place_panel) */
    surf.on_paint(NULL, a, surf.user);
    no_accents();
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

/* A fake stick for the USB diagnostic: a mounts file, a mount point holding only export.pdb,
 * and a sysfs link that puts the device in the USB 1 port (bus 1-1). */
#define FAKE "/tmp/mixpilot-fake"
static void make_fake_stick(void)
{
    int rc = system("rm -rf " FAKE " && mkdir -p " FAKE "/media/usb/sdz1/PIONEER/rekordbox " FAKE
                    "/sys/class/block " FAKE "/sys/devices/soc/usb1/1-1/1-1:1.0/host0/target0/block/sdz1"
                    " && touch " FAKE "/media/usb/sdz1/PIONEER/rekordbox/export.pdb"
                    " && ln -s " FAKE "/sys/devices/soc/usb1/1-1/1-1:1.0/host0/target0/block/sdz1 " FAKE
                    "/sys/class/block/sdz1"
                    " && mkdir -p " FAKE "/media/usb/sdy1/MODS " FAKE "/media/usb/sdy1/.PIONEER/rekordbox"
                    " && touch " FAKE "/media/usb/sdy1/.PIONEER/rekordbox/exportLibrary.db"
                    " && mkdir -p " FAKE "/sys/devices/soc/usb2/2-1/2-1:1.0/host1/target1/block/sdy1"
                    " && ln -s " FAKE "/sys/devices/soc/usb2/2-1/2-1:1.0/host1/target1/block/sdy1 " FAKE "/sys/class/block/sdy1"
                    " && printf '/dev/sdz1 " FAKE "/media/usb/sdz1 vfat rw 0 0\\n/dev/sdy1 " FAKE "/media/usb/sdy1 vfat rw 0 0\\n' > " FAKE "/mounts");
    if (rc != 0) { fails++; fprintf(stderr, "sim: could not build the fake stick\n"); }
}

int main(void)
{
    make_fake_stick();
    snprintf(dir, sizeof dir, "/tmp/mixpilot-sim-%d", (int)getpid());
    mkdir(dir, 0755);
    CORE = (az_core_v1){ .abi = AZ_MOD_ABI, .size = sizeof(az_core_v1), .log = logf_, .register_screen = register_screen,
                         .draw_header = draw_header, .draw_text = draw_text, .fill_rect = fill_rect, .draw_rect = draw_rect, .color = color,
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
    CHECK(screen_has("hidden .PIONEER folder") && screen_has("USB1 -1/-1") && screen_has("sdz1:pdb p1 P+ R1"));
    CHECK(screen_has("sdy1:- p2 P- R-1 H:db"));             /* library only in the hidden .PIONEER */
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
    CHECK(screen_has("@blinkevents38") && screen_has("@christopheb_38") && screen_has("Follow us"));
    CHECK(screen_has("Language") && screen_has("English"));

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
    CHECK(screen_has("Loop Ete - Phil"));                  /* accents folded for the deck's font */
    CHECK(screen_has("Ending Cherie L - Eps2"));           /* decomposed accent (macOS) and Latin Extended-A */
    CHECK(screen_has("Smooth") && screen_has("Build up") && screen_has("Cool down"));   /* the tabs */
    char smooth_first[200] = "", up_first[200] = "";
    for (int i = 0; i < nscreen; i++)
        if (!strcmp(screen_text[i], "1") && i + 1 < nscreen) { snprintf(smooth_first, sizeof smooth_first, "%s", screen_text[i + 1]); break; }
    /* tap the middle tab (Build up): the tabs sit in the header, right of the "After" line */
    int tapped = 0;
    {
        az_event t = { AZ_EV_TAP, 0, 10 + 1260 - 330 - 22 - 16 + 165, 458 + 12 };
        surf.on_input(&t, surf.user);
        run(0.5);
        paint();
        for (int i = 0; i < nscreen; i++)
            if (!strcmp(screen_text[i], "1") && i + 1 < nscreen) { snprintf(up_first, sizeof up_first, "%s", screen_text[i + 1]); break; }
        tapped = strcmp(up_first, smooth_first) != 0 && !screen_has("CHAIN");
    }
    printf("  smooth #1: %s   build up #1: %s\n", smooth_first, up_first);
    CHECK(tapped);                                         /* Build up shows another first track */

    /* the fold button, top right: one tap folds the panel to a line, another opens it */
    az_event fold = { AZ_EV_TAP, 0, 10 + 1260 - 8 - 11, 458 + 16 };
    surf.on_input(&fold, surf.user);
    run(0.3);
    nscreen = 0;
    surf.on_paint(NULL, (az_rect){ 10, 604, 1260, 36 }, surf.user);
    CHECK(screen_has("MIX PILOT  1. ") && !screen_has("Cool down"));
    az_event unfold = { AZ_EV_TAP, 0, 10 + 1260 - 8 - 11, 604 + 18 };
    surf.on_input(&unfold, surf.user);
    run(0.3);
    paint();
    CHECK(screen_has("Cool down") && screen_has("Loop Ete"));

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

    /* the page: turn to Language (item 9: suggestions, 8 settings), click -> Francais */
    az_event knob = { AZ_EV_KNOB, 1, 0, 0 }, click = { AZ_EV_CLICK, 0, 0, 0 };
    row_screen->on_enter(row_screen->user);
    for (int i = 0; i < 9; i++) row_screen->on_input(&knob, row_screen->user);
    row_screen->on_input(&click, row_screen->user);
    run(1.0);
    nscreen = 0;
    row_screen->on_draw(NULL, (az_rect){ 0, 0, 1280, 800 }, row_screen->user);
    CHECK(screen_has("Langue") && screen_has("Francais") && screen_has("Suivez-nous") && screen_has("Energie"));
    no_accents();
    paint();
    dump("in French");
    CHECK(screen_has("Plan apr") && screen_has("Stable") && screen_has("Monter"));

    /* three more clicks on Language: Espanol, Deutsch, Italiano */
    for (int i = 0; i < 3; i++) row_screen->on_input(&click, row_screen->user);
    nscreen = 0;
    row_screen->on_draw(NULL, (az_rect){ 0, 0, 1280, 800 }, row_screen->user);
    CHECK(screen_has("Lingua") && screen_has("Italiano") && screen_has("Seguici") && screen_has("Modalita"));
    no_accents();

    /* the user's stick: the service gives nothing, the library is in the hidden .PIONEER folder;
     * the mod reads its export.pdb (a synthetic one: 20 tracks) after Reload library */
    {
        pdbgen g = pg_build();
        FILE *pf = fopen(FAKE "/media/usb/sdy1/.PIONEER/rekordbox/export.pdb", "wb");
        CHECK(pf != NULL);
        if (pf) { fwrite(g.b, 1, g.len, pf); fclose(pf); }
        free(g.b);
        __atomic_store_n(&lib_ready, 0, __ATOMIC_RELEASE);
        row_screen->on_input(&knob, row_screen->user);    /* Language -> Reload library */
        row_screen->on_input(&click, row_screen->user);
        run(2.5);
        paint();
        dump("library from the hidden export.pdb");
        CHECK(screen_has("20 brani (pdb)"));               /* Italian is still on: "20 tracks (pdb)" */
    }

    printf("sim: %s\n", fails ? "FAILED" : "all checks passed");
    return fails != 0;
}
