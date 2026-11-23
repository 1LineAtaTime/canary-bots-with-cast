/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_learning.cpp — what the engine learns from the world while it runs.
//
// #30 PASSIVE SPAWN AUTO-CLAIM. `/cavebot claim` protects a player who asks for it. Most players
// never type it, so a real player who stays inside a #29 spawn group's area (or fights its
// monsters there) for a while now claims the group implicitly. The claim is an ordinary
// playerClaims_ row flagged autoClaim, so every existing pick gate (live hunt, virtual hunt, live
// and virtual party formation, wake resume, restore) honours it by spawnGroup with no new check.
// Holders already inside are sent out through their normal LEAVING leg, not teleported. The
// detector also owns the release: leaving the area, going idle, entering a town, logging out.
// Journal: [SPAWN_CLAIM] auto claim|release|evict.
// OFF by default since 2026-11-23 (botSpawnAutoClaimEnable=false, user decision: claims are
// explicit-only). When enabled it protects only the group itself: it never sets the
// playerHuntEngaged_ roam keep-out / hunt repel, which stays tied to the explicit command.
//
// #96 LEARNED HUNT-DANGER ZONES. A bot death (radius 14, +45) or near-death (radius 12, +20)
// while working a hunt records a zone keyed by script; the score caps at 100 and a zone lives
// 7 days after its last event. Rows persist asynchronously to `bot_hunt_danger`
// (database/bots/25_bot_hunt_danger.sql) and are loaded on every engine load. Consumers:
//   - target choice (live): a solo bot skips monsters inside a zone unless they attack it;
//   - waypoint choice (live + virtual twin): plain patrol nodes inside a zone are skipped;
//   - party conversion (live + virtual): a script with established danger (max score >= 60 or
//     deaths >= 2) is party-only — solo picks skip it and party formation tries it first.
// Journal: [DANGER] record|avoid|convert|summary|load.
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

namespace {
	int32_t chebXY(const Position& a, const Position& b) {
		return std::max(std::abs(static_cast<int32_t>(a.x) - static_cast<int32_t>(b.x)),
		                std::abs(static_cast<int32_t>(a.y) - static_cast<int32_t>(b.y)));
	}
	int64_t unixNow() {
		return static_cast<int64_t>(std::time(nullptr));
	}
	const char* phaseName(HuntPhase p) {
		switch (p) {
			case HuntPhase::PREPARING: return "PREPARING";
			case HuntPhase::TRAVEL_TO: return "TRAVEL_TO";
			case HuntPhase::PATROLLING: return "PATROLLING";
			case HuntPhase::LEAVING: return "LEAVING";
			case HuntPhase::RESUPPLYING: return "RESUPPLYING";
		}
		return "?";
	}
}

void BotEngine::tickLearning(int64_t nowMs) {
	tickSpawnAutoClaim(nowMs);
	dangerReport(nowMs);
}

// ============================================================================
// #30 passive spawn auto-claim
// ============================================================================

// Chebyshev distance from pos to the nearest same-floor patrol waypoint of any script in `group`.
int32_t BotEngine::distToSpawnGroup(const Position& pos, const std::string& group) const {
	int32_t best = INT32_MAX;
	for (const auto& s : huntScripts_) {
		if (s.spawnGroup != group || !s.enabled || s.scriptCategory != "hunt") {
			continue;
		}
		for (const auto& wp : s.patrolWaypoints) {
			if (wp.pos.z == pos.z) {
				best = std::min(best, chebXY(wp.pos, pos));
			}
		}
	}
	return best;
}

