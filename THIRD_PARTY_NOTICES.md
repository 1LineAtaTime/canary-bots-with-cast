# Third-Party Notices

This project (a fork of the Canary server) bundles or derives data from the
third-party sources listed below. The original upstream Canary code and all
modifications in this fork are distributed under **GPL-2.0** (see `LICENSE`).

## Upstream project

- **Canary** — https://github.com/opentibiabr/canary — GPL-2.0.
  This repository is a fork; all server code and modifications inherit GPL-2.0.

## Bundled bot data

The bot hunt routes, target definitions, and city-navigation waypoints shipped
in `database/bots/` were derived (parsed and transformed) from publicly
available community information and routes shared on community forums. Per-row
data-provenance fields were not included in the distributed data. Attribution to
the original authors where known; redistributors should verify the licensing of
any third-party-derived data they reuse.

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

## Acknowledgements

Some bot routes and ideas were adapted from community forum posts, including the
**Gesior** Thais bot forum post. Thanks to those community authors.

## Game client & web

This distribution does not bundle a game client or the MyAAC web framework. It
references them; install them from their own upstream projects under their own
licenses (see the setup documentation).
