# Scenarios

Scenario files (`*.steps`) are **not tracked in git**: each server setup writes its own, since they
name local characters, bots, coordinates and passwords. Only `example_*.steps` and this README are
committed; everything else in this folder is ignored (see the repo `.gitignore`).

## Write your own
1. Copy `example_smoke.steps` to `my_test.steps` in this folder.
2. Edit the header line (`# SCENARIO: my_test   SEED: 0   CAMERAS: auto   BOTCFG: auto`).
3. Add verbs: client actors (`@p1 login`, `@p1 goto x,y,z`, `@p1 wait-see any-bot as=B`), cameras
   (`@cam:$B login`), server commands (`@srv cmd "$B" status expect="state="`) and checks
   (`@srv expect ...`, `@srv expect-none ... msg~="Lua Script Error"`).
   The full verb and expect reference is in `../README.md` ("Steps format").
4. Run it: `python tools/bottest/runner.py tools/bottest/scenarios/my_test.steps --run-id my_1`
   (add `--var NAME=VALUE` for anything your scenario reads as `$NAME`, e.g. a password).
5. Read `runs/my_1/result.md` (PASS / FAIL / STUCK per step and per expect).

Tips: keep passwords out of the file (pass them with `--var`), give each concurrently running
scenario its own harness profile/character, and end with an `expect-none` for Lua errors.
