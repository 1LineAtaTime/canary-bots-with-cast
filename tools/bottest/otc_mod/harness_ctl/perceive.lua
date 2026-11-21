-- harness_ctl perception: what a human player perceives, as events + snapshot verbs.
--
-- Sources (all Lua, no OTClient rebuild):
--  * PACKET TAP. ProtocolGame::parseMessage calls Lua `onOpcode(opcode, msg)` for EVERY server
--    opcode before the C++ parser, and restores the read position when Lua returns false
--    (src/client/protocolgameparse.cpp:65-71). We wrap gamelib's ProtocolGame.onOpcode, read the
--    effect / missile / text-effect / text-message opcodes ourselves and always return false, so
--    the C++ parse and rendering are untouched. This replaces the tile poller planned in R1:
--    Lua has NO getter for tile effects (Tile::m_effects, tile.h:186), floor missiles
--    (Map::getFloorMissiles, map.h:213) or animated/static texts (map.h:215-216) - none are bound
--    in luafunctions.cpp - and the tap sees every packet at parse time instead of sampling.
--      0x83 (131) GraphicalEffect : effect list incl. distance missiles + sound effects (>= 1203)
--      0x84 (132) TextEffect      : >= 1320 "remove magic effect", older: animated text
--      0x85 (133) MissleEffect    : old-protocol missile (13.x+ with GameAnthem: anthem, ignored)
--      0xB4 (180) TextMessage     : damage / heal / mana / exp numbers = the animated texts
--  * Creature / LocalPlayer / g_game / Container signals (exact timing) + a light poller for
--    direction and walking state (no signal exists for a creature turn).
--
-- Events: see_effect, see_effect_end, see_missile, see_sound, see_text (kind=anim|static),
--         see_creature (deltas), see_self (deltas), see_cooldown, tap_error.
-- Verbs : view (full snapshot), self (own UI state), perceive (config + counters).

HarnessPerceive = {}
local P = HarnessPerceive

local emit, reply = nil, nil
local cfg = { tap = true, creatures = true, self = true, pollMs = 100, selfPollMs = 500, recentMs = 3000 }
local stats = { tap = {}, tapErr = 0, lastErr = nil, ev = {}, t0 = 0 }
local recent = {}        -- ring of tapped perception events (for `view`)
local RECENT_MAX = 600
local origOnOpcode = nil
local tapInstalled = false
local pollEvent, selfPollEvent = nil, nil
local track = {}         -- cid -> { snap, dir, walking, lastWalkT }
local lastEffectAt = {}  -- "x,y,z#id" -> cms (repeat detection)
local phasesCache = {}

local OP_EFFECT, OP_TEXTEFFECT, OP_MISSILE, OP_TEXTMSG = 131, 132, 133, 180
local EFFECT_TICKS, MISSILE_TICKS = 75, 75   -- data/setup.otml effect-/missile-ticks-per-frame

local PLAYER_FIRST_ID = 0x10000000
local function isBotCid(cid) return cid >= PLAYER_FIRST_ID + 65001 and cid <= PLAYER_FIRST_ID + 66004 end
local function cms() return g_clock.realMillis() end

local function count(kind) stats.ev[kind] = (stats.ev[kind] or 0) + 1 end

local function remember(f)
    table.insert(recent, f)
    if #recent > RECENT_MAX then table.remove(recent, 1) end
end

local function emitP(kind, f, keep)
    count(kind)
    emit(kind, f)
    if keep then remember(f) end
end

-- ------------------------------------------------------------------ helpers
local function myPos()
    local lp = g_game.getLocalPlayer()
    return lp and lp:getPosition() or nil
end

local function rel(f, p, prefix)
    prefix = prefix or ''
    f[prefix .. 'x'], f[prefix .. 'y'], f[prefix .. 'z'] = p.x, p.y, p.z
    f[prefix .. 'at'] = p.x .. ',' .. p.y .. ',' .. p.z
    local me = myPos()
    if me and prefix == '' then
        f.dx, f.dy, f.dz = p.x - me.x, p.y - me.y, p.z - me.z
    end
end

local function creatureAt(p)
    local ok, c = pcall(function()
        local t = g_map.getTile({ x = p.x, y = p.y, z = p.z })
        return t and t:getTopCreature() or nil
    end)
    return ok and c or nil
end

local function annotate(f, p, prefix)
    local c = creatureAt(p)
    if c then
        f[prefix .. 'cid'] = c:getId()
        f[prefix .. 'name'] = c:getName()
    end
end

local function feat(name, num)
    local v = _G[name] or num
    return g_game.getFeature(v)
end

local function s8(v) if v >= 128 then return v - 256 end return v end

