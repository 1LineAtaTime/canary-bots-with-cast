/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_chat.cpp — bot chat subsystem (BOT_NAV_REALISM Phase 11 module split).
//
// First module carved out of the monolithic bot_engine.cpp, chosen because it
// has ZERO file-scope static dependencies (verified by scanning every `static`
// symbol in the engine against this region) — its state lives entirely in
// BotState fields and BotEngine members, both of which are shared automatically
// via bot_engine_impl.hpp. That makes it a pure code move with no state to
// reclassify, i.e. the safest possible first split.
//
// Contents: corpus loading, template rendering, the emitter, hibernated-bot
// chat, and the Phase-F keyword/PM reply pipeline.
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

// The impl header carries the engine-wide file-scope helpers; this TU uses only a
// subset of them, so silence the unused-function noise that would otherwise appear.
#pragma GCC diagnostic ignored "-Wunused-function"

// ============================================================================
// BOT_LIVENESS_PACK Phase C.2 + D: chat loader + emitter
// ============================================================================

void BotEngine::loadBotChatPhrases() {
	using json = nlohmann::json;
	chatCatalog_.clear();
	tradeCatalog_.clear();

	const std::string path = "data/bot_chat/phrases.json";
	std::ifstream file(path);
	if (!file.is_open()) {
		g_logger().warn("[BotEngine] Bot chat phrases file not found at '{}' — chat disabled", path);
		return;
	}

	json doc;
	try {
		file >> doc;
	} catch (const json::parse_error& e) {
		g_logger().error("[BotEngine] JSON parse error in '{}': {}", path, e.what());
		return;
	}

	// ---- v2 _meta.config -> chatCfg_ (hot-reloadable tunables) ----
	chatCfg_ = ChatCfg {};  // reset to defaults first so missing keys fall back
	if (doc.contains("_meta") && doc["_meta"].is_object()
	    && doc["_meta"].contains("config") && doc["_meta"]["config"].is_object()) {
		const auto& c = doc["_meta"]["config"];
		auto num = [&](const char* key, int32_t fallback) -> int32_t {
			return (c.contains(key) && c[key].is_number()) ? c[key].get<int32_t>() : fallback;
		};
		if (c.contains("worldChatMode") && c["worldChatMode"].is_string()) {
			const std::string mode = c["worldChatMode"].get<std::string>();
			chatCfg_.worldChatMode = (mode == "off") ? 0 : (mode == "channel") ? 2 : 1;
		}
		chatCfg_.banterSharePct = std::clamp(num("banterSharePct", chatCfg_.banterSharePct), 0, 100);
		chatCfg_.dedupWindowAdvertisingMs = num("dedupWindowAdvertisingMs", chatCfg_.dedupWindowAdvertisingMs);
		chatCfg_.dedupWindowBanterMs = num("dedupWindowBanterMs", chatCfg_.dedupWindowBanterMs);
		chatCfg_.playerThrottleMinMs = num("playerThrottleMinMs", chatCfg_.playerThrottleMinMs);
		chatCfg_.playerThrottleMaxMs = num("playerThrottleMaxMs", chatCfg_.playerThrottleMaxMs);
		chatCfg_.replyChancePct = std::clamp(num("replyChancePct", chatCfg_.replyChancePct), 0, 100);
		chatCfg_.pmReplyChancePct = std::clamp(num("pmReplyChancePct", chatCfg_.pmReplyChancePct), 0, 100);
		chatCfg_.replyDelayMinMs = num("replyDelayMinMs", chatCfg_.replyDelayMinMs);
		chatCfg_.replyDelayMaxMs = num("replyDelayMaxMs", chatCfg_.replyDelayMaxMs);
		chatCfg_.replyCooldownPerBotMs = num("replyCooldownPerBotMs", chatCfg_.replyCooldownPerBotMs);

		// BOT_PVP_REALISM tunables (same _meta.config block, hot-reloadable).
		pvpCfg_ = PvpCfg {};  // reset so missing keys fall back to defaults
		auto boolKey = [&](const char* key, bool fallback) -> bool {
			return (c.contains(key) && c[key].is_boolean()) ? c[key].get<bool>() : fallback;
		};
		pvpCfg_.enableReposition  = boolKey("pvpEnableReposition", pvpCfg_.enableReposition);
		pvpCfg_.enableDance       = boolKey("pvpEnableDance", pvpCfg_.enableDance);
		pvpCfg_.enableHaste       = boolKey("pvpEnableHaste", pvpCfg_.enableHaste);
		pvpCfg_.enableMagicWall   = boolKey("pvpEnableMagicWall", pvpCfg_.enableMagicWall);
		pvpCfg_.enablePzAwareFlee = boolKey("pvpEnablePzAwareFlee", pvpCfg_.enablePzAwareFlee);
		pvpCfg_.enableAoeBias     = boolKey("pvpEnableAoeBias", pvpCfg_.enableAoeBias);
		pvpCfg_.mageKeepDist     = std::clamp(num("pvpMageKeepDist", pvpCfg_.mageKeepDist), 1, 7);
		pvpCfg_.paladinKeepDist  = std::clamp(num("pvpPaladinKeepDist", pvpCfg_.paladinKeepDist), 1, 7);
		pvpCfg_.danceChancePct   = std::clamp(num("pvpDanceChancePct", pvpCfg_.danceChancePct), 0, 100);
		pvpCfg_.danceCooldownMs  = num("pvpDanceCooldownMs", pvpCfg_.danceCooldownMs);
		pvpCfg_.hasteHpPct       = std::clamp(num("pvpHasteHpPct", pvpCfg_.hasteHpPct), 0, 100);
		pvpCfg_.hasteCooldownMs  = num("pvpHasteCooldownMs", pvpCfg_.hasteCooldownMs);
		pvpCfg_.fleeHpPct        = std::clamp(num("pvpFleeHpPct", pvpCfg_.fleeHpPct), 0, 100);
		pvpCfg_.wallChancePct    = std::clamp(num("pvpWallChancePct", pvpCfg_.wallChancePct), 0, 100);
		pvpCfg_.wallCooldownMs   = num("pvpWallCooldownMs", pvpCfg_.wallCooldownMs);
		pvpCfg_.aoeBiasPct       = std::clamp(num("pvpAoeBiasPct", pvpCfg_.aoeBiasPct), 0, 300);
	} else {
		pvpCfg_ = PvpCfg {};  // no _meta.config at all — use PvP defaults
	}

	size_t totalTemplates = 0;
	for (auto it = doc.begin(); it != doc.end(); ++it) {
		std::string key = it.key();
		if (key.empty() || key[0] == '_') continue;
		// Non-bucket sections handled separately below.
		if (key == "replies" || key == "lexicon" || key == "trade_items") continue;
		// v1 corpus compatibility: world_chat is banter's old name.
		if (key == "world_chat") key = "banter";
		const auto& bucketJson = it.value();
		if (!bucketJson.is_object()) continue;
		ChatBucket bucket;
		if (bucketJson.contains("chance") && bucketJson["chance"].is_number()) {
			int c = bucketJson["chance"].get<int>();
			bucket.chance = static_cast<uint8_t>(std::clamp(c, 0, 100));
		}
		if (bucketJson.contains("templates") && bucketJson["templates"].is_array()) {
			for (const auto& t : bucketJson["templates"]) {
				if (t.is_string()) bucket.templates.push_back(t.get<std::string>());
			}
		}
		// v2 advertising: intent-split arrays (sell/buy/neutral) flattened into
		// templates + parallel intents so the lane filter can subset cheaply.
		static const std::array<std::pair<const char*, uint8_t>, 3> kIntentKeys = {{
			{ "neutral", 0 }, { "sell", 1 }, { "buy", 2 },
		}};
		for (const auto& [intentKey, intentVal] : kIntentKeys) {
			if (!bucketJson.contains(intentKey) || !bucketJson[intentKey].is_array()) continue;
			for (const auto& t : bucketJson[intentKey]) {
				if (!t.is_string()) continue;
				if (bucket.intents.size() < bucket.templates.size()) {
					bucket.intents.resize(bucket.templates.size(), 0);  // plain templates = neutral
				}
				bucket.templates.push_back(t.get<std::string>());
				bucket.intents.push_back(intentVal);
			}
		}
		totalTemplates += bucket.templates.size();
		// Merge instead of emplace: a transitional corpus carrying BOTH
		// world_chat and banter keys must not silently drop whichever bucket
		// the JSON iterator visits second (emplace would no-op).
		auto [dstIt, inserted] = chatCatalog_.try_emplace(key);
		ChatBucket& dst = dstIt->second;
		if (inserted) {
			dst = std::move(bucket);
		} else {
			if (!dst.intents.empty() || !bucket.intents.empty()) {
				dst.intents.resize(dst.templates.size(), 0);
				bucket.intents.resize(bucket.templates.size(), 0);
				dst.intents.insert(dst.intents.end(), bucket.intents.begin(), bucket.intents.end());
			}
			dst.templates.insert(dst.templates.end(),
				std::make_move_iterator(bucket.templates.begin()),
				std::make_move_iterator(bucket.templates.end()));
			dst.chance = std::max(dst.chance, bucket.chance);
			g_logger().info("[BotEngine] Chat bucket '{}' merged from two corpus keys ({} templates total)",
				key, dst.templates.size());
		}
	}

	// ---- Phase F reply catalog ----
	replyCatalog_.clear();
	if (doc.contains("replies") && doc["replies"].is_object()) {
		for (auto it = doc["replies"].begin(); it != doc["replies"].end(); ++it) {
			if (!it.value().is_array()) continue;
			auto& lines = replyCatalog_[it.key()];
			for (const auto& t : it.value()) {
				if (t.is_string()) lines.push_back(t.get<std::string>());
			}
		}
	}

	// ---- lexicon for %creature/%spawn level gating ----
	lexCreatures_.clear();
	lexSpawns_.clear();
	if (doc.contains("lexicon") && doc["lexicon"].is_object()) {
		const auto& lex = doc["lexicon"];
		if (lex.contains("creatures") && lex["creatures"].is_array()) {
			for (const auto& c : lex["creatures"]) {
				if (!c.is_object() || !c.contains("name")) continue;
				lexCreatures_.push_back({
					c["name"].get<std::string>(),
					c.value("level_min", 0), c.value("level_max", 0) });
			}
		}
		if (lex.contains("spawns") && lex["spawns"].is_array()) {
			for (const auto& s : lex["spawns"]) {
				if (!s.is_object() || !s.contains("name")) continue;
				lexSpawns_.push_back({
					s["name"].get<std::string>(), s.value("town", std::string()),
					s.value("level_min", 0), s.value("level_max", 0) });
			}
		}
	}

	// ---- trade catalog: JSON first, legacy hardcoded table as fallback ----
	if (doc.contains("trade_items") && doc["trade_items"].is_array()) {
		for (const auto& itj : doc["trade_items"]) {
			if (!itj.is_object() || !itj.contains("name")) continue;
			TradeEntry e;
			e.name = itj["name"].get<std::string>();
			e.priceMinGp = itj.value("price_min", 0);
			e.priceMaxGp = itj.value("price_max", e.priceMinGp);
			const std::string intent = itj.value("intent", std::string("both"));
			e.intent = (intent == "sell") ? 1 : (intent == "buy") ? 2 : 0;
			if (itj.contains("aliases") && itj["aliases"].is_array()) {
				for (const auto& a : itj["aliases"]) {
					if (a.is_string()) e.aliases.push_back(a.get<std::string>());
				}
			}
			if (!e.name.empty() && e.priceMinGp > 0) tradeCatalog_.push_back(std::move(e));
		}
	}
	if (tradeCatalog_.empty()) {
		// Legacy v1 fallback (subset) — keeps %item/%price functional if a stale
		// phrases.json without trade_items is deployed.
		tradeCatalog_ = {
			{ "sd", 200, 280, 0, {} },
			{ "soft boots", 35000, 42000, 0, {} },
			{ "stone skin amulet", 240000, 290000, 0, { "ssa" } },
			{ "great mana potion", 175, 200, 0, {} },
			{ "royal helmet", 300000, 380000, 0, {} },
			{ "magic plate armor", 850000, 1000000, 0, { "mpa" } },
		};
	}

	g_logger().info("[BotEngine] Loaded chat catalog v2: {} categories, {} templates; trade {} items; "
		"lexicon {} creatures / {} spawns; replies {} groups; mode={} banterShare={}%",
		chatCatalog_.size(), totalTemplates, tradeCatalog_.size(),
		lexCreatures_.size(), lexSpawns_.size(), replyCatalog_.size(),
		chatCfg_.worldChatMode == 0 ? "off" : chatCfg_.worldChatMode == 2 ? "channel" : "local",
		chatCfg_.banterSharePct);
}

