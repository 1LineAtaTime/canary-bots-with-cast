-- #47 review: give each persisted bot task a timestamp so the engine can ignore stale ones.
--
-- Since #47, rows in `bot_state_persistence` of bots outside the startup population survive
-- boot (only restored rows are deleted). Without an age the engine could, after a crash or a
-- later raise of botPlayersOnline, resume a hunt task saved days ago. The engine now restores
-- only rows with `saved_at` within the last 24 h and garbage-collects older ones.
--
-- MUST run BEFORE the engine built from this commit starts: its INSERT/SELECT statements name
-- `saved_at`, and without the column they fail (no task is saved or restored).
--
-- Idempotent (MySQL 8 and MariaDB): creates the table if it is missing, adds the column only
-- if it is missing. Safe to run with canary up (the table is tiny and only the bot engine
-- writes it). Rows that already exist get the ALTER's time, i.e. a 24 h grace, then expire.
CREATE TABLE IF NOT EXISTS bot_state_persistence (
  guid INT UNSIGNED NOT NULL PRIMARY KEY,
  name VARCHAR(255) NOT NULL,
  ai_state TINYINT UNSIGNED NOT NULL,
  hunt_script_id INT UNSIGNED DEFAULT NULL,
  hunt_phase TINYINT UNSIGNED DEFAULT NULL,
  waypoint_idx INT UNSIGNED DEFAULT NULL,
  kill_count INT UNSIGNED DEFAULT NULL,
  travel_dest_town_id INT UNSIGNED DEFAULT NULL,
  saved_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3;

SET @bot_has_saved_at := (
  SELECT COUNT(*) FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE()
    AND TABLE_NAME = 'bot_state_persistence'
    AND COLUMN_NAME = 'saved_at'
);
SET @bot_saved_at_ddl := IF(@bot_has_saved_at = 0,
  'ALTER TABLE `bot_state_persistence` ADD COLUMN `saved_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP',
  'DO 0');
PREPARE bot_saved_at_stmt FROM @bot_saved_at_ddl;
EXECUTE bot_saved_at_stmt;
DEALLOCATE PREPARE bot_saved_at_stmt;