local function readPos(msg)
    local x = msg:getU16()
    local y = msg:getU16()
    local z = msg:getU8()
    return { x = x, y = y, z = z }
end

local function effectPhases(id)
    local v = phasesCache[id]
    if v == nil then
        v = false
        pcall(function()
            local tt = g_things.getThingType(id, ThingCategoryEffect or 2)
            if tt then v = tt:getAnimationPhases() end
        end)
        phasesCache[id] = v
    end
    return v or nil
end

-- ------------------------------------------------------------------ packet tap
local function onEffect(pos, id, source, delay)
    local f = { id = id }
    rel(f, pos)
    annotate(f, pos, '')
    if source and source ~= 0 then f.source = source end
    if delay and delay ~= 0 then f.delay = delay end
    local ph = effectPhases(id)
    if ph then
        f.phases = ph
        f.life_est_ms = ph * EFFECT_TICKS
    end
    local key = f.at .. '#' .. id
    local now = cms()
    local prev = lastEffectAt[key]
    if prev and now - prev < 2000 then f.rep_ms = now - prev end
    lastEffectAt[key] = now
    emitP('see_effect', f, true)
end

local function onMissile(from, to, id, source)
    local f = { id = id }
    rel(f, from)
    f.from = f.at
    f.to = to.x .. ',' .. to.y .. ',' .. to.z
    f.tx, f.ty, f.tz = to.x, to.y, to.z
    annotate(f, from, '')
    annotate(f, to, 'to_')
    local dx, dy = to.x - from.x, to.y - from.y
    local len = math.sqrt(dx * dx + dy * dy)
    f.dist = math.max(math.abs(dx), math.abs(dy))
    f.life_ms = math.floor(MISSILE_TICKS * 2 * math.sqrt(len))   -- Missile::setPath, missile.cpp:81
    if source and source ~= 0 then f.source = source end
    emitP('see_missile', f, true)
end

local function onSound(pos, sound, source, which)
    local f = { sound = sound, which = which }
    if source and source ~= 0 then f.source = source end
    rel(f, pos)
    emitP('see_sound', f, false)
end

-- mirrors ProtocolGame::parseMagicEffect byte for byte (protocolgameparse.cpp:1903 in the Jun-12 build,
-- :1911 in OTClient 4.2; re-checked for the 15.25 port: same loop, same MAGIC_EFFECTS_* ids 0-7)
local function tapEffect(msg)
    local pos = readPos(msg)
    if g_game.getProtocolVersion() < 1203 then
        local id = feat('GameMagicEffectU16', 16) and msg:getU16() or msg:getU8()
        if g_game.getClientVersion() <= 750 then id = id + 1 end
        return onEffect(pos, id, 0, 0)
    end
    local u16 = feat('GameEffectU16', 105)
    local hasSrc = feat('GameEffectSource', 132)
    local delay = 0
    local t = msg:getU8()
    local guard = 0
    while t ~= 0 do
        guard = guard + 1
        if guard > 256 then error('effect loop guard') end
        if t == 1 or t == 2 then
            local v = msg:getU8()   -- C++ reads one byte and ignores it (delta / delay)
            if t == 2 then delay = delay + v end
        elseif t == 4 or t == 5 then
            local id = u16 and msg:getU16() or msg:getU8()
            local ox = s8(msg:getU8())
            local oy = s8(msg:getU8())
            local source = hasSrc and msg:getU8() or 0
            local other = { x = pos.x + ox, y = pos.y + oy, z = pos.z }
            if t == 4 then onMissile(pos, other, id, source) else onMissile(other, pos, id, source) end
        elseif t == 3 then
            local id = u16 and msg:getU16() or msg:getU8()
            local source = hasSrc and msg:getU8() or 0
            onEffect(pos, id, source, delay)
        elseif t == 6 then
            local source = msg:getU8()
            local snd = msg:getU16()
            onSound(pos, snd, source, 'main')
        elseif t == 7 then
            msg:getU8()
            local source = msg:getU8()
            local snd = msg:getU16()
            onSound(pos, snd, source, 'secondary')
        end
        t = msg:getU8()
    end
end

local function animText(pos, value, color, modeName, part)
    local f = { kind = 'anim', text = tostring(value), value = value, color = color, mode = modeName }
    if part then f.part = part end
    rel(f, pos)
    annotate(f, pos, '')
    emitP('see_text', f, true)
end

local function tapTextEffect(msg)
    if g_game.getClientVersion() >= 1320 then
        local pos = readPos(msg)
        local id = feat('GameEffectU16', 105) and msg:getU16() or msg:getU8()
        local f = { id = id }
        rel(f, pos)
        emitP('see_effect_end', f, true)
    else
        local pos = readPos(msg)
        local color = msg:getU8()
        local text = msg:getString()
        animText(pos, text, color, 'AnimatedText', nil)
    end
