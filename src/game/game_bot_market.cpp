/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

// Bot market wrapper functions.
//
// Mirrors the inner transaction work of:
//   - Game::playerCreateMarketOffer    (game.cpp:9398)
//   - Game::playerAcceptMarketOffer    (game.cpp:9588)
//   - Game::playerCancelMarketOffer    (game.cpp:9500)
//
// Key differences from the user-facing functions:
//   - Skip isInMarket / isUIExhausted gates (bots have no UI session).
//   - Skip account-self-match check (bot scheduler filters bot-bot pairs at SQL level).
//   - Skip depotLocker requirement; bot SELL/BUY-fulfill bypasses depot item removal
//     (items are conjured for delivery to the real-player counterparty).
//   - Skip UI side-effects (sendMarketEnter, sendMarketBrowseItem, sendMarketAcceptOffer,
//     sendMarketCancelOffer, sendCancelMessage, sendTextMessage, onReceiveMail).
//
// Preserved (so real players experience identical economic side-effects):
//   - Standard 2% fee on offer creation (clamped 20-1M gp).
//   - Bank balance debit/credit for both bot and counterparty.
//   - Item delivery to the real-player counterparty's inbox via the same
//     addItemBatchToPaginedContainer primitive used by processItemInsertion.
//   - IOMarket::createOffer / acceptOffer / deleteOffer / appendHistory /
//     moveOfferToHistory for canonical DB writes.
//   - g_saveManager().savePlayer() for offline counterparties.
//
// IMPORTANT: when the user-facing market functions in game.cpp change, mirror those
// changes here. The two paths share no helper code; drift is the main risk.

#include "game/game.hpp"

#include "config/configmanager.hpp"
#include "creatures/players/player.hpp"
#include "database/botdatabasetasks.hpp" // bundle 6: dedicated bot-DB worker
#include "game/scheduling/save_manager.hpp"
#include "io/iologindata.hpp"
#include "io/iomarket.hpp"
#include "items/containers/inbox/inbox.hpp"
#include "items/items.hpp"
#include "lib/metrics/metrics.hpp"
#include "lua/scripts/lua_environment.hpp"

namespace {

// Inline equivalent of game.cpp's anonymous-namespace processItemInsertion (line 9310).
// Keeps the bot path independent of game.cpp's translation unit.
// Cross-reference: keep this in sync with src/game/game.cpp:9310.
void botProcessItemInsertion(
	const std::shared_ptr<Player> &recipient,
	uint16_t itemId,
	uint16_t &amount,
	uint8_t tier,
	uint64_t &totalPrice,
	uint32_t pricePerItem
) {
	if (!recipient) {
		return;
	}
	uint32_t actuallyAdded = 0;
	const auto &inbox = recipient->getInbox();
	if (!inbox) {
		amount = 0;
		totalPrice = 0;
		return;
	}
	ReturnValue rv = recipient->addItemBatchToPaginedContainer(inbox, itemId, amount, actuallyAdded, FLAG_NOLIMIT, tier);
	if (rv != RETURNVALUE_NOERROR) {
		g_logger().warn(
			"[botProcessItemInsertion] add to inbox returned {} for player {} item {} amount {}",
			static_cast<int>(rv),
			recipient->getName(),
			itemId,
			amount
		);
	}
	if (actuallyAdded < amount) {
		totalPrice = pricePerItem * actuallyAdded;
		amount = actuallyAdded;
	}
}

// Compute the server's standard market fee for a given total price.
// Mirrors game.cpp:9420-9429 (2% fee, clamped 20gp..1Mgp).
uint64_t botComputeMarketFee(uint64_t totalPrice) {
	uint64_t totalFee = static_cast<uint64_t>(totalPrice * 0.02);
	uint64_t minFee = 20;
	uint64_t maxFee = std::min<uint64_t>(1000000, totalFee);
	if (maxFee < minFee) {
		maxFee = minFee;
	}
	return std::clamp(totalFee, uint64_t(20), maxFee);
}

} // namespace

