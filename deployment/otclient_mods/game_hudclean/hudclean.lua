--[[
game_hudclean -- two HUD cleanups for OTClient Redemption.

A. A locked action bar hides its unassigned slots IN PLACE. The assigned slots keep
   their exact position -- nothing packs to the left.
B. In extended view (game_interface view mode 2) the bottom action-bar background
   strip, the three padlock stubs and the bottom splitter render nothing, always,
   regardless of whether a bar exists / is visible / is locked. Assigned action
   buttons stay visible.

Why opacity and not setVisible, for (A):
  UIHorizontalLayout::internalUpdate (uihorizontallayout.cpp:51) and its vertical
  twin (uiverticallayout.cpp:52) skip children failing isExplicitlyVisible(), so
  setVisible(false) would repack the survivors. UIWidget::drawChildren
  (uiwidget.cpp:141) ALSO skips children whose getOpacity() <= Fw::MIN_ALPHA
  (0.003f, const.h:56) -- and opacity never enters layout. So setOpacity(0) hides
  a slot while it holds its cell. Stock never calls setOpacity in game_actionbar
  or game_interface, so the channel is uncontended.

Why wrappers on module table fields:
  modules == package.loaded (corelib/globals.lua:4) and each sandboxed module's env
  IS that table entry (module.cpp:44-48), with __index -> _G and no __newindex
  (luainterface.cpp:554-562). Module-global functions are fields of that env, so
  reassigning modules.game_actionbar.X intercepts the module's own bare-global call
  sites too. This also sidesteps the fact that resizeLockButtons /
  updateVisibleWidgets / moveActionButtons / changeLockStatus are each defined
  TWICE (logics/ActionBarLayout.lua and logics/ActionBarOptions.lua); the otmod
  scripts order makes the Options copies live, and patching the field is immune to
  which copy won.

See implementation_plans/otclient_hudclean_mod.md for the full rationale, the
verified engine mechanics and the documented limitations.
]]

local ORIGINAL = {} -- key "module.fn" -> stock function
local WRAPPER = {} -- key "module.fn" -> the closure we installed
local chromeStash = nil -- nil = chrome not applied
local connected = false

-- Only bars 1-3 load otui/actionbar, whose root Panel owns the background strip
-- (otui/actionbar.otui:191). Bars 4-9 load otui/sideactionbar, whose root has no
-- image and no border -- nothing to strip there.
local STRIP_BARS = 3
local BAR_COUNT = 9
local LOCK_STUB_IDS = { 'lockLeftPanel', 'lockRightPanel', 'lockPanel' }

-- ---------------------------------------------------------------------------
-- slot masking (feature A)
-- ---------------------------------------------------------------------------

local function slotIds(widget)
    if not widget or widget:isDestroyed() then
        return nil
    end
    local id = widget:getId()
    if not id then
        return nil
    end
    local barId, buttonId = id:match('^(%d+)%.(%d+)$')
    if not barId then
        return nil
    end
    return tonumber(barId), tonumber(buttonId)
end

-- An assigned slot is one with a mapping entry carrying an actionsetting. This
-- mirrors updateButton's own hasNewData test (ActionButtonLogic.lua:926).
-- Fails OPEN: anything we cannot reason about stays visible.
local function isAssigned(api, barId, buttonId)
    if not api or not api.getMapping then
        return true
    end
    local ok, entry = pcall(api.getMapping, barId, buttonId)
    if not ok then
        return true
    end
    return entry ~= nil and entry.actionsetting ~= nil
end

local function applySlot(widget, locked, api, barId, buttonId)
    local show = true
    if locked then
        show = isAssigned(api, barId, buttonId)
    end
    widget:setOpacity(show and 1 or 0)
    -- setEnabled(false) blocks the right-click -> updateButton inflation path at
    -- source (propagateOnMouseEvent checks isExplicitlyEnabled, uiwidget.cpp:2168).
    -- ActionButton declares no $disabled style and stock never disables tabBar
    -- slots, so this is inert beyond the input block.
    widget:setEnabled(show)
end

local function liveBar(n)
    local actionbar = modules.game_actionbar
    if not actionbar then
        return nil
    end
    -- Re-read every call: actionBars is reassigned to {} at game_actionbar.lua:375.
    local bars = actionbar.actionBars
    local bar = bars and bars[n]
    if not bar or bar:isDestroyed() then
        return nil
    end
    if not bar.tabBar or bar.tabBar:isDestroyed() then
        return nil
    end
    return bar, actionbar.ApiJson
