/* SPDX-License-Identifier: GPL-3.0-only
 *
 * pdb.c: rekordbox export.pdb reader (see pdb.h).
 *
 * The file is a sequence of fixed-size pages. Page 0 is the header: the page size and, for each
 * table, its type and its first and last page. A table's pages are chained by next_page. A data
 * page holds rows in a heap that starts 0x28 bytes into the page; the row offsets are stored in
 * groups of 16 at the end of the page, growing backwards, each group with a "present" bitmask.
 * Strings are DeviceSQL strings: a short ASCII form (odd first byte: length << 1 | 1), a long
 * ASCII form (0x40) and a long UTF-16LE form (0x90). Every read below is bounds-checked: a
 * damaged file gives fewer rows, never a read outside the buffer.
 */
#include "pdb.h"

#include <stdlib.h>
#include <string.h>

enum { T_TRACKS = 0, T_GENRES = 1, T_ARTISTS = 2, T_ALBUMS = 3, T_KEYS = 5, T_PL_TREE = 7, T_PL_ENTRIES = 8,
       T_HISTORY_ENTRIES = 12 };

#define HEAP 0x28
#define GROUP 0x24

typedef struct {
    const unsigned char *b;
    size_t len;
    uint32_t plen, ntables;
} pdb;

typedef struct {
    uint32_t id;
    uint32_t off;               /* in pool */
} name;

typedef struct {
    name *v;
    size_t n, cap;
    char *pool;
    size_t pn, pcap;
} names;

static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* A DeviceSQL string at row + ofs (row has `avail` readable bytes) into UTF-8 out[n]. */
static void dstr(const unsigned char *row, size_t avail, size_t ofs, char *out, size_t n)
{
    out[0] = '\0';
    if (ofs >= avail || n < 2) return;
    const unsigned char *s = row + ofs;
    size_t left = avail - ofs, o = 0;
    unsigned k = s[0];
    if (k == 0x40 || k == 0x90) {
        if (left < 4) return;
        size_t L = rd16(s + 1);
        if (L < 4 || L > left) return;
        const unsigned char *t = s + 4;
        size_t tl = L - 4;
        if (k == 0x40) {
            for (size_t i = 0; i < tl && o + 1 < n; i++)
                if (t[i]) out[o++] = (char)t[i];
        } else {
            for (size_t i = 0; i + 1 < tl && o + 4 < n; i += 2) {
                unsigned c = rd16(t + i);
                if (c >= 0xD800 && c < 0xDC00 && i + 3 < tl) {           /* surrogate pair */
                    unsigned lo = rd16(t + i + 2);
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                        i += 2;
                    }
                }
                if (c == 0) break;
                if (c < 0x80) out[o++] = (char)c;
                else if (c < 0x800) {
                    out[o++] = (char)(0xC0 | (c >> 6));
                    out[o++] = (char)(0x80 | (c & 0x3F));
                } else if (c < 0x10000) {
                    out[o++] = (char)(0xE0 | (c >> 12));
                    out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
                    out[o++] = (char)(0x80 | (c & 0x3F));
                } else {
                    out[o++] = (char)(0xF0 | (c >> 18));
                    out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
                    out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
                    out[o++] = (char)(0x80 | (c & 0x3F));
                }
            }
        }
    } else if (k & 1) {
        size_t L = k >> 1;
        if (L < 1 || L > left) return;
        for (size_t i = 1; i < L && o + 1 < n; i++)
            if (s[i]) out[o++] = (char)s[i];
    }
    out[o] = '\0';
}

typedef int (*row_fn)(const unsigned char *row, size_t avail, void *user);

