-- Adds the comfyatmosphere controls to Video -> Shaders.
--
-- This client's options panel is data-driven, as the ComfyGrass addon describes: GameOptions is a global
-- table, and OptionsFrame.lua builds each page from it (both in patch-9.mpq). So each control is one
-- table entry with a cvar. The panel calls SetCVar while a slider moves, and comfyfog.dll reads the CVars
-- a few times a second, so a change shows in the world at once. Cancel puts the old values back the same
-- way.
--
-- comfyfog.dll registers the CVars, with the values in comfyfog.ini as their defaults. So the page's
-- Defaults button puts the ini values back.
--
-- Without the DLL the CVars do not exist, and GetCVar raises an error for a CVar that does not exist.
-- The panel calls GetCVar for every entry on a page, so an entry for a missing CVar would break the
-- whole Shaders page. The controls are added only when the DLL has registered its CVars, and each one
-- only if its own CVar exists: an older DLL has fewer of them than this addon.
--
-- Without the DLL the addon says nothing. A launcher that turns the mod off removes its dlls.txt line
-- and leaves this folder, so a message here would repeat at every login.
--
-- The client saves the values to Config.wtf itself, and only the ones moved away from their default
-- (measured: a thickness of 85 was written, fog at its default 1 was not). So a setting nobody moved
-- still follows comfyfog.ini, and the addon keeps no copy of its own.

COMFYATMOSPHERE_FOG            = "Atmospheric Fog";
COMFYATMOSPHERE_FOG_THICKNESS  = "Fog Thickness";
COMFYATMOSPHERE_VOLUME         = "Volumetric Light";
COMFYATMOSPHERE_VOLUME_STRENGTH = "Volumetric Light Strength";
COMFYATMOSPHERE_VOLUME_QUALITY = "Volumetric Light Quality";
COMFYATMOSPHERE_RAYS           = "Sun Rays";
COMFYATMOSPHERE_RAYS_STRENGTH  = "Sun Rays Strength";
COMFYATMOSPHERE_RAYS_SOFTEN    = "Sun Rays Softness";
COMFYATMOSPHERE_RAYS_SMOOTH    = "Sun Rays Smoothing";
COMFYATMOSPHERE_DEBUG_VIEW     = "Debug View";
COMFYATMOSPHERE_NIGHT_STRENGTH = "Night Strength";
COMFYATMOSPHERE_CLOUDS         = "Clouds";

local ENTRIES = {
	-- name is a KEY, not a string: the panel does _G[option.name] to get the label.
	{
		name = "COMFYATMOSPHERE_FOG",
		desc = "Thicker fog, with haze close to you. Trees and characters get the same fog as the ground.",
		type = "checkbutton",
		cvar = "comfyFog",
	},
	{
		name = "COMFYATMOSPHERE_FOG_THICKNESS",
		desc = "0 is the game's own fog. 100 is the heaviest.",
		type = "slider",
		cvar = "comfyFogThickness",
		dependency = { "comfyFog", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
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
			.. "6 sun shadows: how much each surface faces the sun\n"
			.. "7 lamps: the glow alone\n"
			.. "8 lamps: the distance read\n"
			.. "9 lamps: the light on surfaces alone\n"
			.. "10 sun rays: the mask\n"
			.. "11 sun rays: the rays alone\n"
			.. "12 sun rays: the sky kept before the clouds",
		type = "slider",
		cvar = "comfyDebugView",
		minval = 0,
		maxval = 12,
		step = 1,
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
		if category.name == PIXEL_SHADERS and category.options then
			for _, option in ipairs(category.options) do
				if option.cvar == "comfyFog" then
					return true;    -- already present
				end
			end
			for _, option in ipairs(ENTRIES) do
				if HasCVar(option.cvar) then
					table.insert(category.options, option);
				end
			end
			return true;
		end
	end

	return false;
end

local frame = CreateFrame("Frame");
frame:RegisterEvent("VARIABLES_LOADED");
frame:SetScript("OnEvent", function()
	if not DllLoaded() then
		return;
	end
	if not AddControls() then
		DEFAULT_CHAT_FRAME:AddMessage(
			"|cff88cc88comfyatmosphere|r: this client's options panel is not the data-driven one, "
			.. "so no controls were added. Use the F11 keys and comfyfog.ini instead.");
	end
end);
