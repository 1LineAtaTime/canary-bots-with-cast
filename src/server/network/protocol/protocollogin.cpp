/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "server/network/protocol/protocollogin.hpp"

#include "config/configmanager.hpp"
#include "server/network/message/outputmessage.hpp"
#include "game/scheduling/dispatcher.hpp"
#include "account/account.hpp"
#include "io/iologindata.hpp"
#include "creatures/players/management/ban.hpp"
#include "game/game.hpp"
#include "creatures/players/player.hpp"
#include "creatures/players/bot/bot_engine.hpp"
#include "core.hpp"
#include "enums/account_errors.hpp"

void ProtocolLogin::disconnectClient(const std::string &message) const {
	const auto output = OutputMessagePool::getOutputMessage();

	output->addByte(0x0B);
	output->addString(message);
	send(output);

	disconnect();
}

void ProtocolLogin::getCharacterList(const std::string &accountDescriptor, const std::string &password) const {
	// Cast viewer — return list of broadcasting players
	if (accountDescriptor == "@cast" || accountDescriptor == "@livestream") {
		auto output = OutputMessagePool::getOutputMessage();

		output->addByte(0x14); // MOTD
		output->addString(fmt::format("{}\nCast Viewer - Select a player to watch", g_game().getMotdNum()));

		output->addByte(0x28); // Session key
		output->addString(fmt::format("@cast\n{}", password));

		output->addByte(0x00); // char list header
		output->addByte(1); // 1 world
		output->addByte(0); // world id
		output->addString(g_configManager().getString(SERVER_NAME));
		output->addString(g_configManager().getString(IP));
		output->add<uint16_t>(g_configManager().getNumber(GAME_PORT));
		output->addByte(0);

		// Phase 6: toggle for whether bots appear in the cast viewer character list.
		// Set to false to revert to pre-hibernation behavior (only currently-broadcasting
		// in-world players). When true, ALL registered active bots appear in the list
		// regardless of hibernation state — selecting a hibernated bot wakes it on
		// connect (see castViewerLogin in protocolgame.cpp).
		static constexpr bool kShowBotsInCastList = true;

		std::vector<std::string> casters;
		if (kShowBotsInCastList) {
			// All registered active bots, regardless of hibernation/broadcasting state.
			// `bot.active` is set at registerBot and never cleared until unregisterBot,
			// so this is stable across hibernate/wake transitions — avoids the cast list
			// flicker observed during user testing.
			for (auto &name : g_botEngine().getActiveBotNames()) {
				casters.push_back(name);
			}
			// Plus any non-bot real players who voluntarily enabled broadcasting.
			for (const auto &[id, p] : g_game().getPlayers()) {
				if (p && !p->isBotPlayer() && p->isCastBroadcasting() && !p->isRemoved()) {
					casters.push_back(p->getName());
				}
			}
		} else {
			// Original behavior: only currently-broadcasting in-world players.
			for (const auto &[id, p] : g_game().getPlayers()) {
				if (p && p->isCastBroadcasting() && !p->isRemoved()) {
					casters.push_back(p->getName());
				}
			}
		}
		std::sort(casters.begin(), casters.end());
		g_logger().info("[Cast] Character list: {} broadcasting players found", casters.size());

		uint8_t size = std::min<size_t>(std::numeric_limits<uint8_t>::max(), casters.size());
		output->addByte(size);
		for (uint8_t i = 0; i < size; i++) {
			output->addByte(0); // world id
			output->addString(casters[i]);
		}

		// Footer (matches working livestream implementation)
		output->addByte(0);
		output->addByte(0);
		output->add<uint32_t>(0);
		output->add<uint16_t>(0);

		send(output);
		disconnect();
		return;
	}

	Account account(accountDescriptor);
	account.setProtocolCompat(oldProtocol);

	if (oldProtocol && !g_configManager().getBoolean(OLD_PROTOCOL)) {
		disconnectClient(fmt::format("Only protocol version {}.{} is allowed.", CLIENT_VERSION_UPPER, CLIENT_VERSION_LOWER));
		return;
	} else if (!oldProtocol) {
		disconnectClient(fmt::format("Only protocol version {}.{} or outdated 11.00 is allowed.", CLIENT_VERSION_UPPER, CLIENT_VERSION_LOWER));
		return;
	}

	if (account.load() != AccountErrors_t::Ok || !account.authenticate(password)) {
		std::ostringstream ss;
		ss << (oldProtocol ? "Username" : "Email") << " or password is not correct.";
		disconnectClient(ss.str());
		return;
	}

	auto output = OutputMessagePool::getOutputMessage();
	const std::string &motd = g_configManager().getString(SERVER_MOTD);
	if (!motd.empty()) {
		// Add MOTD
		output->addByte(0x14);

		std::ostringstream ss;
		ss << g_game().getMotdNum() << "\n"
		   << motd;
		output->addString(ss.str());
	}

	// Add session key
	output->addByte(0x28);
	output->addString(accountDescriptor + "\n" + password);

	// Add char list
	auto [players, result] = account.getAccountPlayers();
	if (AccountErrors_t::Ok != result) {
		g_logger().warn("Account[{}] failed to load players!", account.getID());
	}

	output->addByte(0x64);

	output->addByte(1); // number of worlds

	output->addByte(0); // world id
	output->addString(g_configManager().getString(SERVER_NAME));
	output->addString(g_configManager().getString(IP));

	output->add<uint16_t>(g_configManager().getNumber(GAME_PORT));

	output->addByte(0);

	uint8_t size = std::min<size_t>(std::numeric_limits<uint8_t>::max(), players.size());
	output->addByte(size);
	for (const auto &[name, deletion] : players) {
		output->addByte(0);
		output->addString(name);
	}

	// Get premium days, check is premium and get lastday
	output->addByte(account.getPremiumRemainingDays());
	output->addByte(account.getPremiumLastDay() > getTimeNow());
	output->add<uint32_t>(account.getPremiumLastDay());

	send(output);

	disconnect();
}

