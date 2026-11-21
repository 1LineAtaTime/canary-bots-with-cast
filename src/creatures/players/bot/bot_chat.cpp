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
		chatCfg_.convoOwnerWindowMs = std::max(0, num("convoOwnerWindowMs", chatCfg_.convoOwnerWindowMs));
		chatCfg_.convoExchangeCap = std::max(1, num("convoExchangeCap", chatCfg_.convoExchangeCap));
		chatCfg_.convoReplyChancePct = std::clamp(num("convoReplyChancePct", chatCfg_.convoReplyChancePct), 0, 100);
		chatCfg_.convoReplyCooldownMs = std::max(0, num("convoReplyCooldownMs", chatCfg_.convoReplyCooldownMs));
		chatCfg_.replyIntentGate = std::max(0, num("replyIntentGate", chatCfg_.replyIntentGate));
		chatCfg_.convoAntiRepeat = std::clamp(num("convoAntiRepeat", chatCfg_.convoAntiRepeat), 0, 20);

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

// #122: who may answer a keyword said out loud. Baseline B7 (runs/bl_b7, bl_b7b): 0 of 15 local
// keywords answered at the Thais depot, because only IDLE/DWELLING bots were eligible and the bots
// actually standing near a player there are mostly TRAVELING -- walking to or from the boat.
// A real player walking past answers "hi" without stopping, so a traveller that is WALKING
// (walk_to_boat / walk_from_boat / arrived) and not mid floor-change may answer too. Never: a bot
// at the boat NPC or in the ride itself (at_boat / teleported -- it is about to vanish), a
// hibernated bot (it has no body to say anything with), a paused one, or any other state
// (combat, hunting, party, PvP).
namespace {
	enum class ReplyEligibility { Stationary, Walking, No };

	ReplyEligibility localReplyEligibility(const BotState& bot) {
		if (!bot.active || bot.hibernated || bot.aiPaused) return ReplyEligibility::No;
		if (bot.state == BotAIState::IDLE || bot.state == BotAIState::DWELLING) return ReplyEligibility::Stationary;
		if (bot.state == BotAIState::TRAVELING && bot.fcState == FloorChangeState::NONE
		    && (bot.travelPhase == "walk_to_boat" || bot.travelPhase == "walk_from_boat" || bot.travelPhase == "arrived")) {
			return ReplyEligibility::Walking;
		}
		return ReplyEligibility::No;
	}

	// [CHAT_REPLY] lines are rate-limited per kind (1/s each): a player spamming keywords must not
	// turn them into a journal flood. said/dropped follow a queued line, so they are bounded too.
	int64_t s_lastNoResponderLogMs = 0;
	int64_t s_lastSkippedLogMs = 0;
	int64_t s_lastQueuedLogMs = 0;

	// #122 (review): ONE earshot predicate, used both to pick a responder and at fire time. A say is
	// seen on the speaker's screen: |dx| <= 8, |dy| <= 6, same floor.
	bool inSayEarshot(const Position& a, const Position& b) {
		return a.z == b.z && std::abs(static_cast<int32_t>(a.x) - static_cast<int32_t>(b.x)) <= 8
			&& std::abs(static_cast<int32_t>(a.y) - static_cast<int32_t>(b.y)) <= 6;
	}

	// #122 follow-up (P5): how long a responder's BODY stays in earshot of the speaker, replaying
	// its pending walk queue (Creature::listWalkDir, consumed from the back) at its real step
	// duration. Runs p5c_f122/p5p_f122b (cli-p1.ndjson): bots cross this 17x13 box in ~2 s median
	// at ~100-150 ms/step, and the ones heading south to the (32345,32225) exit leave it in ~1 s --
	// so a 1.5-2.5 s typing delay was a coin flip in BOTH builds. Also covers IDLE/DWELLING bots
	// that are walking (a POI walk): p5p_f122b Dalgorsh was st=IDLE and moving 7 tiles/s.
	// Returns horizonMs when the body is still in earshot at the horizon (or is not walking). The
	// queue is one chunk: when it runs out we assume the bot stops; fire time re-checks anyway.
	// walkDirs = the body's listWalkDir (protected in Creature; the BotEngine caller is a friend).
	int32_t earshotRemainingMs(Player& body, const std::vector<Direction>& walkDirs, const Position& speaker, int32_t horizonMs) {
		Position p = body.getPosition();
		if (!inSayEarshot(p, speaker)) return 0;
		int32_t t = 0;
		for (auto it = walkDirs.rbegin(); it != walkDirs.rend() && t < horizonMs; ++it) {
			t += std::max<int32_t>(50, body.getStepDuration(*it));
			p = getNextPosition(*it, p);
			if (!inSayEarshot(p, speaker)) return t;
		}
		return horizonMs;
	}
}

