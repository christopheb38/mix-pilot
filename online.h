/* SPDX-License-Identifier: GPL-3.0-only
 *
 * online.h: Mix Pilot's web access, used only from the mod's own network thread.
 *
 * HTTPS through the deck's own libcurl, opened with dlopen at the first use (the deck's
 * Community Mods fetcher already relies on curl). When the library is missing, every call
 * fails cleanly and the mod runs offline. Downloads are HTTPS only, size-capped and
 * time-limited; answers are parsed with minijson, never evaluated.
 */
#ifndef MIXPILOT_ONLINE_H
#define MIXPILOT_ONLINE_H

#include <stddef.h>

/* 0 = libcurl found and usable. Safe to call again; cheap after the first call. */
int net_available(void);
/* GET an https:// URL into a malloc'd, NUL-terminated buffer (*body, caller frees).
 * Returns the HTTP status (200 = ok), or -1 on a refused URL, a network failure, a body over
 * max bytes or a timeout of `timeout_s`. */
int net_get(const char *url, char **body, size_t *len, size_t max, int timeout_s);
/* The last failure in a few words ("curl 60 SSL ...", "HTTP 503", "error 10 Invalid API key"). */
const char *net_last_error(void);
/* Percent-encodes s into out (RFC 3986 unreserved kept). */
void net_escape(const char *s, char *out, size_t n);

/* Last.fm (https://www.last.fm/api). key = your free API key. Each returns the number of
 * results passed to fn, 0 for none (Last.fm does not know the song: a normal answer),
 * LFM_BADKEY when the key is refused, LFM_FAIL when there was no answer. fn returns nonzero
 * to stop. Titles and artists are cleaned first (lfm_clean). */
#define LFM_FAIL (-1)
#define LFM_BADKEY (-2)
#define LFM_NONE (-3)   /* internal: "not found", reported as 0 results */
void lfm_clean(const char *artist, const char *title, char *a, size_t an, char *t, size_t tn);
typedef int (*lfm_pair_fn)(const char *artist, const char *title, double match, void *user);
typedef int (*lfm_tag_fn)(const char *tag, int count, void *user);
typedef int (*lfm_artist_fn)(const char *artist, double match, void *user);
int lfm_similar_tracks(const char *key, const char *artist, const char *title, int limit,
                       lfm_pair_fn fn, void *user);
int lfm_similar_artists(const char *key, const char *artist, int limit, lfm_artist_fn fn,
                        void *user);
int lfm_track_tags(const char *key, const char *artist, const char *title, lfm_tag_fn fn,
                   void *user);
int lfm_artist_tags(const char *key, const char *artist, lfm_tag_fn fn, void *user);

/* Tags that say nothing about style ("seen live", "favorites", years...): 1 = skip it. */
int lfm_tag_is_noise(const char *tag);

#endif
