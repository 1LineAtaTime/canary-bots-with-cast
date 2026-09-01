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
