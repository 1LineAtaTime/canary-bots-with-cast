-- game_botcontrol: a playerbots/MultiBot-style control panel for canary-bot-players bots.
--
-- Talks to the server over ExtendedOpcode 120 (data/scripts/creaturescripts/bot/bot_control_opcode.lua):
--   client -> server, plain text:  "list"  |  "cmd <verb text>"   (verb text as in party chat:
--                                  "@all follow", "@Some_Bot invite", "summon ek,ed", ...)
--   server -> client, JSON:        {t="list", me, inParty, leader, bots=[{name,voc,level,hp,mp,state,order,target,party,dist}]}
--                                  {t="res", verb, ok, text, replies=[{bot, ok, msg, result}]}
-- Every permission check happens on the server (the bot command security ladder): the panel
-- only builds the command text. Works with OTClient 4.2 / Redemption (extended opcodes are
-- enabled by the server at login for every OTClient).

local OPCODE = 120
local POLL_MS = 2000

local window = nil
local toggleButton = nil
local pollEvent = nil
local selectedName = nil
local lastList = nil
local lastResult = ''

local function protocol()
    return g_game.isOnline() and g_game.getProtocolGame() or nil
end

local function send(text)
    local p = protocol()
    if not p then
        return false
    end
    p:sendExtendedOpcode(OPCODE, text)
    return true
end

local function requestList()
    send('list')
end

local function stopPoll()
    if pollEvent then
        removeEvent(pollEvent)
        pollEvent = nil
    end
end

local function poll()
    pollEvent = nil
    if not window or not window:isVisible() then
        return
    end
    requestList()
    pollEvent = scheduleEvent(poll, POLL_MS)
end

local function addressFor(name)
    return '@' .. name:gsub(' ', '_')
end

local function rowText(b)
    local where = b.party and (b.order ~= '' and b.order or 'follow') or ('near ' .. tostring(b.dist))
    local tgt = (b.target and b.target ~= '') and (' > ' .. b.target) or ''
    return string.format('[%s] %s %d  %d%%/%d%%  %s %s%s', b.voc or '??', b.name, b.level or 0, b.hp or 0,
        b.mp or 0, (b.state or ''):lower(), where, tgt)
end

local function updateList(data)
    lastList = data
    if not window then
        return
    end
    local header = window:getChildById('header')
    local nParty = 0
    for _, b in ipairs(data.bots or {}) do
        if b.party then
            nParty = nParty + 1
        end
    end
    if data.inParty then
        header:setText(string.format('%s %s - %d bot(s) in party', data.leader and 'Leader:' or 'Member:',
            data.me or '', nParty))
    else
        header:setText('No party - select a bot and Invite, or Summon')
    end

    local list = window:getChildById('botList')
    local keep = selectedName
    list:destroyChildren()
    local focus = nil
    for _, b in ipairs(data.bots or {}) do
        local row = g_ui.createWidget('BotRow', list)
        row:setId('bot_' .. tostring(b.guid))
        row.botName = b.name
        row:setText(rowText(b))
        row:setColor(b.party and '#c0ffc0' or '#c0c0c0')
        if b.name == keep then
            focus = row
        end
    end
    if focus then
        list:focusChild(focus, KeyboardFocusReason)
    end
end

local function showResult(data)
    local lines = {}
    for _, r in ipairs(data.replies or {}) do
        lines[#lines + 1] = string.format('%s: %s', r.bot ~= '' and r.bot or 'server', r.msg or '')
    end
    if #lines == 0 then
        lines[1] = data.text or ''
    end
    lastResult = table.concat(lines, '\n')
    if window then
        window:getChildById('result'):setText(lastResult)
    end
end

local function onOpcode(protocol, opcode, buffer)
    local ok, data = pcall(json.decode, buffer)
    if not ok or type(data) ~= 'table' then
        return
    end
    if data.t == 'list' then
        updateList(data)
    elseif data.t == 'res' then
        showResult(data)
        scheduleEvent(requestList, 400)
    end
end

-- ---------------------------------------------------------------------------
-- public (also used by the test harness: modules.game_botcontrol.*)
-- ---------------------------------------------------------------------------
function show()
    if not window then
        return
    end
    window:show()
    window:raise()
    window:focus()
    stopPoll()
    poll()
end

function hide()
    stopPoll()
    if window then
        window:hide()
    end
end

function toggle()
    if window and window:isVisible() then
        hide()
    else
        show()
    end
end

function selectBot(name)
    selectedName = name
    if lastList then
        updateList(lastList)
    end
end

-- Builds the verb text for a button and sends it. Returns the text sent (or nil + reason).
function click(action)
    local onlySelected = window and window:getChildById('selectedOnly'):isChecked()
    local addr
    if action == 'invite' or action == 'kick' then
        if not selectedName then
            showResult({ text = 'Select a bot first.' })
            return nil, 'no-selection'
        end
        addr = addressFor(selectedName)
    elseif onlySelected and selectedName then
        addr = addressFor(selectedName)
    elseif action == 'heal' then
        addr = '@ed'
    else
        addr = '@all'
    end
    local verb = action
    if action == 'heal' then
        verb = 'heal me'
    elseif action == 'summon' then
        local vocs = window and window:getChildById('summonVocs'):getText() or 'ek,ed'
        vocs = vocs:match('^%s*(.-)%s*$')
        local text = 'cmd summon ' .. (vocs ~= '' and vocs or 'ek,ed')
        send(text)
        return text
    end
    local text = 'cmd ' .. addr .. ' ' .. verb
    if not send(text) then
        return nil, 'offline'
    end
    return text
end

-- One-line state for the harness: visible, header, rows, selection, last result.
function dump()
    local rows = {}
    if window then
        for _, c in ipairs(window:getChildById('botList'):getChildren()) do
            rows[#rows + 1] = c:getText()
        end
    end
    return string.format('visible=%s header=%q rows=%d [%s] selected=%s result=%q',
        tostring(window and window:isVisible()), window and window:getChildById('header'):getText() or '',
        #rows, table.concat(rows, ' | '), tostring(selectedName), lastResult)
end

function getLastList()
    return lastList
end

local function onGameStart()
    selectedName = nil
    lastList = nil
end

local function onGameEnd()
    hide()
    lastList = nil
end

function init()
    connect(g_game, { onGameStart = onGameStart, onGameEnd = onGameEnd })
    ProtocolGame.registerExtendedOpcode(OPCODE, onOpcode)
    window = g_ui.displayUI('botcontrol')
    window:hide()
    connect(window:getChildById('botList'), {
        onChildFocusChange = function(_, child)
            if child and child.botName then
                selectedName = child.botName
            end
        end
    })
    if modules.client_topmenu and modules.client_topmenu.addLeftGameButton then
        toggleButton = modules.client_topmenu.addLeftGameButton('botControlButton', tr('Bot Control') .. ' (Ctrl+Shift+B)',
            '/images/topbuttons/bot', toggle)
    end
    g_keyboard.bindKeyDown('Ctrl+Shift+B', toggle)
end

function terminate()
    stopPoll()
    disconnect(g_game, { onGameStart = onGameStart, onGameEnd = onGameEnd })
    pcall(ProtocolGame.unregisterExtendedOpcode, OPCODE)
    g_keyboard.unbindKeyDown('Ctrl+Shift+B')
    if toggleButton then
        toggleButton:destroy()
        toggleButton = nil
    end
    if window then
        window:destroy()
        window = nil
    end
end