end

local function tapMissile(msg)
    if feat('GameAnthem', 95) then return end
    local from = readPos(msg)
    local to = readPos(msg)
    local id = feat('GameDistanceEffectU16', 77) and msg:getU16() or msg:getU8()
    onMissile(from, to, id, 0)
end

-- server message codes for protocol >= 1094 (Proto::buildMessageModesMap, protocolcodes.cpp:33-87)
local DMG = { [23] = 'DamageDealed', [24] = 'DamageReceived', [27] = 'DamageOthers' }
local HEAL = { [25] = 'Heal', [28] = 'HealOthers', [43] = 'Mana' }
local EXP = { [26] = 'Exp', [29] = 'ExpOthers' }

-- mirrors ProtocolGame::parseTextMessage (protocolgameparse.cpp:2990) for the modes that draw
-- an animated text; every other mode reaches Lua as g_game.onTextMessage -> see_msg
local function tapTextMessage(msg)
    if g_game.getClientVersion() < 1094 then return end
    local code = msg:getU8()
    if DMG[code] then
        local pos = readPos(msg)
        local v1, c1 = msg:getU32(), msg:getU8()
        local v2, c2 = msg:getU32(), msg:getU8()
        if v1 ~= 0 then animText(pos, v1, c1, DMG[code], 'physical') end
        if v2 ~= 0 then animText(pos, v2, c2, DMG[code], 'magic') end
    elseif HEAL[code] then
        local pos = readPos(msg)
        local v, c = msg:getU32(), msg:getU8()
        animText(pos, v, c, HEAL[code], nil)
    elseif EXP[code] then
        local pos = readPos(msg)
        local v = g_game.getClientVersion() >= 1332 and msg:getU64() or msg:getU32()
        local c = msg:getU8()
        animText(pos, v, c, EXP[code], nil)
    end
end

local TAP = { [OP_EFFECT] = tapEffect, [OP_TEXTEFFECT] = tapTextEffect, [OP_MISSILE] = tapMissile,
    [OP_TEXTMSG] = tapTextMessage }

local function onOpcodeTap(self, opcode, msg)
    if origOnOpcode and origOnOpcode(self, opcode, msg) then return true end
    local h = cfg.tap and TAP[opcode]
    if h then
        stats.tap[opcode] = (stats.tap[opcode] or 0) + 1
        local ok, err = pcall(h, msg)
        if not ok then
            stats.tapErr = stats.tapErr + 1
            stats.lastErr = string.format('op=%d %s', opcode, tostring(err))
            if stats.tapErr <= 10 then emit('tap_error', { op = opcode, err = tostring(err) }) end
        end
    end
    return false   -- C++ restores the read position and parses/draws as usual
end

local function installTap()
    if tapInstalled or not ProtocolGame then return end
    origOnOpcode = ProtocolGame.onOpcode
    ProtocolGame.onOpcode = onOpcodeTap
    tapInstalled = true
end

local function removeTap()
    if not tapInstalled then return end
    if ProtocolGame.onOpcode == onOpcodeTap then ProtocolGame.onOpcode = origOnOpcode end
    tapInstalled = false
end

-- ------------------------------------------------------------------ static text (speech bubbles)
-- Called from ctl.lua's onTalk. The bubble itself is created by game_console in Lua for the
-- positioned modes (say/whisper/yell/spell/monster/npc/potion), exactly those with a map pos.
function P.onTalk(name, level, mode, text, channelId, pos)
    if not pos or not pos.x or pos.x == 0 or pos.x >= 65535 then return end
    local f = { kind = 'static', text = text, name = name, mode = mode, level = level }
    rel(f, pos)
    -- name of the speaker is the creature (the `bot=` filter matches on name)
    emitP('see_text', f, true)
end

-- ------------------------------------------------------------------ creatures
local CTYPE = { [0] = 'player', [1] = 'monster', [2] = 'npc', [3] = 'summon', [4] = 'summon', [5] = 'hidden' }

local function outfitStr(o)
    if type(o) ~= 'table' then return nil end
    return string.format('%d/%d-%d-%d-%d+%d m%d', o.type or 0, o.head or 0, o.body or 0, o.legs or 0,
        o.feet or 0, o.addons or 0, o.mount or 0)
end

local function creatureSnap(c)
    local s = {}
    pcall(function()
        s.name = c:getName()
        s.hp = c:getHealthPercent()
        s.skull = c:getSkull()
        s.shield = c:getShield()
        s.emblem = c:getEmblem()
        s.ctype_id = c:getType()
        s.master = c:getMasterId()
        s.speed = c:getSpeed()
        s.outfit = outfitStr(c:getOutfit())
        s.dir = c:getDirection()
        s.walking = c:isWalking() and 1 or 0
        s.dead = c:isDead() and 1 or 0
        s.invisible = c:isInvisible() and 1 or 0
    end)
    return s