// Sends every bot holding a hunt of `group` out of it. A patrolling holder walks its authored
// return leg (LEAVING), which is what a person would see: the other hunter finishes and goes.
// A holder still on its way in (PREPARING / TRAVEL_TO) never arrived and simply aborts. Holders
// already LEAVING or RESUPPLYING are left alone; their hunt ends on its own.
uint32_t BotEngine::autoClaimEvictGroup(const std::string& group, const std::string& owner) {
	std::vector<uint32_t> holders;
	for (const auto& [sid, guid] : activeHunts_) {
		for (const auto& s : huntScripts_) {
			if (s.id == sid) {
				if (s.spawnGroup == group) {
					holders.push_back(guid);
				}
				break;
			}
		}
	}
	if (auto sg = activeSpawnGroups_.find(group); sg != activeSpawnGroups_.end()) {
		holders.push_back(sg->second);
	}
	std::sort(holders.begin(), holders.end());
	holders.erase(std::unique(holders.begin(), holders.end()), holders.end());

	uint32_t evicted = 0;
	for (const uint32_t guid : holders) {
		auto it = guidToIndex_.find(guid);
		if (it == guidToIndex_.end()) {
			continue;
		}
		BotState& b = bots_[it->second];
		if (b.huntScriptId == 0 || b.huntPhase == HuntPhase::LEAVING || b.huntPhase == HuntPhase::RESUPPLYING) {
			continue;
		}
		const HuntPhase was = b.huntPhase;
		auto hp = b.getPlayer();
		const bool live = hp && !hp->isRemoved() && !b.hibernated;
		if (was == HuntPhase::PATROLLING) {
			if (b.isPartyHuntLeader && b.partyHuntId > 0) {
				if (live) {
					dissolvePartyHunt(b.partyHuntId, "spawn_auto_claimed");
				} else {
					dissolveVirtualPartyHunt(b.partyHuntId, "spawn_auto_claimed");
				}
				b.isPartyHuntLeader = false;
				b.partyHuntId = 0;
				b.partyRole = 0;
				b.partyLeaderGuid = 0;
			}
			b.huntTargetId = 0;
			if (live) {
				hp->setAttackedCreature(nullptr);
				b.isRecoveryRoute = false;
				b.recoveryWaypoints.clear();
				beginHuntPhase(b, HuntPhase::LEAVING);
			} else {
				b.huntWaypointIdx = 0;
				b.huntPhase = HuntPhase::LEAVING;
			}
		} else {
			abortHunt(b, "spawn auto-claimed");
		}
		evicted++;
		g_logger().info("[SPAWN_CLAIM] auto evict bot='{}' guid={} group={} phase={} live={} owner='{}'",
			b.name, b.guid, group, phaseName(was), live ? 1 : 0, owner);
	}
	return evicted;
}

void BotEngine::autoClaimRelease(uint32_t guid, AutoClaimTrack& t, const char* reason) {
	if (t.claimed) {
		std::string owner;
		for (auto it = playerClaims_.begin(); it != playerClaims_.end();) {
			if (it->second.autoClaim && it->second.guid == guid) {
				owner = it->second.ownerName;
				it = playerClaims_.erase(it);
			} else {
				++it;
			}
		}
		// The owner-offline sweep (player spawn-claims, ~15 s) may have erased the row first.
		if (owner.empty()) {
			owner = t.playerName;
		}
		g_logger().info("[SPAWN_CLAIM] auto release player='{}' guid={} group={} reason={} held={}s",
			owner, guid, t.group, reason, (OTSYS_TIME() - t.enteredAt) / 1000);
	}
	t.claimed = false;
	t.group.clear();
	t.scriptId = 0;
	t.enteredAt = 0;
	t.fightMs = 0;
}

