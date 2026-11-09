/* SPDX-License-Identifier: GPL-3.0-only
 *
 * pdb.h: a reader for rekordbox's export.pdb (the "DeviceSQL" device library), from the
 * format's public analysis (Deep Symmetry, "Analyzing the rekordbox export database";
 * the crate-digger project's rekordbox_pdb.ksy). Pure C, no I/O: it reads a buffer the caller
 * loaded, checks every offset against the buffer, and calls back with the rows it finds.
 *
 * Mix Pilot uses it when the AZ-MODS library service has no library for a stick: the service
 * reads PIONEER/rekordbox/exportLibrary.db only, while rekordbox can keep the library in the
 * hidden .PIONEER/rekordbox folder, where export.pdb sits beside exportLibrary.db.
 */
#ifndef MIXPILOT_PDB_H
#define MIXPILOT_PDB_H

#include <stddef.h>
#include <stdint.h>

typedef struct pdb_track {
    uint32_t id;                          /* rekordbox track id */
    const char *title, *artist, *album, *genre, *key;   /* UTF-8, "" when none; valid in the callback */
    double bpm;                           /* 0 when not analyzed */
    int len_s;
    int rating;                           /* stars, 0..5 */
    int color;                            /* rekordbox color, 0 none */
} pdb_track;

typedef int (*pdb_track_fn)(const pdb_track *t, void *user);                 /* nonzero stops */
typedef int (*pdb_playlist_fn)(uint32_t id, uint32_t parent, const char *name, int folder, void *user);
typedef int (*pdb_entry_fn)(uint32_t playlist, uint32_t track, uint32_t index, void *user);

/* Reads the whole library: tracks first (with their artist, album, genre and key names), then
 * the playlist tree, then the playlist entries. Any callback may be NULL.
 * Returns the number of tracks reported, or -1 when the buffer is not an export.pdb. */
int pdb_read(const unsigned char *buf, size_t len, pdb_track_fn on_track, pdb_playlist_fn on_playlist,
             pdb_entry_fn on_entry, void *user);

#endif
