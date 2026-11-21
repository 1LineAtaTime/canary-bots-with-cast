-- harness_ctl: bot test harness control mod (Phase 1 + perception, perceive.lua).
--
-- Inert unless the client was started with OTC_HARNESS_HUB set (ws://127.0.0.1:<port>).
-- OTC_HARNESS_INSTANCE names this client (p1, c1, ...). Everything else - login target,
-- session key, record file - arrives from the hub as a `login` verb, so no credential is
-- ever written to disk by this mod.
--
-- Wire protocol (JSON text frames):
--   client -> hub : {t="hello", name, cms, ems, ffi, writeDir}
--                   {t="ev", k=<kind>, seq, cms, ems?, ...fields}
--                   {t="reply", id, ok, text, cms, ems?, ...}
--                   {t="pong", id, cms, ems?}
--   hub -> client : {t="cmd", id, verb, args={...}}
--                   {t="ping", id}
-- cms = g_clock.realMillis() (process steady clock), ems = wall-clock epoch ms via LuaJIT
-- ffi GetSystemTimePreciseAsFileTime when available.
--
-- Every verb replies exactly once: `OK <verb> ...` or `STUCK verb=<v> reason=<r> [pos=x,y,z]`.
-- Long verbs (goto, walk, wait_see, wait_gone) carry their own deadline, so a reply always comes.

HarnessCtl = {}

local hubUrl = os.getenv('OTC_HARNESS_HUB')
local instance = os.getenv('OTC_HARNESS_INSTANCE') or 'default'
-- Hidden by default when launched by the hub (OTC_HARNESS_HIDDEN=1). OTClient's run() calls
-- g_window.show() AFTER init.lua and the mods load, so a hide during load is undone: the window
-- is moved off-screen during load (the show then happens off-screen) and hidden in g_app.onRun.
-- SW_HIDE does not set m_visible=false (only WM_SIZE minimise does), so the client keeps
-- rendering and every Lua/network path behaves exactly as when visible.
local hidden = os.getenv('OTC_HARNESS_HIDDEN') == '1'

local PLAYER_FIRST_ID = 0x10000000
local BOT_CID_MIN = PLAYER_FIRST_ID + 65001
local BOT_CID_MAX = PLAYER_FIRST_ID + 66004

local ws = nil
local wsOpen = false
local outQueue = {}
local seq = 0
local verbs = {}
local gotoState = nil
local gotoEvent = nil
local statEvent = nil
local reconnectEvent = nil
local terminated = false
local pollers = {}       -- id -> scheduled event of a polling verb (wait_see / wait_gone / walk)

-- ---------------------------------------------------------------- clocks
local epochMs = nil
do
    local ok, ffi = pcall(require, 'ffi')
    if ok and ffi and ffi.os == 'Windows' then
        local ok2 = pcall(function()
            ffi.cdef [[
                typedef struct { uint32_t lo; uint32_t hi; } HARNESS_FILETIME;
                void GetSystemTimePreciseAsFileTime(HARNESS_FILETIME* ft);
            ]]
        end)
        if ok2 then
            local ft = ffi.new('HARNESS_FILETIME')
            epochMs = function()
                ffi.C.GetSystemTimePreciseAsFileTime(ft)
                return math.floor((ft.hi * 4294967296 + ft.lo) / 10000 - 11644473600000)
            end
            local ok3 = pcall(epochMs)
            if not ok3 then epochMs = nil end
        end
    end
end

local function cms() return g_clock.realMillis() end

local function stamp(t)
    t.cms = cms()
    if epochMs then t.ems = epochMs() end
    return t
end

-- ---------------------------------------------------------------- transport
local function rawSend(s)
    if ws and wsOpen then
        g_http.wsSend(ws.id, s)
    else
        if #outQueue < 20000 then table.insert(outQueue, s) end
    end
end

local function send(t)
    local ok, s = pcall(json.encode, t)
    if not ok then
        s = json.encode({ t = 'ev', k = 'encode_error', err = tostring(s), cms = cms() })
    end
    rawSend(s)
end

local function emit(kind, fields)
    fields = fields or {}
    seq = seq + 1
    fields.t = 'ev'
    fields.k = kind
    fields.seq = seq
    send(stamp(fields))
end
HarnessCtl.emit = emit

local function reply(id, ok, text, extra)
    local t = extra or {}
    t.t = 'reply'; t.id = id; t.ok = ok; t.text = text
    send(stamp(t))
end

local function posT(p)
    if not p then return nil end
    return { x = p.x, y = p.y, z = p.z }
end

local function posStr(p)
    if not p then return '?' end
    return string.format('%d,%d,%d', p.x, p.y, p.z)
end

local function isBotCid(cid) return cid >= BOT_CID_MIN and cid <= BOT_CID_MAX end

local function ctypeOf(c)
    if c:isLocalPlayer() then return 'self' end
    if c:isPlayer() then return 'player' end
    if c:isNpc() then return 'npc' end
    if c:isMonster() then return 'monster' end
    return 'creature'
end

local function creatureInfo(c)
    local cid = c:getId()
    return { cid = cid, name = c:getName(), ctype = ctypeOf(c), pos = posT(c:getPosition()),
        bot = (c:isPlayer() and isBotCid(cid)) or nil }
end

local function myPos()
    local lp = g_game.getLocalPlayer()
    return lp and lp:getPosition() or nil
end

local function stuck(id, verb, reason, extra)
    local p = myPos()
    local text = 'STUCK verb=' .. verb .. ' reason=' .. reason
    if p then text = text .. ' pos=' .. posStr(p) end
    reply(id, false, text, extra or { pos = posT(p) })
end

-- ---------------------------------------------------------------- hub connection
local onWsMessage

local function connectHub()
    if terminated or not hubUrl then return end
    wsOpen = false
    ws = HTTP.WebSocket(hubUrl, {
        onOpen = function(message, id)
            wsOpen = true
            local wd = nil
            pcall(function() wd = g_resources.getWriteDir() end)
            send(stamp({ t = 'hello', name = instance, ffi = epochMs ~= nil,
                version = g_app.getVersion(), fps = g_app.getFps(), writeDir = wd, hidden = hidden }))
            local q = outQueue
            outQueue = {}
            for _, s in ipairs(q) do g_http.wsSend(ws.id, s) end
        end,
        onMessage = function(message, id)
            local ok, err = pcall(onWsMessage, message)
            if not ok then emit('ctl_error', { where = 'onMessage', err = tostring(err) }) end
        end,
        onClose = function(message, id)
            wsOpen = false
            if not terminated then reconnectEvent = scheduleEvent(connectHub, 2000) end
        end,
        onError = function(message, id)
            wsOpen = false
            g_logger.warning('[harness_ctl] ws error: ' .. tostring(message))
            if not terminated then reconnectEvent = scheduleEvent(connectHub, 2000) end
        end
    }, 5)
end

onWsMessage = function(message)
    local m = json.decode(message)
    if type(m) ~= 'table' then return end
    if m.t == 'ping' then
        send(stamp({ t = 'pong', id = m.id }))
    elseif m.t == 'cmd' then
        local fn = verbs[m.verb]
        if not fn then
            reply(m.id, false, 'STUCK verb=' .. tostring(m.verb) .. ' reason=unknown-verb')
            return
        end
        local ok, err = pcall(fn, m.id, m.args or {})
        if not ok then reply(m.id, false, 'STUCK verb=' .. m.verb .. ' reason=lua-error ' .. tostring(err)) end
    end
end

-- ---------------------------------------------------------------- event handlers
local lastMoveKey = {}   -- cid -> {key, t}  (also the "is moving" signal for wait_see moving=1)

local function onCreatureAppear(c)
    local info = creatureInfo(c)
    info.hp = c:getHealthPercent()
    info.skull = c:getSkull()
    if HarnessPerceive then pcall(HarnessPerceive.onAppear, c, info) end
    emit('see_appear', info)
end

local function onCreatureDisappear(c)
    if HarnessPerceive then pcall(HarnessPerceive.onDisappear, c) end
    emit('see_disappear', creatureInfo(c))
end

local function onCreaturePositionChange(c, newPos, oldPos)
    if not newPos then return end
    local cid = c:getId()
    -- Creature and LocalPlayer both carry this signal; collapse the duplicate for self.
    local key = newPos.x .. ',' .. newPos.y .. ',' .. newPos.z
    local now = cms()
    local last = lastMoveKey[cid]
    if last and last.key == key and now - last.t < 50 then return end
    lastMoveKey[cid] = { key = key, t = now }
    local info = { cid = cid, name = c:getName(), to = posT(newPos), from = posT(oldPos) }
    if c:isLocalPlayer() then
        emit('self_pos', info)
    else
        info.ctype = ctypeOf(c)
        emit('see_move', info)
    end
end

-- #170: the Cast channel tab is the operator surface; report every open-channel packet and,
-- one frame later, whether game_console actually holds a tab for it.
local function consoleHasChannel(channelId)
    local con = modules.game_console
    if not con or not con.getChannelTab then return nil end
    local ok, tab = pcall(con.getChannelTab, channelId)
    return ok and tab ~= nil
end
local function onOpenChannel(channelId, channelName)
    emit('channel_open', { channel = channelId, name = channelName })
    scheduleEvent(function()
        emit('channel_tab', { channel = channelId, name = channelName, tab = tostring(consoleHasChannel(channelId)) })
    end, 50)
end

local function onTalk(name, level, mode, text, channelId, pos)
    emit('see_talk', { name = name, level = level, mode = mode, text = text, channel = channelId, pos = posT(pos) })
    if HarnessPerceive then pcall(HarnessPerceive.onTalk, name, level, mode, text, channelId, pos) end
end

local MODE_NAME = {}
for k, v in pairs(MessageModes or {}) do MODE_NAME[v] = k end

-- every server text message (status, loot, look, damage/heal log lines, /cavebot replies, ...);
-- the floating numbers of damage/heal/exp messages are see_text kind=anim (perceive.lua tap)
local function onTextMessage(mode, text)
    if text == nil or text == '' then return end
    emit('see_msg', { mode = mode, mode_name = MODE_NAME[mode], text = text })
end

-- ---------------------------------------------------------------- audio OFF
-- The overlay (init.lua) already persisted enableAudio/enableMusicSound=false and disabled the
-- sound manager before any module ran; this re-asserts it after client_options applied its
-- settings and again on every game start, and reports the state (client_state verb).
local function audioOff()
    pcall(function()
        g_settings.set('enableAudio', false)
        g_settings.set('enableMusicSound', false)
        g_settings.set('musicSoundVolume', 0)
        -- harness clients are terminated, never exit cleanly, so persist now (per-instance config.otml)
        g_settings.save()
    end)
    pcall(function()
        local o = modules.client_options
        if o and o.setOption then
            o.setOption('enableAudio', false, true)
            o.setOption('enableMusicSound', false, true)
            o.setOption('musicSoundVolume', 0, true)
        end
    end)
    if g_sounds then
        pcall(function()
            g_sounds.setAudioEnabled(false)
            for _, ch in pairs(SoundChannels or {}) do
                local c = g_sounds.getChannel(ch)
                if c then c:stop(); c:setEnabled(false); c:setGain(0) end
            end
            g_sounds.stopAll()
        end)
    end
end

local function audioState()
    local st = { sounds = g_sounds ~= nil }
    pcall(function()
        st.audio_enabled = g_sounds and g_sounds.isAudioEnabled() or false
        st.channels = {}
        for name, ch in pairs(SoundChannels or {}) do
            local c = g_sounds.getChannel(ch)
            if c then st.channels[name] = { enabled = c:isEnabled(), gain = c:getGain() } end
        end
    end)
    pcall(function()
        st.settings = { enableAudio = g_settings.getBoolean('enableAudio'),
            enableMusicSound = g_settings.getBoolean('enableMusicSound'),
            musicSoundVolume = g_settings.getNumber('musicSoundVolume') }
        local o = modules.client_options
        if o and o.getOption then
            st.options = { enableAudio = o.getOption('enableAudio'), enableMusicSound = o.getOption('enableMusicSound'),
                musicSoundVolume = o.getOption('musicSoundVolume') }
        end
    end)
    return st
end

local function onGameStart()
    audioOff()
    if hidden then scheduleEvent(function() pcall(function() g_window.hide() end) end, 50) end
    -- loginWorld bypasses the login UI, so its dialog would stay drawn over the game view in shots
    pcall(function() if EnterGame and EnterGame.hide then EnterGame.hide() end end)
    local lp = g_game.getLocalPlayer()
    emit('login_ok', { char = g_game.getCharacterName(), cid = lp and lp:getId() or 0,
        pos = lp and posT(lp:getPosition()) or nil })
end

local function onGameEnd() emit('game_end', {}) end
local function onLoginError(err) emit('login_error', { err = tostring(err) }) end
local function onLoginAdvice(msg) emit('login_advice', { msg = tostring(msg) }) end
local function onLoginWait(msg, time) emit('login_wait', { msg = tostring(msg), time = time }) end
local function onConnectionError(msg, code) emit('conn_error', { msg = tostring(msg), code = code }) end

local function statTick()
    local d = { fps = g_app.getFps(), maxfps = g_app.getMaxFps(), online = g_game.isOnline() }
    if g_game.isOnline() then
        d.ping = g_game.getPing()
        local lp = g_game.getLocalPlayer()
        if lp then d.pos = posT(lp:getPosition()) end
        d.rx_packets = g_game.getRecivedPacketsCount and g_game.getRecivedPacketsCount() or nil
    end
    emit('stat', d)
    statEvent = scheduleEvent(statTick, 2000)
end

-- ---------------------------------------------------------------- helpers
local function chebyshev(a, b)
    return math.max(math.abs(a.x - b.x), math.abs(a.y - b.y))
end

local DIRS = {
    n = North, north = North, e = East, east = East, s = South, south = South, w = West, west = West,
    ne = NorthEast, northeast = NorthEast, se = SouthEast, southeast = SouthEast,
    sw = SouthWest, southwest = SouthWest, nw = NorthWest, northwest = NorthWest
}
local DIRNAME = { [North] = 'n', [East] = 'e', [South] = 's', [West] = 'w', [NorthEast] = 'ne',
    [SouthEast] = 'se', [SouthWest] = 'sw', [NorthWest] = 'nw' }

local function parseDir(d)
    if type(d) == 'number' then return d end
    return DIRS[string.lower(tostring(d or ''))]
end

local function tileAt(p) return g_map.getTile({ x = p.x, y = p.y, z = p.z }) end

-- walkable ignoring creatures (terrain only)
local function terrainWalkable(p)
    local t = tileAt(p)
    return t ~= nil and t:isWalkable(true)
end

-- walkable right now (terrain AND no blocking creature)
local function freeNow(p)
    local t = tileAt(p)
    return t ~= nil and t:isWalkable(false)
end

local function hasBlockingCreature(p)
    local t = tileAt(p)
    return t ~= nil and t:isWalkable(true) and not t:isWalkable(false)
end

-- client-side path over known tiles; flags: 1 = allow not-seen tiles
local function findPath(from, to, flags)
    local ok, dirs, res = pcall(g_map.findPath, from, to, 20000, flags or 1)
    if not ok or res ~= 0 or not dirs or #dirs == 0 then return nil, res end
    return dirs, res
end

local function findCreatureByName(name, multiFloor)
    local p = myPos()
    if not p then return nil end
    local lname = string.lower(name)
    for _, c in ipairs(g_map.getSpectators(p, multiFloor and true or false)) do
        if string.lower(c:getName()) == lname then return c end
    end
    return nil
end

-- ---------------------------------------------------------------- verbs
local function waitThingsThen(deadline, fn, failFn)
    if modules.game_things and modules.game_things.isLoaded() then return fn() end
    if cms() > deadline then return failFn() end
    scheduleEvent(function() waitThingsThen(deadline, fn, failFn) end, 100)
end

verbs.login = function(id, a)
    if g_game.isOnline() then
        if not (g_game.isDead and g_game.isDead()) then
            return reply(id, false, 'STUCK verb=login reason=already-online')
        end
        -- Dead: the client stays "online" behind the death window (its OK button would relogin
        -- through the account list, not our session key). Leave the game, then log in again.
        emit('relogin_after_death', {})
        pcall(g_game.forceLogout)
        local deadline = cms() + 15000
        local function again()
            if g_game.isOnline() then
                if cms() > deadline then return reply(id, false, 'STUCK verb=login reason=dead-logout-timeout') end
                return scheduleEvent(again, 200)
            end
            verbs.login(id, a)
        end
        scheduleEvent(again, 300)
        return
    end
    -- The runner always sends harness.json client_version (1525 for the port, 1500 for an M0 control
    -- arm). No silent default: a wrong version is a disconnect with no useful message.
    local version = tonumber(a.version)
    if not version then
        return reply(id, false, 'STUCK verb=login reason=no-client-version')
    end
    g_game.setClientVersion(version)
    g_game.setProtocolVersion(g_game.getClientProtocolVersion(version))
    g_game.chooseRsa(a.host)
    waitThingsThen(cms() + 30000, function()
        local before = { cms = cms(), frame_ms = g_clock.millis(), ems = epochMs and epochMs() or nil }
        g_game.loginWorld('', '', a.world or 'Canary', a.host, a.port, a.char, '', a.sessionKey, a.recordTo or '')
        local after = { cms = cms(), frame_ms = g_clock.millis(), ems = epochMs and epochMs() or nil }
        -- PacketRecorder m_start = g_clock.millis() (frame-cached) taken inside loginWorld, so
        -- record epoch(.cam t=0) = ems - (cms - frame_ms) at the same instant.
        emit('record_start', { file = a.recordTo or '', before = before, after = after })
        reply(id, true, 'OK login sent char=' .. tostring(a.char))
    end, function()
        reply(id, false, 'STUCK verb=login reason=things-not-loaded')
    end)
end

-- ---- goto: client-side path planning over the known map, re-planned whenever the walk
-- stalls. Handles: occupied destination (waits, or uses `range`), crowds on the way (re-plan
-- around current creature positions), far targets (walk to the known tile nearest the
-- destination, then re-plan), unwalkable destination (approach, then STUCK dest-unwalkable).
local function gotoFinish(st, ok, text, extra)
    gotoState = nil
    if gotoEvent then removeEvent(gotoEvent); gotoEvent = nil end
    reply(st.id, ok, text, extra)
end

local function gotoPlan(st, pos)
    local dest = st.dest
    -- 1) straight to the destination (or a tile within range of it)
    local goals = {}
    if freeNow(dest) then table.insert(goals, dest) end
    local r = math.max(st.range, 1)
    local ring = {}
    for dx = -r, r do
        for dy = -r, r do
            if dx ~= 0 or dy ~= 0 then
                local p = { x = dest.x + dx, y = dest.y + dy, z = dest.z }
                if freeNow(p) then table.insert(ring, p) end
            end
        end
    end
    table.sort(ring, function(a, b)
        local da, db = chebyshev(a, dest), chebyshev(b, dest)
        if da ~= db then return da < db end
        return chebyshev(a, pos) < chebyshev(b, pos)
    end)
    for _, p in ipairs(ring) do table.insert(goals, p) end
    for i, g in ipairs(goals) do
        if i > 12 then break end
        local dirs = findPath(pos, g, 1)
        if dirs then return dirs, g, 'direct' end
    end
    -- 2) far / blocked: walk to the reachable known tile closest to the destination
    local here = chebyshev(pos, dest)
    local cands = {}
    for _, t in ipairs(g_map.getTiles(pos.z)) do
        local tp = t:getPosition()
        local d = chebyshev(tp, dest)
        if d < here and t:isWalkable(false) and chebyshev(tp, pos) >= 1 then
            table.insert(cands, { p = tp, d = d })
        end
    end
    table.sort(cands, function(a, b) return a.d < b.d end)
    for i, c in ipairs(cands) do
        if i > 20 then break end
        local dirs = findPath(pos, c.p, 0)
        if dirs then return dirs, c.p, 'chunk' end
    end
    return nil, nil, 'no-path'