void BotEngine::tickSpawnAutoClaim(int64_t nowMs) {
	if (!g_configManager().getBoolean(BOT_SPAWN_AUTOCLAIM_ENABLE)) {
		for (auto& [guid, t] : autoClaimTrack_) {
			autoClaimRelease(guid, t, "disabled");
		}
		autoClaimTrack_.clear();
		return;
	}
	if (nowMs - autoClaimLastSweep_ < AUTOCLAIM_SWEEP_MS) {
		return;
	}
	const int64_t dt = autoClaimLastSweep_ == 0 ? AUTOCLAIM_SWEEP_MS : std::min<int64_t>(nowMs - autoClaimLastSweep_, 2 * AUTOCLAIM_SWEEP_MS);
	autoClaimLastSweep_ = nowMs;

	const int64_t dwellMs = std::max<int64_t>(5, g_configManager().getNumber(BOT_SPAWN_AUTOCLAIM_DWELL_SEC)) * 1000;
	const int64_t idleMs = std::max<int64_t>(30, g_configManager().getNumber(BOT_SPAWN_AUTOCLAIM_IDLE_SEC)) * 1000;
	const int64_t leaveMs = std::max<int64_t>(5, g_configManager().getNumber(BOT_SPAWN_AUTOCLAIM_LEAVE_SEC)) * 1000;

	std::unordered_set<uint32_t> seen;
	for (const auto& [id, player] : g_game().getPlayers()) {
		if (!player || player->isBotPlayer() || player->isRemoved()) {
			continue;
		}
		const uint32_t guid = player->getGUID();
		seen.insert(guid);
		const Position pos = player->getPosition();
		auto [tIt, created] = autoClaimTrack_.try_emplace(guid);
		AutoClaimTrack& t = tIt->second;
		t.playerName = player->getName();
		const auto attacked = player->getAttackedCreature();
		const bool fighting = attacked && attacked->getMonster() != nullptr;
		if (created || pos != t.lastPos || fighting) {
			t.lastActive = nowMs;
		}
		t.lastPos = pos;

		const auto tile = g_game().map.getTile(pos);
		if ((tile && tile->hasFlag(TILESTATE_PROTECTIONZONE)) || isInTownArea(pos)) {
			autoClaimRelease(guid, t, "town");
			continue;
		}

		// Which group is the player standing in? Stickiness first: while inside the tracked
		// group's area it stays the group, even where it overlaps a neighbour.
		std::string group;
		uint32_t scriptId = 0;
		if (!t.group.empty() && distToSpawnGroup(pos, t.group) <= PLAYER_CLAIM_MAX_DIST) {
			group = t.group;
			scriptId = t.scriptId;
		} else {
			int32_t bestDist = 0, runnerUpDist = 0;
			const HuntScript* runnerUp = nullptr;
			const HuntScript* best = detectClaimableScript(pos, bestDist, runnerUp, runnerUpDist);
			if (best && bestDist <= PLAYER_CLAIM_MAX_DIST && !best->spawnGroup.empty() && !best->isQuest) {
				group = best->spawnGroup;
				scriptId = best->id;
			}
		}

		if (group != t.group) {
			if (t.claimed) {
				// Out of the claimed area: hold it for the leave grace (a player chasing a monster a
				// few tiles out has not left), unless they already stand in another group.
				if (group.empty() && nowMs - t.lastInArea <= leaveMs) {
					continue;
				}
				autoClaimRelease(guid, t, "left");
			}
			t.group = group;
			t.scriptId = scriptId;
			t.enteredAt = nowMs;
			t.lastInArea = nowMs;
			t.fightMs = 0;
			continue;
		}
		if (group.empty()) {
			continue;
		}

		t.lastInArea = nowMs;
		if (fighting) {
			t.fightMs += dt;
		}
		const bool idle = nowMs - t.lastActive > idleMs;

		if (t.claimed) {
			if (idle) {
				autoClaimRelease(guid, t, "idle");
				t.group = group;   // keep tracking the area; a fresh dwell is needed to re-claim
				t.scriptId = scriptId;
				t.enteredAt = nowMs;
				continue;
			}
			auto row = playerClaims_.find(t.scriptId);
			if (row == playerClaims_.end() || row->second.guid != guid) {
				// Released by a command (/cavebot release, clearclaim) — honour it: a new dwell.
				t.claimed = false;
				t.enteredAt = nowMs;
				t.fightMs = 0;
				continue;
			}
			row->second.expiresAt = std::max(row->second.expiresAt, nowMs + AUTOCLAIM_ROW_TTL_MS);
			autoClaimEvictGroup(group, player->getName());   // a holder that slipped in (restore, force)
			continue;
		}
		if (idle) {
			continue;
		}
		const int64_t dwell = nowMs - t.enteredAt;
		const bool fightShortcut = t.fightMs >= AUTOCLAIM_FIGHT_SHORTCUT_MS && dwell >= AUTOCLAIM_FIGHT_SHORTCUT_MS;
		if (dwell < dwellMs && !fightShortcut) {
			continue;
		}

		// Already claimed by anyone (another player, or this player's own command claim)?
		bool taken = false;
		for (const auto& [sid, c] : playerClaims_) {
			if (nowMs < c.expiresAt && (sid == scriptId || c.spawnGroup == group)) {
				taken = true;
				break;
			}
		}
		if (taken) {
			continue;
		}

		PlayerSpawnClaim claim;
		claim.guid = guid;
		claim.ownerName = player->getName();
		claim.scriptId = scriptId;
		claim.spawnGroup = group;
		claim.expiresAt = nowMs + AUTOCLAIM_ROW_TTL_MS;
		claim.autoClaim = true;
		playerClaims_[scriptId] = claim;   // recorded BEFORE evicting, so abortHunt's recovery honours it
		t.claimed = true;
		// Deliberately NOT playerHuntEngaged_ (user decision 2026-11-23). That flag is what keeps
		// roamers 30 tiles out (ROAM_SUPPRESS_KEEPOUT) and repels every hunt within 50 tiles
		// (HUNT_REPEL_TILES); only an explicit `/cavebot claim` may ask for that much quiet. An
		// auto-claim protects the spawn group (the playerClaims_ row) and nothing around it.
		const uint32_t evicted = autoClaimEvictGroup(group, claim.ownerName);
		std::string scriptName;
		for (const auto& s : huntScripts_) {
			if (s.id == scriptId) {
				scriptName = s.name;
				break;
			}
		}
		g_logger().info("[SPAWN_CLAIM] auto claim player='{}' guid={} group={} script={} '{}' dwell={}s fight={}s evicted={}",
			claim.ownerName, guid, group, scriptId, scriptName, dwell / 1000, t.fightMs / 1000, evicted);
	}

	for (auto it = autoClaimTrack_.begin(); it != autoClaimTrack_.end();) {
		if (!seen.count(it->first)) {
			autoClaimRelease(it->first, it->second, "offline");
			it = autoClaimTrack_.erase(it);
		} else {
			++it;
		}
	}
}

