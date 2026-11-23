/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (C) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 */

// ============================================================================
// bot_verb.cpp — #91 / #92 / #93 / #94: player-facing bot command verbs.
//
// A player commands bots by PM ("follow"), in the party channel ("@ek stay", "heal me") or from
// the OTClient panel (game_botcontrol, ExtendedOpcode 120 -> data/scripts/creaturescripts/
// bot_control_opcode.lua -> `_global verb opcode ...`). All three entries end in handleVerb,
// so the parser, the addressing, the security ladder, the rate limit and the telemetry are one
// piece of code.
//
//   text     := [#w|#p|#g]* [@addr[,addr...]]* verb [args]
//   addr     := all | ek|knight|tank | ed|druid|heal|healer | ms|sorc|sorcerer | rp|paladin |
//               dps | <min>-<max> | <min>+ | <name>   (name: '_' = space, a prefix of 3+ letters
//               or the first name matches)
//   reply    := the channel the verb came from (PM -> whisper, party -> party channel, panel ->
//               the panel), or #w / #p / #g to override (guild falls back to whisper when the bot
//               is not in the speaker's guild).
//
// Security ladder (#93) — one function, securityLevel(), used by every verb:
//   STRANGER  help who stats lfg wts invite(join) leave summon
//   PARTY     follow stay attack flee come heal hunt loot status     (speaker in the bot's party)
//   LEADER    kick                                                   (the human who leads it)
//   GM        cmd <any /cavebot subcommand>                          (gamemaster+ or access group)
// A red/black-skulled speaker is refused everything below GM ("murderer"). A refusal always says
// why. Rate limit: kVerbBurst commands per kVerbWindowMs per speaker, one "slow down" reply.
//
// Behaviour lives in applyPartyOrder (called from doPartyFollow): stay/flee/come/attack/hunt/heal
// are standing orders on a human-led party member (s_partyOrder, erased in exitPartyMode). The
// virtual twin is the empty set by construction: a human-led party member never hibernates while
// its leader is near, and the stranger verbs that DO work from hibernation (who, stats, lfg, wts)
// only read BotState.
//
// Telemetry (always on, one line per bot per verb — verbs are rare):
//   [BOT_VERB] from='<speaker>' src=<pm|party|opcode> verb=<v> target='<bot|*>' level=<l> need=<l>
//              result=<ok|denied:<why>|...> reply='<text>'
//   [BOT_VERB] apply bot='<bot>' mode=<m> tgt='<name>' ...   (an order changing what the bot does,
//              at most every 5 s per bot)
// ============================================================================

#include "creatures/players/bot/bot_engine_impl.hpp"

namespace {

	enum VerbLevel : uint8_t { VL_DENY = 0, VL_STRANGER = 1, VL_PARTY = 2, VL_LEADER = 3, VL_GM = 4 };

	const char* verbLevelName(uint8_t l) {
		switch (l) {
			case VL_STRANGER: return "stranger";
			case VL_PARTY: return "party";
			case VL_LEADER: return "leader";
			case VL_GM: return "gm";
			default: return "deny";
		}
	}

	struct VerbDef {
		const char* name;
		uint8_t level;
		bool perBot; // false = answered once (help, summon)
	};

	constexpr VerbDef kVerbs[] = {
		{ "help", VL_STRANGER, false },  { "who", VL_STRANGER, true },    { "stats", VL_STRANGER, true },
		{ "lfg", VL_STRANGER, true },    { "wts", VL_STRANGER, true },    { "invite", VL_STRANGER, true },
		{ "leave", VL_STRANGER, true },  { "summon", VL_STRANGER, false },
		{ "follow", VL_PARTY, true },    { "stay", VL_PARTY, true },      { "attack", VL_PARTY, true },
		{ "flee", VL_PARTY, true },      { "come", VL_PARTY, true },      { "heal", VL_PARTY, true },
		{ "hunt", VL_PARTY, true },      { "loot", VL_PARTY, true },      { "status", VL_PARTY, true },
		{ "kick", VL_LEADER, true },
		{ "cmd", VL_GM, true },
	};

	constexpr std::pair<const char*, const char*> kAliases[] = {
		{ "join", "invite" }, { "hold", "stay" }, { "wait", "stay" }, { "f", "follow" },
		{ "run", "flee" },     { "retreat", "flee" }, { "here", "come" }, { "dismiss", "kick" },
		{ "info", "stats" },   { "state", "status" }, { "commands", "help" }, { "atk", "attack" },
	};

	constexpr int64_t kVerbWindowMs = 10000;
	constexpr size_t kVerbBurst = 6;
	constexpr int64_t kListMinGapMs = 300;
	constexpr int64_t kFleeMs = 20000;
	constexpr int64_t kComeMs = 8000;
	constexpr int64_t kHealOrderMs = 30000;
	constexpr int64_t kApplyLogGapMs = 5000;

	std::string lower(std::string s) {
		std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	}

	std::vector<std::string> splitWs(const std::string& s) {
		std::vector<std::string> out;
		std::istringstream is(s);
		std::string t;
		while (is >> t) out.push_back(t);
		return out;
	}

	int32_t cheb(const Position& a, const Position& b) {
		return std::max(std::abs(static_cast<int32_t>(a.x) - static_cast<int32_t>(b.x)),
			std::abs(static_cast<int32_t>(a.y) - static_cast<int32_t>(b.y)));
	}

