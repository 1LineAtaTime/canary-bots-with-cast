function onUpdateDatabase()
	logger.info("Updating database to version 62 (bot players + cast system schema)")

	-- Single consolidated migration for the bot + cast distribution. All tables
	-- use IF NOT EXISTS so this is safe on a server that already has them. The
	-- shipped schema.sql is at db_version 56; this migration brings everything the
	-- bot/cast system needs. `source`/`source_file` use generic labels and are
	-- defaulted (the distributed data omits provenance). `source_name` on
	-- bot_city_routes is functional routing data and IS populated by the dumps.

	-- ---- Cast / livestream ----
	db.query([[
		CREATE TABLE IF NOT EXISTS `cast_broadcasters` (
			`player_id` INT NOT NULL,
			`player_name` VARCHAR(255) NOT NULL,
			`started_at` TIMESTAMP NULL DEFAULT CURRENT_TIMESTAMP,
			CONSTRAINT `cast_broadcasters_pk` PRIMARY KEY (`player_id`),
			CONSTRAINT `cast_broadcasters_players_fk`
				FOREIGN KEY (`player_id`) REFERENCES `players` (`id`) ON DELETE CASCADE
		) ENGINE=InnoDB DEFAULT CHARSET=utf8;
	]])

	-- ---- Bots currently loaded in the engine (for the @cast list filter) ----
	db.query([[
		CREATE TABLE IF NOT EXISTS `bot_active_players` (
			`player_id` INT(11) NOT NULL PRIMARY KEY,
			CONSTRAINT `bot_active_players_fk` FOREIGN KEY (`player_id`)
				REFERENCES `players` (`id`) ON DELETE CASCADE
		) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
	]])

	-- ---- Telemetry (opt-in writes; never read by runtime logic) ----
	db.query([[
		CREATE TABLE IF NOT EXISTS `bot_chat_emissions` (
			`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
			`ts` INT UNSIGNED NOT NULL,
			`bot_guid` INT UNSIGNED NOT NULL,
			`category` VARCHAR(32) NOT NULL,
			`phrase_idx` SMALLINT UNSIGNED NOT NULL,
			`channel_id` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
			KEY `ts_idx` (`ts`), KEY `bot_idx` (`bot_guid`), KEY `category_idx` (`category`)
		) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
	]])
	db.query([[
		CREATE TABLE IF NOT EXISTS `bot_hub_presence_60s` (
			`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
			`ts` INT UNSIGNED NOT NULL,
			`hub_name` VARCHAR(64) NOT NULL,
			`hub_x` SMALLINT UNSIGNED NOT NULL,
			`hub_y` SMALLINT UNSIGNED NOT NULL,
			`hub_z` TINYINT UNSIGNED NOT NULL,
			`bot_count_within_2` SMALLINT UNSIGNED NOT NULL,
			KEY `ts_idx` (`ts`), KEY `hub_idx` (`hub_name`)
		) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
	]])

	-- ---- Market price reference (generic source label; see THIRD_PARTY_NOTICES) ----
	db.query([[
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
	]])

	-- ---- Hunt scripts + children ----
	db.query([[
		CREATE TABLE IF NOT EXISTS `bot_hunt_scripts` (
			`id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
			`name` VARCHAR(128) NOT NULL,
			`source` ENUM('import','manual') NOT NULL DEFAULT 'import',
			`source_file` VARCHAR(255) NOT NULL DEFAULT '',
			`town_name` VARCHAR(64) NOT NULL,
			`town_id` INT NOT NULL,
			`min_level` INT NOT NULL DEFAULT 1,
			`max_level` INT NOT NULL DEFAULT 9999,
			`vocation_mask` TINYINT UNSIGNED NOT NULL DEFAULT 15,
			`keep_distance_ek` TINYINT UNSIGNED NOT NULL DEFAULT 0,
			`keep_distance_ms` TINYINT UNSIGNED NOT NULL DEFAULT 0,
			`keep_distance_ed` TINYINT UNSIGNED NOT NULL DEFAULT 0,
			`keep_distance_rp` TINYINT UNSIGNED NOT NULL DEFAULT 0,
			`script_type` VARCHAR(32) DEFAULT NULL,
			`enabled` TINYINT(1) NOT NULL DEFAULT 1,
			`is_quest` TINYINT(1) NOT NULL DEFAULT 0,
			`script_category` ENUM('hunt','quest','traveling') NOT NULL DEFAULT 'hunt',
			`successful_hunts` INT UNSIGNED NOT NULL DEFAULT 0,
			`total_kills` INT UNSIGNED NOT NULL DEFAULT 0,
			`created_at` TIMESTAMP NULL DEFAULT CURRENT_TIMESTAMP,
			`updated_at` TIMESTAMP NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
			PRIMARY KEY (`id`),
			KEY `idx_town_level_voc` (`town_id`,`min_level`,`max_level`,`vocation_mask`),
			KEY `idx_enabled` (`enabled`),
			KEY `idx_is_quest` (`is_quest`),
			KEY `idx_script_category` (`script_category`)
		) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3;
	]])
	db.query([[
		CREATE TABLE IF NOT EXISTS `bot_hunt_waypoints` (
			`id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
			`script_id` INT UNSIGNED NOT NULL,
			`phase` ENUM('travel_to','hunt_patrol','travel_from') NOT NULL,
			`seq` INT UNSIGNED NOT NULL,
			`waypoint_type` ENUM('node','stand','ladder','rope','hole','stairs_up','stairs_down','door','lever','levitate_up','levitate_down','shovel','label','conditional','machete','use_with','npc_interact','teleport') NOT NULL,
			`pos_x` INT NOT NULL DEFAULT 0,
			`pos_y` INT NOT NULL DEFAULT 0,
			`pos_z` TINYINT NOT NULL DEFAULT 0,
			`label` VARCHAR(128) DEFAULT NULL,
			`extra_data` VARCHAR(255) DEFAULT NULL,
			PRIMARY KEY (`id`),
			KEY `idx_script_phase_seq` (`script_id`,`phase`,`seq`),
			CONSTRAINT `bot_hunt_waypoints_ibfk_1` FOREIGN KEY (`script_id`) REFERENCES `bot_hunt_scripts` (`id`) ON DELETE CASCADE
		) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3;
	]])
	db.query([[
		CREATE TABLE IF NOT EXISTS `bot_hunt_targets` (
			`id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
			`script_id` INT UNSIGNED NOT NULL,
			`monster_name` VARCHAR(64) NOT NULL,
			`priority` TINYINT UNSIGNED NOT NULL DEFAULT 5,
			`behavior` ENUM('melee','distance','avoid') NOT NULL DEFAULT 'melee',
			`min_hp_percent` TINYINT UNSIGNED NOT NULL DEFAULT 0,
			`max_hp_percent` TINYINT UNSIGNED NOT NULL DEFAULT 100,
			`count` TINYINT UNSIGNED NOT NULL DEFAULT 1,
			`proximity_radius` TINYINT UNSIGNED NOT NULL DEFAULT 7,
			PRIMARY KEY (`id`),
			KEY `idx_target_script` (`script_id`),
			CONSTRAINT `bot_hunt_targets_ibfk_1` FOREIGN KEY (`script_id`) REFERENCES `bot_hunt_scripts` (`id`) ON DELETE CASCADE
		) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3;
	]])
	db.query([[
		CREATE TABLE IF NOT EXISTS `bot_hunt_fields` (
			`id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
			`script_id` INT UNSIGNED NOT NULL,
			`field_type` ENUM('fire','energy','poison','ice','physical') NOT NULL,
			`action` ENUM('walk_on','avoid') NOT NULL DEFAULT 'avoid',
			PRIMARY KEY (`id`),
			KEY `script_id` (`script_id`),
			CONSTRAINT `bot_hunt_fields_ibfk_1` FOREIGN KEY (`script_id`) REFERENCES `bot_hunt_scripts` (`id`) ON DELETE CASCADE
		) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3;
	]])
	db.query([[
		CREATE TABLE IF NOT EXISTS `bot_hunt_exclusion_zones` (
			`id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
			`script_id` INT UNSIGNED NOT NULL,
			`x1` INT NOT NULL, `y1` INT NOT NULL, `x2` INT NOT NULL, `y2` INT NOT NULL, `z` TINYINT NOT NULL,
			PRIMARY KEY (`id`),
			KEY `script_id` (`script_id`),
			CONSTRAINT `bot_hunt_exclusion_zones_ibfk_1` FOREIGN KEY (`script_id`) REFERENCES `bot_hunt_scripts` (`id`) ON DELETE CASCADE
		) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3;
	]])

	-- ---- City routes + waypoints ----
	db.query([[
		CREATE TABLE IF NOT EXISTS `bot_city_routes` (
			`id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
			`town_name` VARCHAR(64) NOT NULL,
			`town_id` INT NOT NULL,
			`route_type` VARCHAR(32) NOT NULL,
			`source_name` VARCHAR(128) DEFAULT NULL,
			`source` ENUM('import','manual') NOT NULL DEFAULT 'import',
			`enabled` TINYINT(1) NOT NULL DEFAULT 1,
			PRIMARY KEY (`id`),
			UNIQUE KEY `uq_city_route` (`town_id`,`source_name`),
			KEY `idx_town_id` (`town_id`)
		) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3;
	]])
	db.query([[
		CREATE TABLE IF NOT EXISTS `bot_city_route_waypoints` (
			`id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
			`route_id` INT UNSIGNED NOT NULL,
			`seq` INT UNSIGNED NOT NULL,
			`waypoint_type` ENUM('node','stand','ladder','rope','hole','stairs_up','stairs_down','door','action','machete','use_with') NOT NULL,
			`pos_x` INT NOT NULL DEFAULT 0,
			`pos_y` INT NOT NULL DEFAULT 0,
			`pos_z` TINYINT NOT NULL DEFAULT 0,
			`action_label` VARCHAR(128) DEFAULT NULL,
			PRIMARY KEY (`id`),
			KEY `idx_route_seq` (`route_id`,`seq`),
			CONSTRAINT `bot_city_route_waypoints_ibfk_1` FOREIGN KEY (`route_id`) REFERENCES `bot_city_routes` (`id`) ON DELETE CASCADE
		) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3;
	]])

	-- ---- Equipment + town mapping + admin command queue ----
	db.query([[
		CREATE TABLE IF NOT EXISTS `bot_equipment` (
			`id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
			`level` SMALLINT UNSIGNED NOT NULL,
			`vocation` TINYINT UNSIGNED NOT NULL,
			`slot_head` INT UNSIGNED DEFAULT NULL,
			`slot_armor` INT UNSIGNED DEFAULT NULL,
			`slot_legs` INT UNSIGNED DEFAULT NULL,
			`slot_feet` INT UNSIGNED DEFAULT NULL,
			`slot_right` INT UNSIGNED DEFAULT NULL,
			`slot_left` INT UNSIGNED DEFAULT NULL,
			`slot_backpack` INT UNSIGNED DEFAULT 0,
			PRIMARY KEY (`id`),
			UNIQUE KEY `uq_level_voc` (`level`,`vocation`)
		) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3;
	]])
	db.query([[
		CREATE TABLE IF NOT EXISTS `bot_town_mapping` (
			`source_name` VARCHAR(64) NOT NULL,
			`canary_town_id` INT NOT NULL,
			`canary_town_name` VARCHAR(64) NOT NULL,
			PRIMARY KEY (`source_name`)
		) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3;
	]])
	db.query([[
		CREATE TABLE IF NOT EXISTS `bot_commands` (
			`id` INT NOT NULL AUTO_INCREMENT,
			`bot_name` VARCHAR(50) NOT NULL,
			`command` VARCHAR(255) NOT NULL,
			`created_at` TIMESTAMP NULL DEFAULT CURRENT_TIMESTAMP,
			`processed` TINYINT DEFAULT 0,
			`result` TEXT,
			`executed_at` INT UNSIGNED DEFAULT NULL,
			PRIMARY KEY (`id`),
			KEY `idx_bot_processed` (`bot_name`,`processed`)
		) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
	]])
end
