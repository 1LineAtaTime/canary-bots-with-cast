-- Bot system table DDL.
--
-- Authored bot data (hunt scripts, waypoints, targets, city routes, POIs,
-- equipment, town mapping) is NOT in MySQL — it lives in data/bot/authored/*.csv
-- and ships in the repo. The engine reads the CSVs directly. Only the tables
-- below are written at runtime.

CREATE TABLE IF NOT EXISTS `bot_market_item_prices` (
  `item_id` INT UNSIGNED NOT NULL,
  `name` VARCHAR(128) NULL,
  `npc_buy` BIGINT UNSIGNED NULL,
  `npc_sell` BIGINT UNSIGNED NULL,
  `market_max` BIGINT UNSIGNED NULL,
  `market_low` BIGINT UNSIGNED NULL,
  `market_high` BIGINT UNSIGNED NULL,
  `marketable` TINYINT(1) NOT NULL DEFAULT 0,
  `weight` INT UNSIGNED NULL,
  `category` VARCHAR(64) NULL,
  `upgrade_class` TINYINT UNSIGNED NULL DEFAULT 0,
  `source` ENUM('protobuf','npc_lua','external','heuristic') NULL,
  `last_updated` TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`item_id`),
  INDEX `idx_marketable` (`marketable`),
  INDEX `idx_market` (`market_max`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS `bot_hunt_script_stats` (
  `script_id` INT UNSIGNED NOT NULL PRIMARY KEY,
  `successful_hunts` INT UNSIGNED NOT NULL DEFAULT 0,
  `total_kills` INT UNSIGNED NOT NULL DEFAULT 0,
  `updated_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3;

-- ---- Bots currently loaded in the engine (the @cast list filter) ----
CREATE TABLE IF NOT EXISTS `bot_active_players` (
  `player_id` INT NOT NULL,
  PRIMARY KEY (`player_id`),
  CONSTRAINT `bot_active_players_fk` FOREIGN KEY (`player_id`)
    REFERENCES `players` (`id`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---- Admin command queue (/cavebot over MySQL, harness headless control) ----
CREATE TABLE IF NOT EXISTS `bot_commands` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `bot_name` VARCHAR(50) NOT NULL,
  `command` VARCHAR(255) NOT NULL,
  `created_at` TIMESTAMP NULL DEFAULT CURRENT_TIMESTAMP,
  `processed` TINYINT DEFAULT 0,
  `result` TEXT,
  `executed_at` INT UNSIGNED DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `idx_bot_processed` (`bot_name`,`processed`),
  -- The queue poll is `WHERE processed=0 ORDER BY id LIMIT 10`; idx_bot_processed
  -- cannot serve it (its leading column is bot_name).
  KEY `idx_processed_id` (`processed`,`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---- Telemetry (opt-in writes; never read by runtime logic) ----
-- text_hash is written by bot_chat.cpp; the old 62.lua never created it.
CREATE TABLE IF NOT EXISTS `bot_chat_emissions` (
  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `ts` INT UNSIGNED NOT NULL,
  `bot_guid` INT UNSIGNED NOT NULL,
  `category` VARCHAR(32) NOT NULL,
  `phrase_idx` SMALLINT UNSIGNED NOT NULL,
  `channel_id` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `text_hash` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `ts_idx` (`ts`),
  KEY `bot_idx` (`bot_guid`),
  KEY `category_idx` (`category`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `bot_hub_presence_60s` (
  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `ts` INT UNSIGNED NOT NULL,
  `hub_name` VARCHAR(64) NOT NULL,
  `hub_x` SMALLINT UNSIGNED NOT NULL,
  `hub_y` SMALLINT UNSIGNED NOT NULL,
  `hub_z` TINYINT UNSIGNED NOT NULL,
  `bot_count_within_2` SMALLINT UNSIGNED NOT NULL,
  PRIMARY KEY (`id`),
  KEY `ts_idx` (`ts`),
  KEY `hub_idx` (`hub_name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
