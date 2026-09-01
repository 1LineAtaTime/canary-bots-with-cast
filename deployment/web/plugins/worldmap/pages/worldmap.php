<?php
/**
 * World Map — live positions of the bot players.
 *
 * Two layers over the SAME minimap pixel grid (1 px = 1 world tile), differing
 * only by an integer origin. See ../config.php for the constants and
 * implementation_plans/BOT_WORLDMAP_PLAN.md §4 for how they were verified.
 *
 * Data comes straight out of `players` + `players_online` — the bot engine
 * already persists bot positions (bot_tick.cpp maybeQueuePositionSave), so this
 * page adds NO server-side telemetry and needs no schema.
 */
defined('MYAAC') or die('Direct access not allowed!');

$wmCfg = require PLUGINS . 'worldmap/config.php';

$wmAccountId = isset($wmCfg['account_id']) && $wmCfg['account_id'] !== null
	? (int)$wmCfg['account_id'] : null;

// ---------------------------------------------------------------------------
// JSON endpoint: /index.php/worldmap?data=1
// ---------------------------------------------------------------------------
if (isset($_GET['data'])) {
	while (ob_get_level() > 0) {
		ob_end_clean();
	}
	header('Content-Type: application/json');
	header('Cache-Control: no-store');

	$cacheFile = CACHE . 'worldmap_data.json';
	$ttl = (int)($wmCfg['cache_ttl'] ?? 5);
	if ($ttl > 0 && is_file($cacheFile) && (time() - filemtime($cacheFile)) < $ttl) {
		readfile($cacheFile);
		exit;
	}

	// account_id is cast to int above and never interpolated from user input.
	$where = $wmAccountId !== null ? ' WHERE p.`account_id` = ' . $wmAccountId : '';
	try {
		$rows = $db->query(
			'SELECT p.`id`, p.`name`, p.`level`, p.`vocation`, p.`posx`, p.`posy`, p.`posz`'
			. ' FROM `players_online` po'
			. ' JOIN `players` p ON p.`id` = po.`player_id`'
			. $where
		)->fetchAll(PDO::FETCH_ASSOC);
	} catch (Throwable $e) {
		http_response_code(500);
		echo json_encode(['t' => time(), 'error' => 'query failed', 'markers' => []]);
		exit;
	}

	// Compact payload: fixed-order arrays instead of per-row keys (~40% smaller
	// over 500 rows). Order must match the BOT_* indices in worldmap.js.
	$markers = [];
	foreach ($rows as $r) {
		$markers[] = [
			$r['name'],
			(int)$r['level'],
			(int)$r['vocation'],
			(int)$r['posx'],
			(int)$r['posy'],
			(int)$r['posz'],
		];
	}

	$json = json_encode(['t' => time(), 'markers' => $markers]);
	// Best-effort cache write; a failure here must not break the response.
	if ($ttl > 0) {
		@file_put_contents($cacheFile, $json, LOCK_EX);
	}
	echo $json;
	exit;
}

// ---------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------
$title = 'World Map';

$vocations = setting('core.vocations');
if (!is_array($vocations)) {
	$vocations = [];
}
// setting() keeps promoted vocations (5-8); trim stray whitespace from the
// comma-separated config string ("Elder Druid,Royal Paladin" has no space).
$vocations = array_map('trim', $vocations);

// map_big.jpg is CipSoft artwork and is NOT redistributed with this plugin, so a
// fresh install will not have it. Detect that and drop the Overview layer rather
// than offering a button that renders a broken image.
$hasOverview = is_file(PLUGINS . 'worldmap/static/map_big.jpg');
$defaultLayer = $wmCfg['default_layer'] ?? 'overview';
if (!$hasOverview) {
	$defaultLayer = 'floors';
}

$cfg = [
	'dataUrl'      => BASE_URL . 'index.php/worldmap?data=1',
	'assets'       => BASE_URL . 'plugins/worldmap/static/',
	'pollMs'       => (int)($wmCfg['poll_ms'] ?? 30000),
	'defaultFloor' => (int)($wmCfg['default_floor'] ?? 7),
	'defaultLayer' => $defaultLayer,
	'hasOverview'  => $hasOverview,
	'layers'       => $wmCfg['layers'],
	'bounds'       => $wmCfg['bounds'],
	'vocations'    => $vocations,
];
?>
<link rel="stylesheet" href="<?php echo BASE_URL; ?>plugins/worldmap/static/leaflet.css"/>
<link rel="stylesheet" href="<?php echo BASE_URL; ?>plugins/worldmap/static/worldmap.css"/>

<div id="worldmap-wrap">
	<div id="worldmap-bar">
		<span class="wm-group"<?php echo $hasOverview ? '' : ' hidden'; ?>>
			<button type="button" class="wm-btn wm-layer<?php echo $defaultLayer === 'overview' ? ' is-active' : ''; ?>"
					data-layer="overview">Overview</button><button
					type="button" class="wm-btn wm-layer<?php echo $defaultLayer === 'floors' ? ' is-active' : ''; ?>"
					data-layer="floors">Floors</button>
		</span>
		<span class="wm-group" id="wm-floorbox"<?php echo $defaultLayer === 'floors' ? '' : ' hidden'; ?>>
			<button type="button" class="wm-btn" id="wm-floor-up" title="Up one floor (PageUp)">&minus;</button>
			<span id="wm-floor-label"><?php echo (int)($wmCfg['default_floor'] ?? 7); ?></span>
			<button type="button" class="wm-btn" id="wm-floor-down" title="Down one floor (PageDown)">&plus;</button>
		</span>
		<span class="wm-status" id="wm-status">loading&hellip;</span>
	</div>

	<div id="worldmap"></div>

	<div id="wm-legend">
		<b>Vocation</b>
		<span><i style="background:#e05252"></i>Sorcerer</span>
		<span><i style="background:#4fb04f"></i>Druid</span>
		<span><i style="background:#e8b84b"></i>Paladin</span>
		<span><i style="background:#5aa9e6"></i>Knight</span>
		<span><i style="background:#9b9b9b"></i>None</span>
	</div>

	<p id="wm-foot">
		Positions are persisted every few minutes, so dots lag reality by up to that much.
		<?php if ($hasOverview): ?>
			The <b>Overview</b> layer ignores floors and plots every character by x/y alone; the
			<b>Floors</b> layer shows only those actually on the selected floor.
		<?php else: ?>
			Each floor shows only the characters actually on it.
		<?php endif; ?>
		Floor imagery from <a href="https://tibiamaps.io/" rel="noopener" target="_blank">tibiamaps.io</a>
		(<a href="https://github.com/tibiamaps/tibia-map-data" rel="noopener" target="_blank">tibia-map-data</a>, MIT).
	</p>
</div>

<script>window.WORLDMAP_CFG = <?php echo json_encode($cfg); ?>;</script>
<script src="<?php echo BASE_URL; ?>plugins/worldmap/static/leaflet.js"></script>
<script src="<?php echo BASE_URL; ?>plugins/worldmap/static/worldmap.js"></script>