/* Calls fn for every present row of the table of this type. */
static void each_row(const pdb *P, uint32_t type, row_fn fn, void *user)
{
    for (uint32_t t = 0; t < P->ntables; t++) {
        const unsigned char *e = P->b + 0x1c + 16 * (size_t)t;
        if (rd32(e) != type) continue;
        uint32_t page = rd32(e + 8), last = rd32(e + 12);
        size_t maxpages = P->len / P->plen;
        unsigned char *seen = calloc(maxpages / 8 + 1, 1);    /* a damaged chain may loop: each page once */
        if (!seen) return;
        while (page > 0 && (size_t)page < maxpages && !(seen[page / 8] & (1u << (page % 8)))) {
            seen[page / 8] |= (unsigned char)(1u << (page % 8));
            const unsigned char *pg = P->b + (size_t)page * P->plen;
            uint32_t next = rd32(pg + 12);
            if (rd32(pg + 8) == type && !(pg[27] & 0x40)) {           /* a data page of this table */
                unsigned small = pg[24], large = rd16(pg + 34);
                unsigned nrows = (large > small && large != 0x1fff) ? large : small;
                unsigned groups = nrows ? (nrows - 1) / 16 + 1 : 0;
                for (unsigned g = 0; g < groups; g++) {
                    size_t base = P->plen - (size_t)g * GROUP;
                    if (base < HEAP + GROUP) break;
                    unsigned present = rd16(pg + base - 4);
                    for (unsigned j = 0; j < 16 && g * 16 + j < nrows; j++) {
                        if (!((present >> j) & 1)) continue;
                        size_t ofs = HEAP + (size_t)rd16(pg + base - 6 - 2 * j);
                        if (ofs >= P->plen - GROUP) continue;
                        if (fn(pg + ofs, P->plen - ofs, user)) { free(seen); return; }
                    }
                }
            }
            if (page == last) break;
            page = next;
        }
        free(seen);
        return;
    }
}

/* ---- id -> name tables ------------------------------------------------------------------- */

static void names_add(names *m, uint32_t id, const char *s)
{
    size_t l = strlen(s) + 1;
    if (m->n == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 256;
        name *v = realloc(m->v, cap * sizeof *v);
        if (!v) return;
        m->v = v;
        m->cap = cap;
    }
    if (m->pn + l > m->pcap) {
        size_t cap = m->pcap ? m->pcap * 2 : 8192;
        while (cap < m->pn + l) cap *= 2;
        char *p = realloc(m->pool, cap);
        if (!p) return;
        m->pool = p;
        m->pcap = cap;
    }
    memcpy(m->pool + m->pn, s, l);
    m->v[m->n].id = id;
    m->v[m->n].off = (uint32_t)m->pn;
    m->n++;
    m->pn += l;
}

static int by_id(const void *a, const void *b)
{
    uint32_t x = ((const name *)a)->id, y = ((const name *)b)->id;
    return x < y ? -1 : x > y;
}

static const char *names_get(const names *m, uint32_t id)
{
    if (!id || !m->n) return "";
    name k = { id, 0 };
    const name *f = bsearch(&k, m->v, m->n, sizeof *m->v, by_id);
    return f ? m->pool + f->off : "";
}

static void names_free(names *m)
{
    free(m->v);
    free(m->pool);
}

static int on_genre(const unsigned char *r, size_t a, void *u)
{
    if (a < 6) return 0;
    char s[192];
    dstr(r, a, 4, s, sizeof s);
    names_add(u, rd32(r), s);
    return 0;
}

static int on_key(const unsigned char *r, size_t a, void *u)
{
    if (a < 10) return 0;
    char s[32];
    dstr(r, a, 8, s, sizeof s);
    names_add(u, rd32(r), s);
    return 0;
}

static int on_artist(const unsigned char *r, size_t a, void *u)
{
    if (a < 12) return 0;
    unsigned sub = rd16(r);
    size_t ofs = (sub & 0x04) ? rd16(r + 0x0a) : r[9];            /* 0x64: a far offset */
    char s[192];
    dstr(r, a, ofs, s, sizeof s);
    names_add(u, rd32(r + 4), s);
    return 0;
}

static int on_album(const unsigned char *r, size_t a, void *u)
{
    if (a < 24) return 0;
    unsigned sub = rd16(r);
    size_t ofs = (sub & 0x04) ? rd16(r + 0x16) : r[0x15];          /* 0x84: a far offset */
    char s[192];
    dstr(r, a, ofs, s, sizeof s);
    names_add(u, rd32(r + 12), s);
    return 0;
}

/* ---- tracks and playlists -------------------------------------------------------------- */

typedef struct {
    names genres, keys, artists, albums;
    pdb_track_fn on_track;
    pdb_playlist_fn on_playlist;
    pdb_entry_fn on_entry;
    void *user;
    int ntracks, stop;
} ctx;

#define TRACK_FIXED 0x88   /* the fixed part of a track row: 0x5e bytes of fields, 21 string offsets */

