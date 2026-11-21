-- #96 learned hunt-danger zones (bot_learning.cpp).
--
-- One row per zone: a bot death (radius 14, +45) or near-death (radius 12, +20) while working a
-- hunt script. The centre never moves after the first event, so (script, centre) is the key and
-- every later event upserts the row. Scores cap at 100; a zone expires 7 days after its last
-- event (`expires_at`, unix seconds). The engine loads unexpired rows on every engine load and
-- deletes expired ones; all writes are asynchronous (g_botDatabaseTasks).
--
-- Upgrade path for an existing database. A fresh install gets the same table from
-- 00_bot_schema.sql. Idempotent; safe to run with canary up (only the bot engine touches it).
-- Without the table the engine still runs: zones live in memory and the load logs zones=0.
CREATE TABLE IF NOT EXISTS `bot_hunt_danger` (
  `script_id` INT UNSIGNED NOT NULL,
  `pos_x` SMALLINT UNSIGNED NOT NULL,
  `pos_y` SMALLINT UNSIGNED NOT NULL,
  `pos_z` TINYINT UNSIGNED NOT NULL,
  `radius` TINYINT UNSIGNED NOT NULL,
  `min_party` TINYINT UNSIGNED NOT NULL DEFAULT 2,
  `score` TINYINT UNSIGNED NOT NULL,
  `deaths` INT UNSIGNED NOT NULL DEFAULT 0,
  `near_deaths` INT UNSIGNED NOT NULL DEFAULT 0,
  `last_seen` BIGINT NOT NULL,
  `expires_at` BIGINT NOT NULL,
  PRIMARY KEY (`script_id`, `pos_x`, `pos_y`, `pos_z`),
  KEY `expires_idx` (`expires_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
