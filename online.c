/* SPDX-License-Identifier: GPL-3.0-only
 * online.c: see online.h. Runs only on Mix Pilot's network thread. */
#include "online.h"

#include <ctype.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minijson.h"

/* The few libcurl entry points and constants we need (stable ABI since curl 7.x). */
typedef void CURL;
typedef CURL *(*easy_init_fn)(void);
typedef int (*easy_setopt_fn)(CURL *, int, ...);
typedef int (*easy_perform_fn)(CURL *);
typedef int (*easy_getinfo_fn)(CURL *, int, ...);
typedef void (*easy_cleanup_fn)(CURL *);

#define O_WRITEDATA 10001
#define O_URL 10002
#define O_USERAGENT 10018
#define O_WRITEFUNCTION 20011
#define O_TIMEOUT 13
#define O_LOW_SPEED_LIMIT 19
#define O_LOW_SPEED_TIME 20
#define O_FOLLOWLOCATION 52
#define O_MAXREDIRS 68
#define O_CONNECTTIMEOUT 78
#define O_NOSIGNAL 99
#define O_PROTOCOLS 181
#define O_REDIR_PROTOCOLS 182
#define PROTO_HTTPS 2L
#define I_RESPONSE_CODE 0x200002

static struct {
    int tried, ok;
    easy_init_fn init;
    easy_setopt_fn setopt;
    easy_perform_fn perform;
    easy_getinfo_fn getinfo;
    easy_cleanup_fn cleanup;
} cu;
static pthread_mutex_t cu_mu = PTHREAD_MUTEX_INITIALIZER;

int net_available(void)
{
    pthread_mutex_lock(&cu_mu);
    if (!cu.tried) {
        cu.tried = 1;
        static const char *const names[] = { "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so", NULL };
        void *h = NULL;
        for (int i = 0; names[i] && !h; i++) h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (h) {
            cu.init = (easy_init_fn)dlsym(h, "curl_easy_init");
            cu.setopt = (easy_setopt_fn)dlsym(h, "curl_easy_setopt");
            cu.perform = (easy_perform_fn)dlsym(h, "curl_easy_perform");
            cu.getinfo = (easy_getinfo_fn)dlsym(h, "curl_easy_getinfo");
            cu.cleanup = (easy_cleanup_fn)dlsym(h, "curl_easy_cleanup");
            cu.ok = cu.init && cu.setopt && cu.perform && cu.getinfo && cu.cleanup;
        }
    }
    int ok = cu.ok;
    pthread_mutex_unlock(&cu_mu);
    return ok ? 0 : -1;
}

typedef struct {
    char *b;
    size_t n, cap, max;
    int over;
} sink;

static size_t on_data(char *p, size_t sz, size_t nm, void *user)
{
    sink *s = user;
    size_t add = sz * nm;
    if (s->n + add > s->max) { s->over = 1; return 0; }   /* 0 aborts the transfer */
    if (s->n + add + 1 > s->cap) {
        size_t cap = s->cap ? s->cap : 16384;
        while (cap < s->n + add + 1) cap *= 2;
        char *b = realloc(s->b, cap);
        if (!b) return 0;
        s->b = b;
        s->cap = cap;
    }
    memcpy(s->b + s->n, p, add);
    s->n += add;
    s->b[s->n] = '\0';
    return add;
}

int net_get(const char *url, char **body, size_t *len, size_t max, int timeout_s)
{
    *body = NULL;
    if (len) *len = 0;
    if (!url || strncmp(url, "https://", 8) != 0) return -1;
    if (net_available() != 0) return -1;
    CURL *c = cu.init();
    if (!c) return -1;
    sink s = { NULL, 0, 0, max, 0 };
    cu.setopt(c, O_URL, url);
    cu.setopt(c, O_USERAGENT, "MixPilot/1.0 (XDJ-AZ community mod)");
    cu.setopt(c, O_WRITEFUNCTION, on_data);
    cu.setopt(c, O_WRITEDATA, &s);
    cu.setopt(c, O_NOSIGNAL, 1L);                         /* never touch EP147's signals */
    cu.setopt(c, O_CONNECTTIMEOUT, 6L);
    cu.setopt(c, O_TIMEOUT, (long)timeout_s);
    cu.setopt(c, O_LOW_SPEED_LIMIT, 512L);
    cu.setopt(c, O_LOW_SPEED_TIME, 8L);
    cu.setopt(c, O_FOLLOWLOCATION, 1L);
    cu.setopt(c, O_MAXREDIRS, 4L);
    cu.setopt(c, O_PROTOCOLS, PROTO_HTTPS);
    cu.setopt(c, O_REDIR_PROTOCOLS, PROTO_HTTPS);
    int rc = cu.perform(c);
    long code = 0;
    cu.getinfo(c, I_RESPONSE_CODE, &code);
    cu.cleanup(c);
    if (rc != 0 || s.over || !s.b) {
        free(s.b);
        return -1;
    }
    *body = s.b;
    if (len) *len = s.n;
    return (int)code;
}

void net_escape(const char *s, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; s && *s && o + 4 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
}

/* ---- Last.fm --------------------------------------------------------------------------- */

#define LFM "https://ws.audioscrobbler.com/2.0/?format=json&autocorrect=1"

