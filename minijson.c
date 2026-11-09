/* SPDX-License-Identifier: GPL-3.0-only
 * minijson.c: see minijson.h. */
#include "minijson.h"

#include <stdlib.h>
#include <string.h>

#define MJ_MAX_DEPTH 48
#define MJ_MAX_TOKENS 200000

typedef struct {
    mj_doc *d;
    size_t p;
} mj_p;

static void ws(mj_p *p)
{
    while (p->p < p->d->len) {
        char c = p->d->s[p->p];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        p->p++;
    }
}

static int add(mj_p *p, int type, int start)
{
    mj_doc *d = p->d;
    if (d->n >= MJ_MAX_TOKENS) return -1;
    if (d->n == d->cap) {
        int cap = d->cap ? d->cap * 2 : 256;
        mj_tok *t = realloc(d->t, sizeof *t * (size_t)cap);
        if (!t) return -1;
        d->t = t;
        d->cap = cap;
    }
    mj_tok *t = &d->t[d->n];
    t->type = type;
    t->start = start;
    t->end = start;
    t->first = t->next = -1;
    t->count = 0;
    return d->n++;
}

static int value(mj_p *p, int depth);

static int string(mj_p *p)
{
    const char *s = p->d->s;
    if (p->p >= p->d->len || s[p->p] != '"') return -1;
    int t = add(p, MJ_STR, (int)++p->p);
    if (t < 0) return -1;
    while (p->p < p->d->len && s[p->p] != '"') {
        if ((unsigned char)s[p->p] < 0x20) return -1;
        if (s[p->p] == '\\') p->p++;
        p->p++;
    }
    if (p->p >= p->d->len) return -1;
    p->d->t[t].end = (int)p->p++;
    return t;
}

static void link(mj_p *p, int parent, int *last, int child)
{
    if (*last < 0) p->d->t[parent].first = child;
    else p->d->t[*last].next = child;
    *last = child;
}

static int value(mj_p *p, int depth)
{
    const char *s = p->d->s;
    if (depth > MJ_MAX_DEPTH) return -1;
    ws(p);
    if (p->p >= p->d->len) return -1;
    char c = s[p->p];
    if (c == '{' || c == '[') {
        int obj = c == '{';
        int t = add(p, obj ? MJ_OBJ : MJ_ARR, (int)p->p);
        if (t < 0) return -1;
        p->p++;
        int last = -1;
        ws(p);
        if (p->p < p->d->len && s[p->p] == (obj ? '}' : ']')) {
            p->d->t[t].end = (int)++p->p;
            return t;
        }
        for (;;) {
            if (obj) {
                ws(p);
                int k = string(p);
                if (k < 0) return -1;
                link(p, t, &last, k);
                ws(p);
                if (p->p >= p->d->len || s[p->p] != ':') return -1;
                p->p++;
            }
            int v = value(p, depth + 1);
            if (v < 0) return -1;
            link(p, t, &last, v);
            p->d->t[t].count++;
            ws(p);
            if (p->p >= p->d->len) return -1;
            if (s[p->p] == ',') { p->p++; continue; }
            if (s[p->p] == (obj ? '}' : ']')) { p->d->t[t].end = (int)++p->p; return t; }
            return -1;
        }
    }
    if (c == '"') return string(p);
    int t = add(p, (c == '-' || (c >= '0' && c <= '9')) ? MJ_NUM : MJ_LIT, (int)p->p);
    if (t < 0) return -1;
    if (p->d->t[t].type == MJ_LIT) {
        static const char *const lit[] = { "true", "false", "null" };
        int ok = 0;
        for (int i = 0; i < 3 && !ok; i++) {
            size_t l = strlen(lit[i]);
            if (p->p + l <= p->d->len && memcmp(s + p->p, lit[i], l) == 0) { p->p += l; ok = 1; }
        }
        if (!ok) return -1;
    } else {
        while (p->p < p->d->len && strchr("+-0123456789.eE", s[p->p])) p->p++;
    }
    p->d->t[t].end = (int)p->p;
    return t;
}

int mj_parse(mj_doc *d, const char *s, size_t len)
{
    memset(d, 0, sizeof *d);
    d->s = s;
    d->len = len;
    mj_p p = { d, 0 };
    if (len >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB) p.p = 3;
    if (value(&p, 0) != 0) return -1;
    return 0;
}

