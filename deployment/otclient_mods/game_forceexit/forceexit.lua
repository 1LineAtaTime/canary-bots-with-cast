-- game_forceexit: leave the game world without closing the client window.
--
-- Two escape hatches, because they exercise very different server paths:
--
--   forceKey (default Ctrl+Shift+Q) -> g_game.forceLogout()
--       Sends a logout packet, then tears the connection down locally no
--       matter what the server answers. Unlike Ctrl+Q / Ctrl+L (safe logout)
--       it cannot be refused by "You may not logout during a fight" — the
--       client stops waiting and drops straight back to the character list.
--       The server still sees an orderly logout request.
--
--   dropKey (default Ctrl+Shift+D) -> ProtocolGame:disconnect()
--       Closes the TCP socket with NO logout packet at all. The server does
--       not learn the player wanted to leave; it just finds the connection
--       dead. This is the one that exercises forceLogoutOnConnectionLoss /
--       the isDisconnected() check in sendPing.
--       Caveat: this is still a clean close (FIN), so the server notices
--       immediately. To simulate a silently vanished connection — no FIN,
--       server waits for the ping timeout — block the port in the firewall
--       or suspend the client process instead; no client-side Lua can do it.
--
-- Runtime config from the in-client terminal (Ctrl+T):
--   modules.game_forceexit.status()
--   modules.game_forceexit.force()                       -- fire force-logout now
--   modules.game_forceexit.drop()                        -- fire socket drop now
--   modules.game_forceexit.set('dropKey', 'Ctrl+Alt+D')  -- rebind (rebinds live)
--   modules.game_forceexit.set('confirm', true)          -- ask before dropping

-- Edit these values for persistent (across-restart) defaults.
local config = {
    forceKey = 'Ctrl+Shift+Q',  -- force logout, ignores in-fight block
    dropKey  = 'Ctrl+Shift+D',  -- kill the socket, no logout packet
    confirm  = false,           -- true = yes/no dialog before either action
}

local boundForceKey = nil
local boundDropKey  = nil
local confirmWindow = nil

-- ---------- actions ----------

local function destroyConfirm()
    if confirmWindow and not confirmWindow:isDestroyed() then confirmWindow:destroy() end
    confirmWindow = nil
end

local function ask(text, yesCallback)
    if not config.confirm then
        yesCallback()
        return
    end
    destroyConfirm()
    local yesFunc = function()
        destroyConfirm()
        yesCallback()
    end
    confirmWindow = displayGeneralBox(tr('Force Exit'), text, {
        { text = tr('Yes'), callback = yesFunc },
        { text = tr('No'),  callback = destroyConfirm },
        anchor = AnchorHorizontalCenter,
    }, yesFunc, destroyConfirm)
end

-- g_game.forceLogout(): logout packet + local teardown, cannot be refused.
function force()
    if not g_game.isOnline() then
        print('[forceexit] not online')
        return
    end
    ask(tr('Force logout now? The client stays open.'), function()
        print('[forceexit] forceLogout()')
        g_game.forceLogout()
    end)
end

-- Raw socket close, no logout packet: the server sees the connection die.
function drop()
    if not g_game.isOnline() then
        print('[forceexit] not online')
        return
    end
    ask(tr('Kill the connection with no logout packet?'), function()
        local protocol = g_game.getProtocolGame and g_game.getProtocolGame()
        if not protocol then
            print('[forceexit] no protocol object — falling back to forceLogout()')
            g_game.forceLogout()
            return
        end
        -- disconnect() is a Protocol member; if this build does not export it
        -- to Lua the pcall keeps the hotkey from erroring out mid-test.
        local ok, err = pcall(function() protocol:disconnect() end)
        if ok then
            print('[forceexit] socket dropped (no logout packet sent)')
        else
            print('[forceexit] disconnect() unavailable (' .. tostring(err) .. ') — using forceLogout()')
            g_game.forceLogout()
        end
    end)
end

-- ---------- key binding ----------

local function unbindKeys()
    if boundForceKey then g_keyboard.unbindKeyDown(boundForceKey) end
    if boundDropKey  then g_keyboard.unbindKeyDown(boundDropKey)  end
    boundForceKey = nil
    boundDropKey  = nil
end

local function bindKeys()
    unbindKeys()
    if config.forceKey and config.forceKey ~= '' then
        g_keyboard.bindKeyDown(config.forceKey, force)
        boundForceKey = config.forceKey
    end
    if config.dropKey and config.dropKey ~= '' then
        g_keyboard.bindKeyDown(config.dropKey, drop)
        boundDropKey = config.dropKey
    end
end

-- ---------- terminal helpers ----------

function status()
    print('[forceexit] forceKey = ' .. tostring(config.forceKey) .. '  (force logout, ignores in-fight block)')
    print('[forceexit] dropKey  = ' .. tostring(config.dropKey)  .. '  (kill socket, no logout packet)')
    print('[forceexit] confirm  = ' .. tostring(config.confirm))
    print('[forceexit] online   = ' .. tostring(g_game.isOnline()))
end

-- modules.game_forceexit.set('dropKey', 'Ctrl+Alt+D')
function set(key, value)
    if config[key] == nil then
        print('[forceexit] valid keys: forceKey, dropKey, confirm')
        return
    end
    local old = config[key]
    config[key] = value
    print(string.format('[forceexit] %s: %s -> %s', key, tostring(old), tostring(value)))
    if key == 'forceKey' or key == 'dropKey' then bindKeys() end
end

-- ---------- Lifecycle ----------

function init()
    bindKeys()
end

function terminate()
    unbindKeys()
    destroyConfirm()
end