end
P.creatureSnap = creatureSnap

local DIFF_KEYS = { 'name', 'hp', 'skull', 'shield', 'emblem', 'ctype_id', 'master', 'speed', 'outfit', 'dead', 'invisible' }

local function creatureFields(c, f)
    local cid = c:getId()
    f.cid = cid
    f.name = c:getName()
    local ty = CTYPE[c:getType()] or (c:isPlayer() and 'player') or (c:isMonster() and 'monster') or
        (c:isNpc() and 'npc') or 'creature'
    if c:isPlayer() and isBotCid(cid) then f.bot = true end
    f.ctype = ty
    local p = c:getPosition()
    if p then rel(f, p) end
    return f
end

-- seeds tracking and adds the full state to see_appear (called from ctl.lua)
function P.onAppear(c, info)
    if c:isLocalPlayer() then return end
    local s = creatureSnap(c)
    track[c:getId()] = { snap = s, dir = s.dir, walking = s.walking == 1, lastWalkT = cms() }
    for _, k in ipairs({ 'skull', 'shield', 'emblem', 'speed', 'outfit', 'dir', 'master', 'walking' }) do
        info[k] = s[k]
    end
    info.ctype_id = s.ctype_id
end

function P.onDisappear(c)
    track[c:getId()] = nil
end

local function creatureDiff(c, why)
    if not cfg.creatures then return end
    local tr = track[c:getId()]
    if not tr then return end   -- not yet on screen (creation-time signals precede onAppear)
    local s = creatureSnap(c)
    local f = {}
    local changed = {}
    for _, k in ipairs(DIFF_KEYS) do
        local o, v = tr.snap[k], s[k]
        if o ~= v then
            table.insert(changed, k)
            f[k] = v
            if type(o) == 'number' and type(v) == 'number' then f['d_' .. k] = v - o else f['was_' .. k] = o end
        end
    end
    tr.snap = s
    if #changed == 0 then return end
    creatureFields(c, f)
    f.why = why
    f.changed = table.concat(changed, ',')
    emitP('see_creature', f, false)
end

local creatureSignals = {}
for _, sig in ipairs({ 'onHealthPercentChange', 'onSkullChange', 'onShieldChange', 'onEmblemChange',
    'onOutfitChange', 'onSpeedChange', 'onBaseSpeedChange', 'onTypeChange', 'onChangeName', 'onDeath' }) do
    local why = sig:gsub('^on', ''):gsub('Change$', '')
    creatureSignals[sig] = function(c)
        if c:isLocalPlayer() then
            if P.selfDirty then P.selfDirty(why) end
            return
        end
        creatureDiff(c, why)
    end
end

local function pollCreatures()
    pollEvent = nil
    if cfg.creatures and g_game.isOnline() then
        local now = cms()
        for cid, tr in pairs(track) do
            local c = g_map.getCreatureById(cid)
            if c then
                local dir = c:getDirection()
                local walking = c:isWalking()
                if walking then tr.lastWalkT = now end
                local f = nil
                if dir ~= tr.dir then
                    f = { why = 'dir', dir = dir, was_dir = tr.dir, walking = walking and 1 or 0 }
                    tr.dir = dir
                end
                if walking and not tr.walking then
                    tr.walking = true
                    f = f or {}
                    f.why = f.why and (f.why .. ',walk') or 'walk'
                    f.walking = 1
                elseif not walking and tr.walking and now - tr.lastWalkT >= 400 then
                    tr.walking = false
                    f = f or {}
                    f.why = f.why and (f.why .. ',walk') or 'walk'
                    f.walking = 0
                end
                if f then
                    creatureFields(c, f)
                    f.changed = f.why
                    emitP('see_creature', f, false)
                end
            end
        end
    end
    pollEvent = scheduleEvent(pollCreatures, cfg.pollMs)
end

-- ------------------------------------------------------------------ self (own UI state)
local SKILLS = { [0] = 'fist', 'club', 'sword', 'axe', 'dist', 'shield', 'fish', 'critchance', 'critdmg',
    'lifeleechchance', 'lifeleechamt', 'manaleechchance', 'manaleechamt', 'fatal', 'dodge', 'momentum', 'transcend' }