void mj_free(mj_doc *d)
{
    free(d->t);
    memset(d, 0, sizeof *d);
}

static int key_is(const mj_doc *d, int k, const char *key, size_t kl)
{
    const mj_tok *t = &d->t[k];
    return (size_t)(t->end - t->start) == kl && memcmp(d->s + t->start, key, kl) == 0;
}

static int get_n(const mj_doc *d, int obj, const char *key, size_t kl)
{
    if (obj < 0 || obj >= d->n || d->t[obj].type != MJ_OBJ) return -1;
    for (int k = d->t[obj].first; k >= 0; k = d->t[d->t[k].next].next) {
        int v = d->t[k].next;
        if (v < 0) return -1;
        if (key_is(d, k, key, kl)) return v;
    }
    return -1;
}

int mj_get(const mj_doc *d, int obj, const char *key)
{
    return get_n(d, obj, key, strlen(key));
}

int mj_path(const mj_doc *d, int from, const char *path)
{
    int cur = from;
    while (cur >= 0 && *path) {
        const char *dot = strchr(path, '.');
        size_t l = dot ? (size_t)(dot - path) : strlen(path);
        cur = get_n(d, cur, path, l);
        path += l + (dot ? 1 : 0);
    }
    return cur;
}

int mj_len(const mj_doc *d, int a)
{
    if (a < 0 || a >= d->n) return 0;
    if (d->t[a].type == MJ_ARR) return d->t[a].count;
    return d->t[a].type == MJ_OBJ ? 1 : 0;
}

int mj_at(const mj_doc *d, int a, int i)
{
    if (a < 0 || a >= d->n || i < 0) return -1;
    if (d->t[a].type == MJ_OBJ) return i == 0 ? a : -1;
    if (d->t[a].type != MJ_ARR) return -1;
    int c = d->t[a].first;
    while (c >= 0 && i-- > 0) c = d->t[c].next;
    return c;
}

double mj_num(const mj_doc *d, int tok)
{
    if (tok < 0 || tok >= d->n) return 0;
    const mj_tok *t = &d->t[tok];
    if (t->type != MJ_NUM && t->type != MJ_STR) return 0;
    char buf[48];
    size_t l = (size_t)(t->end - t->start);
    if (l >= sizeof buf) return 0;
    memcpy(buf, d->s + t->start, l);
    buf[l] = '\0';
    return strtod(buf, NULL);
}

static int hex4(const char *s, unsigned *v)
{
    *v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        *v <<= 4;
        if (c >= '0' && c <= '9') *v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') *v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') *v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    return 0;
}

int mj_str(const mj_doc *d, int tok, char *buf, size_t n)
{
    if (!n) return 0;
    buf[0] = '\0';
    if (tok < 0 || tok >= d->n || d->t[tok].type != MJ_STR) return 0;
    const char *s = d->s + d->t[tok].start, *e = d->s + d->t[tok].end;
    size_t o = 0;
    while (s < e && o + 1 < n) {
        unsigned c = (unsigned char)*s++;
        if (c == '\\' && s < e) {
            char x = *s++;
            switch (x) {
            case 'n': case 'r': case 't': case 'b': case 'f': c = ' '; break;
            case 'u':
                if (e - s < 4 || hex4(s, &c)) { c = '?'; break; }
                s += 4;
                if (c >= 0xD800 && c < 0xDC00 && e - s >= 6 && s[0] == '\\' && s[1] == 'u') {
                    unsigned lo;
                    if (!hex4(s + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                        s += 6;
                    }
                }
                break;
            default: c = (unsigned char)x; break;
            }
        }
        if (c < 0x20) c = ' ';
        if (c < 0x80) buf[o++] = (char)c;
        else if (c < 0x800 && o + 2 < n) {
            buf[o++] = (char)(0xC0 | (c >> 6));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000 && o + 3 < n) {
            buf[o++] = (char)(0xE0 | (c >> 12));
            buf[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c >= 0x10000 && o + 4 < n) {
            buf[o++] = (char)(0xF0 | (c >> 18));
            buf[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            buf[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        } else break;
    }
    buf[o] = '\0';
    return (int)o;
}
