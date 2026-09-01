/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_party.cpp — party + autonomous party-hunt system (Phases 9-10)
//
// BOT_NAV_REALISM Phase 11 module split. Compiles into the SAME libbot_engine.so
// as bot_engine.cpp, so /cavebot reload is unchanged. Shared includes, engine-local
// types and the BotEngine class declaration all live in bot_engine_impl.hpp.
//
// Carved out only after tools/botnavsim/module_promote.py reported zero external
// dependencies for this range.
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

// ============================================================================
// Party system (Phase 9)
// ============================================================================




std::vector<uint32_t> BotEngine::findBotsForParty(uint8_t baseVocId, uint32_t playerLevel,
	uint32_t count, const std::unordered_set<uint32_t>& excludeGuids,
	uint32_t minLevelOverride, uint32_t maxLevelOverride, bool preferActive) {

	// An explicit [min,max] from the /party command replaces the derived window entirely.
	uint32_t minLevel = minLevelOverride > 0 ? minLevelOverride : playerLevel * 2 / 3;
	uint32_t maxLevel = maxLevelOverride > 0 ? maxLevelOverride : playerLevel * 3 / 2;

	// Collect candidates into tiers
	std::vector<std::pair<int32_t, uint32_t>> candidates; // (tier, guid)

	for (auto& bot : bots_) {
		if (excludeGuids.count(bot.guid)) { continue; }
		if (s_partyLeaderId.count(bot.guid)) { continue; } // already in a party
		if (s_botToPartyHunt.count(bot.guid)) { continue; } // already in a party hunt
		// Don't recruit a bot committed to an Adventurer's-Stone trip — doAdventurerStone
		// preempts the state switch in processBot, so it would never run doParty/follow and
		// would walk off to the AdvStone forcefield/temple instead of following the EK.
		// (setupSupport/setupVirtualSupport also clearAdvStoneState defensively for the
		// selection→setup timing gap.)
		if (bot.advStoneActive) { continue; }
		// Same reasoning for a fishing trip: it owns the bot's dwell timer and its own walk
		// phases, so recruiting one mid-session would strand the run — and the state snapshot
		// taken below copies dwellUntil verbatim, so a stretched value would resurface later.
		if (isFishing(bot.guid)) { continue; }
		// And the same for a house visit: it owns the dwell timer, holds three claims, and may
		// have a Lua training loop running against a dummy. Conscripting one strands all of it.
		if (isHouseVisiting(bot.guid)) { continue; }
		// A pinned bot must not be conscripted either. `pin on` promises "no auto tasks", but
		// until now only doActivityReroll honoured it, so a pinned IDLE bot was not merely
		// eligible here -- it was a PREFERRED (tier-2) recruit, and party follow would then
		// teleport it away. That breaks debug pins mid-observation and drags perf-harness probe
		// bots off their itinerary, silently moving the anchor a measurement depends on.
		if (s_debugPinned.count(bot.guid)) { continue; }

		uint8_t baseVoc = getBaseVocation(bot.vocationId);
		if (baseVoc != baseVocId) { continue; }

		// Allow hibernated bots: they'll be activated by setupSupport via activateBot.
		// Live bots still need a non-removed Player; hibernated bots fall back to cachedLevel.
		auto player = bot.getPlayer();
		uint32_t botLevel = 0;
		if (player) {
			if (player->isRemoved()) { continue; }
			botLevel = player->getLevel();
		} else {
			if (bot.cachedLevel == 0) { continue; } // no level info — can't filter safely
			botLevel = bot.cachedLevel;
		}
		if (botLevel < minLevel || botLevel > maxLevel) { continue; }

		int32_t tier;
		if (!bot.active) {
			// preferActive (human-led recruitment): a never-logged-in bot is the LAST choice,
			// not the first. Conscripting one promotes it to active=true, and while
			// s_reclaimToInactive walks that back when hibernation eventually comes for it,
			// spending an already-logged-in bot first avoids the churn entirely. Tier 6 rather
			// than a hard skip: a large quest party still needs this overflow pool to fill.
			tier = preferActive ? 6 : 1;
		} else if (bot.state == BotAIState::IDLE) {
			tier = 2;
		} else if (bot.state == BotAIState::DWELLING) {
			tier = 3;
		} else if (bot.state == BotAIState::TRAVELING || bot.state == BotAIState::COMBAT
				|| bot.state == BotAIState::FLEEING) {
			tier = 4;
		} else {
			tier = 5; // HUNTING, PK_ATTACK, or anything else
		}

		candidates.emplace_back(tier, bot.guid);
	}

	// Sort by tier, then shuffle within each tier
	std::sort(candidates.begin(), candidates.end(),
		[](const auto& a, const auto& b) { return a.first < b.first; });

	// Shuffle within each tier group
	size_t start = 0;
	while (start < candidates.size()) {
		size_t end = start;
		while (end < candidates.size() && candidates[end].first == candidates[start].first) end++;
		if (end - start > 1) {
			for (size_t i = end - 1; i > start; i--) {
				size_t j = start + (uniform_random(0, static_cast<int>(i - start)));
				std::swap(candidates[i], candidates[j]);
			}
		}
		start = end;
	}

	std::vector<uint32_t> result;
	for (const auto& [tier, guid] : candidates) {
		if (result.size() >= count) break;
		result.push_back(guid);
	}
	return result;
}

bool BotEngine::activateBotForParty(uint32_t guid, uint32_t leaderCreatureId, const Position& summonPos) {
	auto it = guidToIndex_.find(guid);
	if (it == guidToIndex_.end()) return false;

	auto& bot = bots_[it->second];
	auto player = bot.getPlayer();
	if (!player) return false;

	bool wasInactive = !bot.active;

	// Remember only whether this bot was ever logged in. The full pre-party BotState snapshot
	// is gone (BOT_PARTY_INVITE_RENDEZVOUS §3.7): exit rerolls in place instead of rewinding.
	// This bool still matters, because a never-active bot conscripted into a party would
	// otherwise stay active=true forever and inflate the logged-in population.
	s_partyWasInactive[guid] = wasInactive;

	if (wasInactive) {
		// Use existing activation flow (equip, HP/mana, cast broadcasting, etc.)
		activateBot(guid);
		// activateBot sets state=IDLE, teleports to temple — we override below
	} else {
		// Active bot: restore HP/mana
		player->health = player->healthMax;
		player->mana = player->getMaxMana();
		g_game().addCreatureHealth(player);
		g_game().addPlayerMana(player);
	}

	// Override state to PARTY
	bot.state = BotAIState::PARTY;
	s_partyLeaderId[guid] = leaderCreatureId;

	// Save and set PvP toggle (party bots don't PvP)
	s_partyPrevSecureMode[guid] = player->secureMode;
	player->setSecureMode(true);

	// Teleport to summon position (near the leader)
	BOT_TELEPORT(player, summonPos, true);
	bot.currentPos = summonPos;
	bot.lastPos = summonPos;

	// Clear walk/navigation state (don't want stale auto-walks)
	if (!player->listWalkDir.empty()) {
		player->listWalkDir.clear();
		player->stopEventWalk();
	}
	bot.hasWalkTarget = false;
	bot.followingCityRoute = false;
	bot.cityRouteWps.clear();
	bot.cityRouteIdx = 0;

	// Clear hunt target (don't want leftover targets from hunt)
	bot.huntTargetId = 0;
	player->setAttackedCreature(nullptr);

	castLog(bot, fmt::format("PARTY: Activated for party (leader={}, wasInactive={}, pos=({},{},{}))",
		leaderCreatureId, wasInactive, summonPos.x, summonPos.y, summonPos.z));

	return true;
}

// BOT_PARTY_INVITE_RENDEZVOUS: the single teardown that frees a bot from whatever it was doing
// so it can serve a party (or leave the world). Extracted from setupSupport's inline block so
// the four callers cannot drift apart: invite acceptance, /party formation, the assembly
// deadline teardown, and hibernateBot's reclaim-to-inactive routing.
//
// Releasing huntScriptId WITH its activeHunts_/activeSpawnGroups_ entries is the load-bearing
// part. Dropping the field alone would strand the reservation forever: a bot in PARTY state
// runs no hunt path and no reroll, so nothing downstream would ever hand that spawn back.
void BotEngine::releasePartyMemberActivity(BotState& bot, const char* reason) {
	if (bot.huntScriptId > 0) {
		activeHunts_.erase(bot.huntScriptId);
		for (const auto& hs : huntScripts_) {
			if (hs.id == bot.huntScriptId && !hs.spawnGroup.empty()) {
				activeSpawnGroups_.erase(hs.spawnGroup);
				break;
			}
		}
		bot.huntScriptId = 0;
	}
	bot.huntTargetId = 0;
	bot.huntKillCount = 0;
	bot.huntPhase = HuntPhase::PREPARING;
	bot.huntWaypointIdx = 0;
	bot.huntWaypointSkipCount = 0;
	bot.huntIgnoredMonsters.clear();

	// The three activities that own a bot's dwell timer and hold their own claims. Each already
	// no-ops when the bot is not in that activity.
	clearAdvStoneState(bot);
	clearFishingRun(bot.guid);
	endHouseVisit(bot.guid, reason);
	endShrineVisit(bot.guid, reason);
	endIceFishSession(bot, reason);
	// BOT_AMBIENT_ROAM: whatever is conscripting this bot now owns it. Only the behavioural
	// session goes — the ledger entry stays, because the bot is still awake near the player and
	// therefore still one of the extra bodies the roam reserve paid for. Releasing the slot here
	// would let the next injection push the ring past its true ceiling.
	s_roam.erase(bot.guid);

	// Travel triplet — leaving pendingHuntAfterTravel set would start a fresh hunt on arrival.
	bot.travelDestTownId = 0;
	bot.travelPhase.clear();
	bot.pendingHuntAfterTravel = false;
	bot.travelWaitUntil = 0;

	// Nav/walk state. doActivityReroll hard-returns while any of hasWalkTarget /
	// followingCityRoute / pendingNavDest is set, so a caller that skipped these would leave
	// the bot standing still forever rather than picking a new activity.
	bot.hasWalkTarget = false;
	bot.followingCityRoute = false;
	bot.cityRouteWps.clear();
	bot.cityRouteIdx = 0;
	bot.pendingNavDest.clear();
	bot.currentPOI = nullptr;
	bot.hasDepotTarget = false;
	bot.stopCooldownUntil = 0;

	// Gang membership is NOT covered by the state checks elsewhere: a bot mid gang-staging
	// sits at state=IDLE, so the invite classifier does not see it as busy, and gang staging
	// preempts earlier in doIdle than assembly staging — it would win the race and drag the
	// new party member off to a PK burst.
	leaveGang(bot.guid);

	// Post-combat return walk and PK re-engage are keyed by guid and outlive the fight. Left
	// set, they walk the bot back to a pre-combat spot after it leaves the party.
	s_returnPos.erase(bot.guid);
	s_returnStartTime.erase(bot.guid);
	s_reengageTarget.erase(bot.guid);
	s_reengageUntil.erase(bot.guid);

	if (auto player = bot.getPlayer()) {
		if (!player->listWalkDir.empty()) {
			player->listWalkDir.clear();
			player->stopEventWalk();
		}
		player->setAttackedCreature(nullptr);
		player->setFollowCreature(nullptr);
	}
}

// Chebyshev helper. Forward-declared because its definition is a file-scope `static inline`
// further down this TU (the trail section), and C++ requires a declaration before use — the
// same ordering constraint that governs carving these modules apart.
static inline int32_t trailCheb(const Position& a, const Position& b);

// ============================================================================
// BOT_PARTY_INVITE_RENDEZVOUS — invite detection and acceptance
//
// Detection is LEADER-side and uses only public stock API. There is no accessor for an invited
// player's own invitePartyList (it is private, and we will not add one just for this), but every
// invite also lives in some party's inviteList, every party is reachable through its leader, and
// Party::getInvitees() is public. A hibernated bot cannot be click-invited at all — it is not in
// the world — so polling the online leaders sees everything that matters.
//
// The engine's own formations never show up here: they call invitePlayer + joinParty back-to-back
// within one tick, so the invite never survives to a poll.
// ============================================================================

void BotEngine::tickPartyInvites(int64_t nowMs) {
	if (!inviteCfg_.enable) return;
	// O(1) presence gate. An invite can only originate from a real player (or the invitebot
	// debug command), and currentAnchorPts_ is already refreshed at tick top, so an empty
	// server costs one comparison. Slightly conservative: a cast viewer cannot invite but
	// still keeps the gate open, which is harmless.
	if (currentAnchorPts_.empty() && s_inviteDebugKeepAlive.empty()) return;
	if (nowMs - s_lastInvitePollMs < inviteCfg_.pollMs) return;
	s_lastInvitePollMs = nowMs;

	// ---- Re-validate what is already pending, and fire the ones whose time has come.
	for (auto it = s_pendingInvites.begin(); it != s_pendingInvites.end();) {
		const uint32_t botGuid = it->first;
		PendingInvite& pi = it->second;

		auto botIt = guidToIndex_.find(botGuid);
		auto inviterCreature = g_game().getCreatureByID(pi.inviterCreatureId);
		auto inviter = inviterCreature ? inviterCreature->getPlayer() : nullptr;
		auto botPlayer = (botIt != guidToIndex_.end()) ? bots_[botIt->second].getPlayer() : nullptr;

		// Any of these means the invite is no longer ours to answer: revoked, leader gone, party
		// filled, bot left the world, or the bot already joined something. All are legitimate.
		if (botIt == guidToIndex_.end() || !botPlayer || botPlayer->isRemoved() || !inviter
		    || !inviter->isInviting(botPlayer) || botPlayer->getParty()) {
			it = s_pendingInvites.erase(it);
			continue;
		}

		auto& bot = bots_[botIt->second];
		const bool busy = bot.state == BotAIState::COMBAT || bot.state == BotAIState::FLEEING
			|| bot.state == BotAIState::PK_ATTACK || bot.deathPauseUntil > 0;

		if (pi.phase == InvitePhase::HOLDING) {
			if (!busy) {
				// Fight resolved — fall through to the ordinary human-like accept delay.
				pi.phase = InvitePhase::ACCEPT_WAIT;
				pi.actAtMs = nowMs + uniform_random(inviteCfg_.acceptMinMs, inviteCfg_.acceptMaxMs);
			} else if (nowMs >= pi.actAtMs) {
				declinePartyInvite(bot, inviter->getParty(), "hold_expired");
				s_prv.declinedHoldExpired++;
				it = s_pendingInvites.erase(it);
				continue;
			} else if (nowMs - pi.lastHoldLogMs > 10000) {
				pi.lastHoldLogMs = nowMs;
				g_logger().info("[BotEngine] [PINVITE] hold: bot='{}' state={} remainMs={}",
					bot.name, botStateName(bot.state), pi.actAtMs - nowMs);
			}
			++it;
			continue;
		}

		// ACCEPT_WAIT: a fight that STARTED during the wait flips us to holding rather than
		// dragging the bot out of it.
		if (busy) {
			pi.phase = InvitePhase::HOLDING;
			pi.actAtMs = nowMs + inviteCfg_.holdMaxMs;
			++it;
			continue;
		}
		if (nowMs < pi.actAtMs) { ++it; continue; }

		auto party = inviter->getParty();
		if (party && acceptPartyInvite(bot, inviter, party)) {
			it = s_pendingInvites.erase(it);
		} else {
			// Drop rather than spin — but SAY SO. An earlier version erased silently here, and a
			// single real detection that never converted then had no diagnostic trail at all.
			// joinParty is the likely refuser (its Lua onJoin hook, or an inviteList whose
			// shared_ptr no longer matches this bot's current Player), and it erases the invite
			// before some of its own failure paths, so re-discovery cannot always recover it.
			g_logger().warn("[BotEngine] [PINVITE] accept-failed: bot='{}' inviter='{}' "
				"hasParty={} isInviting={} botInParty={}",
				bot.name, inviter->getName(), party != nullptr,
				inviter->isInviting(bot.getPlayer()),
				bot.getPlayer() && bot.getPlayer()->getParty() != nullptr);
			it = s_pendingInvites.erase(it);
		}
	}

	// ---- Discover new invites.
	for (const auto& [id, p] : g_game().getPlayers()) {
		if (!p) continue;
		// Only real players invite, except for the all-bot invitebot test party.
		if (p->isBotPlayer() && s_inviteDebugKeepAlive.count(p->getGUID()) == 0) continue;
		auto party = p->getParty();
		if (!party || party->getLeader() != p) continue;
		for (const auto& invitee : party->getInvitees()) {
			if (!invitee || !invitee->isBotPlayer()) continue;
			const uint32_t g = invitee->getGUID();
			if (s_pendingInvites.count(g) > 0) continue;
			auto botIt = guidToIndex_.find(g);
			if (botIt == guidToIndex_.end()) continue;
			onBotInvited(bots_[botIt->second], p, party);
		}
	}
}

void BotEngine::onBotInvited(BotState& bot, const std::shared_ptr<Player>& inviter,
	const std::shared_ptr<Party>& party) {
	const int64_t nowMs = OTSYS_TIME();
	s_prv.detected++;

	// A bot already committed to an autonomous party hunt declines: a player must not be able to
	// silently dissolve a running 2-3h team by clicking one of its members.
	if (s_botToPartyHunt.count(bot.guid) > 0) {
		declinePartyInvite(bot, party, "party_hunt");
		s_prv.declinedPartyHunt++;
		return;
	}
	// Already walking in for somebody else.
	if (s_rvMember.count(bot.guid) > 0) {
		declinePartyInvite(bot, party, "assembling");
		s_prv.declinedAssembling++;
		return;
	}
	// Pinned bots decline every invite. Same promise findBotsForParty now keeps: a pin means the
	// operator (or the perf harness) owns this bot's position, and accepting would hand it to
	// party-follow, which teleports.
	if (s_debugPinned.count(bot.guid) > 0) {
		declinePartyInvite(bot, party, "pinned");
		return;
	}

	PendingInvite pi;
	pi.inviterCreatureId = inviter->getID();
	pi.detectedMs = nowMs;

	// A bot mid gang-staging sits at state=IDLE, so the state set alone would call it free.
	// It is not: gang staging preempts earlier in doIdle and would win the race.
	const bool busy = bot.state == BotAIState::COMBAT || bot.state == BotAIState::FLEEING
		|| bot.state == BotAIState::PK_ATTACK || bot.deathPauseUntil > 0
		|| s_gangByGuid.count(bot.guid) > 0;
	if (busy) {
		// Hold, do not decline: the bot finishes the fight it was already in, then joins.
		pi.phase = InvitePhase::HOLDING;
		pi.actAtMs = nowMs + inviteCfg_.holdMaxMs;
	} else {
		pi.phase = InvitePhase::ACCEPT_WAIT;
		pi.actAtMs = nowMs + uniform_random(inviteCfg_.acceptMinMs, inviteCfg_.acceptMaxMs);
	}
	s_pendingInvites[bot.guid] = pi;

	g_logger().info("[BotEngine] [PINVITE] detected: bot='{}' inviter='{}' state={} decision={} delayMs={}",
		bot.name, inviter->getName(), botStateName(bot.state),
		pi.phase == InvitePhase::HOLDING ? "hold" : "accept_wait", pi.actAtMs - nowMs);
}

void BotEngine::declinePartyInvite(BotState& bot, const std::shared_ptr<Party>& party,
	const char* reason) {
	if (party) {
		if (auto botPlayer = bot.getPlayer()) {
			// Stock Canary has no bot-side "reject" entry point (playerJoinParty needs a client,
			// and revoke is leader-only), so clear the invite directly. removeInvite erases both
			// sides and disbands a party left empty.
			party->removeInvite(botPlayer);
		}
	}
	g_logger().info("[BotEngine] [PINVITE] declined: bot='{}' reason={}", bot.name, reason);
}

bool BotEngine::acceptPartyInvite(BotState& bot, const std::shared_ptr<Player>& inviter,
	const std::shared_ptr<Party>& party) {
	auto botPlayer = bot.getPlayer();
	// Same guards Game::playerJoinParty applies before letting a real client join.
	if (!botPlayer || !inviter || !party) return false;
	if (!inviter->isInviting(botPlayer) || botPlayer->getParty()) return false;

	const int64_t nowMs = OTSYS_TIME();
	releasePartyMemberActivity(bot, "invite_accept");

	if (!party->joinParty(botPlayer)) return false;

	s_partyLeaderId[bot.guid] = inviter->getID();
	s_partyWasInactive[bot.guid] = !bot.active;
	s_partyPrevSecureMode[bot.guid] = botPlayer->secureMode;
	botPlayer->setSecureMode(true);

	// Close enough to just fall in step; otherwise walk in (or teleport, if assembly is off).
	const Position leaderPos = inviter->getPosition();
	const int32_t d = trailCheb(bot.currentPos, leaderPos);
	const char* route = "near";
	if (bot.currentPos.z == leaderPos.z && d <= PARTY_HUNT_SUPPORT_FOLLOW_DIST) {
		bot.state = BotAIState::PARTY;
	} else if (asmCfg_.enable) {
		const uint32_t asmId = enrollHumanLedMember(bot, inviter);
		route = (asmId > 0 && s_rvMember.count(bot.guid) > 0
		         && s_partyAssembly.count(asmId) > 0) ? "walk" : "near";
		// TRAVELLING entry means a cross-town trip precedes the walk.
		if (auto aIt = s_partyAssembly.find(asmId); aIt != s_partyAssembly.end()) {
			for (const auto& m : aIt->second.members) {
				if (m.guid == bot.guid && m.phase == RvPhase::TRAVELLING) route = "travel";
			}
		}
	} else {
		// Assembly disabled — today's instant assembly, the valid intermediate deploy stage.
		failAssemblyMemberToTeleport(bot, leaderPos, "rv_disabled");
		bot.state = BotAIState::PARTY;
		route = "teleport";
	}

	s_prv.accepted++;
	const int64_t latency = nowMs - (s_pendingInvites.count(bot.guid)
		? s_pendingInvites[bot.guid].detectedMs : nowMs);
	g_logger().info("[BotEngine] [PINVITE] accepted: bot='{}' leader='{}' latencyMs={} dist={} route={}",
		bot.name, inviter->getName(), latency, d, route);
	return true;
}

// ============================================================================
// BOT_PARTY_INVITE_RENDEZVOUS — the assembly supervisor
//
// Members walk to their leader instead of being teleported onto it. The supervisor only
// OBSERVES and issues phase transitions; the actual walking happens in each member's own
// processBot pass, which is what keeps the cost flat at 500 bots.
// ============================================================================

bool BotEngine::assemblyActiveForPartyHunt(uint32_t partyHuntId) const {
	if (partyHuntId == 0 || s_partyAssembly.empty()) return false;
	for (const auto& [id, asmb] : s_partyAssembly) {
		if (asmb.kind == RvKind::BOT_LED_HUNT && asmb.partyHuntId == partyHuntId) return true;
	}
	return false;
}

// Remove one member from its assembly WITHOUT restoring or teleporting anything. A member
// dropped mid-assembly simply stops assembling; if it had already joined the Canary party
// (HUMAN_LED does that at commit time) the caller is responsible for that side.
void BotEngine::dropAssemblyMember(uint32_t guid, const char* reason) {
	auto mIt = s_rvMember.find(guid);
	if (mIt == s_rvMember.end()) return;
	const uint32_t assemblyId = mIt->second;
	s_rvMember.erase(mIt);

	auto aIt = s_partyAssembly.find(assemblyId);
	if (aIt == s_partyAssembly.end()) return;
	auto& members = aIt->second.members;
	std::erase_if(members, [guid](const RvMember& m) { return m.guid == guid; });
	g_logger().info("[BotEngine] [PARTYRV] assembly #{} member guid={} dropped reason={}",
		assemblyId, guid, reason);
	if (members.empty()) {
		s_partyAssembly.erase(aIt);
	}
}

// Which phase a member starts in: a different town means it has to get there first.
RvPhase BotEngine::pickAssemblyEntryPhase(const BotState& bot, uint32_t anchorTownId) const {
	if (anchorTownId == 0) return RvPhase::WALKING_IN;
	// Decide on where the bot ACTUALLY IS, not on bot.townId. townId is the bot's home/registered
	// town and can disagree with its position (it lags a teleport, and a bot can be standing in
	// another town for all sorts of reasons) — trusting it would put a bot already next to its
	// leader onto a cross-world boat trip.
	const uint32_t hereTown = findNearestTown(bot.currentPos);
	if (hereTown > 0 && hereTown != anchorTownId) return RvPhase::TRAVELLING;
	return RvPhase::WALKING_IN;
}

// An off-screen tile near the anchor to stage a distant member on, so it walks the last leg in view
// instead of making a real multi-minute cross-world journey nobody can watch. This is RENDEZVOUS
// §3.5's "staged walk-in", which the first implementation skipped in favour of startTravel — the
// reason /party recruits took 30-75s to show up.
//
// Rings outward from ~10 tiles because that is comfortably beyond the client viewport (8x6) yet
// close enough that the visible walk takes seconds. Every candidate must be BOTH invisible to any
// observer AND safe, which are two different tests: wouldBeSeenByAnchor is an exact port of
// ProtocolGame::canSee over every anchor (so a cast-watched leader counts as a camera), while
// chooseWakePosition applies isUnsafeWakeTile — floor-change/teleport/depot/magic-field tiles,
// registered MoveEvents, house tiles — plus occupancy.
Position BotEngine::chooseAssemblyStagingPos(BotState& bot, const Position& anchor) {
	refreshAnchorsIfStale(100);
	static constexpr int32_t kRings[] = { 10, 13, 16, 19 };
	// Eight compass directions per ring; first safe + unseen candidate wins.
	static constexpr int32_t kDx[] = { 1, -1, 0, 0, 1, 1, -1, -1 };
	static constexpr int32_t kDy[] = { 0, 0, 1, -1, 1, -1, 1, -1 };
	for (int32_t r : kRings) {
		for (int i = 0; i < 8; i++) {
			Position cand = anchor;
			const int32_t nx = static_cast<int32_t>(anchor.x) + kDx[i] * r;
			const int32_t ny = static_cast<int32_t>(anchor.y) + kDy[i] * r;
			if (nx < 1 || ny < 1 || nx > 65000 || ny > 65000) continue;
			cand.x = static_cast<uint16_t>(nx);
			cand.y = static_cast<uint16_t>(ny);
			if (wouldBeSeenByAnchor(cand, ASSEMBLY_OFFSCREEN_MARGIN)) continue;
			// chooseSafePartyFollowPos, NOT chooseWakePosition. Both vet a tile, but only this one
			// stays NEAR the center it is given. chooseWakePosition's repair ladder falls back to
			// the bot's own route/virtual position, and it did exactly that live: recruits were
			// "staged" 873-1146 tiles from the leader, right beside where they woke, and then set
			// off walking across the world. One died on the way.
			std::unordered_set<uint64_t> stagingReserved;
			const Position vetted = chooseSafePartyFollowPos(bot, cand, stagingReserved);
			if (vetted.x == 0) continue;
			// Two independent post-conditions, because a vet that relocates can break either:
			// still invisible, AND still actually near the leader. The distance check is what the
			// first version lacked — a tile 1000 tiles away is trivially "off-screen".
			if (wouldBeSeenByAnchor(vetted, ASSEMBLY_OFFSCREEN_MARGIN)) continue;
			if (vetted.z != anchor.z) continue;
			if (trailCheb(vetted, anchor) > ASSEMBLY_STAGE_MAX_DIST) continue;
			return vetted;
		}
	}
	// Nothing off-screen, safe AND near the leader — the caller must NOT teleport. Logged because
	// the alternative is a silent multi-minute journey that looks like the feature is broken.
	g_logger().info("[BotEngine] [PARTYRV] no staging tile for '{}' near ({},{},{}) — falling back "
		"to a real approach", bot.name, anchor.x, anchor.y, anchor.z);
	return Position();
}