// ============================================================================
// #96 learned hunt-danger zones
// ============================================================================

void BotEngine::loadDangerZones() {
	dangerZones_.clear();
	auto& db = Database::getInstance();
	auto result = db.storeQuery(
		"SELECT `script_id`, `pos_x`, `pos_y`, `pos_z`, `radius`, `min_party`, `score`, `deaths`, "
		"`near_deaths`, `last_seen`, `expires_at` FROM `bot_hunt_danger` WHERE `expires_at` > UNIX_TIMESTAMP()");
	if (result) {
		do {
			DangerZone z;
			z.scriptId = result->getNumber<uint32_t>("script_id");
			z.center = Position(result->getNumber<uint16_t>("pos_x"), result->getNumber<uint16_t>("pos_y"), result->getNumber<uint8_t>("pos_z"));
			z.radius = result->getNumber<int32_t>("radius");
			z.minParty = result->getNumber<uint32_t>("min_party");
			z.score = result->getNumber<int32_t>("score");
			z.deaths = result->getNumber<uint32_t>("deaths");
			z.nearDeaths = result->getNumber<uint32_t>("near_deaths");
			z.lastSeenS = result->getNumber<int64_t>("last_seen");
			z.expiresAtS = result->getNumber<int64_t>("expires_at");
			dangerZones_.push_back(z);
		} while (result->next());
	}
	g_botDatabaseTasks().execute("DELETE FROM `bot_hunt_danger` WHERE `expires_at` <= UNIX_TIMESTAMP()");
	rebuildDangerSummary();
	g_logger().info("[DANGER] load zones={} scripts={} (bot_hunt_danger)", dangerZones_.size(), dangerSummary_.size());
}

void BotEngine::rebuildDangerSummary() {
	const int64_t now = unixNow();
	std::erase_if(dangerZones_, [now](const DangerZone& z) { return z.expiresAtS <= now; });
	dangerSummary_.clear();
	for (const auto& z : dangerZones_) {
		auto& s = dangerSummary_[z.scriptId];
		s.maxScore = std::max(s.maxScore, z.score);
		s.deaths += z.deaths;
		s.nearDeaths += z.nearDeaths;
		s.zones++;
	}
}

uint32_t BotEngine::dangerPartySize(const BotState& bot) const {
	if (auto p = bot.getPlayer(); p && !p->isRemoved()) {
		if (auto party = p->getParty()) {
			return 1 + static_cast<uint32_t>(party->getMembers().size());
		}
	}
	if (bot.partyHuntId > 0) {
		auto m = s_partyHuntMembers.find(bot.partyHuntId);
		return std::max<uint32_t>(2, m != s_partyHuntMembers.end() ? 1 + static_cast<uint32_t>(m->second.size()) : 2);
	}
	return 1;
}

bool BotEngine::dangerAvoids(const BotState& bot, uint32_t scriptId, const Position& pos) const {
	if (dangerZones_.empty() || scriptId == 0) {
		return false;
	}
	const int64_t now = unixNow();
	uint32_t party = 0;   // computed lazily: most calls match no zone
	for (const auto& z : dangerZones_) {
		if (z.scriptId != scriptId || z.center.z != pos.z || z.expiresAtS <= now || chebXY(z.center, pos) > z.radius) {
			continue;
		}
		if (party == 0) {
			party = dangerPartySize(bot);
		}
		if (party < z.minParty) {
			return true;
		}
	}
	return false;
}

