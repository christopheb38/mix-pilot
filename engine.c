/* SPDX-License-Identifier: GPL-3.0-only
 *
 * engine.c: Mix Pilot's recommendation engine (see engine.h). No I/O, no deck services.
 */
#include "engine.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NT_MAX_PL 4096

/* ---- keys ---------------------------------------------------------------------------- */

static int pitch_class(const char **ps)
{
    static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 };   /* A B C D E F G */
    const char *s = *ps;
    int c = toupper((unsigned char)*s);
    if (c < 'A' || c > 'G') return -1;
    int p = base[c - 'A'];
    s++;
    if (*s == '#') { p++; s++; }
    else if (*s == 'b') { p--; s++; }
    else if ((unsigned char)s[0] == 0xE2 && (unsigned char)s[1] == 0x99) {   /* U+266F sharp, U+266D flat */
        if ((unsigned char)s[2] == 0xAF) { p++; s += 3; }
        else if ((unsigned char)s[2] == 0xAD) { p--; s += 3; }
    }
    *ps = s;
    return (p + 12) % 12;
}

int nt_camelot(const char *key)
{
    if (!key) return -1;
    while (*key == ' ') key++;
    if (!*key) return -1;
    if (isdigit((unsigned char)*key)) {                  /* Camelot "8A", "08B" */
        int num = 0;
        while (isdigit((unsigned char)*key)) num = num * 10 + (*key++ - '0');
        int l = toupper((unsigned char)*key);
        if (num < 1 || num > 12 || (l != 'A' && l != 'B')) return -1;
        return num * 2 + (l == 'B');
    }
    const char *s = key;
    int p = pitch_class(&s);
    if (p < 0) return -1;
    while (*s == ' ') s++;
    int minor = 0;
    if (*s == 'm' && strncmp(s, "maj", 3) != 0) minor = 1;
    if (strncmp(s, "min", 3) == 0 || strncmp(s, "Min", 3) == 0) minor = 1;
    int major_pc = minor ? (p + 3) % 12 : p;             /* the relative major */
    int num = (7 * major_pc + 7) % 12 + 1;               /* C = 8, G = 9, F = 7 ... */
    return num * 2 + (minor ? 0 : 1);
}

void nt_camelot_str(int code, char *buf)
{
    if (code < 2) { buf[0] = '\0'; return; }
    snprintf(buf, 4, "%d%c", code / 2, (code & 1) ? 'B' : 'A');
}

float nt_key_score(int a, int b, int *step)
{
    if (step) *step = 99;
    if (a < 2 || b < 2) return 0.5f;
    int na = a / 2, nb = b / 2, la = a & 1, lb = b & 1;
    int d = ((nb - na) % 12 + 12) % 12;                  /* 0..11 */
    if (d > 6) d -= 12;                                   /* -5..6 */
    if (la == lb) {
        if (step) *step = d;
        switch (d) {
        case 0: return 1.0f;
        case 1: case -1: return 0.9f;
        case 2: return 0.55f;                             /* energy boost */
        case -2: return 0.5f;
        case -5: return 0.5f;                             /* +7: one semitone up */
        case 5: return 0.4f;                              /* one semitone down */
        default: return 0.1f;
        }
    }
    if (d == 0) return 0.85f;                             /* relative major / minor */
    if ((la == 0 && d == 1) || (la == 1 && d == -1)) return 0.6f;   /* diagonal */
    if (d == 3 && la == 0) return 0.4f;                   /* minor to its parallel major area */
    return 0.1f;
}

float nt_bpm_score(double from, double to, double *pct, int *halfdouble)
{
    if (pct) *pct = 0;
    if (halfdouble) *halfdouble = 0;
    if (from <= 0 || to <= 0) return 0.45f;
    double best = 1e9, bestr = 1;
    int hd = 0;
    const double mult[3] = { 1.0, 2.0, 0.5 };
    for (int i = 0; i < 3; i++) {
        double r = from / (to * mult[i]);                 /* pitch factor to apply to `to` */
        double d = fabs(r - 1.0);
        if (d < best - 1e-9) { best = d; bestr = r; hd = i != 0; }
    }
    double p = (bestr - 1.0) * 100.0;
    if (pct) *pct = p;
    if (halfdouble) *halfdouble = hd;
    double s = exp(-(p / 4.0) * (p / 4.0));               /* 2% 0.78, 4% 0.37, 6% 0.11 */
    if (hd) s *= 0.75;
    return (float)s;
}