static int lfm_get(const char *url, mj_doc *d, char **body)
{
    size_t len;
    int code = net_get(url, body, &len, 2u << 20, 12);
    if (code != 200 || !*body) {
        free(*body);
        *body = NULL;
        return -1;
    }
    if (mj_parse(d, *body, len) != 0 || mj_get(d, 0, "error") >= 0) {
        mj_free(d);
        free(*body);
        *body = NULL;
        return -1;
    }
    return 0;
}

int lfm_similar_tracks(const char *key, const char *artist, const char *title, int limit,
                       lfm_pair_fn fn, void *user)
{
    char ea[256], et[384], ek[96], url[900];
    net_escape(artist, ea, sizeof ea);
    net_escape(title, et, sizeof et);
    net_escape(key, ek, sizeof ek);
    snprintf(url, sizeof url, LFM "&method=track.getsimilar&artist=%s&track=%s&limit=%d&api_key=%s",
             ea, et, limit, ek);
    mj_doc d;
    char *body;
    if (lfm_get(url, &d, &body)) return -1;
    int arr = mj_path(&d, 0, "similartracks.track"), n = 0;
    for (int i = 0; i < mj_len(&d, arr); i++) {
        int e = mj_at(&d, arr, i);
        char a[160], t[200];
        mj_str(&d, mj_get(&d, e, "name"), t, sizeof t);
        mj_str(&d, mj_path(&d, e, "artist.name"), a, sizeof a);
        double m = mj_num(&d, mj_get(&d, e, "match"));
        if (!a[0] || !t[0]) continue;
        n++;
        if (fn(a, t, m, user)) break;
    }
    mj_free(&d);
    free(body);
    return n;
}

int lfm_similar_artists(const char *key, const char *artist, int limit, lfm_artist_fn fn, void *user)
{
    char ea[256], ek[96], url[700];
    net_escape(artist, ea, sizeof ea);
    net_escape(key, ek, sizeof ek);
    snprintf(url, sizeof url, LFM "&method=artist.getsimilar&artist=%s&limit=%d&api_key=%s", ea, limit, ek);
    mj_doc d;
    char *body;
    if (lfm_get(url, &d, &body)) return -1;
    int arr = mj_path(&d, 0, "similarartists.artist"), n = 0;
    for (int i = 0; i < mj_len(&d, arr); i++) {
        int e = mj_at(&d, arr, i);
        char a[160];
        mj_str(&d, mj_get(&d, e, "name"), a, sizeof a);
        if (!a[0]) continue;
        n++;
        if (fn(a, mj_num(&d, mj_get(&d, e, "match")), user)) break;
    }
    mj_free(&d);
    free(body);
    return n;
}

static int tags_from(const char *url, lfm_tag_fn fn, void *user)
{
    mj_doc d;
    char *body;
    if (lfm_get(url, &d, &body)) return -1;
    int arr = mj_path(&d, 0, "toptags.tag"), n = 0;
    for (int i = 0; i < mj_len(&d, arr); i++) {
        int e = mj_at(&d, arr, i);
        char t[48];
        mj_str(&d, mj_get(&d, e, "name"), t, sizeof t);
        int count = (int)mj_num(&d, mj_get(&d, e, "count"));
        if (!t[0] || lfm_tag_is_noise(t)) continue;
        n++;
        if (fn(t, count, user)) break;
    }
    mj_free(&d);
    free(body);
    return n;
}

int lfm_track_tags(const char *key, const char *artist, const char *title, lfm_tag_fn fn, void *user)
{
    char ea[256], et[384], ek[96], url[900];
    net_escape(artist, ea, sizeof ea);
    net_escape(title, et, sizeof et);
    net_escape(key, ek, sizeof ek);
    snprintf(url, sizeof url, LFM "&method=track.gettoptags&artist=%s&track=%s&api_key=%s", ea, et, ek);
    return tags_from(url, fn, user);
}

int lfm_artist_tags(const char *key, const char *artist, lfm_tag_fn fn, void *user)
{
    char ea[256], ek[96], url[700];
    net_escape(artist, ea, sizeof ea);
    net_escape(key, ek, sizeof ek);
    snprintf(url, sizeof url, LFM "&method=artist.gettoptags&artist=%s&api_key=%s", ea, ek);
    return tags_from(url, fn, user);
}

int lfm_tag_is_noise(const char *tag)
{
    static const char *const noise[] = { "seen live", "favorite", "favourite", "love", "awesome",
                                         "beautiful", "cool", "my ", "best", "spotify", "albums i own",
                                         "under 2000 listeners", "fip", "check out", "good", "great",
                                         "amazing", "classic", "male vocalists", "female vocalists",
                                         "british", "american", "french", "german", "usa", "uk", NULL };
    char low[48];
    size_t o = 0;
    int digits = 0;
    for (const char *s = tag; *s && o + 1 < sizeof low; s++) {
        low[o++] = (char)tolower((unsigned char)*s);
        if (isdigit((unsigned char)*s)) digits++;
    }
    low[o] = '\0';
    if (o < 2 || o > 23) return 1;
    if (digits >= 4) return 1;                            /* years: "2019" */
    if (digits && strstr(low, "0s") && o <= 4) return 0;  /* "80s", "90s" say something */
    for (int i = 0; noise[i]; i++)
        if (strstr(low, noise[i])) return 1;
    return 0;
}
