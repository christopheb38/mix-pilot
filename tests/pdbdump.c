/* SPDX-License-Identifier: GPL-3.0-only
 * pdbdump: print what pdb.c reads from an export.pdb (a development tool).
 *   make pdbdump && out/pdbdump path/to/export.pdb */
#include <stdio.h>
#include <stdlib.h>

#include "../pdb.h"

static int ntr, npl, nfold, nent, nbpm, nkey, ngenre;

static int tr(const pdb_track *t, void *u)
{
    (void)u;
    if (ntr < 12) printf("  #%u  %-34.34s | %-20.20s | %6.2f | %-4s | %-14.14s | %ds\n", t->id, t->title,
                         t->artist, t->bpm, t->key, t->genre, t->len_s);
    ntr++;
    nbpm += t->bpm > 0;
    nkey += t->key[0] != 0;
    ngenre += t->genre[0] != 0;
    return 0;
}

static int pl(uint32_t id, uint32_t parent, const char *name, int folder, void *u)
{
    (void)u;
    if (npl + nfold < 15) printf("  playlist %u (parent %u)%s: %s\n", id, parent, folder ? " [folder]" : "", name);
    if (folder) nfold++;
    else npl++;
    return 0;
}

static int en(uint32_t p, uint32_t t, uint32_t i, void *u)
{
    (void)p; (void)t; (void)i; (void)u;
    nent++;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: pdbdump export.pdb\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 1; }
    fclose(f);
    int r = pdb_read(b, (size_t)n, tr, pl, en, NULL);
    printf("pdb_read -> %d\ntracks %d (bpm %d, key %d, genre %d), playlists %d, folders %d, entries %d\n", r, ntr,
           nbpm, nkey, ngenre, npl, nfold, nent);
    free(b);
    return r < 0;
}