/* ---- identity ------------------------------------------------------------------------ */

/* One UTF-8 Latin-1 letter (C3 xx) folded to ASCII, or 0. */
static char fold_c3(unsigned char b)
{
    static const char map[65] = "aaaaaaaceeeeiiiidnooooo*ouuuuyts"
                                "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
    char c = map[b & 0x3F];
    return (c == '*' || c == '/') ? 0 : c;
}

static void fold(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (const unsigned char *s = (const unsigned char *)in; *s && o + 1 < n; s++) {
        if (*s < 0x80) out[o++] = (char)tolower(*s);
        else if (*s == 0xC3 && s[1]) {
            char c = fold_c3(s[1]);
            s++;
            if (c) out[o++] = c;
        } else if (*s >= 0xC0) {                          /* other multibyte: skip it whole */
            while ((s[1] & 0xC0) == 0x80) s++;
            out[o++] = ' ';
        }
    }
    out[o] = '\0';
}

static int has_word(const char *s, const char *w)
{
    return strstr(s, w) != NULL;
}

/* Removes "(Original Mix)", "[Extended]", "(feat. X)" and similar version noise. */
static void drop_noise(char *s)
{
    static const char *const noise[] = { "original", "extended", "radio", "club mix", "main mix",
                                         "clean", "explicit", "dirty", "remaster", "feat", "ft.",
                                         "ft ", "featuring", "with ", "album version", "intro",
                                         "outro", "edit)", NULL };
    for (char *o = s; (o = strpbrk(o, "([")) != NULL;) {
        char close = *o == '(' ? ')' : ']';
        char *e = strchr(o, close);
        if (!e) { *o = '\0'; break; }
        char save = e[1];
        e[1] = '\0';
        int drop = 0;
        for (int i = 0; noise[i] && !drop; i++)
            if (has_word(o, noise[i])) drop = 1;
        /* keep named remixes and edits: "(Fisher Remix)" is another track */
        if (drop && (has_word(o, "remix") || has_word(o, " rework") || has_word(o, " vip")) &&
            !has_word(o, "feat") && !has_word(o, "ft")) drop = 0;
        e[1] = save;
        if (drop) memmove(o, e + 1, strlen(e + 1) + 1);
        else o = e + 1;
    }
    static const char *const cut[] = { " feat. ", " feat ", " ft. ", " ft ", " featuring ", NULL };
    for (int i = 0; cut[i]; i++) {
        char *f = strstr(s, cut[i]);
        if (f) {
            char *rest = strpbrk(f, "([");               /* keep a remix name after the guest */
            if (rest) memmove(f + 1, rest, strlen(rest) + 1);
            else *f = '\0';
        }
    }
}

