/* SPDX-License-Identifier: GPL-3.0-only
 *
 * minijson.h: a small, strict, non-evaluating JSON reader for the web answers Mix Pilot reads
 * (Last.fm). The document is parsed once into a flat token array; values are looked up by
 * key or path and copied out. Nothing is ever executed, and depth and size are capped.
 */
#ifndef MIXPILOT_MINIJSON_H
#define MIXPILOT_MINIJSON_H

#include <stddef.h>

enum { MJ_OBJ = 1, MJ_ARR, MJ_STR, MJ_NUM, MJ_LIT };

typedef struct {
    int type;
    int start, end;     /* byte range in the source (strings: inside the quotes) */
    int first, next;    /* first child, next sibling; -1 = none. Objects: key, value, key, ... */
    int count;          /* children (objects: members) */
} mj_tok;

typedef struct {
    mj_tok *t;
    int n, cap;
    const char *s;
    size_t len;
} mj_doc;

int mj_parse(mj_doc *d, const char *s, size_t len);   /* 0 = ok; the root is token 0 */
void mj_free(mj_doc *d);
int mj_get(const mj_doc *d, int obj, const char *key);    /* member value, -1 none */
int mj_path(const mj_doc *d, int from, const char *path); /* "a.b.c", -1 none */
/* Array length; an object counts as a one-element array (Last.fm sends a lone result that way). */
int mj_len(const mj_doc *d, int arr);
int mj_at(const mj_doc *d, int arr, int i);
double mj_num(const mj_doc *d, int tok);                  /* numbers and numeric strings; 0 else */
int mj_str(const mj_doc *d, int tok, char *buf, size_t n); /* decoded UTF-8, returns length */

#endif