// #105: does `lowerText` (lowercased, punctuation already turned into spaces, padded with a space
// on both ends) address this bot by name? The full name, or its first word when that word is at
// least 3 letters ("santiago, how much?"). Word-bounded so "al" never matches "all".
namespace {
	std::string chatLowerPadded(const std::string& text) {
		std::string lower = " ";
		lower.reserve(text.size() + 2);
		for (char ch : text) {
			char c = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			if (c == '!' || c == '?' || c == '.' || c == ',' || c == ':' || c == ';') c = ' ';
			lower += c;
		}
		lower += ' ';
		return lower;
	}

	bool chatAddressesName(const std::string& lowerPadded, const std::string& botName) {
		std::string name;
		name.reserve(botName.size());
		for (char ch : botName) name += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		if (name.empty()) return false;
		if (lowerPadded.find(" " + name + " ") != std::string::npos) return true;
		const size_t sp = name.find(' ');
		if (sp == std::string::npos || sp < 3) return false;
		return lowerPadded.find(" " + name.substr(0, sp) + " ") != std::string::npos;
	}

	int32_t chatWordCount(const std::string& lowerPadded) {
		int32_t n = 0;
		bool in = false;
		for (char c : lowerPadded) {
			if (c == ' ') { in = false; continue; }
			if (!in) { ++n; in = true; }
		}
		return n;
	}

	// Intent strength of the keyword group alone. Concrete questions are always worth an answer;
	// a social filler is, only when it is the whole line ("hi", "ty m8") -- buried in a long
	// sentence it is the player talking to someone else.
	int32_t chatKeywordScore(const std::string& group, int32_t words) {
		if (group.empty() || group == "generic") return 0;
		if (group == "price_query" || group == "trade_query") return 3;
		return words <= 4 ? 3 : 2;
	}

	int64_t s_lastConvoLogMs = 0;
}

