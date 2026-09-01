# Crystal 15.30 asset audit

- `appearances.dat` size: 4,568,934 bytes
- `appearances.dat` SHA-256: `ec29a245e364f190682aa3677ca04f657cb9a2076eca5fc931d039bab8c32887`
- migrated item definitions: 14,591
- unique migrated item IDs: 14,591
- Crystal item IDs in mapping: 14,591
- items allocated a new Canary/server ID: 45

## Safety status

`appearances.dat` is present and is treated as a binary protobuf asset. It is **not** rewritten by this audit. Modern Canary uses client appearance IDs separately from server item IDs; therefore an item-ID collision cannot be resolved by blindly changing protobuf appearance IDs.

## Items requiring explicit appearance-ID verification

The following migrated items received a newly allocated server ID and therefore must be checked against their Crystal client appearance before final client packaging:

- Crystal `3204` → Canary `54267` — your own dead body
- Crystal `5952` → Canary `54268` — scroll
- Crystal `21462` → Canary `54269` — red flame
- Crystal `21463` → Canary `54270` — blue flame
- Crystal `21464` → Canary `54271` — violet flame
- Crystal `21465` → Canary `54272` — yellow flame
- Crystal `28750` → Canary `54273` — your stash
- Crystal `31159` → Canary `54274` — grass
- Crystal `44012` → Canary `54275` — dead bakragore
- Crystal `44013` → Canary `54276` — dead bakragore
- Crystal `44014` → Canary `54277` — dead bakragore
- Crystal `44015` → Canary `54278` — dead murcion
- Crystal `44016` → Canary `54279` — dead murcion
- Crystal `44017` → Canary `54280` — dead murcion
- Crystal `44018` → Canary `54281` — dead Ichgahal
- Crystal `44019` → Canary `54282` — dead Ichgahal
- Crystal `44020` → Canary `54283` — dead Ichgahal
- Crystal `44021` → Canary `54284` — dead vemiath
- Crystal `44022` → Canary `54285` — dead vemiath
- Crystal `44023` → Canary `54286` — dead vemiath
- Crystal `44024` → Canary `54287` — dead chagorz
- Crystal `44025` → Canary `54288` — dead chagorz
- Crystal `44026` → Canary `54289` — dead chagorz
- Crystal `49656` → Canary `54290` — stairs
- Crystal `49657` → Canary `54291` — stairs
- Crystal `49658` → Canary `54292` — stairs
- Crystal `49659` → Canary `54293` — stairs
- Crystal `49660` → Canary `54294` — stairs
- Crystal `49661` → Canary `54295` — stairs
- Crystal `49678` → Canary `54296` — closed gate
- Crystal `49679` → Canary `54297` — closed gate
- Crystal `49680` → Canary `54298` — closed gate
- Crystal `49682` → Canary `54299` — open gate
- Crystal `49683` → Canary `54300` — open gate
- Crystal `49684` → Canary `54301` — closed gate
- Crystal `49685` → Canary `54302` — closed gate
- Crystal `49686` → Canary `54303` — closed gate
- Crystal `49687` → Canary `54304` — closed gate
- Crystal `49688` → Canary `54305` — open gate
- Crystal `49689` → Canary `54306` — open gate
- Crystal `50027` → Canary `54307` — dead arbaziloth
- Crystal `50028` → Canary `54308` — dead arbaziloth
- Crystal `50029` → Canary `54309` — dead arbaziloth
- Crystal `50102` → Canary `54310` — dead corpse
- Crystal `50183` → Canary `54311` — sai

## Outfit / mount / creature assets

The same `appearances.dat` contains the client appearance catalogue used by 15.x. Outfit, mount and creature appearance IDs must be decoded from the protobuf catalogue and matched to the imported server `lookType` values. No numeric ID is changed by this audit until that relationship is proven.
