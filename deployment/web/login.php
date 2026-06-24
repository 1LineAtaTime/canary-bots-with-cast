<?php

use MyAAC\Models\BoostedCreature;
use MyAAC\Models\PlayerOnline;
use MyAAC\Models\Account;
use MyAAC\Models\Player;
use MyAAC\RateLimit;

require_once 'common.php';
require_once SYSTEM . 'functions.php';
require_once SYSTEM . 'init.php';
require_once SYSTEM . 'status.php';

# error function
function sendError($message, $code = 3){
	$ret = [];
	$ret['errorCode'] = $code;
	$ret['errorMessage'] = $message;
	die(json_encode($ret));
}

# event schedule function
function parseEvent($table1, $date, $table2)
{
	if ($table1) {
		if ($date) {
			if ($table2) {
				$date = $table1->getAttribute('startdate');
				return date_create("{$date}")->format('U');
			} else {
				$date = $table1->getAttribute('enddate');
				return date_create("{$date}")->format('U');
			}
		} else {
			foreach($table1 as $attr) {
				if ($attr) {
					return $attr->getAttribute($table2);
				}
			}
		}
	}
	return 'error';
}

$request = json_decode(file_get_contents('php://input'));
$action = $request->type ?? '';

/** @var OTS_Base_DB $db */
/** @var array $config */