bool Game::botCreateMarketOffer(uint32_t botGuid, uint8_t action, uint16_t itemId, uint16_t amount, uint64_t price, uint8_t tier, bool anonymous) {
	// JITTER FIX 2026-06-11 (bundle 3): this function used to perform THREE
	// synchronous DB operations on the dispatcher per offer — a full offline
	// IOLoginData load for hibernated bots (getPlayerByGUID(.., true)), a sync
	// per-player COUNT (IOMarket::getPlayerOfferCount) and a sync INSERT
	// (IOMarket::createOffer). Behind worker-side DB convoys each INSERT measured
	// 106-151ms; six in one seller fire stacked into an 885ms GlobalEvents task ->
	// HB_STALL 882ms -> the 08:39:28 lagmark. Now:
	//   - validation stays in-memory;
	//   - NO offline load: for hibernated bots the fee/escrow debit was theater
	//     anyway (debited onto a temp Player and discarded — see the 2026-05-27
	//     skip-bot-save precedent below), so it is skipped when the bot is not in
	//     the online map;
	//   - cap check + INSERT collapse into ONE statement enqueued on the
	//     DatabaseTasks worker; the per-player cap is enforced atomically in SQL
	//     (INSERT .. SELECT .. WHERE count < cap; the derived table sidesteps
	//     MySQL error 1093 on self-referencing inserts).
	const ItemType &it = Item::items[itemId];
	if (it.id == 0 || it.wareId == 0) {
		g_logger().warn("[botCreateMarketOffer] item {} not marketable (wareId=0)", itemId);
		return false;
	}

	if (action != MARKETACTION_BUY && action != MARKETACTION_SELL) {
		return false;
	}

	if (amount == 0 || price == 0) {
		return false;
	}

	// Tier validation — mirrors game.cpp:9411-9418
	const uint8_t maxTier = static_cast<uint8_t>(g_configManager().getNumber(FORGE_MAX_ITEM_TIER));
	if (tier > maxTier) {
		tier = maxTier;
	}
	if (tier > 0 && it.upgradeClassification == 0) {
		tier = 0;
	}

	const uint64_t totalPrice = price * static_cast<uint64_t>(amount);
	const uint64_t fee = botComputeMarketFee(totalPrice);

	// Money theater only for AWAKE bots (present in the online map). Hibernated
	// bots skip it: their temp-player debit was discarded unpersisted anyway.
	const auto &bot = getPlayerByGUID(botGuid); // no offline load
	if (bot) {
		if (action == MARKETACTION_SELL) {
			// SELL: bot conjures items (skip removeOfferItems). Only fee is debited.
			if (fee > (bot->getMoney() + bot->getBankBalance())) {
				g_logger().warn("[botCreateMarketOffer] bot {} fee {} > funds for SELL", bot->getName(), fee);
				return false;
			}
			removeMoney(bot, fee, 0, true);
			g_metrics().addCounter("balance_decrease", fee, { { "player", bot->getName() }, { "context", "market_fee_bot" } });
		} else {
			// BUY: bot escrows totalPrice + fee (mirrors game.cpp:9462-9472)
			const uint64_t needed = totalPrice + fee;
			if (needed > (bot->getMoney() + bot->getBankBalance())) {
				g_logger().warn("[botCreateMarketOffer] bot {} needs {} for BUY but has {}+{} bank+pocket", bot->getName(), needed, bot->getBankBalance(), bot->getMoney());
				return false;
			}
			removeMoney(bot, needed, 0, true);
			g_metrics().addCounter("balance_decrease", needed, { { "player", bot->getName() }, { "context", "market_offer_bot" } });
		}
	}

	// Cap-guarded async INSERT on the DatabaseTasks worker. Mirrors
	// IOMarket::createOffer's column set; the WHERE clause enforces
	// maxMarketOffersAtATimePerPlayer atomically (cap 0 = unlimited, plain insert).
	const uint32_t maxOfferCount = g_configManager().getNumber(MAX_MARKET_OFFERS_AT_A_TIME_PER_PLAYER);
	std::string insertQuery;
	if (maxOfferCount == 0) {
		insertQuery = fmt::format(
			"INSERT INTO `market_offers` (`player_id`, `sale`, `itemtype`, `amount`, `created`, `anonymous`, `price`, `tier`) "
			"VALUES ({}, {}, {}, {}, {}, {}, {}, {})",
			botGuid, static_cast<int>(action), it.id, amount, time(nullptr), anonymous ? 1 : 0, price, static_cast<int>(tier)
		);
	} else {
		insertQuery = fmt::format(
			"INSERT INTO `market_offers` (`player_id`, `sale`, `itemtype`, `amount`, `created`, `anonymous`, `price`, `tier`) "
			"SELECT {}, {}, {}, {}, {}, {}, {}, {} FROM DUAL "
			"WHERE (SELECT `c` FROM (SELECT COUNT(*) AS `c` FROM `market_offers` WHERE `player_id` = {}) AS `t`) < {}",
			botGuid, static_cast<int>(action), it.id, amount, time(nullptr), anonymous ? 1 : 0, price, static_cast<int>(tier),
			botGuid, maxOfferCount
		);
	}
	g_botDatabaseTasks().execute(insertQuery);

	// PERF FIX (2026-05-27): skip bot save here. Bots have effectively unlimited
	// money and bot inventory is purely cosmetic — the market is just for "feels
	// alive" purposes (visible offers + counterparty trades). Persisting bot
	// money/inventory changes per market action was firing the 15-query sync save
	// chain on the dispatcher for every offer. Crash-time data loss accepted:
	// the async INSERT above DOES persist the offer row itself, which is the
	// only thing that needs to survive (so real players can see/accept it).
	// User explicit approval 2026-05-27.

	return true;
}

