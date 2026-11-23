-- ============================================================================
-- #197 monk conversion — in-place UPDATE (generate.py --emit-monk-conversion)
-- 199 of 997 bots (account_id=65000) become monks: 20% of every
-- (loaded@500 = config.lua botPlayersOnline, base vocation, level band) stratum, seed 20260523.
-- Touches ONLY vocation, health(max), mana(max), cap and, for some, looktype.
-- Names, ids, levels, experience, skills (all 100), houses, storage: unchanged.
--
-- RUN WITH CANARY STOPPED: a save rewrites the whole players row from memory, so a
-- running server reverts this silently (project_bot_rename_997). Rollback:
-- the matching .before.sql (regenerate it from the live rows right before applying;
-- tools/bot_population_generator/apply_monk_conversion.sh does both).
-- Idempotent: values are computed from `level`, keyed by id.
-- ============================================================================

START TRANSACTION;

UPDATE `players` SET `vocation` = IF(`level` >= 20, 10, 9), `health` = 185 + (`level` - 1) * 10, `healthmax` = 185 + (`level` - 1) * 10, `mana` = 90 + (`level` - 1) * 10, `manamax` = 90 + (`level` - 1) * 10, `cap` = 470 + (`level` - 1) * 25 WHERE `account_id` = 65000 AND `id` IN (65009,65010,65019,65020,65021,65031,65032,65036,65042,65045,65050,65051,65052,65061,65065,65076,65079,65082,65084,65089,65101,65102,65107,65108,65112,65123,65124,65131,65137,65141,65146,65150,65151,65163,65172,65176,65182,65184,65188,65191,65195,65222,65223,65227,65232,65235,65240,65241,65248,65255,65261,65264,65269,65278,65283,65286,65287,65299,65301,65306,65311,65328,65332,65333,65334,65338,65344,65347,65349,65353,65356,65357,65358,65361,65362,65363,65365,65369,65374,65384,65387,65388,65391,65396,65415,65418,65440,65445,65448,65450,65455,65457,65458,65460,65468,65473,65482,65485,65486,65509,65510,65514,65516,65519,65536,65537,65538,65544,65554,65561,65572,65574,65581,65585,65587,65593,65596,65597,65604,65605,65614,65625,65626,65629,65639,65645,65651,65654,65655,65656,65658,65663,65676,65680,65691,65694,65704,65707,65717,65719,65722,65723,65731,65737,65738,65740,65743,65751,65758,65759,65764,65766,65767,65777,65781,65784,65786,65798,65801,65805,65806,65810,65815,65820,65846,65847,65850,65853,65855,65856,65864,65869,65876,65879,65883,65889,65899,65902,65905,65912,65922,65930,65931,65935,65939,65940,65956,65958,65961,65966,65973,65974,65983,65985,65988,65994,65998,65999,66000);
UPDATE `players` SET `looktype` = 1824 WHERE `account_id` = 65000 AND `id` IN (65036,65042,65076,65084,65102,65146,65150,65172,65176,65182,65184,65227,65235,65241,65283,65287,65301,65311,65333,65347,65349,65353,65357,65369,65391,65415,65445,65455,65473,65485,65537,65561,65581,65597,65605,65651,65655,65663,65691,65707,65717,65723,65731,65743,65751,65759,65801,65815,65853,65869,65889,65931,65973,65999);
UPDATE `players` SET `looktype` = 1825 WHERE `account_id` = 65000 AND `id` IN (65045,65089,65101,65107,65123,65137,65191,65286,65328,65332,65334,65338,65356,65362,65374,65384,65418,65440,65448,65450,65468,65482,65544,65572,65626,65694,65722,65738,65740,65758,65786,65798,65806,65820,65846,65856,65864,65876,65902,65912,65922,65956,65974,65998);

COMMIT;

-- Verification (expect ~200 per base vocation; every monk on the 10/10/25 formula):
SELECT `vocation`, COUNT(*) FROM `players` WHERE `account_id` = 65000 GROUP BY `vocation` ORDER BY `vocation`;
SELECT COUNT(*) AS bad_monk_rows FROM `players` WHERE `account_id` = 65000 AND `vocation` IN (9, 10) AND (`healthmax` <> 185 + (`level` - 1) * 10 OR `manamax` <> 90 + (`level` - 1) * 10 OR `cap` <> 470 + (`level` - 1) * 25 OR `vocation` <> IF(`level` >= 20, 10, 9));