// One decision point for "how does this member get to the leader", so enrolment, the FINISHING
// transition and the death-rejoin cannot drift apart.
void BotEngine::beginAssemblyApproach(BotState& bot, const PartyAssembly& asmb, RvMember& m) {
	const int64_t nowMs = OTSYS_TIME();
	m.phaseSinceMs = nowMs;
	m.travelSinceMs = nowMs;

	// Already walkable-close on the same floor: just walk, no teleport at all.
	const bool sameFloorNear = (bot.currentPos.z == asmb.anchor.z)
		&& trailCheb(bot.currentPos, asmb.anchor) <= ASSEMBLY_WALK_FROM_DIST;
	if (sameFloorNear) {
		m.phase = RvPhase::WALKING_IN;
		return;
	}

	// Teleport ONLY off-screen -> off-screen. If an observer can currently see the member, moving it
	// would be a visible pop-OUT, which is the same sin as a pop-in at the far end; such a member
	// travels for real instead.
	const bool memberVisibleNow = wouldBeSeenByAnchor(bot.currentPos, 0);
	if (!memberVisibleNow) {
		if (const Position staging = chooseAssemblyStagingPos(bot, asmb.anchor); staging.x != 0) {
			if (auto player = bot.getPlayer()) {
				BOT_TELEPORT(player, staging, true);
				bot.currentPos = staging;
				bot.lastPos = staging;
				if (!player->listWalkDir.empty()) {
					player->listWalkDir.clear();
					player->stopEventWalk();
				}
				bot.hasWalkTarget = false;
				bot.pendingNavDest.clear();
				s_prv.asmStaged++;
				m.phase = RvPhase::WALKING_IN;
				g_logger().info("[BotEngine] [PARTYRV] staged '{}' off-screen at ({},{},{}) — {} tiles "
					"from the leader, walking in",
					bot.name, staging.x, staging.y, staging.z, trailCheb(staging, asmb.anchor));
				return;
			}
		}
	}

	// No safe off-screen staging tile (or the member is being watched): fall back to a real journey.
	m.phase = pickAssemblyEntryPhase(bot, asmb.anchorTownId);
	if (m.phase == RvPhase::TRAVELLING) {
		startTravel(bot, asmb.anchorTownId);
	}
}

// The declared fallback: no route, or the budget ran out. Teleporting next to a watching human
// is exactly the pop-in this feature removes, so it is counted, never silent — an eternally
// lost bot is still worse than one visible teleport.
void BotEngine::failAssemblyMemberToTeleport(BotState& bot, const Position& anchor, const char* reason) {
	auto player = bot.getPlayer();
	if (!player) return;
	// Full teardown BEFORE the teleport. A member force-flipped to PARTY while still FINISHING
	// would otherwise keep its huntScriptId in activeHunts_ forever: in PARTY state no hunt path
	// and no reroll runs for it again, so nothing downstream ever hands that spawn back.
	releasePartyMemberActivity(bot, reason);
	Position placeAt = chooseSafePartyFollowPos(bot, anchor, s_partyFollowReservedThisTick);
	s_ptrail.formationTele++;
	s_prv.asmTeleFallback++;
	BOT_TELEPORT(player, placeAt, true);
	bot.currentPos = placeAt;
	bot.lastPos = placeAt;
	g_logger().info("[BotEngine] [PARTYRV] teleport fallback: bot='{}' reason={} at ({},{},{})",
		bot.name, reason, placeAt.x, placeAt.y, placeAt.z);
}

// Enroll a bot that has just accepted (or been recruited into) a HUMAN-led party. Returns the
// assembly id, or 0 if the bot is already close enough that no assembly is needed.
uint32_t BotEngine::enrollHumanLedMember(BotState& bot, const std::shared_ptr<Player>& leader) {
	if (!leader) return 0;
	const uint32_t leaderCreatureId = leader->getID();

	// One assembly per human leader — a second /party or a second invite joins the same record.
	uint32_t assemblyId = 0;
	for (auto& [id, asmb] : s_partyAssembly) {
		if (asmb.kind == RvKind::HUMAN_LED && asmb.leaderCreatureId == leaderCreatureId) {
			assemblyId = id;
			break;
		}
	}
	if (assemblyId == 0) {
		assemblyId = s_nextAssemblyId++;
		PartyAssembly asmb;
		asmb.assemblyId = assemblyId;
		asmb.kind = RvKind::HUMAN_LED;
		asmb.leaderCreatureId = leaderCreatureId;
		asmb.anchor = leader->getPosition();
		asmb.anchorTownId = findNearestTown(asmb.anchor);
		asmb.startedMs = OTSYS_TIME();
		s_partyAssembly[assemblyId] = std::move(asmb);
		s_prv.asmStarted++;
	}
	auto& asmb = s_partyAssembly[assemblyId];

	RvMember m;
	m.guid = bot.guid;
	m.wasHibernated = bot.hibernated;
	m.canaryJoined = true; // HUMAN_LED joins Canary at commit time — see the impl.hpp note
	// Decides walk / stage-off-screen-then-walk / real journey, and performs the staging teleport.
	beginAssemblyApproach(bot, asmb, m);
	asmb.members.push_back(m);
	s_rvMember[bot.guid] = assemblyId;

	// A WALKING_IN member must be IDLE, because that is the state whose handler preempts into
	// handleAssemblyStaging. Leaving it in HUNTING/TRAVELING/DWELLING means its own AI keeps
	// driving it and the approach never happens. TRAVELLING members are the exception: startTravel
	// sets its own state and doTravel has to keep running the trip.
	//
	// activatedAt is zeroed rather than relying on preempt ordering: doIdle's 60s activation
	// fallback TELEPORTS a bot to its temple when activatedAt is set and it has no walk target,
	// and an invite-woken member has it set. Belt and braces against a future reorder.
	// beginAssemblyApproach already issued startTravel for a TRAVELLING member (which sets its own
	// state); a WALKING_IN member needs IDLE here so handleAssemblyStaging preempts for it.
	if (m.phase != RvPhase::TRAVELLING) {
		bot.state = BotAIState::IDLE;
		bot.activatedAt = 0;
		bot.dwellUntil = 0;
	}
	g_logger().info("[BotEngine] [PARTYRV] assembly #{} kind=human leader='{}' member='{}' entry={}",
		assemblyId, leader->getName(), bot.name,
		m.phase == RvPhase::TRAVELLING ? "TRAVELLING" : "WALKING_IN");
	return assemblyId;
}

// Drives an assembling member's approach from INSIDE its own per-tick pass. This has to live here
// rather than in the supervisor: the supervisor runs at tick top, and processBot then runs the
// member's own state AI afterwards and overrides whatever the supervisor asked for. Verified live
// — a member that accepted while TRAVELING stayed TRAVELING, doTravel kept driving it, and it
// drifted to 1230 tiles instead of converging. Same shape as handleGangStaging, for the same
// reason: returning true consumes the tick so nothing else moves the bot.
bool BotEngine::handleAssemblyStaging(BotState& bot) {
	// BOT_LED hold: the party-hunt leader waits where it stands while its members converge, so it
	// is IDLE with a reserved hunt script. Consume its tick — otherwise doIdle's body runs and the
	// 60s activation fallback teleports it to temple, or vigilante/randomPK/gang flips it out of
	// the hold entirely, stranding members walking toward a leader that left.
	if (bot.isPartyHuntLeader && bot.partyHuntId > 0
	    && assemblyActiveForPartyHunt(bot.partyHuntId)) {
		return true;
	}

	auto mIt = s_rvMember.find(bot.guid);
	if (mIt == s_rvMember.end()) return false;
	auto aIt = s_partyAssembly.find(mIt->second);
	if (aIt == s_partyAssembly.end()) { s_rvMember.erase(bot.guid); return false; }
	const PartyAssembly& asmb = aIt->second;

	const RvMember* me = nullptr;
	for (const auto& m : asmb.members) {
		if (m.guid == bot.guid) { me = &m; break; }
	}
	if (!me) return false;

	auto player = bot.getPlayer();
	if (!player) return false;

	// Only WALKING_IN is DRIVEN here, but every live phase still CONSUMES the tick. A member that
	// has just arrived from a cross-town trip is state-IDLE with its phase not yet flipped (the
	// supervisor runs at most every 500ms), and travel arrival hands it a fresh depot target — so
	// letting doIdle's body run in that window starts an errand that fights the assembly.
	// TRAVELLING members are dispatched into doTraveling, not here, so this costs them nothing.
	if (me->phase != RvPhase::WALKING_IN) return true;

	// Already close enough; the supervisor will flip us to ARRIVED on its next pass. Consume the
	// tick anyway so no idle behaviour wanders off in the meantime.
	const int32_t d = trailCheb(bot.currentPos, asmb.anchor);
	if (bot.currentPos.z == asmb.anchor.z && d <= PARTY_HUNT_SUPPORT_FOLLOW_DIST) return true;

	// Chunked walk toward the live anchor. The observed-walk-pause feature may inject pauses here
	// (listWalkDir stays empty while paused); that is desirable and the phase budget tolerates it.
	if (player->listWalkDir.empty()) {
		goToWithDoors(bot, asmb.anchor, 1);
	}
	return true;
}

void BotEngine::tickPartyAssembly(int64_t nowMs) {
	if (s_partyAssembly.empty()) return;
	static int64_t s_lastAsmTickMs = 0;
	if (nowMs - s_lastAsmTickMs < 500) return;
	s_lastAsmTickMs = nowMs;

	std::vector<uint32_t> deadAssemblies;

	for (auto& [assemblyId, asmb] : s_partyAssembly) {
		// ---- Resolve and refresh the anchor. A human leader drifts around the depot while
		// waiting, so the target is re-read every pass rather than frozen at commit time.
		std::shared_ptr<Player> leaderPlayer;
		if (asmb.kind == RvKind::HUMAN_LED) {
			auto c = g_game().getCreatureByID(asmb.leaderCreatureId);
			leaderPlayer = c ? c->getPlayer() : nullptr;
			// Same trap as doPartyFollow: a human who leaves the party stays online and healthy
			// while Party::leaveParty hands leadership to a bot, so liveness alone would keep
			// members walking toward someone who is no longer in their party.
			bool leaderStillLeads = false;
			if (leaderPlayer) {
				if (auto lp = leaderPlayer->getParty()) {
					leaderStillLeads = (lp->getLeader() == leaderPlayer);
				}
			}
			if (!leaderPlayer || leaderPlayer->isRemoved() || leaderPlayer->getHealth() <= 0
			    || !leaderStillLeads) {
				// The human left, died, or logged out. Members stop assembling and reroll where
				// they stand; doPartyFollow/exitPartyMode owns any that already arrived.
				for (const auto& m : asmb.members) {
					s_rvMember.erase(m.guid);
					if (auto it = guidToIndex_.find(m.guid); it != guidToIndex_.end()) {
						exitPartyMode(bots_[it->second]);
					}
				}
				asmb.members.clear();
				deadAssemblies.push_back(assemblyId);
				continue;
			}
			asmb.anchor = leaderPlayer->getPosition();
		} else {
			auto it = guidToIndex_.find(asmb.leaderGuid);
			if (it == guidToIndex_.end()) { for (const auto& m : asmb.members) s_rvMember.erase(m.guid);
				deadAssemblies.push_back(assemblyId); continue; }
			leaderPlayer = bots_[it->second].getPlayer();
			if (!leaderPlayer) { for (const auto& m : asmb.members) s_rvMember.erase(m.guid);
				deadAssemblies.push_back(assemblyId); continue; }
			asmb.anchor = bots_[it->second].currentPos;
		}

		// A PZ anchor cannot be reached by a member that is still pz-locked from a fight, so the
		// last leg would stall at the depot doorway for the whole cooldown. Accept arrival a
		// little further out in that case.
		const auto anchorTile = g_game().map.getTile(asmb.anchor);
		const bool anchorInPz = anchorTile && anchorTile->hasFlag(TILESTATE_PROTECTIONZONE);
		const int32_t arriveDist = anchorInPz
			? PARTY_HUNT_SUPPORT_FOLLOW_DIST + 2 : PARTY_HUNT_SUPPORT_FOLLOW_DIST;

		for (auto& m : asmb.members) {
			if (m.phase == RvPhase::ARRIVED || m.phase == RvPhase::FAILED) continue;
			auto it = guidToIndex_.find(m.guid);
			if (it == guidToIndex_.end()) { s_rvMember.erase(m.guid); continue; }
			auto& bot = bots_[it->second];
			auto player = bot.getPlayer();
			if (!player || player->isRemoved()) { continue; }

			// Never yank a bot out of a fight, and never drive one that is mid-death.
			if (bot.state == BotAIState::COMBAT || bot.state == BotAIState::FLEEING
			    || bot.state == BotAIState::PK_ATTACK || bot.deathPauseUntil > 0) {
				continue;
			}

			switch (m.phase) {
				case RvPhase::FINISHING: {
					// Wind-down completion has no single observable site, so use the predicate
					// the plan settled on: the hunt is released AND the bot is idle again.
					if (bot.huntScriptId == 0
					    && (bot.state == BotAIState::IDLE || bot.state == BotAIState::DWELLING)) {
						// Issues the staging teleport / startTravel itself.
						beginAssemblyApproach(bot, asmb, m);
						if (m.phase != RvPhase::TRAVELLING) {
							bot.state = BotAIState::IDLE;  // see enrollHumanLedMember
							bot.activatedAt = 0;
						}
					} else if (nowMs - m.phaseSinceMs > asmCfg_.finishMaxMs) {
						m.phase = RvPhase::FAILED;
						failAssemblyMemberToTeleport(bot, asmb.anchor, "finish_timeout");
					}
					break;
				}
				case RvPhase::TRAVELLING: {
					// startTravel drives itself; watch for arrival in the anchor's town.
					if (bot.travelDestTownId == 0) {
						m.phase = RvPhase::WALKING_IN;
						m.phaseSinceMs = nowMs;
						bot.state = BotAIState::IDLE;  // hand over to handleAssemblyStaging
						bot.activatedAt = 0;
					} else if (nowMs - m.travelSinceMs > asmCfg_.maxMs) {
						m.phase = RvPhase::FAILED;
						failAssemblyMemberToTeleport(bot, asmb.anchor, "travel_timeout");
					}
					break;
				}
				case RvPhase::WALKING_IN: {
					// Self-heal the state regardless of how the member got here — DWELLING never
					// preempts into assembly staging from doIdle, and a spent travel leaves
					// TRAVELING behind. Cheaper than trusting every entry path to be correct.
					if (bot.state == BotAIState::DWELLING
					    || (bot.state == BotAIState::TRAVELING && bot.travelDestTownId == 0)) {
						bot.state = BotAIState::IDLE;
						bot.dwellUntil = 0;
						bot.activatedAt = 0;
					}
					const int32_t d = trailCheb(bot.currentPos, asmb.anchor);
					if (bot.currentPos.z == asmb.anchor.z && d <= arriveDist) {
						m.phase = RvPhase::ARRIVED;
						m.phaseSinceMs = nowMs;
						bot.state = BotAIState::PARTY;
						s_prv.asmWalked++;
						s_prv.assembleMsTotal += static_cast<uint64_t>(nowMs - asmb.startedMs);
						s_prv.assembleCount++;
						g_logger().info("[BotEngine] [PARTYRV] assembly #{} kind={} member='{}' "
							"WALKING_IN->ARRIVED tookMs={}",
							assemblyId, asmb.kind == RvKind::HUMAN_LED ? "human" : "bot",
							bot.name, nowMs - asmb.startedMs);
						break;
					}
					if (nowMs - m.travelSinceMs > asmCfg_.maxMs) {
						m.phase = RvPhase::FAILED;
						failAssemblyMemberToTeleport(bot, asmb.anchor, "walk_timeout");
						break;
					}
					// The WALK itself happens in handleAssemblyStaging, inside the member's own
					// per-tick pass. Issuing it from here does not survive: processBot runs the
					// member's state AI afterwards and overrode it (live: a member drifted to
					// 1230 tiles). The supervisor owns phases, budgets and arrival only.
					break;
				}
				default: break;
			}
		}

		if (asmb.kind == RvKind::BOT_LED_HUNT) {
			// BOT_LED joins the Canary party at ARRIVED, never earlier: an all-bot party whose
			// members are still walking matches sweepStaleCanaryParties' staleness predicate and
			// would be reclaimed mid-assembly. FAILED members were teleported in, so they join too
			// — otherwise a party-hunting member would be invisible in the party list and outside
			// shared exp.
			auto leaderParty = leaderPlayer->getParty();
			for (auto& m : asmb.members) {
				if (m.canaryJoined) continue;
				if (m.phase != RvPhase::ARRIVED && m.phase != RvPhase::FAILED) continue;
				auto it = guidToIndex_.find(m.guid);
				if (it == guidToIndex_.end()) continue;
				auto& mb = bots_[it->second];
				if (auto mp = mb.getPlayer(); mp && leaderParty) {
					leaderParty->invitePlayer(mp);
					if (leaderParty->joinParty(mp)) m.canaryJoined = true;
					mp->setSecureMode(true);
				}
				mb.state = BotAIState::PARTY;
			}

			// Barrier: resolve once every member is ARRIVED or FAILED (a FAILED member was
			// teleported in, so it counts — the barrier always resolves within maxMs).
			bool allDone = true;
			for (const auto& m : asmb.members) {
				if (m.phase != RvPhase::ARRIVED && m.phase != RvPhase::FAILED) { allDone = false; break; }
			}
			if (allDone && !asmb.members.empty()) {
				auto lIt = guidToIndex_.find(asmb.leaderGuid);
				if (lIt != guidToIndex_.end()) {
					auto& lb = bots_[lIt->second];
					// (i) shared exp — only meaningful now that members have actually joined.
					if (leaderParty) leaderParty->setSharedExperience(leaderPlayer, true);
					// (ii) start the hunt, mirroring the formation tail EXACTLY. A same-town script
					// must NOT go through startTravel: it early-returns without setting state,
					// which would leave the leader IDLE with pendingHuntAfterTravel set forever —
					// and the reroll that used to rescue such a strand is now guarded off.
					const HuntScript* hs = nullptr;
					if (auto sIt = s_partyHuntScript.find(asmb.partyHuntId); sIt != s_partyHuntScript.end()) {
						for (const auto& cand : huntScripts_) {
							if (cand.id == sIt->second) { hs = &cand; break; }
						}
					}
					if (hs && hs->townId != lb.townId) {
						lb.pendingHuntAfterTravel = true;
						startTravel(lb, hs->townId);
					} else {
						beginHuntPhase(lb, HuntPhase::PREPARING);
					}
					g_logger().info("[BotEngine] [PARTYRV] assembly #{} kind=bot RESOLVED — leader '{}' "
						"starts hunt with {} member(s) after {}ms",
						assemblyId, lb.name, asmb.members.size(), nowMs - asmb.startedMs);
				}
				// (iii) erase LAST. tickPartyAssembly runs at tick top, before the per-bot loop, so
				// erasing here leaves zero window in which an ARRIVED member's doPartyHunt could see
				// "not assembling AND leader not HUNTING/TRAVELING" and dissolve the party.
				for (const auto& m : asmb.members) s_rvMember.erase(m.guid);
				asmb.members.clear();
				deadAssemblies.push_back(assemblyId);
			}
		} else {
			// HUMAN_LED: members retire individually — there is no barrier, because the leader is a
			// human who is already playing and has nothing to wait for.
			std::erase_if(asmb.members, [&](const RvMember& m) {
				if (m.phase == RvPhase::ARRIVED || m.phase == RvPhase::FAILED) {
					if (m.phase == RvPhase::FAILED) {
						if (auto it = guidToIndex_.find(m.guid); it != guidToIndex_.end()) {
							bots_[it->second].state = BotAIState::PARTY;
						}
					}
					s_rvMember.erase(m.guid);
					return true;
				}
				return false;
			});
			if (asmb.members.empty()) deadAssemblies.push_back(assemblyId);
		}
	}

	for (uint32_t id : deadAssemblies) {
		s_partyAssembly.erase(id);
	}
}

void BotEngine::exitPartyMode(BotState& bot) {
	auto player = bot.getPlayer();
	uint32_t guid = bot.guid;

	if (player) {
		// Leave server party if still in one
		auto party = player->getParty();
		if (party) {
			party->leaveParty(player, true);
		}

		// Clear target
		player->setAttackedCreature(nullptr);
		player->setFollowCreature(nullptr);
		if (!player->listWalkDir.empty()) {
			player->listWalkDir.clear();
			player->stopEventWalk();
		}
	}

	// BOT_PARTY_INVITE_RENDEZVOUS §3.7 — a bot that leaves a party rolls its NEXT task from
	// where it is standing. It no longer rewinds: no state snapshot is restored and, crucially,
	// no teleport back to wherever it happened to be when the party recruited it. Leaving a
	// party now looks like finishing any other activity.
	//
	// s_prePartyState (the full BotState snapshot) is gone entirely — restoring it was the only
	// reason a member could be yanked across the world on exit. s_partyWasInactive SURVIVES as
	// a bare bool, because it answers a different question that still matters: was this bot ever
	// logged in before the party? findBotsForParty can conscript never-active bots, and a party
	// promotes them to active=true, so without tracking that, every party would ratchet the
	// logged-in population upward. Those guids go into s_reclaimToInactive and hibernateBot
	// routes them to deactivateBot when the ordinary reclaim rules eventually come for them.
	const bool wasInactive = s_partyWasInactive.count(guid) ? s_partyWasInactive[guid] : false;
	if (wasInactive) {
		s_reclaimToInactive.insert(guid);
	}

	// Hand back everything the bot was holding. Without this the reroll below never fires:
	// doActivityReroll hard-returns while hasWalkTarget / followingCityRoute / pendingNavDest is
	// set, and the party path is exactly what leaves them set.
	releasePartyMemberActivity(bot, "left_party");

	bot.state = BotAIState::IDLE;
	bot.dwellUntil = 0;
	// Re-anchor the bot's notion of "home" to where it actually is — it may have followed the
	// leader into a different town entirely, and every activity pick keys off townId.
	if (const uint32_t nearTown = findNearestTown(bot.currentPos); nearTown > 0) {
		bot.townId = nearTown;
		if (const auto town = g_game().map.towns.getTown(nearTown)) {
			bot.townName = town->getName();
		}
	}
	// Seeds the FIRST reroll only; doActivityReroll then sets its own BOT_REROLL_COOLDOWN_SEC.
	bot.nextRerollTime = OTSYS_TIME() + uniform_random(2000, 15000);
	castLog(bot, fmt::format("PARTY: left party — rerolling from current location ({},{},{}) town={}{}",
		bot.currentPos.x, bot.currentPos.y, bot.currentPos.z, bot.townName,
		wasInactive ? " [reclaim-to-inactive armed]" : ""));


	// Restore PvP toggle
	if (player) {
		auto secIt = s_partyPrevSecureMode.find(guid);
		player->setSecureMode(secIt != s_partyPrevSecureMode.end() ? secIt->second : true);
	}

	// Clear all party static maps for this bot
	s_partyLeaderId.erase(guid);
	s_lastLeaderPos.erase(guid);
	s_lastPartyHealTime.erase(guid);
	s_partyWasInactive.erase(guid);
	s_partyPrevSecureMode.erase(guid);
	// P7: a human-party support routed through followPartyHuntLeader/role fns writes these guid-keyed
	// cohesion/combat maps. Clear them on exit so a stale separation timer / retreat cooldown can't
	// mis-fire (e.g. an instant teleport) when the bot next solo-hunts or joins an autonomous party.
	s_followerSeparatedSince.erase(guid);
	s_partyFollowTeleportCooldown.erase(guid);
	s_followerLeaderZStamp.erase(guid);
	s_followerLastTeleLeaderZ.erase(guid);
	s_followerLastLeaderZ.erase(guid);
	s_followerZChangeDetected.erase(guid);
	s_retreatUntil.erase(guid);
	s_approachCooldown.erase(guid);
	s_spreadCooldown.erase(guid);
	s_partyFormationOffset.erase(guid); // P8 inc2: sticky slot state
	// TRAIL: per-FOLLOWER state only. The shared leader-keyed trail (s_leaderTrail /
	// s_trailWanted / s_partyWaitStartMs) is dropped in dissolvePartyHunt or by TTL expiry —
	// one bot dying or leaving must not wipe the route out from under its siblings.
	s_followerCursor.erase(guid);
	trailRetireZHopSession(guid, bot.currentPos.z); // ROUND2 A4: dissolved after a completed hop
	s_lastSlotRollMs.erase(guid);
	s_roleWalkedThisTick.erase(guid);
}

void BotEngine::doParty(BotState& bot) {
	if (bot.partyHuntId > 0) {
		doPartyHunt(bot);  // autonomous bot-to-bot party hunt
	} else {
		doPartyFollow(bot); // human-led party follow
	}
}

