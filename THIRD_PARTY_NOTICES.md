# Third-Party Notices

This project (a fork of the Canary server) bundles or derives data from the
third-party sources listed below. The original upstream Canary code and all
modifications in this fork are distributed under **GPL-2.0** (see `LICENSE`).

## Upstream project

- **Canary** — https://github.com/opentibiabr/canary — GPL-2.0.
  This repository is a fork; all server code and modifications inherit GPL-2.0.

## Bundled bot data — community waypoint scripts

The bot hunt routes, target definitions, and city-navigation waypoints shipped in
`data/bot/authored/` were derived (parsed and transformed) from **publicly shared
community cavebot scripts** — waypoint files and routes published on OTServ and
Tibia community forums and in public script collections.

This data is the single largest third-party contribution to this project. The
original authors recorded these routes by hand, walking every corridor, and chose
to publish them for free. We are grateful, and we redistribute the transformed
result in the same spirit.

Per-row data-provenance fields (`source`, `source_file`) are generic in the
distributed data — they identified the local import batch, not an author, and
carried no attribution value. Attribution is therefore given here collectively.
**If you recognise your work in this data and would like specific credit, or
would like it removed, please open an issue.** Redistributors should verify the
licensing of any third-party-derived data they reuse.

Recording new routes for this server is done with the
[OTClient Redemption](https://github.com/opentibiabr/otclient) cavebot, whose
`.cfg` format the bundled importer (`tools/cfg_importer/`) reads directly.

## Bundled market price data

The market item-price reference data in
`database/bots/11_bot_market_item_prices.sql` is derived in part from the Tibia
community wiki (tibiawiki / `tibia.fandom.com`), which is licensed under
**Creative Commons Attribution-ShareAlike 3.0 (CC BY-SA 3.0)**:
https://creativecommons.org/licenses/by-sa/3.0/

Attribution: **Tibia Wiki / Fandom** (`tibia.fandom.com`) contributors. Per
CC BY-SA 3.0, this derived data is provided under the same ShareAlike terms.

## Bundled house content

House NPCs and house decoration items used by the bot house system are credited
to **Gunzodus** (community content). Used with thanks; redistributors should
verify the current upstream licensing/terms.

## Bundled web assets (world-map page)

The MyAAC "World Map" plugin under `deployment/web/plugins/worldmap/` bundles two
third-party assets:

- **Leaflet 1.9.4** — https://leafletjs.com — **BSD-2-Clause**,
  (c) 2010-2023 Volodymyr Agafonkin, (c) 2010-2011 CloudMade.
  Bundled unmodified as `static/leaflet.js` and `static/leaflet.css`. This is the
  repository's first vendored JavaScript dependency.

- **tibia-map-data** (per-floor minimap renders, `static/floors/floor-NN.png`) —
  https://github.com/tibiamaps/tibia-map-data — **MIT**. The page carries a
  visible attribution link to https://tibiamaps.io/.

Not bundled, and deliberately excluded from this repository (see `.gitignore`):
the world-map "Overview" background `map_big.jpg` is **CipSoft artwork** from
tibia.com. It is self-hosted on the private server only and is not redistributed
here.

## Acknowledgements

Some bot routes and ideas were adapted from community forum posts, including the
**Gesior** Thais bot forum post. Thanks to those community authors — see
"Bundled bot data" above.

The World Map page (`deployment/web/plugins/worldmap/`) was designed after
AzerothCore's player map — **DustinsAzerothMap**
(https://github.com/DustinHendrickson/DustinsAzerothMap, Dustin Hendrickson) and
the **azerothcore/playermap** it fixes (https://github.com/azerothcore/playermap,
originally by Dmitry Koterov, later maintained by Helias). No code was copied:
what we took is the approach — query the position columns the core already
persists, and plot them over a static map image. Thanks to those authors.

## Game client & web

This distribution does not bundle a game client or the MyAAC web framework. It
references them; install them from their own upstream projects under their own
licenses (see the setup documentation).
