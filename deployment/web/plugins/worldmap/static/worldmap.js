/* World Map — live character positions.
 *
 * Geometry, in full: both layers are the SAME minimap pixel grid at 1 px = 1
 * world tile and differ ONLY by an integer origin (verified by registering
 * map_big.jpg against the tibiamaps floor-07 render — 8 temple anchors across
 * the map converged on offset (128,256) to within 1 px). So there is one
 * projection helper and one marker pipeline; the layer switch swaps an origin,
 * an image and a filter, nothing else.
 *
 * Every tunable comes from ../config.php via window.WORLDMAP_CFG.
 */
(function () {
	'use strict';

	var CFG = window.WORLDMAP_CFG || {};
	var ASSETS = CFG.assets || '';
	var POLL_MS = CFG.pollMs || 30000;
	var VOCATIONS = CFG.vocations || {};
	var BOUNDS = CFG.bounds || { minx: 31744, maxx: 34303, miny: 30976, maxy: 33023 };

	// Compact marker payload indices — must match worldmap.php's $markers order.
	var BOT_NAME = 0, BOT_LEVEL = 1, BOT_VOC = 2, BOT_X = 3, BOT_Y = 4, BOT_Z = 5;

	// Origins/sizes come from config.php; behaviour is attached here.
	var LAYERS = {};
	if (CFG.hasOverview && CFG.layers && CFG.layers.overview) {
		LAYERS.overview = Object.assign({}, CFG.layers.overview, {
			url: function () { return ASSETS + 'map_big.jpg'; },
			pixelated: false,
			keep: function () { return true; }          // z-agnostic by design
		});
	}
	LAYERS.floors = Object.assign({}, (CFG.layers && CFG.layers.floors) || {}, {
		url: function (f) { return ASSETS + 'floors/floor-' + (f < 10 ? '0' + f : f) + '.png'; },
		pixelated: true,
		keep: function (bot, floor) { return bot[BOT_Z] === floor; }
	});

	var VOC_COLOR = {
		0: '#9b9b9b',
		1: '#e05252', 5: '#e05252',   // Sorcerer / Master Sorcerer
		2: '#4fb04f', 6: '#4fb04f',   // Druid / Elder Druid
		3: '#e8b84b', 7: '#e8b84b',   // Paladin / Royal Paladin
		4: '#5aa9e6', 8: '#5aa9e6'    // Knight / Elite Knight
	};

	var state = {
		layer: LAYERS[CFG.defaultLayer] ? CFG.defaultLayer : 'floors',
		floor: typeof CFG.defaultFloor === 'number' ? CFG.defaultFloor : 7,
		bots: [],
		lastOk: 0,
		failed: false,
		counts: { shown: 0, otherFloor: 0, offMap: 0 }
	};

	var CALIB = /[?&]calib=1/.test(window.location.search);
	// Engine ground truth: bot_city_pois, poi_type='temple' (name, x, y, z).
	var TEMPLES = [
		['Thais', 32369, 32241, 7], ['Venore', 32958, 32077, 7], ['Ankrahmun', 33197, 32851, 7],
		['Carlin', 32360, 31782, 7], ['Svargrond', 32212, 31132, 7], ['Feyrist', 33490, 32221, 7],
		['Port Hope', 32594, 32745, 7], ['Darashia', 33213, 32453, 7], ['Yalahar', 32787, 31276, 7],
		['Edron', 33217, 31814, 8], ['Rathleton', 33594, 31899, 6], ['Issavi', 33921, 31477, 5]
	];

	var el = {
		status: document.getElementById('wm-status'),
		floorBox: document.getElementById('wm-floorbox'),
		floorLabel: document.getElementById('wm-floor-label')
	};

	var map = L.map('worldmap', {
		crs: L.CRS.Simple,
		minZoom: -3,
		maxZoom: 3,
		zoomSnap: 0.25,
		attributionControl: false
	});

	var canvas = L.canvas({ padding: 0.3 });
	var markerLayer = L.layerGroup().addTo(map);
	var calibLayer = L.layerGroup().addTo(map);
	var overlay = null;

	function cur() { return LAYERS[state.layer]; }

	function imageBounds(L_) { return [[-L_.h, 0], [0, L_.w]]; }

	// world -> Leaflet latLng. CRS.Simple has y increasing upward, so the pixel
	// row is negated.
	function toLatLng(L_, wx, wy) { return [-(wy - L_.oy), wx - L_.ox]; }

	function inWorld(wx, wy) {
		return wx >= BOUNDS.minx && wx <= BOUNDS.maxx && wy >= BOUNDS.miny && wy <= BOUNDS.maxy;
	}

	function vocName(id) {
		return VOCATIONS[id] || VOCATIONS[String(id)] || 'Vocationless';
	}

	function setOverlay(fit) {
		var L_ = cur();
		var b = imageBounds(L_);
		if (overlay) { map.removeLayer(overlay); }
		overlay = L.imageOverlay(L_.url(state.floor), b, {
			className: L_.pixelated ? 'wm-pixelated' : ''
		}).addTo(map);
		overlay.bringToBack();
		map.setMaxBounds(L.latLngBounds(b).pad(0.25));
		if (fit) { map.fitBounds(b); }
	}

	function draw() {
		markerLayer.clearLayers();
		var L_ = cur();
		var shown = 0, offMap = 0, otherFloor = 0;

		for (var i = 0; i < state.bots.length; i++) {
			var b = state.bots[i];
			if (!inWorld(b[BOT_X], b[BOT_Y])) { offMap++; continue; }
			if (!L_.keep(b, state.floor)) { otherFloor++; continue; }

			var m = L.circleMarker(toLatLng(L_, b[BOT_X], b[BOT_Y]), {
				renderer: canvas,
				radius: 4,
				weight: 1,
				color: '#1b1b1b',
				opacity: 0.85,
				fillColor: VOC_COLOR[b[BOT_VOC]] || VOC_COLOR[0],
				fillOpacity: 0.95
			});
			m.bindTooltip(
				'<b>' + escapeHtml(b[BOT_NAME]) + '</b><br>Level ' + b[BOT_LEVEL] +
				' ' + escapeHtml(vocName(b[BOT_VOC])) +
				'<br>' + b[BOT_X] + ', ' + b[BOT_Y] + ', ' + b[BOT_Z],
				{ className: 'wm-tip', direction: 'top', sticky: true }
			);
			markerLayer.addLayer(m);
			shown++;
		}
		state.counts = { shown: shown, otherFloor: otherFloor, offMap: offMap };
		drawCalibration();
		setStatus();
	}

	function setStatus() {
		var shown = state.counts.shown, otherFloor = state.counts.otherFloor, offMap = state.counts.offMap;
		var bits = [shown + ' bot' + (shown === 1 ? '' : 's') + ' shown'];
		if (state.layer === 'floors' && otherFloor) { bits.push(otherFloor + ' on other floors'); }
		if (offMap) { bits.push(offMap + ' off-map'); }
		if (state.failed) {
			el.status.className = 'wm-status wm-warn';
			bits.push('connection lost — retrying');
		} else {
			el.status.className = 'wm-status';
			if (state.lastOk) { bits.push('updated ' + agoText(state.lastOk)); }
		}
		el.status.textContent = bits.join(' · ');
	}

	function agoText(ts) {
		var s = Math.max(0, Math.round((Date.now() - ts) / 1000));
		if (s < 60) { return s + 's ago'; }
		return Math.round(s / 60) + 'm ago';
	}

	function escapeHtml(s) {
		return String(s).replace(/[&<>"']/g, function (c) {
			return ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c];
		});
	}

	function fetchData() {
		var url = CFG.dataUrl + '&_=' + Date.now();
		fetch(url, { credentials: 'same-origin' })
			.then(function (r) {
				if (!r.ok) { throw new Error('HTTP ' + r.status); }
				return r.json();
			})
			.then(function (d) {
				state.bots = (d && d.markers) || [];
				state.lastOk = Date.now();
				state.failed = false;
				draw();
			})
			.catch(function () {
				// Keep last-known markers on screen rather than blanking the map.
				state.failed = true;
				setStatus();
			});
	}

	function selectLayer(name) {
		if (!LAYERS[name] || state.layer === name) { return; }
		state.layer = name;
		var btns = document.querySelectorAll('.wm-layer');
		for (var i = 0; i < btns.length; i++) {
			btns[i].classList.toggle('is-active', btns[i].getAttribute('data-layer') === name);
		}
		el.floorBox.hidden = (name !== 'floors');
		setOverlay(true);
		draw();
	}

	function setFloor(f) {
		f = Math.max(0, Math.min(15, f));
		if (f === state.floor) { return; }
		state.floor = f;
		el.floorLabel.textContent = f;
		if (state.layer === 'floors') {
			overlay.setUrl(cur().url(f));
			draw();
		}
	}

	// Calibration check (?calib=1): pins engine temple POIs on whichever layer is
	// active. Every pin must land on its town; a miss means an origin constant in
	// config.php is wrong, and there is nothing else that could be wrong.
	function drawCalibration() {
		if (!CALIB) { return; }
		calibLayer.clearLayers();
		var L_ = cur();
		TEMPLES.forEach(function (t) {
			L.circleMarker(toLatLng(L_, t[1], t[2]), {
				renderer: canvas, radius: 7, weight: 2, color: '#ff00aa', fill: false
			}).bindTooltip(t[0] + ' temple ' + t[1] + ',' + t[2] + ' z' + t[3],
				{ className: 'wm-tip', sticky: true }).addTo(calibLayer);
		});
	}

	// ---- wiring -----------------------------------------------------------
	var layerBtns = document.querySelectorAll('.wm-layer');
	for (var i = 0; i < layerBtns.length; i++) {
		layerBtns[i].addEventListener('click', function (e) {
			selectLayer(e.currentTarget.getAttribute('data-layer'));
		});
	}
	// "Up" is toward floor 0 (z decreases going up in Tibia).
	document.getElementById('wm-floor-up').addEventListener('click', function () { setFloor(state.floor - 1); });
	document.getElementById('wm-floor-down').addEventListener('click', function () { setFloor(state.floor + 1); });
	document.addEventListener('keydown', function (e) {
		if (e.key === 'PageUp') { setFloor(state.floor - 1); e.preventDefault(); }
		if (e.key === 'PageDown') { setFloor(state.floor + 1); e.preventDefault(); }
	});

	el.floorLabel.textContent = state.floor;
	el.floorBox.hidden = (state.layer !== 'floors');
	setOverlay(true);
	if (CALIB) {
		// Handle for manual/automated verification. Only exists in calib mode.
		window.WORLDMAP_DEBUG = { map: map, state: state, LAYERS: LAYERS, toLatLng: toLatLng };
	}

	fetchData();
	setInterval(fetchData, POLL_MS);
	setInterval(function () {
		if (!state.failed && state.lastOk) { setStatus(); }
	}, 15000);
})();
