# Crystal 15.30 -> Canary appearance migration

## Safety policy

- Existing Canary appearance IDs are preserved.
- No binary `appearances.dat` is overwritten by this step.
- Crystal IDs are not assumed to equal appearance IDs.
- Conflicting identities must receive a new free ID after binary comparison.

- Item mappings inspected: 14591
- appearances.dat files found: 1

- `data/items/appearances.dat` — 4568934 bytes — `ec29a245e364f190682aa3677ca04f657cb9a2076eca5fc931d039bab8c32887`

## Pending

Binary protobuf identity comparison is required before assigning new appearance IDs or merging outfit/mount/creature appearances.