end

local function maskBar(n)
    local bar, api = liveBar(n)
    if not bar then
        return
    end
    local locked = bar.locked and true or false
    for _, child in ipairs(bar.tabBar:getChildren()) do
        local barId, buttonId = slotIds(child)
        if barId then
            applySlot(child, locked, api, barId, buttonId)
        end
    end
end

local function maskAll()
    for n = 1, BAR_COUNT do
        maskBar(n)
    end
end

-- Re-mask a single slot after its widget was destroyed and recreated. The caller
-- must have captured the ids BEFORE running the stock function, because
-- updateButton (ActionButtonLogic.lua:898-908) destroys a placeholder and builds a
-- fresh ActionButton at default opacity 1.
local function maskSlot(barId, buttonId)
    local bar, api = liveBar(barId)
    if not bar then
        return
    end
    local widget = bar.tabBar:getChildById(barId .. '.' .. buttonId)
    if not widget or widget:isDestroyed() then
        return
    end
    applySlot(widget, bar.locked and true or false, api, barId, buttonId)
end

local function unmaskAll()
    for n = 1, BAR_COUNT do
        local bar = liveBar(n)
        if bar then
            for _, child in ipairs(bar.tabBar:getChildren()) do
                if child and not child:isDestroyed() then
                    child:setOpacity(1)
                    child:setEnabled(true)
                end
            end
        end
    end
end

-- ---------------------------------------------------------------------------
-- chrome blanking (features B and C)
-- ---------------------------------------------------------------------------

local function chromeTargets()
    local imageTargets, opacityTargets = {}, {}

    local actionbar = modules.game_actionbar
    local bars = actionbar and actionbar.actionBars
    if bars then
        for n = 1, STRIP_BARS do
            local bar = bars[n]
            if bar and not bar:isDestroyed() then
                table.insert(imageTargets, bar)
            end
        end
    end

    local interface = modules.game_interface
    if interface then
        local root = interface.getRootPanel and interface.getRootPanel()
        if root and not root:isDestroyed() then
            for _, id in ipairs(LOCK_STUB_IDS) do
                local widget = root:recursiveGetChildById(id)
                if widget and not widget:isDestroyed() then
                    table.insert(opacityTargets, widget)
                end
            end
        end
        -- Opacity, not image-strip: Splitter < UISplitter carries
        -- background: #ffffff44 (data/styles/10-splitters.otui:1-4) which survives
        -- clearing the image, and the instance overrides the base style's own
        -- opacity: 0 with opacity: 100 (gameinterface.otui:287). Keeping the widget
        -- preserves its rect, which anchors the whole bottom stack, and its drag.
        local splitter = interface.getBottomSplitter and interface.getBottomSplitter()
        if splitter and not splitter:isDestroyed() then
            table.insert(opacityTargets, splitter)
        end
    end

    return imageTargets, opacityTargets
end

-- Idempotent, and tops up targets that appeared since the last call. setupViewMode
-- runs three times during show()'s 0->1->2 sequence and our wrapper fires even when
-- the stock function early-returns (gameinterface.lua:1747).
local function applyChrome(on)
    if on then
        chromeStash = chromeStash or { images = {}, opacities = {} }
        local imageTargets, opacityTargets = chromeTargets()
        for _, widget in ipairs(imageTargets) do
            if chromeStash.images[widget] == nil then
                chromeStash.images[widget] = widget:getImageSource() or ''
            end
            -- Image only. image-border is the 9-slice texture margin, not a drawn
            -- border, so there is nothing else to clear -- and note there is no
            -- getBorderWidth() binding to stash anyway (only getBorderTopWidth /
            -- getBorderLeftWidth exist, luafunctions.cpp:776-779).
            widget:setImageSource('')
        end
        for _, widget in ipairs(opacityTargets) do
            if chromeStash.opacities[widget] == nil then
                chromeStash.opacities[widget] = widget:getOpacity()
            end
            widget:setOpacity(0)
        end
    else
        if not chromeStash then
            return
        end
        for widget, value in pairs(chromeStash.images) do
            if widget and not widget:isDestroyed() then
                widget:setImageSource(value)
            end
        end
        for widget, value in pairs(chromeStash.opacities) do
            if widget and not widget:isDestroyed() then
                widget:setOpacity(value)
            end
        end
        chromeStash = nil
    end
end