static int on_track_row(const unsigned char *r, size_t a, void *u)
{
    ctx *c = u;
    if (a < TRACK_FIXED) return 0;
    char title[256], fname[256];
    size_t ofs_title = rd16(r + 0x5e + 2 * 17), ofs_file = rd16(r + 0x5e + 2 * 19);
    dstr(r, a, ofs_title, title, sizeof title);
    if (!title[0]) {                                                /* no title: the file name */
        dstr(r, a, ofs_file, fname, sizeof fname);
        char *dot = strrchr(fname, '.');
        if (dot) *dot = '\0';
        memcpy(title, fname, sizeof title);
    }
    pdb_track t;
    t.id = rd32(r + 72);
    t.title = title;
    t.artist = names_get(&c->artists, rd32(r + 68));
    t.album = names_get(&c->albums, rd32(r + 64));
    t.genre = names_get(&c->genres, rd32(r + 60));
    t.key = names_get(&c->keys, rd32(r + 32));
    t.bpm = rd32(r + 56) / 100.0;
    t.len_s = rd16(r + 84);
    t.color = r[88];
    t.rating = r[89] <= 5 ? r[89] : 0;
    if (!t.id) return 0;
    c->ntracks++;
    if (c->on_track && c->on_track(&t, c->user)) { c->stop = 1; return 1; }
    return 0;
}

static int on_tree_row(const unsigned char *r, size_t a, void *u)
{
    ctx *c = u;
    if (a < 22) return 0;
    char s[192];
    dstr(r, a, 20, s, sizeof s);
    if (c->on_playlist && c->on_playlist(rd32(r + 12), rd32(r), s, rd32(r + 16) != 0, c->user)) {
        c->stop = 1;
        return 1;
    }
    return 0;
}

static int on_entry_row(const unsigned char *r, size_t a, void *u)
{
    ctx *c = u;
    if (a < 12) return 0;
    if (c->on_entry && c->on_entry(rd32(r + 8), rd32(r + 4), rd32(r), c->user)) {
        c->stop = 1;
        return 1;
    }
    return 0;
}

static int pdb_open(pdb *P, const unsigned char *buf, size_t len)
{
    if (!buf || len < 0x1c) return -1;
    *P = (pdb){ buf, len, rd32(buf + 4), rd32(buf + 8) };
    if (rd32(buf) != 0 || P->plen < 512 || P->plen > 65536 || (P->plen & (P->plen - 1)) || len < 2 * (size_t)P->plen ||
        P->ntables == 0 || P->ntables > 64 || 0x1c + 16 * (size_t)P->ntables > P->plen)
        return -1;
    return 0;
}

typedef struct {
    pdb_entry_fn fn;
    void *user;
    int n;
} hist_ctx;

static int on_history_row(const unsigned char *r, size_t a, void *u)
{
    hist_ctx *h = u;
    if (a < 12) return 0;
    h->n++;
    return h->fn ? h->fn(rd32(r + 4), rd32(r), rd32(r + 8), h->user) : 0;   /* track, history, index */
}

int pdb_history(const unsigned char *buf, size_t len, pdb_entry_fn fn, void *user)
{
    pdb P;
    if (pdb_open(&P, buf, len)) return -1;
    hist_ctx h = { fn, user, 0 };
    each_row(&P, T_HISTORY_ENTRIES, on_history_row, &h);
    return h.n;
}

int pdb_read(const unsigned char *buf, size_t len, pdb_track_fn on_track, pdb_playlist_fn on_playlist,
             pdb_entry_fn on_entry, void *user)
{
    pdb P;
    if (pdb_open(&P, buf, len)) return -1;
    ctx c;
    memset(&c, 0, sizeof c);
    c.on_track = on_track;
    c.on_playlist = on_playlist;
    c.on_entry = on_entry;
    c.user = user;
    each_row(&P, T_GENRES, on_genre, &c.genres);
    each_row(&P, T_KEYS, on_key, &c.keys);
    each_row(&P, T_ARTISTS, on_artist, &c.artists);
    each_row(&P, T_ALBUMS, on_album, &c.albums);
    names *all[4] = { &c.genres, &c.keys, &c.artists, &c.albums };
    for (int i = 0; i < 4; i++)
        if (all[i]->n) qsort(all[i]->v, all[i]->n, sizeof *all[i]->v, by_id);
    each_row(&P, T_TRACKS, on_track_row, &c);
    if (!c.stop) each_row(&P, T_PL_TREE, on_tree_row, &c);
    if (!c.stop) each_row(&P, T_PL_ENTRIES, on_entry_row, &c);
    for (int i = 0; i < 4; i++) names_free(all[i]);
    return c.ntracks;
}