void BotEngine::doPartyFollow(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	// PvP assist: secure mode is NOT cosmetic server-side. hasSecureMode() is enforced in
	// Combat::canTargetCreature (combat.cpp:258), the aggressive-rune check
	// (spells.cpp:682) and Weapon::useWeapon (weapons.cpp:572/581), so with it left ON a
	// member lands instant spells on an unmarked player but has every melee swing and every
	// rune refused - a knight would flail uselessly. Flip it to match the current target,
	// exactly as the gang / vigilante / random-PK paths already do before they engage.
	// exitPartyMode restores the pre-party value from s_partyPrevSecureMode.
	auto syncPvpSecureMode = [&](const std::shared_ptr<Creature>& tgt) {
		const bool wantSecure = !(tgt && tgt->getPlayer());
		if (player->hasSecureMode() != wantSecure) {
			player->setSecureMode(wantSecure);
			if (!wantSecure) {
				s_prv.pvpAssistEngagements++;
				castLog(bot, fmt::format("PARTY_PVP: assisting leader against player '{}'",
					tgt->getName()));
			}
		}
	};

	// 1. Validate party + leader
	auto leaderIt = s_partyLeaderId.find(bot.guid);
	if (leaderIt == s_partyLeaderId.end()) {
		exitPartyMode(bot);
		return;
	}

	auto party = player->getParty();
	if (!party) {
		exitPartyMode(bot);
		return;
	}

	auto leaderCreature = g_game().getCreatureByID(leaderIt->second);
	auto leader = leaderCreature ? leaderCreature->getPlayer() : nullptr;

	// The stored leader must still actually BE in this bot's party. Liveness is not enough:
	// when a human leaves, Party::leaveParty hands leadership to memberList.front() — a BOT —
	// and the party survives, while the human's Player stays online and healthy. Every check
	// below then passes and the bot cheerfully keeps following someone who left the party.
	// Verified live: /party leave left two bots still trailing the ex-leader.
	bool leaderInParty = false;
	if (leader) {
		leaderInParty = (party->getLeader() == leader);
		if (!leaderInParty) {
			for (const auto& m : party->getMembers()) {
				if (m == leader) { leaderInParty = true; break; }
			}
		}
	}

	if (!leader || leader->isRemoved() || leader->getHealth() <= 0 || !leaderInParty) {
		// Leader gone — check if another real player exists in the party
		std::shared_ptr<Player> newLeader = nullptr;

		// Check current party leader
		auto partyLeader = party->getLeader();
		if (partyLeader && !partyLeader->isBotPlayer() && partyLeader->getHealth() > 0) {
			newLeader = partyLeader;
		}

		// Check members
		if (!newLeader) {
			for (const auto& member : party->getMembers()) {
				if (member && !member->isBotPlayer() && member->getHealth() > 0) {
					newLeader = member;
					break;
				}
			}
		}

		if (newLeader) {
			// Transfer leadership to real player if current leader is a bot
			if (partyLeader && partyLeader->isBotPlayer()) {
				party->passPartyLeadership(newLeader);
			}
			// Update stored leader for this bot
			s_partyLeaderId[bot.guid] = newLeader->getID();
			leader = newLeader;
			castLog(bot, fmt::format("PARTY: Leader changed to '{}'", leader->getName()));
		} else {
			// No real players left — exit party
			exitPartyMode(bot);
			return;
		}
	}

	auto leaderPos = leader->getPosition();
	int32_t leaderDist = std::max(
		std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(leaderPos.x)),
		std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(leaderPos.y)));

	// TRAIL: human-led parties record too (gated on botPartyTrailHumanLead). Registered on
	// every run so the trail already exists when a separation happens.
	if (trailCfg_.enable && trailCfg_.humanLead) {
		auto& want = s_trailWanted[leader->getGUID()];
		want.botGuid = bot.guid;
		want.creatureId = leader->getID();
		want.expiresMs = OTSYS_TIME() + TRAIL_WANT_TTL_MS;
	}

	// 2. Self-heal (uses existing doHealing which handles HP, paralysis, mana)
	doHealing(bot);

	// 3. Mana restore
	if (player->getMana() < player->getMaxMana() / 2) {
		player->mana = player->getMaxMana();
		g_game().addPlayerMana(player);
	}

	uint8_t baseVoc = getBaseVocation(bot.vocationId);
	bool chasing = false;

	// === P7: find the party's EK anchor (lowest-guid EK bot — deterministic across all members) ===
	// If a human-led party contains an EK bot, the EK tanks (challenge + target/fallback) and the
	// other bots support it exactly like an autonomous party hunt, reusing the verified P2/P3/P4/P5
	// cohesion. The EK itself follows the human leader.
	BotState* ekBot = nullptr;
	std::shared_ptr<Player> ekPlayer = nullptr;
	uint32_t ekGuid = 0xFFFFFFFFu;
	for (const auto& member : party->getMembers()) {
		if (!member || !member->isBotPlayer()) continue;
		auto voc = member->getVocation();
		if (!voc || voc->getBaseId() != 4) continue;
		uint32_t mg = member->getGUID();
		if (mg >= ekGuid) continue;
		auto idx = guidToIndex_.find(mg);
		if (idx == guidToIndex_.end()) continue;
		ekGuid = mg;
		ekBot = &bots_[idx->second];
		ekPlayer = member;
	}
	const bool thisIsFirstEk = (baseVoc == 4 && ekGuid == bot.guid);

	if (thisIsFirstEk) {
		// ===== A1: EK TANK — exeta res + obey human target / fallback to nearest, follow human =====
		const auto& challengeSpell = g_spells().getInstantSpell("exeta res");
		if (challengeSpell
			&& player->getLevel() >= 20
			&& !player->hasCondition(CONDITION_SPELLCOOLDOWN, challengeSpell->getSpellId())
			&& !player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, challengeSpell->getGroup())
			&& player->getMana() >= 30) {
			bool monstersNearby = false;
			auto spectators = Spectators().find<Monster>(bot.currentPos, false, 1, 1, 1, 1);
			for (const auto& spec : spectators) {
				if (spec->getMonster() && spec->getHealth() > 0) { monstersNearby = true; break; }
			}
			if (monstersNearby) {
				std::string words = "exeta res";
				if (g_spells().playerSaySpell(player, words) == TALKACTION_BREAK) {
					player->saySpell(TALKTYPE_SAY, "exeta res", false);
					castLog(bot, "PARTY: exeta res (challenge)");
				}
			}
		}

		auto ekTarget = pickEkTankTarget(player, leader);
		syncPvpSecureMode(ekTarget);
		// Leash bound: chaseTarget has no leash of its own, so never let the chase drag the EK out of
		// the human's leash. If we've drifted too far, drop the target and follow the human instead.
		if (ekTarget && ekTarget->getPosition().z == bot.currentPos.z
			&& leaderDist <= PARTY_LEASH_DIST + 3) {
			if (player->getAttackedCreature() != ekTarget) player->setAttackedCreature(ekTarget);
			castSpell(bot, ekTarget);
			chaseTarget(bot, ekTarget);
			chasing = true;
			if (ekTarget != leader->getAttackedCreature()) {
				castLog(bot, fmt::format("PARTY: [PARTYTANK] fallback target #{} (human idle)", ekTarget->getID()));
			}
		} else if (player->getAttackedCreature()) {
			player->setAttackedCreature(nullptr);
		}
	} else if (ekBot != nullptr) {
		// ===== A2: support anchored to the EK bot (reuse autonomous role + cohesion) =====
		// The role fns below mirror the EK target verbatim, so the member needs the same
		// secure-mode state the EK has for that target or its melee/runes are refused.
		syncPvpSecureMode(ekPlayer ? ekPlayer->getAttackedCreature() : nullptr);
		// Guard (defensive): never run while the bot is also tracked in an autonomous hunt — the
		// doParty() dispatcher already separates the two via partyHuntId, so this should never hit.
		if (s_botToPartyHunt.find(bot.guid) == s_botToPartyHunt.end()) {
			switch (baseVoc) {
				case 1: doPartyHuntDpsMage(bot, ekBot); break;   // MS: mirror EK target + AoE
				case 2: doPartyHuntHealer(bot, ekBot); break;    // ED: heal EK+human+party AND attack
				case 3: doPartyHuntDpsRanged(bot, ekBot); break; // RP
				default: { // baseVoc==4: a SECOND EK — just mirror EK#1's target (melee), no fallback
					auto t = ekPlayer->getAttackedCreature();
					if (t && t->getMonster() && t->getHealth() > 0 && !t->isRemoved()
						&& t->getPosition().z == bot.currentPos.z) {
						if (player->getAttackedCreature() != t) player->setAttackedCreature(t);
						castSpell(bot, t);
					}
					break;
				}
			}
			// Cohesion positioning around the EK. Call it UNCONDITIONALLY and signal "already
			// walking" through s_roleWalkedThisTick, exactly as the autonomous call site does.
			//
			// The old `if (listWalkDir.empty())` gate was the same-tile pile-up: formation slot
			// claims live in the per-tick s_partyFormationClaims set, so a member that was mid-walk
			// skipped the call and therefore inserted NO claim. The other same-vocation member's
			// next roll then saw the slot free — and validated it, because the destination tile is
			// still empty while the first member is walking to it — and took the same slot. Both
			// walked to one tile; in a PZ they stack outright. A race, which is why it only
			// happened sometimes. The autonomous path was fixed this way in P8 inc1
			// (s_roleWalkedThisTick); this A2 site was simply never modernised.
			//
			// It also restores the teleport/trail safety nets inside followPartyHuntLeader, which
			// its own header comment requires to run every tick — the gate suppressed those too.
			if (!player->listWalkDir.empty()) {
				s_roleWalkedThisTick.insert(bot.guid);
			}
			followPartyHuntLeader(bot, ekPlayer, ekBot);
		}
		chasing = true; // A2 fully owns positioning — skip the human-follow block below
	} else {
		// ===== A3: no EK in party — human-target mirror + druid heal =====
		// Same two-level priority as the EK branch, via the same helper: the human's target first,
		// else a monster standing right next to the human. Without the fallback these members would
		// simply idle while something chewed on the leader — the A3 path had no fallback at all.
		auto leaderTarget = leader->getAttackedCreature();
		// No getMonster() filter here: a PvP target from the leader must survive to the mirror
		// below, and pickEkTankTarget already vets everything (friendly fire, z, leash).
		if (!leaderTarget || leaderTarget->getHealth() <= 0 || leaderTarget->isRemoved()) {
			leaderTarget = pickEkTankTarget(player, leader);
		}
		syncPvpSecureMode(leaderTarget);
		if (leaderTarget && leaderTarget->getHealth() > 0) {
			auto tpos = leaderTarget->getPosition();

			// Leash check: don't chase if it would take us too far from leader or break LOS
			bool leashOk = leaderDist <= PARTY_LEASH_DIST
				&& (bot.currentPos.z == leaderPos.z)
				&& g_game().map.isSightClear(bot.currentPos, leaderPos, true);

			if (leashOk && tpos.z == bot.currentPos.z) {
				if (player->getAttackedCreature() != leaderTarget) {
					player->setAttackedCreature(leaderTarget);
				}
				castSpell(bot, leaderTarget);
				chaseTarget(bot, leaderTarget);
				chasing = true;
			} else if (!leashOk && player->getAttackedCreature()) {
				player->setAttackedCreature(nullptr);
			}
		} else if (player->getAttackedCreature()) {
			player->setAttackedCreature(nullptr);
		}

		// Druid party healing (A3 ED only — A2 ED heals via doPartyHuntHealer)
		if (baseVoc == 2) {
			doPartyHealing(bot, party, leader);
		}
	}

	// 7. Follow leader (only if not chasing a target)
	if (!chasing) {
		bool teleported = false;
		if (bot.currentPos.z != leaderPos.z) {
			// TRAIL: walk the human leader's recorded route before falling back to the teleport.
			if (trailCfg_.humanLead && tryFollowLeaderTrail(bot, leader)) {
				return;
			}
			// Different floor — teleport to leader
			Position placeAt = chooseSafePartyFollowPos(bot, leaderPos, s_partyFollowReservedThisTick);
			BOT_TELEPORT(player, placeAt, true);
			bot.currentPos = placeAt;
			bot.lastPos = placeAt;
			teleported = true;
			s_ptrail.partyTele++; // [PTRAIL] baseline: human-led z-change teleport
			castLog(bot, fmt::format("PARTY: Teleported to leader (z-change) at ({},{},{})",
				placeAt.x, placeAt.y, placeAt.z));
		} else if (leaderDist > PARTY_TELEPORT_DIST) {
			// TRAIL: same guard as the z-change branch above.
			if (trailCfg_.humanLead && tryFollowLeaderTrail(bot, leader)) {
				return;
			}
			// Too far — teleport
			Position placeAt = chooseSafePartyFollowPos(bot, leaderPos, s_partyFollowReservedThisTick);
			BOT_TELEPORT(player, placeAt, true);
			bot.currentPos = placeAt;
			bot.lastPos = placeAt;
			teleported = true;
			s_ptrail.partyTele++; // [PTRAIL] baseline: human-led too-far teleport
			castLog(bot, fmt::format("PARTY: Teleported to leader (too far, dist={}) at ({},{},{})",
				leaderDist, placeAt.x, placeAt.y, placeAt.z));
		}

		if (teleported) {
			// Clear stale walk/follow state from before the teleport
			if (!player->listWalkDir.empty()) {
				player->listWalkDir.clear();
				player->stopEventWalk();
			}
			player->setFollowCreature(nullptr);
			player->setAttackedCreature(nullptr);
		} else if (leaderDist > trailCfg_.followDist) {
			// Walk toward leader
			if (!player->listWalkDir.empty()) {
				// Already walking — but if leader is getting far, recompute path
				if (leaderDist > PARTY_LEASH_DIST) {
					player->listWalkDir.clear();
					player->stopEventWalk();
				}
			}

			if (player->listWalkDir.empty()) {
				FindPathParams fpp;
				fpp.fullPathSearch = true;
				fpp.clearSight = true;
				fpp.allowDiagonal = true;
				fpp.keepDistance = false;
				fpp.maxSearchDist = PATH_MAX_DIST;
				fpp.minTargetDist = 0;
				fpp.maxTargetDist = 1;

				std::vector<Direction> dirList;
				if (g_game().map.getPathMatching(player, leaderPos, dirList, FrozenPathingConditionCall(leaderPos), fpp)) {
					botStartAutoWalk(bot, player,dirList);
				} else {
					// Try without clear sight
					fpp.clearSight = false;
					dirList.clear();
					if (g_game().map.getPathMatching(player, leaderPos, dirList, FrozenPathingConditionCall(leaderPos), fpp)) {
						botStartAutoWalk(bot, player,dirList);
					} else {
						// Try opening doors
						tryOpenDoors(bot, player, leaderPos);
					}
				}
			}
		}
	}
}

void BotEngine::doPartyHealing(BotState& bot, const std::shared_ptr<Party>& party,
	const std::shared_ptr<Player>& leader) {
	auto player = bot.getPlayer();
	if (!player || !party) return;

	// Check healing spell group cooldown (all heals share this)
	const auto& sioSpell = g_spells().getInstantSpell("exura sio");
	if (!sioSpell) return;
	if (player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, sioSpell->getGroup())) return;

	int32_t playerHpPct = (player->getMaxHealth() > 0)
		? (player->getHealth() * 100 / player->getMaxHealth()) : 100;

	// Priority 1: Self-critical heal (< 50% HP) — survival first
	if (playerHpPct < 50) {
		// Self-heal is handled by doHealing() which runs before this
		// But if healing group is available, we let doHealing handle it next tick
		return; // Don't cast sio/mas res — save cooldown for self-heal
	}

	// Collect party members that need healing (< 70% HP, within 7 tiles, same z)
	struct HealTarget {
		std::shared_ptr<Player> player;
		int32_t hpPct;
		int32_t priority; // lower = higher priority
	};
	std::vector<HealTarget> needsHealing;

	auto checkMember = [&](const std::shared_ptr<Player>& member, int32_t prio) {
		if (!member || member->getID() == player->getID()) return;
		if (member->getHealth() <= 0 || member->isRemoved()) return;
		auto mpos = member->getPosition();
		if (mpos.z != bot.currentPos.z) return;
		int32_t dist = std::max(std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(mpos.x)),
			std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(mpos.y)));
		if (dist > 7) return;
		int32_t hpPct = member->getMaxHealth() > 0
			? (member->getHealth() * 100 / member->getMaxHealth()) : 100;
		if (hpPct < 70) {
			needsHealing.push_back({member, hpPct, prio});
		}
	};

	// Check leader (highest priority)
	if (leader) checkMember(leader, 0);

	// Check members — knights get priority 1, others priority 2
	for (const auto& member : party->getMembers()) {
		if (!member || member->getID() == player->getID()) continue;
		uint8_t memberBaseVoc = 0;
		auto voc = member->getVocation();
		if (voc) memberBaseVoc = voc->getBaseId();
		int32_t prio = (memberBaseVoc == 4) ? 1 : 2;
		checkMember(member, prio);
	}

	if (needsHealing.empty()) return;

	// Priority 2: Mass healing (exura gran mas res) if 2+ members need healing
	if (needsHealing.size() >= 2 && player->getLevel() >= 36) {
		const auto& masResSpell = g_spells().getInstantSpell("exura gran mas res");
		if (masResSpell
			&& !player->hasCondition(CONDITION_SPELLCOOLDOWN, masResSpell->getSpellId())
			&& player->getMana() >= 150) {

			std::string words = "exura gran mas res";
			auto result = g_spells().playerSaySpell(player, words);
			if (result == TALKACTION_BREAK) {
				player->saySpell(TALKTYPE_SAY, "exura gran mas res", false);
				castLog(bot, fmt::format("PARTY: exura gran mas res (mass heal, {} members low)",
					needsHealing.size()));
				return;
			}
		}
	}

	// Priority 3: Targeted heal (exura sio) by priority order
	if (player->getLevel() < 18) return;
	if (player->getMana() < 120) return;
	if (player->hasCondition(CONDITION_SPELLCOOLDOWN, sioSpell->getSpellId())) return;

	// Sort by priority (lower = heal first), then by HP% (lower = heal first)
	std::sort(needsHealing.begin(), needsHealing.end(),
		[](const auto& a, const auto& b) {
			if (a.priority != b.priority) return a.priority < b.priority;
			return a.hpPct < b.hpPct;
		});

	auto& target = needsHealing[0];
	std::string words = "exura sio \"" + target.player->getName() + "\"";
	auto result = g_spells().playerSaySpell(player, words);
	if (result == TALKACTION_BREAK) {
		player->saySpell(TALKTYPE_SAY, words, false);
		castLog(bot, fmt::format("PARTY: exura sio \"{}\" (hp={}%)",
			target.player->getName(), target.hpPct));
	}
}

// ============================================================================
// Autonomous Party Hunt System (Phase 10)
// ============================================================================

// Helper: get the hunt script's keep distance for a support bot's vocation in party hunt context
int32_t BotEngine::getPartyHuntKeepDistance(const BotState& bot, BotState* leaderBot) const {
	if (!leaderBot || leaderBot->huntScriptId == 0) return 3; // default
	for (const auto& s : huntScripts_) {
		if (s.id == leaderBot->huntScriptId) {
			switch (getBaseVocation(bot.vocationId)) {
				case 1: return s.keepDistanceMS > 0 ? s.keepDistanceMS : 3;
				case 2: return s.keepDistanceED > 0 ? s.keepDistanceED : 3;
				case 3: return 0; // RP in party hunt: melee range (can tank, needs close for AoE)
				default: return 3;
			}
		}
	}
	return 3;
}

// Try to cast exeta res (challenge) — shared between doPartyFollow and party hunt EK
void BotEngine::tryCastChallenge(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	uint8_t baseVoc = getBaseVocation(bot.vocationId);
	if (baseVoc != 4) return; // EK only

	const auto& challengeSpell = g_spells().getInstantSpell("exeta res");
	if (!challengeSpell) return;
	if (player->getLevel() < 20) return;
	if (player->hasCondition(CONDITION_SPELLCOOLDOWN, challengeSpell->getSpellId())) return;
	if (player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, challengeSpell->getGroup())) return;
	if (player->getMana() < 30) {
		player->mana = player->getMaxMana();
		g_game().addPlayerMana(player);
	}

	// Only cast if monsters within 1 tile (SQUARE1X1 area)
	auto spectators = Spectators().find<Monster>(bot.currentPos, false, 1, 1, 1, 1);
	bool monstersNearby = false;
	for (const auto& spec : spectators) {
		if (spec->getMonster() && spec->getHealth() > 0) {
			monstersNearby = true;
			break;
		}
	}

	if (monstersNearby) {
		std::string words = "exeta res";
		auto result = g_spells().playerSaySpell(player, words);
		if (result == TALKACTION_BREAK) {
			player->saySpell(TALKTYPE_SAY, "exeta res", false);
			castLog(bot, "PARTY_HUNT: exeta res (challenge)");
		}
	}
}

// Try to cast utamo vita (mana shield) for ED self-protection
bool BotEngine::tryCastUtamoVita(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return false;

	// Only if not already shielded
	if (player->hasCondition(CONDITION_MANASHIELD)) return false;

	const auto& utamoSpell = g_spells().getInstantSpell("utamo vita");
	if (!utamoSpell) return false;
	if (player->getLevel() < static_cast<int32_t>(utamoSpell->getLevel())) return false;
	if (player->hasCondition(CONDITION_SPELLCOOLDOWN, utamoSpell->getSpellId())) return false;
	if (player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, utamoSpell->getGroup())) return false;

	// Ensure mana
	if (player->getMana() < 50) {
		player->mana = player->getMaxMana();
		g_game().addPlayerMana(player);
	}

	std::string words = "utamo vita";
	auto result = g_spells().playerSaySpell(player, words);
	if (result == TALKACTION_BREAK) {
		player->saySpell(TALKTYPE_SAY, "utamo vita", false);
		castLog(bot, "PARTY_HUNT: utamo vita (mana shield for self-protection)");
		return true;
	}
	return false;
}

// --- ROUND2 E: shared roster recruitment + leader election (live AND virtual) ---
//
// Hoisted out of the per-script loop deliberately. findBotsForParty takes no script parameter and
// the old loop re-ran the identical query with an identical exclusion set on every iteration, so a
// roster that failed for one script failed for all of them — the retry was vestigial. Recruiting
// once lets us elect the leader BEFORE any state is wired, which is what makes "the initiator may
// not be the leader" tractable: there is no wire-then-reassign and nothing to unwind.
BotEngine::PartyRoster BotEngine::recruitPartyRoster(const BotState& initiator, int32_t initLevel,
	size_t* bestEd, size_t* bestMs, size_t* bestRp) {
	PartyRoster roster;
	std::unordered_set<uint32_t> usedGuids;
	usedGuids.insert(initiator.guid);

	const uint8_t initVoc = getBaseVocation(initiator.vocationId);
	// The initiator occupies its own slot; recruit one of every OTHER base vocation.
	switch (initVoc) {
		case 4: roster.ek = initiator.guid; break;
		case 2: roster.ed = initiator.guid; break;
		case 1: roster.ms = initiator.guid; break;
		case 3: roster.rp = initiator.guid; break;
		default: break;
	}

	auto recruit = [&](uint8_t baseVoc, uint32_t& slot, size_t* bestOut) {
		if (slot != 0) {
			return; // the initiator already fills this slot
		}
		auto candidates = findBotsForParty(baseVoc, static_cast<uint32_t>(initLevel), 1, usedGuids);
		if (bestOut) {
			*bestOut = std::max(*bestOut, candidates.size());
		}
		if (!candidates.empty()) {
			slot = candidates[0];
			usedGuids.insert(slot);
		}
	};

	// EK first so a knight is in the roster before the election runs (order is cosmetic — the
	// election scans slots, not insertion order — but it keeps the common case cheap).
	size_t sinkEk = 0;
	recruit(4, roster.ek, &sinkEk);
	recruit(2, roster.ed, bestEd);
	recruit(1, roster.ms, bestMs);
	recruit(3, roster.rp, bestRp);
	return roster;
}

// EK > RP > initiator. By construction the roster holds at most one bot per base vocation and the
// initiator occupies its own slot, so a recruited EK always wins and an EK can never end up as a
// support of a non-EK leader.
uint32_t BotEngine::electPartyLeader(const PartyRoster& roster, uint32_t initiatorGuid) {
	if (roster.ek != 0) {
		return roster.ek;
	}
	if (roster.rp != 0) {
		return roster.rp;
	}
	return initiatorGuid; // sorcerer/druid pair — whoever set it up leads
}

// --- Party Formation ---
bool BotEngine::tryStartPartyHunt(BotState& bot, int32_t forceScriptId) {
	// ROUND2 E: ANY vocation may initiate. The leader is ELECTED from the assembled roster
	// (EK > RP > initiator), so the initiator here may well end up a support.
	if (bot.partyHuntId > 0 || s_botToPartyHunt.count(bot.guid)) return false; // already in party hunt

	// Hibernated initiator → the virtual formation path; bots stay hibernated and wake together
	// only when observed. This is the steady state since ~197/200 bots are hibernated.
	auto player = bot.getPlayer();
	if (!player || player->isRemoved()) {
		return virtualTryStartPartyHunt(bot, forceScriptId);
	}

	if (player->getLevel() < PARTY_HUNT_MIN_LEVEL) return false;
	const int32_t initLevel = static_cast<int32_t>(player->getLevel());
	const std::string initName = player->getName();
	g_logger().info("[BotEngine] PARTYHUNT-TRY: '{}' lv{} voc={} (active={}, hibernated=false, forced={})",
		initName, initLevel, getBaseVocation(bot.vocationId), bot.active, forceScriptId);

	// 1. Recruit ONCE (initiator's level — the only level known before the election, and it keeps
	//    the whole roster mutually level-compatible whoever wins).
	size_t bestEd = 0, bestMs = 0, bestRp = 0;
	const PartyRoster roster = recruitPartyRoster(bot, initLevel, &bestEd, &bestMs, &bestRp);

	// 2. Elect.
	const uint32_t leaderGuid = electPartyLeader(roster, bot.guid);

	// 3. Require at least one support besides the leader — a solo bot is not a party.
	if (roster.supportCount(leaderGuid) == 0) {
		g_logger().info("[BotEngine] PARTYHUNT-FAIL: '{}' lv{} no supports (best ed={}, ms={}, rp={})",
			initName, initLevel, bestEd, bestMs, bestRp);
		return false;
	}

	// 3b. BOT_PARTY_CAP: gated HERE because recruitment and election are pure reads — the roster
	//     size is exact and nothing has been mutated yet (step 4 below is the first write), so a
	//     refusal needs no unwinding and never over-reserves. forceScriptId != 0 = explicit admin
	//     command, which bypasses the cap (an operator should not be silently refused).
	if (forceScriptId == 0 && !partyCapAllows(1 + static_cast<uint32_t>(roster.supportCount(leaderGuid)), "live")) {
		return false;
	}

	// 4. Resolve the elected leader to a live BotState + Player. If it is a recruit it may be
	//    hibernated or busy; prepare it exactly as setupSupport prepares a support, then let it own
	//    the formation. If it cannot be made live, demote down the ladder — the initiator is awake
	//    by precondition, so election always terminates.
	BotState* leaderBot = &bot;
	std::shared_ptr<Player> leaderPlayer = player;
	if (leaderGuid != bot.guid) {
		auto it = guidToIndex_.find(leaderGuid);
		bool ok = false;
		if (it != guidToIndex_.end()) {
			BotState& cand = bots_[it->second];
			if (cand.hibernated) {
				s_forceWakeGuid = leaderGuid;   // bypass the density band for an explicit assembly wake
				s_proximityWake = false;        // assemble at the initiator, don't walk in
				ok = wakeBot(leaderGuid);
			} else {
				ok = cand.active;
			}
			if (ok) {
				auto candPlayer = cand.getPlayer();
				if (candPlayer && !candPlayer->isRemoved()) {
					// Release whatever it was doing — same teardown setupSupport performs.
					if (cand.huntScriptId > 0) {
						activeHunts_.erase(cand.huntScriptId);
						for (const auto& hs : huntScripts_) {
							if (hs.id == cand.huntScriptId && !hs.spawnGroup.empty()) {
								activeSpawnGroups_.erase(hs.spawnGroup);
								break;
							}
						}
						cand.huntScriptId = 0;
					}
					clearAdvStoneState(cand);
					clearFishingRun(cand.guid);
					endHouseVisit(cand.guid, "party_elected_leader");
					endShrineVisit(cand.guid, "party_elected_leader");
					cand.followingCityRoute = false;
					cand.cityRouteWps.clear();
					cand.cityRouteIdx = 0;
					cand.hasWalkTarget = false;
					if (!candPlayer->listWalkDir.empty()) {
						candPlayer->listWalkDir.clear();
						candPlayer->stopEventWalk();
					}
					// Bring the elected leader to the initiator — the initiator is the bot that was
					// observed rolling into the party window, so assembly happens where the action is.
					std::unordered_set<uint64_t> reserved;
					const Position at = chooseWakePosition(cand, bot.currentPos, false);
					BOT_TELEPORT(candPlayer, at, true);
					cand.currentPos = at;
					cand.lastPos = at;
					s_ptrail.formationTele++;
					leaderBot = &cand;
					leaderPlayer = candPlayer;
				} else {
					ok = false;
				}
			}
		}
		if (!ok) {
			g_logger().info("[BotEngine] PARTYHUNT: elected leader guid={} unavailable — demoting to initiator '{}'",
				leaderGuid, initName);
			leaderBot = &bot;
			leaderPlayer = player;
		}
	}

	return formPartyWithLeader(*leaderBot, leaderPlayer, roster, bot.guid, forceScriptId,
		bestEd, bestMs, bestRp);
}