	int32_t pct(int64_t v, int64_t max) {
		return max > 0 ? static_cast<int32_t>(v * 100 / max) : 0;
	}

	const VerbDef* findVerb(const std::string& v) {
		std::string name = v;
		for (const auto& [a, c] : kAliases) {
			if (name == a) { name = c; break; }
		}
		for (const auto& d : kVerbs) {
			if (name == d.name) return &d;
		}
		return nullptr;
	}

	struct ParsedVerb {
		const VerbDef* def = nullptr;
		std::string args;            // original case
		std::string replyChan;       // "" = default for the source, else w / p / g
		std::vector<std::string> addr; // lowercase
	};

	bool parseVerbText(const std::string& text, ParsedVerb& out) {
		const auto toks = splitWs(text);
		size_t i = 0;
		for (; i < toks.size(); ++i) {
			const std::string& t = toks[i];
			if (t.size() >= 2 && t[0] == '#') {
				const std::string c = lower(t.substr(1));
				if (c == "w" || c == "whisper") out.replyChan = "w";
				else if (c == "p" || c == "party") out.replyChan = "p";
				else if (c == "g" || c == "guild") out.replyChan = "g";
				else return false;
				continue;
			}
			if (t.size() >= 2 && t[0] == '@') {
				std::string rest = lower(t.substr(1));
				size_t start = 0;
				while (start <= rest.size()) {
					size_t comma = rest.find(',', start);
					std::string a = rest.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
					if (!a.empty()) out.addr.push_back(a);
					if (comma == std::string::npos) break;
					start = comma + 1;
				}
				continue;
			}
			break;
		}
		if (i >= toks.size()) return false;
		out.def = findVerb(lower(toks[i]));
		if (!out.def) return false;
		std::string args;
		for (size_t j = i + 1; j < toks.size(); ++j) {
			if (!args.empty()) args += ' ';
			args += toks[j];
		}
		out.args = args;
		return true;
	}

	bool sameParty(const std::shared_ptr<Player>& a, const std::shared_ptr<Player>& b) {
		if (!a || !b) return false;
		const auto pa = a->getParty();
		return pa && pa == b->getParty();
	}

} // namespace

// ---------------------------------------------------------------------------
// Entry from executeCommand: `verb <src> <speakerCreatureId> <text>`
// ---------------------------------------------------------------------------
std::string BotEngine::verbCommand(const std::string& args) {
	const auto sp1 = args.find(' ');
	if (sp1 == std::string::npos) return "usage: verb <pm:<guid>|party|opcode> <speakerCreatureId> <text>";
	const std::string src = args.substr(0, sp1);
	const auto sp2 = args.find(' ', sp1 + 1);
	uint32_t speakerId = 0;
	try {
		speakerId = static_cast<uint32_t>(std::stoul(args.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1)));
	} catch (...) {
		return "verb: bad speaker id";
	}
	const std::string text = sp2 == std::string::npos ? std::string() : args.substr(sp2 + 1);
	const auto creature = g_game().getCreatureByID(speakerId);
	const auto speaker = creature ? creature->getPlayer() : nullptr;
	if (!speaker || speaker->isBotPlayer()) {
		return src == "opcode" ? std::string() : "verb: speaker not found";
	}

	if (src == "opcode") {
		// Panel protocol (client -> server, plain text): "list" | "cmd <verb text>".
		// Server -> client: JSON produced here (t=list | t=res | t=err); the Lua handler relays it.
		if (text == "list") {
			auto& rate = s_verbRate[speaker->getID()];
			const int64_t now = OTSYS_TIME();
			if (now - rate.lastListMs < kListMinGapMs) return std::string();
			rate.lastListMs = now;
			return verbStateJson(speaker);
		}
		if (text.rfind("cmd ", 0) == 0) {
			std::string json;
			if (!handleVerb("opcode", speaker, 0, text.substr(4), &json)) {
				nlohmann::json j = { { "t", "res" }, { "ok", false }, { "text", "Unknown command. Try: help" }, { "replies", nlohmann::json::array() } };
				g_logger().info("[BOT_VERB] from='{}' src=opcode verb=? target='*' level=- need=- result=unknown reply=''",
					speaker->getName());
				return j.dump();
			}
			return json;
		}
		nlohmann::json j = { { "t", "err" }, { "text", "bad request" } };
		return j.dump();
	}
	if (src == "party") {
		return handleVerb("party", speaker, 0, text, nullptr) ? "handled" : "chat";
	}
	if (src.rfind("pm:", 0) == 0) {
		uint32_t guid = 0;
		try { guid = static_cast<uint32_t>(std::stoul(src.substr(3))); } catch (...) { return "verb: bad pm guid"; }
		return handleVerb(src, speaker, guid, text, nullptr) ? "handled" : "chat";
	}
	return "verb: unknown source";
}