void ProtocolLogin::onRecvFirstMessage(NetworkMessage &msg) {
	if (g_game().getGameState() == GAME_STATE_SHUTDOWN) {
		disconnect();
		return;
	}

	msg.skipBytes(2); // client OS

	auto version = msg.get<uint16_t>();

	// Old protocol support
	oldProtocol = version == 1100;

	msg.skipBytes(17);
	/*
	 - Skipped bytes:
	 - 4 bytes: client version (971+)
	 - 12 bytes: dat, spr, pic signatures (4 bytes each)
	 - 1 byte: preview world(971+)
	 */

	if (!Protocol::RSA_decrypt(msg)) {
		g_logger().warn("[ProtocolLogin::onRecvFirstMessage] - RSA Decrypt Failed");
		disconnect();
		return;
	}

	std::array<uint32_t, 4> key = { msg.get<uint32_t>(), msg.get<uint32_t>(), msg.get<uint32_t>(), msg.get<uint32_t>() };
	enableXTEAEncryption();
	setXTEAKey(key.data());

	setChecksumMethod(CHECKSUM_METHOD_ADLER32);

	if (g_game().getGameState() == GAME_STATE_STARTUP) {
		disconnectClient("Gameworld is starting up. Please wait.");
		return;
	}

	if (g_game().getGameState() == GAME_STATE_MAINTAIN) {
		disconnectClient("Gameworld is under maintenance.\nPlease re-connect in a while.");
		return;
	}

	BanInfo banInfo;
	auto curConnection = getConnection();
	if (!curConnection) {
		return;
	}

	if (IOBan::isIpBanned(curConnection->getIP(), banInfo)) {
		if (banInfo.reason.empty()) {
			banInfo.reason = "(none)";
		}

		std::ostringstream ss;
		ss << "Your IP has been banned until " << formatDateShort(banInfo.expiresAt) << " by " << banInfo.bannedBy << ".\n\nReason specified:\n"
		   << banInfo.reason;
		disconnectClient(ss.str());
		return;
	}

	std::string accountDescriptor = msg.getString();
	g_logger().info("[ProtocolLogin] accountDescriptor='{}' len={}", accountDescriptor, accountDescriptor.length());

	// Cast viewer — skip password validation
	if (accountDescriptor == "@cast" || accountDescriptor == "@livestream") {
		std::string password = msg.getString(); // consume password field
		g_dispatcher().addEvent(
			[self = std::static_pointer_cast<ProtocolLogin>(shared_from_this()), accountDescriptor, password] {
				self->getCharacterList(accountDescriptor, password);
			},
			__FUNCTION__
		);
		return;
	}

	if (accountDescriptor.empty()) {
		std::ostringstream ss;
		ss << "Invalid " << (oldProtocol ? "username" : "email") << ".";
		disconnectClient(ss.str());
		return;
	}

	std::string password = msg.getString();
	if (password.empty()) {
		disconnectClient("Invalid password.");
		return;
	}

	g_dispatcher().addEvent(
		[self = std::static_pointer_cast<ProtocolLogin>(shared_from_this()), accountDescriptor, password] {
			self->getCharacterList(accountDescriptor, password);
		},
		__FUNCTION__
	);
}
