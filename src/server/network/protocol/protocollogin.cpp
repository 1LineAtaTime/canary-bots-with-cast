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
#include "server/network/protocol/protocol_port_utils.hpp"
#include "server/network/protocol/protocol_session_hint.hpp"
#include "server/network/protocol/transport_codec.hpp"
#include "game/scheduling/dispatcher.hpp"
#include "account/account.hpp"
#include "creatures/players/livestream/livestream.hpp"
#include "creatures/players/player.hpp"
#include "io/iologindata.hpp"
#include "creatures/players/management/ban.hpp"
#include "game/game.hpp"
#include "creatures/players/player.hpp"
#include "creatures/players/bot/bot_engine.hpp"
#include "core.hpp"
#include "enums/account_errors.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <array>
#endif

void ProtocolLogin::disconnectClient(const std::string &message) const {
	const auto output = OutputMessagePool::getOutputMessage();

	output->addByte(0x0B);
	output->addString(message);
	send(output);

	disconnect();
}

namespace {
	// FORK(cast) #153: who a bot camera can watch. Every registered active bot, awake or
	// hibernated (`bot.active` is set at registerBot and cleared only by unregisterBot, so the
	// list does not flicker with hibernation; selecting a hibernated bot wakes it, see
	// ProtocolGame::botCameraLogin), plus human players broadcasting through OUR /cast, only
	// when allowPlayerCast is on (#163; off by default, humans then use upstream's livestream).
	std::vector<std::string> collectBotCameraCasters() {
		std::vector<std::string> casters;
		if (isBotEngineReady()) {
			for (auto &name : g_botEngine().getActiveBotNames()) {
				casters.push_back(name);
			}
		}
		if (!g_configManager().getBoolean(ALLOW_PLAYER_CAST)) {
			std::sort(casters.begin(), casters.end());
			return casters;
		}
		for (const auto &[id, p] : g_game().getPlayers()) {
			if (p && !p->isBotPlayer() && p->isCastBroadcasting() && !p->isRemoved()) {
				casters.push_back(p->getName());
			}
		}
		std::sort(casters.begin(), casters.end());
		return casters;
	}
}

void ProtocolLogin::sendViewerCharacterList(const std::string &accountDescriptor, const std::string &password, const std::string &motd, const std::vector<std::string> &names) const {
	// Upstream's livestream list format (getLivestreamCharacterList), which upstream keeps current
	// for the 15.x clients. The session key carries the password tail: for "@cast" it is the
	// cast-operator secret, for "@livestream" a livestream password.
	auto output = OutputMessagePool::getOutputMessage();
	output->addByte(0x14);
	output->addString(motd);

	output->addByte(0x28);
	output->addString(fmt::format("{}\n{}", accountDescriptor, password));

	output->addByte(0x64);
	output->addByte(0x01); // worlds
	output->addByte(0x00);
	output->addString(g_configManager().getString(SERVER_NAME));
	output->addString(g_configManager().getString(IP));
	output->add<uint16_t>(g_configManager().getNumber(GAME_PORT));
	output->addByte(0x00);

	// uint8 count: past 255 the list is truncated (direct name login still works).
	const auto count = static_cast<uint8_t>(std::min<size_t>(std::numeric_limits<uint8_t>::max(), names.size()));
	output->addByte(count);
	for (size_t index = 0; index < count; ++index) {
		output->addByte(0x00);
		output->addString(names[index]);
	}

	output->addByte(0x00);
	output->addByte(0x00);
	output->add<uint32_t>(0x00);

	send(output);
	disconnect();
}