// ---------------------------------------------------------------------------
// Panel state: the speaker's party bots + the bots on the speaker's screen.
// ---------------------------------------------------------------------------
std::string BotEngine::verbStateJson(const std::shared_ptr<Player>& speaker) {
	nlohmann::json bots = nlohmann::json::array();
	std::unordered_set<uint32_t> seen;
	const Position me = speaker->getPosition();
	const auto party = speaker->getParty();

	auto addBot = [&](const std::shared_ptr<Player>& bp, bool inParty) {
		if (!bp || !bp->isBotPlayer() || bp->isRemoved()) return;
		const uint32_t guid = bp->getGUID();
		if (!seen.insert(guid).second) return;
		BotState* bs = getBotState(guid);
		if (!bs) return;
		const auto orderIt = s_partyOrder.find(guid);
		const auto tgt = bp->getAttackedCreature();
		const Position bpos = bp->getPosition();
		bots.push_back({
			{ "name", bp->getName() },
			{ "guid", guid },
			{ "cid", bp->getID() },
			{ "voc", vocShortName(getBaseVocation(bs->vocationId)) },
			{ "level", bp->getLevel() },
			{ "hp", pct(bp->getHealth(), bp->getMaxHealth()) },
			{ "mp", pct(bp->getMana(), bp->getMaxMana()) },
			{ "state", botStateName(bs->state) },
			{ "order", orderIt != s_partyOrder.end() ? partyOrderModeName(orderIt->second.mode) : (inParty ? "follow" : "") },
			{ "target", tgt ? tgt->getName() : "" },
			{ "party", inParty },
			{ "dist", bpos.z == me.z ? cheb(bpos, me) : 99 },
		});
	};

	if (party) {
		if (const auto leader = party->getLeader()) addBot(leader, true);
		for (const auto& m : party->getMembers()) addBot(m, true);
	}
	for (const auto& c : Spectators().find<Player>(me, false, 8, 8, 6, 6)) {
		if (const auto p = c ? c->getPlayer() : nullptr) {
			addBot(p, sameParty(p, speaker));
		}
	}
	nlohmann::json j = {
		{ "t", "list" },
		{ "me", speaker->getName() },
		{ "inParty", party != nullptr },
		{ "leader", party && party->getLeader() == speaker },
		{ "bots", bots },
	};
	return j.dump();
}