void BotEngine::onPlayerSayNearBots(uint32_t playerId, const Position& pos, const std::string& text) {
	if (replyCatalog_.empty() || chatCfg_.replyChancePct <= 0) return;
	const int64_t now = OTSYS_TIME();

	// One queued reply per player at a time -- stops "hi hi hi" from queueing a
	// chorus before the first reply even fires.
	for (const auto& pending : pendingReplies_) {
		if (pending.playerId == playerId) return;
	}

	// #105: the conversation this line belongs to. A thread lives while the player keeps talking
	// (each line re-arms the window), so it is judged against the PREVIOUS line's time.
	if (conversations_.size() > 256) {
		std::erase_if(conversations_, [&](const auto& kv) {
			return now - kv.second.lastSayMs > std::max<int64_t>(chatCfg_.convoOwnerWindowMs, 60000);
		});
	}
	Conversation& convo = conversations_[playerId];
	const bool threadLive = convo.ownerGuid != 0 && now - convo.lastSayMs < chatCfg_.convoOwnerWindowMs;
	if (!threadLive) {
		convo = Conversation {};
	}
	convo.lastSayMs = now;

	const std::string lower = chatLowerPadded(text);
	const int32_t words = chatWordCount(lower);
	std::string group = classifyReplyTrigger(text, /*isPm=*/false);

	// Pick the NEAREST eligible bot within earshot (most natural responder). A bot that is standing
	// still (IDLE/DWELLING) is preferred over a walking traveller at any distance: it will still
	// be there when the reply fires. A traveller is the fallback, not a giving-up point.
	// P5 follow-up: "walking" is decided by the BODY (a non-empty walk queue), not by the AI state --
	// an IDLE/DWELLING bot on a POI walk crosses the player's screen just as fast as a traveller. A
	// candidate whose body leaves earshot before the shortest acceptable reply is skipped ("leaving").
	const int32_t delayMax = std::max(chatCfg_.replyDelayMinMs, chatCfg_.replyDelayMaxMs);
	const int32_t delayFloor = std::min(chatCfg_.replyDelayMinMs, 800);
	constexpr int32_t kEarshotMarginMs = 200;
	struct Cand {
		BotState* bot = nullptr;
		int32_t rank = INT32_MAX;
		bool walking = false;
		int32_t stayMs = 0;
		Position bodyPos;
	};
	Cand best;       // nearest eligible bot (opens a thread)
	Cand addressed;  // a bot the line names
	Cand owner;      // the thread's owner, when it can answer
	const char* ownerWhy = "absent";
	uint32_t inRange = 0, ineligible = 0, cooling = 0, leaving = 0;
	std::array<uint32_t, 9> inelByState {};
	for (auto& bot : bots_) {
		if (!bot.active || !inSayEarshot(bot.currentPos, pos)) continue;
		inRange++;
		const bool isOwner = threadLive && bot.guid == convo.ownerGuid;
		const bool named = chatAddressesName(lower, bot.name);
		const ReplyEligibility e = localReplyEligibility(bot);
		if (e == ReplyEligibility::No) {
			ineligible++;
			const auto si = static_cast<size_t>(bot.state);
			if (si < inelByState.size()) inelByState[si]++;
			if (isOwner) ownerWhy = "ineligible";
			continue;
		}
		// The owner of a live thread (and a bot addressed by name) answers on the short
		// conversation floor; everyone else on the long ambient per-bot cooldown.
		const int32_t cd = (isOwner || named) ? chatCfg_.convoReplyCooldownMs : chatCfg_.replyCooldownPerBotMs;
		if (now - bot.lastReplyTimeMs < cd) {
			cooling++;
			if (isOwner) ownerWhy = "cooling";
			continue;
		}
		const auto body = g_game().getPlayerByGUID(bot.guid);
		if (!body || body->isRemoved()) { ineligible++; continue; }
		const Position bodyPos = body->getPosition();
		const int32_t stayMs = earshotRemainingMs(*body, body->listWalkDir, pos, delayMax);
		if (stayMs < delayFloor + kEarshotMarginMs) {
			leaving++;
			if (isOwner) ownerWhy = "leaving";
			continue;
		}
		const bool walking = e == ReplyEligibility::Walking || !body->listWalkDir.empty();
		const int32_t dist = std::max(
			std::abs(static_cast<int32_t>(bodyPos.x) - static_cast<int32_t>(pos.x)),
			std::abs(static_cast<int32_t>(bodyPos.y) - static_cast<int32_t>(pos.y)));
		// Standing first; among walkers, the one that stays on screen longest, then the nearest.
		const int32_t rank = walking ? 1000 + (delayMax - stayMs) / 100 * 10 + dist : dist;
		const Cand c { &bot, rank, walking, stayMs, bodyPos };
		if (isOwner) owner = c;
		if (named && rank < addressed.rank) addressed = c;
		if (rank < best.rank) best = c;
	}

	// Who may answer. Addressed by name beats everything (and takes the thread over); inside a
	// live thread ONLY the owner may answer; otherwise the nearest eligible bot opens a thread.
	Cand chosen;
	const char* ownReason = "first";
	bool inThread = false;
	if (addressed.bot) {
		chosen = addressed;
		inThread = threadLive && addressed.bot->guid == convo.ownerGuid;
		ownReason = threadLive ? "transfer" : "addressed";
		if (group.empty()) group = "generic";
	} else if (threadLive && !owner.bot && std::string_view(ownerWhy) == "cooling") {
		// The owner is right here and just answered: the thread is still its own. Nobody else
		// jumps in; the player's next line (past the short floor) goes to the owner again.
		if (now - s_lastConvoLogMs >= 1000) {
			s_lastConvoLogMs = now;
			g_logger().info("[CHAT_CONVO] held owner_guid={} player={} reason=owner-cooling others_in_range={}",
				convo.ownerGuid, playerId, inRange);
		}
		return;
	} else if (threadLive && owner.bot) {
		chosen = owner;
		inThread = true;
	} else {
		// No thread, or its owner walked off / went busy (absent, leaving, ineligible): a thread
		// whose owner is gone must not silence everyone else for as long as the player talks.
		if (threadLive) {
			g_logger().info("[CHAT_CONVO] release owner_guid={} player={} reason=owner-{}", convo.ownerGuid, playerId, ownerWhy);
			const int64_t keepSay = convo.lastSayMs;
			convo = Conversation {};
			convo.lastSayMs = keepSay;
		}
		chosen = best;
	}
	if (inThread && convo.ended) {
		if (now - s_lastConvoLogMs >= 1000) {
			s_lastConvoLogMs = now;
			g_logger().info("[CHAT_CONVO] silent bot='{}' player={} reason=ended exchanges={}",
				chosen.bot->name, playerId, convo.exchanges);
		}
		return;
	}

	if (!chosen.bot) {
		if (group.empty()) return;
		if (now - s_lastNoResponderLogMs >= 1000) {
			s_lastNoResponderLogMs = now;
			std::string inel;
			for (size_t i = 0; i < inelByState.size(); ++i) {
				if (inelByState[i]) {
					inel += fmt::format("{}{}:{}", inel.empty() ? "" : ",", botStateName(static_cast<BotAIState>(i)), inelByState[i]);
				}
			}
			g_logger().info("[CHAT_REPLY] none group={} at=({},{},{}) bots_in_range={} ineligible={} cooling={} leaving={} inel=[{}]",
				group, pos.x, pos.y, pos.z, inRange, ineligible, cooling, leaving, inel);
		}
		return;
	}

	// Intent gate. A line nobody could take as meant for a bot is not answered at all.
	const int32_t score = chatKeywordScore(group, words) + (addressed.bot ? 3 : 0) + (inThread ? 1 : 0);
	if (group.empty() || score < chatCfg_.replyIntentGate) {
		if ((inThread || addressed.bot) && now - s_lastSkippedLogMs >= 1000) {
			s_lastSkippedLogMs = now;
			g_logger().info("[CHAT_REPLY] skipped bot='{}' group={} reason=intent(score={} gate={})",
				chosen.bot->name, group.empty() ? "-" : group, score, chatCfg_.replyIntentGate);
		}
		return;
	}
	const int32_t chance = (inThread || addressed.bot) ? chatCfg_.convoReplyChancePct : chatCfg_.replyChancePct;
	if (uniform_random(1, 100) > chance) {
		if (now - s_lastSkippedLogMs >= 1000) {
			s_lastSkippedLogMs = now;
			g_logger().info("[CHAT_REPLY] skipped bot='{}' group={} reason=chance({}%)", chosen.bot->name, group, chance);
		}
		return;  // an opening line that lost the roll opens no thread: the next line gets a fresh pick
	}

	// Thread bookkeeping. A new owner starts a fresh thread; the cap answer is the last one.
	if (!inThread) {
		convo.ownerGuid = chosen.bot->guid;
		convo.exchanges = 0;
		convo.ended = false;
		convo.recentLines.clear();
		g_logger().info("[CHAT_CONVO] own bot='{}' player={} reason={} score={}", chosen.bot->name, playerId, ownReason, score);
	}
	convo.exchanges++;
	if (group == "bye" || convo.exchanges >= chatCfg_.convoExchangeCap) {
		const bool capped = group != "bye";
		convo.ended = true;
		if (capped && replyCatalog_.count("bye") > 0) group = "bye";  // close the thread politely
		g_logger().info("[CHAT_CONVO] end bot='{}' player={} exchanges={} reason={}",
			chosen.bot->name, playerId, convo.exchanges, capped ? "cap" : "bye");
	}

	// A walker answers from the quick end of the typing delay: it keeps walking while it types, and
	// a 5 s pause would carry it out of earshot. Either way the reply must land while the body is
	// still on the speaker's screen (stayMs, from its walk queue), so the delay is capped there.
	int32_t delay = chosen.walking
		? uniform_random(chatCfg_.replyDelayMinMs, std::min(delayMax, chatCfg_.replyDelayMinMs + 1000))
		: uniform_random(chatCfg_.replyDelayMinMs, delayMax);
	if (delay > chosen.stayMs - kEarshotMarginMs) {
		delay = std::max(delayFloor, chosen.stayMs - kEarshotMarginMs);
	}
	chosen.bot->lastReplyTimeMs = now;  // reserve immediately so the next say cannot double-book
	pendingReplies_.push_back({ now + delay, chosen.bot->guid, playerId, /*isPm=*/false, group });
	if (now - s_lastQueuedLogMs >= 1000) {
		s_lastQueuedLogMs = now;
		g_logger().info("[CHAT_REPLY] queued bot='{}' st={} walking={} dist={} group={} delayMs={} stayMs={} bot=({},{},{}) convo={}/{} score={} inRange={}",
			chosen.bot->name, botStateName(chosen.bot->state), chosen.walking ? 1 : 0,
			std::max(std::abs(static_cast<int32_t>(chosen.bodyPos.x) - static_cast<int32_t>(pos.x)),
			         std::abs(static_cast<int32_t>(chosen.bodyPos.y) - static_cast<int32_t>(pos.y))),
			group, delay, chosen.stayMs, chosen.bodyPos.x, chosen.bodyPos.y, chosen.bodyPos.z,
			convo.exchanges, chatCfg_.convoExchangeCap, score, inRange);
	}
}

