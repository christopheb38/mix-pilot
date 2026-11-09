/* SPDX-License-Identifier: GPL-3.0-only
 *
 * pdbgen.h: builds a small synthetic export.pdb in memory, for the tests (no user library is
 * ever committed). One page per table: genres, keys, artists, albums, tracks, playlist tree,
 * playlist entries, history entries. It covers the format's corners: short ASCII and long UTF-16 strings, an
 * artist row with a far name offset (subtype 0x64), and more than 16 rows on a page.
 */
#ifndef MIXPILOT_PDBGEN_H
#define MIXPILOT_PDBGEN_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PG 4096
#define PG_HEAP 0x28

typedef struct {
    unsigned char *b;
    size_t len;
    int heap[16], rows[16];
} pdbgen;

static void pg_w32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static void pg_w16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

/* A short ASCII DeviceSQL string; returns its size. */
static size_t pg_sstr(unsigned char *o, const char *s)
{
    size_t l = strlen(s);
    o[0] = (unsigned char)(((l + 1) << 1) | 1);
    memcpy(o + 1, s, l);
    return l + 1;
}

/* A long UTF-16LE DeviceSQL string from UTF-8 (BMP only); returns its size. */
static size_t pg_ustr(unsigned char *o, const char *s)
{
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p++;
        if (c >= 0xE0) { c = (c & 0x0F) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F); p += 2; }
        else if (c >= 0xC0) { c = (c & 0x1F) << 6 | (p[0] & 0x3F); p++; }
        pg_w16(o + 4 + 2 * n++, c);
    }
    o[0] = 0x90;
    pg_w16(o + 1, (unsigned)(4 + 2 * n));
    o[3] = 0;
    return 4 + 2 * n;
}

static void pg_page(pdbgen *g, int page, uint32_t type)
{
    unsigned char *p = g->b + (size_t)page * PG;
    pg_w32(p + 4, (uint32_t)page);
    pg_w32(p + 8, type);
    pg_w32(p + 12, 0);
    p[27] = 0x24;                                         /* a data page */
    g->heap[page] = 0;
    g->rows[page] = 0;
}

static void pg_row(pdbgen *g, int page, const unsigned char *row, size_t len)
{
    unsigned char *p = g->b + (size_t)page * PG;
    int r = g->rows[page]++, grp = r / 16, j = r % 16;
    size_t base = PG - (size_t)grp * 0x24;
    pg_w16(p + base - 6 - 2 * j, (unsigned)g->heap[page]);
    pg_w16(p + base - 4, (unsigned)(p[base - 4] | p[base - 3] << 8) | (1u << j));
    memcpy(p + PG_HEAP + g->heap[page], row, len);
    g->heap[page] += (int)((len + 3) & ~(size_t)3);
    if (g->rows[page] <= 255) p[24] = (unsigned char)g->rows[page];
    pg_w16(p + 34, (unsigned)g->rows[page]);
}

static void pg_track(pdbgen *g, uint32_t id, const char *title, uint32_t artist, uint32_t album, uint32_t genre,
                     uint32_t key, double bpm, unsigned len_s)
{
    unsigned char r[512];
    memset(r, 0, sizeof r);
    pg_w16(r, 0x24);
    pg_w32(r + 32, key);
    pg_w32(r + 56, (uint32_t)(bpm * 100 + 0.5));
    pg_w32(r + 60, genre);
    pg_w32(r + 64, album);
    pg_w32(r + 68, artist);
    pg_w32(r + 72, id);
    pg_w16(r + 84, len_s);
    size_t o = 0x88;
    r[o] = 0x03;                                          /* an empty string for every other field */
    for (int i = 0; i < 21; i++) pg_w16(r + 0x5e + 2 * i, 0x88);
    o += 1;
    pg_w16(r + 0x5e + 2 * 17, (unsigned)o);
    o += pg_sstr(r + o, title);
    pg_row(g, 5, r, o);
}

/* The library the tests expect: 20 tracks on one page (two row groups), 3 artists (one with a
 * far offset, one in UTF-16 with an accent), 2 genres, 3 keys, a folder with two playlists. */