// ROUND2 E: formation with an ALREADY-ELECTED leader and an ALREADY-RECRUITED roster.
// Split out of tryStartPartyHunt so that "the initiator may not be the leader" needs no
// wire-then-reassign: by the time this runs, `leader` is the bot that will own the hunt script,
// the hunt phase machine and the Canary party, and every other roster member is a support —
// including the initiator, when it lost the election.
bool BotEngine::formPartyWithLeader(BotState& leader, const std::shared_ptr<Player>& leaderPlayer,
	const PartyRoster& roster, uint32_t initiatorGuid, int32_t forceScriptId,
	size_t rosterBestEd, size_t rosterBestMs, size_t rosterBestRp) {
	const int32_t leaderLevel = static_cast<int32_t>(leaderPlayer->getLevel());
	const std::string leaderName = leaderPlayer->getName();

	// Find eligible hunt scripts (prefer higher level scripts suitable for parties)
	std::vector<const HuntScript*> eligible;
	bool foundForced = false;
	for (const auto& script : huntScripts_) {
		if (forceScriptId > 0) {
			if (static_cast<int32_t>(script.id) != forceScriptId) continue;
			foundForced = true;

			// Force-clear: abort whatever bot currently holds this reservation
			if (activeHunts_.count(script.id)) {
				uint32_t holderGuid = activeHunts_[script.id];
				auto holderIt = guidToIndex_.find(holderGuid);
				if (holderIt != guidToIndex_.end()) {
					auto& holderBot = bots_[holderIt->second];
					g_logger().info("[BotEngine] PARTYHUNT: Force-clearing script {} from '{}' (guid={})",
						script.id, holderBot.getPlayer() ? holderBot.getPlayer()->getName() : "?", holderGuid);
					abortHunt(holderBot, "force-cleared by partyhunt command");
					holderBot.state = BotAIState::IDLE;
					holderBot.hasWalkTarget = false;
					// Teleport holder to temple
					if (holderBot.getPlayer()) {
						auto templePos = holderBot.getPlayer()->getTemplePosition();
						BOT_TELEPORT(holderBot.getPlayer(), templePos, true);
						holderBot.currentPos = templePos;
						holderBot.lastPos = templePos;
					}
				} else {
					// Bot not found — just clear the reservation directly
					activeHunts_.erase(script.id);
					if (!script.spawnGroup.empty()) activeSpawnGroups_.erase(script.spawnGroup);
				}
			} else if (!script.spawnGroup.empty() && activeSpawnGroups_.count(script.spawnGroup)) {
				// Spawn group held by a different script — find and abort that bot
				uint32_t holderGuid = activeSpawnGroups_[script.spawnGroup];
				auto holderIt = guidToIndex_.find(holderGuid);
				if (holderIt != guidToIndex_.end()) {
					auto& holderBot = bots_[holderIt->second];
					g_logger().info("[BotEngine] PARTYHUNT: Force-clearing spawnGroup '{}' from '{}' (guid={})",
						script.spawnGroup, holderBot.getPlayer() ? holderBot.getPlayer()->getName() : "?", holderGuid);
					abortHunt(holderBot, "force-cleared by partyhunt command");
					holderBot.state = BotAIState::IDLE;
					holderBot.hasWalkTarget = false;
					if (holderBot.getPlayer()) {
						auto templePos = holderBot.getPlayer()->getTemplePosition();
						BOT_TELEPORT(holderBot.getPlayer(), templePos, true);
						holderBot.currentPos = templePos;
						holderBot.lastPos = templePos;
					}
				} else {
					activeSpawnGroups_.erase(script.spawnGroup);
				}
			}
		} else {
			if (!script.enabled) continue;
			if (script.patrolWaypoints.empty()) continue;
			// Hunts only. A party hunt is a repeating spawn-camp activity; a quest is a
			// linear one-shot walkthrough, and a traveling script has no spawn at all.
			// This loop had no category filter, so a party EK could be handed a quest and
			// then "finish" it after a single pass.
			if (script.scriptCategory != "hunt") continue;
			// Empty targetNames is allowed — bot attacks all monsters during PATROLLING.
			// ROUND2 E: the spawn-level tolerance depends on WHO leads. x3 was calibrated for an EK
			// tank with full support; a mage-led party has no tank at all, so it gets x1.5.
			const PartyLevelTolerance tol = partyLevelToleranceFor(getBaseVocation(leader.vocationId));
			int32_t effectiveLevel = leaderLevel * tol.num / tol.den;
			if (script.levelMin > 0 && effectiveLevel < static_cast<int32_t>(script.levelMin)) continue;
			if (script.levelMax > 0 && leaderLevel > static_cast<int32_t>(script.levelMax)) continue;
			if (activeHunts_.count(script.id)) continue;
			if (!script.spawnGroup.empty() && activeSpawnGroups_.count(script.spawnGroup)) continue;
			if (isScriptPlayerClaimed(script.id, script.spawnGroup)) continue; // player spawn-claim
			if (isScriptHuntRepelled(script)) continue; // hunt-flagged player nearby
		}
		eligible.push_back(&script);
	}

	if (eligible.empty()) {
		g_logger().info("[BotEngine] PARTYHUNT-FAIL: '{}' no eligible scripts (forced={} found={} activeHunts={} activeSpawnGroups={})",
			leaderName, forceScriptId, foundForced, activeHunts_.size(), activeSpawnGroups_.size());
		return false;
	}

	// Shuffle eligible scripts (irrelevant when forced, but harmless)
	auto rng = std::mt19937(std::random_device{}());
	std::shuffle(eligible.begin(), eligible.end(), rng);
	size_t bestEdCount = rosterBestEd, bestMsCount = rosterBestMs, bestRpCount = rosterBestRp;

	// ROUND2 E: the roster was recruited ONCE before the leader was elected, so the loop below only
	// picks a free script. The old per-script re-recruitment was vestigial: findBotsForParty takes
	// no script parameter and was re-run with an identical exclusion set every iteration, so a
	// roster that failed for one script failed for all of them.
	const uint32_t edGuid = (roster.ed != leader.guid) ? roster.ed : 0;
	const uint32_t msGuid = (roster.ms != leader.guid) ? roster.ms : 0;
	const uint32_t rpGuid = (roster.rp != leader.guid) ? roster.rp : 0;
	for (const auto* script : eligible) {

		// We have EK + at least 1 support member — form the party!
		uint32_t partyHuntId = s_nextPartyHuntId++;

		// Reserve hunt script
		activeHunts_[script->id] = leader.guid;
		if (!script->spawnGroup.empty()) {
			activeSpawnGroups_[script->spawnGroup] = leader.guid;
		}

		// Setup EK (leader) — uses normal hunting state machine
		leader.huntScriptId = script->id;
		logHuntAssign(leader, script->id);
		leader.huntTownId = script->townId;
		leader.huntStartTime = OTSYS_TIME();
		leader.huntEndTime = leader.huntStartTime + uniform_random(PARTY_HUNT_TIME_MIN, PARTY_HUNT_TIME_MAX) * 1000LL;
		leader.huntKillCount = 0;
		leader.huntWaypointIdx = 0;
		leader.huntPatrolCycles = 0;
		// Reset huntPhase explicitly — hibernated EKs may carry stale phase (e.g. LEAVING
		// from a virtualSim hunt that was mid-resupply). Without this, doPartyHunt's
		// HUNTING+LEAVING/RESUPPLYING check would dissolve the party 100ms after formation.
		// For same-town hunts, beginHuntPhase(PREPARING) below also sets this; for
		// cross-town (startTravel), the reset must happen here.
		leader.huntPhase = HuntPhase::PREPARING;
		leader.huntTargetId = 0;
		leader.huntChaseFailCount = 0;
		leader.huntIgnoredMonsters.clear();
		leader.huntWaypointSkipCount = 0;
		leader.partyHuntId = partyHuntId;
		leader.partyRole = PARTY_ROLE_TANK;
		leader.partyLeaderGuid = leader.guid; // self
		leader.isPartyHuntLeader = true;

		// Register in static maps
		s_partyHuntLeaderGuid[partyHuntId] = leader.guid;
		s_partyHuntScript[partyHuntId] = script->id;
		s_partyHuntDeathCount[partyHuntId] = 0;
		s_partyHuntKillCount[partyHuntId] = 0;
		s_botToPartyHunt[leader.guid] = partyHuntId;

		std::vector<uint32_t> members;
		members.push_back(leader.guid);
		if (edGuid) members.push_back(edGuid);
		if (msGuid) members.push_back(msGuid);
		if (rpGuid) members.push_back(rpGuid);
		s_partyHuntMembers[partyHuntId] = members;

		// Create Canary Party with EK as leader
		// BOT_PARTY_LEAK_FIX: never reuse a party we do not actually lead. A stale membership here
		// would leave the engine believing this bot leads while Canary's leader is someone else —
		// invitePlayer/joinParty then succeed against the OLD leader and setSharedExperience
		// silently fails its leader gate, i.e. a wrong bot treated as party leader.
		if (auto stale = leaderPlayer->getParty(); stale && stale->getLeader() != leaderPlayer) {
			reclaimStaleCanaryParty(leader.guid, "formation_reuse");
		}
		auto party = leaderPlayer->getParty();
		if (!party) {
			party = Party::create(leaderPlayer);
			if (!party) {
				// Cleanup
				activeHunts_.erase(script->id);
				if (!script->spawnGroup.empty()) activeSpawnGroups_.erase(script->spawnGroup);
				s_partyHuntLeaderGuid.erase(partyHuntId);
				s_partyHuntScript.erase(partyHuntId);
				s_partyHuntDeathCount.erase(partyHuntId);
				s_partyHuntKillCount.erase(partyHuntId);
				s_botToPartyHunt.erase(leader.guid);
				s_partyHuntMembers.erase(partyHuntId);
				leader.partyHuntId = 0;
				leader.partyRole = 0;
				leader.isPartyHuntLeader = false;
				g_logger().warn("[BotEngine] PARTYHUNT-FAIL: '{}' Party::create returned null", leaderName);
				return false;
			}
		}

		// BOT_LED assembly: when enabled, members converge on the LEADER on foot instead of
		// being teleported onto it. The record is created BEFORE the first setupSupport so
		// every member enrols into it; the formation-failure cleanup below erases it again.
		// v1 deliberately has no FINISHING phase: the per-state wind-down table that phase
		// needs does not exist, so a mid-patrol recruit would sit the full finishMaxMs and
		// THEN teleport - a systematic pop-in, strictly worse than releasing the hunt now and
		// walking over.
		const bool botLedAssembly = asmCfg_.enable;
		uint32_t botLedAssemblyId = 0;
		if (botLedAssembly) {
			botLedAssemblyId = s_nextAssemblyId++;
			PartyAssembly pa;
			pa.assemblyId = botLedAssemblyId;
			pa.kind = RvKind::BOT_LED_HUNT;
			pa.partyHuntId = partyHuntId;
			pa.leaderGuid = leader.guid;
			// Members converge on the LEADER, not the script town: the post-resolution hunt
			// start owns the journey to the spawn.
			pa.anchor = leader.currentPos;
			pa.anchorTownId = findNearestTown(leader.currentPos);
			pa.startedMs = OTSYS_TIME();
			s_partyAssembly[botLedAssemblyId] = std::move(pa);
			s_prv.asmStarted++;
		}

		// Activate and join support bots
		auto setupSupport = [&](uint32_t guid, uint8_t role) {
			auto it = guidToIndex_.find(guid);
			if (it == guidToIndex_.end()) return;
			auto& supportBot = bots_[it->second];

			bool wasInactive = !supportBot.active;
			bool wasHibernated = supportBot.hibernated;

			// Only the "was it ever logged in" bool — nothing is restored on dissolution any
			// more (BOT_PARTY_INVITE_RENDEZVOUS §3.7); it feeds s_reclaimToInactive so a
			// conscripted never-active bot goes all the way back out rather than lingering
			// as active=true.
			s_partyWasInactive[guid] = wasInactive;

			if (wasHibernated) {
				// Release virtualSim hunt reservation before wake — wakeBot's restoreSingleBotState
				// may re-claim, but we'll overwrite below anyway.
				if (supportBot.huntScriptId > 0) {
					activeHunts_.erase(supportBot.huntScriptId);
					for (const auto& hs : huntScripts_) {
						if (hs.id == supportBot.huntScriptId && !hs.spawnGroup.empty()) {
							activeSpawnGroups_.erase(hs.spawnGroup);
							break;
						}
					}
				}
				// Explicit support-assembly wake: bypass the density gate (single-shot).
				// s_inPartyCascade is only set AFTER this initiating wake passes the
				// gate, so without this the outerLimitPct=0 band rule would abort
				// party assembly for any support bot >midRadius from all anchors.
				s_forceWakeGuid = guid;
				// Assembly mode wakes the recruit OFF-SCREEN and lets it walk in; the old
				// behaviour (false) materialises it beside the leader for the instant teleport.
				s_proximityWake = botLedAssembly;
				if (!wakeBot(guid)) {
					g_logger().warn("[BotEngine] PARTYHUNT: failed to wake hibernated support guid={}", guid);
					return;
				}
			} else if (wasInactive) {
				if (!activateBot(guid)) {
					g_logger().warn("[BotEngine] PARTYHUNT: failed to activate inactive support guid={}", guid);
					return;
				}
			} else {
				// Release any active hunt
				if (supportBot.huntScriptId > 0) {
					activeHunts_.erase(supportBot.huntScriptId);
					for (const auto& hs : huntScripts_) {
						if (hs.id == supportBot.huntScriptId && !hs.spawnGroup.empty()) {
							activeSpawnGroups_.erase(hs.spawnGroup);
							break;
						}
					}
				}
			}

			auto supportPlayer = supportBot.getPlayer();
			if (!supportPlayer) {
				g_logger().warn("[BotEngine] PARTYHUNT: support guid={} has no Player after wake/activate", guid);
				return;
			}

			// Reset HP/mana + clear any hunt re-set by restoreSingleBotState
			if (supportBot.huntScriptId > 0) {
				activeHunts_.erase(supportBot.huntScriptId);
				for (const auto& hs : huntScripts_) {
					if (hs.id == supportBot.huntScriptId && !hs.spawnGroup.empty()) {
						activeSpawnGroups_.erase(hs.spawnGroup);
						break;
					}
				}
				supportBot.huntScriptId = 0;
			}
			// ...and the rest of the hunt block with it. Releasing the reservation while leaving
			// huntPhase where it was is what made roam suppression a coin flip: a member drafted
			// out of a live patrol arrived carrying PATROLLING and was suppressed by accident,
			// while one drafted from town arrived PREPARING and was not — an outcome decided by
			// what the bot happened to be doing when it was conscripted. isBotSpawnEngaged now
			// reads the LEADER, so nothing depends on this field any more; it is normalised
			// regardless, because a stale phase on a bot with no hunt is a lie the next reader
			// will believe. Unconditional: restoreSingleBotState writes the phase back on a
			// hibernated recruit even when the script id was already clear.
			// Same field set as releasePartyMemberActivity, which this path deliberately does not
			// call (it would also tear down travel, roam and dwell state this path manages itself).
			supportBot.huntPhase = HuntPhase::PREPARING;
			supportBot.huntWaypointIdx = 0;
			supportBot.huntWaypointSkipCount = 0;
			supportBot.huntKillCount = 0;
			supportBot.huntIgnoredMonsters.clear();
			supportPlayer->health = supportPlayer->healthMax;
			supportPlayer->mana = supportPlayer->getMaxMana();
			g_game().addCreatureHealth(supportPlayer);
			g_game().addPlayerMana(supportPlayer);

			// Defensive: a candidate that began an AdvStone trip in the selection→setup gap
			// would keep running doAdventurerStone (preempts the state switch) and never
			// follow. findBotsForParty already excludes advStoneActive; this closes the race.
			clearAdvStoneState(supportBot);
			clearFishingRun(supportBot.guid); // closes the same selection->setup race
			endHouseVisit(supportBot.guid, "party_conscripted"); // same race, same reason
			endShrineVisit(supportBot.guid, "party_conscripted");

			// Set party hunt state. partyHuntId is set even in assembly mode - it is what blocks
			// independent hibernation, supply/fishing/house conscription and double-recruitment.
			// Only the TELEPORT is deferred.
			supportBot.state = botLedAssembly ? BotAIState::IDLE : BotAIState::PARTY;
			supportBot.partyHuntId = partyHuntId;
			supportBot.partyRole = role;
			supportBot.partyLeaderGuid = leader.guid;
			supportBot.isPartyHuntLeader = false;
			s_botToPartyHunt[guid] = partyHuntId;

			// Also set the human-party leader tracking (for doParty dispatch)
			s_partyLeaderId[guid] = leaderPlayer->getID();

			if (!botLedAssembly) {
				// Teleport to EK. Via chooseSafePartyFollowPos, not the leader's exact tile —
				// internalTeleport skips queryDestination, so landing on the leader's ladder or
				// hole leaves the support standing on it. Mirrors materializeCanaryParty.
				std::unordered_set<uint64_t> formationReserved;
				Position supportPos = chooseSafePartyFollowPos(supportBot, leader.currentPos, formationReserved);
				s_ptrail.formationTele++; // [PTRAIL]: ACCEPTED teleport (formation assembly, cross-town)
				BOT_TELEPORT(supportPlayer, supportPos, true);
				supportBot.currentPos = supportPos;
				supportBot.lastPos = supportPos;
			}

			// Clear walk state
			if (!supportPlayer->listWalkDir.empty()) {
				supportPlayer->listWalkDir.clear();
				supportPlayer->stopEventWalk();
			}
			supportBot.hasWalkTarget = false;
			supportBot.followingCityRoute = false;
			supportBot.cityRouteWps.clear();
			supportBot.cityRouteIdx = 0;
			supportBot.huntTargetId = 0;
			supportBot.huntScriptId = 0;
			supportPlayer->setAttackedCreature(nullptr);
			supportPlayer->setSecureMode(true);

			if (botLedAssembly) {
				// Do NOT join the Canary party yet. An all-bot party whose members are still
				// IDLE and walking matches sweepStaleCanaryParties staleness predicate, so an
				// early join would be reclaimed mid-assembly. The join happens at ARRIVED.
				supportBot.activatedAt = 0;   // never let the 60s fallback teleport it
				supportBot.dwellUntil = 0;
				RvMember m;
				m.guid = guid;
				m.role = role;
				m.wasHibernated = wasHibernated;
				m.canaryJoined = false;
				// Same approach decision a human-led member gets: stage off-screen near the leader
				// and walk the last leg, not a real cross-world journey nobody can watch.
				beginAssemblyApproach(supportBot, s_partyAssembly[botLedAssemblyId], m);
				s_partyAssembly[botLedAssemblyId].members.push_back(m);
				s_rvMember[guid] = botLedAssemblyId;
				g_logger().info("[BotEngine] [PARTYRV] assembly #{} kind=bot leader='{}' member='{}' entry={}",
					botLedAssemblyId, leader.name, supportBot.name,
					m.phase == RvPhase::TRAVELLING ? "TRAVELLING" : "WALKING_IN");
			} else {
				// Add to Canary Party
				party->invitePlayer(supportPlayer);
				party->joinParty(supportPlayer);
			}

			castLog(supportBot, fmt::format("PARTY_HUNT: Joined as {} (party={}, leader={})",
				role == PARTY_ROLE_HEALER ? "HEALER" :
				role == PARTY_ROLE_DPS_MAGE ? "DPS_MAGE" : "DPS_RANGED",
				partyHuntId, leaderPlayer->getName()));
		};

		if (edGuid) setupSupport(edGuid, PARTY_ROLE_HEALER);
		if (msGuid) setupSupport(msGuid, PARTY_ROLE_DPS_MAGE);
		if (rpGuid) setupSupport(rpGuid, PARTY_ROLE_DPS_RANGED);

		if (botLedAssembly && s_partyAssembly.count(botLedAssemblyId) > 0
		    && !s_partyAssembly[botLedAssemblyId].members.empty()) {
			// HOLD. The leader waits where it stands, keeping huntScriptId reserved, until every
			// member has arrived. setSharedExperience and the hunt start both move to resolution in
			// tickPartyAssembly: at this point no member has joined the Canary party, so a
			// setSharedExperience here would have nothing to act on.
			leader.state = BotAIState::IDLE;
			leader.dwellUntil = 0;
			leader.activatedAt = 0;
			g_logger().info("[BotEngine] [PARTYRV] assembly #{} kind=bot leader='{}' HOLDING for {} member(s)",
				botLedAssemblyId, leader.name, s_partyAssembly[botLedAssemblyId].members.size());
		} else {
			// No assembly (disabled, or every member failed to enrol) — today's instant behaviour.
			if (botLedAssembly) s_partyAssembly.erase(botLedAssemblyId);

			// Enable shared experience
			party->setSharedExperience(leaderPlayer, true);

			// Now start the hunt — travel if different town, otherwise prepare
			if (script->townId != leader.townId) {
				leader.pendingHuntAfterTravel = true;
				startTravel(leader, script->townId);
			} else {
				beginHuntPhase(leader, HuntPhase::PREPARING);
			}
		}

		std::string memberNames;
		for (uint32_t guid : members) {
			auto mIt = guidToIndex_.find(guid);
			if (mIt != guidToIndex_.end()) {
				auto mPlayer = bots_[mIt->second].getPlayer();
				if (mPlayer) {
					if (!memberNames.empty()) memberNames += ", ";
					memberNames += mPlayer->getName();
					memberNames += "(";
					uint8_t role = bots_[mIt->second].partyRole;
					// ROUND2 E: label by the member's real VOCATION. PARTY_ROLE_TANK now just means
					// "leader/anchor" and can be held by any vocation, so the old role->"EK" mapping
					// would print a druid leader as "EK" and poison every acceptance review.
					const uint8_t mVoc = getBaseVocation(bots_[mIt->second].vocationId);
					memberNames += mVoc == 4 ? "EK" : mVoc == 2 ? "ED" : mVoc == 1 ? "MS" : "RP";
					if (role == PARTY_ROLE_TANK) memberNames += "*"; // * = leader
					memberNames += ")";
				}
			}
		}

		g_logger().info("[BotEngine] PARTY_HUNT #{} formed: '{}' [{}] — {} members: {} (leader={} voc={} elected={}, initiator guid={})",
			partyHuntId, script->name, script->id, members.size(), memberNames,
			leaderName, getBaseVocation(leader.vocationId),
			leader.guid == initiatorGuid ? "INITIATOR" : (getBaseVocation(leader.vocationId) == 4 ? "EK" : "RP"),
			initiatorGuid);
		castLog(leader, fmt::format("PARTY_HUNT: Formed! script='{}' members=[{}] hunt={}min",
			script->name, memberNames, (leader.huntEndTime - leader.huntStartTime) / 60000));

		return true;
	}

	g_logger().info("[BotEngine] PARTYHUNT-FAIL: '{}' lv{} no supports across {} scripts (best ed={}, ms={}, rp={})",
		leaderName, leaderLevel, eligible.size(), bestEdCount, bestMsCount, bestRpCount);
	return false; // couldn't form party for any script
}

// --- Party Dissolution ---
void BotEngine::dissolvePartyHunt(uint32_t partyHuntId, const std::string& reason) {
	// BOT_PARTY_INVITE_RENDEZVOUS: drop any assembly record for this party FIRST and
	// unconditionally, exactly as the trail state is dropped below. This runs before the
	// members-map guard on purpose — a dissolve path that returns early must not be able to
	// strand a record whose leader is already gone, because a stranded record would keep
	// assemblyActiveForPartyHunt() true forever and permanently disarm the trap-#4 guards.
	for (auto it = s_partyAssembly.begin(); it != s_partyAssembly.end();) {
		if (it->second.kind == RvKind::BOT_LED_HUNT && it->second.partyHuntId == partyHuntId) {
			for (const auto& m : it->second.members) s_rvMember.erase(m.guid);
			it = s_partyAssembly.erase(it);
		} else {
			++it;
		}
	}

	auto membersIt = s_partyHuntMembers.find(partyHuntId);
	if (membersIt == s_partyHuntMembers.end()) return;

	auto members = membersIt->second; // copy — we'll modify the map

	g_logger().info("[BotEngine] PARTY_HUNT #{} dissolving: {}", partyHuntId, reason);

	// Find and handle support bots (non-leader)
	uint32_t leaderGuid = 0;
	auto leaderIt = s_partyHuntLeaderGuid.find(partyHuntId);
	if (leaderIt != s_partyHuntLeaderGuid.end()) {
		leaderGuid = leaderIt->second;
	}

	for (uint32_t guid : members) {
		// BOT_LURE_KITE: every member, leader included. A support never passes through
		// exitPartyHuntMode on this path, so this is its only hygiene point.
		clearLureKiteState(guid);
		if (guid == leaderGuid) continue; // handle leader separately
		// BOT_PARTY_CAP (ratchet insurance): erase BEFORE the guidToIndex_ guard. A guid missing
		// from the index would otherwise strand its entry forever, and s_botToPartyHunt.size() is
		// now the cap numerator — a permanent stale entry would slowly block ALL party formation.
		// Unconditional erase is a no-op when already absent.
		s_botToPartyHunt.erase(guid);

		auto it = guidToIndex_.find(guid);
		if (it == guidToIndex_.end()) continue;
		auto& supportBot = bots_[it->second];

		castLog(supportBot, fmt::format("PARTY_HUNT: Dissolved ({})", reason));

		// Clear party hunt fields before exitPartyMode
		supportBot.partyHuntId = 0;
		supportBot.partyRole = 0;
		supportBot.partyLeaderGuid = 0;
		supportBot.isPartyHuntLeader = false;
		s_botToPartyHunt.erase(guid);
		s_followerLastLeaderZ.erase(guid);
		s_followerZChangeDetected.erase(guid);
		s_followerSeparatedSince.erase(guid);
		s_partyFollowTeleportCooldown.erase(guid);
		s_followerLastTeleLeaderZ.erase(guid);
		s_followerLeaderZStamp.erase(guid);
		s_partyFormationOffset.erase(guid); // P8 inc2
		s_lastSlotRollMs.erase(guid);

		// exitPartyMode handles: leave Canary party, restore pre-party state, teleport back
		exitPartyMode(supportBot);
	}

	// Handle EK leader
	if (leaderGuid) {
		auto it = guidToIndex_.find(leaderGuid);
		if (it != guidToIndex_.end()) {
			auto& leaderBot = bots_[it->second];
			auto leaderPlayer = leaderBot.getPlayer();

			castLog(leaderBot, fmt::format("PARTY_HUNT: Dissolved ({})", reason));

			// Leave Canary party
			if (leaderPlayer) {
				auto party = leaderPlayer->getParty();
				if (party) {
					party->disband();
				}
			}

			// Clear party hunt fields — EK can continue solo hunting if mid-hunt
			leaderBot.partyHuntId = 0;
			leaderBot.partyRole = 0;
			leaderBot.partyLeaderGuid = 0;
			leaderBot.isPartyHuntLeader = false;
			s_botToPartyHunt.erase(leaderGuid);
		}
	}

	// TRAIL: dissolve is the ONLY place the shared leader-keyed trail state is dropped — it
	// alone resolves the leader and iterates the full member list (per-bot exits must not).
	if (leaderGuid) {
		s_leaderTrail.erase(leaderGuid);
		s_trailWanted.erase(leaderGuid);
		s_partyWaitStartMs.erase(leaderGuid);
	}

	// Clean up all static maps for this party hunt
	s_partyHuntMembers.erase(partyHuntId);
	s_partyHuntScript.erase(partyHuntId);
	s_partyHuntLeaderGuid.erase(partyHuntId);
	s_partyHuntDeathCount.erase(partyHuntId);
	s_partyHuntKillCount.erase(partyHuntId);
}