// ---------------------------------------------------------------------------
// The verb layer.
// ---------------------------------------------------------------------------
bool BotEngine::handleVerb(const std::string& src, const std::shared_ptr<Player>& speaker, uint32_t pmBotGuid,
	const std::string& text, std::string* jsonOut) {
	if (!speaker || speaker->isBotPlayer()) return false;
	ParsedVerb pv;
	if (!parseVerbText(text, pv)) return false;
	const bool isPm = src.rfind("pm:", 0) == 0;
	const bool isOpcode = src == "opcode";
	const std::string srcTag = isPm ? "pm" : src;
	const std::string verb = pv.def->name;
	const int64_t now = OTSYS_TIME();

	// ---- candidate bots ----
	std::vector<BotState*> cands;
	std::unordered_set<uint32_t> candSet;
	auto addCand = [&](uint32_t guid) {
		if (!candSet.insert(guid).second) return;
		if (BotState* b = getBotState(guid)) cands.push_back(b);
	};
	if (isPm) {
		addCand(pmBotGuid);
	} else {
		if (const auto party = speaker->getParty()) {
			if (const auto l = party->getLeader(); l && l->isBotPlayer()) addCand(l->getGUID());
			for (const auto& m : party->getMembers()) {
				if (m && m->isBotPlayer()) addCand(m->getGUID());
			}
		}
		// The panel may address a bot outside the party by name (invite / who on a nearby bot).
		if (isOpcode) {
			for (const auto& a : pv.addr) {
				if (a.size() < 3 || a == "all" || std::isdigit(static_cast<unsigned char>(a[0]))) continue;
				std::string want = a;
				std::ranges::replace(want, '_', ' ');
				for (auto& b : bots_) {
					if (lower(b.name) == want) addCand(b.guid);
				}
			}
		}
	}

	// ---- addressing filter ----
	auto matches = [&](BotState& b) {
		if (pv.addr.empty()) return true;
		const uint8_t base = getBaseVocation(b.vocationId);
		const auto bp = b.getPlayer();
		const uint32_t lvl = bp ? bp->getLevel() : b.cachedLevel;
		const std::string lname = lower(b.name);
		for (const auto& a : pv.addr) {
			if (a == "all" || a == "everyone") return true;
			if ((a == "ek" || a == "knight" || a == "knights" || a == "tank") && base == 4) return true;
			if ((a == "ed" || a == "druid" || a == "druids" || a == "heal" || a == "healer" || a == "healers") && base == 2) return true;
			if ((a == "ms" || a == "sorc" || a == "sorcerer" || a == "sorcerers") && base == 1) return true;
			if ((a == "rp" || a == "pally" || a == "paladin" || a == "paladins") && base == 3) return true;
			if ((a == "mk" || a == "em" || a == "monk" || a == "monks") && base == BOT_BASE_VOC_MONK) return true;
			if (a == "dps" && (base == 1 || base == 3 || base == BOT_BASE_VOC_MONK)) return true;
			if (!a.empty() && std::isdigit(static_cast<unsigned char>(a[0]))) {
				uint32_t lo = 0, hi = UINT32_MAX;
				const auto dash = a.find('-');
				try {
					if (a.back() == '+') {
						lo = static_cast<uint32_t>(std::stoul(a.substr(0, a.size() - 1)));
					} else if (dash != std::string::npos) {
						lo = static_cast<uint32_t>(std::stoul(a.substr(0, dash)));
						hi = static_cast<uint32_t>(std::stoul(a.substr(dash + 1)));
					} else {
						lo = hi = static_cast<uint32_t>(std::stoul(a));
					}
				} catch (...) { continue; }
				if (lvl >= lo && lvl <= hi) return true;
				continue;
			}
			std::string want = a;
			std::ranges::replace(want, '_', ' ');
			if (lname == want) return true;
			if (want.size() >= 3 && lname.rfind(want, 0) == 0) return true;
		}
		return false;
	};
	std::vector<BotState*> targets;
	for (BotState* b : cands) {
		if (matches(*b)) targets.push_back(b);
	}

	nlohmann::json replies = nlohmann::json::array();
	const std::string defaultChan = isPm ? "w" : (isOpcode ? "o" : "p");
	const std::string chan = pv.replyChan.empty() ? defaultChan : pv.replyChan;

	auto botBody = [&](BotState& b) -> std::shared_ptr<Player> {
		if (auto p = b.getPlayer()) return p;
		return getHibernatedBotPlayer(b.guid);
	};

	auto sendReply = [&](BotState& b, const std::string& msg) {
		const auto bp = botBody(b);
		if (!bp) return;
		std::string c = chan;
		if (c == "o") return; // the panel gets it in the JSON
		if (c == "g") {
			const auto gs = speaker->getGuild();
			const auto gb = bp->getGuild();
			const auto ch = (gs && gb && gs == gb) ? g_chat().getChannel(speaker, CHANNEL_GUILD) : nullptr;
			if (ch) {
				for (const auto& [uid, u] : ch->getUsers()) {
					if (u) u->sendToChannel(bp, TALKTYPE_CHANNEL_Y, msg, CHANNEL_GUILD);
				}
				return;
			}
			c = "w";
		}
		if (c == "p") {
			const auto ch = speaker->getParty() ? g_chat().getChannel(speaker, CHANNEL_PARTY) : nullptr;
			if (ch) {
				for (const auto& [uid, u] : ch->getUsers()) {
					if (u) u->sendToChannel(bp, TALKTYPE_CHANNEL_Y, msg, CHANNEL_PARTY);
				}
				return;
			}
			c = "w";
		}
		speaker->sendPrivateMessage(bp, TALKTYPE_PRIVATE_FROM, msg);
	};

	auto logLine = [&](const std::string& target, uint8_t level, const std::string& result, const std::string& msg) {
		g_logger().info("[BOT_VERB] from='{}' src={} verb={} target='{}' level={} need={} result={} reply='{}'",
			speaker->getName(), srcTag, verb, target, verbLevelName(level), verbLevelName(pv.def->level), result, msg);
	};

	auto finishJson = [&](bool ok, const std::string& summary) {
		if (!jsonOut) return;
		nlohmann::json j = { { "t", "res" }, { "verb", verb }, { "ok", ok }, { "text", summary }, { "replies", replies } };
		*jsonOut = j.dump();
	};

	// Nothing addressed: a PM to one bot with an address that is someone else ("@ek follow" to a
	// druid) is consumed silently; in party chat there is nobody to answer, so it is just chat.
	if (targets.empty() && pv.def->perBot) {
		if (isPm) {
			logLine(pmBotGuid ? std::to_string(pmBotGuid) : "*", VL_DENY, "not_addressed", "");
			return true;
		}
		if (!isOpcode) return false;
		logLine("*", VL_DENY, "no_bot", "");
		finishJson(false, "No bot matched that address.");
		return true;
	}

	// ---- rate limit (per speaker, per command) ----
	{
		auto& rate = s_verbRate[speaker->getID()];
		std::erase_if(rate.times, [&](int64_t t) { return now - t > kVerbWindowMs; });
		if (rate.times.size() >= kVerbBurst) {
			const std::string msg = "Slow down, one thing at a time.";
			if (now >= rate.warnedUntilMs) {
				rate.warnedUntilMs = now + kVerbWindowMs;
				if (!targets.empty()) sendReply(*targets.front(), msg);
				else speaker->sendTextMessage(MESSAGE_STATUS, msg);
			}
			logLine("*", VL_DENY, "rate_limited", msg);
			finishJson(false, msg);
			return true;
		}
		rate.times.push_back(now);
		if (s_verbRate.size() > 512) {
			std::erase_if(s_verbRate, [&](const auto& kv) {
				return kv.second.times.empty() && now - kv.second.lastListMs > 60000;
			});
		}
	}

	// ---- security ladder (#93) ----
	const bool speakerGm = speaker->getAccountType() >= AccountType::ACCOUNT_TYPE_GAMEMASTER || speaker->isAccessPlayer();
	const bool murderer = speaker->getSkull() == SKULL_RED || speaker->getSkull() == SKULL_BLACK;
	auto securityLevel = [&](BotState& b, const std::shared_ptr<Player>& bp) -> uint8_t {
		if (speakerGm) return VL_GM;
		if (murderer) return VL_DENY;
		if (bp && !bp->isRemoved() && sameParty(bp, speaker)) {
			const auto lIt = s_partyLeaderId.find(b.guid);
			const bool leads = (lIt != s_partyLeaderId.end() && lIt->second == speaker->getID())
				|| bp->getParty()->getLeader() == speaker;
			return leads ? VL_LEADER : VL_PARTY;
		}
		return VL_STRANGER;
	};
	auto denyText = [&](uint8_t have) -> std::pair<std::string, std::string> {
		if (have == VL_DENY) return { "denied:murderer", "I don't deal with murderers." };
		switch (pv.def->level) {
			case VL_PARTY: return { "denied:not_in_party", "Sorry, I only take orders from my party (not in my party)." };
			case VL_LEADER: return { "denied:not_leader", "Only the party leader can ask me that." };
			default: return { "denied:gm_only", "Only a gamemaster can do that." };
		}
	};

	auto helpText = [&](uint8_t have) {
		std::string s = "Commands: who, stats, lfg, wts, invite, leave, summon <ek,ed..>";
		if (have >= VL_PARTY) s += " | party: follow, stay, attack [name], flee, come, heal [me], hunt, loot, status";
		if (have >= VL_LEADER) s += " | leader: kick";
		if (have >= VL_GM) s += " | gm: cmd <cavebot subcommand>";
		s += " | address: @all @ek @ed @ms @rp @100-200 @Name | reply: #w #p #g";
		return s;
	};

	// ---- global verbs (answered once) ----
	if (!pv.def->perBot) {
		BotState* first = targets.empty() ? nullptr : targets.front();
		const uint8_t have = first ? securityLevel(*first, botBody(*first)) : (speakerGm ? VL_GM : (murderer ? VL_DENY : VL_STRANGER));
		std::string msg, result = "ok";
		bool ok = true;
		if (have == VL_DENY) {
			std::tie(result, msg) = denyText(have);
			ok = false;
		} else if (verb == "help") {
			msg = helpText(have);
		} else { // summon <vocs>: the player-facing /cavebot party create (open to all players)
			std::string vocs = pv.args.empty() ? std::string("ek,ed") : lower(pv.args);
			msg = executeCommand("*", fmt::format("party create {} {}", speaker->getID(), vocs));
			ok = msg.rfind("Party formed", 0) == 0;
			result = ok ? "ok" : "failed";
		}
		if (first && verb == "help") {
			sendReply(*first, msg);
		} else if (chan != "o") {
			speaker->sendTextMessage(MESSAGE_STATUS, msg);
		}
		replies.push_back({ { "bot", first ? first->name : "" }, { "ok", ok }, { "msg", msg }, { "result", result } });
		logLine(first ? first->name : "*", have, result, msg);
		finishJson(ok, msg);
		return true;
	}

	// ---- per-bot verbs ----
	uint32_t nOk = 0;
	for (BotState* bptr : targets) {
		BotState& b = *bptr;
		const auto bp = botBody(b);
		const bool awake = bp && !bp->isRemoved() && b.getPlayer() == bp;
		const uint8_t have = securityLevel(b, bp);
		std::string msg;
		std::string result = "ok";

		if (have == VL_DENY || have < pv.def->level) {
			std::tie(result, msg) = denyText(have);
		} else if (verb == "who") {
			const uint32_t lvl = bp ? bp->getLevel() : b.cachedLevel;
			std::string doing;
			if (s_partyLeaderId.count(b.guid)) doing = "partying";
			else if (b.huntScriptId > 0) {
				const std::string hn = getHuntScriptName(b, huntScripts_);
				doing = hn.empty() ? "hunting" : "hunting (" + hn + ")";
			} else doing = "around " + (b.townName.empty() ? std::string("town") : b.townName);
			msg = fmt::format("{}, level {} {}, {}.", b.name, lvl, vocShortName(getBaseVocation(b.vocationId)), doing);
		} else if (verb == "stats") {
			if (bp) {
				msg = fmt::format("Level {} {} | HP {}/{} | MP {}/{}", bp->getLevel(), vocShortName(getBaseVocation(b.vocationId)),
					bp->getHealth(), bp->getMaxHealth(), bp->getMana(), bp->getMaxMana());
			} else {
				msg = fmt::format("Level {} {}", b.cachedLevel, vocShortName(getBaseVocation(b.vocationId)));
			}
		} else if (verb == "lfg") {
			if (awake && bp->getParty()) { msg = "Already in a party."; result = "busy:party"; }
			else if (s_botToPartyHunt.count(b.guid)) { msg = "Busy with my team right now."; result = "busy:party_hunt"; }
			else if (!awake) { msg = "Not right now, maybe later."; result = "busy:far"; }
			else msg = fmt::format("Sure, invite me! Level {} {}.", bp->getLevel(), vocShortName(getBaseVocation(b.vocationId)));
		} else if (verb == "wts") {
			msg = "Nothing for sale right now, check the market.";
		} else if (verb == "invite") {
			const auto sParty = speaker->getParty();
			if (!awake) { msg = "I'm busy right now, try again later."; result = "refused:hibernated"; }
			else if (bp->getParty()) {
				msg = sameParty(bp, speaker) ? "I'm already in your party." : "I'm already in a party.";
				result = "refused:in_party";
			} else if (s_botToPartyHunt.count(b.guid)) { msg = "I'm hunting with my team."; result = "refused:party_hunt"; }
			else if (s_rvMember.count(b.guid)) { msg = "I'm already on my way to someone."; result = "refused:assembling"; }
			else if (s_debugPinned.count(b.guid)) { msg = "I can't right now."; result = "refused:pinned"; }
			else if (!inviteCfg_.enable) { msg = "I can't party right now."; result = "refused:invites_off"; }
			else if (sParty && sParty->getLeader() != speaker) { msg = "Ask your party leader to invite me."; result = "refused:not_leader"; }
			else {
				g_game().playerInviteToParty(speaker->getID(), bp->getID());
				if (speaker->isInviting(bp)) {
					msg = "Sure, I'll join you in a moment.";
				} else {
					msg = "That didn't work, try inviting me yourself.";
					result = "failed:invite";
				}
			}
		} else if (verb == "leave" || verb == "kick") {
			if (awake && sameParty(bp, speaker) && s_partyLeaderId.count(b.guid)) {
				msg = verb == "kick" ? "Okay, leaving the party. Bye!" : "Leaving, see you around!";
				sendReply(b, msg); // before the exit: the party channel is the speaker's
				exitPartyMode(b);
				result = "ok:left";
				replies.push_back({ { "bot", b.name }, { "ok", true }, { "msg", msg }, { "result", result } });
				logLine(b.name, have, result, msg);
				nOk++;
				continue;
			}
			msg = "I'm not in your party.";
			result = "refused:not_in_party";
		} else if (verb == "cmd") {
			if (pv.args.empty()) { msg = "cmd <cavebot subcommand>"; result = "usage"; }
			else {
				std::string r = executeCommand(b.name, pv.args);
				if (const auto nl = r.find('\n'); nl != std::string::npos) r = r.substr(0, nl);
				if (r.size() > 240) r = r.substr(0, 240) + "...";
				msg = r;
			}
		} else {
			// ---- PARTY verbs: need a human-led party member, awake ----
			if (!awake || !s_partyLeaderId.count(b.guid) || !bp->getParty()) {
				msg = "I'm not in a party, invite me first.";
				result = "refused:no_party";
			} else if (verb == "status") {
				const auto it = s_partyOrder.find(b.guid);
				const auto tgt = bp->getAttackedCreature();
				msg = fmt::format("{} | order {} | HP {}% MP {}% | target {}", botStateName(b.state),
					it != s_partyOrder.end() ? partyOrderModeName(it->second.mode) : "follow",
					pct(bp->getHealth(), bp->getMaxHealth()), pct(bp->getMana(), bp->getMaxMana()),
					tgt ? tgt->getName() : "none");
			} else if (verb == "loot") {
				// By design (bot_hunt.cpp lootGatePasses): a bot never loots while a human is in its
				// party, because the human uses the corpse highlight as their own loot marker.
				msg = "I'll leave the loot to you while we party.";
				result = "by_design";
			} else {
				PartyOrder& o = s_partyOrder[b.guid];
				o.issuerId = speaker->getID();
				o.lastApplyLogMs = 0;
				if (verb == "follow") {
					o.mode = PartyOrderMode::FOLLOW;
					o.attackTargetId = 0;
					o.fleeUntilMs = 0;
					msg = "Right behind you.";
				} else if (verb == "stay") {
					o.mode = PartyOrderMode::STAY;
					o.stayPos = bp->getPosition();
					if (!bp->listWalkDir.empty()) {
						bp->listWalkDir.clear();
						bp->stopEventWalk();
					}
					bp->setFollowCreature(nullptr);
					msg = "Holding here.";
				} else if (verb == "flee" || verb == "come") {
					o.mode = PartyOrderMode::FLEE;
					o.fleeUntilMs = now + (verb == "flee" ? kFleeMs : kComeMs);
					o.attackTargetId = 0;
					bp->setAttackedCreature(nullptr);
					msg = verb == "flee" ? "Falling back to you!" : "On my way.";
				} else if (verb == "hunt") {
					o.mode = PartyOrderMode::HUNT;
					o.fleeUntilMs = 0;
					msg = "Let's hunt, I'll take anything near you.";
				} else if (verb == "attack") {
					const std::string a = lower(pv.args);
					std::shared_ptr<Creature> t;
					if (a.empty() || a == "my target" || a == "target" || a == "mine" || a == "it") {
						t = speaker->getAttackedCreature();
					} else {
						int32_t best = INT32_MAX;
						for (const auto& c : Spectators().find<Creature>(speaker->getPosition(), false, 8, 8, 6, 6)) {
							if (!c || c->isRemoved() || c->getHealth() <= 0 || c->getNpc()) continue;
							const std::string cn = lower(c->getName());
							if (cn != a && cn.rfind(a, 0) != 0) continue;
							const int32_t d = cheb(c->getPosition(), speaker->getPosition()) + (c->getMonster() ? 0 : 100);
							if (d < best) { best = d; t = c; }
						}
					}
					if (!t || t->isRemoved() || t->getHealth() <= 0) {
						msg = a.empty() ? "You aren't attacking anything." : "I can't see that.";
						result = "refused:no_target";
					} else if (t == bp || t == speaker || (t->getPlayer() && (speaker->isPartner(t->getPlayer()) || bp->isPartner(t->getPlayer())))) {
						msg = "I won't attack my own party.";
						result = "refused:friendly";
					} else if (t->getPlayer() && speaker->getAttackedCreature() != t) {
						msg = "I only fight players you are fighting.";
						result = "refused:pvp";
					} else if (t->getPosition().z != bp->getPosition().z || cheb(t->getPosition(), bp->getPosition()) > 10) {
						msg = "That's too far from me.";
						result = "refused:far";
					} else {
						o.attackTargetId = t->getID();
						if (o.mode == PartyOrderMode::FLEE) o.mode = PartyOrderMode::FOLLOW;
						msg = fmt::format("Attacking {}!", t->getName());
						result = "ok:" + t->getName();
					}
				} else if (verb == "heal") {
					std::shared_ptr<Player> ht = speaker;
					const std::string a = lower(pv.args);
					if (!a.empty() && a != "me" && a != "pls" && a != "please") {
						ht = nullptr;
						const auto party = bp->getParty();
						auto check = [&](const std::shared_ptr<Player>& m) {
							if (m && !ht && (lower(m->getName()) == a || lower(m->getName()).rfind(a, 0) == 0)) ht = m;
						};
						check(party->getLeader());
						for (const auto& m : party->getMembers()) check(m);
					}
					if (getBaseVocation(b.vocationId) != 2) {
						msg = "I can't heal others, ask a druid.";
						result = "refused:not_druid";
					} else if (!ht) {
						msg = "Who? I can't find them in the party.";
						result = "refused:no_target";
					} else {
						o.healTargetId = ht->getID();
						o.healUntilMs = now + kHealOrderMs;
						std::string why;
						if (tryHealFriend(b, bp, ht, "verb", &why)) {
							msg = ht == speaker ? "Exura sio on you!" : fmt::format("Exura sio on {}!", ht->getName());
							result = "ok:cast";
						} else {
							msg = why + " You're next.";
							result = "ok:queued";
						}
					}
				}
				castLog(b, fmt::format("VERB: {} from '{}' -> mode={} tgt={} heal={}", verb, speaker->getName(),
					partyOrderModeName(o.mode), o.attackTargetId, o.healTargetId));
				// A FOLLOW order with nothing else pending is the default: drop the entry.
				if (o.mode == PartyOrderMode::FOLLOW && o.attackTargetId == 0 && o.healTargetId == 0) {
					s_partyOrder.erase(b.guid);
				}
			}
		}

		const bool ok = result.rfind("ok", 0) == 0;
		if (ok) nOk++;
		sendReply(b, msg);
		replies.push_back({ { "bot", b.name }, { "ok", ok }, { "msg", msg }, { "result", result } });
		logLine(b.name, have, result, msg);
	}
	finishJson(nOk > 0, fmt::format("{}: {}/{} bot(s) ok", verb, nOk, targets.size()));
	return true;
}