end

local function gotoCheck()
    gotoEvent = nil
    local st = gotoState
    if not st then return end
    local lp = g_game.getLocalPlayer()
    if not lp or not g_game.isOnline() then
        gotoState = nil
        return reply(st.id, false, 'STUCK verb=goto reason=offline')
    end
    local pos = lp:getPosition()
    local now = cms()
    local fmtPos = posStr(pos)
    if pos.z ~= st.dest.z then
        return gotoFinish(st, false, 'STUCK verb=goto reason=other-floor pos=' .. fmtPos, { pos = posT(pos) })
    end
    local dist = chebyshev(pos, st.dest)
    if dist <= st.range then
        return gotoFinish(st, true, string.format('OK goto pos=%s dist=%d ms=%d plans=%d', fmtPos, dist,
            now - st.t0, st.plans), { pos = posT(pos) })
    end
    -- destination itself can never be stood on: approach, then report
    local destKnown = tileAt(st.dest) ~= nil
    if destKnown and not terrainWalkable(st.dest) and dist <= 1 then
        return gotoFinish(st, false, 'STUCK verb=goto reason=dest-unwalkable pos=' .. fmtPos, { pos = posT(pos) })
    end
    if not st.lastPos or st.lastPos.x ~= pos.x or st.lastPos.y ~= pos.y then
        st.lastPos = posT(pos)
        st.lastProgress = now
    end
    if now - st.lastProgress > st.stuckMs then
        local reason = 'no-progress'
        if dist <= 1 and hasBlockingCreature(st.dest) then
            reason = 'blocked-by-creature'
        elseif destKnown and not terrainWalkable(st.dest) then
            reason = 'dest-unwalkable'
        elseif st.lastPlan == 'no-path' then
            reason = 'no-path'
        end
        return gotoFinish(st, false, 'STUCK verb=goto reason=' .. reason .. ' pos=' .. fmtPos, { pos = posT(pos) })
    end
    if now > st.deadline then
        return gotoFinish(st, false, 'STUCK verb=goto reason=timeout pos=' .. fmtPos, { pos = posT(pos) })
    end
    local moving = lp:isWalking() or lp:isServerWalking() or lp:isAutoWalking()
    if not moving and now - st.lastProgress > 400 and now - st.lastIssue > 700 then
        st.lastIssue = now
        -- adjacent to an occupied exact destination: just wait for it to clear
        if not (st.range == 0 and dist == 1 and hasBlockingCreature(st.dest)) then
            local dirs, goal, how = gotoPlan(st, pos)
            st.lastPlan = how
            st.plans = st.plans + 1
            if dirs then
                if #dirs > 127 then
                    local cut = {}
                    for i = 1, 127 do cut[i] = dirs[i] end
                    dirs = cut
                end
                g_game.autoWalk(dirs, pos)
            end
        end
    end
    gotoEvent = scheduleEvent(gotoCheck, 100)