switch ($action) {
	case 'cacheinfo':
		$playersonline = PlayerOnline::count();
		die(json_encode([
			'playersonline' => $playersonline,
			'twitchstreams' => 0,
			'twitchviewer' => 0,
			'gamingyoutubestreams' => 0,
			'gamingyoutubeviewer' => 0
		]));

	case 'eventschedule':
		$eventlist = [];
		$file_path = config('server_path') . 'data/XML/events.xml';
		if (!file_exists($file_path)) {
			die(json_encode([]));
		}
		$xml = new DOMDocument;
		$xml->load($file_path);
		$tmplist = [];
		$tableevent = $xml->getElementsByTagName('event');

		foreach ($tableevent as $event) {
			if ($event) { $tmplist = [
			'colorlight' => parseEvent($event->getElementsByTagName('colors'), false, 'colorlight'),
			'colordark' => parseEvent($event->getElementsByTagName('colors'), false, 'colordark'),
			'description' => parseEvent($event->getElementsByTagName('description'), false, 'description'),
			'displaypriority' => intval(parseEvent($event->getElementsByTagName('details'), false, 'displaypriority')),
			'enddate' => intval(parseEvent($event, true, false)),
			'isseasonal' => getBoolean(intval(parseEvent($event->getElementsByTagName('details'), false, 'isseasonal'))),
			'name' => $event->getAttribute('name'),
			'startdate' => intval(parseEvent($event, true, true)),
			'specialevent' => intval(parseEvent($event->getElementsByTagName('details'), false, 'specialevent'))
				];
			$eventlist[] = $tmplist; } }
		die(json_encode(['eventlist' => $eventlist, 'lastupdatetimestamp' => time()]));

	case 'boostedcreature':
		$creature = $db->query("SELECT raceid FROM boosted_creature LIMIT 1")->fetch();
		$boss = $db->query("SELECT raceid FROM boosted_boss LIMIT 1")->fetch();
		die(json_encode([
			'boostedcreature' => true,
			'creatureraceid' => intval($creature['raceid'] ?? 0),
			'bossraceid' => intval($boss['raceid'] ?? 0)
		]));

		case 'login':

		$port = $config['lua']['gameProtocolPort'];

		// Cast system: intercept @cast login before authentication
		$inputEmail_check = $request->email ?? "";
		$inputAccountName_check = $request->accountname ?? "";
		if ($inputEmail_check === "@cast" || $inputAccountName_check === "@cast") {
			$castWorld = [
				"id" => 0,
				"name" => $config["lua"]["serverName"],
				"externaladdress" => $config["lua"]["ip"],
				"externalport" => $port,
				"externaladdressprotected" => $config["lua"]["ip"],
				"externalportprotected" => $port,
				"externaladdressunprotected" => $config["lua"]["ip"],
				"externalportunprotected" => $port,
				"previewstate" => 0,
				"location" => "BRA",
				"anticheatprotection" => false,
				"pvptype" => 0,
				"istournamentworld" => false,
				"restrictedstore" => false,
				"currenttournamentphase" => 2
			];
			// Show:
			//   (a) real-player broadcasters in cast_broadcasters, AND
			//   (b) every bot character (account_id = BOT_ACCOUNT_ID), even when hibernated.
			// Hibernated bots' cast_broadcasters row is deleted by C++ (bot_engine.cpp::hibernateBot
			// line 2432), but they should still appear in the cast list — selecting one triggers
			// castViewerLogin (protocolgame.cpp:746-755) which calls wakeBot synchronously, then
			// the viewer connects to the freshly-woken Player. Stream OFF for hibernated stays
			// correct because the in-memory castBroadcasting flag is set false on hibernate and
			// true on wake.
			//
			// bot_active_players is maintained by bot_engine.cpp (registerBot/unregisterBot
			// at src/creatures/players/bot/bot_engine.cpp:2303,2342) and contains the GUIDs
			// of bots currently loaded into the C++ engine. Using this instead of a raw
			// account_id=65000 filter ensures:
			//   - we only show bots that are actually loaded (not all 997 seeded characters
			//     when botPlayersOnline < total — see data/scripts/lib/bot_system.lua:15)
			//   - independent of the botPlayersShowAsOnline config (separate concern)
			//   - independent of the 10-min players_online update cycle (instant on register)
			//   - hibernated bots are kept (wake-on-click still works via castViewerLogin)
			// Table contract: data-otservbr-global/migrations/60.lua.
			$broadcasters = Player::where(function ($q) {
				// Inner closure → grouped OR: (id IN cast_broadcasters OR id IN bot_active_players)
				$q->whereIn("id", function ($sub) {
					$sub->select("player_id")->from("cast_broadcasters");
				})->orWhereIn("id", function ($sub) {
					$sub->select("player_id")->from("bot_active_players");
				});
			})
				// notDeleted() chained OUTSIDE the closure so it AND-binds to the full disjunction:
				//   (id IN cast_broadcasters OR id IN bot_active_players) AND deletion = 0
				// MyAAC scopeNotDeleted (system/src/Models/Player.php:72) maps to `WHERE deletion = 0`.
				// The column is `deletion` (BIGINT, 0 = alive), NOT `deleted_at`.
				->notDeleted()
				->selectRaw("id, name, level, sex, vocation, looktype, lookhead, lookbody, looklegs, lookfeet, lookaddons")
				->orderBy("name")
				->get();
			$castCharacters = [];
			$first = true;
			foreach ($broadcasters as $player) {
				$castCharacters[] = [
					"worldid" => 0,
					"name" => $player->name,
					"ismale" => $player->sex === 1,
					"tutorial" => false,
					"level" => $player->level,
					"vocation" => $player->vocation_name,
					"outfitid" => $player->looktype,
					"headcolor" => $player->lookhead,
					"torsocolor" => $player->lookbody,
					"legscolor" => $player->looklegs,
					"detailcolor" => $player->lookfeet,
					"addonsflags" => $player->lookaddons,
					"ishidden" => false,
					"istournamentparticipant" => false,
					"ismaincharacter" => $first,
					"dailyrewardstate" => 0,
					"remainingdailytournamentplaytime" => 0
				];
				$first = false;
			}
			$castSession = [
				"sessionkey" => "@cast\n",
				"lastlogintime" => 0,
				"ispremium" => true,
				"premiumuntil" => time() + 30 * 86400,
				"status" => "active",
				"returnernotification" => false,
				"showrewardnews" => false,
				"isreturner" => false,
				"fpstracking" => false,
				"optiontracking" => false,
				"tournamentticketpurchasestate" => 0,
				"emailcoderequest" => false
			];
			$castPlaydata = ["worlds" => [$castWorld], "characters" => $castCharacters];
			die(json_encode(["session" => $castSession, "playdata" => $castPlaydata]));
		}

		// default world info
		$world = [
			'id' => 0,
			'name' => $config['lua']['serverName'],
			'externaladdress' => $config['lua']['ip'],
			'externalport' => $port,
			'externaladdressprotected' => $config['lua']['ip'],
			'externalportprotected' => $port,
			'externaladdressunprotected' => $config['lua']['ip'],
			'externalportunprotected' => $port,
			'previewstate' => 0,
			'location' => 'BRA', // BRA, EUR, USA
			'anticheatprotection' => false,
			'pvptype' => array_search($config['lua']['worldType'], ['pvp', 'no-pvp', 'pvp-enforced']),
			'istournamentworld' => false,
			'restrictedstore' => false,
			'currenttournamentphase' => 2
		];

		$characters = [];

		$inputEmail = $request->email ?? false;
		$inputAccountName = $request->accountname ?? false;
		$inputToken = $request->token ?? false;

		$account = Account::query();
		if ($inputEmail != false) { // login by email
			$account->where('email', $inputEmail);
		}
		else if($inputAccountName != false) { // login by account name
			$account->where('name', $inputAccountName);
		}

		$account = $account->first();

		$ip = get_browser_real_ip();
		$limiter = new RateLimit('failed_logins', setting('core.account_login_attempts_limit'), setting('core.account_login_ban_time'));
		$limiter->enabled = setting('core.account_login_ipban_protection');
		$limiter->load();

		$ban_msg = 'A wrong account, password or secret has been entered ' . setting('core.account_login_attempts_limit') . ' times in a row. You are unable to log into your account for the next ' . setting('core.account_login_ban_time') . ' minutes. Please wait.';
		if (!$account) {
			$limiter->increment($ip);
			if ($limiter->exceeded($ip)) {
				sendError($ban_msg);
			}
			
			sendError(($inputEmail != false ? 'Email' : 'Account name') . ' or password is not correct.');
		}

		$current_password = encrypt((USE_ACCOUNT_SALT ? $account->salt : '') . $request->password);
		if (!$account || $account->password != $current_password) {
			$limiter->increment($ip);
			if ($limiter->exceeded($ip)) {
				sendError($ban_msg);
			}

			sendError(($inputEmail != false ? 'Email' : 'Account name') . ' or password is not correct.');
		}

		$accountHasSecret = false;
		if (fieldExist('secret', 'accounts')) {
			$accountSecret = $account->secret;
			if ($accountSecret != null && $accountSecret != '') {
				$accountHasSecret = true;
				if ($inputToken === false) {
					$limiter->increment($ip);
					if ($limiter->exceeded($ip)) {
						sendError($ban_msg);
					}
					sendError('Submit a valid two-factor authentication token.', 6);
				} else {
					require_once LIBS . 'rfc6238.php';
					if (TokenAuth6238::verify($accountSecret, $inputToken) !== true) {
						$limiter->increment($ip);
						if ($limiter->exceeded($ip)) {
							sendError($ban_msg);
						}

						sendError('Two-factor authentication failed, token is wrong.', 6);
					}
				}
			}
		}

		$limiter->reset($ip);
		if (setting('core.account_mail_verify') && $account->email_verified !== 1) {
			sendError('You need to verify your account, enter in our site and resend verify e-mail!');
		}

		// common columns
		$columns = 'id, name, level, sex, vocation, looktype, lookhead, lookbody, looklegs, lookfeet, lookaddons';

		if (fieldExist('isreward', 'accounts')) {
			$columns .= ', isreward';
		}

		if (fieldExist('istutorial', 'accounts')) {
			$columns .= ', istutorial';
		}

		$players = Player::where('account_id', $account->id)->notDeleted()->selectRaw($columns)->get();
		if($players && $players->count()) {
			$highestLevelId = $players->sortByDesc('experience')->first()->getKey();

			foreach ($players as $player) {
				$characters[] = create_char($player, $highestLevelId);
			}
		}

		if (fieldExist('premdays', 'accounts') && fieldExist('lastday', 'accounts')) {
			$save = false;
			$timeNow = time();
			$premDays = $account->premdays;
			$lastDay = $account->lastday;
			$lastLogin = $lastDay;

			if ($premDays != 0 && $premDays != PHP_INT_MAX) {
				if ($lastDay == 0) {
					$lastDay = $timeNow;
					$save = true;
				} else {
					$days = (int)(($timeNow - $lastDay) / 86400);
					if ($days > 0) {
						if ($days >= $premDays) {
							$premDays = 0;
							$lastDay = 0;
						} else {
							$premDays -= $days;
							$reminder = ($timeNow - $lastDay) % 86400;
							$lastDay = $timeNow - $reminder;
						}

						$save = true;
					}
				}
			} else if ($lastDay != 0) {
				$lastDay = 0;
				$save = true;
			}
			if ($save) {
				$account->premdays = $premDays;
				$account->lastday = $lastDay;
				$account->save();
			}
		}

		$worlds = [$world];
		$playdata = compact('worlds', 'characters');

		$sessionKey = ($inputEmail !== false) ? $inputEmail : $inputAccountName; // email or account name
		$sessionKey .= "\n" . $request->password; // password
		if (!fieldExist('istutorial', 'players')) {
			$sessionKey .= "\n";
		}
		$sessionKey .= ($accountHasSecret && strlen($accountSecret) > 5) ? $inputToken : '';

		// this is workaround to distinguish between TFS 1.x and otservbr
		// TFS 1.x requires the number in session key
		// otservbr requires just login and password
		// so we check for istutorial field which is present in otservbr, and not in TFS
		if (!fieldExist('istutorial', 'players')) {
			$sessionKey .= "\n".floor(time() / 30);
		}

		$session = [
			'sessionkey' => $sessionKey,
			'lastlogintime' => 0,
			'ispremium' => $account->is_premium,
			'premiumuntil' => ($account->premium_days) > 0 ? (time() + ($account->premium_days * 86400)) : 0,
			'status' => 'active', // active, frozen or suspended
			'returnernotification' => false,
			'showrewardnews' => true,
			'isreturner' => true,
			'fpstracking' => false,
			'optiontracking' => false,
			'tournamentticketpurchasestate' => 0,
			'emailcoderequest' => false
		];
		die(json_encode(compact('session', 'playdata')));

	default:
		sendError("Unrecognized event {$action}.");
	break;
}

function create_char($player, $highestLevelId) {
	return [
		'worldid' => 0,
		'name' => $player->name,
		'ismale' => $player->sex === 1,
		'tutorial' => isset($player->istutorial) && $player->istutorial,
		'level' => $player->level,
		'vocation' => $player->vocation_name,
		'outfitid' => $player->looktype,
		'headcolor' => $player->lookhead,
		'torsocolor' => $player->lookbody,
		'legscolor' => $player->looklegs,
		'detailcolor' => $player->lookfeet,
		'addonsflags' => $player->lookaddons,
		'ishidden' => $player->is_deleted,
		'istournamentparticipant' => false,
		'ismaincharacter' => $highestLevelId === $player->getKey(),
		'dailyrewardstate' => $player->isreward ?? 0,
		'remainingdailytournamentplaytime' => 0
	];
}
