# Crystal -> Canary migration

This directory contains the non-binary migration layer for importing Crystal Server data into this Canary fork.

## Scope

The migration is intentionally split into stages:

1. items
2. monsters
3. spells
4. client appearances / sprites
5. C++/Lua compatibility fixes

Do **not** replace Canary's C++ engine with Crystal's C++ source. The bot/cast implementation in this fork must remain intact.

## Item migration

Use `items_converter.py` against a Crystal `data/items/items.xml`. The converter preserves Canary entries and adds Crystal-only IDs. Existing IDs are not blindly renumbered: an explicit mapping file is required for an ID conflict.

## Monster migration

Monster Lua definitions are copied into the Canary global datapack only after their referenced `lookType`, item IDs and spell names have been checked against the target client/server data.

## Client assets

`Tibia.spr`, `Tibia.dat`, and modern `appearances*.dat` files are binary client assets. They must be handled separately from this text-data commit and must match the exact client protocol/version used by the server.

## Safety

This branch is a migration branch. Review the generated diff and build the server before merging it into `main`.