end

verbs['goto'] = function(id, a)
    local lp = g_game.getLocalPlayer()
    if not lp or not g_game.isOnline() then return reply(id, false, 'STUCK verb=goto reason=offline') end
    if gotoState then gotoFinish(gotoState, false, 'STUCK verb=goto reason=superseded') end
    local now = cms()
    gotoState = { id = id, dest = { x = a.x, y = a.y, z = a.z }, range = a.range or 0, t0 = now,
        deadline = now + (a.timeout_s or 120) * 1000, stuckMs = (a.stuck or 10) * 1000,
        lastProgress = now, lastPos = posT(lp:getPosition()), lastIssue = 0, plans = 0 }
    gotoEvent = scheduleEvent(gotoCheck, 10)
end

verbs.stop = function(id, a)
    if gotoState then gotoFinish(gotoState, false, 'STUCK verb=goto reason=stopped') end
    pcall(g_game.cancelAttackAndFollow)
    pcall(g_game.stop)
    local lp = g_game.getLocalPlayer()
    if lp then pcall(function() lp:stopAutoWalk() end) end
    reply(id, true, 'OK stop pos=' .. posStr(myPos()), { pos = posT(myPos()) })
end

verbs.walk = function(id, a)
    local dir = parseDir(a.dir)
    if not dir then return reply(id, false, 'STUCK verb=walk reason=bad-dir') end
    local n = a.n or 1
    local done = 0
    local stepDeadline = 0
    local startPos = nil
    local function step()
        pollers[id] = nil
        if not g_game.isOnline() then return stuck(id, 'walk', 'offline') end
        local p = myPos()
        if startPos and (p.x ~= startPos.x or p.y ~= startPos.y or p.z ~= startPos.z) then
            done = done + 1
            startPos = nil
        end
        if done >= n then
            return reply(id, true, string.format('OK walk dir=%s steps=%d pos=%s', DIRNAME[dir] or dir, done, posStr(p)),
                { pos = posT(p) })
        end
        local lp = g_game.getLocalPlayer()
        if not startPos then
            if lp:isWalking() then
                pollers[id] = scheduleEvent(step, 50)
                return
            end
            startPos = posT(p)
            stepDeadline = cms() + 1500
            g_game.walk(dir)
        elseif cms() > stepDeadline then
            return stuck(id, 'walk', 'blocked step=' .. (done + 1))
        end
        pollers[id] = scheduleEvent(step, 50)
    end
    step()
