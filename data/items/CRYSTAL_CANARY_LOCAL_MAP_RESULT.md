# Crystal -> Canary item ID mapping result

Generated from the supplied Crystal Server and Canary archives.

- Crystal item definitions: **14,591**
- Canary item definitions: **14,060**
- Same ID + same name: **13,935**
- Crystal IDs free in Canary and preserved: **611**
- New IDs allocated: **45**
- Unique mapped Crystal IDs: **14,591**
- Maximum resulting ID: **54,311**

## Rules

1. Same Crystal ID + same normalized name in Canary: keep the ID.
2. Crystal ID unused by Canary: keep the Crystal ID.
3. If the ID is occupied, use a unique Canary item with the same normalized name when that target ID is still unused.
4. Otherwise allocate the next free ID.
5. Never assign two Crystal items to the same resulting ID.

The generated `items.xml` was produced locally from the supplied archives. The repository also contains a GitHub Actions mapper which regenerates the file from the public Crystal `items.xml` source.

**Important:** item IDs and client appearances are separate concerns in modern Canary; Canary uses `appearances.dat` for appearances rather than the old `items.otb` approach.