void BotEngine::onPlayerPmToBot(uint32_t botGuid, uint32_t playerId, const std::string& text) {
	// #91: a PM that is a command verb ("follow", "@ek stay", "who") is answered by the verb layer
	// (bot_verb.cpp) and never also gets a small-talk reply. Ordinary chat falls through.
	if (const auto speakerCreature = g_game().getCreatureByID(playerId)) {
		if (const auto speaker = speakerCreature->getPlayer();
		    speaker && handleVerb(fmt::format("pm:{}", botGuid), speaker, botGuid, text, nullptr)) {
			return;
		}
	}
	if (replyCatalog_.empty() || chatCfg_.pmReplyChancePct <= 0) return;
	BotState* bot = getBotState(botGuid);
	if (!bot) return;
	const int64_t now = OTSYS_TIME();
	// Hard 10s floor between PM replies per bot -- a PM is direct, so the usual
	// reply cooldown would feel like being ignored, but instant machine-gun
	// replies to spam would be worse.
	if (now - bot->lastReplyTimeMs < 10000) return;
	for (const auto& pending : pendingReplies_) {
		if (pending.botGuid == botGuid && pending.isPm) return;
	}
	// #105: a PM is addressed by definition, so it opens (or continues) a thread owned by this bot,
	// and the same cap applies: past it the bot stops answering until the player goes quiet.
	Conversation& convo = conversations_[playerId];
	const bool threadLive = convo.ownerGuid == botGuid && now - convo.lastSayMs < chatCfg_.convoOwnerWindowMs;
	if (!threadLive) {
		convo = Conversation {};
		convo.ownerGuid = botGuid;
	}
	convo.lastSayMs = now;
	if (convo.ended) {
		g_logger().info("[CHAT_CONVO] silent bot='{}' player={} reason=ended exchanges={} pm=1", bot->name, playerId, convo.exchanges);
		return;
	}
	if (uniform_random(1, 100) > chatCfg_.pmReplyChancePct) return;
	std::string group = classifyReplyTrigger(text, /*isPm=*/true);
	convo.exchanges++;
	if (group == "bye" || convo.exchanges >= chatCfg_.convoExchangeCap) {
		const bool capped = group != "bye";
		convo.ended = true;
		if (replyCatalog_.count("bye") > 0) group = "bye";
		g_logger().info("[CHAT_CONVO] end bot='{}' player={} exchanges={} reason={} pm=1",
			bot->name, playerId, convo.exchanges, capped ? "cap" : "bye");
	}

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
		// #105 anti-repeat: inside a thread the owner does not say one of its last few lines again
		// ("you just said that"). A handful of re-rolls; a tiny catalog may still repeat.
		auto convoIt = conversations_.find(reply.playerId);
		Conversation* convo = (convoIt != conversations_.end() && convoIt->second.ownerGuid == reply.botGuid)
			? &convoIt->second : nullptr;
		uint32_t lineIdx = 0;
		for (int attempt = 0; attempt < 6; ++attempt) {
			lineIdx = static_cast<uint32_t>(uniform_random(0, static_cast<int32_t>(catIt->second.size()) - 1));
			if (!convo) break;
			const size_t h = std::hash<std::string> {}(catIt->second[lineIdx]);
			if (std::find(convo->recentLines.begin(), convo->recentLines.end(), h) == convo->recentLines.end()) break;
		}
		const std::string& line = catIt->second[lineIdx];

		// Re-resolve both parties — either may have moved on in the delay window.
		auto target = g_game().getCreatureByID(reply.playerId);
		auto targetPlayer = target ? target->getPlayer() : nullptr;
		if (!targetPlayer || targetPlayer->isRemoved()) continue;
		auto botPlayer = g_game().getPlayerByGUID(reply.botGuid);
		if (!botPlayer && reply.isPm) {
			botPlayer = getHibernatedBotPlayer(reply.botGuid);  // PMs work from hibernation
		}
		if (!botPlayer) {
			// Was silent: a local responder hibernated (left g_game) mid-delay. p5c_f122 Santiago
			// Darkfrost was queued and never said/dropped.
			if (!reply.isPm) {
				g_logger().info("[CHAT_REPLY] dropped guid={} group={} reason=gone", reply.botGuid, reply.group);
			}
			continue;
		}

		if (reply.isPm) {
			targetPlayer->sendPrivateMessage(botPlayer, TALKTYPE_PRIVATE_FROM, line);
		} else {
			if (botPlayer->isRemoved()) {  // hibernated mid-delay — can't local-say
				g_logger().info("[CHAT_REPLY] dropped guid={} group={} reason=hibernated", reply.botGuid, reply.group);
				continue;
			}
			// #122: a walking responder may have moved on during the typing delay. A say is heard
			// on the speaker's screen (8 x 6 tiles); past that the player would never see it, so
			// do not pretend it was an answer.
			const Position botAt = botPlayer->getPosition();
			const Position playerAt = targetPlayer->getPosition();
			if (!inSayEarshot(botAt, playerAt)) {
				g_logger().info("[CHAT_REPLY] dropped guid={} group={} reason=out-of-earshot bot=({},{},{}) player=({},{},{})",
					reply.botGuid, reply.group, botAt.x, botAt.y, botAt.z, playerAt.x, playerAt.y, playerAt.z);
				continue;
			}
			g_game().internalCreatureSay(botPlayer, TALKTYPE_SAY, line, /*ghostMode=*/false);
		}
		// #122: always-on server truth for replies (they are rare: one per player say at most).
		// bot_chat_emissions has been dark since 2026-06-22 by design -- botTelemetryEnabled
		// defaults false (faaf0d585, public-distribution prep) -- so acceptance reads this line.
		// #105: owner=1 when the speaker owns the player's thread -- the acceptance invariant
		// (a local reply from a non-owner, owner=0, would mean another bot jumped in).
		const bool isOwner = convo != nullptr;
		if (convo && chatCfg_.convoAntiRepeat > 0) {
			convo->recentLines.push_back(std::hash<std::string> {}(line));
			if (convo->recentLines.size() > static_cast<size_t>(chatCfg_.convoAntiRepeat)) {
				convo->recentLines.erase(convo->recentLines.begin());
			}
		}
		g_logger().info("[CHAT_REPLY] said bot='{}' pm={} group={} owner={} convo={}", botPlayer->getName(),
			reply.isPm ? 1 : 0, reply.group, isOwner ? 1 : 0, convo ? convo->exchanges : 0);

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
