/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#pragma once

#include "server/network/protocol/protocol.hpp"
#include "server/network/protocol/protocol_profile.hpp"

class NetworkMessage;
class OutputMessage;

class ProtocolLogin final : public Protocol {
public:
	// static protocol information
	enum { SERVER_SENDS_FIRST = false };
	enum { PROTOCOL_IDENTIFIER = 0x01 };
	enum { USE_CHECKSUM = true };
	static const char* protocol_name() {
		return "login protocol";
	}

	explicit ProtocolLogin(const Connection_ptr &loginConnection) :
		Protocol(loginConnection) { }

	void onRecvFirstMessage(NetworkMessage &msg) override;

private:
	void disconnectClient(const std::string &message) const;
	const AccountLoginLayout* resolveLoginLayout(NetworkMessage &msg, uint16_t version);

	void getCharacterList(const std::string &accountDescriptor, const std::string &password) const;
	void getLivestreamCharacterList(const std::string &password) const;
	// FORK(cast) #153: the viewer character list for "@cast" (bot camera) and "@livestream"
	// (upstream livestream + bots). One wire format for both: upstream's livestream list.
	void sendViewerCharacterList(const std::string &accountDescriptor, const std::string &password, const std::string &motd, const std::vector<std::string> &names) const;

	bool oldProtocol = false;
	const ProtocolProfile* protocolProfile = &ProtocolProfileRegistry::getCurrentProfile();
};