static void squash(const char *in, char *out, size_t n)
{
    size_t o = 0;
    int sp = 1;
    for (const char *s = in; *s && o + 1 < n; s++) {
        char c = *s;
        if (isalnum((unsigned char)c)) { out[o++] = c; sp = 0; }
        else if (!sp) { out[o++] = ' '; sp = 1; }
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = '\0';
}

static void norm(const char *in, char *out, size_t n, int artist)
{
    char a[256], b[256];
    fold(in ? in : "", a, sizeof a);
    if (artist) {                                         /* the first artist only */
        static const char *const sep[] = { ",", " & ", " x ", " vs", " feat", " ft.", " ft ",
                                           " featuring ", ";", " / ", " and ", NULL };
        for (int i = 0; sep[i]; i++) {
            char *f = strstr(a, sep[i]);
            if (f && f != a) *f = '\0';
        }
    } else {
        drop_noise(a);
    }
    squash(a, b, sizeof b);
    snprintf(out, n, "%s", b);
}

void nt_normalize(const char *in, char *out, size_t n)
{
    norm(in, out, n, 0);
}

static uint32_t fnv(uint32_t h, const char *s)
{
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

uint32_t nt_ident(const char *artist, const char *title)
{
    char a[160], t[200];
    norm(artist, a, sizeof a, 1);
    norm(title, t, sizeof t, 0);
    uint32_t h = fnv(2166136261u, a);
    h = fnv(h ^ '|', "|");
    h = fnv(h, t);
    return h ? h : 1;
}

/* ---- pair maps ----------------------------------------------------------------------- */

static uint64_t pkey(uint32_t a, uint32_t b)
{
    uint64_t k = ((uint64_t)a << 32) | b;
    return k ? k : 1;
}

static uint32_t phash(uint64_t k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    return (uint32_t)k;
}

static int pair_grow(nt_pairmap *m)
{
    uint32_t cap = m->cap ? m->cap * 2 : 1024;
    uint64_t *k = calloc(cap, sizeof *k);
    float *v = calloc(cap, sizeof *v);
    if (!k || !v) { free(k); free(v); return -1; }
    for (uint32_t i = 0; i < m->cap; i++) {
        if (!m->keys[i]) continue;
        uint32_t j = phash(m->keys[i]) & (cap - 1);
        while (k[j]) j = (j + 1) & (cap - 1);
        k[j] = m->keys[i];
        v[j] = m->vals[i];
    }
    free(m->keys);
    free(m->vals);
    m->keys = k;
    m->vals = v;
    m->cap = cap;
    return 0;
}

static float *pair_slot(nt_pairmap *m, uint32_t a, uint32_t b, int create)
{
    if (!m->cap) {
        if (!create || pair_grow(m)) return NULL;
    }
    uint64_t k = pkey(a, b);
    uint32_t j = phash(k) & (m->cap - 1);
    while (m->keys[j]) {
        if (m->keys[j] == k) return &m->vals[j];
        j = (j + 1) & (m->cap - 1);
    }
    if (!create) return NULL;
    if ((m->n + 1) * 10 > m->cap * 7) {
        if (pair_grow(m)) return NULL;
        return pair_slot(m, a, b, 1);
    }
    m->keys[j] = k;
    m->vals[j] = 0;
    m->n++;
    return &m->vals[j];
}

int nt_pair_add(nt_pairmap *m, uint32_t a, uint32_t b, float v)
{
    float *p = pair_slot(m, a, b, 1);
    if (!p) return -1;
    *p += v;
    return 0;
}

int nt_pair_set(nt_pairmap *m, uint32_t a, uint32_t b, float v)
{
    float *p = pair_slot(m, a, b, 1);
    if (!p) return -1;
    if (v > *p) *p = v;
    return 0;
}

float nt_pair_get(const nt_pairmap *m, uint32_t a, uint32_t b)
{
    float *p = pair_slot((nt_pairmap *)m, a, b, 0);
    return p ? *p : 0.0f;
}

void nt_pair_free(nt_pairmap *m)
{
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof *m);
}

/* ---- the library --------------------------------------------------------------------- */

typedef struct { uint32_t size[NT_MAX_PL]; } plsizes;

static plsizes *sizes_of(const nt_lib *L)
{
    /* The playlist sizes are stored at the start of the pool allocation's header. */
    return (plsizes *)(void *)(L->pool - sizeof(plsizes));
}

int nt_lib_init(nt_lib *L, int cap)
{
    memset(L, 0, sizeof *L);
    if (cap < 1) cap = 1;
    if (cap > NT_MAX_TRACKS) cap = NT_MAX_TRACKS;
    L->t = calloc((size_t)cap, sizeof *L->t);
    L->pool_cap = 64 * 1024;
    char *raw = calloc(1, sizeof(plsizes) + L->pool_cap);
    if (!L->t || !raw) { free(L->t); free(raw); memset(L, 0, sizeof *L); return -1; }
    L->pool = raw + sizeof(plsizes);
    L->pool_n = 1;                                        /* offset 0 = "" */
    L->cap = cap;
    return 0;
}

void nt_lib_free(nt_lib *L)
{
    free(L->t);
    if (L->pool) free(L->pool - sizeof(plsizes));
    nt_pair_free(&L->pairs);
    nt_pair_free(&L->sim);
    nt_pair_free(&L->byid);
    memset(L, 0, sizeof *L);
}

static uint32_t intern(nt_lib *L, const char *s)
{
    if (!s || !*s) return 0;
    size_t len = strlen(s);
    if (len > 200) len = 200;
    if (L->pool_n + len + 1 > L->pool_cap) {
        uint32_t cap = L->pool_cap * 2;
        while (L->pool_n + len + 1 > cap) cap *= 2;
        char *raw = realloc(L->pool - sizeof(plsizes), sizeof(plsizes) + cap);
        if (!raw) return 0;
        L->pool = raw + sizeof(plsizes);
        L->pool_cap = cap;
    }
    uint32_t off = L->pool_n;
    memcpy(L->pool + off, s, len);
    L->pool[off + len] = '\0';
    L->pool_n += (uint32_t)len + 1;
    return off;
}

const char *nt_str(const nt_lib *L, uint32_t off)
{
    return (L->pool && off < L->pool_n) ? L->pool + off : "";
}

int nt_find(const nt_lib *L, int stick, uint32_t id, uint32_t ident)
{
    if (id) {
        for (int s = 0; s < 2; s++) {
            if (stick >= 0 && s != stick) continue;
            float v = nt_pair_get(&L->byid, (uint32_t)s + 1, id);
            if (v > 0) return (int)v - 1;
        }
    }
    if (ident) {
        for (int i = 0; i < L->n; i++)
            if (L->t[i].ident == ident) return i;
    }
    return -1;
}

int nt_lib_add(nt_lib *L, int stick, uint32_t id, const char *title, const char *artist,
               const char *album, const char *key, double bpm, double len_s)
{
    if (stick < 0 || stick > 1) return -1;
    if (id) {
        int i = nt_find(L, stick, id, 0);
        if (i >= 0) return i;
    }
    if (L->n >= L->cap) return -1;
    nt_track *t = &L->t[L->n];
    memset(t, 0, sizeof *t);
    t->id = id;
    t->stick = (int8_t)stick;
    t->camelot = (int8_t)nt_camelot(key);
    t->bpm = (float)(bpm > 0 && bpm < 400 ? bpm : 0);
    t->len_s = (float)(len_s > 0 ? len_s : 0);
    t->title = intern(L, title);
    t->artist = intern(L, artist);
    t->album = intern(L, album);
    t->ident = nt_ident(artist, title);
    t->aident = nt_ident(artist, "");
    if (id && nt_pair_set(&L->byid, (uint32_t)stick + 1, id, (float)(L->n + 1))) return -1;
    return L->n++;
}

void nt_lib_in_playlist(nt_lib *L, int i, int pl, const char *name)
{
    if (i < 0 || i >= L->n || pl < 0 || pl >= NT_MAX_PL) return;
    nt_track *t = &L->t[i];
    for (int j = 0; j < t->npl; j++)
        if (t->pl[j] == pl) return;
    if (pl + 1 > L->npl) L->npl = pl + 1;
    sizes_of(L)->size[pl]++;
    if (!t->where) t->where = intern(L, name);
    if (t->npl < NT_TRACK_PL) t->pl[t->npl++] = (uint16_t)pl;
}

int nt_tag_index(nt_lib *L, const char *tag)
{
    char low[24];
    size_t o = 0;
    for (const char *s = tag; *s && o + 1 < sizeof low; s++) low[o++] = (char)tolower((unsigned char)*s);
    low[o] = '\0';
    if (!low[0]) return -1;
    for (int i = 0; i < L->ntags; i++)
        if (strcmp(L->tags[i], low) == 0) return i;
    if (L->ntags >= NT_MAX_TAGS) return -1;
    memcpy(L->tags[L->ntags], low, sizeof low);
    return L->ntags++;
}

int nt_curation(const nt_track *t)
{
    return t->npl * 8 + t->rating * 2 + (t->color ? 1 : 0);
}

void nt_set_genre(nt_lib *L, int i, const char *genre)
{
    if (i < 0 || i >= L->n || !genre || !genre[0]) return;
    int ix = nt_tag_index(L, genre);
    if (ix >= 0) L->t[i].genre = (uint16_t)(ix + 1);
}

void nt_set_tags(nt_lib *L, uint32_t ident, const int *tag, const int *w, int n)
{
    if (n > NT_TRACK_TAGS) n = NT_TRACK_TAGS;
    for (int i = 0; i < L->n; i++) {
        nt_track *t = &L->t[i];
        if (t->ident != ident) continue;
        int k = 0;
        for (int j = 0; j < n; j++) {
            if (tag[j] < 0 || tag[j] >= L->ntags) continue;
            t->tag[k] = (uint16_t)tag[j];
            t->tagw[k] = (uint8_t)(w[j] < 1 ? 1 : w[j] > 100 ? 100 : w[j]);
            k++;
        }
        t->ntags = (uint8_t)k;
    }
}

/* ---- scoring ------------------------------------------------------------------------- */

/* A track's style vector: its web tags plus its rekordbox genre (weight 100) when that genre
 * is not already one of the tags. Returns the count (at most NT_TRACK_TAGS + 1). */
static int style_vec(const nt_track *t, int *tag, float *w)
{
    int n = 0, has_genre = 0;
    for (int i = 0; i < t->ntags; i++) {
        tag[n] = t->tag[i];
        w[n++] = t->tagw[i];
        if (t->genre && t->tag[i] == t->genre - 1) has_genre = 1;
    }
    if (t->genre && !has_genre) {
        tag[n] = t->genre - 1;
        w[n++] = 100;
    }
    return n;
}

static float tag_cosine(const nt_track *a, const nt_track *b, int *best)
{
    int ta[NT_TRACK_TAGS + 1], tb[NT_TRACK_TAGS + 1];
    float wa[NT_TRACK_TAGS + 1], wb[NT_TRACK_TAGS + 1], dot = 0, na = 0, nb = 0, top = 0;
    int ka = style_vec(a, ta, wa), kb = style_vec(b, tb, wb);
    *best = -1;
    for (int i = 0; i < ka; i++) na += wa[i] * wa[i];
    for (int j = 0; j < kb; j++) nb += wb[j] * wb[j];
    for (int i = 0; i < ka; i++)
        for (int j = 0; j < kb; j++)
            if (ta[i] == tb[j]) {
                float p = wa[i] * wb[j];
                dot += p;
                if (p > top) { top = p; *best = ta[i]; }
            }
    if (na <= 0 || nb <= 0) return 0;
    return dot / sqrtf(na * nb);
}

/* Playlists shared, weighted so that a huge "All tracks" playlist says little. */
static float playlist_overlap(const nt_lib *L, const nt_track *a, const nt_track *b)
{
    if (!a->npl || !b->npl) return -1;
    const plsizes *ps = sizes_of(L);
    float shared = 0, uni = 0;
    for (int i = 0; i < a->npl; i++) {
        float w = 1.0f / log2f(2.0f + (float)ps->size[a->pl[i]]);
        uni += w;
        for (int j = 0; j < b->npl; j++)
            if (a->pl[i] == b->pl[j]) { shared += w; break; }
    }
    for (int j = 0; j < b->npl; j++) {
        int in = 0;
        for (int i = 0; i < a->npl; i++)
            if (a->pl[i] == b->pl[j]) { in = 1; break; }
        if (!in) uni += 1.0f / log2f(2.0f + (float)ps->size[b->pl[j]]);
    }
    return uni > 0 ? shared / uni : 0;
}

float nt_score(const nt_lib *L, const nt_ctx *c, int from, double from_bpm, int to, nt_reason *r)
{
    nt_reason tmp;
    if (!r) r = &tmp;
    memset(r, 0, sizeof *r);
    r->shared_tag = -1;
    r->step = 99;
    if (from < 0 || to < 0 || from >= L->n || to >= L->n || from == to) return 0;
    const nt_track *a = &L->t[from], *b = &L->t[to];
    if (a->ident == b->ident) return 0;

    double tempo = from_bpm > 0 ? from_bpm : a->bpm;
    r->bpm = nt_bpm_score(tempo, b->bpm, &r->pct, &r->halfdouble);
    float pc = nt_pair_get(&L->pairs, a->ident, b->ident);
    r->pair_count = (int)(pc + 0.5f);
    r->pair = pc > 0 ? 1.0f - expf(-pc / 1.5f) : 0.0f;
    r->sim = nt_pair_get(&L->sim, a->ident, b->ident);
    if (r->bpm < 0.02f && r->pair <= 0 && r->sim <= 0) return 0;   /* not mixable: cheap exit */

    r->key = nt_key_score(a->camelot, b->camelot, &r->step);

    float cos = ((a->ntags || a->genre) && (b->ntags || b->genre)) ? tag_cosine(a, b, &r->shared_tag) : -1;
    float pl = playlist_overlap(L, a, b);
    if (cos >= 0 && pl >= 0) r->style = 0.65f * cos + 0.35f * fminf(1.0f, pl * 1.5f);
    else if (cos >= 0) r->style = cos;
    else if (pl >= 0) r->style = fminf(1.0f, pl * 1.5f);
    else r->style = 0.35f;

    float t = 0.24f * r->key + 0.24f * r->bpm + 0.18f * r->style + 0.12f * r->sim + 0.22f * r->pair;
    if (r->bpm < 0.08f && r->pair <= 0) t *= 0.55f;

    /* Energy. pct < 0: the next track is natively faster (we pitch it down to meet us).
     * Build up wants faster tracks and rising keys, Cool down the opposite, Smooth no move. */
    int flow = c ? c->flow : NT_FLOW_SMOOTH;
    int up = r->pct < -0.3 && r->pct > -8, down = r->pct > 0.3 && r->pct < 8;
    int kup = r->step == 1 || r->step == 2 || r->step == -5, kdown = r->step == -1 || r->step == -2 || r->step == 5;
    if (flow == NT_FLOW_UP) {
        t += (up ? 0.09f : down ? -0.09f : 0) + (kup ? 0.06f : kdown ? -0.05f : 0);
    } else if (flow == NT_FLOW_DOWN) {
        t += (down ? 0.09f : up ? -0.09f : 0) + (kdown ? 0.06f : kup ? -0.05f : 0);
    } else {
        if (r->pct > -1.0 && r->pct < 1.0) t += 0.03f;
        if (r->step == 0) t += 0.03f;
    }
    if (c && c->same_artist_penalty && a->artist && b->artist && a->aident == b->aident)
        t -= 0.08f;
    if (t < 0) t = 0;
    r->total = t;
    return t;
}

static void insert_top(int *idx, nt_reason *why, int *n, int k, int i, const nt_reason *r)
{
    int pos = *n;
    while (pos > 0 && why[pos - 1].total < r->total) pos--;
    if (pos >= k) return;
    int last = *n < k ? *n : k - 1;
    for (int j = last; j > pos; j--) { idx[j] = idx[j - 1]; why[j] = why[j - 1]; }
    idx[pos] = i;
    why[pos] = *r;
    if (*n < k) (*n)++;
}

static int rank_ex(const nt_lib *L, const nt_ctx *c, const uint8_t *ex, int from, double from_bpm,
                   int k, int *out, nt_reason *why)
{
    int n = 0;
    nt_reason r;
    for (int i = 0; i < L->n; i++) {
        if (i == from || (ex && ex[i])) continue;
        float s = nt_score(L, c, from, from_bpm, i, &r);
        if (s <= 0) continue;
        /* The same song twice (a duplicate in the library, or on both sticks): the copy you
         * sorted wins (in playlists, stars, a color), then the better score. It takes the
         * other copy's place in the list. */
        int dup = -1;
        for (int j = 0; j < n; j++)
            if (L->t[out[j]].ident == L->t[i].ident) { dup = j; break; }
        if (dup >= 0) {
            int ci = nt_curation(&L->t[i]), cd = nt_curation(&L->t[out[dup]]);
            if (ci < cd || (ci == cd && s <= why[dup].total)) continue;
            r.total = s > why[dup].total ? s : why[dup].total;
            out[dup] = i;
            why[dup] = r;
            for (int j = dup; j > 0 && why[j - 1].total < why[j].total; j--) {   /* keep the order */
                int ti = out[j]; out[j] = out[j - 1]; out[j - 1] = ti;
                nt_reason tr = why[j]; why[j] = why[j - 1]; why[j - 1] = tr;
            }
            continue;
        }
        if (n == k && s <= why[k - 1].total) continue;
        insert_top(out, why, &n, k, i, &r);
    }
    return n;
}

int nt_rank(const nt_lib *L, const nt_ctx *c, int from, double from_bpm, int k, int *out,
            nt_reason *why)
{
    if (k > NT_MAX_RESULTS) k = NT_MAX_RESULTS;
    if (k < 1 || from < 0 || from >= L->n) return 0;
    /* also skip other copies of the same track (same song on both sticks) */
    return rank_ex(L, c, c ? c->excluded : NULL, from, from_bpm, k, out, why);
}

#define BEAM 12
#define EXPAND 6

typedef struct {
    int seq[NT_MAX_RESULTS];
    nt_reason why[NT_MAX_RESULTS];
    float sum;
    int len;
} beam_state;

int nt_chain(const nt_lib *L, const nt_ctx *c, int from, double from_bpm, int k, int *out,
             nt_reason *why)
{
    if (k > NT_MAX_RESULTS) k = NT_MAX_RESULTS;
    if (k < 1 || from < 0 || from >= L->n) return 0;
    uint8_t *ex = calloc((size_t)L->n, 1);
    beam_state *cur = calloc(BEAM, sizeof *cur), *next = calloc(BEAM * EXPAND, sizeof *next);
    if (!ex || !cur || !next) { free(ex); free(cur); free(next); return 0; }
    if (c && c->excluded) memcpy(ex, c->excluded, (size_t)L->n);
    ex[from] = 1;
    int ncur = 1;
    memset(&cur[0], 0, sizeof cur[0]);

    for (int depth = 0; depth < k; depth++) {
        int nnext = 0;
        for (int s = 0; s < ncur; s++) {
            beam_state *b = &cur[s];
            int last = b->len ? b->seq[b->len - 1] : from;
            double tempo = b->len ? 0 : from_bpm;
            for (int j = 0; j < b->len; j++) ex[b->seq[j]] = 1;
            int idx[EXPAND];
            nt_reason rr[EXPAND];
            int m = rank_ex(L, c, ex, last, tempo, EXPAND, idx, rr);
            for (int j = 0; j < b->len; j++) ex[b->seq[j]] = (c && c->excluded) ? c->excluded[b->seq[j]] : 0;
            for (int e = 0; e < m; e++) {
                /* the same song from the other stick is not a new track */
                int dup = 0;
                for (int j = 0; j < b->len; j++)
                    if (L->t[b->seq[j]].ident == L->t[idx[e]].ident) dup = 1;
                if (dup || L->t[idx[e]].ident == L->t[from].ident) continue;
                beam_state *nb = &next[nnext++];
                *nb = *b;
                nb->seq[nb->len] = idx[e];
                nb->why[nb->len] = rr[e];
                nb->len++;
                nb->sum += rr[e].total;
            }
        }
        if (!nnext) break;
        /* keep the BEAM best partial chains */
        for (int i = 1; i < nnext; i++) {
            beam_state v = next[i];
            int j = i - 1;
            while (j >= 0 && next[j].sum < v.sum) { next[j + 1] = next[j]; j--; }
            next[j + 1] = v;
        }
        ncur = nnext < BEAM ? nnext : BEAM;
        memcpy(cur, next, sizeof *cur * (size_t)ncur);
    }
    int len = ncur ? cur[0].len : 0;
    for (int i = 0; i < len; i++) { out[i] = cur[0].seq[i]; why[i] = cur[0].why[i]; }
    free(ex);
    free(cur);
    free(next);
    return len;
}

void nt_reason_str(const nt_lib *L, const nt_reason *r, int from, int to, char *buf, size_t n)
{
    char ka[4], kb[4], part[48];
    size_t o = 0;
    buf[0] = '\0';
    if (from < 0 || to < 0 || from >= L->n || to >= L->n) return;
    nt_camelot_str(L->t[from].camelot, ka);
    nt_camelot_str(L->t[to].camelot, kb);
    if (ka[0] && kb[0]) o += (size_t)snprintf(buf + o, n - o, "%s>%s", ka, kb);
    else if (kb[0]) o += (size_t)snprintf(buf + o, n - o, "%s", kb);
    if (o < n && L->t[to].bpm > 0) {
        snprintf(part, sizeof part, "%s%+.1f%%%s", o ? "  " : "", r->pct, r->halfdouble ? " x2" : "");
        o += (size_t)snprintf(buf + o, n - o, "%s", part);
    }
    if (o < n && r->shared_tag >= 0 && r->shared_tag < L->ntags)
        o += (size_t)snprintf(buf + o, n - o, "  %s", L->tags[r->shared_tag]);
    if (o < n && r->pair_count > 0)
        o += (size_t)snprintf(buf + o, n - o, "  mixed %dx", r->pair_count);
    else if (o < n && r->sim >= 0.15f)
        o += (size_t)snprintf(buf + o, n - o, "  similar %d%%", (int)(r->sim * 100 + 0.5f));
}