void ProtocolLogin::getCharacterList(const std::string &accountDescriptor, const std::string &password) const {
	// FORK(cast) #153: "@cast" lists the bot camera's casters. "@livestream" never gets here: it
	// goes to getLivestreamCharacterList (upstream's list plus the active bots).
	if (accountDescriptor == "@cast") {
		const auto casters = collectBotCameraCasters();
		g_logger().info("[Cast] Character list: {} broadcasting players found", casters.size());
		sendViewerCharacterList("@cast", password, fmt::format("{}\nCast Viewer - Select a player to watch", g_game().getMotdNum()), casters);
		return;
	}

	Account account(accountDescriptor);
	account.setProtocolCompat(oldProtocol);

	if (oldProtocol && !g_configManager().getBoolean(OLD_PROTOCOL)) {
		disconnectClient(ProtocolProfileRegistry::getUnsupportedClientProtocolMessage(false));
		return;
	}

	if (account.load() != AccountErrors_t::Ok || !account.authenticate(password)) {
		std::ostringstream ss;
		ss << (oldProtocol ? "Username" : "Email") << " or password is not correct.";
		disconnectClient(ss.str());
		return;
	}

	auto output = OutputMessagePool::getOutputMessage();
	const std::string sessionKey = accountDescriptor + "\n" + password;
	const std::string &motd = g_configManager().getString(SERVER_MOTD);
	if (!motd.empty()) {
		// Add MOTD
		output->addByte(0x14);

		std::ostringstream ss;
		ss << g_game().getMotdNum() << "\n"
		   << motd;
		output->addString(ss.str());
	}

	// Add char list
	auto [players, result] = account.getAccountPlayers();
	if (AccountErrors_t::Ok != result) {
		g_logger().warn("Account[{}] failed to load players!", account.getID());
	}

	const auto* loginLayout = protocolProfile ? ProtocolProfileRegistry::resolveAccountLoginLayout(protocolProfile->id) : nullptr;
	const auto characterListLayout = loginLayout ? loginLayout->characterListLayout : AccountCharacterListLayout::WorldListWithSessionKey;
	if (loginLayout && loginLayout->sendsSessionKey) {
		// Add session key
		output->addByte(0x28);
		output->addString(sessionKey);
	}

	output->addByte(0x64);

	if (characterListLayout == AccountCharacterListLayout::LegacyCharacterList) {
		const auto serverName = g_configManager().getString(SERVER_NAME);
		const auto configuredWorldIp = g_configManager().getString(IP);
		const auto worldIp = protocol_port_utils::legacyIpStringToNumber(configuredWorldIp);
		if (worldIp == 0) {
			g_logger().warn("Legacy character list cannot encode configured IP '{}'; old clients require a numeric IPv4 address.", configuredWorldIp);
			disconnectClient("Legacy 8.60 clients require the server IP to be a numeric IPv4 address.");
			return;
		}

		uint8_t size = std::min<size_t>(std::numeric_limits<uint8_t>::max(), players.size());
		output->addByte(size);

		const auto worldPort = protocolProfile ? protocol_port_utils::getGamePortForProfile(*protocolProfile) : protocol_port_utils::getModernGamePort();
		std::vector<std::string> characterNames;
		characterNames.reserve(size);
		for (const auto &[name, deletion] : players) {
			output->addString(name);
			output->addString(serverName);
			output->add<uint32_t>(worldIp);
			output->add<uint16_t>(worldPort);
			characterNames.emplace_back(name);
		}

		output->add<uint16_t>(std::min<uint32_t>(std::numeric_limits<uint16_t>::max(), account.getPremiumRemainingDays()));

		send(output);

		if (protocolProfile) {
			ProtocolSessionHintStore::getInstance().registerHint(getIP(), protocolProfile->id, sessionKey, characterNames);
		}

		disconnect();
		return;
	}

	output->addByte(1); // number of worlds

	output->addByte(0); // world id
	output->addString(g_configManager().getString(SERVER_NAME));
	output->addString(g_configManager().getString(IP));

	output->add<uint16_t>(protocolProfile ? protocol_port_utils::getGamePortForProfile(*protocolProfile) : protocol_port_utils::getModernGamePort());

	output->addByte(0);

	uint8_t size = std::min<size_t>(std::numeric_limits<uint8_t>::max(), players.size());
	output->addByte(size);
	std::vector<std::string> characterNames;
	characterNames.reserve(size);
	for (const auto &[name, deletion] : players) {
		output->addByte(0);
		output->addString(name);
		characterNames.emplace_back(name);
	}

	// Get premium days, check is premium and get lastday
	output->addByte(account.getPremiumRemainingDays());
	output->addByte(account.getPremiumLastDay() > getTimeNow());
	output->add<uint32_t>(account.getPremiumLastDay());

	send(output);

	if (protocolProfile) {
		ProtocolSessionHintStore::getInstance().registerHint(getIP(), protocolProfile->id, sessionKey, characterNames);
	}

	disconnect();
}