end

verbs.face = function(id, a)
    local dir = parseDir(a.dir)
    if not dir or dir > West then return reply(id, false, 'STUCK verb=face reason=bad-dir') end
    g_game.turn(dir)
    reply(id, true, 'OK face dir=' .. (DIRNAME[dir] or dir))
end

verbs.follow = function(id, a)
    local c = findCreatureByName(a.name or '', false)
    if not c then return stuck(id, 'follow', 'not-visible') end
    g_game.cancelAttackAndFollow()
    g_game.follow(c)
    reply(id, true, string.format('OK follow name=%s cid=%d', c:getName(), c:getId()), { cid = c:getId() })
end

verbs.say = function(id, a)
    if a.npc then
        -- after an NPC greeting the real client talks in the NPCs channel (P5 V11c)
        g_game.talkChannel(MessageModes.NpcTo, 0, a.text)
        return reply(id, true, 'OK say npc')
    end
    g_game.talk(a.text)
    reply(id, true, 'OK say')
end

verbs.pm = function(id, a)
    g_game.talkPrivate(MessageModes.PrivateTo, a.name, a.text)
    reply(id, true, 'OK pm to=' .. tostring(a.name))
end

-- attack <name> | attack stop. Safe-fight OFF (else the client refuses to attack an unmarked player)
-- and chase ON so a melee character closes the distance itself.
verbs.attack = function(id, a)
    if not g_game.isOnline() then return stuck(id, 'attack', 'offline') end
    if (a.name or '') == 'stop' then
        pcall(g_game.cancelAttack)
        return reply(id, true, 'OK attack stop')
    end
    local c = findCreatureByName(a.name or '', false)
    if not c then return stuck(id, 'attack', 'not-visible') end
    pcall(g_game.setSafeFight, false)
    pcall(g_game.setChaseMode, 1)
    g_game.attack(c)
    reply(id, true, string.format('OK attack name=%s cid=%d', c:getName(), c:getId()), { cid = c:getId() })
