-- Adds the comfyatmosphere controls to the options window, on a page of their own: Video -> Atmosphere.
--
-- This client's options panel is data-driven, as the ComfyGrass addon describes: GameOptions is a global
-- table, and OptionsFrame.lua builds each page from it (both in patch-9.mpq). An entry with `options` is
-- a page in the list on the left, and one without is a heading (Video, Sound, Interface). So the page is
-- one entry, put after Shaders, and each control is one table entry with a cvar. The list shows 18 rows
-- and the client uses 15, so the page fits without scrolling the list. Until 2026-09-29 the controls
-- were added to the Shaders page.
--
-- The panel calls SetCVar while a slider moves, and comfyfog.dll reads the CVars a few times a second, so
-- a change shows in the world at once. Cancel puts the old values back the same way.
--
-- comfyfog.dll registers the CVars, with the values in comfyfog.ini as their defaults. So the page's
-- Defaults button puts the ini values back.
--
-- Without the DLL the CVars do not exist, and GetCVar raises an error for a CVar that does not exist.
-- The panel calls GetCVar for every entry on a page, so an entry for a missing CVar would break the
-- whole page. The controls are added only when the DLL has registered its CVars, and each one
-- only if its own CVar exists: an older DLL has fewer of them than this addon.
--
-- Without the DLL the addon says nothing. A launcher that turns the mod off removes its dlls.txt line
-- and leaves this folder, so a message here would repeat at every login.
--
-- The client saves the values to Config.wtf itself, and only the ones moved away from their default
-- (measured: a thickness of 85 was written, fog at its default 1 was not). So a setting nobody moved
-- still follows comfyfog.ini, and the addon keeps no copy of its own.

COMFYATMOSPHERE_CATEGORY       = "Atmosphere";
COMFYATMOSPHERE_MASTER         = "Atmosphere Effects";
COMFYATMOSPHERE_FOG            = "Atmospheric Fog";
COMFYATMOSPHERE_FOG_THICKNESS  = "Fog Thickness";
COMFYATMOSPHERE_FOG_HAZE       = "Ground Haze";
COMFYATMOSPHERE_FOG_HEIGHT     = "Fog Height";
COMFYATMOSPHERE_FOG_FADE       = "Fog Edge Fade";
COMFYATMOSPHERE_FOG_DARKNESS   = "Fog Darkness";
COMFYATMOSPHERE_FOG_GREYNESS   = "Fog Greyness";
COMFYATMOSPHERE_FOG_SUN_GLOW   = "Fog Sun Glow";
COMFYATMOSPHERE_FOG_REACH      = "Fog Distance";
COMFYATMOSPHERE_FOG_NEAR       = "Fog Near You";
COMFYATMOSPHERE_FOG_FAR        = "Distance Fog Amount";
COMFYATMOSPHERE_FOG_SUN_BRIGHT = "Fog Sun Brightness";
COMFYATMOSPHERE_VOLUME         = "Volumetric Light";
COMFYATMOSPHERE_VOLUME_STRENGTH = "Volumetric Light Strength";
COMFYATMOSPHERE_VOLUME_QUALITY = "Volumetric Light Quality";
COMFYATMOSPHERE_VOLUME_DENSITY = "Light Density";
COMFYATMOSPHERE_VOLUME_DISTANCE = "Light Distance";
COMFYATMOSPHERE_VOLUME_DIRECTION = "Light Toward the Sun";
COMFYATMOSPHERE_LAMP_GLOW      = "Lamp Glow";
COMFYATMOSPHERE_LAMP_DISTANCE  = "Lamp Distance";
COMFYATMOSPHERE_SUN_SHADOWS    = "Sun Shadows";
COMFYATMOSPHERE_SUN_SHADOW_STRENGTH = "Sun Shadow Strength";
COMFYATMOSPHERE_SHADOWS_WORLD  = "World / Object Shadows";
COMFYATMOSPHERE_SHADOWS_UNITS  = "Player / Creature Shadows";
COMFYATMOSPHERE_SHADOW_LOCK    = "Lock Shadow Angle";
COMFYATMOSPHERE_SHADOW_TILT    = "Shadow Angle";
COMFYATMOSPHERE_SUNLIGHT       = "Sunlight";
COMFYATMOSPHERE_SHADE_TINT     = "Shade Colour";
COMFYATMOSPHERE_SUN_TINT       = "Sunlight Warmth";
COMFYATMOSPHERE_SHADOW_RESOLUTION = "Shadow Resolution";
COMFYATMOSPHERE_SHADOW_SOFTNESS = "Shadow Softness";
COMFYATMOSPHERE_SHADOW_EVERY   = "Shadow Redraw";
COMFYATMOSPHERE_RAYS           = "Sun Rays";
COMFYATMOSPHERE_RAYS_STRENGTH  = "Sun Rays Strength";
COMFYATMOSPHERE_RAYS_SOFTEN    = "Sun Rays Softness";
COMFYATMOSPHERE_RAYS_SMOOTH    = "Sun Rays Smoothing";
COMFYATMOSPHERE_DEBUG_VIEW     = "Debug View";
COMFYATMOSPHERE_NIGHT_STRENGTH = "Night Strength";
COMFYATMOSPHERE_NIGHT_DARKNESS = "Night Darkness";
COMFYATMOSPHERE_MOONLIGHT      = "Moonlight Colour";
COMFYATMOSPHERE_CLOUDS         = "Clouds";

