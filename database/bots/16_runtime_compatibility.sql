-- Missing runtime state table referenced by this release's bot engine.
CREATE TABLE IF NOT EXISTS bot_state_persistence (
  guid INT UNSIGNED NOT NULL PRIMARY KEY,
  name VARCHAR(255) NOT NULL,
  ai_state TINYINT UNSIGNED NOT NULL,
  hunt_script_id INT UNSIGNED DEFAULT NULL,
  hunt_phase TINYINT UNSIGNED DEFAULT NULL,
  waypoint_idx INT UNSIGNED DEFAULT NULL,
  kill_count INT UNSIGNED DEFAULT NULL,
  travel_dest_town_id INT UNSIGNED DEFAULT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3;
-- Seeded characters may exceed the signed SMALLINT range during migrations.
ALTER TABLE player_charms
  MODIFY charm_points INT UNSIGNED NOT NULL DEFAULT 0,
  MODIFY max_charm_points INT UNSIGNED NOT NULL DEFAULT 0;
-- Runtime flag used when saving and restoring bot state across restarts.
CREATE TABLE IF NOT EXISTS bot_startup_config (
  config_key VARCHAR(191) NOT NULL PRIMARY KEY,
  config_value VARCHAR(255) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3;
-- Aggregated navigation diagnostics emitted by the bot engine.
CREATE TABLE IF NOT EXISTS bot_nav_events (
  event_type VARCHAR(100) NOT NULL,
  hunt_script_id INT NOT NULL DEFAULT 0,
  hunt_script_name VARCHAR(255) NOT NULL DEFAULT '',
  route_town_id INT NOT NULL DEFAULT 0,
  route_type VARCHAR(100) NOT NULL DEFAULT '',
  town_name VARCHAR(255) DEFAULT NULL,
  bot_name VARCHAR(255) NOT NULL DEFAULT '',
  context TEXT DEFAULT NULL,
  event_count BIGINT UNSIGNED NOT NULL DEFAULT 0,
  last_seen TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (event_type, hunt_script_id, route_town_id, route_type)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3;
-- Seeded bot online counters can already equal INT_MAX before the next save.
ALTER TABLE players MODIFY onlinetime BIGINT UNSIGNED NOT NULL DEFAULT 0;