local SLOTS = { 'head', 'neck', 'back', 'body', 'right', 'left', 'legs', 'feet', 'finger', 'ammo' }
-- canary icon bits (ICON_*), as the client panel draws them
local STATES = { { 1, 'poison' }, { 2, 'burn' }, { 4, 'energy' }, { 8, 'drunk' }, { 16, 'manashield' },
    { 32, 'paralyze' }, { 64, 'haste' }, { 128, 'swords' }, { 256, 'drowning' }, { 512, 'freezing' },
    { 1024, 'dazzled' }, { 2048, 'cursed' }, { 4096, 'partybuff' }, { 8192, 'pzblock' }, { 16384, 'pz' },
    { 32768, 'bleeding' }, { 65536, 'lesserhex' }, { 131072, 'intensehex' }, { 262144, 'greaterhex' },
    { 524288, 'rooted' }, { 1048576, 'feared' }, { 2097152, 'taint1' }, { 4194304, 'taint2' },
    { 8388608, 'taint3' }, { 16777216, 'taint4' }, { 33554432, 'taint5' }, { 67108864, 'newmanashield' },
    { 134217728, 'agony' }, { 268435456, 'powerless' }, { 536870912, 'mentored' } }

local function decodeStates(v)
    local out = {}
    v = v or 0
    for _, b in ipairs(STATES) do
        if math.floor(v / b[1]) % 2 == 1 then table.insert(out, b[2]) end
    end
    return table.concat(out, ',')
end
P.decodeStates = decodeStates

local function itemInfo(it)
    if not it then return nil end
    local d = { id = it:getId() }
    pcall(function()
        d.count = it:getCountOrSubType()
        local ch = it:getCharges(); if ch and ch > 0 then d.charges = ch end
        local du = it:getDurationTime(); if du and du > 0 then d.dur = du end
        local ti = it:getTier(); if ti and ti > 0 then d.tier = ti end
        if it:isStackable() then d.stack = true end
    end)
    return d
end

-- delta key of an item: id, count, charges, tier. The remaining duration (d.dur) ticks every
-- second on a decaying ring/amulet, so it stays out of the see_self diff; `self` reports it.
local function itemStr(it)
    local d = itemInfo(it)
    if not d then return '-' end
    local s = tostring(d.id)
    if d.count and d.count > 1 then s = s .. 'x' .. d.count end
    if d.charges then s = s .. ':c' .. d.charges end
    if d.tier then s = s .. ':t' .. d.tier end
    return s
end

local cdSpell, cdGroup, cdMulti = {}, {}, 0

local function activeList(t, now)
    local ids = {}
    for id, exp in pairs(t) do if exp > now then table.insert(ids, id) end end
    table.sort(ids)
    return table.concat(ids, ',')
end

local function containersDetail()
    local out = {}
    local ok = pcall(function()
        for cid, ct in pairs(g_game.getContainers()) do
            local items = {}
            for _, it in ipairs(ct:getItems()) do table.insert(items, itemInfo(it)) end
            table.insert(out, { id = ct:getId(), name = ct:getName(), capacity = ct:getCapacity(),
                size = ct:getSize(), item = itemInfo(ct:getContainerItem()), items = items })
        end
    end)
    table.sort(out, function(a, b) return (a.id or 0) < (b.id or 0) end)
    return out
end

local function selfSnap()
    local lp = g_game.getLocalPlayer()
    if not lp or not g_game.isOnline() then return nil end
    local s = {}
    local now = cms()
    pcall(function()
        s.hp, s.hpmax = lp:getHealth(), lp:getMaxHealth()
        s.mp, s.mpmax = lp:getMana(), lp:getMaxMana()
        -- free capacity only: >= 1281 the stats packet resets total capacity to 0 and the skills
        -- packet sets it again (protocolgameparse.cpp:2581-2583 / 2731-2739), so it flaps
        s.cap = lp:getFreeCapacity()
        s.soul, s.stamina = lp:getSoul(), lp:getStamina()
        s.level, s.levelpct, s.exp = lp:getLevel(), lp:getLevelPercent(), lp:getExperience()
        s.mlvl, s.mlvlpct = lp:getMagicLevel(), lp:getMagicLevelPercent()
        s.speed, s.basespeed = lp:getSpeed(), lp:getBaseSpeed()
        s.manashield = lp:getManaShield()
        s.states = lp:getStates()
        s.cond = decodeStates(s.states)
        s.vocation = lp:getVocation()
        s.premium = lp:isPremium() and 1 or 0
        s.skull = lp:getSkull()
        s.outfit = outfitStr(lp:getOutfit())
    end)
    for i = 0, 16 do
        pcall(function()
            s['sk_' .. SKILLS[i]] = lp:getSkillLevel(i)
            if i <= 6 then s['skp_' .. SKILLS[i]] = lp:getSkillLevelPercent(i) end
        end)
    end
    for slot, n in ipairs(SLOTS) do
        local it = nil
        pcall(function() it = lp:getInventoryItem(slot) end)
        s['inv_' .. n] = itemStr(it)
    end
    for _, ct in ipairs(containersDetail()) do
        local parts = {}
        for _, it in ipairs(ct.items) do
            local x = tostring(it.id)
            if it.count and it.count > 1 then x = x .. 'x' .. it.count end
            table.insert(parts, x)
        end
        s['cont_' .. ct.id] = (ct.name or '?') .. '|' .. table.concat(parts, ',')
    end
    pcall(function()
        local a = g_game.getAttackingCreature()
        s.attack = a and (a:getName() .. '#' .. a:getId()) or '-'
        local f = g_game.getFollowingCreature()
        s.follow = f and (f:getName() .. '#' .. f:getId()) or '-'
        s.fight, s.chase, s.pvp = g_game.getFightMode(), g_game.getChaseMode(), g_game.getPVPMode()
        s.safe = g_game.isSafeFight() and 1 or 0
    end)
    s.cd = activeList(cdSpell, now)
    s.cdg = activeList(cdGroup, now)
    s.cdm = cdMulti > now and 1 or 0
    return s