bool BotEngine::dangerWantsParty(uint32_t scriptId) const {
	if (dangerSummary_.empty() || !g_configManager().getBoolean(BOT_DANGER_ZONES_ENABLE)) {
		return false;
	}
	auto it = dangerSummary_.find(scriptId);
	return it != dangerSummary_.end() && (it->second.maxScore >= DANGER_PARTY_SCORE || it->second.deaths >= DANGER_PARTY_DEATHS);
}

void BotEngine::recordDanger(const BotState& bot, uint32_t scriptId, const Position& pos, bool death, const char* why) {
	if (scriptId == 0 || pos.x == 0 || pos.y == 0) {
		return;
	}
	const int32_t radius = death ? DANGER_DEATH_RADIUS : DANGER_NEAR_RADIUS;
	const uint32_t party = dangerPartySize(bot);
	const uint32_t minParty = party >= 2 ? 3 : 2;   // a party that still got hurt needs a bigger one
	const int64_t now = unixNow();
	DangerZone* zone = nullptr;
	for (auto& z : dangerZones_) {
		if (z.scriptId == scriptId && z.center.z == pos.z && chebXY(z.center, pos) <= std::max(z.radius, radius) + 4) {
			zone = &z;
			break;
		}
	}
	if (!zone) {
		DangerZone z;
		z.scriptId = scriptId;
		z.center = pos;
		z.radius = radius;
		z.minParty = minParty;
		dangerZones_.push_back(z);
		zone = &dangerZones_.back();
	}
	zone->radius = std::max(zone->radius, radius);
	zone->minParty = std::max(zone->minParty, minParty);
	zone->score = std::min(DANGER_SCORE_CAP, zone->score + (death ? DANGER_DEATH_SCORE : DANGER_NEAR_SCORE));
	(death ? zone->deaths : zone->nearDeaths)++;
	zone->lastSeenS = now;
	zone->expiresAtS = now + DANGER_TTL_S;
	const DangerZone snap = *zone;   // rebuild may reorder the vector
	rebuildDangerSummary();

	g_botDatabaseTasks().execute(fmt::format(
		"INSERT INTO `bot_hunt_danger` (`script_id`, `pos_x`, `pos_y`, `pos_z`, `radius`, `min_party`, `score`, "
		"`deaths`, `near_deaths`, `last_seen`, `expires_at`) VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}) "
		"ON DUPLICATE KEY UPDATE `radius`=VALUES(`radius`), `min_party`=VALUES(`min_party`), `score`=VALUES(`score`), "
		"`deaths`=VALUES(`deaths`), `near_deaths`=VALUES(`near_deaths`), `last_seen`=VALUES(`last_seen`), "
		"`expires_at`=VALUES(`expires_at`)",
		snap.scriptId, snap.center.x, snap.center.y, static_cast<int>(snap.center.z), snap.radius, snap.minParty,
		snap.score, snap.deaths, snap.nearDeaths, snap.lastSeenS, snap.expiresAtS));

	std::string scriptName;
	for (const auto& s : huntScripts_) {
		if (s.id == scriptId) {
			scriptName = s.name;
			break;
		}
	}
	g_logger().info("[DANGER] record kind={} why={} script={} '{}' bot='{}' party={} pos=({},{},{}) center=({},{},{}) radius={} score={} deaths={} near={} minParty={} established={}",
		death ? "death" : "near_death", why, scriptId, scriptName, bot.name, party, pos.x, pos.y, pos.z,
		snap.center.x, snap.center.y, snap.center.z, snap.radius, snap.score, snap.deaths, snap.nearDeaths,
		snap.minParty, dangerWantsParty(scriptId) ? 1 : 0);
}

void BotEngine::dangerNoteHealth(BotState& bot, const std::shared_ptr<Player>& player) {
	if (!player || !g_configManager().getBoolean(BOT_DANGER_ZONES_ENABLE)) {
		return;
	}
	const int32_t maxHp = player->getMaxHealth();
	if (maxHp <= 0) {
		return;
	}
	const int32_t pct = player->getHealth() * 100 / maxHp;
	if (pct >= DANGER_NEAR_REARM_PCT) {
		dangerNearDisarmed_.erase(bot.guid);
		return;
	}
	if (pct > g_configManager().getNumber(BOT_DANGER_NEAR_DEATH_PCT) || dangerNearDisarmed_.count(bot.guid)) {
		return;
	}
	const HuntScript* worked = nullptr;
	if (!isBotSpawnEngaged(bot, &worked) || !worked || botScriptIsQuest(worked)) {
		return;
	}
	// A PvP fight is not the spawn's fault (doSelfDefense moves the bot to COMBAT/FLEEING).
	if (bot.state == BotAIState::COMBAT || bot.state == BotAIState::FLEEING || bot.state == BotAIState::PK_ATTACK) {
		return;
	}
	dangerNearDisarmed_.insert(bot.guid);
	recordDanger(bot, worked->id, player->getPosition(), false, fmt::format("hp{}%", pct).c_str());
}

