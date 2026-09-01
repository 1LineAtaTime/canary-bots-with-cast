<?php
/**
 * World Map plugin — installer.
 *
 * Adds the "World Map" entry to the Community menu (category 3) for every
 * template that has a "Who is Online?" entry, placing it directly after that
 * entry. Idempotent: running it twice does not duplicate the row.
 *
 * The page itself needs no schema — it reads `players` + `players_online`.
 */
defined('MYAAC') or die('Direct access not allowed!');

use MyAAC\Models\Menu;

const WORLDMAP_MENU_LINK = 'worldmap';
const WORLDMAP_MENU_NAME = 'World Map';
const WORLDMAP_MENU_CATEGORY = 3; // Community

try {
	$anchors = Menu::where('link', 'online')
		->where('category', WORLDMAP_MENU_CATEGORY)
		->get();

	if ($anchors->isEmpty()) {
		warning('World Map: no "Who is Online?" menu entry found to anchor to. '
			. 'Add a menu entry pointing to "worldmap" (category 3) by hand in the admin panel.');
		return;
	}

	$added = 0;
	$skipped = 0;
	foreach ($anchors as $anchor) {
		$exists = Menu::where('template', $anchor->template)
			->where('link', WORLDMAP_MENU_LINK)
			->exists();
		if ($exists) {
			$skipped++;
			continue;
		}

		// Make room directly after the anchor, then insert there.
		Menu::where('template', $anchor->template)
			->where('category', WORLDMAP_MENU_CATEGORY)
			->where('ordering', '>', $anchor->ordering)
			->increment('ordering');

		Menu::create([
			'template' => $anchor->template,
			'name'     => WORLDMAP_MENU_NAME,
			'link'     => WORLDMAP_MENU_LINK,
			'blank'    => $anchor->blank,
			'color'    => $anchor->color,
			'category' => WORLDMAP_MENU_CATEGORY,
			'ordering' => $anchor->ordering + 1,
			'enabled'  => 1,
		]);
		$added++;
	}

	success('World Map: menu entry added for ' . $added . ' template(s)'
		. ($skipped ? ', ' . $skipped . ' already present' : '') . '.');

	if (!is_file(PLUGINS . 'worldmap/static/map_big.jpg')) {
		warning('World Map: static/map_big.jpg is absent, so the "Overview" layer is '
			. 'disabled and the page opens on "Floors". That file is CipSoft artwork and is '
			. 'not redistributed with this plugin — see plugins/worldmap/README.md to add it.');
	}
} catch (Throwable $e) {
	error('World Map install failed: ' . $e->getMessage());
}