bool BotEngine::tryHealFriend(BotState& bot, const std::shared_ptr<Player>& player,
	const std::shared_ptr<Player>& target, const char* why, std::string* failReason) {
	auto fail = [&](const char* r) {
		if (failReason) *failReason = r;
		return false;
	};
	if (!player || !target || target->isRemoved() || target->getHealth() <= 0) return fail("I can't reach you.");
	const auto& sio = g_spells().getInstantSpell("exura sio");
	if (!sio) return fail("I don't know that spell.");
	if (player->getLevel() < 18) return fail("I'm too low level for exura sio.");
	const Position tp = target->getPosition();
	if (tp.z != bot.currentPos.z || cheb(tp, bot.currentPos) > 7) return fail("You're too far away.");
	if (player->hasCondition(CONDITION_SPELLGROUPCOOLDOWN, sio->getGroup())
	    || player->hasCondition(CONDITION_SPELLCOOLDOWN, sio->getSpellId())) {
		return fail("On cooldown.");
	}
	if (player->getMana() < 120) return fail("Out of mana.");
	std::string words = "exura sio \"" + target->getName() + "\"";
	if (g_spells().playerSaySpell(player, words) == TALKACTION_BREAK) {
		player->saySpell(TALKTYPE_SAY, words, false);
		castLog(bot, fmt::format("PARTY: exura sio \"{}\" ({}, hp={}%)", target->getName(), why,
			pct(target->getHealth(), target->getMaxHealth())));
		g_logger().info("[BOT_VERB] apply bot='{}' mode=heal tgt='{}' why={} hp={}", bot.name, target->getName(), why,
			pct(target->getHealth(), target->getMaxHealth()));
		return true;
	}
	return fail("That didn't work.");
}