void BotEngine::exitPartyHuntMode(BotState& bot) {
	uint32_t partyHuntId = bot.partyHuntId;
	if (partyHuntId == 0) return;

	// BOT_LURE_KITE: a party hunt arms the lure unconditionally (botLurePartyAlways),
	// so leaving one can demote a bot from "always lures" to "not eligible". The
	// per-tick gate would catch it anyway; clearing here keeps the telemetry honest.
	clearLureKiteState(bot.guid);

	// If this bot is the leader, dissolve the whole party
	if (bot.isPartyHuntLeader) {
		dissolvePartyHunt(partyHuntId, "leader_exit");
	} else {
		// Support bot leaving — remove from member list, clean up
		auto membersIt = s_partyHuntMembers.find(partyHuntId);
		if (membersIt != s_partyHuntMembers.end()) {
			auto& members = membersIt->second;
			members.erase(std::remove(members.begin(), members.end(), bot.guid), members.end());
		}

		bot.partyHuntId = 0;
		bot.partyRole = 0;
		bot.partyLeaderGuid = 0;
		bot.isPartyHuntLeader = false;
		s_botToPartyHunt.erase(bot.guid);

		exitPartyMode(bot);
	}
}

// --- Support Bot Main Loop ---
void BotEngine::doPartyHunt(BotState& bot) {
	auto player = bot.getPlayer();
	if (!player) return;

	// 1. Validate party hunt session
	auto phIt = s_botToPartyHunt.find(bot.guid);
	if (phIt == s_botToPartyHunt.end() || bot.partyHuntId == 0) {
		exitPartyMode(bot);
		return;
	}

	uint32_t partyHuntId = bot.partyHuntId;

	// Find leader bot
	auto leaderGuidIt = s_partyHuntLeaderGuid.find(partyHuntId);
	if (leaderGuidIt == s_partyHuntLeaderGuid.end()) {
		exitPartyHuntMode(bot);
		return;
	}

	auto leaderIdx = guidToIndex_.find(leaderGuidIt->second);
	if (leaderIdx == guidToIndex_.end()) {
		exitPartyHuntMode(bot);
		return;
	}

	BotState* leaderBot = &bots_[leaderIdx->second];
	auto leaderPlayer = leaderBot->getPlayer();
	if (!leaderPlayer || leaderPlayer->getHealth() <= 0) {
		exitPartyHuntMode(bot);
		return;
	}

	// BOT_PARTY_INVITE_RENDEZVOUS trap #4 — the first-arriver dissolve. During an assembly the
	// leader deliberately holds where it stands and has NOT started its hunt, so it is neither
	// HUNTING nor TRAVELING. Nothing dissolves while every member is still walking (none of them
	// is in PARTY state, so doPartyHunt never runs for them) — but the moment the FIRST member
	// arrives and flips to PARTY, its very next tick reaches this check and tears the party down.
	// The 5s freshness grace below cannot save it either: that keys off huntStartTime, set at
	// formation and long expired by the time a cross-town member has walked in.
	//
	// A party with no assembly record has no record to find, so normal parties are unaffected —
	// and the record is erased at resolution, which restores full guard strength.
	const bool assembling = assemblyActiveForPartyHunt(partyHuntId);

	// Check if EK's hunt is over (RESUPPLYING or state changed away from HUNTING)
	if (!assembling
	    && leaderBot->state != BotAIState::HUNTING && leaderBot->state != BotAIState::TRAVELING) {
		// EK is no longer hunting — dissolve
		dissolvePartyHunt(partyHuntId, "leader_hunt_ended");
		return;
	}

	// If EK is in LEAVING or RESUPPLYING phase, dissolve.
	// Guard: skip if the party was just formed in the last 5 seconds — hibernated EKs
	// can carry stale huntPhase from their virtualSim hunt. The reset in tryStartPartyHunt
	// covers most paths; this is a belt-and-suspenders safety net.
	if (!assembling && leaderBot->state == BotAIState::HUNTING &&
		(leaderBot->huntPhase == HuntPhase::LEAVING || leaderBot->huntPhase == HuntPhase::RESUPPLYING)) {
		int64_t partyAge = OTSYS_TIME() - leaderBot->huntStartTime;
		if (partyAge < 5000) {
			g_logger().warn("[BotEngine] PARTYHUNT-STALE: party #{} fresh ({}ms) but leader phase={} — resetting to PREPARING instead of dissolving",
				partyHuntId, partyAge, static_cast<int>(leaderBot->huntPhase));
			leaderBot->huntPhase = HuntPhase::PREPARING;
			// BOT_LURE_KITE: one of the 16 direct huntPhase assignments that bypass
			// beginHuntPhase. The per-tick gate would disarm the lure anyway (phase is no
			// longer PATROLLING), but clearing here keeps the engagement counters honest.
			clearLureKiteState(leaderBot->guid);
			return;
		}
		dissolvePartyHunt(partyHuntId, "hunt_leaving/resupply");
		return;
	}

	// 2. Self-heal
	doHealing(bot);

	// 3. Mana restore (infinite mana for party hunt bots)
	if (player->getMana() < player->getMaxMana() / 2) {
		player->mana = player->getMaxMana();
		g_game().addPlayerMana(player);
	}

	// 4. Role-specific behavior
	switch (bot.partyRole) {
		case PARTY_ROLE_HEALER:
			doPartyHuntHealer(bot, leaderBot);
			break;
		case PARTY_ROLE_DPS_MAGE:
			doPartyHuntDpsMage(bot, leaderBot);
			break;
		case PARTY_ROLE_DPS_RANGED:
			doPartyHuntDpsRanged(bot, leaderBot);
			break;
	}

	// P8 walk-fight guard: if the role fn (AoE reposition) queued a walk this tick, tell
	// followPartyHuntLeader to skip its cohesion-walk so it doesn't immediately stomp that walk.
	if (player && !player->listWalkDir.empty()) {
		s_roleWalkedThisTick.insert(bot.guid);
	}

	// 5. Follow EK leader with smart positioning
	followPartyHuntLeader(bot, leaderPlayer, leaderBot);
}

// --- ED Healer Role ---
void BotEngine::doPartyHuntHealer(BotState& bot, BotState* leaderBot) {
	auto player = bot.getPlayer();
	if (!player || !leaderBot) return;

	auto leaderPlayer = leaderBot->getPlayer();
	if (!leaderPlayer) return;

	auto party = player->getParty();

	// Check heal cooldowns separately — healing and attack spells have DIFFERENT cooldown groups
	// So we can do BOTH healing and attacking in the same tick!

	// === HEALING (support group cooldown) ===
	const auto& sioSpell = g_spells().getInstantSpell("exura sio");
	bool healGroupReady = sioSpell && !player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, sioSpell->getGroup());

	if (healGroupReady) {
		int32_t selfHpPct = player->getMaxHealth() > 0
			? (player->getHealth() * 100 / player->getMaxHealth()) : 100;

		// Priority 1: Self-critical heal (< 60% HP) — survival first + mana shield
		if (selfHpPct < PARTY_HUNT_HEALER_SELF_THRESHOLD) {
			// Use mana shield for extra protection while healing self
			tryCastUtamoVita(bot);
			// Self-heal handled by doHealing() which already ran
			// Don't spend sio cooldown on others when we're in danger
		} else {
			// Priority 2: Heal EK if below threshold
			int32_t ekHpPct = leaderPlayer->getMaxHealth() > 0
				? (leaderPlayer->getHealth() * 100 / leaderPlayer->getMaxHealth()) : 100;

			if (ekHpPct < PARTY_HUNT_HEALER_HP_THRESHOLD) {
				// EK is critically low — activate mana shield for self-protection
				if (ekHpPct < 40) {
					tryCastUtamoVita(bot);
				}

				// Cast exura sio on EK
				if (!player->hasCondition(CONDITION_SPELLCOOLDOWN, sioSpell->getSpellId())) {
					if (player->getMana() < 120) {
						player->mana = player->getMaxMana();
						g_game().addPlayerMana(player);
					}

					// Check range and LOS
					auto leaderPos = leaderPlayer->getPosition();
					if (leaderPos.z == bot.currentPos.z) {
						std::string words = "exura sio \"" + leaderPlayer->getName() + "\"";
						auto result = g_spells().playerSaySpell(player, words);
						if (result == TALKACTION_BREAK) {
							player->saySpell(TALKTYPE_SAY, words, false);
							castLog(bot, fmt::format("PARTY_HUNT: exura sio \"{}\" (hp={}%)",
								leaderPlayer->getName(), ekHpPct));
						}
					}
				}
			} else if (party) {
				// Priority 3: Check other party members
				struct HealTarget {
					std::shared_ptr<Player> player;
					int32_t hpPct;
					int32_t priority;
				};
				std::vector<HealTarget> needsHealing;

				auto checkMember = [&](const std::shared_ptr<Player>& member, int32_t prio) {
					if (!member || member->getID() == player->getID()) return;
					if (member->getHealth() <= 0 || member->isRemoved()) return;
					auto mpos = member->getPosition();
					if (mpos.z != bot.currentPos.z) return;
					int32_t dist = std::max(
						std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(mpos.x)),
						std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(mpos.y)));
					if (dist > 7) return;
					int32_t hpPct = member->getMaxHealth() > 0
						? (member->getHealth() * 100 / member->getMaxHealth()) : 100;
					if (hpPct < 70) {
						needsHealing.push_back({member, hpPct, prio});
					}
				};

				// Check all members
				for (const auto& member : party->getMembers()) {
					if (!member) continue;
					uint8_t memberBaseVoc = 0;
					auto voc = member->getVocation();
					if (voc) memberBaseVoc = voc->getBaseId();
					int32_t prio = (memberBaseVoc == 4) ? 0 : 1; // Knights first
					checkMember(member, prio);
				}
				auto partyLeader = party->getLeader();
				if (partyLeader && partyLeader->getID() != player->getID()) {
					uint8_t leaderBaseVoc = 0;
					auto voc = partyLeader->getVocation();
					if (voc) leaderBaseVoc = voc->getBaseId();
					checkMember(partyLeader, leaderBaseVoc == 4 ? 0 : 1);
				}

				if (!needsHealing.empty()) {
					// Mass heal if 2+ members need healing
					if (needsHealing.size() >= 2 && player->getLevel() >= 36) {
						const auto& masResSpell = g_spells().getInstantSpell("exura gran mas res");
						if (masResSpell
							&& !player->hasCondition(CONDITION_SPELLCOOLDOWN, masResSpell->getSpellId())
							&& player->getMana() >= 150) {

							std::string words = "exura gran mas res";
							auto result = g_spells().playerSaySpell(player, words);
							if (result == TALKACTION_BREAK) {
								player->saySpell(TALKTYPE_SAY, "exura gran mas res", false);
								castLog(bot, fmt::format("PARTY_HUNT: exura gran mas res ({} low)",
									needsHealing.size()));
							}
						}
					} else {
						// Single target heal
						std::sort(needsHealing.begin(), needsHealing.end(),
							[](const auto& a, const auto& b) {
								if (a.priority != b.priority) return a.priority < b.priority;
								return a.hpPct < b.hpPct;
							});

						if (!player->hasCondition(CONDITION_SPELLCOOLDOWN, sioSpell->getSpellId())) {
							auto& target = needsHealing[0];
							std::string words = "exura sio \"" + target.player->getName() + "\"";
							auto result = g_spells().playerSaySpell(player, words);
							if (result == TALKACTION_BREAK) {
								player->saySpell(TALKTYPE_SAY, words, false);
								castLog(bot, fmt::format("PARTY_HUNT: exura sio \"{}\" (hp={}%)",
									target.player->getName(), target.hpPct));
							}
						}
					}
				}
			}
		}
	}

	// === ATTACKING (attack group cooldown — separate from heal!) ===
	// ED can attack while healing is on cooldown
	auto leaderTarget = leaderPlayer->getAttackedCreature();
	if (leaderTarget && leaderTarget->getHealth() > 0) {
		auto tpos = leaderTarget->getPosition();
		if (tpos.z == bot.currentPos.z) {
			// EK engagement gate: don't attack until EK is within melee range (has aggro)
			auto ekPos = leaderPlayer->getPosition();
			int32_t ekDistToTarget = std::max(
				std::abs(static_cast<int32_t>(ekPos.x) - static_cast<int32_t>(tpos.x)),
				std::abs(static_cast<int32_t>(ekPos.y) - static_cast<int32_t>(tpos.y)));
			// Same reasoning as the DPS roles: a kiting PLAYER is never reached by the melee EK,
			// so gating on it would silence the healer's offence for the whole fight.
			if (ekDistToTarget > 2 && !leaderTarget->getPlayer()) return; // EK not engaged — healing still runs

			// Try AoE reposition first (finds best position + spell)
			if (!tryPartyAoeReposition(bot, leaderBot)) {
				// Fallback: cast from current position
				if (player->getAttackedCreature() != leaderTarget) {
					player->setAttackedCreature(leaderTarget);
					int32_t kd = getPartyHuntKeepDistance(bot, leaderBot);
					if (kd > 0) {
						player->setFollowCreature(nullptr);
					}
				}
				castSpell(bot, leaderTarget);
			}
		}
	}
}

// --- MS DPS Mage Role ---
void BotEngine::doPartyHuntDpsMage(BotState& bot, BotState* leaderBot) {
	auto player = bot.getPlayer();
	if (!player || !leaderBot) return;

	auto leaderPlayer = leaderBot->getPlayer();
	if (!leaderPlayer) return;

	// EK engagement gate: don't attack until EK is within melee range of its target
	auto leaderTarget = leaderPlayer->getAttackedCreature();
	if (leaderTarget && leaderTarget->getHealth() > 0) {
		auto tpos = leaderTarget->getPosition();
		auto ekPos = leaderPlayer->getPosition();
		int32_t ekDistToTarget = std::max(
			std::abs(static_cast<int32_t>(ekPos.x) - static_cast<int32_t>(tpos.x)),
			std::abs(static_cast<int32_t>(ekPos.y) - static_cast<int32_t>(tpos.y)));
		// The EK-engagement gate exists so supports do not open before the melee tank has
		// closed on a MONSTER. Against a kiting PLAYER the EK rarely gets within 2, which would
		// leave the whole party silent for the entire fight. Autonomous leaders only ever target
		// monsters (scanAndAttackMonster), so skipping the gate for a player target cannot
		// change autonomous party-hunt behaviour.
		if (ekDistToTarget > 2 && !leaderTarget->getPlayer()) return; // EK hasn't engaged yet — wait

		if (tpos.z == bot.currentPos.z) {
			// Try AoE reposition first (best position + spell for maximum damage)
			// tryPartyAoeReposition has its own walking check (returns false if walking)
			if (tryPartyAoeReposition(bot, leaderBot)) return;

			// Fallback: mirror EK's target with single-target/AoE from current position
			if (player->getAttackedCreature() != leaderTarget) {
				player->setAttackedCreature(leaderTarget);
				int32_t kd = getPartyHuntKeepDistance(bot, leaderBot);
				if (kd > 0) {
					player->setFollowCreature(nullptr);
				}
			}
			castSpell(bot, leaderTarget);
		}
	}
}

// --- RP DPS Ranged Role ---
void BotEngine::doPartyHuntDpsRanged(BotState& bot, BotState* leaderBot) {
	auto player = bot.getPlayer();
	if (!player || !leaderBot) return;

	auto leaderPlayer = leaderBot->getPlayer();
	if (!leaderPlayer) return;

	// EK engagement gate: don't attack until EK is within melee range of its target
	auto leaderTarget = leaderPlayer->getAttackedCreature();
	if (leaderTarget && leaderTarget->getHealth() > 0) {
		auto tpos = leaderTarget->getPosition();
		auto ekPos = leaderPlayer->getPosition();
		int32_t ekDistToTarget = std::max(
			std::abs(static_cast<int32_t>(ekPos.x) - static_cast<int32_t>(tpos.x)),
			std::abs(static_cast<int32_t>(ekPos.y) - static_cast<int32_t>(tpos.y)));
		// The EK-engagement gate exists so supports do not open before the melee tank has
		// closed on a MONSTER. Against a kiting PLAYER the EK rarely gets within 2, which would
		// leave the whole party silent for the entire fight. Autonomous leaders only ever target
		// monsters (scanAndAttackMonster), so skipping the gate for a player target cannot
		// change autonomous party-hunt behaviour.
		if (ekDistToTarget > 2 && !leaderTarget->getPlayer()) return; // EK hasn't engaged yet — wait

		if (tpos.z == bot.currentPos.z) {
			// Try AoE reposition (RP has holy mass AoE)
			if (tryPartyAoeReposition(bot, leaderBot)) return;

			// Fallback: attack EK's target from range
			if (player->getAttackedCreature() != leaderTarget) {
				player->setAttackedCreature(leaderTarget);
				int32_t kd = getPartyHuntKeepDistance(bot, leaderBot);
				if (kd > 0) {
					player->setFollowCreature(nullptr);
				}
			}
			castSpell(bot, leaderTarget);
		}
	}
}

// --- AoE Repositioning Algorithm ---
// Evaluates nearby tiles to find optimal position for AoE spells
// Returns true if AoE was cast or bot is repositioning to cast
bool BotEngine::tryPartyAoeReposition(BotState& bot, BotState* leaderBot) {
	auto player = bot.getPlayer();
	if (!player || !leaderBot) return false;

	// Don't evaluate AoE right after z-change (grace period)
	auto zGraceIt = s_lastZChangeTime.find(bot.guid);
	if (zGraceIt != s_lastZChangeTime.end() && OTSYS_TIME() - zGraceIt->second < Z_CHANGE_GRACE_MS) {
		return false;
	}

	auto leaderPlayer = leaderBot->getPlayer();
	if (!leaderPlayer) return false;

	auto leaderPos = leaderPlayer->getPosition();
	if (leaderPos.z != bot.currentPos.z) return false;

	uint8_t baseVoc = getBaseVocation(bot.vocationId);
	const auto& aoeSpells = resolvedAoeSpells_[baseVoc];
	if (aoeSpells.empty()) return false;

	int32_t level = player->getLevel();
	int32_t mlevel = player->getMagicLevel();
	int32_t keepDist = getPartyHuntKeepDistance(bot, leaderBot);

	// Scan monsters near EK (the cluster center)
	auto spectators = Spectators().find<Monster>(leaderPos, false,
		MONSTER_SCAN_RADIUS + 3, MONSTER_SCAN_RADIUS + 3, MONSTER_SCAN_RADIUS + 3, MONSTER_SCAN_RADIUS + 3);

	std::vector<std::shared_ptr<Creature>> nearbyMonsters;
	for (const auto& spec : spectators) {
		if (spec->isRemoved() || spec->getHealth() <= 0) continue;
		auto cpos = spec->getPosition();
		if (cpos.z != leaderPos.z) continue;
		nearbyMonsters.push_back(spec);
	}

	if (nearbyMonsters.size() < 2) return false; // not enough targets for AoE

	// Generate candidate tiles: current pos + adjacent + tiles on keepDistance arc
	struct CandidateTile {
		Position pos;
		double bestScore = 0;
		Direction bestDir = DIRECTION_NORTH;
		const ResolvedSpell* bestSpell = nullptr;
	};
	std::vector<CandidateTile> candidates;

	auto evaluateTile = [&](const Position& tilePos) -> CandidateTile {
		CandidateTile result;
		result.pos = tilePos;

		// Skip tiles on different z than leader (monsters are on leader's z)
		if (tilePos.z != leaderPos.z) return result;

		// Safety checks
		auto tile = g_game().map.getTile(tilePos);
		if (!tile || tile->hasFlag(TILESTATE_BLOCKPATH) || tile->hasFlag(TILESTATE_FLOORCHANGE)) return result;

		// Check keep-distance from all monsters (avoid being too close)
		int32_t nearestMonsterDist = 999;
		for (const auto& mon : nearbyMonsters) {
			auto mpos = mon->getPosition();
			int32_t d = std::max(
				std::abs(static_cast<int32_t>(tilePos.x) - static_cast<int32_t>(mpos.x)),
				std::abs(static_cast<int32_t>(tilePos.y) - static_cast<int32_t>(mpos.y)));
			nearestMonsterDist = std::min(nearestMonsterDist, d);
		}

		// Skip tiles where we'd be too close to monsters (relaxed by 1 tile for AoE positioning)
		int32_t aoeMinDist = std::max(0, keepDist - 1);
		if (nearestMonsterDist < aoeMinDist && keepDist > 0) return result;

		// P4/P5: keep the AoE/wave/beam reposition tile on the leader's screen (axis-aware viewport
		// leash |dx|<=7,|dy|<=5) AND with a clear shot to the EK (no wall between — P5). The wave/beam
		// scoring itself is untouched — supports still pick the optimal N-sqm-from-the-monster tile for
		// their spell, just never off-screen or walled off from the leader.
		if (!withinLeaderShot(tilePos, leaderPos)) return result;

		// Check creatures blocking (don't step on occupied tiles)
		auto topCreature = tile->getTopCreature();
		if (topCreature && topCreature->getID() != player->getID()) return result;

		// Evaluate each AoE spell from this position
		for (const auto& spell : aoeSpells) {
			if (static_cast<int32_t>(spell.level) > level) continue;
			if (spell.level > 0 && spell.level < 10 && level >= 10) continue;

			if (player->hasCondition(CONDITION_SPELLCOOLDOWN, spell.spellId)) continue;
			if (player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, spell.group)) continue;
			if (spell.secondaryGroup != SPELLGROUP_NONE &&
				player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, spell.secondaryGroup)) continue;

			// For directional spells: find best direction
			if (spell.aoeAreaType == AoeAreaType::WAVE4 || spell.aoeAreaType == AoeAreaType::SQUAREWAVE5
				|| spell.aoeAreaType == AoeAreaType::BEAM5) {
				for (uint8_t d = 0; d <= DIRECTION_WEST; d++) {
					auto dir = static_cast<Direction>(d);
					double weighted = 0;
					int32_t count = 0;
					auto combatType = spell.combatType;

					for (const auto& mon : nearbyMonsters) {
						if (mon->getPosition().z != tilePos.z) continue;
						if (spellHits(tilePos, mon->getPosition(), spell, dir)) {
							if (g_game().map.isSightClear(tilePos, mon->getPosition(), true)) {
								count++;
								int32_t resist = getElementResistance(mon, combatType);
								if (resist < 50) {
									weighted += (100.0 - static_cast<double>(resist)) / 100.0;
								}
							}
						}
					}

					if (count >= spell.minTargets) {
						double dmgPerTarget = (spell.avgMlCoef > 0)
							? estimateDamage(level, mlevel, spell.avgMlCoef, spell.avgConst)
							: static_cast<double>(level) / 5.0;
						double score = dmgPerTarget * weighted;
						if (score > result.bestScore) {
							result.bestScore = score;
							result.bestDir = dir;
							result.bestSpell = &spell;
						}
					}
				}
			} else {
				double weighted = 0;
				int32_t count = 0;
				auto combatType = spell.combatType;

				for (const auto& mon : nearbyMonsters) {
					if (mon->getPosition().z != tilePos.z) continue;
					if (spellHits(tilePos, mon->getPosition(), spell, DIRECTION_NORTH)) {
						if (g_game().map.isSightClear(tilePos, mon->getPosition(), true)) {
							count++;
							int32_t resist = getElementResistance(mon, combatType);
							if (resist < 50) {
								weighted += (100.0 - static_cast<double>(resist)) / 100.0;
							}
						}
					}
				}

				if (count >= spell.minTargets) {
					double dmgPerTarget = (spell.avgMlCoef > 0)
						? estimateDamage(level, mlevel, spell.avgMlCoef, spell.avgConst)
						: static_cast<double>(level) / 5.0;
					double score = dmgPerTarget * weighted;
					if (score > result.bestScore) {
						result.bestScore = score;
						result.bestDir = DIRECTION_NORTH;
						result.bestSpell = &spell;
					}
				}
			}
		}

		return result;
	};

	// === P8 perf: cast-in-place FIRST, and short-circuit ===
	// Evaluate ONLY the current tile first. If we can already land an AoE on >=minTargets from here,
	// cast and return immediately — WITHOUT generating/evaluating the other ~20 candidate tiles. This is
	// behavior-identical to before (cast-in-place already won unconditionally — see the inc1 fix), it just
	// skips the ~20 evaluateTile() calls (each = per-spell x per-monster spellHits + isSightClear) whose
	// scores were never used in the common case (support already on its formation slot, which has good AoE
	// coverage). ~20x less work on the hot path during combat.
	CandidateTile currentTile = evaluateTile(bot.currentPos);
	if (currentTile.bestSpell && currentTile.bestScore > 0) {
		castAoeSpell(bot, currentTile.bestSpell, currentTile.bestDir);
		return true;
	}

	// Current tile can't land an AoE — evaluate the other candidates and reposition to the best that can.
	candidates.push_back(currentTile);

	// Evaluate 8 adjacent tiles
	static const int32_t dx8[] = {-1, 0, 1, -1, 1, -1, 0, 1};
	static const int32_t dy8[] = {-1, -1, -1, 0, 0, 1, 1, 1};
	for (int i = 0; i < 8; i++) {
		Position adj(bot.currentPos.x + dx8[i], bot.currentPos.y + dy8[i], bot.currentPos.z);
		candidates.push_back(evaluateTile(adj));
	}

	// Evaluate tiles on keepDistance arc around EK (sample 8 positions)
	if (keepDist > 0) {
		for (int angle = 0; angle < 8; angle++) {
			int32_t ox = 0, oy = 0;
			switch (angle) {
				case 0: ox = 0;         oy = -keepDist; break; // N
				case 1: ox = keepDist;  oy = -keepDist; break; // NE
				case 2: ox = keepDist;  oy = 0;         break; // E
				case 3: ox = keepDist;  oy = keepDist;  break; // SE
				case 4: ox = 0;         oy = keepDist;  break; // S
				case 5: ox = -keepDist; oy = keepDist;  break; // SW
				case 6: ox = -keepDist; oy = 0;         break; // W
				case 7: ox = -keepDist; oy = -keepDist; break; // NW
			}
			Position arcPos(leaderPos.x + ox, leaderPos.y + oy, leaderPos.z);
			// Only if within reasonable walking distance
			int32_t walkDist = std::max(
				std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(arcPos.x)),
				std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(arcPos.y)));
			if (walkDist <= PARTY_HUNT_AOE_EVAL_RADIUS) {
				candidates.push_back(evaluateTile(arcPos));
			}
		}
	}

	// Add candidate tiles along the line from bot toward monster centroid
	if (!nearbyMonsters.empty()) {
		int64_t sumX = 0, sumY = 0;
		for (const auto& mon : nearbyMonsters) {
			sumX += mon->getPosition().x;
			sumY += mon->getPosition().y;
		}
		Position centroid(
			static_cast<uint16_t>(sumX / static_cast<int64_t>(nearbyMonsters.size())),
			static_cast<uint16_t>(sumY / static_cast<int64_t>(nearbyMonsters.size())),
			leaderPos.z);
		for (int step = 1; step <= 4; step++) {
			int32_t ix = static_cast<int32_t>(bot.currentPos.x) +
				(static_cast<int32_t>(centroid.x) - static_cast<int32_t>(bot.currentPos.x)) * step / 5;
			int32_t iy = static_cast<int32_t>(bot.currentPos.y) +
				(static_cast<int32_t>(centroid.y) - static_cast<int32_t>(bot.currentPos.y)) * step / 5;
			Position stepPos(static_cast<uint16_t>(ix), static_cast<uint16_t>(iy), leaderPos.z);
			int32_t walkDist = std::max(
				std::abs(static_cast<int32_t>(bot.currentPos.x) - ix),
				std::abs(static_cast<int32_t>(bot.currentPos.y) - iy));
			if (walkDist <= PARTY_HUNT_AOE_EVAL_RADIUS) {
				candidates.push_back(evaluateTile(stepPos));
			}
		}
	}

	// Current tile can't land an AoE — find the best reachable candidate tile that can, and move to it.
	// (The cast-in-place case already returned above before these candidates were even evaluated.)
	CandidateTile* best = nullptr;
	for (auto& c : candidates) {
		if (c.bestSpell && c.bestScore > 0) {
			if (!best || c.bestScore > best->bestScore) {
				best = &c;
			}
		}
	}

	if (!best || !best->bestSpell) return false;

	// Best is the current tile (shouldn't happen — current couldn't cast above) — cast anyway.
	if (best->pos.x == bot.currentPos.x && best->pos.y == bot.currentPos.y) {
		castAoeSpell(bot, best->bestSpell, best->bestDir);
		return true;
	}

	// Reposition: pathfind to best tile
	if (!player->listWalkDir.empty()) {
		// Already walking — check if we're heading to the right place
		return false;
	}

	FindPathParams fpp;
	fpp.fullPathSearch = false;
	fpp.clearSight = true;
	fpp.allowDiagonal = true;
	fpp.keepDistance = false;
	fpp.maxSearchDist = PARTY_HUNT_AOE_EVAL_RADIUS + 2;
	fpp.minTargetDist = 0;
	fpp.maxTargetDist = 0;

	std::vector<Direction> dirList;
	if (g_game().map.getPathMatching(player, best->pos, dirList, FrozenPathingConditionCall(best->pos), fpp)) {
		botStartAutoWalk(bot, player,dirList);
		castLog(bot, fmt::format("PARTY_HUNT: Repositioning for AoE ({},{},{}) score={:.0f}",
			best->pos.x, best->pos.y, best->pos.z, best->bestScore));
		return true;
	}

	// Can't reach — cast from current position if possible
	if (currentTile.bestSpell) {
		castAoeSpell(bot, currentTile.bestSpell, currentTile.bestDir);
		return true;
	}

	return false;
}