// Fix #11: hibernated-bot channel chat. Hibernated bots are off-world (removed via
// removeCreature(isLogout=false), held in hibernationPool_) but still subscribed to
// channels (removeUserFromAllChannels is NOT called by removeCreature for non-logout).
// They can still emit channel posts through their pooled Player ref. Skipped for
// Local Chat since they're not in the world to broadcast to.
void BotEngine::tickHibernatedChat(BotState& bot) {
	const auto& cfg = livenessCfg_;
	if (!cfg.hibernatedChatEnabled) return;
	// No real players online → no reader for channel posts. Skip entirely so
	// hibernated bots do ZERO chat work during night idle. (Cast viewers can't
	// watch hibernated bots — bots with viewers don't hibernate per the cast
	// guard in hibernateBot — so this is safe to gate on real-player count.)
	if (cfg.realPlayerCount == 0) return;
	const int64_t now = OTSYS_TIME();
	// Fast common case. In local/off mode the world-chat timer is intentionally
	// never advanced (see below) and must not defeat this early-exit.
	const bool worldChannelOn = chatCfg_.worldChatMode == 2;
	if ((!worldChannelOn || now < bot.nextWorldChatTime) && now < bot.nextAdvertisingTime) return;

	// Pool ref keeps Player alive across hibernation.
	auto poolIt = hibernationPool_.find(bot.guid);
	if (poolIt == hibernationPool_.end() || !poolIt->second) return;
	const auto& player = poolIt->second;

	// v2 Phase D: hibernated bots only post World Chat in legacy "channel" mode.
	// In "local" mode the banter corpus is a local-say behavior, and hibernated
	// bots are off-world with nobody around to hear them — they keep Advertising
	// (a server-wide trade channel staying active is realistic) and nothing else.
	if (chatCfg_.worldChatMode == 2 && now >= bot.nextWorldChatTime) {
		tryEmitChat(bot, player, "banter", /*channelId=*/3);
		const int32_t minMs = cfg.worldChatIntervalMinMs;
		const int32_t maxMs = cfg.worldChatIntervalMaxMs;
		bot.nextWorldChatTime = now + uniform_random(std::max(1, minMs), std::max(minMs, maxMs));
	}
	if (now >= bot.nextAdvertisingTime) {
		tryEmitChat(bot, player, "advertising", /*channelId=*/5);
		const int32_t minMs = cfg.advertisingIntervalMinMs;
		const int32_t maxMs = cfg.advertisingIntervalMaxMs;
		bot.nextAdvertisingTime = now + uniform_random(std::max(1, minMs), std::max(minMs, maxMs));
	}
}