// ---------------------------------------------------------------------------
// Behaviour: the standing order, applied from doPartyFollow before the default targeting.
// ---------------------------------------------------------------------------
bool BotEngine::applyPartyOrder(BotState& bot, const std::shared_ptr<Player>& player, const std::shared_ptr<Party>& party,
	const std::shared_ptr<Player>& leader, int32_t leaderDist,
	const std::function<void(const std::shared_ptr<Creature>&)>& syncSecure) {
	const auto it = s_partyOrder.find(bot.guid);
	if (it == s_partyOrder.end()) return false;
	PartyOrder& o = it->second;
	const int64_t now = OTSYS_TIME();
	const uint8_t baseVoc = getBaseVocation(bot.vocationId);
	const Position leaderPos = leader->getPosition();

	auto applyLog = [&](const char* mode, const std::shared_ptr<Creature>& tgt, const std::string& extra) {
		if (now - o.lastApplyLogMs < kApplyLogGapMs) return;
		o.lastApplyLogMs = now;
		g_logger().info("[BOT_VERB] apply bot='{}' mode={} tgt='{}' pos=({},{},{}) leaderDist={}{}", bot.name, mode,
			tgt ? tgt->getName() : "", bot.currentPos.x, bot.currentPos.y, bot.currentPos.z, leaderDist, extra);
	};

	// Expiries.
	if (o.mode == PartyOrderMode::FLEE && now >= o.fleeUntilMs) {
		o.mode = PartyOrderMode::FOLLOW;
		g_logger().info("[BOT_VERB] expire bot='{}' mode=flee", bot.name);
	}
	if (o.healTargetId && now >= o.healUntilMs) {
		o.healTargetId = 0;
	}

	// Heal order (druids): the named player gets exura sio below 90 %, ahead of the party ladder.
	if (o.healTargetId && baseVoc == 2) {
		const auto c = g_game().getCreatureByID(o.healTargetId);
		const auto hp = c ? c->getPlayer() : nullptr;
		if (hp && hp->getHealth() > 0 && pct(hp->getHealth(), hp->getMaxHealth()) < 90) {
			tryHealFriend(bot, player, hp, "order", nullptr);
		}
	}

	// Forced target validity.
	std::shared_ptr<Creature> forced;
	if (o.attackTargetId) {
		forced = g_game().getCreatureByID(o.attackTargetId);
		if (!forced || forced->isRemoved() || forced->getHealth() <= 0 || forced->getPosition().z != bot.currentPos.z
		    || cheb(forced->getPosition(), leaderPos) > PARTY_LEASH_DIST + 3) {
			g_logger().info("[BOT_VERB] expire bot='{}' mode=attack reason={}", bot.name,
				(!forced || forced->isRemoved() || forced->getHealth() <= 0) ? "target_down" : "out_of_leash");
			o.attackTargetId = 0;
			forced = nullptr;
		}
	}

	if (o.mode == PartyOrderMode::FOLLOW && !forced) {
		if (o.healTargetId == 0) s_partyOrder.erase(it);
		return false; // nothing left to override: the default behaviour runs
	}

	if (o.mode == PartyOrderMode::FLEE) {
		if (player->getAttackedCreature()) player->setAttackedCreature(nullptr);
		syncSecure(nullptr);
		if (baseVoc == 2) doPartyHealing(bot, party, leader);
		applyLog("flee", nullptr, fmt::format(" remainMs={}", o.fleeUntilMs - now));
		partyFollowMove(bot, player, leader, leaderDist, 1);
		return true;
	}

	if (o.mode == PartyOrderMode::STAY) {
		if (!player->listWalkDir.empty()) {
			player->listWalkDir.clear();
			player->stopEventWalk();
		}
		player->setFollowCreature(nullptr);
		std::shared_ptr<Creature> tgt = forced;
		if (!tgt) {
			tgt = leader->getAttackedCreature();
			if (!tgt || tgt->getHealth() <= 0 || tgt->isRemoved()) tgt = pickEkTankTarget(player, leader);
		}
		const int32_t reach = baseVoc == 4 ? 1 : std::max<int32_t>(1, getAttackRange(baseVoc));
		if (tgt && (tgt->getPosition().z != bot.currentPos.z || cheb(tgt->getPosition(), bot.currentPos) > reach
		            || !g_game().map.isSightClear(bot.currentPos, tgt->getPosition(), true))) {
			tgt = nullptr; // a holding bot only fights what it can reach from its tile
		}
		syncSecure(tgt);
		if (tgt) {
			if (player->getAttackedCreature() != tgt) player->setAttackedCreature(tgt);
			castSpell(bot, tgt);
		} else if (player->getAttackedCreature()) {
			player->setAttackedCreature(nullptr);
		}
		if (baseVoc == 2) doPartyHealing(bot, party, leader);
		applyLog("stay", tgt, fmt::format(" stayAt=({},{},{})", o.stayPos.x, o.stayPos.y, o.stayPos.z));
		return true;
	}

	// FOLLOW with a forced target, or HUNT.
	std::shared_ptr<Creature> tgt = forced;
	if (!tgt && o.mode == PartyOrderMode::HUNT) {
		const auto lt = leader->getAttackedCreature();
		if (!lt || lt->getHealth() <= 0 || lt->isRemoved()) {
			int32_t best = INT32_MAX;
			for (const auto& m : Spectators().find<Monster>(leaderPos, false, 6, 6, 6, 6)) {
				const auto mon = m ? m->getMonster() : nullptr;
				if (!mon || mon->isRemoved() || mon->getHealth() <= 0) continue;
				if (mon->getPosition().z != bot.currentPos.z) continue;
				if (const auto master = mon->getMaster(); master && master->getPlayer()) continue; // a player's summon
				if (!g_game().map.isSightClear(bot.currentPos, mon->getPosition(), true)) continue;
				const int32_t d = cheb(mon->getPosition(), bot.currentPos);
				if (d < best) { best = d; tgt = mon; }
			}
		}
	}
	if (!tgt) return false; // HUNT with nothing around: the default follow + leader-target mirror

	syncSecure(tgt);
	if (player->getAttackedCreature() != tgt) player->setAttackedCreature(tgt);
	castSpell(bot, tgt);
	chaseTarget(bot, tgt);
	if (baseVoc == 2) doPartyHealing(bot, party, leader);
	applyLog(forced ? "attack" : "hunt", tgt, "");
	return true;
}