// --- Party Member Spread: prevent support bots from stacking on the same tile ---
// Returns true if the bot stepped aside (caller should return).
bool BotEngine::tryPartyMemberSpread(BotState& bot, const Position& leaderPos, int32_t keepDist) {
	auto player = bot.getPlayer();
	if (!player || !player->listWalkDir.empty()) return false; // already walking

	// Cooldown to prevent oscillation
	auto cdIt = s_spreadCooldown.find(bot.guid);
	if (cdIt != s_spreadCooldown.end() && OTSYS_TIME() < cdIt->second) return false;

	// Resolve the party member guid list. Autonomous hunts use s_partyHuntMembers[partyHuntId];
	// human-led parties (partyHuntId==0) fall back to the Canary Party's bot members (P7).
	std::vector<uint32_t> humanPartyGuids;
	const std::vector<uint32_t>* memberGuids = nullptr;
	auto membersIt = s_partyHuntMembers.find(bot.partyHuntId);
	if (membersIt != s_partyHuntMembers.end()) {
		memberGuids = &membersIt->second;
	} else if (auto party = player->getParty()) {
		for (const auto& m : party->getMembers()) {
			if (m && m->isBotPlayer()) humanPartyGuids.push_back(m->getGUID());
		}
		memberGuids = &humanPartyGuids;
	}
	if (!memberGuids || memberGuids->empty()) return false;

	// Check if another party member is on the same tile
	bool hasOverlap = false;
	uint32_t overlapGuid = 0;
	for (uint32_t memberGuid : *memberGuids) {
		if (memberGuid == bot.guid) continue;
		auto idx = guidToIndex_.find(memberGuid);
		if (idx == guidToIndex_.end()) continue;
		auto& other = bots_[idx->second];
		if (!other.active) continue;
		if (other.currentPos == bot.currentPos) {
			hasOverlap = true;
			overlapGuid = memberGuid;
			break;
		}
	}
	if (!hasOverlap) return false;

	// Only the higher-guid bot spreads to prevent oscillation (both bots spreading simultaneously)
	if (bot.guid < overlapGuid) return false;

	// Try 8 directions, prefer tiles that maintain keepDistance + LOS to leader
	int32_t attackRange = static_cast<int32_t>(getAttackRange(getBaseVocation(bot.vocationId)));
	int32_t safeMax = std::max(keepDist > 0 ? keepDist + 1 : 1, attackRange);

	static const Direction dirs[] = {
		DIRECTION_NORTH, DIRECTION_SOUTH, DIRECTION_EAST, DIRECTION_WEST,
		DIRECTION_NORTHEAST, DIRECTION_NORTHWEST, DIRECTION_SOUTHEAST, DIRECTION_SOUTHWEST
	};

	Direction bestDir = DIRECTION_NONE;
	int32_t bestScore = -1;

	for (auto dir : dirs) {
		Position candidatePos = getNextPosition(dir, bot.currentPos);
		auto tile = g_game().map.getTile(candidatePos);
		if (!tile || tile->hasFlag(TILESTATE_BLOCKPATH) || tile->hasFlag(TILESTATE_BLOCKSOLID)) continue;
		if (tile->getCreatureCount() > 0) continue; // don't step onto occupied tiles

		int32_t distToLeader = std::max(
			std::abs(static_cast<int32_t>(candidatePos.x) - static_cast<int32_t>(leaderPos.x)),
			std::abs(static_cast<int32_t>(candidatePos.y) - static_cast<int32_t>(leaderPos.y)));

		bool hasLOS = g_game().map.isSightClear(candidatePos, leaderPos, true);

		int32_t score = 0;
		if (hasLOS) score += 10;
		if (distToLeader <= safeMax) score += 5;
		if (distToLeader >= (keepDist > 0 ? keepDist : 1)) score += 3;

		// Check tile isn't occupied by another party member
		bool otherPartyOnTile = false;
		for (uint32_t memberGuid : *memberGuids) {
			if (memberGuid == bot.guid) continue;
			auto idx = guidToIndex_.find(memberGuid);
			if (idx == guidToIndex_.end()) continue;
			if (bots_[idx->second].currentPos == candidatePos) {
				otherPartyOnTile = true;
				break;
			}
		}
		if (otherPartyOnTile) continue;

		if (score > bestScore) {
			bestScore = score;
			bestDir = dir;
		}
	}

	if (bestDir != DIRECTION_NONE) {
		botStartAutoWalk(bot, player,{bestDir});
		s_spreadCooldown[bot.guid] = OTSYS_TIME() + 2000LL; // 2s cooldown before trying again
		// Also suppress approach so we don't immediately path back to the same tile
		s_approachCooldown[bot.guid] = OTSYS_TIME() + 3000LL;
		return true;
	}

	// No free tile found — set cooldown anyway to prevent spam
	s_spreadCooldown[bot.guid] = OTSYS_TIME() + 3000LL;
	return false;
}

// ============================================================================
// BOT_PARTY_TRAIL_FOLLOW — leader breadcrumb trail recorder + [PTRAIL] telemetry
// (implementation_plans/BOT_PARTY_TRAIL_FOLLOW.md)
// ============================================================================

// Called once at the top of tick(), so every wanted leader is sampled at the full engine
// cadence (200ms) regardless of that leader's own isTickDue phase. Free at steady state:
// most party hunts are virtual/hibernated and never register demand, so the map is empty.
void BotEngine::recordLeaderTrails(int64_t nowMs) {
	if (s_trailWanted.empty()) {
		return;
	}
	for (auto it = s_trailWanted.begin(); it != s_trailWanted.end();) {
		const uint32_t leaderGuid = it->first;
		if (nowMs >= it->second.expiresMs) {
			// Demand expired (no follower asked for >TRAIL_WANT_TTL_MS) — drop the trail with it.
			s_leaderTrail.erase(leaderGuid);
			it = s_trailWanted.erase(it);
			continue;
		}
		auto leaderCreature = g_game().getCreatureByID(it->second.creatureId);
		auto leader = leaderCreature ? leaderCreature->getPlayer() : nullptr;
		if (!leader || leader->isRemoved()) {
			++it;
			continue;
		}

		auto& trail = s_leaderTrail[leaderGuid];
		const Position cur = leader->getPosition();
		if (trail.lastRecordMs == 0) {
			// First sample: establish the origin, record nothing yet.
			trail.lastPos = cur;
			trail.lastRecordMs = nowMs;
			++it;
			continue;
		}
		if (cur == trail.lastPos) {
			// Stationary leader: nothing to record. lastRecordMs deliberately NOT bumped —
			// node/trail age measures how old the newest RECORDED step is.
			++it;
			continue;
		}

		// Staleness: a trail idle longer than maxAgeMs describes a world that may no longer
		// exist (hibernate/re-wake, possibly relocated by proximity weighting). Restart it
		// rather than splicing a giant JUMP across the gap — but never yank the deque out from
		// under a live cursor mid-walk (pruning has the same rule below).
		if (nowMs - trail.lastRecordMs > trailCfg_.maxAgeMs && !trail.nodes.empty()) {
			bool cursorAlive = false;
			for (const auto& [fGuid, cursor] : s_followerCursor) {
				if (cursor.leaderGuid == leaderGuid) {
					cursorAlive = true;
					break;
				}
			}
			if (!cursorAlive) {
				trail.nodes.clear();
				trail.lastPos = cur;
				trail.lastRecordMs = nowMs;
				++it;
				continue;
			}
		}

		const int32_t d = std::max(
			std::abs(static_cast<int32_t>(cur.x) - static_cast<int32_t>(trail.lastPos.x)),
			std::abs(static_cast<int32_t>(cur.y) - static_cast<int32_t>(trail.lastPos.y)));
		const int32_t dz = std::abs(static_cast<int32_t>(cur.z) - static_cast<int32_t>(trail.lastPos.z));

		TrailNode node;
		node.seq = trail.nextSeq++;
		node.prePos = trail.lastPos;
		node.postPos = cur;
		node.recordedAtMs = nowMs;
		if (dz == 0 && d <= 3) {
			node.kind = TrailNodeKind::STEP;
		} else if (dz != 0 && d <= 2) {
			node.kind = TrailNodeKind::ZHOP;
			resolveTrailZHopPortal(leaderGuid, node); // capture the mechanism at record time
		} else {
			node.kind = TrailNodeKind::JUMP; // BOT_TELEPORT, boat, quest teleporter, AdvStone
		}
		trail.nodes.push_back(std::move(node));
		trail.lastPos = cur;
		trail.lastRecordMs = nowMs;

		// Prune to maxNodes, oldest first — but never past the lowest live cursor still walking
		// this trail (a follower's route must not evaporate underneath it).
		const size_t cap = static_cast<size_t>(std::max<int32_t>(trailCfg_.maxNodes, 8));
		if (trail.nodes.size() > cap) {
			uint32_t lowestLiveSeq = UINT32_MAX;
			for (const auto& [fGuid, cursor] : s_followerCursor) {
				if (cursor.leaderGuid == leaderGuid && cursor.seq < lowestLiveSeq) {
					lowestLiveSeq = cursor.seq;
				}
			}
			while (trail.nodes.size() > cap && trail.nodes.front().seq < lowestLiveSeq) {
				trail.nodes.pop_front();
			}
		}
		++it;
	}
}

// 60s cadence, unconditional g_logger (castLog is verboseLog-gated and therefore not a usable
// global baseline). Emitted only while at least one AWAKE party exists or a counter is nonzero
// — s_partyHuntMembers alone is NOT the gate, because virtual/hibernated party hunts keep it
// populated indefinitely and would produce an all-zero line forever.
void BotEngine::emitPtrailSummaryIfDue(int64_t nowMs) {
	if (s_ptrail.lastEmitMs == 0) {
		s_ptrail.lastEmitMs = nowMs;
		return;
	}
	if (nowMs - s_ptrail.lastEmitMs < 60 * 1000) {
		return;
	}
	s_ptrail.lastEmitMs = nowMs;

	const bool anyCount = (s_ptrail.legs | s_ptrail.zhopOk | s_ptrail.zhopFail | s_ptrail.jumpTele
		| s_ptrail.watchdogTele | s_ptrail.partyTele | s_ptrail.formationTele | s_ptrail.respawnTele) != 0;

	bool awakeParty = false;
	for (const auto& [guid, cid] : s_partyLeaderId) { // human-led party members + party-hunt supports
		auto idxIt = guidToIndex_.find(guid);
		if (idxIt != guidToIndex_.end() && bots_[idxIt->second].active) {
			awakeParty = true;
			break;
		}
	}
	if (!awakeParty) {
		for (const auto& [phId, members] : s_partyHuntMembers) {
			for (uint32_t guid : members) {
				auto idxIt = guidToIndex_.find(guid);
				if (idxIt != guidToIndex_.end() && bots_[idxIt->second].active) {
					awakeParty = true;
					break;
				}
			}
			if (awakeParty) {
				break;
			}
		}
	}
	if (!awakeParty && !anyCount) {
		return;
	}

	size_t trailNodes = 0;
	for (const auto& [guid, trail] : s_leaderTrail) {
		trailNodes += trail.nodes.size();
	}

	// Skulled-bot count: the live number every autonomous PvP cap reads (gang / vigilante /
	// random PK all stand down at >=5%), so party PvP assist is visible in the same terms even
	// though it deliberately does not consult the cap itself.
	uint32_t skulledBots = 0;
	for (const auto& b : bots_) {
		if (!b.active) continue;
		if (auto bp = b.getPlayer(); bp && bp->getSkull() >= SKULL_WHITE) skulledBots++;
	}

	g_logger().info("[PTRAIL] parties={} legs={} zhopOk={} zhopFail={} jumpTele={} watchdogTele={} "
		"partyTele={} formTele={} respawnTele={} zhopAband={} nodes={} cursors={} partyBound={}/{} capRefused={} "
		"pvpAssist={} skulled={} staged={} (per 60s)",
		s_partyHuntMembers.size(), s_ptrail.legs, s_ptrail.zhopOk, s_ptrail.zhopFail,
		s_ptrail.jumpTele, s_ptrail.watchdogTele, s_ptrail.partyTele, s_ptrail.formationTele,
		s_ptrail.respawnTele, s_ptrail.zhopAbandoned, trailNodes, s_followerCursor.size(),
		s_botToPartyHunt.size(), countActiveBots(), s_partyCapRefusals,
		s_prv.pvpAssistEngagements, skulledBots, s_prv.asmStaged);

	const int64_t keepEmitMs = s_ptrail.lastEmitMs;
	s_ptrail = PtrailCounters {};
	s_ptrail.lastEmitMs = keepEmitMs;
	s_prv.pvpAssistEngagements = 0; // per-window, like the counters above
	s_prv.asmStaged = 0;
}

static inline int32_t trailCheb(const Position& a, const Position& b) {
	return std::max(std::abs(static_cast<int32_t>(a.x) - static_cast<int32_t>(b.x)),
		std::abs(static_cast<int32_t>(a.y) - static_cast<int32_t>(b.y)));
}

// Kind resolution, best source first (plan 3.6):
//  (a) a scripted waypoint of a floor-change type near P — hand-authored ground truth, needs
//      no disambiguation. Includes LEVITATE_UP/DOWN, which the portal graph does not index —
//      those keep kind INFERRED (the FC machine cannot levitate) but the curated match still
//      blocks a wrong portal-graph resolution.
//  (b) the live portal graph around P. Landing distance is the disambiguator, not position:
//      two side-by-side staircases differ by DESTINATION, which position-only matching cannot
//      resolve. Radius 3 because the P->portal distance is mechanism-dependent: rope spots are
//      stepped ONTO first (portal.pos == P), ladder/sewer/shovel fire USE from distance 1, and
//      stairs/holes relocate the leader as part of the move so the stair tile is never sampled.
//  (c) nothing — portalResolved stays false; the executor seeds a synthetic INFERRED hop.
void BotEngine::resolveTrailZHopPortal(uint32_t leaderGuid, TrailNode& node) {
	const Position& P = node.prePos;
	const Position& L = node.postPos;
	const bool goesDown = L.z > P.z;

	// (a) curated waypoint type from the leader's active hunt script (bot leaders only —
	// a human leaderGuid simply misses guidToIndex_ and falls through to the graph).
	auto leaderIdx = guidToIndex_.find(leaderGuid);
	if (leaderIdx != guidToIndex_.end()) {
		const BotState& leaderBot = bots_[leaderIdx->second];
		if (leaderBot.huntScriptId > 0) {
			const HuntScript* script = nullptr;
			for (const auto& s : huntScripts_) {
				if (s.id == leaderBot.huntScriptId) {
					script = &s;
					break;
				}
			}
			if (script != nullptr) {
				const std::vector<Waypoint>* lists[3] = {
					&script->travelToWaypoints, &script->patrolWaypoints, &script->travelFromWaypoints
				};
				const Waypoint* bestWp = nullptr;
				int32_t bestD = INT32_MAX;
				for (const auto* wps : lists) {
					for (const auto& wp : *wps) {
						if (!isFloorChangeType(wp.type)) {
							continue;
						}
						if (wp.pos.z != P.z) {
							continue;
						}
						const int32_t d = trailCheb(wp.pos, P);
						if (d <= 3 && d < bestD) {
							bestD = d;
							bestWp = &wp;
						}
					}
				}
				if (bestWp != nullptr) {
					node.portal.pos = bestWp->pos;
					node.portal.landing = L; // the OBSERVED landing beats any estimate
					node.portal.goesDown = goesDown;
					switch (bestWp->type) {
						case WaypointType::LADDER:
							node.portal.kind = botnav::ZPortalKind::LADDER;
							break;
						case WaypointType::ROPE:
							node.portal.kind = botnav::ZPortalKind::ROPE_SPOT;
							break;
						case WaypointType::HOLE:
							node.portal.kind = botnav::ZPortalKind::HOLE;
							break;
						case WaypointType::STAIRS_UP:
						case WaypointType::STAIRS_DOWN:
							node.portal.kind = botnav::ZPortalKind::STAIRS;
							break;
						default: // LEVITATE_UP / LEVITATE_DOWN
							node.portal.kind = botnav::ZPortalKind::INFERRED;
							break;
					}
					node.portalResolved = true;
					return;
				}
			}
		}
	}

	// (b) portal graph around P: direction must match, landing must corroborate the observed L.
	if (zGraphReady_) {
		const int64_t nowMs = OTSYS_TIME();
		const botnav::ZPortal* best = nullptr;
		int32_t bestScore = INT32_MAX;
		int32_t bestKindRank = INT32_MAX;
		uint32_t bestIdx = 0;
		zGraph_.forEachOnFloorNear(P.z, P, 3, [&](uint32_t idx, const botnav::ZPortal& p) {
			if (p.goesDown != goesDown) {
				return;
			}
			if (p.landing.z != L.z) {
				return;
			}
			const int32_t landDist = trailCheb(L, p.landing);
			if (landDist > 4) {
				return;
			}
			auto blIt = s_zPortalBlacklist.find(botTileKey(p.pos));
			if (blIt != s_zPortalBlacklist.end() && blIt->second > nowMs) {
				return;
			}
			const int32_t posDist = trailCheb(P, p.pos);
			const int32_t score = posDist + landDist;
			const int32_t kindRank = static_cast<int32_t>(p.kind); // deterministic tie-break
			if (score < bestScore
				|| (score == bestScore
					&& (kindRank < bestKindRank || (kindRank == bestKindRank && idx < bestIdx)))) {
				bestScore = score;
				bestKindRank = kindRank;
				bestIdx = idx;
				best = &p;
			}
		});
		if (best != nullptr) {
			node.portal = *best;
			node.portalResolved = true;
			return;
		}
	}
	// (c) unresolved — node.portalResolved stays false.
}

// Replays "the leader was at P, ended at L one floor away" through the EXISTING floor-change
// machine: seed s_plannedFc with the captured (or synthetic) portal, call startFloorChange,
// and read the outcome on re-entry — processBot early-returns into handleFloorChange for the
// entire session, so this function is never called mid-session.
TrailZHopResult BotEngine::executeTrailZHop(BotState& bot, const TrailNode& node, uint32_t leaderGuid) {
	auto player = bot.getPlayer();
	if (!player) {
		return TrailZHopResult::GIVE_UP;
	}
	const int64_t nowMs = OTSYS_TIME();

	auto sesIt = s_followerZHopSession.find(bot.guid);
	if (sesIt != s_followerZHopSession.end() && sesIt->second.nodeSeq != node.seq) {
		s_followerZHopSession.erase(sesIt); // session for another node — stale
		sesIt = s_followerZHopSession.end();
	}

	if (sesIt != s_followerZHopSession.end()) {
		if (bot.fcState != FloorChangeState::NONE) {
			return TrailZHopResult::IN_PROGRESS; // defensive; processBot normally shields this
		}
		// The FC session this record belongs to has ENDED — read the outcome. The z of the
		// observed landing is the hard success gate.
		if (bot.currentPos.z == node.postPos.z) {
			// TRAP (plan §4): the stale-walk-queue guard at the z-change site computes
			// isFollowingWps WITHOUT the PARTY state — deliberately unchanged there — so any
			// old-floor steps still queued would be walked out on the NEW floor. Clear them
			// here instead.
			if (!player->listWalkDir.empty()) {
				player->listWalkDir.clear();
				player->stopEventWalk();
			}
			s_followerZHopSession.erase(bot.guid);
			return TrailZHopResult::SUCCEEDED;
		}
		if (sesIt->second.attempts >= ZHOP_MAX_SESSION_ATTEMPTS) {
			s_followerZHopSession.erase(bot.guid);
			return TrailZHopResult::GIVE_UP; // cap replaces the blacklist's anti-livelock role
		}
		// fall through: stage another attempt
	}

	const Position& pre = node.prePos; // "P"
	if (pre.z != bot.currentPos.z) {
		s_followerZHopSession.erase(bot.guid);
		return TrailZHopResult::GIVE_UP; // wrong floor to stage from (drifted / bad anchor)
	}
	const int32_t dPre = trailCheb(bot.currentPos, pre);
	if (dPre > 6) {
		return TrailZHopResult::DECLINED_FAR; // the STEP walker closes this gap
	}
	if (dPre > 3) {
		if (player->listWalkDir.empty()) {
			goTo(bot, pre, 1);
		}
		return TrailZHopResult::APPROACHING;
	}

	// Portal tile is telemetry-only now (the exclusive claim that used to key on it is gone).
	const Position portalPos = node.portalResolved ? node.portal.pos : pre;
	// <=3 tiles: start (or restart) an FC session.
	//
	// ROUND2 A3: the old EXCLUSIVE portal claim (one owner per tile for 3s, never released early)
	// serialized the whole party — 3 supports took ~9s to follow, single file, which is exactly what
	// was observed live. Concurrent traversal is safe: walk-on portals step with FLAG_NOLIMIT so
	// occupancy cannot fail the step (and the first traverser is relocated off the tile by the move
	// itself), and ladder/rope/sewer/shovel are item USES from distance 0-1 that need no tile
	// exclusivity. All that remains is a per-follower cosmetic stagger so N followers eligible on the
	// same tick do not issue startFloorChange in that same tick.
	{
		auto& cur = s_followerCursor[bot.guid];
		if (cur.zhopStartAfterMs == 0) {
			cur.zhopStartAfterMs = nowMs + 120 + static_cast<int64_t>(botNavSeed(bot.guid) % 280);
		}
		if (nowMs < cur.zhopStartAfterMs) {
			return TrailZHopResult::APPROACHING; // staggering, not queueing — burns no attempt
		}
	}

	// startFloorChange silently no-ops at >=5 consecutive FC failures (no return signal) —
	// convert that into an honest GIVE_UP instead of an invisible stall.
	if (s_fcConsecutiveFailures[bot.guid] >= 5) {
		s_followerZHopSession.erase(bot.guid);
		return TrailZHopResult::GIVE_UP;
	}

	auto& ses = s_followerZHopSession[bot.guid];
	if (ses.nodeSeq != node.seq) {
		ses = FollowerZHopSession {};
		ses.leaderGuid = leaderGuid;
		ses.nodeSeq = node.seq;
		ses.expectedPre = pre;
		ses.expectedLanding = node.postPos;
		ses.startedAtMs = nowMs;
		ses.portalResolved = node.portalResolved;
	}
	ses.attempts++;

	// Seed the planned hop BEFORE startFloorChange so SCANNING injects it first. An unresolved
	// node gets a synthetic INFERRED portal anchored at P with landing = L — never left
	// unseeded, because startFloorChange would then run its own whole-map plan and can select
	// a portal up to Z_LEG_MAX tiles away (a detour, not a replay). SCANNING still runs its
	// independent live-tile scan, which re-checks tile flags and ladder/rope/sewer/shovel item
	// ids directly, so the real mechanism is usually found anyway.
	ZPlannedHop hop;
	if (node.portalResolved) {
		hop.portal = node.portal;
	} else {
		hop.portal.pos = pre;
		hop.portal.landing = node.postPos;
		hop.portal.kind = botnav::ZPortalKind::INFERRED;
		hop.portal.goesDown = node.postPos.z > pre.z;
	}
	hop.plannedAt = nowMs;
	s_plannedFc[bot.guid] = hop;

	// Drop any queued steps before the machine takes over (same trap as on landing).
	if (!player->listWalkDir.empty()) {
		player->listWalkDir.clear();
		player->stopEventWalk();
	}
	player->setFollowCreature(nullptr);

	const bool goDown = node.postPos.z > pre.z;
	// fcTargetPos = the OBSERVED landing, not our estimated portal.landing: it feeds
	// VERIFYING's wrong-direction gate and SCANNING's extra search anchor.
	startFloorChange(bot, goDown, node.postPos);
	if (bot.fcState == FloorChangeState::NONE) {
		// Refused to start (defensive) — treat like a failed attempt.
		return ses.attempts >= ZHOP_MAX_SESSION_ATTEMPTS ? TrailZHopResult::GIVE_UP
														 : TrailZHopResult::APPROACHING;
	}
	castLog(bot, fmt::format("TRAIL: ZHOP attempt {}/{} via {} ({},{},{}) -> ({},{},{})",
		ses.attempts, ZHOP_MAX_SESSION_ATTEMPTS,
		node.portalResolved ? botnav::zPortalKindName(node.portal.kind) : "inferred",
		portalPos.x, portalPos.y, portalPos.z,
		node.postPos.x, node.postPos.y, node.postPos.z));
	return TrailZHopResult::IN_PROGRESS;
}

