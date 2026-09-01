-- Offline harness for game_hudclean. Stubs the OTClient widget/module surface and
-- exercises the mask + chrome paths. Run: lua hudclean_test.lua <path-to-hudclean.lua>

local target = arg[1] or 'hudclean.lua'

local fails, passes = 0, 0
local function check(name, cond, extra)
    if cond then
        passes = passes + 1
        print('  ok   ' .. name)
    else
        fails = fails + 1
        print('  FAIL ' .. name .. (extra and ('  -> ' .. tostring(extra)) or ''))
    end
end

-- ------------------------------------------------------------------ widget stub
local Widget = {}
Widget.__index = Widget
local function newWidget(id)
    return setmetatable({
        id = id, opacity = 1, enabled = true, imageSource = '',
        destroyed = false, children = {}, setVisibleCalls = 0,
    }, Widget)
end
function Widget:getId() return self.id end
function Widget:isDestroyed() return self.destroyed end
function Widget:setOpacity(v) self.opacity = v end
function Widget:getOpacity() return self.opacity end
function Widget:setEnabled(v) self.enabled = v end
function Widget:isEnabled() return self.enabled end
function Widget:setVisible(v) self.setVisibleCalls = self.setVisibleCalls + 1 end
function Widget:setImageSource(v) self.imageSource = v end
function Widget:getImageSource() return self.imageSource end
function Widget:getChildren() return self.children end
function Widget:getChildById(id)
    for _, c in ipairs(self.children) do if c.id == id then return c end end
end
function Widget:recursiveGetChildById(id)
    local hit = self:getChildById(id)
    if hit then return hit end
    for _, c in ipairs(self.children) do
        local deep = c:recursiveGetChildById(id)
        if deep then return deep end
    end
end
function Widget:addChild(c) table.insert(self.children, c); return c end

-- ------------------------------------------------------------------ game stub
local mappings = {}
local function mapKey(b, i) return b .. ':' .. i end
local function assign(b, i, setting) mappings[mapKey(b, i)] = { actionsetting = setting or {} } end
local function unassign(b, i) mappings[mapKey(b, i)] = nil end

local ApiJson = {}
function ApiJson.getMapping(b, i) return mappings[mapKey(b, i)] end

local function newBar(n, slots)
    local bar = newWidget('actionBar')
    bar.imageSource = (n <= 3) and '/images/ui/actionbar/actionbar_background-light' or ''
    bar.locked = false
    bar.tabBar = newWidget('tabBar')
    for i = 1, slots do bar.tabBar:addChild(newWidget(n .. '.' .. i)) end
    return bar
end

local events = {}
local calls = {}
local G

local function buildWorld()
    mappings = {}
    events = {}
    calls = {}
    local root = newWidget('gameRootPanel')
    root:addChild(newWidget('lockLeftPanel'))
    root:addChild(newWidget('lockRightPanel'))
    root:addChild(newWidget('lockPanel'))
    local splitter = newWidget('bottomSplitter')
    splitter.opacity = 1

    local bars = {}
    for n = 1, 9 do bars[n] = newBar(n, 5) end

    local gameActionbar
    gameActionbar = {
        actionBars = bars,
        ApiJson = ApiJson,
        updateVisibleWidgets = function() calls.uvw = (calls.uvw or 0) + 1 end,
        changeLockStatus = function() calls.cls = (calls.cls or 0) + 1 end,
        setupActionBar = function(n) calls.sab = (calls.sab or 0) + 1 end,
        clearButton = function(btn) calls.clr = (calls.clr or 0) + 1 end,
        -- mirrors ActionButtonLogic: destroy the widget and put a fresh one (opacity 1)
        -- back at the same id
        updateButton = function(widget)
            calls.ub = (calls.ub or 0) + 1
            local id = widget:getId()
            local n = tonumber(id:match('^(%d+)%.'))
            local bar = gameActionbar.actionBars[n]
            for idx, c in ipairs(bar.tabBar.children) do
                if c.id == id then
                    c.destroyed = true
                    bar.tabBar.children[idx] = newWidget(id)
                    break
                end
            end
        end,
    }

    local gameInterface = {
        currentViewMode = 0,
        getRootPanel = function() return root end,
        getBottomSplitter = function() return splitter end,
        setupViewMode = function(m) calls.svm = (calls.svm or 0) + 1 end,
    }

    G = {
        modules = { game_actionbar = gameActionbar, game_interface = gameInterface },
        root = root, splitter = splitter, bars = bars,
        actionbar = gameActionbar, interface = gameInterface,
    }
    return G
end

-- ------------------------------------------------------------------ env + load
local function loadMod()
    local env = {
        modules = G.modules,
        ipairs = ipairs, pairs = pairs, pcall = pcall, tonumber = tonumber, type = type,
        tostring = tostring, string = string, table = table, math = math, print = print,
        pwarning = function(m) print('    [warn] ' .. m) end,
        pinfo = function(m) print('    [info] ' .. m) end,
        addEvent = function(fn) table.insert(events, fn) end,
        connect = function() calls.connect = (calls.connect or 0) + 1 end,
        disconnect = function() calls.disconnect = (calls.disconnect or 0) + 1 end,
        g_game = { isOnline = function() return true end },
    }
    env._G = env
    local chunk = assert(loadfile(target))
    setfenv(chunk, env)
    chunk()
    return env
end

local function drain()
    while #events > 0 do
        local queued = events
        events = {}
        for _, fn in ipairs(queued) do fn() end
    end
end

local function slot(n, i)
    return G.bars[n].tabBar:getChildById(n .. '.' .. i)
end