// Called from pauseBotForDeath, i.e. from onDeath BEFORE Player::death() teleports the bot, so
// the position is still the death tile and the hunt reservation is still intact.
void BotEngine::dangerNoteDeath(BotState& bot, const std::shared_ptr<Player>& player) {
	dangerNearDisarmed_.erase(bot.guid);
	if (!player || !g_configManager().getBoolean(BOT_DANGER_ZONES_ENABLE)) {
		return;
	}
	if (bot.state == BotAIState::COMBAT || bot.state == BotAIState::FLEEING || bot.state == BotAIState::PK_ATTACK) {
		return;
	}
	if (bot.attackerId != 0) {
		auto attacker = g_game().getCreatureByID(bot.attackerId);
		if (attacker && attacker->getPlayer()) {
			return;   // killed in PvP
		}
	}
	const HuntScript* worked = nullptr;
	if (!isBotSpawnEngaged(bot, &worked) || !worked || botScriptIsQuest(worked)) {
		return;
	}
	recordDanger(bot, worked->id, player->getPosition(), true, "death");
}

// Waypoint choice. Plain NODE waypoints inside a zone are skipped while the next one stays on the
// same floor; anything with an action (ladder, rope, door, stand, teleport...) is kept, because
// skipping it would break the route rather than steer it.
size_t BotEngine::dangerSkipPatrolIdx(const BotState& bot, const HuntScript& s, size_t idx, bool isVirtual) {
	if (dangerZones_.empty() || !g_configManager().getBoolean(BOT_DANGER_ZONES_ENABLE)) {
		return idx;
	}
	const auto& wps = s.patrolWaypoints;
	const size_t n = wps.size();
	size_t j = idx;
	size_t skipped = 0;
	while (j + 1 < n && skipped < n / 2 && wps[j].type == WaypointType::NODE && wps[j + 1].pos.z == wps[j].pos.z
	       && dangerAvoids(bot, s.id, wps[j].pos)) {
		j++;
		skipped++;
	}
	if (skipped > 0) {
		dangerAvoidWp_ += static_cast<uint32_t>(skipped);
		static std::unordered_map<uint32_t, int64_t> s_lastWpLog;
		auto& last = s_lastWpLog[bot.guid];
		if (OTSYS_TIME() - last > 30000) {
			last = OTSYS_TIME();
			g_logger().info("[DANGER] avoid kind=wp bot='{}' script={} virtual={} skip={}..{} of {}",
				bot.name, s.id, isVirtual ? 1 : 0, idx + 1, j, n);
		}
	}
	return j;
}

// Party conversion. After the caller's shuffle, established-danger scripts move to the front so a
// formation tries them first; they are the scripts solo picks now refuse.
void BotEngine::dangerPreferForParty(std::vector<const HuntScript*>& eligible, const char* path) {
	if (dangerSummary_.empty() || eligible.size() < 1) {
		return;
	}
	auto mid = std::stable_partition(eligible.begin(), eligible.end(), [this](const HuntScript* s) { return dangerWantsParty(s->id); });
	const size_t dangerous = static_cast<size_t>(mid - eligible.begin());
	if (dangerous == 0) {
		return;
	}
	dangerConvert_++;
	g_logger().info("[DANGER] convert path={} first={} '{}' dangerous={} eligible={}",
		path, eligible.front()->id, eligible.front()->name, dangerous, eligible.size());
}

void BotEngine::dangerReport(int64_t nowMs) {
	if (nowMs - dangerLastReport_ < 60000) {
		return;
	}
	dangerLastReport_ = nowMs;
	if (dangerAvoidTarget_ + dangerAvoidWp_ + dangerAvoidPick_ + dangerConvert_ == 0) {
		return;
	}
	g_logger().info("[DANGER] summary zones={} scripts={} avoid target={} wp={} pick={} convert={}",
		dangerZones_.size(), dangerSummary_.size(), dangerAvoidTarget_, dangerAvoidWp_, dangerAvoidPick_, dangerConvert_);
	dangerAvoidTarget_ = dangerAvoidWp_ = dangerAvoidPick_ = dangerConvert_ = 0;
}

