/* SPDX-License-Identifier: GPL-3.0-only
 * Host unit tests for engine.c:  make engine-test */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../engine.h"
#include "../minijson.h"

static int fails, checks;
#define CHECK(c)                                                                   \
    do {                                                                           \
        checks++;                                                                  \
        if (!(c)) { fails++; fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

static void keys(void)
{
    char b[4];
    CHECK(nt_camelot("Am") == 8 * 2 + 0);
    CHECK(nt_camelot("C") == 8 * 2 + 1);
    CHECK(nt_camelot("8A") == 16);
    CHECK(nt_camelot("08B") == 17);
    CHECK(nt_camelot("F#m") == 11 * 2);
    CHECK(nt_camelot("Gbm") == 11 * 2);
    CHECK(nt_camelot("Db") == 3 * 2 + 1);
    CHECK(nt_camelot("C#") == 3 * 2 + 1);
    CHECK(nt_camelot("Abm") == 1 * 2);
    CHECK(nt_camelot("E") == 12 * 2 + 1);
    CHECK(nt_camelot("Ebmin") == 2 * 2);                  /* Ebm = 2A */
    CHECK(nt_camelot("A minor") == 16);
    CHECK(nt_camelot("") == -1 && nt_camelot("13A") == -1 && nt_camelot("H") == -1);
    nt_camelot_str(nt_camelot("Bm"), b);
    CHECK(strcmp(b, "10A") == 0);

    int st;
    CHECK(nt_key_score(16, 16, &st) == 1.0f && st == 0);
    CHECK(nt_key_score(16, 18, &st) > 0.85f && st == 1);  /* 8A -> 9A */
    CHECK(nt_key_score(24, 2, &st) > 0.85f && st == 1);   /* 12A -> 1A wraps */
    CHECK(nt_key_score(16, 17, NULL) > 0.8f);             /* 8A -> 8B relative */
    CHECK(nt_key_score(16, 4, NULL) < 0.2f);              /* 8A -> 2A: far */
    CHECK(nt_key_score(-1, 16, NULL) == 0.5f);
}

static void tempo(void)
{
    double p;
    int hd;
    CHECK(nt_bpm_score(124, 124, &p, &hd) > 0.99f && fabs(p) < 1e-9 && !hd);
    float s2 = nt_bpm_score(124, 126, &p, &hd);
    CHECK(s2 > 0.8f && p < 0);                            /* faster track pitched down */
    CHECK(nt_bpm_score(124, 140, NULL, NULL) < 0.01f);
    CHECK(nt_bpm_score(140, 70, &p, &hd) > 0.7f && hd);   /* half time */
    CHECK(nt_bpm_score(0, 120, NULL, NULL) == 0.45f);
}

static void identity(void)
{
    char n[128];
    CHECK(nt_ident("Daft Punk", "One More Time") == nt_ident("DAFT PUNK", "One More Time (Original Mix)"));
    CHECK(nt_ident("Beyoncé", "Cuff It") == nt_ident("Beyonce", "CUFF IT"));
    CHECK(nt_ident("Artist feat. Guest", "Song") == nt_ident("Artist", "Song"));
    CHECK(nt_ident("A", "Song feat. Guest") == nt_ident("A", "Song"));
    CHECK(nt_ident("A", "Song (Fisher Remix)") != nt_ident("A", "Song"));
    CHECK(nt_ident("A", "Song (feat. B) [Extended Mix]") == nt_ident("A", "Song"));
    CHECK(nt_ident("A & B", "Song") == nt_ident("A", "Song"));
    nt_normalize("Café del Mar (Energy 52 Remix)", n, sizeof n);
    CHECK(strcmp(n, "cafe del mar energy 52 remix") == 0);
}

static void pairmap(void)
{
    nt_pairmap m = { 0 };
    for (uint32_t i = 1; i < 5000; i++) nt_pair_add(&m, i, i * 7, 1);
    nt_pair_add(&m, 3, 21, 2);
    CHECK(nt_pair_get(&m, 3, 21) == 3.0f);
    CHECK(nt_pair_get(&m, 21, 3) == 0.0f);
    CHECK(nt_pair_get(&m, 4999, 4999 * 7) == 1.0f);
    nt_pair_set(&m, 3, 21, 1);
    CHECK(nt_pair_get(&m, 3, 21) == 3.0f);
    nt_pair_free(&m);
}

static void ranking(void)
{
    nt_lib L;
    CHECK(nt_lib_init(&L, 100) == 0);
    int a = nt_lib_add(&L, 0, 1, "Now", "DJ One", "", "Am", 124, 360);
    int good = nt_lib_add(&L, 0, 2, "Next Good", "DJ Two", "", "Em", 125, 360);   /* 9A, +0.8% */
    int key_bad = nt_lib_add(&L, 0, 3, "Clash", "DJ Three", "", "F#", 124, 360);  /* 2B */
    int far = nt_lib_add(&L, 0, 4, "Too Fast", "DJ Four", "", "Am", 150, 360);
    int known = nt_lib_add(&L, 0, 5, "Classic Follow", "DJ Five", "", "Dm", 122, 360); /* 7A, 1.6% */
    int dup = nt_lib_add(&L, 1, 77, "Now", "DJ One", "", "Am", 124, 360);           /* same song, USB 2 */
    CHECK(nt_lib_add(&L, 0, 2, "Next Good", "DJ Two", "", "Em", 125, 360) == good);  /* dedup */
    CHECK(L.n == 6);
    nt_lib_in_playlist(&L, a, 0, "Deep");
    nt_lib_in_playlist(&L, good, 0, "Deep");
    nt_lib_in_playlist(&L, key_bad, 1, "Peak");
    nt_pair_add(&L.pairs, L.t[a].ident, L.t[known].ident, 3);
    int tags[2] = { nt_tag_index(&L, "Deep House"), nt_tag_index(&L, "house") };
    int w[2] = { 100, 60 };
    nt_set_tags(&L, L.t[a].ident, tags, w, 2);
    nt_set_tags(&L, L.t[good].ident, tags, w, 2);

    nt_ctx c = { NT_FLOW_SMOOTH, 1, NULL };
    int out[NT_MAX_RESULTS];
    nt_reason why[NT_MAX_RESULTS];
    int n = nt_rank(&L, &c, a, 0, 5, out, why);
    CHECK(n >= 2);
    CHECK(out[0] == known);                               /* a transition you play wins */
    int pos_bad = -1, pos_far = -1, pos_dup = -1;
    for (int i = 0; i < n; i++) {
        if (out[i] == key_bad) pos_bad = i;
        if (out[i] == far) pos_far = i;
        if (out[i] == dup) pos_dup = i;
    }
    CHECK(pos_far < 0);                                   /* 21% off: never suggested */
    CHECK(pos_dup < 0);                                   /* the playing song itself */
    CHECK(pos_bad < 0 || pos_bad > 1);
    char s[96];
    for (int i = 0; i < n; i++) {
        nt_reason_str(&L, &why[i], a, out[i], s, sizeof s);
        printf("  next %d: %-16s %.3f  %s\n", i + 1, nt_str(&L, L.t[out[i]].title), why[i].total, s);
    }
    for (int i = 0; i < n; i++)
        if (out[i] == known) CHECK(why[i].pair_count == 3);

    /* excluded tracks never come back */
    uint8_t ex[100] = { 0 };
    ex[known] = 1;
    c.excluded = ex;
    n = nt_rank(&L, &c, a, 0, 5, out, why);
    for (int i = 0; i < n; i++) CHECK(out[i] != known);
    c.excluded = NULL;

    /* a chain never repeats a song and starts with a good first step */
    n = nt_chain(&L, &c, a, 0, 4, out, why);
    CHECK(n >= 2);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) CHECK(L.t[out[i]].ident != L.t[out[j]].ident);
    for (int i = 0; i < n; i++) {
        CHECK(L.t[out[i]].ident != L.t[a].ident);
        printf("  chain %d: %s\n", i + 1, nt_str(&L, L.t[out[i]].title));
    }

    /* flow: building up prefers the natively faster track */
    nt_reason r1, r2;
    nt_ctx up = { NT_FLOW_UP, 0, NULL }, down = { NT_FLOW_DOWN, 0, NULL };
    nt_score(&L, &up, a, 0, good, &r1);
    nt_score(&L, &down, a, 0, good, &r2);
    CHECK(r1.total > r2.total);
    nt_lib_free(&L);
}

/* The energy tabs: Build up and Cool down must not show the same first track. */
static void energy(void)
{
    nt_lib L;
    CHECK(nt_lib_init(&L, 16) == 0);
    int a = nt_lib_add(&L, 0, 1, "Now", "A", "", "Am", 124, 300);
    int faster = nt_lib_add(&L, 0, 2, "Faster", "B", "", "Em", 127, 300);    /* 9A, natively faster */
    int slower = nt_lib_add(&L, 0, 3, "Slower", "C", "", "Dm", 121, 300);    /* 7A, natively slower */
    int same = nt_lib_add(&L, 0, 4, "Same", "D", "", "Am", 124, 300);
    int out[NT_MAX_RESULTS];
    nt_reason why[NT_MAX_RESULTS];
    nt_ctx up = { NT_FLOW_UP, 0, NULL }, down = { NT_FLOW_DOWN, 0, NULL }, smooth = { NT_FLOW_SMOOTH, 0, NULL };
    CHECK(nt_rank(&L, &up, a, 0, 3, out, why) == 3 && out[0] == faster);
    CHECK(nt_rank(&L, &down, a, 0, 3, out, why) == 3 && out[0] == slower);
    CHECK(nt_rank(&L, &smooth, a, 0, 3, out, why) == 3 && out[0] == same);
    nt_lib_free(&L);
}

static void big(void)
{
    nt_lib L;
    CHECK(nt_lib_init(&L, NT_MAX_TRACKS) == 0);
    static const char *const K[] = { "Am", "Em", "Bm", "F#m", "C", "G", "D", "Dm", "Gm", "Cm" };
    char t[32], a[32];
    unsigned seed = 7;
    for (int i = 0; i < NT_MAX_TRACKS; i++) {
        seed = seed * 1103515245u + 12345u;
        snprintf(t, sizeof t, "Track %d", i);
        snprintf(a, sizeof a, "Artist %u", (seed >> 8) % 900);
        int ix = nt_lib_add(&L, 0, (uint32_t)i + 1, t, a, "", K[(seed >> 16) % 10],
                            100 + (seed >> 4) % 50, 300);
        nt_lib_in_playlist(&L, ix, (int)((seed >> 12) % 300), "pl");
    }
    CHECK(L.n == NT_MAX_TRACKS);
    CHECK(nt_lib_add(&L, 0, 999999, "x", "y", "", "", 120, 1) == -1);
    nt_ctx c = { 0, 1, NULL };
    int out[NT_MAX_RESULTS];
    nt_reason why[NT_MAX_RESULTS];
    CHECK(nt_rank(&L, &c, 5, 0, 10, out, why) == 10);
    for (int i = 1; i < 10; i++) CHECK(why[i - 1].total >= why[i].total);
    CHECK(nt_chain(&L, &c, 5, 0, 10, out, why) == 10);
    nt_lib_free(&L);
}

static void json(void)
{
    const char *doc = "{\"similartracks\":{\"track\":[{\"name\":\"Song \\u00e9\",\"match\":0.83,"
                      "\"artist\":{\"name\":\"X\"}},{\"name\":\"B\",\"match\":\"0.5\",\"artist\":{\"name\":\"Y\"}}]}}";
    mj_doc d;
    CHECK(mj_parse(&d, doc, strlen(doc)) == 0);
    int arr = mj_path(&d, 0, "similartracks.track");
    CHECK(arr >= 0 && mj_len(&d, arr) == 2);
    int e0 = mj_at(&d, arr, 0);
    char s[64];
    mj_str(&d, mj_get(&d, e0, "name"), s, sizeof s);
    CHECK(strcmp(s, "Song \xc3\xa9") == 0);
    CHECK(fabs(mj_num(&d, mj_get(&d, e0, "match")) - 0.83) < 1e-9);
    CHECK(fabs(mj_num(&d, mj_get(&d, mj_at(&d, arr, 1), "match")) - 0.5) < 1e-9);
    mj_str(&d, mj_path(&d, e0, "artist.name"), s, sizeof s);
    CHECK(strcmp(s, "X") == 0);
    CHECK(mj_get(&d, e0, "nope") < 0);
    mj_free(&d);
    CHECK(mj_parse(&d, "{\"a\":[1,2", 9) != 0);
    mj_free(&d);
    const char *one = "{\"toptags\":{\"tag\":{\"name\":\"house\",\"count\":100}}}";   /* object, not array */
    CHECK(mj_parse(&d, one, strlen(one)) == 0);
    int tg = mj_path(&d, 0, "toptags.tag");
    CHECK(mj_len(&d, tg) == 1 && mj_at(&d, tg, 0) == tg);
    mj_free(&d);
}

int main(void)
{
    keys();
    tempo();
    identity();
    pairmap();
    ranking();
    energy();
    big();
    json();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
