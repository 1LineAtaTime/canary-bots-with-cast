<?php
/**
 * World Map plugin — configuration.
 *
 * Everything a fresh install is likely to need to change lives here. Nothing in
 * this file needs the game server: the page reads positions that the server
 * already persists into `players`.
 */
defined('MYAAC') or die('Direct access not allowed!');

return [
	// Account whose characters are drawn on the map. On this project all bot
	// players share account 65000 (see .claude/CLAUDE.md "Architecture").
	// Set to null to plot EVERY online character instead of only one account's —
	// note that doing so publishes your real players' live positions.
	'account_id' => 65000,

	// Browser poll interval (ms). The underlying positions are only persisted
	// every few minutes, so polling faster than this buys nothing.
	'poll_ms' => 30000,

	// Server-side response cache (seconds). N simultaneous viewers cost one
	// query per this window, not one query each.
	'cache_ttl' => 5,

	// Floor shown when the Floors layer is opened. 7 is Tibia's ground level.
	'default_floor' => 7,

	// Layer shown on load: 'overview' or 'floors'. If 'overview' is chosen but
	// static/map_big.jpg is absent, the page falls back to 'floors' on its own.
	'default_layer' => 'overview',

	// ---- Geometry -----------------------------------------------------------
	// Both layers are the SAME minimap pixel grid at 1 px = 1 world tile and
	// differ only by an integer origin. Do not change these unless you replace
	// the images; see implementation_plans/BOT_WORLDMAP_PLAN.md §4 for how they
	// were derived and how to re-verify with ?calib=1.
	'layers' => [
		// tibiamaps floor-NN-map.png. bounds.json's width/height are
		// authoritative; its xMax/yMax fields contradict them and would wrongly
		// clip Ankrahmun (y=32851) and Liberty Bay (y=32827).
		'floors'   => ['ox' => 31744, 'oy' => 30976, 'w' => 2560, 'h' => 2048],
		// map_big.jpg — the same grid inset by its 128px left / 256px top border.
		'overview' => ['ox' => 31616, 'oy' => 30720, 'w' => 2784, 'h' => 2592],
	],

	// World extent covered by the imagery, shared by both layers. Characters
	// outside it are counted as "off-map" instead of being drawn in the border.
	'bounds' => ['minx' => 31744, 'maxx' => 34303, 'miny' => 30976, 'maxy' => 33023],
];