// Bounded leader wait (plan 3.5): the party-hunt leader stops ADVANCING waypoints while an
// awake member straggles (different floor, or > botPartyLeaderWaitDist away), for at most
// botPartyLeaderWaitMaxMs per episode. Everything above the doHuntPatrol gate still runs —
// hunt-time expiry, scanAndAttackMonster, kill lingering — so the EK keeps fighting, casting
// exeta res and holding the lure; it just stops walking away. One [PARTYWAIT] journal line
// per hold start / end (resume or expiry).
bool BotEngine::partyLeaderShouldHoldForStragglers(BotState& bot) {
	if (!trailCfg_.enable) {
		return false; // kill switch: today's behavior
	}
	if (!bot.isPartyHuntLeader || bot.partyHuntId == 0) {
		return false;
	}
	auto membersIt = s_partyHuntMembers.find(bot.partyHuntId);
	if (membersIt == s_partyHuntMembers.end()) {
		s_partyWaitStartMs.erase(bot.guid);
		return false;
	}
	const int64_t nowMs = OTSYS_TIME();
	bool straggler = false;
	for (uint32_t guid : membersIt->second) {
		if (guid == bot.guid) {
			continue;
		}
		auto idxIt = guidToIndex_.find(guid);
		if (idxIt == guidToIndex_.end()) {
			continue;
		}
		const BotState& member = bots_[idxIt->second];
		if (!member.active || member.state != BotAIState::PARTY) {
			continue; // only AWAKE followers can straggle; virtual members ride the simulation
		}
		if (member.deathPauseUntil > nowMs) {
			continue; // dead / temple-chilling — the respawn rejoin teleports regardless
		}
		// ROUND2 A2: x/y distance ONLY — deliberately NOT z. The old `z != z ||` clause made every
		// follower a straggler the instant the leader changed floor, so the leader froze ON the
		// landing tile (the tile the followers then had to arrive on) and the party filed up one at
		// a time. Ignoring z lets the leader walk on; the x/y gap grows as it advances and the hold
		// fires at waitDist, several tiles clear of the transition. Not a stranding risk: a follower
		// that cannot make the hop runs its own ladder (2 ZHOP attempts -> GIVE_UP -> the untouched
		// separation teleport), and this hold was already bounded at waitMaxMs regardless.
		if (trailCheb(member.currentPos, bot.currentPos) > trailCfg_.waitDist) {
			straggler = true;
			break;
		}
	}
	auto holdIt = s_partyWaitStartMs.find(bot.guid);
	if (!straggler) {
		if (holdIt != s_partyWaitStartMs.end()) {
			if (holdIt->second != INT64_MIN) {
				g_logger().info("[PARTYWAIT] party #{} leader '{}' resumes — stragglers caught up",
					bot.partyHuntId, bot.name);
			}
			s_partyWaitStartMs.erase(holdIt); // re-arm for the next episode
		}
		return false;
	}
	if (holdIt == s_partyWaitStartMs.end()) {
		s_partyWaitStartMs[bot.guid] = nowMs;
		g_logger().info("[PARTYWAIT] party #{} leader '{}' holding for stragglers (>{} tiles or z-diff, max {}ms)",
			bot.partyHuntId, bot.name, trailCfg_.waitDist, trailCfg_.waitMaxMs);
		return true;
	}
	if (holdIt->second == INT64_MIN) {
		return false; // this episode's budget already expired — advance until they catch up
	}
	if (nowMs - holdIt->second >= trailCfg_.waitMaxMs) {
		g_logger().info("[PARTYWAIT] party #{} leader '{}' hold expired after {}ms — advancing anyway",
			bot.partyHuntId, bot.name, trailCfg_.waitMaxMs);
		holdIt->second = INT64_MIN; // expired marker: don't instantly re-arm, don't re-log
		return false;
	}
	return true;
}

// The only behavioral change point (plan 3.4). Returns true = "I moved / am moving this tick"
// (the caller returns and its teleport is skipped); false = "give up, use the old teleport".
// Every false return drops the cursor — returning false IS the decision to fall back.
bool BotEngine::tryFollowLeaderTrail(BotState& bot, const std::shared_ptr<Player>& leader) {
	if (!trailCfg_.enable) {
		return false; // kill switch: bit-identical to today's teleports
	}
	auto player = bot.getPlayer();
	if (!player || !leader) {
		return false;
	}
	const int64_t nowMs = OTSYS_TIME();
	const uint32_t leaderGuid = leader->getGUID();
	const Position leaderPos = leader->getPosition();
	const bool diffZ = bot.currentPos.z != leaderPos.z;

	auto giveUp = [&](bool countWatchdog) {
		if (s_followerCursor.erase(bot.guid) > 0 && countWatchdog) {
			s_ptrail.watchdogTele++;
		}
		trailRetireZHopSession(bot.guid, bot.currentPos.z); // ROUND2 A4
		return false;
	};

	// 4b (review finding #5, accepted with modification): same-floor separation takes the
	// DIRECT path to where the leader is NOW first — a historical meandering replay is worse,
	// and needs none of the portal machinery. Only when no direct path exists (precisely the
	// case where today's code gives up and teleports) fall through to trail replay.
	if (!diffZ) {
		if (!player->listWalkDir.empty()) {
			return true; // a leg is already in flight
		}
		FindPathParams fpp;
		fpp.fullPathSearch = true;
		fpp.clearSight = false;
		fpp.allowDiagonal = true;
		fpp.keepDistance = false;
		fpp.maxSearchDist = PATH_MAX_DIST;
		fpp.minTargetDist = 0;
		fpp.maxTargetDist = 2;
		std::vector<Direction> dirList;
		if (g_game().map.getPathMatching(player, leaderPos, dirList, FrozenPathingConditionCall(leaderPos), fpp)) {
			botStartAutoWalk(bot, player, dirList);
			s_ptrail.legs++;
			// The direct path supersedes any replay in progress.
			s_followerCursor.erase(bot.guid);
			trailRetireZHopSession(bot.guid, bot.currentPos.z); // ROUND2 A4
			return true;
		}
	}

	auto trailIt = s_leaderTrail.find(leaderGuid);
	if (trailIt == s_leaderTrail.end() || trailIt->second.nodes.empty()) {
		return giveUp(false); // no recorded route (fresh demand / just reloaded) — teleport
	}
	auto& trail = trailIt->second;

	// O(1) node lookup: seqs are contiguous because pruning only ever pops the front.
	auto nodeBySeq = [&](uint32_t seq) -> const TrailNode* {
		const uint32_t frontSeq = trail.nodes.front().seq;
		if (seq < frontSeq) {
			return nullptr;
		}
		const size_t idx = static_cast<size_t>(seq - frontSeq);
		return idx < trail.nodes.size() ? &trail.nodes[idx] : nullptr;
	};

	auto curIt = s_followerCursor.find(bot.guid);
	if (curIt != s_followerCursor.end() && curIt->second.leaderGuid != leaderGuid) {
		s_followerCursor.erase(curIt); // switched leaders — stale cursor
		s_followerZHopSession.erase(bot.guid);
		curIt = s_followerCursor.end();
	}

	// 2. Anchor the cursor if absent. Newest-first deliberately: if the leader looped back past
	// us, we skip the loop instead of re-walking it. Nodes older than maxAgeMs are never anchor
	// candidates (stale-world rule) — and once one is too old, all earlier ones are too.
	if (curIt == s_followerCursor.end()) {
		constexpr int32_t ANCHOR_RADIUS = 6;
		const TrailNode* anchor = nullptr;
		for (auto rit = trail.nodes.rbegin(); rit != trail.nodes.rend(); ++rit) {
			if (nowMs - rit->recordedAtMs > trailCfg_.maxAgeMs) {
				break;
			}
			if (rit->postPos.z == bot.currentPos.z && trailCheb(rit->postPos, bot.currentPos) <= ANCHOR_RADIUS) {
				anchor = &*rit;
				break;
			}
		}
		if (anchor == nullptr) {
			return giveUp(false); // nowhere near the recorded route — teleport
		}
		FollowerCursor fresh;
		fresh.leaderGuid = leaderGuid;
		fresh.seq = anchor->seq + 1; // we stand at the anchor — the NEXT node is the target
		fresh.startedMs = nowMs;
		fresh.lastProgressMs = nowMs;
		fresh.lastDist = INT32_MAX;
		curIt = s_followerCursor.emplace(bot.guid, fresh).first;
	}
	FollowerCursor& cursor = curIt->second;

	// 6. Watchdogs — checked before dispatch so a stuck leg can never run forever.
	//
	// ROUND2 A5: a cross-town leader JUMP (boat, carpet, teleport waypoint) lands the leader far
	// beyond maxLagTiles from every follower at once. Without this exemption the watchdog killed
	// every cursor on the same tick, dumping the whole party into the separation teleport
	// simultaneously — which is exactly the "they all teleport together" the user saw. When a JUMP
	// node still lies ahead on the trail, the distance is EXPECTED and is not evidence of being
	// hopelessly behind: the follower walks to the pre-jump tile and replicates the jump there, so
	// members leave one at a time in the order they reach the boat.
	{
		bool jumpAhead = false;
		for (const auto& n : trail.nodes) {
			if (n.seq >= cursor.seq && n.kind == TrailNodeKind::JUMP) {
				jumpAhead = true;
				break;
			}
		}
		if (!jumpAhead && trailCheb(bot.currentPos, leaderPos) > trailCfg_.maxLagTiles) {
			return giveUp(true); // hopelessly behind
		}
	}
	if (nowMs - cursor.lastProgressMs > trailCfg_.stuckMs) {
		return giveUp(true); // no progress
	}
	if (nodeBySeq(cursor.seq) == nullptr && cursor.seq < trail.nodes.front().seq) {
		return giveUp(true); // our stretch of the trail was pruned/cleared out from under us
	}

	// 3. Retire (belt-and-braces — the authoritative erase lives in the not-separated branch of
	// followPartyHuntLeader, which the outer separation gate reaches first on convergence).
	if (!diffZ && trailCheb(bot.currentPos, leaderPos) <= PARTY_HUNT_SUPPORT_FOLLOW_DIST) {
		s_followerCursor.erase(bot.guid);
		s_followerZHopSession.erase(bot.guid);
		return true; // co-located; normal cohesion resumes next tick
	}

	// 4. Advance: within the current run of STEP nodes, the newest one within 2 tiles on our
	// floor counts as reached. Never advances past a ZHOP/JUMP node.
	{
		uint32_t reachedSeq = 0; // seqs start at 1, so 0 is a safe sentinel
		for (uint32_t s = cursor.seq;; ++s) {
			const TrailNode* n = nodeBySeq(s);
			if (n == nullptr || n->kind != TrailNodeKind::STEP) {
				break;
			}
			if (n->postPos.z == bot.currentPos.z && trailCheb(n->postPos, bot.currentPos) <= 2) {
				reachedSeq = s;
			}
		}
		if (reachedSeq != 0) {
			cursor.seq = reachedSeq + 1;
			cursor.zhopStartAfterMs = 0; // ROUND2 A3: fresh stagger per hop
			cursor.lastProgressMs = nowMs;
			cursor.lastDist = INT32_MAX;
		}
	}

	// 5. Dispatch on node kind.
	const TrailNode* node = nodeBySeq(cursor.seq);
	if (node == nullptr) {
		// Caught up with everything recorded but still separated: the leader is mid-move and
		// new nodes arrive at the next tick top. Bounded by the stuck watchdog.
		return true;
	}

	if (node->kind == TrailNodeKind::ZHOP) {
		switch (executeTrailZHop(bot, *node, leaderGuid)) {
			case TrailZHopResult::SUCCEEDED:
				s_ptrail.zhopOk++;
				cursor.seq++;
				cursor.zhopStartAfterMs = 0; // ROUND2 A3: fresh stagger per hop
				cursor.lastProgressMs = nowMs;
				cursor.lastDist = INT32_MAX;
				return true;
			case TrailZHopResult::GIVE_UP:
				s_ptrail.zhopFail++;
				return giveUp(true);
			case TrailZHopResult::DECLINED_FAR:
				// >6 tiles from P — close the gap like an ordinary leg toward the hop's base.
				if (player->listWalkDir.empty()) {
					if (goTo(bot, node->prePos, 1)) {
						s_ptrail.legs++;
					}
				}
				break;
			case TrailZHopResult::IN_PROGRESS:
			case TrailZHopResult::APPROACHING:
				break; // the FC machine / approach walk owns the next ticks
		}
		// Shared progress bookkeeping below covers the approach: P and L are <=2 tiles apart
		// in x/y, so distance-to-postPos shrinks as the bot closes on the hop base.
	} else if (node->kind == TrailNodeKind::JUMP) {
		if (node->prePos.z != bot.currentPos.z) {
			return giveUp(true); // can't stage the jump from another floor
		}
		const int32_t dPre = trailCheb(bot.currentPos, node->prePos);
		if (dPre > 1) {
			if (player->listWalkDir.empty()) {
				if (goTo(bot, node->prePos, 1)) {
					s_ptrail.legs++;
				}
			}
		} else if (cursor.jumpWaitUntilMs == 0) {
			// On the pre-jump tile: give the map 2s to teleport us (real teleporter tiles do);
			// if nothing happens, replicate the jump ourselves. Preserves today's behavior for
			// boats / AdvStone / engine teleports.
			cursor.jumpWaitUntilMs = nowMs + 2000;
		} else if (bot.currentPos.z == node->postPos.z && trailCheb(bot.currentPos, node->postPos) <= 2) {
			cursor.seq++; // the map moved us — jump complete
			cursor.zhopStartAfterMs = 0; // ROUND2 A3: fresh stagger per hop
			cursor.jumpWaitUntilMs = 0;
			cursor.lastProgressMs = nowMs;
			cursor.lastDist = INT32_MAX;
		} else if (nowMs >= cursor.jumpWaitUntilMs) {
			Position placeAt = chooseSafePartyFollowPos(bot, node->postPos, s_partyFollowReservedThisTick);
			s_ptrail.jumpTele++;
			BOT_TELEPORT(player, placeAt, true);
			bot.currentPos = placeAt;
			bot.lastPos = placeAt;
			if (!player->listWalkDir.empty()) {
				player->listWalkDir.clear();
				player->stopEventWalk();
			}
			player->setFollowCreature(nullptr);
			cursor.jumpWaitUntilMs = 0;
			cursor.seq++;
			cursor.zhopStartAfterMs = 0; // ROUND2 A3: fresh stagger per hop
			cursor.lastProgressMs = nowMs;
			cursor.lastDist = INT32_MAX;
			castLog(bot, fmt::format("TRAIL: replayed JUMP to ({},{},{})", placeAt.x, placeAt.y, placeAt.z));
		}
	} else {
		// STEP: A* to the farthest node within horizon tiles that does not cross a ZHOP/JUMP.
		// One walk in flight at a time — no per-tick A*. Never chunks (horizon < CHUNK_DIST).
		if (player->listWalkDir.empty()) {
			const TrailNode* legTarget = node;
			for (uint32_t s = cursor.seq;; ++s) {
				const TrailNode* n = nodeBySeq(s);
				if (n == nullptr || n->kind != TrailNodeKind::STEP) {
					break;
				}
				if (n->postPos.z != bot.currentPos.z || trailCheb(n->postPos, bot.currentPos) > trailCfg_.horizon) {
					break;
				}
				legTarget = n;
			}
			if (goTo(bot, legTarget->postPos, 1)) {
				s_ptrail.legs++;
			}
		}
	}

	// Progress bookkeeping: distance-to-current-node shrinking counts as progress.
	if (const TrailNode* n2 = nodeBySeq(cursor.seq)) {
		const int32_t dNode = trailCheb(bot.currentPos, n2->postPos);
		if (dNode < cursor.lastDist) {
			cursor.lastDist = dNode;
			cursor.lastProgressMs = nowMs;
		}
	}
	return true;
}