// /cavebot danger                         list zones
// /cavebot danger add <scriptId> <x> <y> <z> [death|near] [botName]   record a synthetic event (tests)
// /cavebot danger clear [scriptId]        drop zones (memory + table)
// /cavebot danger check <scriptId>        what the consumers would do for that script
std::string BotEngine::dangerCommand(const std::string& args) {
	std::istringstream in(args);
	std::string verb;
	in >> verb;
	if (verb.empty() || verb == "list") {
		rebuildDangerSummary();
		std::string out = fmt::format("[DANGER] zones={} scripts={}\n", dangerZones_.size(), dangerSummary_.size());
		const int64_t now = unixNow();
		for (const auto& z : dangerZones_) {
			out += fmt::format("  script={} center=({},{},{}) r={} score={} deaths={} near={} minParty={} ttl={}h party_only={}\n",
				z.scriptId, z.center.x, z.center.y, z.center.z, z.radius, z.score, z.deaths, z.nearDeaths, z.minParty,
				(z.expiresAtS - now) / 3600, dangerWantsParty(z.scriptId) ? 1 : 0);
		}
		return out;
	}
	if (verb == "add") {
		uint32_t sid = 0;
		int x = 0, y = 0, z = 0;
		std::string kind, botName;
		in >> sid >> x >> y >> z >> kind >> botName;
		if (sid == 0 || x <= 0 || y <= 0) {
			return "Usage: danger add <scriptId> <x> <y> <z> [death|near] [botName]";
		}
		BotState probe;
		probe.name = botName.empty() ? "(cmd)" : botName;
		const BotState* who = &probe;
		for (const auto& b : bots_) {
			if (!botName.empty() && strcasecmp(b.name.c_str(), botName.c_str()) == 0) {
				who = &b;
				break;
			}
		}
		recordDanger(*who, sid, Position(static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint8_t>(z)), kind != "near", "command");
		return fmt::format("[DANGER] recorded {} for script {} at ({},{},{}); party_only={}", kind != "near" ? "death" : "near_death", sid, x, y, z, dangerWantsParty(sid) ? 1 : 0);
	}
	if (verb == "clear") {
		uint32_t sid = 0;
		in >> sid;
		const size_t before = dangerZones_.size();
		if (sid == 0) {
			dangerZones_.clear();
			g_botDatabaseTasks().execute("DELETE FROM `bot_hunt_danger`");
		} else {
			std::erase_if(dangerZones_, [sid](const DangerZone& z) { return z.scriptId == sid; });
			g_botDatabaseTasks().execute(fmt::format("DELETE FROM `bot_hunt_danger` WHERE `script_id`={}", sid));
		}
		rebuildDangerSummary();
		return fmt::format("[DANGER] cleared {} zone(s)", before - dangerZones_.size());
	}
	if (verb == "check") {
		uint32_t sid = 0;
		in >> sid;
		const HuntScript* s = nullptr;
		for (const auto& h : huntScripts_) {
			if (h.id == sid) {
				s = &h;
				break;
			}
		}
		if (!s) {
			return fmt::format("[DANGER] check: no script {}", sid);
		}
		BotState solo;
		size_t inZone = 0;
		for (const auto& wp : s->patrolWaypoints) {
			inZone += dangerAvoids(solo, sid, wp.pos) ? 1 : 0;
		}
		return fmt::format("[DANGER] check script={} '{}' party_only={} solo_wps_in_zone={}/{}", sid, s->name,
			dangerWantsParty(sid) ? 1 : 0, inZone, s->patrolWaypoints.size());
	}
	return "Usage: danger [list|add|clear|check]";
}

