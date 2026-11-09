/* SPDX-License-Identifier: GPL-3.0-only
 *
 * i18n.h: Mix Pilot's on-screen text in English, French, Spanish, German and Italian.
 * Included once, by mix-pilot.c. Add a language: a name in LANG_NAMES and one column per row.
 */
#ifndef MIXPILOT_I18N_H
#define MIXPILOT_I18N_H

enum { LANG_EN = 0, LANG_FR, LANG_ES, LANG_DE, LANG_IT, LANG_N };
static const char *const LANG_NAMES[LANG_N] = { "English", "Francais", "Espanol", "Deutsch", "Italiano" };

enum {
    T_SHOW, T_RELOAD, T_PANEL, T_MODE, T_COUNT, T_FLOW, T_REF, T_ARTIST, T_LEARN, T_ONLINE, T_LANG,
    T_RIGHT, T_LEFT, T_HIDDEN, T_OFF, T_ON, T_NEXT, T_CHAIN, T_SMOOTH, T_UP, T_DOWN, T_AUTO, T_DECK,
    T_HDR_NEXT, T_HDR_CHAIN, T_READING, T_NOLIB, T_PLAY, T_NOTIN, T_AFTER, T_PLAN, T_MIXED, T_SIMILAR,
    T_IN, T_TRACKS, T_KNOWN, T_NET_KEY, T_NET_OFFLINE, T_NET_ON, T_NET_FAIL, T_NET_DISABLED, T_BANNER,
    T_LOVE, T_SUGG_TITLE, T_D_NOUSB, T_D_PDB, T_D_HUB, T_D_REFUSED, T_D_NOEXPORT, T_N
};