// --- Follow Party Hunt Leader with Smart Positioning ---
void BotEngine::followPartyHuntLeader(BotState& bot, const std::shared_ptr<Player>& leader, BotState* leaderBot) {
	auto player = bot.getPlayer();
	if (!player || !leader) return;

	// P8 walk-fight guard (consume-once): if the role fn queued a walk this tick (AoE reposition),
	// skip the cohesion-walk logic below so we don't stomp it. The teleport/stair-follow safety nets
	// still run first (they MUST always run). Erase = consume so there's never cross-tick staleness.
	const bool roleWalked = (s_roleWalkedThisTick.erase(bot.guid) > 0);

	auto leaderPos = leader->getPosition();
	int32_t leaderDist = std::max(
		std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(leaderPos.x)),
		std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(leaderPos.y)));

	int32_t keepDist = getPartyHuntKeepDistance(bot, leaderBot);

	// TRAIL: keep the tick-top recorder sampling this leader while any follower runs (10s TTL).
	// Registered on EVERY run — not just while separated — so the breadcrumb trail already
	// exists by the time a separation happens.
	if (trailCfg_.enable) {
		auto& want = s_trailWanted[leader->getGUID()];
		want.botGuid = bot.guid;
		want.creatureId = leader->getID();
		want.expiresMs = OTSYS_TIME() + TRAIL_WANT_TTL_MS;
	}

	// R7 diagnostic (party-hunt ladder block): party-hunt z-descents go through the waypoint
	// system, NOT the FC machine (commit 6e48c71b7), so the Cormaya symptom surfaces here as a
	// SUPPORT loitering on a floorchange/teleport tile the EK must step onto to descend. With
	// the FC-safe placement (chooseSafePartyFollowPos / chooseWakePosition) a support should
	// never be on such a tile; if this still fires once deployed, it pinpoints a residual
	// placement gap and whether same-party walkthrough is actually in effect. One-shot per
	// guid (re-armed when the bot leaves the tile) → rare, low-volume.
	{
		auto myTile = g_game().map.getTile(bot.currentPos);
		if (myTile && myTile->hasFlag(TILESTATE_FLOORCHANGE | TILESTATE_TELEPORT)) {
			if (s_partyFollowOnFcLogged.insert(bot.guid).second) {
				g_logger().warn("[BotEngine] PARTY_HUNT R7: support '{}' on FLOORCHANGE/TELEPORT "
					"tile ({},{},{}) — may block EK descent; leader='{}' at ({},{},{}) dist={} sameParty={}",
					player->getName(), bot.currentPos.x, bot.currentPos.y, bot.currentPos.z,
					leader->getName(), leaderPos.x, leaderPos.y, leaderPos.z, leaderDist,
					(player->getParty() && leader->getParty() == player->getParty()) ? "yes" : "no");
			}
		} else {
			s_partyFollowOnFcLogged.erase(bot.guid);
		}
	}

	// === P2: rapid-stairs-robust separation follow (unified timer) ===
	// The old per-follower 2s z-delay was RESET whenever the leader's z momentarily matched ours
	// (a down-then-up stair-hop) → the teleport never fired and the follower was stranded. We now
	// track WHEN separation began and clear it only on TRUE co-location, so a transient z-bounce no
	// longer zeroes progress. After PARTY_FOLLOW_Z_SETTLE_MS of separation we teleport to the
	// leader's CURRENT authoritative position (immune to tick order), gated by a per-follower
	// cooldown to avoid the teleport-spam that previously froze cast clients.
	const int64_t nowMs = OTSYS_TIME();
	const bool diffZ = (bot.currentPos.z != leaderPos.z);
	// Same-floor "stuck far" recovery: > viewport-X means the support is off-screen and (usually)
	// blocked by geometry on the same floor. Generous threshold so normal follow gaps don't trip it.
	const bool sameZFar = !diffZ && leaderDist > (PARTY_HUNT_SUPPORT_FOLLOW_DIST + 5);
	const bool separated = diffZ || sameZFar;

	// Per-follower leader-z-change detection (drives active-hop window + cooldown-bypass).
	auto lastLZIt = s_followerLastLeaderZ.find(bot.guid);
	const uint8_t lastKnownLeaderZ = (lastLZIt != s_followerLastLeaderZ.end()) ? lastLZIt->second : leaderPos.z;
	const bool leaderChangedZ = (leaderPos.z != lastKnownLeaderZ);
	s_followerLastLeaderZ[bot.guid] = leaderPos.z;
	if (leaderChangedZ) s_followerLeaderZStamp[bot.guid] = nowMs;
	auto lzsIt = s_followerLeaderZStamp.find(bot.guid);
	const bool leaderHoppingRecently = (lzsIt != s_followerLeaderZStamp.end())
		&& (nowMs - lzsIt->second < PARTY_FOLLOW_Z_HOP_WINDOW_MS);

	if (separated) {
		if (s_followerSeparatedSince.find(bot.guid) == s_followerSeparatedSince.end()) {
			s_followerSeparatedSince[bot.guid] = nowMs;
		}
		const int64_t sepElapsed = nowMs - s_followerSeparatedSince[bot.guid];

		// Teleport gate: separation persisted >= settle AND cooldown allows it. A GENUINE new leader
		// floor-change bypasses the 3s cooldown (user's same-tick "teleported then leader went back up
		// the stairs" edge) — but never faster than the 500ms hard-min.
		auto cdIt = s_partyFollowTeleportCooldown.find(bot.guid);
		const int64_t cdUntil = (cdIt != s_partyFollowTeleportCooldown.end()) ? cdIt->second : 0;
		auto lastTeleZIt = s_followerLastTeleLeaderZ.find(bot.guid);
		const bool newFloorSinceTele = (lastTeleZIt == s_followerLastTeleLeaderZ.end())
			|| (leaderPos.z != lastTeleZIt->second);
		bool cooldownOk = (nowMs >= cdUntil) || (newFloorSinceTele && leaderChangedZ);
		if (cooldownOk && cdIt != s_partyFollowTeleportCooldown.end()) {
			const int64_t lastTele = cdUntil - PARTY_FOLLOW_TELE_COOLDOWN_MS; // hard-min gap even on bypass
			if (nowMs - lastTele < PARTY_FOLLOW_TELE_MIN_GAP_MS) cooldownOk = false;
		}

		// TRAIL: try WALKING the leader's recorded route first. true = moving this tick — the
		// teleport below (and the diffZ hold) are skipped. The teleport's own combined
		// settle+cooldown condition below stays byte-for-byte untouched as the fallback.
		//
		// The 1.5s settle gate is BYPASSED for a human-led member, and only for this walk attempt.
		// User-reported: "the 1st bot goes through the FC tile first, then the rest take a second
		// or so to follow and/or sometimes teleport instead." That is this gate. The settle exists
		// to give a BOT leader its lure time before supports pile onto a stairwell
		// (dfe5db322, the 1.5s is the user's own number) — a rationale that simply does not apply
		// when a human leads: the human is already gone down the stairs and wants the party to
		// follow now. Confirmed against live counters: walking works (zhopOk) but a teleport won
		// roughly a third to half of the crossings (partyTele), because nothing could even attempt
		// the walk until the same instant the teleport became eligible.
		//
		// The TELEPORT keeps the full settle in both modes, so this can only ever convert a
		// teleport into a walk, never the reverse, and autonomous party hunts are byte-identical.
		const bool humanLedMember = s_partyLeaderId.count(bot.guid) > 0
			&& s_botToPartyHunt.count(bot.guid) == 0;
		if (humanLedMember || sepElapsed >= PARTY_FOLLOW_Z_SETTLE_MS) {
			if (tryFollowLeaderTrail(bot, leader)) {
				return;
			}
		}

		if (sepElapsed >= PARTY_FOLLOW_Z_SETTLE_MS && cooldownOk) {
			const bool wasBypass = (nowMs < cdUntil);
			Position placeAt = chooseSafePartyFollowPos(bot, leaderPos, s_partyFollowReservedThisTick);
			BOT_TELEPORT(player, placeAt, true);
			bot.currentPos = placeAt;
			bot.lastPos = placeAt;
			s_lastZChangeTime[bot.guid] = nowMs; // reuse existing z-change grace (AoE suppression, etc.)
			if (!player->listWalkDir.empty()) { player->listWalkDir.clear(); player->stopEventWalk(); }
			player->setFollowCreature(nullptr);
			s_partyFollowTeleportCooldown[bot.guid] = nowMs + PARTY_FOLLOW_TELE_COOLDOWN_MS;
			s_followerLastTeleLeaderZ[bot.guid] = leaderPos.z;
			// If the leader already moved to another floor, keep the timer running (new episode).
			const int32_t newDist = std::max(
				std::abs(static_cast<int32_t>(placeAt.x) - static_cast<int32_t>(leaderPos.x)),
				std::abs(static_cast<int32_t>(placeAt.y) - static_cast<int32_t>(leaderPos.y)));
			if (placeAt.z == leaderPos.z && newDist <= PARTY_HUNT_SUPPORT_FOLLOW_DIST) {
				s_followerSeparatedSince.erase(bot.guid);
			}
			s_ptrail.partyTele++; // [PTRAIL] baseline: separation teleport
			castLog(bot, fmt::format("PARTY_HUNT: Teleported to leader ({}{}) at ({},{},{}) sepAge={}ms",
				diffZ ? "z-change" : "stuck-far", wasBypass ? ",cd-bypass" : "",
				placeAt.x, placeAt.y, placeAt.z, sepElapsed));
			return;
		}

		// Separated but settle not reached: for a z-difference HOLD (don't walk to a wrong-floor,
		// path-less target). For same-z stuck-far, fall through to the leashed walk/approach below.
		if (diffZ) {
			if (!player->listWalkDir.empty()) { player->listWalkDir.clear(); player->stopEventWalk(); }
			player->setFollowCreature(nullptr);
			return;
		}
	} else {
		// Not separated this tick. Clear the timer on TRUE co-location, OR once the leader has
		// stopped hopping (so a stale timer can't trigger an instant teleport later). While the
		// leader is actively hopping, a transient same-z pass-through does NOT clear it.
		if (leaderDist <= PARTY_HUNT_SUPPORT_FOLLOW_DIST || !leaderHoppingRecently) {
			s_followerSeparatedSince.erase(bot.guid);
			// TRAIL (review finding #6): the authoritative cursor retire. The outer separation
			// gate stops calling tryFollowLeaderTrail before its own <=FOLLOW_DIST retire test
			// can run, so on the normal convergence path the cursor would dangle without this.
			if (s_followerCursor.erase(bot.guid) > 0) {
				trailRetireZHopSession(bot.guid, bot.currentPos.z); // ROUND2 A4: count the success
			}
		}
		s_followerZChangeDetected.erase(bot.guid); // legacy map retired on this path
	}

	// Hard safety net: way too far on the SAME floor (e.g. AoE-teleported into a pocket) — snap back
	// immediately, independent of the settle timer. Rare; the leashed walk (below) is the norm.
	if (leaderDist > PARTY_HUNT_SUPPORT_TELEPORT_DIST && !diffZ) {
		// TRAIL: walking beats the hard-net snap too — no grace here, matching the branch.
		if (tryFollowLeaderTrail(bot, leader)) {
			return;
		}
		Position placeAt = chooseSafePartyFollowPos(bot, leaderPos, s_partyFollowReservedThisTick);
		BOT_TELEPORT(player, placeAt, true);
		bot.currentPos = placeAt;
		bot.lastPos = placeAt;
		if (!player->listWalkDir.empty()) {
			player->listWalkDir.clear();
			player->stopEventWalk();
		}
		player->setFollowCreature(nullptr);
		s_partyFollowTeleportCooldown[bot.guid] = nowMs + PARTY_FOLLOW_TELE_COOLDOWN_MS;
		s_followerLastTeleLeaderZ[bot.guid] = leaderPos.z;
		s_followerSeparatedSince.erase(bot.guid);
		s_ptrail.partyTele++; // [PTRAIL] baseline: same-floor hard-net teleport
		castLog(bot, fmt::format("PARTY_HUNT: Teleported to leader (dist={}) at ({},{},{})",
			leaderDist, placeAt.x, placeAt.y, placeAt.z));
		return;
	}

	// P8: role fn already queued a walk this tick (AoE reposition) — the teleport/stair-follow safety
	// nets above have run; skip all cohesion WALK logic (YIELD/retreat/approach/spread/follow) so we
	// don't override the role fn's move. Prevents the role-fn-vs-cohesion walk fight (oscillation).
	if (roleWalked) return;

	// === YIELD: If EK is close and we're in its path, step aside ===
	if (leaderDist <= 2 && !leader->listWalkDir.empty() && player->listWalkDir.empty()) {
		Direction ekDir = leader->listWalkDir.front();
		Position ekNextPos = getNextPosition(ekDir, leaderPos);
		if (ekNextPos == bot.currentPos) {
			// We're directly in the EK's path — step perpendicular
			// Try both perpendicular directions
			Direction perpDirs[2];
			if (ekDir == DIRECTION_NORTH || ekDir == DIRECTION_SOUTH) {
				perpDirs[0] = DIRECTION_EAST;
				perpDirs[1] = DIRECTION_WEST;
			} else if (ekDir == DIRECTION_EAST || ekDir == DIRECTION_WEST) {
				perpDirs[0] = DIRECTION_NORTH;
				perpDirs[1] = DIRECTION_SOUTH;
			} else {
				// Diagonal — pick two orthogonal options
				perpDirs[0] = (ekDir == DIRECTION_NORTHEAST || ekDir == DIRECTION_SOUTHEAST) ? DIRECTION_WEST : DIRECTION_EAST;
				perpDirs[1] = (ekDir == DIRECTION_NORTHEAST || ekDir == DIRECTION_NORTHWEST) ? DIRECTION_SOUTH : DIRECTION_NORTH;
			}

			for (auto yieldDir : perpDirs) {
				Position yieldPos = getNextPosition(yieldDir, bot.currentPos);
				auto yieldTile = g_game().map.getTile(yieldPos);
				if (yieldTile && !yieldTile->hasFlag(TILESTATE_BLOCKPATH) && yieldTile->getCreatureCount() == 0) {
					botStartAutoWalk(bot, player,{yieldDir});
					castLog(bot, "PARTY_HUNT: Yielding to EK path");
					return;
				}
			}
		}
	}

	// If EK has a target (combat), use chaseTarget-like positioning with keepDistance
	auto leaderTarget = leader->getAttackedCreature();
	if (leaderTarget && leaderTarget->getHealth() > 0 && leaderTarget->getPosition().z == bot.currentPos.z) {
		// === P8 inc3 (A3): consolidation on aggro ===
		// If a monster is targeting US and the EK has an open immediately-adjacent tile, run AROUND the
		// EK into it so the EK's exeta res can pull the monster off us. Best-effort (exeta res is 1-tile,
		// so this only reliably helps when the chasing monster ends up adjacent to the EK). Cooldown via
		// s_retreatUntil prevents ping-pong; the P3 kite below is the fallback when no open adjacent tile.
		{
			auto a3cd = s_retreatUntil.find(bot.guid);
			if ((a3cd == s_retreatUntil.end() || OTSYS_TIME() >= a3cd->second) && player->listWalkDir.empty()) {
				bool hasAggro = false;
				auto aggroSpecs = Spectators().find<Monster>(bot.currentPos, false,
					MONSTER_SCAN_RADIUS_X, MONSTER_SCAN_RADIUS_X, MONSTER_SCAN_RADIUS_Y, MONSTER_SCAN_RADIUS_Y);
				for (const auto& s : aggroSpecs) {
					auto m = s->getMonster();
					if (!m || s->isRemoved() || s->getHealth() <= 0) continue;
					auto mt = m->getAttackedCreature();
					if (mt && mt->getID() == player->getID()) { hasAggro = true; break; }
				}
				if (hasAggro) {
					const int32_t myEkDist = std::max(
						std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(leaderPos.x)),
						std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(leaderPos.y)));
					if (myEkDist <= 1) {
						// Already next to the EK — hold so exeta res can grab the monster (don't ping-pong).
						s_retreatUntil[bot.guid] = OTSYS_TIME() + 1500LL;
						return;
					}
					static const int32_t adx[] = { 0, 0, -1, 1, -1, 1, -1, 1 };
					static const int32_t ady[] = { -1, 1, 0, 0, -1, -1, 1, 1 };
					Position bestAdj;
					bool foundAdj = false;
					int32_t bestAdjDist = 999;
					for (int i = 0; i < 8; i++) {
						Position ap(static_cast<uint16_t>(leaderPos.x + adx[i]),
							static_cast<uint16_t>(leaderPos.y + ady[i]), leaderPos.z);
						auto tile = g_game().map.getTile(ap);
						if (!tile) continue;
						if (tile->hasFlag(TILESTATE_BLOCKPATH | TILESTATE_FLOORCHANGE | TILESTATE_TELEPORT)) continue;
						if (auto cs = tile->getCreatures(); cs && !cs->empty()) continue;
						int32_t d = std::max(
							std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(ap.x)),
							std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(ap.y)));
						if (d < bestAdjDist) { bestAdjDist = d; bestAdj = ap; foundAdj = true; }
					}
					if (foundAdj) {
						FindPathParams fpp;
						fpp.fullPathSearch = true; fpp.clearSight = true; fpp.allowDiagonal = true;
						fpp.keepDistance = false; fpp.maxSearchDist = PATH_MAX_DIST;
						fpp.minTargetDist = 0; fpp.maxTargetDist = 0;
						std::vector<Direction> dirList;
						if (g_game().map.getPathMatching(player, bestAdj, dirList,
								FrozenPathingConditionCall(bestAdj), fpp) && !dirList.empty()) {
							botStartAutoWalk(bot, player, dirList);
							s_retreatUntil[bot.guid] = OTSYS_TIME() + static_cast<int64_t>(dirList.size()) * 300LL + 600LL;
							castLog(bot, "PARTY_HUNT: [A3] consolidating next to EK (aggro dump)");
							return;
						}
					}
					// No open adjacent tile — fall through to the formation-hold/approach below.
				}
			}
		}
		// === P8 inc2-fix: NO kiting — HOLD the formation slot ===
		// Bots are tanky (infinite HP) and the ED heals, so keepDist is a spell-range PREFERENCE, not a
		// survival need. The old maximize-threat-distance kite (a) fled to the viewport-leash edge (d=7)
		// when the pack was on one side, and (b) bypassed the formation-slot claim system, so BOTH
		// supports converged on the same "farthest from monsters" tile ("same rotations") — confirmed on
		// the EK-centered ASCII grid. We now hold the distinct cardinal formation slot; the APPROACH block
		// below walks each support back to ITS slot (slotDist>1 trigger). Honor an active A3 / hold cooldown.
		if (keepDist > 0) {
			auto retreatIt = s_retreatUntil.find(bot.guid);
			if (retreatIt != s_retreatUntil.end() && OTSYS_TIME() < retreatIt->second) {
				return; // holding (A3 adjacent-hold or a recent reposition) — still casting via the role fn
			}
			s_retreatUntil.erase(bot.guid);
		}
		// === APPROACH: Walk toward EK leader (not target) to stay behind front line ===
		// Support bots converge on the EK's position. keepDistance from EK ensures they
		// stay at attack range of monsters near the EK without running ahead and luring.
		int32_t attackRange = static_cast<int32_t>(getAttackRange(getBaseVocation(bot.vocationId)));
		int32_t safeMax = std::max(keepDist > 0 ? keepDist + 1 : 1, attackRange);
		bool hasLOStoLeader = g_game().map.isSightClear(bot.currentPos, leaderPos, true);

		// P8 inc2-fix: the support's sticky cardinal formation slot (distinct ED/MS via claims).
		// Computed once and reused both as the off-slot trigger and the walk destination. ONLY for
		// ranged supports (keepDist>0); RP melee (keepDist=0) stays adjacent to the EK (original path).
		Position slot = leaderPos;
		int32_t slotDist = 0;
		if (keepDist > 0) {
			slot = desiredFormationSlot(bot.guid, bot.currentPos, leaderPos, keepDist, false, bot.partyHuntId);
			slotDist = std::max(
				std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(slot.x)),
				std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(slot.y)));
		}

		// Also check if we can see at least one monster near the EK
		// (if we can see the EK but not any monsters, we're around a corner / outside a door)
		bool hasLOStoAnyMonster = false;
		if (hasLOStoLeader && leaderDist <= safeMax) {
			auto monsterSpecs = Spectators().find<Monster>(leaderPos, false,
				MONSTER_SCAN_RADIUS_X, MONSTER_SCAN_RADIUS_X, MONSTER_SCAN_RADIUS_Y, MONSTER_SCAN_RADIUS_Y);
			for (const auto& spec : monsterSpecs) {
				if (spec->isRemoved() || spec->getHealth() <= 0) continue;
				auto mpos = spec->getPosition();
				if (mpos.z != bot.currentPos.z) continue;
				int32_t monDist = std::max(
					std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(mpos.x)),
					std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(mpos.y)));
				if (monDist <= attackRange && g_game().map.isSightClear(bot.currentPos, mpos, true)) {
					hasLOStoAnyMonster = true;
					break;
				}
			}
		}

		// Need to approach if: too far from EK, no LOS to EK, can't see any monsters, OR we've been
		// knocked >1 tile off our formation slot (P8 inc2-fix — walk back to the slot, even in range).
		bool needsApproach = leaderDist > safeMax || !hasLOStoLeader
			|| (hasLOStoLeader && leaderDist <= safeMax && !hasLOStoAnyMonster)
			|| slotDist > 1;

		if (needsApproach) {
			// Already walking — let current path complete unless very far
			if (!player->listWalkDir.empty()) {
				if (leaderDist > safeMax + 5) {
					player->listWalkDir.clear();
					player->stopEventWalk();
				} else {
					return; // let current walk finish
				}
			}

			// Cooldown: don't re-pathfind every tick (expensive A*)
			auto cdIt = s_approachCooldown.find(bot.guid);
			if (cdIt != s_approachCooldown.end() && OTSYS_TIME() < cdIt->second) {
				return; // wait for cooldown
			}

			// Pathfind destination: EK position normally, but EK's TARGET if we're
			// in range of EK but can't see any monsters (around corner / outside door)
			Position approachDest = leaderPos;
			bool approachingTarget = false;
			if (hasLOStoLeader && leaderDist <= safeMax && !hasLOStoAnyMonster) {
				// We can see the EK but not the monsters — normally move toward the target for LOS.
				// P3 clamp: only chase the target if it stays on the leader's screen. If the monster
				// is off the leader's viewport (around a corner / down a corridor), the EK will come
				// to it — DON'T drift the support off-screen chasing it (the confirmed "running around
				// the spawn / Approaching target dist=8" symptom). Converge on the EK instead.
				auto tpos = leaderTarget->getPosition();
				if (withinViewportLeash(tpos, leaderPos)) {
					approachDest = tpos;
					approachingTarget = true;
				}
			}
			// P8 inc2: when NOT chasing the target for LOS, converge on the sticky cardinal formation
			// slot (computed above) instead of "anywhere within keepDist" — holds the formation.
			// keepDist==0 (RP melee) keeps approachDest=leaderPos (adjacent).
			if (!approachingTarget && keepDist > 0) {
				approachDest = slot;
			}

			FindPathParams fpp;
			fpp.fullPathSearch = true;
			fpp.clearSight = true;
			fpp.allowDiagonal = true;
			fpp.maxSearchDist = PATH_MAX_DIST;
			if (approachingTarget) {
				// Approaching target for LOS — use keepDistance from the monster
				fpp.keepDistance = true;
				fpp.minTargetDist = std::max(keepDist, 2);
				fpp.maxTargetDist = attackRange;
			} else {
				// Walk directly onto the formation slot (it is already at ~keepDist, cardinal-aligned).
				fpp.keepDistance = false;
				fpp.minTargetDist = 0;
				fpp.maxTargetDist = 1;
			}

			std::vector<Direction> dirList;
			bool pathFound = g_game().map.getPathMatching(player, approachDest, dirList,
					FrozenPathingConditionCall(approachDest), fpp);
			if (!pathFound) {
				// Fallback without clearSight
				fpp.clearSight = false;
				dirList.clear();
				pathFound = g_game().map.getPathMatching(player, approachDest, dirList,
						FrozenPathingConditionCall(approachDest), fpp);
			}

			if (pathFound && !dirList.empty()) {
				botStartAutoWalk(bot, player,dirList);
				// Cooldown = walk time + 1 tick buffer (don't re-pathfind while walking)
				int64_t walkTime = static_cast<int64_t>(dirList.size()) * 300LL + 300LL;
				s_approachCooldown[bot.guid] = OTSYS_TIME() + walkTime;
				castLog(bot, fmt::format("PARTY_HUNT: Approaching {} ({},{},{}) dist={} steps={}",
					approachingTarget ? "target for LOS" : "EK",
					approachDest.x, approachDest.y, approachDest.z,
					approachingTarget ? static_cast<int32_t>(std::max(
						std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(approachDest.x)),
						std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(approachDest.y)))) : leaderDist,
					dirList.size()));
			} else {
				// No path found — cooldown 2 seconds before retrying
				s_approachCooldown[bot.guid] = OTSYS_TIME() + 2000LL;
			}
			return;
		}

		// In range of EK + has LOS to EK — clear approach cooldown
		s_approachCooldown.erase(bot.guid);

		// Spread if stacked on another party member
		if (tryPartyMemberSpread(bot, leaderPos, keepDist)) return;

		// AoE reposition handles optimal tile from here
		return;
	}

	// === P8 inc2: No combat — HOLD the sticky cardinal formation slot ===
	// Stand aligned with the EK (same x OR y) at ~keepDist; only walk when off-slot by >1; otherwise
	// hold (this is what stops the "running around when nothing is chasing" wander). ED & MS take
	// distinct slots (claims). Replaces the old "follow to within keepDist any direction".
	{
		const Position slot = desiredFormationSlot(bot.guid, bot.currentPos, leaderPos, keepDist, false, bot.partyHuntId);
		const int32_t slotDist = std::max(
			std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(slot.x)),
			std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(slot.y)));

		if (slotDist <= 1) {
			// At our slot — hold position; only nudge apart if stacked on another member.
			tryPartyMemberSpread(bot, leaderPos, keepDist);
			return;
		}

		if (!player->listWalkDir.empty()) {
			// Already walking toward the slot — let it finish unless we're way off.
			if (slotDist > PARTY_LEASH_DIST) { player->listWalkDir.clear(); player->stopEventWalk(); }
			else return;
		}

		FindPathParams fpp;
		fpp.fullPathSearch = true;
		fpp.clearSight = true;
		fpp.allowDiagonal = true;
		fpp.keepDistance = false;
		fpp.maxSearchDist = PATH_MAX_DIST;
		fpp.minTargetDist = 0;
		fpp.maxTargetDist = 1; // walk onto/adjacent to the exact slot tile

		std::vector<Direction> dirList;
		if (g_game().map.getPathMatching(player, slot, dirList, FrozenPathingConditionCall(slot), fpp) && !dirList.empty()) {
			botStartAutoWalk(bot, player, dirList);
		} else {
			fpp.clearSight = false;
			dirList.clear();
			if (g_game().map.getPathMatching(player, slot, dirList, FrozenPathingConditionCall(slot), fpp) && !dirList.empty()) {
				botStartAutoWalk(bot, player, dirList);
			} else {
				tryOpenDoors(bot, player, leaderPos);
			}
		}
	}
}


// ============================================================================
// BOT_PARTY_LEAK_FIX — the Canary Party must not outlive the AWAKE party
// ============================================================================
//
// dissolveVirtualPartyHunt documents (correctly, for when it was written) that a virtual party
// owns no Canary Party and so needs no Canary teardown. materializeCanaryParty later broke that
// invariant by building a real Party retroactively on wake, so: virtual-form -> wake (materialize)
// -> re-hibernate / virtual-dissolve left the Party object attached to every member. Symptom:
// an IDLE or solo-hunting bot still reads "in a party with 4 members".
//
// The fix restores the invariant instead of patching each dissolve site: the Canary Party's
// lifetime now mirrors the party's AWAKE lifetime — materialize on wake, dematerialize on the
// leader-hibernate cascade (while the leader's Player is still reachable from the engine).
void BotEngine::dematerializeCanaryParty(uint32_t partyHuntId, const char* reason) {
	auto leaderIt = s_partyHuntLeaderGuid.find(partyHuntId);
	if (leaderIt == s_partyHuntLeaderGuid.end()) return;
	auto lIt = guidToIndex_.find(leaderIt->second);
	if (lIt == guidToIndex_.end()) return;

	auto leaderPlayer = resolveBotPlayer(leaderIt->second);
	if (!leaderPlayer) return;
	auto party = leaderPlayer->getParty();
	if (!party) return;

	// Only disband a party we actually lead. If some other player leads it, we are a member —
	// leave instead, so a human-led party is never torn down by a bot's hibernation.
	if (party->getLeader() == leaderPlayer) {
		party->disband();
	} else {
		party->leaveParty(leaderPlayer, true);
	}
	s_partyLeak.dematerialized++;
	g_logger().info("[BotEngine] PARTY_LEAK: dematerialized Canary party for #{} ({})",
		partyHuntId, reason);
}

// A bot's Player is reachable either live (playerRef) or, while hibernated, through the
// hibernation pool's strong ref — hibernation clears only the ENGINE's handle, the Player object
// itself stays alive (bot_engine.cpp: "weak_ptr stays valid via pool's strong ref"). Every leak
// path below needs the pooled form, because the bots holding stale parties are usually asleep.
std::shared_ptr<Player> BotEngine::resolveBotPlayer(uint32_t guid) {
	auto it = guidToIndex_.find(guid);
	if (it != guidToIndex_.end()) {
		if (auto p = bots_[it->second].getPlayer()) {
			return p;
		}
	}
	auto pIt = hibernationPool_.find(guid);
	return (pIt != hibernationPool_.end()) ? pIt->second : nullptr;
}

// Reclaim a stale Canary party from ONE bot. Returns true if anything was released.
// Safe to call for any bot: it refuses to touch a party that contains a real player.
bool BotEngine::reclaimStaleCanaryParty(uint32_t guid, const char* site) {
	auto player = resolveBotPlayer(guid);
	if (!player) return false;
	auto party = player->getParty();
	if (!party) return false;

	// NEVER touch a party any real player is part of. A human-led party can transiently have a BOT
	// as its Canary leader — Party::leaveParty auto-passes leadership to memberList.front() when
	// the human leaves, and doPartyFollow only re-passes it on its next tick — so "the leader is a
	// bot" is not sufficient. Check leader + members + invitees for any non-bot.
	auto isReal = [](const std::shared_ptr<Player>& p) { return p && !p->isBotPlayer(); };
	if (isReal(party->getLeader())) return false;
	for (const auto& m : party->getMembers()) {
		if (isReal(m)) return false;
	}
	for (const auto& i : party->getInvitees()) {
		if (isReal(i)) return false;
	}

	if (party->getLeader() == player) {
		party->disband();
	} else if (party->getLeader()) {
		party->leaveParty(player, true);
	} else {
		// Leader Player already DESTROYED (weak_ptr expired): disband()/leaveParty() both
		// early-return, so no stock call can free this one. Clear our own membership directly —
		// public stock setters, called from engine code, no stock file modified. Self-cleaning:
		// once every member's m_party is null the Party refcount drops and the memberList
		// reference cycle frees itself.
		player->setParty(nullptr);
		g_game().updatePlayerShield(player);
		s_partyLeak.orphanCleared++;
	}
	s_partyLeak.reclaimed++;
	g_logger().info("[BotEngine] PARTY_LEAK: reclaimed stale party from '{}' at {}",
		player->getName(), site);
	return true;
}

// Belt-and-braces reconciliation. Prevention (dematerialize + the deactivate last-exit checks)
// stops NEW leaks; this clears the ones already in the world and any path not yet enumerated.
void BotEngine::sweepStaleCanaryParties() {
	const int64_t nowMs = OTSYS_TIME();
	if (nowMs - s_partyLeak.lastSweepMs < PARTY_LEAK_SWEEP_MS) return;
	s_partyLeak.lastSweepMs = nowMs;

	uint32_t cleared = 0;
	for (auto& bot : bots_) {
		if (!bot.active) continue;
		// Engine still owns a party for this bot -> not stale.
		if (bot.partyHuntId != 0 || s_botToPartyHunt.count(bot.guid) > 0) continue;
		// PARTY state is the authoritative "this bot is following someone" marker. s_partyLeaderId
		// is NOT: live formation sets it for party-hunt supports too, and the virtual dissolve
		// never erased it, so a stale entry there would hide a genuine leak forever.
		if (bot.state == BotAIState::PARTY) continue;
		// BOT_PARTY_INVITE_RENDEZVOUS: an assembling member is state-IDLE with no partyHuntId, so
		// it matches this predicate exactly. Human-led assemblies are safe by construction
		// (reclaimStaleCanaryParty refuses any party containing a real player), but the all-bot
		// `invitebot` test party has no such shield and would be reclaimed mid-test.
		if (s_rvMember.count(bot.guid) > 0) continue;
		if (auto ka = s_inviteDebugKeepAlive.find(bot.guid); ka != s_inviteDebugKeepAlive.end()) {
			if (nowMs < ka->second) continue;   // TTL still valid — leave the test party alone
			s_inviteDebugKeepAlive.erase(ka);   // expired, fall through and clean it up
		}
		if (reclaimStaleCanaryParty(bot.guid, "sweep")) cleared++;
	}
	if (cleared > 0) {
		g_logger().info("[BotEngine] [PARTY_LEAK] sweep reclaimed {} stale party membership(s) "
			"(total: dematerialized={} reclaimed={} orphanCleared={})",
			cleared, s_partyLeak.dematerialized, s_partyLeak.reclaimed, s_partyLeak.orphanCleared);
	}
}

// ============================================================================
// BOT_PARTY_CAP — cap how many BOTS may be party-bound at once
// ============================================================================
//
// Opening party hunts to all vocations drove production to 112 concurrent parties (~448 of ~500
// logged-in bots), each holding a hunt-script reservation for 2-3 hours and starving solo hunters.
//
// Counted in BOTS, not parties, because that is what the user asked for and what actually consumes
// the population. Denominator is countActiveBots() — hibernated bots keep active=true (only their
// engine playerRef is cleared), so it is exactly "logged in, hibernated or awake"; countTotalBots()
// would be the roster, including bots never activated. Numerator is s_botToPartyHunt (one entry per
// MEMBER, written by both formation paths, erased by every dissolve path).
//
// Gates FORMATION only — it never dissolves a running party, so after a deploy the count decays as
// existing hunts end rather than dropping immediately.
bool BotEngine::partyCapAllows(uint32_t prospectiveMembers, const char* path) {
	const int32_t pct = trailCfg_.maxPartyPct;
	if (pct <= 0 || pct >= 100) {
		return true; // 0 = uncapped (inert-at-zero convention); >=100 can never bind
	}
	const uint32_t loggedIn = countActiveBots();
	const uint32_t capBots = static_cast<uint32_t>(static_cast<uint64_t>(loggedIn) * pct / 100);
	const uint32_t partyBound = static_cast<uint32_t>(s_botToPartyHunt.size());
	if (partyBound + prospectiveMembers <= capBots) {
		return true;
	}

	// Rate-limited per path: at saturation every reroll would otherwise log.
	static std::unordered_map<std::string, int64_t> s_lastCapLog;
	const int64_t nowMs = OTSYS_TIME();
	auto& last = s_lastCapLog[path];
	if (nowMs - last >= 60000) {
		last = nowMs;
		g_logger().info("[BotEngine] [PARTYCAP] refused: partyBound={} +{} > cap={} "
			"(loggedIn={} botPartyMaxPct={}) path={}",
			partyBound, prospectiveMembers, capBots, loggedIn, pct, path);
	}
	s_partyCapRefusals++;
	return false;
}