end

-- party invite <name> | party leave
verbs.party = function(id, a)
    if not g_game.isOnline() then return stuck(id, 'party', 'offline') end
    if a.op == 'leave' then
        g_game.partyLeave()
        return reply(id, true, 'OK party leave')
    end
    if a.op == 'say' then
        -- #91: the party channel (id 1) must be joined before the server accepts talk in it
        pcall(g_game.joinChannel, 1)
        scheduleEvent(function()
            g_game.talkChannel(MessageModes.Channel, 1, a.text or '')
            reply(id, true, 'OK party say')
        end, 600)
        return
    end
    local c = findCreatureByName(a.name or '', false)
    if not c then return stuck(id, 'party', 'not-visible') end
    g_game.partyInvite(c:getId())
    reply(id, true, string.format('OK party invite name=%s cid=%d', c:getName(), c:getId()), { cid = c:getId() })
end

-- trade <name> [slot=1] | trade cancel. Offers the item in inventory slot `slot`; the server's
-- answer arrives as the trade_own / trade_close events below.
verbs.trade = function(id, a)
    if not g_game.isOnline() then return stuck(id, 'trade', 'offline') end
    if (a.name or '') == 'cancel' then
        g_game.rejectTrade()
        return reply(id, true, 'OK trade cancel')
    end
    local lp = g_game.getLocalPlayer()
    local c = findCreatureByName(a.name or '', false)
    if not c then return stuck(id, 'trade', 'not-visible') end
    local item = lp:getInventoryItem(a.slot or 1)
    if not item then return stuck(id, 'trade', 'no-item-in-slot') end
    g_game.requestTrade(item, c)
    reply(id, true, string.format('OK trade name=%s cid=%d item=%d', c:getName(), c:getId(), item:getId()),
        { cid = c:getId() })
end

