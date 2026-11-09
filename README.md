# Mix Pilot

**Suggestions de morceau suivant, directement sur la XDJ-AZ.** Un mod communautaire pour
[AZ-MODS](https://github.com/Kyle-Hosman/xdj-az-mods).

Mix Pilot regarde ce que jouent les decks, retrouve le morceau en cours dans la bibliothèque
rekordbox de tes clés USB, et affiche les morceaux qui s'enchaînent le mieux derrière :

- dans un **panneau** sur les écrans BROWSE et SEARCH (là où tu choisis le prochain morceau) ;
- sur une **page** du Mod Menu (*Community Mods > Installed > Mix Pilot*).

Deux modes (touche le panneau pour passer de l'un à l'autre) :

| Mode | Ce que tu vois |
|---|---|
| **Next tracks** | les 4, 5, 8 ou 10 meilleurs morceaux à jouer juste après, du meilleur au moins bon |
| **Chain** | un plan ordonné : 1 → 2 → 3 → …, chaque morceau s'enchaînant bien avec le suivant (recherche en faisceau) |

Pour chaque suggestion : titre, artiste, BPM, clé Camelot, une barre de couleur (vert = très
bon, jaune = correct, orange = possible, **bleu = enchaînement déjà joué**), la raison
(`8A>9A  -0.8%  deep house  mixed 3x`) et **la playlist où le trouver** (`in Deep House 2024`).

## Comment il note un enchaînement

| Signal | Poids | D'où ça vient |
|---|---|---|
| Tonalité | 24 % | Roue de Camelot : même clé, ±1, relatif majeur/mineur, boost d'énergie (+2, +7)… |
| Tempo | 24 % | Écart de pitch nécessaire, demi-tempo et double tempo compris |
| Style | 18 % | Tags de genre Last.fm (en ligne) + les playlists que les deux morceaux partagent (hors ligne, pondérées : une playlist « tout » compte peu) |
| Enchaînements réels | 22 % | Ce que **toi** tu as enchaîné (appris sur le deck) + un fichier **communautaire** |
| Similarité | 12 % | « Similar tracks » de Last.fm : les auditeurs de A écoutent aussi B |

Réglage **Energy** : *Smooth*, *Build up* (favorise un tempo plus rapide et une clé qui monte) ou
*Cool down* (l'inverse). Les morceaux chargés sur les autres decks et ceux déjà joués ce soir
ne sont jamais proposés.

### Les enchaînements « connus »

C'est l'idée de Pulse DJ (des millions d'enchaînements joués en soirée), à l'échelle d'un deck :

1. **Tes propres sets.** Quand un morceau a joué 40 secondes après un autre, Mix Pilot note
   l'enchaînement dans `my-transitions.tsv` sur ta clé. La prochaine fois, il remonte en tête
   (en bleu, « mixed 2x »).
2. **La communauté.** `assets/pairings.tsv` (livré avec le mod) et un fichier partagé optionnel
   (`pairings_url` dans `config.txt`, téléchargé en HTTPS) utilisent **le même format** que
   `my-transitions.tsv` : tu peux partager tes enchaînements en publiant ce fichier, et
   récupérer ceux des autres.
3. **Last.fm.** Les « similar tracks » reflètent ce que les gens écoutent ensemble.

Format (une ligne par enchaînement, séparateur tabulation) :

```
Artiste A	Titre A	Artiste B	Titre B	nombre	date
```

Les titres sont comparés en souplesse : `(Original Mix)`, `feat. X`, la casse et les accents ne
comptent pas ; un remix nommé (`(Fisher Remix)`) reste un morceau à part.

## Installation

### 1. Construire le paquet `.azmod`

Le mod est un `.so` aarch64 compilé dans le conteneur Docker du SDK. Deux façons :

**Par GitHub (sans rien installer).** Le dépôt est
[christopheb38/mix-pilot](https://github.com/christopheb38/mix-pilot). Pour publier une version :

```bash
git tag mix-pilot-0.1.0 && git push origin mix-pilot-0.1.0
```

Le workflow `.github/workflows/release.yml` compile, valide et publie le `.azmod` en release,
et ajoute l'entrée dans `azmods-repo.json`. Sur le deck : *Mod Menu > Community Mods >
Repositories > Add* → `christopheb38/mix-pilot`, puis *Browse* → *Mix Pilot* → *Install*.

**En local** (Docker Desktop + le [SDK](https://github.com/Kyle-Hosman/az-mods-sdk)) :

```bash
<sdk>/tools/azmod-pack build .
```

```bash
<sdk>/tools/azmod-pack package . --out dist
```

Copie ensuite le `.azmod` sur la clé comme expliqué dans le guide du SDK (*Testing on your
deck*) ; le deck demande **New mod found: allow?**.

### 2. Activer Last.fm (recommandé)

1. Crée une clé API gratuite : <https://www.last.fm/api/account/create>.
2. Au premier démarrage, Mix Pilot crée
   `MODS/community-mods/mix-pilot/data/config.txt` sur ta clé. Ouvre-le sur ton ordinateur :

   ```
   lastfm_api_key=ta_clé_ici
   pairings_url=
   ```

3. Remets la clé dans le deck, puis *Mod Menu > Mix Pilot > Reload library*.

Sans clé, sans WiFi ou sans libcurl sur le deck, Mix Pilot marche **hors ligne** (tonalité,
tempo, playlists, tes enchaînements, fichier communautaire). Les réponses de Last.fm sont mises
en cache sur la clé (`lastfm-cache.tsv`), donc un morceau déjà vu reste enrichi hors ligne en
soirée.

## Réglages (Mod Menu)

| Ligne | Effet |
|---|---|
| Panel | Right / Left / Off. Appui long sur le panneau : le replier |
| Mode | Next tracks / Chain |
| Tracks shown | 4, 5, 8, 10 |
| Energy | Smooth / Build up / Cool down |
| Follow | Auto (le dernier deck lancé, après 6 s de lecture) ou un deck fixe |
| Avoid same artist | baisse un peu les autres morceaux du même artiste |
| Learn my transitions | enregistre tes enchaînements |
| Online (Last.fm) | coupe tout accès réseau |
| Suggestions | la page plein écran |
| Reload library | relit la clé, `config.txt` et les fichiers d'enchaînements |

## Développement

```bash
make engine-test          # 80 tests du moteur et du lecteur JSON (ASan + UBSan)
```

```bash
make sim AZ_SDK_INCLUDE=<sdk>/include      # simulation complète : faux deck, ThreadSanitizer
```

```bash
make test AZ_SDK=<sdk> AZ_SDK_INCLUDE=<sdk>/include   # harnais officiel du SDK, 3 passes
```

| Fichier | Rôle |
|---|---|
| `engine.c/.h` | le moteur : Camelot, tempo, style, paires, classement, chaîne (C pur, sans I/O) |
| `mix-pilot.c` | le mod : decks, lecture de la bibliothèque, apprentissage, panneau, page, réglages |
| `online.c/.h` | HTTPS via la libcurl du deck (dlopen) et l'API Last.fm |
| `minijson.c/.h` | lecteur JSON strict, jamais évalué |
| `tests/` | tests du moteur et simulation de bout en bout |

Threads, conformément aux règles du SDK : le fil UI ne fait ni fichier ni réseau ; un thread
« worker » lit la bibliothèque, écrit les fichiers et calcule ; un thread « net » parle à
Last.fm (au plus ~5 requêtes/s, pause de 30 s si le réseau tombe).

## À savoir / limites (v0.1, bêta)

- **Pas encore testé sur un vrai deck.** Le moteur, la simulation et le harnais du SDK passent,
  mais l'emplacement du panneau (x = droite ou gauche, y = 90, 470 px de large) est à ajuster
  selon ce qu'il cache sur l'écran BROWSE.
- Le deck ne fournit pas la tonalité du morceau en cours : elle vient de la bibliothèque
  rekordbox, donc un morceau joué hors bibliothèque n'a pas de suggestions.
- Le mod ne peut pas charger un morceau tout seul dans un deck (pas d'API pour ça) : il te dit
  où le trouver.
- Le genre rekordbox n'est pas exposé par l'API des mods ; le style vient de Last.fm et des
  playlists. Lire le genre directement dans `export.pdb` est une piste pour la v0.2.
- Change l'`id` avant de publier si `mix-pilot` est déjà pris dans le Mod Index (un id ne doit
  plus jamais changer ensuite ; préfixe-le par ton nom GitHub, par ex. `cbrutti-mix-pilot`).

## Licence

GPL-3.0-only, comme AZ-MODS et tous les mods communautaires. Voir `LICENSE`.