uint32_t BotEngine::hashRenderedChat(const std::string& text) {
	std::string lower = text;
	std::transform(lower.begin(), lower.end(), lower.begin(),
		[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	return static_cast<uint32_t>(std::hash<std::string>{}(lower));
}

std::string BotEngine::renderChatTemplate(const BotState& bot, const std::shared_ptr<Player>& player,
                                          const std::string& tmpl, uint8_t botLane) {
	std::string text = tmpl;
	auto substitute = [&](const std::string& token, const std::string& value) {
		size_t p = 0;
		while ((p = text.find(token, p)) != std::string::npos) {
			text.replace(p, token.size(), value);
			p += value.size();
		}
	};
	auto toLower = [](std::string s) {
		std::transform(s.begin(), s.end(), s.begin(),
			[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return s;
	};

	substitute("%city", bot.townName);
	substitute("%town", bot.townName);
	// Vocation names — index by vocationId (0-12 in canary; baseVoc 1-4 + promotions).
	{
		static const std::array<std::string, 13> kVocNames = {
			"none", "sorc", "druid", "pala", "knight",
			"ms", "ed", "rp", "ek",
			"ms", "ed", "rp", "ek",
		};
		const std::string& voc = (bot.vocationId < kVocNames.size())
			? kVocNames[bot.vocationId] : kVocNames[4];
		substitute("%vocation", voc);
	}

	uint32_t level = bot.cachedLevel;
	if (level == 0 && player) level = player->getLevel();

	// %level — exact level reads naturally ("finally 47").
	if (text.find("%level") != std::string::npos) {
		if (level == 0) return {};
		substitute("%level", std::to_string(level));
	}

	// %creature / %spawn — prefer the bot's ACTUAL hunt so first-person claims
	// are true; otherwise pick a level-coherent lexicon entry; otherwise fail so
	// the caller rerolls the template. A level-40 bot bragging about Medusa
	// Tower is an instant bot tell — never emit level-incoherent content.
	const bool wantsCreature = text.find("%creature") != std::string::npos;
	const bool wantsSpawn = text.find("%spawn") != std::string::npos;
	if (wantsCreature || wantsSpawn) {
		// Bracket check: unknown bounds (0) pass; otherwise allow ~30% slack
		// below min / above max so brackets don't have to be exact.
		auto levelOk = [&](int32_t lvlMin, int32_t lvlMax) {
			if (level == 0) return true;  // unknown bot level — don't over-filter
			if (lvlMin > 0 && static_cast<int32_t>(level) < lvlMin - lvlMin / 3) return false;
			if (lvlMax > 0 && static_cast<int32_t>(level) > lvlMax + lvlMax / 3) return false;
			return true;
		};
		const HuntScript* script = nullptr;
		if (bot.huntScriptId != 0) {
			for (const auto& s : huntScripts_) {
				if (s.id == bot.huntScriptId) { script = &s; break; }
			}
		}
		if (wantsSpawn) {
			std::string spawnName;
			if (script) {
				spawnName = script->name;
			} else {
				std::vector<size_t> matches;
				for (size_t i = 0; i < lexSpawns_.size(); ++i) {
					if (levelOk(lexSpawns_[i].levelMin, lexSpawns_[i].levelMax)) matches.push_back(i);
				}
				if (matches.empty()) return {};
				spawnName = lexSpawns_[matches[uniform_random(0, static_cast<int32_t>(matches.size()) - 1)]].name;
			}
			if (spawnName.empty()) return {};  // blank hunt-script name — reroll, never emit a hole
			substitute("%spawn", toLower(spawnName));
		}
		if (wantsCreature) {
			std::string creatureName;
			if (script && !script->targetNames.empty()) {
				creatureName = script->targetNames[uniform_random(
					0, static_cast<int32_t>(script->targetNames.size()) - 1)];
				// bot_hunt_targets rows can hold comma-separated lists — pick one part.
				if (creatureName.find(',') != std::string::npos) {
					std::vector<std::string> parts;
					size_t start = 0;
					while (start <= creatureName.size()) {
						size_t comma = creatureName.find(',', start);
						std::string part = creatureName.substr(start,
							comma == std::string::npos ? std::string::npos : comma - start);
						while (!part.empty() && part.front() == ' ') part.erase(part.begin());
						while (!part.empty() && part.back() == ' ') part.pop_back();
						if (!part.empty()) parts.push_back(std::move(part));
						if (comma == std::string::npos) break;
						start = comma + 1;
					}
					if (!parts.empty()) {
						creatureName = parts[uniform_random(0, static_cast<int32_t>(parts.size()) - 1)];
					}
				}
			} else {
				std::vector<size_t> matches;
				for (size_t i = 0; i < lexCreatures_.size(); ++i) {
					if (levelOk(lexCreatures_[i].levelMin, lexCreatures_[i].levelMax)) matches.push_back(i);
				}
				if (matches.empty()) return {};
				creatureName = lexCreatures_[matches[uniform_random(0, static_cast<int32_t>(matches.size()) - 1)]].name;
			}
			if (creatureName.empty()) return {};
			substitute("%creature", toLower(creatureName));
		}
	}

	// Trade %item/%price/%stack. Entry choice respects the bot's buy/sell lane
	// (intent 0 = both); community alias used ~40% of the time ("wts ssa 290k").
	if (!tradeCatalog_.empty()
	    && (text.find("%item") != std::string::npos || text.find("%price") != std::string::npos
	        || text.find("%stack") != std::string::npos)) {
		const TradeEntry* entry = nullptr;
		for (int i = 0; i < 8 && !entry; ++i) {
			const TradeEntry& cand = tradeCatalog_[uniform_random(
				0, static_cast<int32_t>(tradeCatalog_.size()) - 1)];
			if (cand.intent == 0 || cand.intent == botLane) entry = &cand;
		}
		if (!entry) {
			entry = &tradeCatalog_[uniform_random(0, static_cast<int32_t>(tradeCatalog_.size()) - 1)];
		}
		std::string itemName = entry->name;
		if (!entry->aliases.empty() && uniform_random(1, 100) <= 40) {
			itemName = entry->aliases[uniform_random(0, static_cast<int32_t>(entry->aliases.size()) - 1)];
		}
		substitute("%item", itemName);
		const int32_t price = uniform_random(entry->priceMinGp, std::max(entry->priceMinGp, entry->priceMaxGp));
		// Format price as "1.2kk" for millions, "12k" for thousands, else raw.
		std::string priceStr;
		if (price >= 1000000) {
			priceStr = (price % 1000000 >= 100000)
				? fmt::format("{}.{}kk", price / 1000000, (price % 1000000) / 100000)
				: fmt::format("{}kk", price / 1000000);
		} else if (price >= 1000) {
			priceStr = fmt::format("{}k", price / 1000);
		} else {
			priceStr = std::to_string(price);
		}
		substitute("%price", priceStr);
		substitute("%stack", std::to_string(uniform_random(1, 50)));
	}

	// Strip any leading slash to defang command injection attempts in templates.
	while (!text.empty() && text.front() == '/') text.erase(text.begin());
	return text;
}

bool BotEngine::tryEmitChat(BotState& bot, const std::shared_ptr<Player>& player,
                             const std::string& category, uint16_t channelId) {
	if (!player) return false;
	auto it = chatCatalog_.find(category);
	if (it == chatCatalog_.end() || it->second.templates.empty()) return false;
	const ChatBucket& bucket = it->second;
	if (bucket.chance == 0) return false;
	const auto& cfg = livenessCfg_;
	// Channel chat (channelId > 0) requires at least one real player online to
	// have a reader. Cast viewers see local-say chat from the bot they're
	// watching but don't typically have channels open. Silencing channels when
	// realPlayerCount == 0 eliminates wasted work during night idle.
	if (channelId > 0 && cfg.realPlayerCount == 0) return false;
	// Fix #10: master "try to talk" gate. Scales overall chat rate without
	// touching per-category percentages. 100 = no-op (per-category chances
	// stand alone). 50 = halve overall chat rate. Etc.
	if (cfg.chatMasterChancePct < 100
	    && uniform_random(1, 100) > cfg.chatMasterChancePct) return false;
	if (uniform_random(1, 100) > bucket.chance) return false;

	// Cooldown check (local chat only; channel posts use their own timers).
	const int64_t now = OTSYS_TIME();
	if (channelId == 0) {
		// Fix #1: cached config (was 3 g_configManager calls inside the hot path —
		// missed by the prior cache hotfix 30a503cc5, contributing to mutex churn).
		// Chat cooldown scaled by personality chattyness (4-bit, 0-15).
		// chattyness=0 -> max cooldown, chattyness=15 -> min cooldown.
		const int32_t cdMin = cfg.chatCooldownMinMs;
		const int32_t cdMax = cfg.chatCooldownMaxMs;
		if (now - bot.lastChatTimeMs < cdMin) return false;
		const int32_t cdEffective = cdMax - ((cdMax - cdMin) * static_cast<int32_t>(bot.chattyness()) / 15);
		if (now - bot.lastChatTimeMs < cdEffective && uniform_random(1, 100) > 30) return false;
	}

	// Observer gate for local-say: skip if no real player or cast viewer is on
	// screen. Channel posts (channelId > 0) bypass — they're server-wide.
	// v2: the scan also CAPTURES the first real player's creature id for the
	// per-player flood throttle (the v1 scan broke on first match without
	// recording who it found).
	uint32_t observerPlayerId = 0;
	if (channelId == 0) {
		// Fix #8: anchor early-exit. If there are zero real-player or cast-watched
		// anchors anywhere on the server, no observer can possibly be in spectator
		// range. Skip the Spectators::find scan entirely (was ~2500 wasted scans/sec
		// with all-bots-no-players scenario).
		if (currentAnchors_.empty()) return false;
		bool observed = false;
		auto spectators = Spectators().find<Player>(player->getPosition(), true);
		for (const auto& s : spectators) {
			auto otherPlayer = s ? s->getPlayer() : nullptr;
			if (!otherPlayer) continue;
			if (otherPlayer->getID() == player->getID()) continue;
			if (!otherPlayer->isBotPlayer()) {
				observed = true;
				observerPlayerId = otherPlayer->getID();
				break;  // real player found — best throttle anchor
			}
			if (otherPlayer->getCastViewerCount() > 0) {
				observed = true;  // cast-watched bot counts as an observer; keep
				                  // scanning in case a real player is also on screen
			}
		}
		if (!observed) return false;

		// Phase D per-player flood throttle, ambient categories only: if ANY bot
		// already spoke near this player within the window, stay quiet — a player
		// walking through a bot-dense depot should get ~1 line per half-minute,
		// not a wall of text. Reactive categories (combat/flee/depot) bypass:
		// they're situational, rarer, and multiple bots reacting is plausible.
		const bool ambient = (category == "idle" || category == "banter");
		if (ambient && observerPlayerId != 0) {
			auto thrIt = playerChatThrottle_.find(observerPlayerId);
			if (thrIt != playerChatThrottle_.end()) {
				const int32_t window = uniform_random(chatCfg_.playerThrottleMinMs,
					std::max(chatCfg_.playerThrottleMinMs, chatCfg_.playerThrottleMaxMs));
				if (now - thrIt->second < window) return false;
			}
			// Prune-on-access keeps the map bounded without a periodic sweep.
			if (playerChatThrottle_.size() > 512) {
				std::erase_if(playerChatThrottle_, [&](const auto& kv) {
					return now - kv.second > 10 * 60 * 1000;
				});
			}
		}
	}

	// Advertising lane: personalitySeed parity fixes each bot's buy/sell
	// preference for the session, so one bot doesn't advertise selling an item
	// and then buying it back five minutes later. Neutral templates always pass.
	const bool laneFiltered = !bucket.intents.empty();
	const uint8_t botLane = (bot.personalitySeed & 1) ? 2 : 1;  // 1=sell, 2=buy

	// v2 pick + render + dedup loop. Up to 4 attempts to find a template that
	// (a) passes the bot's lane, (b) isn't in the bot's per-template anti-repeat
	// ring, (c) renders with level-coherent %creature/%spawn data, and (d) for
	// advertising/banter, hasn't been rendered identically by ANY bot within the
	// global window (Phase C). On full failure: channel posts SKIP (an identical
	// line on a server-wide channel is an obvious tell), local-say accepts the
	// last render (a bot going silent mid-interaction is weirder than a repeat).
	int64_t dedupWindowMs = 0;
	if (category == "advertising") dedupWindowMs = chatCfg_.dedupWindowAdvertisingMs;
	else if (category == "banter") dedupWindowMs = chatCfg_.dedupWindowBanterMs;

	std::string text;
	uint32_t idx = 0;
	bool haveCandidate = false;
	bool globallyFresh = false;
	for (int attempt = 0; attempt < 4; ++attempt) {
		const uint32_t candidate = static_cast<uint32_t>(uniform_random(
			0, static_cast<int32_t>(bucket.templates.size()) - 1));
		if (laneFiltered && candidate < bucket.intents.size()) {
			const uint8_t intent = bucket.intents[candidate];
			if (intent != 0 && intent != botLane) continue;  // other lane — reroll
		}
		// Per-bot template ring (mix category hash with idx so categories don't
		// collide in the ring) — same scheme as v1.
		const uint32_t ringHash = static_cast<uint32_t>(std::hash<std::string>{}(category)) ^ candidate;
		bool inRing = false;
		for (size_t i = 0; i < BotState::kChatAntiRepeatRingSize; ++i) {
			if (bot.recentChatRing[i] == ringHash) { inRing = true; break; }
		}
		if (inRing && attempt < 3) continue;

		std::string rendered = renderChatTemplate(bot, player, bucket.templates[candidate], botLane);
		if (rendered.empty()) continue;  // unsatisfiable placeholder (level gate) — reroll

		idx = candidate;
		text = std::move(rendered);
		haveCandidate = true;
		globallyFresh = true;
		if (dedupWindowMs > 0) {
			auto gIt = recentGlobalChat_.find(hashRenderedChat(text));
			if (gIt != recentGlobalChat_.end() && now - gIt->second < dedupWindowMs) {
				globallyFresh = false;
				continue;  // another bot said exactly this recently — reroll
			}
		}
		break;
	}
	if (!haveCandidate || text.empty()) return false;
	if (!globallyFresh && channelId > 0) return false;  // skip beats dup on channels

	// Emit. Fix #3: capture talkToChannel return so we only log real broadcasts,
	// not phantom-successes from bots that aren't channel members.
	bool emitted = false;
	if (channelId == 0) {
		g_game().internalCreatureSay(player, TALKTYPE_SAY, text, /*ghostMode=*/false);
		emitted = true;  // internalCreatureSay has no return; assume success
	} else {
		emitted = g_chat().talkToChannel(player, TALKTYPE_CHANNEL_Y, text, channelId);
	}
	if (!emitted) return false;

	// Update bot ring + timers + global dedup + per-player throttle
	uint32_t hash = static_cast<uint32_t>(std::hash<std::string>{}(category)) ^ idx;
	bot.recentChatRing[bot.recentChatRingNext] = hash;
	bot.recentChatRingNext = static_cast<uint8_t>((bot.recentChatRingNext + 1) % BotState::kChatAntiRepeatRingSize);
	if (channelId == 0) {
		bot.lastChatTimeMs = now;
		if (observerPlayerId != 0) playerChatThrottle_[observerPlayerId] = now;
	}
	const uint32_t textHash = hashRenderedChat(text);
	if (dedupWindowMs > 0) {
		recentGlobalChat_[textHash] = now;
		// Prune-on-insert: drop entries past the largest window once the map
		// grows past its soft bound. ~1024 entries × 16 bytes is nothing; the
		// bound just stops unbounded growth across long uptimes.
		if (recentGlobalChat_.size() > 1024) {
			const int64_t maxWindow = std::max(chatCfg_.dedupWindowAdvertisingMs,
				chatCfg_.dedupWindowBanterMs);
			std::erase_if(recentGlobalChat_, [&](const auto& kv) {
				return now - kv.second > maxWindow;
			});
		}
	}

	// Async telemetry insert (best-effort, opt-in). Only fires on confirmed broadcast
	// AND when botTelemetryEnabled. text_hash = hash of the rendered lowercased line —
	// lets SQL measure the live cross-bot dup rate (Phase E acceptance: <2% in any
	// 15-min window). Never read by runtime logic; dedup is in-memory (recentGlobalChat_).
	if (cfg.telemetryEnabled) {
		g_botDatabaseTasks().execute(fmt::format(
			"INSERT INTO `bot_chat_emissions` (`ts`, `bot_guid`, `category`, `phrase_idx`, `channel_id`, `text_hash`) "
			"VALUES (UNIX_TIMESTAMP(), {}, {}, {}, {}, {})",
			bot.guid, Database::getInstance().escapeString(category), idx, channelId, textHash));
	}

	// Fix #1 (cont.): cached BOT_CHAT_VERBOSE_LOG (was the 3rd missed cache read).
	if (cfg.chatVerboseLog) {
		g_logger().info("[BOT:CHAT] guid={} cat={} ch={} text='{}'",
			bot.guid, category, channelId, text);
	}
	return true;
}

// ============================================================================
// BOT_CHAT_LIVENESS_V2 Phase F: keyword replies (local say + PMs)
// ============================================================================

// Crude keyword classifier. Word-boundary matching on the lowercased text.
// Priority: concrete intents (price/trade) beat social fillers (greeting), so
// "hi, how much for the ssa?" lands in price_query. Local-say returns "" when
// nothing matches (bots must NOT butt into every nearby conversation); PMs
// fall back to "generic" — a PM is always addressed at the bot.
std::string BotEngine::classifyReplyTrigger(const std::string& text, bool isPm) {
	std::string lower = " ";
	lower.reserve(text.size() + 2);
	for (char ch : text) {
		lower += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	lower += ' ';
	// Strip punctuation into spaces so "hi!" / "price?" still word-match.
	for (char& ch : lower) {
		if (ch == '!' || ch == '?' || ch == '.' || ch == ',' || ch == ':' || ch == ';') ch = ' ';
	}
	auto hasWord = [&](const char* word) {
		const std::string needle = std::string(" ") + word + " ";
		return lower.find(needle) != std::string::npos;
	};
	if (lower.find(" how much ") != std::string::npos || hasWord("price") || hasWord("pc")) {
		return "price_query";
	}
	if (hasWord("wts") || hasWord("wtb") || hasWord("trade") || hasWord("selling") || hasWord("buying")) {
		return "trade_query";
	}
	if (hasWord("ty") || hasWord("thx") || hasWord("thanks") || lower.find(" thank you ") != std::string::npos) {
		return "thanks";
	}
	if (hasWord("bye") || hasWord("cya") || hasWord("gl")) {
		return "bye";
	}
	if (hasWord("hi") || hasWord("hello") || hasWord("hey") || hasWord("yo")
	    || hasWord("sup") || hasWord("hiho")) {
		return "greeting";
	}
	return isPm ? "generic" : "";
}

void BotEngine::onPlayerSayNearBots(uint32_t playerId, const Position& pos, const std::string& text) {
	if (replyCatalog_.empty() || chatCfg_.replyChancePct <= 0) return;
	const std::string group = classifyReplyTrigger(text, /*isPm=*/false);
	if (group.empty()) return;
	const int64_t now = OTSYS_TIME();

	// One queued reply per player at a time — stops "hi hi hi" from queueing a
	// chorus before the first reply even fires.
	for (const auto& pending : pendingReplies_) {
		if (pending.playerId == playerId) return;
	}

	// Pick the NEAREST eligible idle bot on screen (most natural responder).
	BotState* chosen = nullptr;
	int32_t bestDist = INT32_MAX;
	for (auto& bot : bots_) {
		if (!bot.active || bot.hibernated || bot.aiPaused) continue;
		if (bot.state != BotAIState::IDLE && bot.state != BotAIState::DWELLING) continue;
		if (bot.currentPos.z != pos.z) continue;
		const int32_t dist = std::max(
			std::abs(static_cast<int32_t>(bot.currentPos.x) - static_cast<int32_t>(pos.x)),
			std::abs(static_cast<int32_t>(bot.currentPos.y) - static_cast<int32_t>(pos.y)));
		if (dist > 7) continue;
		if (now - bot.lastReplyTimeMs < chatCfg_.replyCooldownPerBotMs) continue;
		if (dist < bestDist) {
			bestDist = dist;
			chosen = &bot;
		}
	}
	if (!chosen) return;
	if (uniform_random(1, 100) > chatCfg_.replyChancePct) return;

	chosen->lastReplyTimeMs = now;  // reserve immediately so the next say can't double-book
	pendingReplies_.push_back({
		now + uniform_random(chatCfg_.replyDelayMinMs,
			std::max(chatCfg_.replyDelayMinMs, chatCfg_.replyDelayMaxMs)),
		chosen->guid, playerId, /*isPm=*/false, group });
}

void BotEngine::onPlayerPmToBot(uint32_t botGuid, uint32_t playerId, const std::string& text) {
	if (replyCatalog_.empty() || chatCfg_.pmReplyChancePct <= 0) return;
	BotState* bot = getBotState(botGuid);
	if (!bot) return;
	const int64_t now = OTSYS_TIME();
	// Hard 10s floor between PM replies per bot — a PM is direct, so the usual
	// reply cooldown would feel like being ignored, but instant machine-gun
	// replies to spam would be worse.
	if (now - bot->lastReplyTimeMs < 10000) return;
	for (const auto& pending : pendingReplies_) {
		if (pending.botGuid == botGuid && pending.isPm) return;
	}
	if (uniform_random(1, 100) > chatCfg_.pmReplyChancePct) return;
	const std::string group = classifyReplyTrigger(text, /*isPm=*/true);

	bot->lastReplyTimeMs = now;
	pendingReplies_.push_back({
		now + uniform_random(chatCfg_.replyDelayMinMs,
			std::max(chatCfg_.replyDelayMinMs, chatCfg_.replyDelayMaxMs)),
		botGuid, playerId, /*isPm=*/true, group });
}

void BotEngine::processPendingReplies(int64_t now) {
	if (pendingReplies_.empty()) return;
	for (auto it = pendingReplies_.begin(); it != pendingReplies_.end();) {
		if (now < it->fireAtMs) {
			++it;
			continue;
		}
		const PendingReply reply = *it;
		it = pendingReplies_.erase(it);

		auto catIt = replyCatalog_.find(reply.group);
		if (catIt == replyCatalog_.end() || catIt->second.empty()) continue;
		const uint32_t lineIdx = static_cast<uint32_t>(uniform_random(
			0, static_cast<int32_t>(catIt->second.size()) - 1));
		const std::string& line = catIt->second[lineIdx];

		// Re-resolve both parties — either may have moved on in the delay window.
		auto target = g_game().getCreatureByID(reply.playerId);
		auto targetPlayer = target ? target->getPlayer() : nullptr;
		if (!targetPlayer || targetPlayer->isRemoved()) continue;
		auto botPlayer = g_game().getPlayerByGUID(reply.botGuid);
		if (!botPlayer && reply.isPm) {
			botPlayer = getHibernatedBotPlayer(reply.botGuid);  // PMs work from hibernation
		}
		if (!botPlayer) continue;

		if (reply.isPm) {
			targetPlayer->sendPrivateMessage(botPlayer, TALKTYPE_PRIVATE_FROM, line);
		} else {
			if (botPlayer->isRemoved()) continue;  // hibernated mid-delay — can't local-say
			g_game().internalCreatureSay(botPlayer, TALKTYPE_SAY, line, /*ghostMode=*/false);
		}

		if (BotState* botState = getBotState(reply.botGuid)) {
			botState->lastChatTimeMs = now;  // a reply counts as chat for ambient pacing
		}
		playerChatThrottle_[reply.playerId] = now;

		if (livenessCfg_.telemetryEnabled) {
			g_botDatabaseTasks().execute(fmt::format(
				"INSERT INTO `bot_chat_emissions` (`ts`, `bot_guid`, `category`, `phrase_idx`, `channel_id`, `text_hash`) "
				"VALUES (UNIX_TIMESTAMP(), {}, {}, {}, {}, {})",
				reply.botGuid, Database::getInstance().escapeString("reply_" + reply.group),
				lineIdx, reply.isPm ? 1 : 0, hashRenderedChat(line)));
		}
		if (livenessCfg_.chatVerboseLog) {
			g_logger().info("[BOT:CHAT:REPLY] guid={} group={} pm={} text='{}'",
				reply.botGuid, reply.group, reply.isPm, line);
		}
	}
}