static pdbgen pg_build(void)
{
    pdbgen g;
    memset(&g, 0, sizeof g);
    g.len = 9 * PG;
    g.b = calloc(1, g.len);
    unsigned char *h = g.b;
    pg_w32(h + 4, PG);
    pg_w32(h + 8, 8);
    pg_w32(h + 12, 9);
    static const uint32_t types[8] = { 1, 5, 2, 3, 0, 7, 8, 12 };
    for (int t = 0; t < 8; t++) {
        unsigned char *e = h + 0x1c + 16 * t;
        pg_w32(e, types[t]);
        pg_w32(e + 4, 0);
        pg_w32(e + 8, (uint32_t)(t + 1));
        pg_w32(e + 12, (uint32_t)(t + 1));
        pg_page(&g, t + 1, types[t]);
    }
    unsigned char r[256];
    size_t n;
    /* genres (page 1) */
    memset(r, 0, sizeof r); pg_w32(r, 1); n = 4 + pg_sstr(r + 4, "Reggaeton"); pg_row(&g, 1, r, n);
    memset(r, 0, sizeof r); pg_w32(r, 2); n = 4 + pg_sstr(r + 4, "House"); pg_row(&g, 1, r, n);
    /* keys (page 2) */
    static const char *const keys[3] = { "Am", "Em", "Dm" };
    for (int k = 0; k < 3; k++) {
        memset(r, 0, sizeof r); pg_w32(r, (uint32_t)k + 1); pg_w32(r + 4, (uint32_t)k + 1);
        n = 8 + pg_sstr(r + 8, keys[k]); pg_row(&g, 2, r, n);
    }
    /* artists (page 3): near offset, far offset (0x64), UTF-16 name */
    memset(r, 0, sizeof r); pg_w16(r, 0x60); pg_w32(r + 4, 1); r[8] = 3; r[9] = 10; n = 10 + pg_sstr(r + 10, "Don Omar"); pg_row(&g, 3, r, n);
    memset(r, 0, sizeof r); pg_w16(r, 0x64); pg_w32(r + 4, 2); r[8] = 3; r[9] = 0; pg_w16(r + 10, 12); n = 12 + pg_sstr(r + 12, "J Balvin"); pg_row(&g, 3, r, n);
    memset(r, 0, sizeof r); pg_w16(r, 0x60); pg_w32(r + 4, 3); r[8] = 3; r[9] = 10; n = 10 + pg_ustr(r + 10, "Beyonc\xc3\xa9"); pg_row(&g, 3, r, n);
    /* albums (page 4) */
    memset(r, 0, sizeof r); pg_w16(r, 0x80); pg_w32(r + 12, 1); r[20] = 3; r[21] = 22; n = 22 + pg_sstr(r + 22, "Hits"); pg_row(&g, 4, r, n);
    /* tracks (page 5): 20, in two row groups */
    char t[32];
    for (int i = 0; i < 20; i++) {
        snprintf(t, sizeof t, "Track %02d", i + 1);
        pg_track(&g, (uint32_t)i + 1, t, (uint32_t)(i % 3) + 1, 1, (uint32_t)(i % 2) + 1, (uint32_t)(i % 3) + 1,
                 120 + i * 0.5, 200 + (unsigned)i);
    }
    /* playlist tree (page 6): folder 1 "STYLE" holding playlists 2 and 3 */
    static const struct { uint32_t id, parent, folder; const char *name; } pl[3] = {
        { 1, 0, 1, "STYLE" }, { 2, 1, 0, "Latino" }, { 3, 1, 0, "House" } };
    for (int i = 0; i < 3; i++) {
        memset(r, 0, sizeof r); pg_w32(r, pl[i].parent); pg_w32(r + 8, (uint32_t)i); pg_w32(r + 12, pl[i].id);
        pg_w32(r + 16, pl[i].folder); n = 20 + pg_sstr(r + 20, pl[i].name); pg_row(&g, 6, r, n);
    }
    /* entries (page 7): tracks 1..10 in Latino, 11..20 in House */
    for (int i = 0; i < 20; i++) {
        memset(r, 0, sizeof r); pg_w32(r, (uint32_t)(i % 10) + 1); pg_w32(r + 4, (uint32_t)i + 1);
        pg_w32(r + 8, i < 10 ? 2 : 3); pg_row(&g, 7, r, 12);
    }
    /* a play history (page 8): tracks 1, 5, 9 in that order (track, history, position) */
    static const uint32_t hist[3] = { 1, 5, 9 };
    for (int i = 0; i < 3; i++) {
        memset(r, 0, sizeof r); pg_w32(r, hist[i]); pg_w32(r + 4, 1); pg_w32(r + 8, (uint32_t)i + 1);
        pg_row(&g, 8, r, 12);
    }
    return g;
}

#endif