local ENTRIES = {
	-- name is a KEY, not a string: the panel does _G[option.name] to get the label.
	{
		-- Every effect at once. The panel can make a control depend on one CVar only, and the others
		-- depend on their own boxes, so they stay movable while this is off; they do nothing then.
		name = "COMFYATMOSPHERE_MASTER",
		desc = "All the effects on this page at once. Off, the game looks as it does without the mod.",
		type = "checkbutton",
		cvar = "comfyAtmosphere",
	},
	{
		name = "COMFYATMOSPHERE_VOLUME",
		desc = "Fog glows where sunlight reaches it and stays dark where leaves and walls shade it.",
		type = "checkbutton",
		cvar = "comfyVolume",
		warning = "Costs the most frame rate of these settings: it draws the world a second time, from the sun.",
	},
	{
		name = "COMFYATMOSPHERE_VOLUME_STRENGTH",
		desc = "How bright the lit fog is.",
		type = "slider",
		cvar = "comfyVolumeStrength",
		dependency = { "comfyVolume", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Three positions: 1 low, 2 medium, 3 high. Without numberLabels the panel labels the ends Low
		-- and High (OptionsFrame.lua in patch-9.mpq).
		name = "COMFYATMOSPHERE_VOLUME_QUALITY",
		desc = "Lower runs faster.",
		type = "slider",
		cvar = "comfyVolumeQuality",
		dependency = { "comfyVolume", "1" },
		minval = 1,
		maxval = 3,
		step = 1,
	},
	{
		-- Thousandths: comfyfog.dll divides by 1000 for [volume] density (15 is 0.015).
		name = "COMFYATMOSPHERE_VOLUME_DENSITY",
		desc = "How thick the air is. Higher gives brighter shafts through the trees.",
		type = "slider",
		cvar = "comfyVolumeDensity",
		dependency = { "comfyVolume", "1" },
		minval = 0,
		maxval = 40,
		step = 1,
		numberLabels = 1,
	},
	{
		-- Yards, [volume] maxDistance. Shorter samples the air more finely: sharper shafts close by.
		name = "COMFYATMOSPHERE_VOLUME_DISTANCE",
		desc = "How far away, in yards, the light still glows. Shorter makes the shafts near you sharper.",
		type = "slider",
		cvar = "comfyVolumeDistance",
		dependency = { "comfyVolume", "1" },
		minval = 50,
		maxval = 250,
		step = 10,
		numberLabels = 1,
	},
	{
		-- Thousandths: comfyfog.dll divides by 1000 for [volume] anisotropy (25 is 0.025).
		name = "COMFYATMOSPHERE_VOLUME_DIRECTION",
		desc = "0 glows the same from every side. Higher glows more when you look toward the sun.",
		type = "slider",
		cvar = "comfyVolumeDirection",
		dependency = { "comfyVolume", "1" },
		minval = 0,
		maxval = 500,
		step = 5,
		numberLabels = 1,
	},
	{
		-- [lamps] strength: the glow around lamps, candles and torches and their light on the walls near them.
		name = "COMFYATMOSPHERE_LAMP_GLOW",
		desc = "How strongly lamps, candles and torches glow and light the walls and ground near them.",
		type = "slider",
		cvar = "comfyLampGlow",
		dependency = { "comfyVolume", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percent: [lamps] fogReach. 100 fades a lamp's glow as the game fades its lamp sprites in the fog.
		name = "COMFYATMOSPHERE_LAMP_DISTANCE",
		desc = "How far away lamps still glow. At 100 they fade into the fog as the game's own lamp glows do.",
		type = "slider",
		cvar = "comfyLampDistance",
		dependency = { "comfyVolume", "1" },
		minval = 50,
		maxval = 400,
		step = 10,
		numberLabels = 1,
	},
	{
		-- The sun shadows use the volumetric light's shadow map, so they need it on. While both are on,
		-- the round shadow the game draws under each character is turned off (SyncUnitShadow below).
		name = "COMFYATMOSPHERE_SUN_SHADOWS",
		desc = "Trees, buildings and characters shade the ground and each other. Replaces the round shadow under characters. Needs Volumetric Light on.",
		type = "checkbutton",
		cvar = "comfySunShadows",
		dependency = { "comfyVolume", "1" },
	},
	{
		name = "COMFYATMOSPHERE_SHADOWS_WORLD",
		desc = "Terrain, buildings, trees and doodads cast shadows. Off, the ground keeps the game's own shadows.",
		type = "checkbutton",
		cvar = "comfySunShadowsWorld",
		dependency = { "comfySunShadows", "1" },
	},
	{
		name = "COMFYATMOSPHERE_SHADOWS_UNITS",
		desc = "Players and creatures cast shadows. Off, they get the game's round shadow instead.",
		type = "checkbutton",
		cvar = "comfySunShadowsUnits",
		dependency = { "comfySunShadows", "1" },
	},
	{
		name = "COMFYATMOSPHERE_SHADOW_LOCK",
		desc = "Shadows keep one angle all day, as the game's own shadows do, instead of following the sun.",
		type = "checkbutton",
		cvar = "comfyShadowLock",
		dependency = { "comfySunShadows", "1" },
	},
	{
		-- Degrees from straight down, [sunshadows] lockTilt.
		name = "COMFYATMOSPHERE_SHADOW_TILT",
		desc = "With Lock Shadow Angle: how far from straight down the light comes, in degrees. Lower gives shorter shadows.",
		type = "slider",
		cvar = "comfyShadowTilt",
		dependency = { "comfyShadowLock", "1" },
		minval = 0,
		maxval = 60,
		step = 5,
		numberLabels = 1,
	},
	{
		name = "COMFYATMOSPHERE_SUN_SHADOW_STRENGTH",
		desc = "How dark the shadows are.",
		type = "slider",
		cvar = "comfySunShadowStrength",
		dependency = { "comfyVolume", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyfog.dll divides it by 100 for [sunshadows] sunlight.
		name = "COMFYATMOSPHERE_SUNLIGHT",
		desc = "Makes what the sun reaches brighter, by up to this many percent. 0 leaves it as the game draws it.",
		type = "slider",
		cvar = "comfySunlight",
		dependency = { "comfyVolume", "1" },
		minval = 0,
		maxval = 50,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percent: comfyfog.dll divides by 100 for [sunshadows] shadeTint.
		name = "COMFYATMOSPHERE_SHADE_TINT",
		desc = "Shade takes the sky's cool blue instead of only going darker.",
		type = "slider",
		cvar = "comfyShadeTint",
		dependency = { "comfySunShadows", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percent: comfyfog.dll divides by 100 for [sunshadows] sunTint.
		name = "COMFYATMOSPHERE_SUN_TINT",
		desc = "Sunlit ground takes a warm colour.",
		type = "slider",
		cvar = "comfySunTint",
		dependency = { "comfySunShadows", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Three positions: 1024, 2048 and 4096 texels a side. The sun shadows use the volumetric light's
		-- shadow map, so they need it on.
		name = "COMFYATMOSPHERE_SHADOW_RESOLUTION",
		desc = "Higher gives sharper shadows and costs more frame rate. The shadows need Volumetric Light on.",
		type = "slider",
		cvar = "comfyShadowResolution",
		dependency = { "comfyVolume", "1" },
		minval = 1,
		maxval = 3,
		step = 1,
	},
	{
		name = "COMFYATMOSPHERE_SHADOW_SOFTNESS",
		desc = "How soft the edges of shadows are. 0 gives sharp edges.",
		type = "slider",
		cvar = "comfyShadowSoftness",
		dependency = { "comfyVolume", "1" },
		minval = 0,
		maxval = 8,
		step = 1,
		numberLabels = 1,
	},
	{
		name = "COMFYATMOSPHERE_SHADOW_EVERY",
		desc = "The shadow map is drawn again every this many frames. 1 is every frame. Higher costs less frame rate, and the shadows of moving things lag further behind.",
		type = "slider",
		cvar = "comfyShadowEvery",
		dependency = { "comfyVolume", "1" },
		minval = 1,
		maxval = 8,
		step = 1,
		numberLabels = 1,
	},
	{
		name = "COMFYATMOSPHERE_RAYS",
		desc = "Rays of light from the sun, through gaps in the trees and clouds.",
		type = "checkbutton",
		cvar = "comfyRays",
	},
	{
		name = "COMFYATMOSPHERE_RAYS_STRENGTH",
		desc = "How bright the rays are.",
		type = "slider",
		cvar = "comfyRaysStrength",
		dependency = { "comfyRays", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		name = "COMFYATMOSPHERE_RAYS_SOFTEN",
		desc = "Higher blurs the light the rays come from, so they hold still behind leaves. Lower gives sharper rays.",
		type = "slider",
		cvar = "comfyRaysSoften",
		dependency = { "comfyRays", "1" },
		minval = 0,
		maxval = 16,
		step = 1,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyfog.dll divides it by 100 for [rays] smooth.
		name = "COMFYATMOSPHERE_RAYS_SMOOTH",
		desc = "Higher keeps more of the last frame, so the rays change more gently as you move. Too high leaves a trail when you turn quickly.",
		type = "slider",
		cvar = "comfyRaysSmooth",
		dependency = { "comfyRays", "1" },
		minval = 0,
		maxval = 90,
		step = 5,
		numberLabels = 1,
	},
	{
		-- No dependency: it scales both the rays and the light.
		name = "COMFYATMOSPHERE_NIGHT_STRENGTH",
		desc = "Sun rays and volumetric light at night. 100 is as strong as by day. 0 is off.",
		type = "slider",
		cvar = "comfyNightStrength",
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percent: comfyfog.dll divides by 100 for [night] darkness. It is drawn with the lamps, from the
		-- volumetric light's depth.
		name = "COMFYATMOSPHERE_NIGHT_DARKNESS",
		desc = "How much darker the world is at night. Lamps still light the ground near them. Not inside buildings. Needs Volumetric Light on.",
		type = "slider",
		cvar = "comfyNightDarkness",
		dependency = { "comfyVolume", "1" },
		minval = 0,
		maxval = 90,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percent: [night] tint, toward [night] moonColor.
		name = "COMFYATMOSPHERE_MOONLIGHT",
		desc = "How blue the night is, as moonlight. 0 only darkens.",
		type = "slider",
		cvar = "comfyMoonlight",
		dependency = { "comfyVolume", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		name = "COMFYATMOSPHERE_CLOUDS",
		desc = "The cloud layer in the sky.",
		type = "checkbutton",
		cvar = "comfyClouds",
	},
	{
		-- The panel cannot draw a dropdown, so each number is one view, named here and in comfyfog.log.
		-- The list is kDebugViews in comfyfog's cvars.cpp: keep the two in the same order.
		name = "COMFYATMOSPHERE_DEBUG_VIEW",
		desc = "For finding faults. Shows one stage of an effect instead of the game:\n"
			.. "0 off\n"
			.. "1 volumetric light: the glow alone\n"
			.. "2 volumetric light: share of each line in sun\n"
			.. "3 volumetric light: the shadow map\n"
			.. "4 volumetric light: the depth it reads\n"
			.. "5 sun shadows: the shade alone\n"
			.. "6 lamps: the glow alone\n"
			.. "7 lamps: the distance read\n"
			.. "8 lamps: the light on surfaces alone\n"
			.. "9 sun rays: the mask\n"
			.. "10 sun rays: the rays alone\n"
			.. "11 sun rays: the sky kept before the clouds",
		type = "slider",
		cvar = "comfyDebugView",
		minval = 0,
		maxval = 11,
		step = 1,
		numberLabels = 1,
	},
	-- The fog last (2026-09-30): it is off by default, and the owner wanted it at the bottom of the page.
	{
		name = "COMFYATMOSPHERE_FOG",
		desc = "Fog that is thick low down and thins higher up, and hides the edge of the view distance. With Volumetric Light off, the game's own fog is made thicker instead.",
		type = "checkbutton",
		cvar = "comfyFog",
	},
	{
		name = "COMFYATMOSPHERE_FOG_THICKNESS",
		desc = "How far you see into the fog. 100 is the heaviest.",
		type = "slider",
		cvar = "comfyFogThickness",
		dependency = { "comfyFog", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percent of the game's own fog distance: comfyfog.dll divides by 100 for [fog] reach.
		name = "COMFYATMOSPHERE_FOG_REACH",
		desc = "How far away the fog goes solid, as a share of the game's own fog distance. Higher lets you see further.",
		type = "slider",
		cvar = "comfyFogReach",
		minval = 20,
		maxval = 200,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percent: [fog] haze, the fog already where you stand when Fog Thickness is high.
		name = "COMFYATMOSPHERE_FOG_NEAR",
		desc = "How much fog there is right where you stand. Lower keeps things near you clear.",
		type = "slider",
		cvar = "comfyFogNear",
		minval = 0,
		maxval = 90,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percent: [fog] distance, the most the distance fog covers.
		name = "COMFYATMOSPHERE_FOG_FAR",
		desc = "The most the distance fog hides. Below 100, far shapes never vanish completely.",
		type = "slider",
		cvar = "comfyFogFar",
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- The height fog's density: 100 = 0.02 a yard ([fog] density). Needs Volumetric Light on.
		name = "COMFYATMOSPHERE_FOG_HAZE",
		desc = "Extra fog low down: in valleys and on the ground near you. 0 is none. Needs Volumetric Light on.",
		type = "slider",
		cvar = "comfyFogHaze",
		dependency = { "comfyFog", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Yards: the fog thins by 2.7 times every this many yards up. Needs Volumetric Light on.
		name = "COMFYATMOSPHERE_FOG_HEIGHT",
		desc = "How deep the fog layer is, in yards. Lower keeps the fog near the ground; higher fills the air.",
		type = "slider",
		cvar = "comfyFogHeight",
		dependency = { "comfyFog", "1" },
		minval = 10,
		maxval = 150,
		step = 5,
		numberLabels = 1,
	},
	{
		-- A percentage of the view distance: comfyfog.dll divides it by 100 for [fog] cover.
		name = "COMFYATMOSPHERE_FOG_FADE",
		desc = "Where things start to fade out before the edge of the view distance, as a percentage of it. Lower starts the fade sooner.",
		type = "slider",
		cvar = "comfyFogFade",
		dependency = { "comfyFog", "1" },
		minval = 50,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percentages, for [fog] darken and desaturate. They change the game's fog colour too, so no
		-- dependency.
		name = "COMFYATMOSPHERE_FOG_DARKNESS",
		desc = "How much darker than the game's own fog colour the fog is.",
		type = "slider",
		cvar = "comfyFogDarkness",
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		name = "COMFYATMOSPHERE_FOG_GREYNESS",
		desc = "How much of the fog colour is taken out, toward grey.",
		type = "slider",
		cvar = "comfyFogGreyness",
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percent: comfyfog.dll divides by 100 for [fog] sunGlow.
		name = "COMFYATMOSPHERE_FOG_SUN_GLOW",
		desc = "The fog turns warm looking toward the sun and cool looking away from it.",
		type = "slider",
		cvar = "comfyFogSunGlow",
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percent: comfyfog.dll divides by 100 for [fog] sunBright.
		name = "COMFYATMOSPHERE_FOG_SUN_BRIGHT",
		desc = "How much brighter the fog is looking straight into the sun.",
		type = "slider",
		cvar = "comfyFogSunBright",
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
};

-- pcall, because GetCVar raises an error for a CVar that does not exist.
local function HasCVar(name)
	local ok, value = pcall(GetCVar, name);
	return ok and value ~= nil;
end

local function DllLoaded()
	return HasCVar("comfyFog");
end

local function AddControls()
	if type(GameOptions) ~= "table" or not PIXEL_SHADERS then
		return false;
	end

	for _, category in ipairs(GameOptions) do
		if category.name == COMFYATMOSPHERE_CATEGORY then
			return true;    -- already present
		end
	end

	for index, category in ipairs(GameOptions) do
		if category.name == PIXEL_SHADERS and category.options then
			local options = {};
			for _, option in ipairs(ENTRIES) do
				if HasCVar(option.cvar) then
					table.insert(options, option);
				end
			end
			table.insert(GameOptions, index + 1, { name = COMFYATMOSPHERE_CATEGORY, options = options });
			-- The panel draws its list of pages once, as it loads, which is before this.
			if OptionsFrame_UpdateCategories then
				OptionsFrame_UpdateCategories();
			end
			return true;
		end
	end

	return false;
end

-- The settings window, /atmos options (2026-09-30). The same controls as the Atmosphere page, from the same
-- ENTRIES, in a window of our own: a client with the old options panel (stock 1.12 and most servers other
-- than Turtle) has no page for them. It uses only what stock 1.12 has: the UIPanelScrollFrameTemplate,
-- UICheckButtonTemplate, OptionsSliderTemplate and UIPanelButtonTemplate templates, and Lua 5.0.
--
-- A control sets its CVar as it moves, as the page does, and comfyfog.dll reads it at once. There is no
-- Cancel: close the window to keep the values. A control whose `dependency` is not met is greyed and does
-- not move, and is indented under the control it depends on.
local WINDOW_VALUE_TEXT = {
	comfyVolumeQuality = { "Low", "Medium", "High" },
	comfyShadowResolution = { "1024", "2048", "4096" },
};
local WINDOW_WIDTH = 266;
local WINDOW_HEIGHT = 500;
local CONTENT_WIDTH = 200;
local INDENT = 16;

local window = nil;
local windowControls = {};
local windowRefreshing = false;

local function WindowLabel(option)
	return getglobal(option.name) or option.name;
end

local function WindowSliderText(control)
	local value = tonumber(GetCVar(control.option.cvar)) or control.option.minval;
	local names = WINDOW_VALUE_TEXT[control.option.cvar];
	local shown = names and names[value - control.option.minval + 1] or tostring(value);
	getglobal(control.frame:GetName() .. "Text"):SetText(WindowLabel(control.option) .. ": " .. shown);
end

local function WindowRefresh()
	if not window then
		return;
	end
	windowRefreshing = true;
	for _, control in ipairs(windowControls) do
		local option = control.option;
		local enabled = not option.dependency or GetCVar(option.dependency[1]) == option.dependency[2];
		if option.type == "checkbutton" then
			control.frame:SetChecked(GetCVar(option.cvar) == "1");
			local text = getglobal(control.frame:GetName() .. "Text");
			if enabled then
				control.frame:Enable();
				text:SetTextColor(NORMAL_FONT_COLOR.r, NORMAL_FONT_COLOR.g, NORMAL_FONT_COLOR.b);
			else
				control.frame:Disable();
				text:SetTextColor(GRAY_FONT_COLOR.r, GRAY_FONT_COLOR.g, GRAY_FONT_COLOR.b);
			end
		else
			control.frame:SetValue(tonumber(GetCVar(option.cvar)) or option.minval);
			WindowSliderText(control);
			control.frame:EnableMouse(enabled);
			control.frame:SetAlpha(enabled and 1 or 0.4);
		end
	end
	windowRefreshing = false;
end

local function WindowTooltip()
	local option = this.comfyOption;
	GameTooltip:SetOwner(this, "ANCHOR_RIGHT");
	GameTooltip:SetText(WindowLabel(option), 1, 1, 1);
	if option.desc then
		GameTooltip:AddLine(option.desc, NORMAL_FONT_COLOR.r, NORMAL_FONT_COLOR.g, NORMAL_FONT_COLOR.b, 1);
	end
	if option.warning then
		GameTooltip:AddLine(option.warning, RED_FONT_COLOR.r, RED_FONT_COLOR.g, RED_FONT_COLOR.b, 1);
	end
	GameTooltip:Show();
end

local function WindowTooltipHide()
	GameTooltip:Hide();
end

local function WindowBuild()
	window = CreateFrame("Frame", "ComfyAtmosphereWindow", UIParent);
	window:SetWidth(WINDOW_WIDTH);
	window:SetHeight(WINDOW_HEIGHT);
	window:SetPoint("CENTER", UIParent, "CENTER", 0, 0);
	window:SetFrameStrata("DIALOG");
	window:SetBackdrop({
		bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
		edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
		tile = true, tileSize = 32, edgeSize = 32,
		insets = { left = 11, right = 12, top = 12, bottom = 11 },
	});
	window:EnableMouse(true);
	window:SetMovable(true);
	window:RegisterForDrag("LeftButton");
	window:SetScript("OnDragStart", function() this:StartMoving(); end);
	window:SetScript("OnDragStop", function() this:StopMovingOrSizing(); end);
	window:SetScript("OnShow", WindowRefresh);
	table.insert(UISpecialFrames, "ComfyAtmosphereWindow");    -- Escape closes it

	local header = window:CreateTexture(nil, "ARTWORK");
	header:SetTexture("Interface\\DialogFrame\\UI-DialogBox-Header");
	header:SetWidth(256);
	header:SetHeight(64);
	header:SetPoint("TOP", window, "TOP", 0, 12);
	local title = window:CreateFontString(nil, "ARTWORK", "GameFontNormal");
	title:SetPoint("TOP", header, "TOP", 0, -14);
	title:SetText(COMFYATMOSPHERE_CATEGORY);

	local close = CreateFrame("Button", "ComfyAtmosphereWindowClose", window, "UIPanelCloseButton");
	close:SetPoint("TOPRIGHT", window, "TOPRIGHT", -3, -3);

	local scroll = CreateFrame("ScrollFrame", "ComfyAtmosphereWindowScroll", window, "UIPanelScrollFrameTemplate");
	scroll:SetPoint("TOPLEFT", window, "TOPLEFT", 20, -30);
	scroll:SetPoint("BOTTOMRIGHT", window, "BOTTOMRIGHT", -40, 44);
	scroll:EnableMouseWheel(true);
	scroll:SetScript("OnMouseWheel", function()
		local bar = getglobal(this:GetName() .. "ScrollBar");
		bar:SetValue(bar:GetValue() - arg1 * 40);
	end);
	local content = CreateFrame("Frame", "ComfyAtmosphereWindowContent", scroll);
	content:SetWidth(CONTENT_WIDTH);

	-- How deep under other controls each CVar sits, for the indent.
	local depth = {};
	local y = -4;
	for i, entry in ipairs(ENTRIES) do
		local option = entry;
		if HasCVar(option.cvar) then
			local level = option.dependency and depth[option.dependency[1]] and depth[option.dependency[1]] + 1 or 0;
			depth[option.cvar] = level;
			local x = level * INDENT;
			local name = "ComfyAtmosphereWindowControl" .. i;
			local control = { option = option };
			if option.type == "checkbutton" then
				if level == 0 and y < -4 then
					y = y - 10;    -- a gap before each group
				end
				local box = CreateFrame("CheckButton", name, content, "UICheckButtonTemplate");
				box:SetWidth(26);
				box:SetHeight(26);
				box:SetPoint("TOPLEFT", content, "TOPLEFT", x, y);
				getglobal(name .. "Text"):SetText(WindowLabel(option));
				box:SetScript("OnClick", function()
					SetCVar(this.comfyOption.cvar, this:GetChecked() and "1" or "0");
					WindowRefresh();
				end);
				control.frame = box;
				y = y - 26;
			else
				local slider = CreateFrame("Slider", name, content, "OptionsSliderTemplate");
				slider:SetWidth(CONTENT_WIDTH - x - 16);
				slider:SetHeight(17);
				slider:SetPoint("TOPLEFT", content, "TOPLEFT", x + 8, y - 16);
				slider:SetMinMaxValues(option.minval, option.maxval);
				slider:SetValueStep(option.step);
				local names = WINDOW_VALUE_TEXT[option.cvar];
				getglobal(name .. "Low"):SetText(names and names[1] or tostring(option.minval));
				getglobal(name .. "High"):SetText(names and names[table.getn(names)] or tostring(option.maxval));
				slider:SetScript("OnValueChanged", function()
					if windowRefreshing then
						return;
					end
					local o = this.comfyOption;
					local value = o.minval + math.floor((this:GetValue() - o.minval) / o.step + 0.5) * o.step;
					SetCVar(o.cvar, tostring(value));
					WindowSliderText(this.comfyControl);
				end);
				slider.comfyControl = control;
				control.frame = slider;
				y = y - 50;
			end
			control.frame.comfyOption = option;
			control.frame:SetScript("OnEnter", WindowTooltip);
			control.frame:SetScript("OnLeave", WindowTooltipHide);
			table.insert(windowControls, control);
		end
	end
	content:SetHeight(-y + 8);
	scroll:SetScrollChild(content);
	scroll:UpdateScrollChildRect();

	-- The values comfyfog.dll registered the CVars with, which are comfyfog.ini's. GetCVarDefault is
	-- not in every 1.12 client, so the button is only there where it is.
	if GetCVarDefault then
		local defaults = CreateFrame("Button", "ComfyAtmosphereWindowDefaults", window, "UIPanelButtonTemplate");
		defaults:SetWidth(88);
		defaults:SetHeight(22);
		defaults:SetPoint("BOTTOMLEFT", window, "BOTTOMLEFT", 18, 16);
		defaults:SetText(DEFAULTS or "Defaults");
		defaults:SetScript("OnClick", function()
			for _, control in ipairs(windowControls) do
				local ok, value = pcall(GetCVarDefault, control.option.cvar);
				if ok and value then
					SetCVar(control.option.cvar, value);
				end
			end
			WindowRefresh();
		end);
	end
	local done = CreateFrame("Button", "ComfyAtmosphereWindowDone", window, "UIPanelButtonTemplate");
	done:SetWidth(88);
	done:SetHeight(22);
	done:SetPoint("BOTTOMRIGHT", window, "BOTTOMRIGHT", -18, 16);
	done:SetText(CLOSE or "Close");
	done:SetScript("OnClick", function() window:Hide(); end);

	window:Hide();
end

local function WindowToggle()
	if not DllLoaded() then
		DEFAULT_CHAT_FRAME:AddMessage("|cff88cc88atmos|r: comfyfog.dll is not loaded, so there are no settings to show.");
		return;
	end
	if not window then
		WindowBuild();
	end
	if window:IsShown() then
		window:Hide();
	else
		window:Show();
	end
end

-- /atmos: read and set any comfyfog.ini value (tune.cpp in comfyfog.dll). /atmos options is the window
-- above and stays in the addon. The command goes to the DLL in
-- the CVar comfyTune as "<number> <text>"; the DLL answers by registering comfyTuneReply<number>, the
-- count of lines, and comfyTuneReply<number>_1 and on, the lines. CVars stay registered until the
-- client closes, /reload included, so each command takes the next number that has no answer yet.
local tuneNumber = 0;
local tuneWaiting = nil;
local tuneUntil = 0;

local function Say(text)
	DEFAULT_CHAT_FRAME:AddMessage("|cff88cc88atmos|r: " .. text);
end

local tuneFrame = CreateFrame("Frame");
tuneFrame:Hide();
tuneFrame:SetScript("OnUpdate", function()
	if not tuneWaiting then
		this:Hide();
		return;
	end
	local count = HasCVar("comfyTuneReply" .. tuneWaiting) and tonumber(GetCVar("comfyTuneReply" .. tuneWaiting));
	if count then
		for i = 1, count do
			local name = "comfyTuneReply" .. tuneWaiting .. "_" .. i;
			if HasCVar(name) then
				Say(GetCVar(name));
			end
		end
	elseif GetTime() < tuneUntil then
		return;
	else
		Say("no answer from comfyfog.dll.");
	end
	tuneWaiting = nil;
	SetCVar("comfyTune", "");
	this:Hide();
end);

SLASH_COMFYATMOS1 = "/atmos";
SlashCmdList["COMFYATMOS"] = function(msg)
	local command = string.lower((string.gsub(msg or "", "^%s*(.-)%s*$", "%1")));
	if command == "options" then
		WindowToggle();
		return;
	end
	if command == "" then
		Say("/atmos options: the settings window.");
	end
	if not HasCVar("comfyTune") then
		Say("this comfyfog.dll has no /atmos. It needs the version from 2026-09-29 or later.");
		return;
	end
	if tuneWaiting then
		Say("still waiting for the last answer.");
		return;
	end
	repeat
		tuneNumber = tuneNumber + 1;
	until not HasCVar("comfyTuneReply" .. tuneNumber);
	tuneWaiting = tuneNumber;
	tuneUntil = GetTime() + 3;
	SetCVar("comfyTune", tuneNumber .. " " .. (msg or ""));
	tuneFrame:Show();
end;

-- The round shadow the game draws under each character and creature. The CVar shadowLOD ("Unit shadow
-- LOD", default 1) turns it off at 0; showShadow, which looks like the switch, is a console command that
-- does not (both measured 2026-09-29). It is off while the sun shadows draw, which is while Volumetric
-- Light and Sun Shadows are both ticked, and at the game's default otherwise.
--
-- At logout it goes back to the default, so the client does not save the 0 to Config.wtf: with the mod
-- removed, the round shadow comes back. VARIABLES_LOADED turns it off again at the next login.
local unitShadowOff = nil;

local function SyncUnitShadow(logout)
	local off = not logout and GetCVar("comfyVolume") == "1" and HasCVar("comfySunShadows")
		and GetCVar("comfySunShadows") == "1"
		and (not HasCVar("comfySunShadowsUnits") or GetCVar("comfySunShadowsUnits") == "1")
		and (not HasCVar("comfyAtmosphere") or GetCVar("comfyAtmosphere") == "1");
	if off == unitShadowOff then
		return;
	end
	unitShadowOff = off;
	SetCVar("shadowLOD", off and "0" or GetCVarDefault("shadowLOD"));
end

local syncFrame = CreateFrame("Frame");
syncFrame:Hide();
local syncWait = 0;
syncFrame:SetScript("OnUpdate", function()
	syncWait = syncWait - arg1;
	if syncWait > 0 then
		return;
	end
	syncWait = 0.5;
	SyncUnitShadow(false);
end);

local frame = CreateFrame("Frame");
frame:RegisterEvent("VARIABLES_LOADED");
frame:RegisterEvent("PLAYER_LOGOUT");
frame:SetScript("OnEvent", function()
	if not DllLoaded() then
		return;
	end
	if event == "PLAYER_LOGOUT" then
		syncFrame:Hide();
		SyncUnitShadow(true);
		return;
	end
	if not AddControls() then
		DEFAULT_CHAT_FRAME:AddMessage(
			"|cff88cc88comfyatmosphere|r: this client's options panel has no page for the settings. "
			.. "Type /atmos options to open them.");
	end
	syncFrame:Show();
end);