bool Game::botAcceptMarketOffer(uint32_t botGuid, uint32_t offerId, uint16_t amount) {
	const auto &bot = getPlayerByGUID(botGuid, true);
	if (!bot) {
		g_logger().warn("[botAcceptMarketOffer] bot {} not found", botGuid);
		return false;
	}

	MarketOfferEx offer = IOMarket::getOfferById(offerId);
	if (offer.id == 0) {
		// Offer no longer exists (raced, expired, cancelled). Caller can skip.
		return false;
	}

	const ItemType &it = Item::items[offer.itemId];
	if (it.id == 0 || it.wareId == 0) {
		g_logger().warn("[botAcceptMarketOffer] offer {} item {} not marketable", offerId, offer.itemId);
		return false;
	}

	const uint8_t maxTier = static_cast<uint8_t>(g_configManager().getNumber(FORGE_MAX_ITEM_TIER));
	const uint8_t offerTier = it.upgradeClassification > 0 ? std::min<uint8_t>(offer.tier, maxTier) : 0;
	offer.tier = offerTier;

	if (amount == 0 || amount > offer.amount) {
		return false;
	}

	uint64_t totalPrice = offer.price * static_cast<uint64_t>(amount);

	// Skip self-accept (account 65000 vs same account). The scheduler should filter bot-bot
	// pairs at SQL level; this is a defensive check.
	if (offer.playerId == bot->getGUID()) {
		return false;
	}

	if (offer.type == MARKETACTION_BUY) {
		// Real player has a BUY offer; bot fulfills as seller. Items are conjured (no depot debit).
		// Mirrors game.cpp:9633-9713 (BUY-acceptance branch) but without removeOfferItems.
		const auto &realBuyer = getPlayerByGUID(offer.playerId, true);
		if (!realBuyer) {
			g_logger().warn("[botAcceptMarketOffer] offer {} buyer {} not loadable", offerId, offer.playerId);
			return false;
		}

		// Bot receives the escrow money (debited from buyer at offer creation time)
		bot->setBankBalance(bot->getBankBalance() + totalPrice);
		g_metrics().addCounter("balance_increase", totalPrice, { { "player", bot->getName() }, { "context", "market_sale_bot" } });

		// Items delivered to real buyer's inbox via the same primitive as the user-facing path
		uint16_t processedAmount = amount;
		uint64_t effectivePrice = offer.price * processedAmount;
		botProcessItemInsertion(realBuyer, it.id, processedAmount, offer.tier, effectivePrice, offer.price);
		amount = processedAmount;
		totalPrice = effectivePrice;

		if (realBuyer->isOffline()) {
			g_saveManager().savePlayer(realBuyer);
		}
	} else { // MARKETACTION_SELL
		// Real player has a SELL offer; bot accepts as buyer. Bot pays bank, items go to bot inbox.
		// Mirrors game.cpp:9714-9763 (SELL-acceptance branch).
		const auto &realSeller = getPlayerByGUID(offer.playerId, true);
		if (!realSeller) {
			g_logger().warn("[botAcceptMarketOffer] offer {} seller {} not loadable", offerId, offer.playerId);
			return false;
		}

		if (totalPrice > (bot->getMoney() + bot->getBankBalance())) {
			g_logger().warn("[botAcceptMarketOffer] bot {} insufficient funds for SELL acceptance: needed {}", bot->getName(), totalPrice);
			return false;
		}

		// Debit bot — prefer bank
		if (totalPrice <= bot->getBankBalance()) {
			bot->setBankBalance(bot->getBankBalance() - totalPrice);
		} else {
			uint64_t remains = totalPrice - bot->getBankBalance();
			bot->setBankBalance(0);
			removeMoney(bot, remains);
		}
		g_metrics().addCounter("balance_decrease", totalPrice, { { "player", bot->getName() }, { "context", "market_purchase_bot" } });

		// Items delivered to bot's inbox (will sit there harmlessly; bot never retrieves them)
		uint16_t processedAmount = amount;
		uint64_t effectivePrice = offer.price * processedAmount;
		botProcessItemInsertion(bot, it.id, processedAmount, offer.tier, effectivePrice, offer.price);
		amount = processedAmount;
		totalPrice = effectivePrice;

		// Credit real seller's bank
		realSeller->setBankBalance(realSeller->getBankBalance() + totalPrice);
		g_metrics().addCounter("balance_increase", totalPrice, { { "player", realSeller->getName() }, { "context", "market_sale" } });

		if (realSeller->isOffline()) {
			g_saveManager().savePlayer(realSeller);
		}
	}

	// History rows for both sides (same call sequence as game.cpp:9777-9787)
	IOMarket::appendHistory(
		bot->getGUID(),
		(offer.type == MARKETACTION_BUY ? MARKETACTION_SELL : MARKETACTION_BUY),
		offer.itemId,
		amount,
		offer.price,
		time(nullptr),
		offer.tier,
		OFFERSTATE_ACCEPTEDEX
	);
	IOMarket::appendHistory(
		offer.playerId,
		offer.type,
		offer.itemId,
		amount,
		offer.price,
		time(nullptr),
		offer.tier,
		OFFERSTATE_ACCEPTED
	);

	const uint16_t remaining = offer.amount - amount;
	if (remaining == 0) {
		IOMarket::deleteOffer(offer.id);
	} else {
		IOMarket::acceptOffer(offer.id, amount);
	}

	// PERF FIX (2026-05-27): skip bot save (see botCreateMarketOffer above).
	// Bot's money/inventory state on this temp Player is discarded when the
	// shared_ptr goes out of scope — accepted per user 2026-05-27. The offer
	// history rows above (IOMarket::appendHistory) and the offer deletion/accept
	// (IOMarket::deleteOffer / acceptOffer) ARE persisted, so the real-player
	// counterparty sees the correct outcome.

	return true;
}

bool Game::botCancelMarketOffer(uint32_t botGuid, uint32_t offerId) {
	const auto &bot = getPlayerByGUID(botGuid, true);
	if (!bot) {
		return false;
	}

	MarketOfferEx offer = IOMarket::getOfferById(offerId);
	if (offer.id == 0 || offer.playerId != bot->getGUID()) {
		return false;
	}

	if (offer.type == MARKETACTION_BUY) {
		// Refund escrow to bot
		bot->setBankBalance(bot->getBankBalance() + offer.price * offer.amount);
		g_metrics().addCounter("balance_increase", offer.price * offer.amount, { { "player", bot->getName() }, { "context", "market_purchase_refund_bot" } });
	} else {
		// SELL cancel — items would normally go back to seller's inbox.
		// For bots, items were never debited from depot (botCreateMarketOffer skips that),
		// so we don't need to deliver anything back. The plan acknowledges this is intentional.
	}

	IOMarket::moveOfferToHistory(offer.id, OFFERSTATE_CANCELLED);

	// PERF FIX (2026-05-27): skip bot save (see botCreateMarketOffer above).
	// Refund of bot money is in-memory only — discarded with the temp Player
	// shared_ptr. Accepted per user.

	return true;
}