const AccountLoginLayout* ProtocolLogin::resolveLoginLayout(NetworkMessage &msg, uint16_t version) {
	const auto* loginLayout = ProtocolProfileRegistry::resolveAccountLoginLayout(version);
	if (!loginLayout) {
		disconnectClient(fmt::format("Unsupported client protocol version {}.", version));
		return nullptr;
	}

	protocolProfile = ProtocolProfileRegistry::getProfile(loginLayout->profileId);
	if (!protocolProfile || !ProtocolProfileRegistry::isProfileAllowed(protocolProfile->id)) {
		disconnectClient(fmt::format("Unsupported client protocol version {}.", version));
		return nullptr;
	}

	if (!loginLayout->hasAssetSignaturesBeforeRsa) {
		msg.skipBytes(loginLayout->bytesToSkipBeforeRsa);
		return loginLayout;
	}

	if (!msg.canRead(sizeof(uint32_t) * 3)) {
		disconnectClient(fmt::format("Invalid login packet for protocol version {}.", version));
		return nullptr;
	}

	const ClientAssetSignatures assetSignatures {
		.dat = msg.get<uint32_t>(),
		.spr = msg.get<uint32_t>(),
		.pic = msg.get<uint32_t>(),
	};

	protocolProfile = ProtocolProfileRegistry::resolveByClientVersionAndAssets(version, assetSignatures);
	if (!protocolProfile || !ProtocolProfileRegistry::isProfileAllowed(protocolProfile->id)) {
		disconnectClient(fmt::format("Unsupported client protocol version {}.", version));
		return nullptr;
	}

	loginLayout = ProtocolProfileRegistry::resolveAccountLoginLayout(protocolProfile->id);
	if (!loginLayout) {
		disconnectClient(fmt::format("Unsupported client protocol version {}.", version));
		return nullptr;
	}

	return loginLayout;
}

void ProtocolLogin::onRecvFirstMessage(NetworkMessage &msg) {
	if (g_game().getGameState() == GAME_STATE_SHUTDOWN) {
		disconnect();
		return;
	}

	msg.skipBytes(2); // client OS

	auto version = msg.get<uint16_t>();
	const auto* loginLayout = resolveLoginLayout(msg, version);
	if (!loginLayout) {
		return;
	}

	if (const auto connection = getConnection()) {
		connection->setTransportCodec(TransportCodecs::get(loginLayout->responseTransport), InitialTransportState::ResolvedFromPrelude);
	}

	// Old protocol support
	oldProtocol = protocolProfile->hasFeature(ProtocolFeature::OldProtocolCompat);
	/*
	 - Current/11.00 skips the remaining pre-RSA metadata:
	   4 bytes client version, 12 bytes dat/spr/pic signatures, 1 preview byte.
	 - 8.60 layouts read the dat/spr/pic signatures before RSA so the profile can
	   be resolved from the actual asset contract instead of the protocol number only.
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

	// FORK(cast) #153: "@cast" (bot camera list) skips password validation. "@livestream" takes
	// upstream's path below, which adds the active bots to its list.
	if (accountDescriptor == "@cast") {
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
	if (accountDescriptor == "@livestream") {
		if (oldProtocol && !g_configManager().getBoolean(OLD_PROTOCOL)) {
			disconnectClient(ProtocolProfileRegistry::getUnsupportedClientProtocolMessage(false));
			return;
		}

		dispatchProtocolTask(
			[self = std::static_pointer_cast<ProtocolLogin>(shared_from_this()), password] {
				self->getLivestreamCharacterList(password);
			},
			"ProtocolLogin::getLivestreamCharacterList"
		);
		return;
	}

	if (password.empty()) {
		disconnectClient("Invalid password.");
		return;
	}

	dispatchProtocolTask(
		[self = std::static_pointer_cast<ProtocolLogin>(shared_from_this()), accountDescriptor, password] {
			self->getCharacterList(accountDescriptor, password);
		},
		__FUNCTION__
	);
}

void ProtocolLogin::getLivestreamCharacterList(const std::string &password) const {
	// FORK(cast) #153: upstream's broadcasting humans, plus every active bot (bots never enter
	// LivestreamManager; selecting one lands in the bot camera, see ProtocolGame::routeViewerLogin).
	std::vector<std::string> names;
	for (const auto &caster : g_livestream().getBroadcastingCasters(password)) {
		if (caster) {
			names.push_back(caster->getName());
		}
	}
	if (isBotEngineReady()) {
		auto bots = g_botEngine().getActiveBotNames();
		std::sort(bots.begin(), bots.end());
		for (auto &name : bots) {
			if (std::find(names.begin(), names.end(), name) == names.end()) {
				names.push_back(std::move(name));
			}
		}
	}
	if (names.empty()) {
		disconnectClient("There are no players with the livestream on.");
		return;
	}

	sendViewerCharacterList("@livestream", password, "Welcome to Livestream System!", names);
}