-- ------------------------------------------------------------------ tests
print('== A. slot masking ==')
buildWorld()
local mod = loadMod()
assign(1, 2)
assign(1, 4)
G.bars[1].locked = true
mod.init()
drain()
check('assigned slot 1.2 visible', slot(1, 2).opacity == 1)
check('assigned slot 1.4 visible', slot(1, 4).opacity == 1)
check('unassigned 1.1 hidden', slot(1, 1).opacity == 0)
check('unassigned 1.3 hidden', slot(1, 3).opacity == 0)
check('unassigned 1.5 hidden', slot(1, 5).opacity == 0)
check('masked slot disabled', slot(1, 1).enabled == false)
check('assigned slot enabled', slot(1, 2).enabled == true)
local usedSetVisible = false
for i = 1, 5 do if slot(1, i).setVisibleCalls > 0 then usedSetVisible = true end end
check('never calls setVisible (no repack)', not usedSetVisible)
check('unlocked bar 2 fully visible', slot(2, 1).opacity == 1 and slot(2, 5).opacity == 1)

print('== A2. unlock restores ==')
G.bars[1].locked = false
G.modules.game_actionbar.updateVisibleWidgets()
check('slot 1.1 back', slot(1, 1).opacity == 1 and slot(1, 1).enabled == true)
check('slot 1.5 back', slot(1, 5).opacity == 1)

print('== A3. updateButton recreation is re-masked ==')
G.bars[1].locked = true
G.modules.game_actionbar.updateVisibleWidgets()
check('1.3 hidden pre', slot(1, 3).opacity == 0)
G.modules.game_actionbar.updateButton(slot(1, 3))
check('fresh widget re-masked', slot(1, 3).opacity == 0, 'opacity=' .. slot(1, 3).opacity)
check('stock updateButton ran', calls.ub == 1)

print('== A4. clearButton re-masks ==')
G.modules.game_actionbar.updateButton(slot(1, 2))
check('assigned 1.2 still visible after recreate', slot(1, 2).opacity == 1)
unassign(1, 2)
G.modules.game_actionbar.clearButton(slot(1, 2))
check('cleared 1.2 now hidden', slot(1, 2).opacity == 0)

print('== A5. vertical bar (7-9) behaves identically ==')
assign(7, 1)
G.bars[7].locked = true
G.modules.game_actionbar.updateVisibleWidgets()
check('7.1 visible', slot(7, 1).opacity == 1)
check('7.2 hidden', slot(7, 2).opacity == 0)

print('== A6. fail-open when mapping lookup errors ==')
local savedGet = ApiJson.getMapping
ApiJson.getMapping = function() error('boom') end
G.modules.game_actionbar.updateVisibleWidgets()
check('errors leave slots visible', slot(1, 1).opacity == 1)
ApiJson.getMapping = savedGet

print('== B. chrome ==')
buildWorld()
mod = loadMod()
mod.init()
drain()
G.modules.game_interface.setupViewMode(2)
drain()
check('bar1 image stripped', G.bars[1].imageSource == '')
check('bar3 image stripped', G.bars[3].imageSource == '')
check('bar4 untouched (no strip to begin with)', G.bars[4].imageSource == '')
check('lockLeftPanel hidden', G.root:getChildById('lockLeftPanel').opacity == 0)
check('lockPanel hidden', G.root:getChildById('lockPanel').opacity == 0)
check('splitter hidden', G.splitter.opacity == 0)

print('== B2. idempotent ==')
G.modules.game_interface.setupViewMode(2)
drain()
G.modules.game_interface.setupViewMode(2)
drain()
check('still stripped after repeats', G.bars[1].imageSource == '')
check('splitter still 0', G.splitter.opacity == 0)

print('== B3. restore on leaving mode 2 ==')
G.modules.game_interface.setupViewMode(0)
drain()
check('bar1 image restored',
    G.bars[1].imageSource == '/images/ui/actionbar/actionbar_background-light',
    G.bars[1].imageSource)
check('splitter opacity restored', G.splitter.opacity == 1)
check('lockPanel restored', G.root:getChildById('lockPanel').opacity == 1)

print('== B4. top-up: bar appearing later still gets stripped ==')
G.modules.game_interface.setupViewMode(2)
drain()
G.bars[2] = newBar(2, 5)
G.modules.game_interface.setupViewMode(2)
drain()
check('late bar2 stripped', G.bars[2].imageSource == '')
G.modules.game_interface.setupViewMode(0)
drain()
check('late bar2 restored', G.bars[2].imageSource == '/images/ui/actionbar/actionbar_background-light')

print('== C. teardown ==')
buildWorld()
mod = loadMod()
local stockUvw = G.actionbar.updateVisibleWidgets
local stockSvm = G.interface.setupViewMode
mod.init()
drain()
check('patched', G.actionbar.updateVisibleWidgets ~= stockUvw)
G.modules.game_interface.setupViewMode(2)
drain()
mod.terminate()
check('chrome restored on terminate',
    G.bars[1].imageSource == '/images/ui/actionbar/actionbar_background-light')
check('updateVisibleWidgets restored', G.actionbar.updateVisibleWidgets == stockUvw)
check('setupViewMode restored', G.interface.setupViewMode == stockSvm)
check('disconnected', calls.disconnect == 1)

print('== C2. guarded restore skips a field someone else replaced ==')
buildWorld()
mod = loadMod()
mod.init()
drain()
local foreign = function() end
G.actionbar.updateVisibleWidgets = foreign
mod.terminate()
check('foreign function preserved', G.actionbar.updateVisibleWidgets == foreign)

print(string.format('\n%d passed, %d failed', passes, fails))
os.exit(fails == 0 and 0 or 1)
