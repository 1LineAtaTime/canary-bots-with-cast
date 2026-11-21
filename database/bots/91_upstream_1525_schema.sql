-- ============================================================================
-- 91_upstream_1525_schema.sql: reconcile an existing bot-server database with
-- upstream canary's migrations 56-59, then set db_version = 59 (#150, #128).
-- ----------------------------------------------------------------------------
-- CUTOVER ONLY. Not part of import.sh. Run it with canary STOPPED, on a database
-- that a 15.00 (pre-port) bot server used, right before the 15.25 port binary
-- starts for the first time. Take a mysqldump first.
--
-- Why it exists: our old migrations/62.lua sat inside upstream's numbered chain
-- and left production at db_version = 62. The stock runner applies a migration
-- only when fileVersion > db_version, so upstream's 56-59 never run by
-- themselves. 59 (`players`.`expert_pvp_mode`) is read and written by the port's
-- player load/save, so a missing column fails every save. The July 2026 resync
-- applied 56/57/58 by hand on production; 59 was never applied.
--
-- What it does, each step only if it is missing (re-running it is a no-op):
--   56  `accounts`.`password` VARCHAR(255) NOT NULL; indexes accounts_email and
--       accounts_password (upstream's CREATE INDEX is not idempotent).
--   57  `players`.`comment`. An existing column is left as it is: production
--       has VARCHAR(5000) where upstream has VARCHAR(255), and shrinking it
--       could truncate data.
--   58  `active_livestream_casters`: created with upstream's definition, or an
--       existing hand-made copy is aligned to it (UNSIGNED counters + the FK to
--       players; rows whose caster no longer exists are deleted first, or the FK
--       could not be added).
--   59  `players`.`expert_pvp_mode` TINYINT(1) UNSIGNED NOT NULL DEFAULT 0
--       AFTER `skulltime`.
--   bot  `bot_chat_emissions`.`text_hash`, which the old 62.lua never created.
--   db_version = 59, i.e. upstream's head at pin 666a60f6f. From then on the
--   stock runner applies upstream's 60, 61, ... normally.
--
-- Works on MySQL 8 and MariaDB (no ADD COLUMN IF NOT EXISTS, which MySQL lacks).
-- ============================================================================

SET @db := DATABASE();

-- ---- 56: accounts.password width + email/password indexes -------------------
ALTER TABLE `accounts` MODIFY `password` VARCHAR(255) NOT NULL;

SET @s := IF((SELECT COUNT(*) FROM information_schema.statistics
               WHERE table_schema = @db AND table_name = 'accounts' AND index_name = 'accounts_email') = 0,
             'CREATE INDEX `accounts_email` ON `accounts` (`email`)', 'DO 0');
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @s := IF((SELECT COUNT(*) FROM information_schema.statistics
               WHERE table_schema = @db AND table_name = 'accounts' AND index_name = 'accounts_password') = 0,
             'CREATE INDEX `accounts_password` ON `accounts` (`password`)', 'DO 0');
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

-- ---- 57: players.comment -----------------------------------------------------
SET @s := IF((SELECT COUNT(*) FROM information_schema.columns
               WHERE table_schema = @db AND table_name = 'players' AND column_name = 'comment') = 0,
             'ALTER TABLE `players` ADD COLUMN `comment` varchar(255) NOT NULL DEFAULT '''' AFTER `boss_points`', 'DO 0');
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

-- ---- 58: active_livestream_casters ------------------------------------------
CREATE TABLE IF NOT EXISTS `active_livestream_casters` (
	`caster_id` int(11) NOT NULL,
	`livestream_status` tinyint(1) UNSIGNED NOT NULL DEFAULT '0',
	`livestream_viewers` int(11) UNSIGNED NOT NULL DEFAULT '0',
	CONSTRAINT `active_livestream_casters_pk` PRIMARY KEY (`caster_id`),
	CONSTRAINT `active_livestream_casters_players_fk`
		FOREIGN KEY (`caster_id`) REFERENCES `players` (`id`)
		ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- A copy created by hand (July) has signed counters and no FK: align it.
ALTER TABLE `active_livestream_casters`
	MODIFY `livestream_status` tinyint(1) UNSIGNED NOT NULL DEFAULT '0',
	MODIFY `livestream_viewers` int(11) UNSIGNED NOT NULL DEFAULT '0';

SET @fk := (SELECT COUNT(*) FROM information_schema.table_constraints
             WHERE constraint_schema = @db AND table_name = 'active_livestream_casters'
               AND constraint_type = 'FOREIGN KEY');
SET @s := IF(@fk = 0,
             'DELETE c FROM `active_livestream_casters` c LEFT JOIN `players` p ON p.`id` = c.`caster_id` WHERE p.`id` IS NULL',
             'DO 0');
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
SET @s := IF(@fk = 0,
             'ALTER TABLE `active_livestream_casters` ADD CONSTRAINT `active_livestream_casters_players_fk` FOREIGN KEY (`caster_id`) REFERENCES `players` (`id`) ON DELETE CASCADE',
             'DO 0');
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

-- ---- 59: players.expert_pvp_mode ---------------------------------------------
SET @s := IF((SELECT COUNT(*) FROM information_schema.columns
               WHERE table_schema = @db AND table_name = 'players' AND column_name = 'expert_pvp_mode') = 0,
             'ALTER TABLE `players` ADD COLUMN `expert_pvp_mode` tinyint(1) UNSIGNED NOT NULL DEFAULT ''0'' AFTER `skulltime`', 'DO 0');
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

-- ---- bot_chat_emissions.text_hash (bot schema, not an upstream migration) ------
-- bot_chat.cpp writes it; the old 62.lua created the table without it. Added only when
-- the table exists and the column is missing (00_bot_schema.sql creates it on fresh installs).
SET @s := IF((SELECT COUNT(*) FROM information_schema.tables
               WHERE table_schema = @db AND table_name = 'bot_chat_emissions') = 1
             AND (SELECT COUNT(*) FROM information_schema.columns
               WHERE table_schema = @db AND table_name = 'bot_chat_emissions' AND column_name = 'text_hash') = 0,
             'ALTER TABLE `bot_chat_emissions` ADD COLUMN `text_hash` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `channel_id`',
             'DO 0');
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

-- ---- db_version tracks upstream only -----------------------------------------
UPDATE `server_config` SET `value` = '59' WHERE `config` = 'db_version';

SELECT `value` AS db_version FROM `server_config` WHERE `config` = 'db_version';
