/* SPDX-License-Identifier: GPL-3.0-only
 *
 * engine.h: Mix Pilot's recommendation engine. Pure C, no deck services, no I/O:
 * it is built into the mod and into the host unit tests (tests/engine_test.c) alike.
 *
 * A library (nt_lib) holds every track the mod read from the sticks. For a "from" track the
 * engine scores every other track as a next track, from five signals:
 *
 *   key     harmonic distance on the Camelot wheel
 *   bpm     tempo distance, half and double time included
 *   style   genre tags (Last.fm, when online) and the playlists the two tracks share
 *   sim     Last.fm "similar track" match for the pair (listener co-occurrence)
 *   pair    transitions actually played: by you (learned on the deck) and by the
 *           community (a pairings file shipped with the mod or fetched from a URL)
 *
 * nt_rank returns the best K next tracks; nt_chain returns the best ordered sequence of K
 * tracks (a beam search over the same transition score), for planning several mixes ahead.
 */
#ifndef MIXPILOT_ENGINE_H
#define MIXPILOT_ENGINE_H

#include <stdint.h>
#include <stddef.h>

#define NT_MAX_TRACKS 20000
#define NT_TRACK_PL 12      /* playlists remembered per track */
#define NT_TRACK_TAGS 8     /* genre tags remembered per track */
#define NT_MAX_TAGS 512     /* tag vocabulary */
#define NT_MAX_RESULTS 10

/* ---- small helpers, exposed for the tests and the mod -------------------------------- */

/* Camelot code from a key string: "8A", "08B", "Am", "F#m", "Gb", "Ebmin", "A minor"...
 * Returns num*2 + (B ? 1 : 0) with num 1..12, or -1 when unknown. */
int nt_camelot(const char *key);
/* "8A" for a code from nt_camelot, "" for -1. buf holds at least 4 bytes. */
void nt_camelot_str(int code, char *buf);
/* Harmonic compatibility from a to b, 0..1 (0.5 when either is unknown). *step gets the
 * signed wheel step (-5..6) when both are known and the letters match, else 99. */
float nt_key_score(int a, int b, int *step);
/* Tempo compatibility of playing `to` (its native BPM) after `from` (current tempo), 0..1.
 * *pct gets the signed pitch change needed, in percent, after half/double time. */
float nt_bpm_score(double from, double to, double *pct, int *halfdouble);
/* Identity hash of a track across sticks and sources: normalized "artist|title"
 * (lowercase ASCII, accents folded, "(Original Mix)", "feat. X" and punctuation dropped). */
uint32_t nt_ident(const char *artist, const char *title);
/* The normalized form behind nt_ident, for logs and files. */
void nt_normalize(const char *in, char *out, size_t n);

/* ---- the library --------------------------------------------------------------------- */

typedef struct nt_track {
    uint32_t id;            /* rekordbox content id on its stick */
    uint32_t ident;         /* nt_ident(artist, title) */
    uint32_t aident;        /* nt_ident(artist, ""): same primary artist */
    int8_t stick;           /* 0 = USB 1, 1 = USB 2 */
    int8_t camelot;         /* nt_camelot, -1 unknown */
    uint8_t npl, ntags;
    float bpm;              /* native BPM, 0 unknown */
    float len_s;
    uint32_t title, artist, album, where;   /* offsets in the string pool; where = first playlist */
    uint16_t pl[NT_TRACK_PL];               /* playlist indexes (lib-wide) */
    uint16_t tag[NT_TRACK_TAGS];            /* tag indexes */
    uint8_t tagw[NT_TRACK_TAGS];            /* tag weights 1..100 */
} nt_track;

/* An open-addressing map from a pair of identities to a float (pair counts, similarity). */
typedef struct nt_pairmap {
    uint64_t *keys;         /* (from << 32) | to; 0 = empty */
    float *vals;
    uint32_t cap, n;        /* cap is a power of two */
} nt_pairmap;

typedef struct nt_lib {
    nt_track *t;
    int n, cap;
    char *pool;
    uint32_t pool_n, pool_cap;
    char tags[NT_MAX_TAGS][24];
    int ntags;
    int npl;                /* playlists seen */
    nt_pairmap pairs;       /* transitions played: count */
    nt_pairmap sim;         /* Last.fm similarity: match 0..1 */
    nt_pairmap byid;        /* (stick + 1, id) -> index + 1 */
} nt_lib;

int nt_lib_init(nt_lib *L, int cap);
void nt_lib_free(nt_lib *L);
/* Adds a track (deduplicated on stick + id); returns its index or -1 when full. */
int nt_lib_add(nt_lib *L, int stick, uint32_t id, const char *title, const char *artist,
               const char *album, const char *key, double bpm, double len_s);
/* Records that track i is in playlist pl (index chosen by the caller), named name. */
void nt_lib_in_playlist(nt_lib *L, int i, int pl, const char *name);
const char *nt_str(const nt_lib *L, uint32_t off);
/* Tag vocabulary index for a tag name (added when new), -1 when the vocabulary is full. */
int nt_tag_index(nt_lib *L, const char *tag);
/* Sets the tags of every track with this identity (weights 1..100). */
void nt_set_tags(nt_lib *L, uint32_t ident, const int *tag, const int *w, int n);
/* Finds a track: by stick + id first, then by identity. -1 = not in the library. */
int nt_find(const nt_lib *L, int stick, uint32_t id, uint32_t ident);

int nt_pair_add(nt_pairmap *m, uint32_t from, uint32_t to, float v);   /* m[from,to] += v */
int nt_pair_set(nt_pairmap *m, uint32_t from, uint32_t to, float v);   /* m[from,to] = max(.., v) */
float nt_pair_get(const nt_pairmap *m, uint32_t from, uint32_t to);
void nt_pair_free(nt_pairmap *m);

/* ---- scoring ------------------------------------------------------------------------- */

enum { NT_FLOW_SMOOTH = 0, NT_FLOW_UP, NT_FLOW_DOWN };

typedef struct nt_ctx {
    int flow;                   /* NT_FLOW_* */
    int same_artist_penalty;    /* 1 = avoid the same artist twice in a row */
    const uint8_t *excluded;    /* NULL or one byte per track: 1 = never suggest (played, loaded) */
} nt_ctx;

typedef struct nt_reason {
    float total, key, bpm, style, sim, pair;
    double pct;                 /* pitch change needed, % */
    int halfdouble;             /* 1 = matched at half or double time */
    int step;                   /* Camelot step, 99 = letter change or unknown */
    int pair_count;             /* times this transition was played (you + community) */
    int shared_tag;             /* best shared tag index, -1 none */
} nt_reason;

/* Transition score from track `from` played at tempo from_bpm (0 = its native BPM) to `to`. */
float nt_score(const nt_lib *L, const nt_ctx *c, int from, double from_bpm, int to, nt_reason *r);

/* The best k next tracks after `from` (k <= NT_MAX_RESULTS); returns how many. */
int nt_rank(const nt_lib *L, const nt_ctx *c, int from, double from_bpm, int k, int *out,
            nt_reason *why);
/* The best ordered sequence of k tracks after `from` (beam search); returns its length. */
int nt_chain(const nt_lib *L, const nt_ctx *c, int from, double from_bpm, int k, int *out,
             nt_reason *why);

/* One line of reasons, e.g. "8A>9A  +1.8%  house  played 3x", for the screen. */
void nt_reason_str(const nt_lib *L, const nt_reason *r, int from, int to, char *buf, size_t n);

#endif