// ============================================================================
// Test drivers (#30, #96): bot-scoped /cavebot verbs the harness uses to reach a state in seconds
// that the AI reaches in minutes.
//   huntpatrol <scriptId>  reserve the hunt and start PATROLLING at patrol waypoint 1 (teleport)
//   testdie                kill the bot where it stands (no attacker), the GM-tool death
//   aipause on|off         freeze the bot (no walking, no defense) so a test can kill it
//   testmonster <name>     spawn a monster next to the bot (a real monster kill for #107)
//   testhp <n>             set the bot's health (so a test kill lands before anyone else's)
// ============================================================================
std::string BotEngine::learningBotCommand(BotState& bot, const std::shared_ptr<Player>& player, const std::string& botName, const std::string& command) {
	if (!player || player->isRemoved()) {
		return fmt::format("Bot '{}' is not awake.", botName);
	}
	if (command == "testdie") {
		const Position at = player->getPosition();
		const uint32_t sid = bot.huntScriptId;
		const HuntPhase ph = bot.huntPhase;
		CombatDamage damage;
		damage.primary.type = COMBAT_PHYSICALDAMAGE;
		damage.primary.value = -(player->getHealth() + 1000);
		g_game().combatChangeHealth(nullptr, player, damage);
		return fmt::format("Bot '{}' testdie at ({},{},{}) script={} phase={}", botName, at.x, at.y, at.z, sid, phaseName(ph));
	}
	if (command == "aipause on" || command == "aipause off") {
		bot.aiPaused = command == "aipause on";
		// "no defense" must include the heal slot: a 5-HP victim otherwise casts exura ico (#107).
		if (bot.aiPaused) {
			s_testNoDefense.insert(bot.guid);
		} else {
			s_testNoDefense.erase(bot.guid);
		}
		if (bot.aiPaused) {
			player->setAttackedCreature(nullptr);
			player->stopWalk();
		}
		return fmt::format("Bot '{}' AI {}.", botName, bot.aiPaused ? "paused (stands still, no defense)" : "resumed");
	}
	if (command.rfind("testhp ", 0) == 0) {
		int32_t hp = 0;
		try {
			hp = std::stoi(command.substr(7));
		} catch (...) {
			return "Usage: testhp <hitpoints>";
		}
		hp = std::clamp<int32_t>(hp, 1, player->getMaxHealth());
		player->changeHealth(hp - player->getHealth());
		return fmt::format("Bot '{}' health set to {}/{}.", botName, player->getHealth(), player->getMaxHealth());
	}
	if (command.rfind("testmonster ", 0) == 0) {
		const std::string name = command.substr(12);
		auto monster = Monster::createMonster(name);
		if (!monster) {
			return fmt::format("No monster type '{}'.", name);
		}
		if (!g_game().placeCreature(monster, player->getPosition(), true, false)) {
			return fmt::format("Could not place '{}' near bot '{}'.", name, botName);
		}
		return fmt::format("Spawned '{}' next to bot '{}' at ({},{},{}).", name, botName, monster->getPosition().x, monster->getPosition().y, monster->getPosition().z);
	}
	uint32_t sid = 0;
	try {
		sid = static_cast<uint32_t>(std::stoul(command.substr(11)));
	} catch (...) {
		return "Usage: huntpatrol <scriptId>";
	}
	const HuntScript* target = nullptr;
	for (const auto& s : huntScripts_) {
		if (s.id == sid) {
			target = &s;
			break;
		}
	}
	if (!target || target->patrolWaypoints.empty()) {
		return fmt::format("No hunt script {} with patrol waypoints.", sid);
	}
	if (bot.huntScriptId > 0) {
		activeHunts_.erase(bot.huntScriptId);
		for (const auto& s : huntScripts_) {
			if (s.id == bot.huntScriptId && !s.spawnGroup.empty()) {
				activeSpawnGroups_.erase(s.spawnGroup);
				break;
			}
		}
	}
	activeHunts_[target->id] = bot.guid;
	if (!target->spawnGroup.empty()) {
		activeSpawnGroups_[target->spawnGroup] = bot.guid;
	}
	bot.huntScriptId = target->id;
	logHuntAssign(bot, target->id);
	bot.huntTownId = target->townId;
	bot.huntStartTime = OTSYS_TIME();
	bot.huntEndTime = bot.huntStartTime + 3600 * 1000LL;
	bot.huntKillCount = 0;
	bot.huntWaypointIdx = 0;
	bot.huntPatrolCycles = 0;
	bot.huntTargetId = 0;
	bot.huntChaseFailCount = 0;
	bot.huntIgnoredMonsters.clear();
	bot.huntWaypointSkipCount = 0;
	bot.hasWalkTarget = false;
	const Position dest = target->patrolWaypoints[0].pos;
	BOT_TELEPORT(player, dest, true);
	bot.currentPos = dest;
	bot.lastPos = dest;
	beginHuntPhase(bot, HuntPhase::PATROLLING, 0);
	return fmt::format("Bot '{}' patrolling '{}' (script={} group={}) from ({},{},{}).", botName, target->name, target->id,
		target->spawnGroup, dest.x, dest.y, dest.z);
}