end
P.selfSnap = selfSnap

local selfPrev, selfWhy, selfPending, selfT0 = nil, {}, nil, nil

local function flushSelf()
    selfPending = nil
    local s = selfSnap()
    if not s then return end
    local whyl = {}
    for k in pairs(selfWhy) do table.insert(whyl, k) end
    table.sort(whyl)
    selfWhy = {}
    if not selfPrev then selfPrev = s; return end
    local f, changed = {}, {}
    for k, v in pairs(s) do
        local o = selfPrev[k]
        if o ~= v then
            table.insert(changed, k)
            f[k] = v
            if type(o) == 'number' and type(v) == 'number' then f['d_' .. k] = v - o else f['was_' .. k] = o or '-' end
        end
    end
    for k, o in pairs(selfPrev) do
        if s[k] == nil then
            table.insert(changed, k)
            f[k] = '-'
            f['was_' .. k] = o
        end
    end
    selfPrev = s
    if #changed == 0 then return end
    table.sort(changed)
    f.changed = table.concat(changed, ',')
    f.why = #whyl > 0 and table.concat(whyl, ',') or 'poll'
    if selfT0 then f.sig_cms = selfT0 end
    emitP('see_self', f, false)
end

function P.selfDirty(why)
    if not cfg.self then return end
    selfWhy[why] = true
    if not selfPending then
        selfT0 = cms()
        selfPending = true
        addEvent(flushSelf)   -- coalesce every signal of one packet batch into one see_self
    end
end

local function pollSelf()
    selfPollEvent = nil
    if cfg.self and g_game.isOnline() and not selfPending then
        selfT0 = nil
        flushSelf()
    end
    selfPollEvent = scheduleEvent(pollSelf, cfg.selfPollMs)
end

local function onSpellCooldown(spellId, delay)
    cdSpell[spellId] = cms() + delay
    emitP('see_cooldown', { kind = 'spell', id = spellId, ms = delay }, false)
    P.selfDirty('cooldown')
end

local function onSpellGroupCooldown(groupId, delay)
    cdGroup[groupId] = cms() + delay
    emitP('see_cooldown', { kind = 'group', id = groupId, ms = delay }, false)
    P.selfDirty('cooldown')
end

local function onMultiUseCooldown(delay)
    cdMulti = cms() + delay
    emitP('see_cooldown', { kind = 'multiuse', ms = delay }, false)
    P.selfDirty('cooldown')
end

local lpSignals, gameSignals, containerSignals = {}, {}, {}
for _, sig in ipairs({ 'onHealthChange', 'onManaChange', 'onLevelChange', 'onExperienceChange',
    'onFreeCapacityChange', 'onTotalCapacityChange', 'onSoulChange', 'onStaminaChange', 'onStatesChange',
    'onSkillChange', 'onBaseSkillChange', 'onMagicLevelChange', 'onBaseMagicLevelChange', 'onInventoryChange',
    'onManaShieldChange', 'onPremiumChange', 'onVocationChange', 'onBlessingsChange' }) do
    local why = sig:gsub('^on', ''):gsub('Change$', '')
    lpSignals[sig] = function() P.selfDirty(why) end
end
for _, sig in ipairs({ 'onAttackingCreatureChange', 'onFollowingCreatureChange', 'onFightModeChange',
    'onChaseModeChange', 'onPVPModeChange', 'onSafeFightChange' }) do
    local why = sig:gsub('^on', ''):gsub('Change$', '')
    gameSignals[sig] = function() P.selfDirty(why) end
end
gameSignals.onSpellCooldown = onSpellCooldown
gameSignals.onSpellGroupCooldown = onSpellGroupCooldown
gameSignals.onMultiUseCooldown = onMultiUseCooldown
gameSignals.onGameStart = function()
    selfPrev, track, cdSpell, cdGroup, cdMulti = nil, {}, {}, {}, 0
    scheduleEvent(function() P.selfDirty('login') end, 300)
