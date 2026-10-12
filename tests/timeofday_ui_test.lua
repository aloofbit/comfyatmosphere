-- Run the actual time controls with a small WoW UI stand-in. No game required.
local file = assert(io.open(arg[1], "r"));
local source = file:read("*a");
file:close();
local function between(first, last)
    local start = assert(string.find(source, first, 1, true));
    local finish = assert(string.find(source, last, start, true));
    return string.sub(source, start, finish - 1);
end

math.mod = math.mod or math.fmod;
table.getn = table.getn or function(t) return #t; end;
getglobal = function(name) return _G[name]; end;
local methods = {};
for _, name in ipairs({ "SetWidth", "SetHeight", "SetPoint", "SetMinMaxValues",
    "SetValueStep", "SetJustifyH" }) do
    methods[name] = function() end;
end
function methods:SetText(value) self.text = value; end
function methods:SetChecked(value) self.checked = value; end
function methods:GetChecked() return self.checked; end
function methods:SetScript(event, handler) self.scripts[event] = handler; end
function methods:EnableMouse(value) self.mouse = value; end
function methods:SetAlpha(value) self.alpha = value; end
function methods:Enable() self.enabled = true; end
function methods:Disable() self.enabled = false; end
function methods:GetValue() return self.value; end
function methods:SetValue(value)
    self.value = value;
    if self.scripts.OnValueChanged then
        local previous = this;
        this = self;
        self.scripts.OnValueChanged();
        this = previous;
    end
end
function CreateFrame(kind, name, parent, template)
    local frame = setmetatable({scripts = {}, enabled = true}, {__index = methods});
    if name then
        _G[name] = frame;
        if template then
            _G[name .. "Text"] = CreateFrame();
            if kind == "Slider" then
                _G[name .. "Low"] = CreateFrame();
                _G[name .. "High"] = CreateFrame();
            end
        end
    end
    return frame;
end
function methods:CreateFontString() return CreateFrame(); end
PanelMake = function(name) return CreateFrame("Frame", name); end;
DEBUG_VIEWS = {"off"};
GetCVar = function() return "0"; end;
HasCVar = function() return true; end;
local shownHour, shownState;
TimeShown = function() return shownHour, shownState; end;
local sent = {};
TimeSend = function(verb, value) table.insert(sent, {verb, value}); end;

local compile = loadstring or load;
local build, refresh = assert(compile(
    between("local function TimeText(", "-- A held key") ..
    between("local debugFrame = nil;", "function ComfyAtmosphere_DebugToggle()") ..
    "return function() debugFrame = DebugBuild(); return debugFrame; end, DebugRefresh"
))();
local panel = build();
local function show(hour, state)
    shownHour, shownState = hour, state;
    refresh();
end
local function manual(enabled)
    assert(ComfyAtmosphereDebugTime.mouse == enabled, "slider mode");
    for _, name in ipairs({"Earlier", "Later", "DayNight"}) do
        assert(_G["ComfyAtmosphereDebug" .. name].enabled == enabled, "button mode");
    end
end
local function click(frame, checked, verb, value)
    this = frame;
    frame:SetChecked(checked);
    frame.scripts.OnClick();
    assert(sent[#sent][1] == verb and sent[#sent][2] == value, "saved mode command");
end

show(22.2625, 4); -- 22:15:45 must not round up to 22:16.
assert(panel.timeText.text == "Computer time: 22:15", "live minute display");
assert(ComfyAtmosphereDebugTimeLocal.checked and not ComfyAtmosphereDebugTimeOn.checked);
manual(false);
show(23.9958, 4);
assert(panel.timeText.text == "Computer time: 23:59", "display before midnight");
show(0, 4);
assert(panel.timeText.text == "Computer time: 00:00", "display at midnight");
show(13.25, 1);
assert(panel.timeText.text == "Time of day: 13:15");
assert(ComfyAtmosphereDebugTimeOn.checked and not ComfyAtmosphereDebugTimeLocal.checked);
manual(true);
for _, state in ipairs({0, 2, 3}) do
    show(13, state);
    assert(not ComfyAtmosphereDebugTimeOn.checked and not ComfyAtmosphereDebugTimeLocal.checked);
    manual(false);
end
show(nil, nil);
assert(panel.timeText.text == "Time of day: waiting for comfyatmos.dll");
manual(false);
assert(#sent == 0, "refresh must not issue time commands");
click(ComfyAtmosphereDebugTimeLocal, true, "local", "1");
click(ComfyAtmosphereDebugTimeLocal, false, "local", "0");
click(ComfyAtmosphereDebugTimeOn, true, "lock", "1");
click(ComfyAtmosphereDebugTimeOn, false, "lock", "0");
print("PASS: local/fixed/server/error controls, mode commands, minute display and refresh guard");
