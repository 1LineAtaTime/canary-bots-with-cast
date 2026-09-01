# Crystal -> Canary monster migration

Monster definitions are registered by `Game.createMonsterType(name)`; there is no stable numeric monster ID to renumber like item IDs.

- Crystal monster Lua files scanned: 1803
- Crystal monster definitions: 1801
- Existing Canary definitions by name: 1817
- Imported/overwritten: 1801
- New monster definitions: 0
- Existing definitions refreshed from Crystal: 1801
- Loot item ID references remapped: 0
- raceId collisions detected: 22

## Coverage

Each imported definition retains Crystal values for name, lookType/outfit, HP, experience, speed, attacks, defenses, elements, immunities, loot, summons and flags.

## raceId collisions

- `Bloated Man-Maggot`: raceId `2392`
- `Converter`: raceId `2379`
- `Darklight Construct`: raceId `2378`
- `Darklight Emitter`: raceId `2382`
- `Darklight Matter`: raceId `2380`
- `Darklight Source`: raceId `2398`
- `Darklight Striker`: raceId `2399`
- `Meandering Mushroom`: raceId `2376`
- `Mushroom`: raceId `0`
- `Mycobiontic Beetle`: raceId `2375`
- `Oozing Carcass`: raceId `2377`
- `Oozing Corpus`: raceId `2381`
- `Pillar of Dark Energy`: raceId `0`
- `Rotten Man-Maggot`: raceId `2393`
- `Sopping Carcass`: raceId `2396`
- `Sopping Corpus`: raceId `2397`
- `Walking Pillar`: raceId `2394`
- `Wandering Pillar`: raceId `2395`
- `Ghost Wolf`: raceId `1148`
- `Butterfly`: raceId `213`
- `Day Night Harpy`: raceId `2764`
- `Night Harpy`: raceId `2764`