end
gameSignals.onGameEnd = function() selfPrev, track = nil, {} end
for _, sig in ipairs({ 'onOpen', 'onClose', 'onAddItem', 'onRemoveItem', 'onUpdateItem', 'onSizeChange' }) do
    containerSignals[sig] = function() P.selfDirty('container' .. sig:gsub('^on', '')) end
end

-- ------------------------------------------------------------------ verbs
local function posStr(p) return p and string.format('%d,%d,%d', p.x, p.y, p.z) or '?' end

local function statsTable()
    local tap = {}
    for op, n in pairs(stats.tap) do tap['op' .. op] = n end
    return { tap = tap, tap_errors = stats.tapErr, last_error = stats.lastErr, events = stats.ev,
        tap_installed = tapInstalled, cfg = cfg, tracked = (function() local n = 0 for _ in pairs(track) do n = n + 1 end return n end)() }
end

local function installVerbs(verbs)
    -- view: every creature in the aware area (all floors the client knows) with its full state,
    -- the own UI state, and the effects / missiles / texts tapped in the last recent_ms.
    verbs.view = function(id, a)
        local lp = g_game.getLocalPlayer()
        if not lp or not g_game.isOnline() then return reply(id, false, 'STUCK verb=view reason=offline') end
        local p = lp:getPosition()
        local list = {}
        for _, c in ipairs(g_map.getSpectators(p, a.multifloor ~= false)) do
            if not c:isLocalPlayer() then
                local s = creatureSnap(c)
                creatureFields(c, s)
                table.insert(list, s)
            end
        end
        table.sort(list, function(x, y) return (math.abs(x.dx or 99) + math.abs(x.dy or 99)) < (math.abs(y.dx or 99) + math.abs(y.dy or 99)) end)
        local since = cms() - (a.recent_ms or cfg.recentMs)
        local rec = {}
        for _, e in ipairs(recent) do if (e.cms or 0) >= since then table.insert(rec, e) end end
        local nb, nm, np, nn = 0, 0, 0, 0
        for _, s in ipairs(list) do
            if s.bot then nb = nb + 1 elseif s.ctype == 'monster' then nm = nm + 1
            elseif s.ctype == 'player' then np = np + 1 elseif s.ctype == 'npc' then nn = nn + 1 end
        end
        reply(id, true, string.format('OK view creatures=%d bots=%d players=%d monsters=%d npcs=%d recent=%d pos=%s',
            #list, nb, np, nm, nn, #rec, posStr(p)),
            { creatures = list, recent = rec, self = selfSnap(), containers = containersDetail(), pos = { x = p.x, y = p.y, z = p.z },
                perceive = statsTable() })
    end

    verbs.self = function(id, a)
        local s = selfSnap()
        if not s then return reply(id, false, 'STUCK verb=self reason=offline') end
        local p = myPos()
        local inv = {}
        local lp = g_game.getLocalPlayer()
        for slot, n in ipairs(SLOTS) do
            local it = nil
            pcall(function() it = lp:getInventoryItem(slot) end)
            inv[n] = itemInfo(it)
        end
        local now = cms()
        local cds = {}
        for sid, exp in pairs(cdSpell) do if exp > now then table.insert(cds, { kind = 'spell', id = sid, left_ms = exp - now }) end end
        for gid, exp in pairs(cdGroup) do if exp > now then table.insert(cds, { kind = 'group', id = gid, left_ms = exp - now }) end end
        if cdMulti > now then table.insert(cds, { kind = 'multiuse', left_ms = cdMulti - now }) end
        local ninv = 0
        for _ in pairs(inv) do ninv = ninv + 1 end
        local ct = containersDetail()
        local bags = {}
        for k, v in pairs(s) do if k:sub(1, 5) == 'cont_' then table.insert(bags, k:sub(6) .. ':' .. v) end end
        table.sort(bags)
        local text = string.format('OK self hp=%s/%s mp=%s/%s cap=%s soul=%s stamina=%s level=%s exp=%s speed=%s cond=%s inv=%d containers=%d cd=%s cdg=%s attack=%s follow=%s fight=%s chase=%s pvp=%s safe=%s pos=%s bags=[%s]',
            tostring(s.hp), tostring(s.hpmax), tostring(s.mp), tostring(s.mpmax), tostring(s.cap), tostring(s.soul),
            tostring(s.stamina), tostring(s.level), tostring(s.exp), tostring(s.speed), s.cond ~= '' and s.cond or '-',
            ninv, #ct, s.cd ~= '' and s.cd or '-', s.cdg ~= '' and s.cdg or '-', s.attack or '-', s.follow or '-',
            tostring(s.fight), tostring(s.chase), tostring(s.pvp), tostring(s.safe), posStr(p), table.concat(bags, ' '))
        reply(id, true, text, { self = s, inventory = inv, containers = ct, cooldowns = cds,
            pos = p and { x = p.x, y = p.y, z = p.z } or nil })
    end

    -- open: open the container in an inventory slot (default back) like a double-click, so its
    -- contents become known to the client (closed containers are invisible to a human too)
    verbs.open = function(id, a)
        local lp = g_game.getLocalPlayer()
        if not lp or not g_game.isOnline() then return reply(id, false, 'STUCK verb=open reason=offline') end
        local slot = a.slot or 'back'
        local idx = tonumber(slot)
        if not idx then for i, n in ipairs(SLOTS) do if n == slot then idx = i end end end
        local it = idx and lp:getInventoryItem(idx) or nil
        if not it then return reply(id, false, 'STUCK verb=open reason=empty-slot slot=' .. tostring(slot)) end
        g_game.open(it, nil)
        reply(id, true, string.format('OK open slot=%s item=%d', tostring(slot), it:getId()))
    end

    -- use: hotkey-style use of an item id from the inventory / open containers (server searches
    -- the player's items), with=self (default) or a creature name on screen, or plain use (with=none)
    verbs.use = function(id, a)
        local lp = g_game.getLocalPlayer()
        if not lp or not g_game.isOnline() then return reply(id, false, 'STUCK verb=use reason=offline') end
        local itemId = tonumber(a.id)
        if not itemId then return reply(id, false, 'STUCK verb=use reason=bad-id') end
        local with = a.with or 'self'
        if with == 'none' then
            g_game.useInventoryItem(itemId)
            return reply(id, true, 'OK use id=' .. itemId)
        end
        local target = lp
        if with ~= 'self' then
            target = nil
            for _, c in ipairs(g_map.getSpectators(lp:getPosition(), false)) do
                if string.lower(c:getName()) == string.lower(with) then target = c break end
            end
            if not target then return reply(id, false, 'STUCK verb=use reason=target-not-visible') end
        end
        g_game.useInventoryItemWith(itemId, target)
        reply(id, true, string.format('OK use id=%d with=%s cid=%d', itemId, target:getName(), target:getId()))
    end

    -- perceive: configure (poll_ms, self_poll_ms, recent_ms, tap/creatures/self on|off, reset) and
    -- return the counters (tap per opcode, tap errors, events per kind)
    verbs.perceive = function(id, a)
        if a.poll_ms then cfg.pollMs = math.max(20, a.poll_ms) end
        if a.self_poll_ms then cfg.selfPollMs = math.max(100, a.self_poll_ms) end
        if a.recent_ms then cfg.recentMs = a.recent_ms end
        if a.tap ~= nil then cfg.tap = a.tap and true or false end
        if a.creatures ~= nil then cfg.creatures = a.creatures and true or false end
        if a.self ~= nil then cfg.self = a.self and true or false end
        if a.reset then stats.tap, stats.tapErr, stats.lastErr, stats.ev = {}, 0, nil, {} end
        local st = statsTable()
        local parts = {}
        for k, v in pairs(st.events) do table.insert(parts, k .. '=' .. v) end
        table.sort(parts)
        local taps = {}
        for k, v in pairs(st.tap) do table.insert(taps, k .. '=' .. v) end
        table.sort(taps)
        reply(id, true, string.format('OK perceive tap=%s poll_ms=%d tracked=%d tap_errors=%d taps[%s] events[%s]',
            tostring(cfg.tap and tapInstalled), cfg.pollMs, st.tracked, st.tap_errors, table.concat(taps, ' '),
            table.concat(parts, ' ')), { perceive = st })
    end
end

-- ------------------------------------------------------------------ lifecycle
function P.init(ctl)
    emit, reply = ctl.emit, ctl.reply
    stats.t0 = cms()
    installTap()
    installVerbs(ctl.verbs)
    connect(Creature, creatureSignals)
    connect(LocalPlayer, lpSignals)
    connect(g_game, gameSignals)
    connect(Container, containerSignals)
    pollEvent = scheduleEvent(pollCreatures, cfg.pollMs)
    selfPollEvent = scheduleEvent(pollSelf, cfg.selfPollMs)
end

function P.terminate()
    removeTap()
    pcall(disconnect, Creature, creatureSignals)
    pcall(disconnect, LocalPlayer, lpSignals)
    pcall(disconnect, g_game, gameSignals)
    pcall(disconnect, Container, containerSignals)
    if pollEvent then removeEvent(pollEvent) end
    if selfPollEvent then removeEvent(selfPollEvent) end
end