/* Plain ASCII only: the deck's font draws no accented letters (boxes or wrong glyphs). */
static const char *const TR[T_N][LANG_N] = {
    [T_SHOW] = { "Show suggestions  >", "Voir les suggestions  >", "Ver sugerencias  >",
                 "Vorschlaege anzeigen  >", "Mostra suggerimenti  >" },
    [T_RELOAD] = { "Reload library", "Relire la bibliotheque", "Recargar biblioteca",
                   "Bibliothek neu laden", "Ricarica libreria" },
    [T_PANEL] = { "Panel", "Panneau", "Panel", "Panel", "Pannello" },
    [T_MODE] = { "Mode", "Mode", "Modo", "Modus", "Modalita" },
    [T_COUNT] = { "Tracks shown", "Morceaux affiches", "Temas mostrados", "Angezeigte Tracks", "Brani mostrati" },
    [T_FLOW] = { "Energy", "Energie", "Energia", "Energie", "Energia" },
    [T_REF] = { "Follow", "Suivre", "Seguir", "Folgen", "Segui" },
    [T_ARTIST] = { "Avoid same artist", "Eviter le meme artiste", "Evitar mismo artista",
                   "Gleichen Artist meiden", "Evita stesso artista" },
    [T_LEARN] = { "Learn my transitions", "Apprendre mes enchainements", "Aprender mis transiciones",
                  "Meine Uebergaenge lernen", "Impara i miei passaggi" },
    [T_ONLINE] = { "Online (Last.fm)", "En ligne (Last.fm)", "En linea (Last.fm)", "Online (Last.fm)", "Online (Last.fm)" },
    [T_LANG] = { "Language", "Langue", "Idioma", "Sprache", "Lingua" },
    [T_RIGHT] = { "Right", "Droite", "Derecha", "Rechts", "Destra" },
    [T_LEFT] = { "Left", "Gauche", "Izquierda", "Links", "Sinistra" },
    [T_HIDDEN] = { "Hidden", "Masque", "Oculto", "Aus", "Nascosto" },
    [T_OFF] = { "Off", "Non", "No", "Aus", "No" },
    [T_ON] = { "On", "Oui", "Si", "An", "Si" },
    [T_NEXT] = { "Next tracks", "Morceaux suivants", "Siguientes", "Naechste Tracks", "Brani successivi" },
    [T_CHAIN] = { "Chain", "Enchainement", "Cadena", "Kette", "Catena" },
    [T_SMOOTH] = { "Smooth", "Stable", "Estable", "Gleich", "Stabile" },
    [T_UP] = { "Build up", "Monter", "Subir", "Steigern", "Salire" },
    [T_DOWN] = { "Cool down", "Descendre", "Bajar", "Runter", "Scendere" },
    [T_AUTO] = { "Auto", "Auto", "Auto", "Auto", "Auto" },
    [T_DECK] = { "Deck %d", "Deck %d", "Deck %d", "Deck %d", "Deck %d" },
    [T_HDR_NEXT] = { "NEXT", "SUIVANTS", "SIGUIENTES", "NAECHSTE", "SUCCESSIVI" },
    [T_HDR_CHAIN] = { "CHAIN", "ENCHAINEMENT", "CADENA", "KETTE", "CATENA" },
    [T_READING] = { "Reading the library...", "Lecture de la bibliotheque...", "Leyendo la biblioteca...",
                    "Bibliothek wird gelesen...", "Lettura della libreria..." },
    [T_NOLIB] = { "No rekordbox library yet, retrying...", "Pas encore de bibliotheque, nouvel essai...",
                  "Aun sin biblioteca, reintentando...", "Noch keine Bibliothek, neuer Versuch...", "Ancora nessuna libreria, nuovo tentativo..." },
    [T_PLAY] = { "Play a track to get suggestions", "Lance un morceau pour avoir des suggestions",
                 "Pon un tema para ver sugerencias", "Spiel einen Track fuer Vorschlaege", "Avvia un brano per i suggerimenti" },
    [T_NOTIN] = { "Deck %d: track not in the USB library", "Deck %d : morceau absent de la cle USB",
                  "Deck %d: tema fuera de la biblioteca USB", "Deck %d: Track nicht in der USB-Bibliothek", "Deck %d: brano non nella libreria USB" },
    [T_AFTER] = { "After", "Apres", "Tras", "Nach", "Dopo" },
    [T_PLAN] = { "Plan after", "Plan apres", "Plan tras", "Plan nach", "Piano dopo" },
    [T_MIXED] = { "mixed %dx", "mixe %dx", "mezclado %dx", "%dx gemixt", "mixato %dx" },
    [T_SIMILAR] = { "similar %d%%", "similaire %d%%", "similar %d%%", "aehnlich %d%%", "simile %d%%" },
    [T_IN] = { "in", "dans", "en", "in", "in" },
    [T_TRACKS] = { "%d tracks", "%d morceaux", "%d temas", "%d Tracks", "%d brani" },
    [T_KNOWN] = { "%u transitions known", "%u enchainements connus", "%u transiciones conocidas",
                  "%u bekannte Uebergaenge", "%u passaggi noti" },
    [T_NET_KEY] = { "Last.fm: add a key in config.txt", "Last.fm : ajoute une cle dans config.txt",
                    "Last.fm: anade una clave en config.txt", "Last.fm: Schluessel in config.txt", "Last.fm: aggiungi una chiave in config.txt" },
    [T_NET_OFFLINE] = { "offline", "hors ligne", "sin conexion", "offline", "offline" },
    [T_NET_ON] = { "Last.fm on", "Last.fm actif", "Last.fm activo", "Last.fm an", "Last.fm attivo" },
    [T_NET_FAIL] = { "Last.fm unreachable", "Last.fm injoignable", "Last.fm inaccesible", "Last.fm nicht erreichbar", "Last.fm non raggiungibile" },
    [T_NET_DISABLED] = { "online off", "en ligne desactive", "en linea desactivado", "online aus", "online disattivato" },
    [T_BANNER] = { "Mix Pilot: reading the library again", "Mix Pilot : relecture de la bibliotheque",
                   "Mix Pilot: releyendo la biblioteca", "Mix Pilot: Bibliothek wird neu gelesen", "Mix Pilot: rilettura della libreria" },
    [T_LOVE] = { "Love Mix Pilot? Follow us on Instagram!", "Vous aimez Mix Pilot ? Suivez-nous sur Instagram !",
                 "Te gusta Mix Pilot? Siguenos en Instagram!",
                 "Gefaellt dir Mix Pilot? Folge uns auf Instagram!", "Ti piace Mix Pilot? Seguici su Instagram!" },
    [T_SUGG_TITLE] = { "Mix Pilot - Suggestions", "Mix Pilot - Suggestions", "Mix Pilot - Sugerencias",
                       "Mix Pilot - Vorschlaege", "Mix Pilot - Suggerimenti" },
    [T_D_NOUSB] = { "The deck sees no USB stick", "Le deck ne voit aucune cle USB",
                    "El deck no ve ninguna memoria USB", "Das Deck sieht keinen USB-Stick", "Il deck non vede nessuna chiavetta USB" },
    [T_D_PDB] = { "No exportLibrary.db: export with Device Library Plus",
                  "Pas d'exportLibrary.db : exporte en Device Library Plus",
                  "Sin exportLibrary.db: exporta con Device Library Plus",
                  "Keine exportLibrary.db: mit Device Library Plus exportieren", "Nessun exportLibrary.db: esporta con Device Library Plus" },
    [T_D_HUB] = { "USB port not recognized: plug the stick straight in",
                  "Port USB non reconnu : branche la cle en direct",
                  "Puerto USB no reconocido: conecta la memoria directamente",
                  "USB-Port nicht erkannt: Stick direkt einstecken", "Porta USB non riconosciuta: collega la chiavetta direttamente" },
    [T_D_REFUSED] = { "Library refused by the deck (codes below)", "Bibliotheque refusee par le deck (codes en bas)",
                      "Biblioteca rechazada por el deck (codigos abajo)",
                      "Bibliothek vom Deck abgelehnt (Codes unten)", "Libreria rifiutata dal deck (codici sotto)" },
    [T_D_NOEXPORT] = { "USB stick found, but no rekordbox export on it",
                       "Cle USB trouvee, mais sans export rekordbox",
                       "Memoria USB encontrada, pero sin exportacion rekordbox",
                       "USB-Stick gefunden, aber ohne rekordbox-Export",
                       "Chiavetta USB trovata, ma senza export rekordbox" },
};

#endif