-- ---------------------------------------------------------------------------
-- patch install / restore
-- ---------------------------------------------------------------------------

local function install(moduleName, fnName, factory)
    local target = modules[moduleName]
    if not target then
        pwarning('[hudclean] module ' .. moduleName .. ' not loaded, skipping ' .. fnName)
        return
    end
    local original = target[fnName]
    if type(original) ~= 'function' then
        pwarning('[hudclean] ' .. moduleName .. '.' .. fnName .. ' is not a function, skipping')
        return
    end
    local key = moduleName .. '.' .. fnName
    local wrapper = factory(original)
    ORIGINAL[key] = original
    WRAPPER[key] = wrapper
    target[fnName] = wrapper
end

local function uninstall()
    for key, wrapper in pairs(WRAPPER) do
        local moduleName, fnName = key:match('^([^.]+)%.(.+)$')
        local target = modules[moduleName]
        -- Guarded: only restore if the live field is still ours. A game_actionbar or
        -- game_interface reload replaces the sandbox table, and blindly writing our
        -- stale closure back would clobber the fresh function.
        if target and target[fnName] == wrapper then
            target[fnName] = ORIGINAL[key]
        end
    end
    ORIGINAL, WRAPPER = {}, {}
end

local function installPatches()
    -- W1: pagination and the lazy placeholder inflation it performs
    -- (ActionBarOptions.lua:186-208). Also fires from tabBar's @onGeometryChange.
    install('game_actionbar', 'updateVisibleWidgets', function(original)
        return function(...)
            local result = original(...)
            maskAll()
            return result
        end
    end)

    -- W2: the padlock buttons themselves (gameinterface.otui:105/145/181).
    install('game_actionbar', 'changeLockStatus', function(original)
        return function(...)
            local result = original(...)
            maskAll()
            return result
        end
    end)

    -- W3: login and hotkey-set switching.
    install('game_actionbar', 'setupActionBar', function(original)
        return function(n, ...)
            local result = original(n, ...)
            if tonumber(n) then
                maskBar(tonumber(n))
            end
            return result
        end
    end)

    -- W4: the widget-recreation path. Reached without any lock gate by the
    -- placeholder right-click handler (game_actionbar.lua:165-176), by
    -- resetAction/resetActionBars (game_actionbar.lua:811-830) and by
    -- MultiActionLogic, which has no lock checks at all.
    install('game_actionbar', 'updateButton', function(original)
        return function(widget, ...)
            local barId, buttonId = slotIds(widget)
            local result = original(widget, ...)
            if barId then
                maskSlot(barId, buttonId)
            end
            return result
        end
    end)

    -- W5: "Clear Action" un-assigns a slot without any re-mask trigger
    -- (ActionButtonLogic.lua:402; the context menu is never lock-gated).
    install('game_actionbar', 'clearButton', function(original)
        return function(button, ...)
            local barId, buttonId = slotIds(button)
            local result = original(button, ...)
            if barId then
                maskSlot(barId, buttonId)
            end
            return result
        end
    end)

    -- W6: chrome follows the view mode. Deferred so it lands after the addEvent
    -- chain at gameinterface.lua:2020, which calls into six other modules and is
    -- known to clobber state set earlier in the same call.
    install('game_interface', 'setupViewMode', function(original)
        return function(mode, ...)
            local result = original(mode, ...)
            local extended = (mode == 2)
            addEvent(function()
                applyChrome(extended)
            end)
            return result
        end
    end)
end

-- ---------------------------------------------------------------------------
-- lifecycle
-- ---------------------------------------------------------------------------

function onGameStart()
    -- Deferred so game_actionbar's own onGameStart has already rebuilt the bars.
    addEvent(maskAll)
end

function init()
    installPatches()
    connect(g_game, { onGameStart = onGameStart })
    connected = true
    if g_game.isOnline() then
        addEvent(maskAll)
    end
end

function terminate()
    if connected then
        disconnect(g_game, { onGameStart = onGameStart })
        connected = false
    end
    applyChrome(false)
    unmaskAll()
    uninstall()
end

-- Terminal helpers (Ctrl+T), e.g. modules.game_hudclean.refresh()
function refresh()
    maskAll()
end

function status()
    local interface = modules.game_interface
    pinfo(string.format('[hudclean] viewMode=%s chrome=%s patches=%d',
        tostring(interface and interface.currentViewMode), chromeStash and 'on' or 'off',
        table.size and table.size(WRAPPER) or -1))
end