local function onOwnTrade(name, items) emit('trade_own', { name = tostring(name), n = items and #items or 0 }) end
local function onCounterTrade(name, items) emit('trade_counter', { name = tostring(name), n = items and #items or 0 }) end
local function onCloseTrade() emit('trade_close', {}) end
local function onOpenNpcTrade(items) emit('npc_trade', { n = items and #items or 0 }) end

-- #26: drive the bot-control panel (mods/game_botcontrol) like a player clicking it.
verbs.botpanel = function(id, a)
    local m = modules.game_botcontrol
    if not m or not m.dump then return stuck(id, 'botpanel', 'module-missing') end
    local op = a.op or 'dump'
    if op == 'open' then m.show()
    elseif op == 'close' then m.hide()
    elseif op == 'select' then m.selectBot(a.arg)
    elseif op == 'click' then
        local sent, why = m.click(a.arg)
        if not sent then return stuck(id, 'botpanel', why or 'click-failed') end
        return reply(id, true, 'OK botpanel click sent=' .. sent .. ' ' .. m.dump())
    end
    reply(id, true, 'OK botpanel ' .. op .. ' ' .. m.dump())
end

verbs.channel_say = function(id, a)
    g_game.talkChannel(a.mode or MessageModes.Channel, a.channel, a.text)
    reply(id, true, 'OK channel_say channel=' .. tostring(a.channel))
end

-- wait_see: name=<exact name> | any=bot|player|monster|npc ; exclude={names} ; moving=true
-- (moved within the last 3 s) ; timeout_s
verbs.wait_see = function(id, a)
    local deadline = cms() + (a.timeout_s or 30) * 1000
    local excl = {}
    for _, n in ipairs(a.exclude or {}) do excl[string.lower(n)] = true end
    local want = a.name and string.lower(a.name) or nil
    local function match(c)
        if c:isLocalPlayer() then return false end
        local lname = string.lower(c:getName())
        if excl[lname] then return false end
        if want and lname ~= want then return false end
        if a.any == 'bot' and not (c:isPlayer() and isBotCid(c:getId())) then return false end
        if a.any == 'player' and not (c:isPlayer() and not isBotCid(c:getId())) then return false end
        if a.any == 'monster' and not c:isMonster() then return false end
        if a.any == 'npc' and not c:isNpc() then return false end
        if a.moving then
            local lm = lastMoveKey[c:getId()]
            if not lm or cms() - lm.t > 3000 then return false end
        end
        return true
    end
    local function poll()
        pollers[id] = nil
        if not g_game.isOnline() then return stuck(id, 'wait_see', 'offline') end
        local p = myPos()
        local best, bestD = nil, 999
        for _, c in ipairs(g_map.getSpectators(p, a.multifloor and true or false)) do
            if match(c) then
                local d = chebyshev(c:getPosition(), p)
                if d < bestD then best, bestD = c, d end
            end
        end
        if best then
            local cp = best:getPosition()
            return reply(id, true, string.format('OK wait-see name=%s cid=%d pos=%s dist=%d', best:getName(),
                best:getId(), posStr(cp), bestD), { name = best:getName(), cid = best:getId(), cpos = posT(cp) })
        end
        if cms() > deadline then return stuck(id, 'wait-see', 'timeout') end
        pollers[id] = scheduleEvent(poll, 200)
    end
    poll()
end

verbs.wait_gone = function(id, a)
    local deadline = cms() + (a.timeout_s or 30) * 1000
    local function poll()
        pollers[id] = nil
        if not g_game.isOnline() then return stuck(id, 'wait-gone', 'offline') end
        if not findCreatureByName(a.name or '', false) then
            return reply(id, true, 'OK wait-gone name=' .. tostring(a.name))
        end
        if cms() > deadline then return stuck(id, 'wait-gone', 'timeout') end
        pollers[id] = scheduleEvent(poll, 200)
    end
    poll()
end

-- dump_view: creatures + ASCII of the known tiles on the current floor.
-- legend: @ self  B bot  P player  M monster  N npc  C other creature
--         . walkable  # not walkable  = floor change  (space) unknown
verbs.dump_view = function(id, a)
    local lp = g_game.getLocalPlayer()
    if not lp then return reply(id, false, 'STUCK verb=dump_view reason=offline') end
    local p = lp:getPosition()
    local list = {}
    local at = {}
    for _, c in ipairs(g_map.getSpectators(p, a.multifloor and true or false)) do
        local info = creatureInfo(c)
        table.insert(list, info)
        local cp = c:getPosition()
        if cp.z == p.z then
            local ch = 'C'
            if info.ctype == 'self' then ch = '@'
            elseif info.ctype == 'player' then ch = info.bot and 'B' or 'P'
            elseif info.ctype == 'monster' then ch = 'M'
            elseif info.ctype == 'npc' then ch = 'N' end
            local k = cp.x .. ',' .. cp.y
            if at[k] ~= '@' then at[k] = ch end
        end
    end
    local rx, ry = a.rx or 8, a.ry or 6
    local rows = {}
    for dy = -ry, ry do
        local line = {}
        for dx = -rx, rx do
            local tp = { x = p.x + dx, y = p.y + dy, z = p.z }
            local ch = at[tp.x .. ',' .. tp.y]
            if not ch then
                local t = tileAt(tp)
                if not t then ch = ' '
                elseif t:hasFloorChange() then ch = '='
                elseif t:isWalkable(true) then ch = '.'
                else ch = '#' end
            end
            table.insert(line, ch)
        end
        table.insert(rows, table.concat(line))
    end
    reply(id, true, 'OK dump-view n=' .. #list .. ' pos=' .. posStr(p),
        { creatures = list, pos = posT(p), ascii = rows, origin = { x = p.x - rx, y = p.y - ry, z = p.z } })
end

verbs.set_fps = function(id, a)
    g_app.setMaxFps(a.fps or 0)
    reply(id, true, 'OK set-fps ' .. tostring(a.fps), { maxfps = g_app.getMaxFps() })
end

-- shot: PNG written asynchronously into the client's write dir; the hub moves it into the run dir
local function harnessHide()
    if not hidden then return end
    pcall(function() g_window.hide() end)
end

-- shot: PNG written asynchronously into the client's write dir; the hub moves it into the run dir.
--   mode=window (default): g_app.doScreenshot - reads the window back buffer
--   mode=map            : g_app.doMapScreenshot - reads the map framebuffer (FBO)
--   reveal=true         : while hidden, briefly show the window on-screen, capture, hide again
verbs.shot = function(id, a)
    local file = a.file or ('harness_' .. instance .. '.png')
    local wd = nil
    pcall(function() wd = g_resources.getWriteDir() end)
    local function take()
        if a.mode == 'map' then g_app.doMapScreenshot(file) else g_app.doScreenshot(file) end
    end
    if hidden and a.reveal then
        g_window.move({ x = 40, y = 40 })
        g_window.show()
        scheduleEvent(function()
            take()
            scheduleEvent(function()
                g_window.hide()
                g_window.move({ x = -32000, y = -32000 })
                reply(id, true, 'OK shot file=' .. file .. ' mode=' .. (a.mode or 'window') .. ' revealed=1',
                    { file = file, writeDir = wd, hidden = hidden })
            end, 400)
        end, a.reveal_ms or 700)
        return
    end
    take()
    reply(id, true, 'OK shot file=' .. file .. ' mode=' .. (a.mode or 'window') .. ' hidden=' .. tostring(hidden),
        { file = file, writeDir = wd, hidden = hidden })
end

-- client_state: process-level state of this client (audio, window, fps, protocol)
verbs.client_state = function(id, a)
    local au = audioState()
    local chans = {}
    for name, c in pairs(au.channels or {}) do
        table.insert(chans, string.format('%s=%s/%.2f', name, tostring(c.enabled), c.gain or -1))
    end
    table.sort(chans)
    local text = string.format('OK client-state audio_enabled=%s channels=[%s] setting_enableAudio=%s setting_enableMusicSound=%s setting_musicSoundVolume=%s hidden=%s fps=%s maxfps=%s online=%s version=%s',
        tostring(au.audio_enabled), table.concat(chans, ' '), tostring(au.settings and au.settings.enableAudio),
        tostring(au.settings and au.settings.enableMusicSound), tostring(au.settings and au.settings.musicSoundVolume),
        tostring(hidden), tostring(g_app.getFps()), tostring(g_app.getMaxFps()), tostring(g_game.isOnline()),
        tostring(g_game.getClientVersion()))
    reply(id, true, text, { audio = au, hidden = hidden })
end

-- ---- P5 (#160) V11c: the REAL login UI path (EnterGame HTTP login -> CharacterList), not loginWorld.
local function charListNames()
    local root = g_ui.getRootWidget()
    local list = root and root:recursiveGetChildById('characters')
    local names = {}
    if list then
        for _, w in ipairs(list:getChildren()) do
            if w.characterName then table.insert(names, w.characterName) end
        end
    end
    return names, list
end

verbs.uilogin = function(id, a)
    if g_game.isOnline() then return reply(id, false, 'STUCK verb=uilogin reason=already-online') end
    local version = tonumber(a.version)
    if not version then return reply(id, false, 'STUCK verb=uilogin reason=no-client-version') end
    G.account, G.password, G.authenticatorToken = a.account or '', a.password or '', ''
    G.host, G.port = a.url, tonumber(a.port)
    pcall(function() CharacterList.destroy() end)
    -- same as verbs.login: the version selects things/<version>, which only then start loading
    g_game.setClientVersion(version)
    g_game.setProtocolVersion(g_game.getClientProtocolVersion(version))
    waitThingsThen(cms() + 30000, function()
        EnterGame.tryHttpLogin(version, true)
        local deadline = cms() + 20000
        local function poll()
            local names = charListNames()
            if #names > 0 or (CharacterList.isVisible and CharacterList.isVisible()) then
                emit('charlist', { n = #names, names = names })
                return reply(id, true, 'OK uilogin chars=' .. #names .. ' [' .. table.concat(names, ', ', 1, math.min(#names, 12)) ..
                    (#names > 12 and ', ...' or '') .. ']', { names = names })
            end
            if cms() > deadline then return reply(id, false, 'STUCK verb=uilogin reason=no-character-list') end
            scheduleEvent(poll, 200)
        end
        poll()
    end, function() reply(id, false, 'STUCK verb=uilogin reason=things-not-loaded') end)
end

verbs.uichar = function(id, a)
    local names, list = charListNames()
    if not list then return reply(id, false, 'STUCK verb=uichar reason=no-character-list') end
    for _, w in ipairs(list:getChildren()) do
        if w.characterName == a.name then
            list:focusChild(w, KeyboardFocusReason or 0)
            if list:getFocusedChild() ~= w then
                return reply(id, false, 'STUCK verb=uichar reason=focus-failed name=' .. a.name)
            end
            local info = string.format('world=%s:%s session=%s', tostring(w.worldHost), tostring(w.worldPort),
                (G.sessionKey and #G.sessionKey > 0) and 'yes' or 'no')
            -- a.raw=1: keep G.account/G.password as the login form left them (diagnostic)
            if not a.raw then G.account, G.password = '', '' end
            local ok, err = pcall(CharacterList.doLogin)
            if not ok then
                return reply(id, false, 'STUCK verb=uichar reason=doLogin-error ' .. tostring(err) .. ' ' .. info)
            end
            return reply(id, true, 'OK uichar ' .. a.name .. ' (from ' .. #names .. ' listed) ' .. info)
        end
    end
    reply(id, false, 'STUCK verb=uichar reason=not-listed name=' .. tostring(a.name))
end

-- open game UI: which panels exist/are visible (inventory, minimap, spells/actionbar/hotkeys, npc trade, chat tabs)
verbs.uicheck = function(id, a)
    local root = g_ui.getRootWidget()
    local function vis(wid) local w = root and root:recursiveGetChildById(wid); return w and (w:isVisible() and 'shown' or 'hidden') or 'none' end
    local mods = {}
    for _, m in ipairs({ 'game_inventory', 'game_minimap', 'game_spelllist', 'game_actionbar', 'game_hotkeys', 'game_npctrade',
                         'game_console', 'game_skills', 'game_battle' }) do
        local mod = g_modules.getModule(m)
        table.insert(mods, m .. '=' .. (mod and (mod:isLoaded() and 'loaded' or 'off') or 'none'))
    end
    local tabs = {}
    pcall(function()
        local ct = (modules.game_console and modules.game_console.consoleTabBar) or (root and root:recursiveGetChildById('consoleTabBar'))
        for _, t in ipairs(ct.getTabs and ct:getTabs() or ct.tabs or {}) do table.insert(tabs, t:getText()) end
    end)
    local npc = 'closed'
    pcall(function() if modules.game_npctrade.npcWindow and modules.game_npctrade.npcWindow:isVisible() then npc = 'open' end end)
    local gs = string.format(' logging=%s online=%s proto=%s ver=%s/%s', tostring(g_game.isLogging()), tostring(g_game.isOnline()),
        tostring(g_game.getProtocolGame() ~= nil), tostring(g_game.getClientVersion()), tostring(g_game.getProtocolVersion()))
    local text = 'OK uicheck' .. gs .. ' ' .. table.concat(mods, ' ') .. ' npctrade=' .. npc .. ' minimapWindow=' .. vis('minimapWindow') ..
        ' tabs=[' .. table.concat(tabs, ', ') .. ']'
    reply(id, true, text, { modules = mods, tabs = tabs, npctrade = npc })
end

verbs.window = function(id, a)
    if a.show then
        hidden = false
        g_window.move({ x = 40, y = 40 })
        g_window.show()
    else
        hidden = true
        g_window.hide()
    end
    reply(id, true, 'OK window hidden=' .. tostring(hidden))
end

verbs.logout = function(id, a)
    if a.force then g_game.forceLogout() else g_game.safeLogout() end
    reply(id, true, 'OK logout')
end

verbs.quit = function(id, a)
    reply(id, true, 'OK quit')
    scheduleEvent(function() g_app.exit() end, 300)
end

-- ---------------------------------------------------------------- lifecycle
function HarnessCtl.init()
    if not hubUrl or hubUrl == '' then
        g_logger.info('[harness_ctl] OTC_HARNESS_HUB not set - inert')
        return
    end
    g_logger.info('[harness_ctl] instance=' .. instance .. ' hub=' .. hubUrl .. ' ffi_epoch=' .. tostring(epochMs ~= nil))
    audioOff()
    scheduleEvent(audioOff, 1000)   -- after any late module start
    -- R1: tile add/remove things only reach Lua with this on. Magic effects / missiles /
    -- animated texts never pass onAddThing; perceive.lua reads them from the packet tap instead.
    pcall(function() g_game.enableTileThingLuaCallback(true) end)
    if hidden then
        pcall(function() g_window.move({ x = -32000, y = -32000 }) end)
        connect(g_app, { onRun = harnessHide })
        scheduleEvent(harnessHide, 500)   -- fallback if onRun already fired
    end

    connect(g_game, {
        onGameStart = onGameStart,
        onGameEnd = onGameEnd,
        onLoginError = onLoginError,
        onLoginAdvice = onLoginAdvice,
        onLoginWait = onLoginWait,
        onConnectionError = onConnectionError,
        onTalk = onTalk,
        onOpenChannel = onOpenChannel,
        onTextMessage = onTextMessage,
        onOwnTrade = onOwnTrade,
        onCounterTrade = onCounterTrade,
        onCloseTrade = onCloseTrade,
        onOpenNpcTrade = onOpenNpcTrade
    })
    connect(Creature, {
        onAppear = onCreatureAppear,
        onDisappear = onCreatureDisappear,
        onPositionChange = onCreaturePositionChange
    })
    connect(LocalPlayer, {
        onPositionChange = onCreaturePositionChange
    })
    HarnessCtl.verbs = verbs
    HarnessCtl.reply = reply
    if HarnessPerceive then
        local ok, err = pcall(HarnessPerceive.init, HarnessCtl)
        if not ok then g_logger.warning('[harness_ctl] perceive init failed: ' .. tostring(err)) end
    end
    connectHub()
    statEvent = scheduleEvent(statTick, 2000)
end

function HarnessCtl.terminate()
    terminated = true
    if not hubUrl or hubUrl == '' then return end
    if HarnessPerceive then pcall(HarnessPerceive.terminate) end
    disconnect(g_game, {
        onGameStart = onGameStart,
        onGameEnd = onGameEnd,
        onLoginError = onLoginError,
        onLoginAdvice = onLoginAdvice,
        onLoginWait = onLoginWait,
        onConnectionError = onConnectionError,
        onTalk = onTalk,
        onOpenChannel = onOpenChannel,
        onTextMessage = onTextMessage,
        onOwnTrade = onOwnTrade,
        onCounterTrade = onCounterTrade,
        onCloseTrade = onCloseTrade,
        onOpenNpcTrade = onOpenNpcTrade
    })
    disconnect(Creature, {
        onAppear = onCreatureAppear,
        onDisappear = onCreatureDisappear,
        onPositionChange = onCreaturePositionChange
    })
    disconnect(LocalPlayer, {
        onPositionChange = onCreaturePositionChange
    })
    if hidden then pcall(function() disconnect(g_app, { onRun = harnessHide }) end) end
    if statEvent then removeEvent(statEvent) end
    if gotoEvent then removeEvent(gotoEvent) end
    if reconnectEvent then removeEvent(reconnectEvent) end
    for _, ev in pairs(pollers) do removeEvent(ev) end
    if ws then pcall(ws.close) end
end
