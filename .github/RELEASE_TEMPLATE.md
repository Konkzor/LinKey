# LinKey <YY.N>

> Release groupée du projet LinKey : firmware, carte électronique et boîtier 3D dans un état cohérent et testé ensemble.
>
> Tag : `<YY.N>` — année.numéro de release (ex. `26.1` = première release de 2026). Pas de cadence fixe : une nouvelle release est publiée à chaque jalon utile.

## Composants

| Composant | Version | Source |
|-----------|---------|--------|
| Firmware  | <X.Y> | [`Firmware/`](../../tree/<YY.N>/Firmware) (`FW_VERSION` dans [`Firmware/main/mqtt_manager.c`](../../blob/<YY.N>/Firmware/main/mqtt_manager.c)) |
| PCB       | <vX.Y> | tag git [`pcb-<vX.Y>`](../../releases/tag/pcb-<vX.Y>) |
| Boîtier   | <vN>   | [`CAO/Linkey-boitier-<vN>.step`](../../blob/<YY.N>/CAO/Linkey-boitier-<vN>.step) |

## Téléchargements

Pas de binaire firmware fourni : il doit être compilé localement avec vos credentials WiFi/MQTT (voir [`Firmware/README.md`](../../blob/<YY.N>/Firmware/README.md)).

| Fichier | Description |
|---------|-------------|
| `Linkey-boitier-<vN>-top-et-bottom-bicouleur.3mf` | Boîtier bi-couleur, imprimantes multi-matériaux (AMS) |
| `Linkey-boitier-<vN>-top.stl` | Boîtier — face top, imprimante mono-extrudeur |
| `Linkey-boitier-<vN>-bottom.stl` | Boîtier — face bottom, imprimante mono-extrudeur |
| `LinKey-pcb-<vX.Y>-fab.zip` | Fichiers de fabrication PCB (Gerbers + drill + BOM + positions) — artefact `LinKey-pcb-<vX.Y>-fab` du job CI sur le commit tagué |

## Nouveautés depuis la version précédente

- _Liste des changements notables par composant._

## Compatibilité

- Firmware <X.Y> compatible avec PCB <vX.Y>.
- Boîtier <vN> conçu pour PCB <vX.Y>.

## Documentation

- [README principal](../../tree/<YY.N>)
- [Guide firmware](../../blob/<YY.N>/Firmware/README.md) — compilation, flashage, MQTT, Home Assistant
- [Guide PCB](../../blob/<YY.N>/PCB/README.md) — composants, brochage, fabrication
- [Guide boîtier](../../blob/<YY.N>/CAO/README.md) — Onshape, paramètres d'impression

---

<sub>Remplacer les `<YY.N>`, `<X.Y>`, `<vX.Y>`, `<vN>` par les valeurs réelles avant publication. Les liens `../../tree/<YY.N>/...` et `../../blob/<YY.N>/...` résolvent correctement lorsqu'ils sont rendus sur la page Releases.</sub>
