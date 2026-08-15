# Crystal -> Canary monster migration

Monster definitions are registered by `Game.createMonsterType(name)`; there is no stable numeric monster ID to renumber like item IDs.

- Crystal monster Lua files scanned: 1803
- Crystal monster definitions: 1801
- Existing Canary definitions by name: 1655
- Imported/overwritten: 1801
- New monster definitions: 162
- Existing definitions refreshed from Crystal: 1639
- Loot item ID references remapped: 0
- raceId collisions detected: 3

## Coverage

Each imported definition retains Crystal values for name, lookType/outfit, HP, experience, speed, attacks, defenses, elements, immunities, loot, summons and flags.

## raceId collisions

- `Druid's Apparition`: raceId `1946`
- `Blue Butterfly`: raceId `227`
- `Butterfly`: raceId `213`
