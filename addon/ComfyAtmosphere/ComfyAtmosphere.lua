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
-- whole Shaders page. The controls are added only when the DLL has registered its CVars.
--
-- The client saves the values to Config.wtf itself, and only the ones moved away from their default
-- (measured: a thickness of 85 was written, fog at its default 1 was not). So a setting nobody moved
-- still follows comfyfog.ini, and the addon keeps no copy of its own.

COMFYATMOSPHERE_FOG            = "Atmospheric Fog";
COMFYATMOSPHERE_FOG_THICKNESS  = "Fog Thickness";
COMFYATMOSPHERE_VOLUME         = "Volumetric Light";
COMFYATMOSPHERE_VOLUME_STRENGTH = "Volumetric Light Strength";
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
		name = "COMFYATMOSPHERE_CLOUDS",
		desc = "The cloud layer in the sky.",
		type = "checkbutton",
		cvar = "comfyClouds",
	},
};

local function DllLoaded()
	-- pcall, because GetCVar raises an error for a CVar that does not exist.
	local ok, value = pcall(GetCVar, "comfyFog");
	return ok and value ~= nil;
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
				table.insert(category.options, option);
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
		DEFAULT_CHAT_FRAME:AddMessage(
			"|cff88cc88comfyatmosphere|r: comfyfog.dll is not loaded, so no controls were added "
			.. "to Video > Shaders.");
		return;
	end
	if not AddControls() then
		DEFAULT_CHAT_FRAME:AddMessage(
			"|cff88cc88comfyatmosphere|r: this client's options panel is not the data-driven one, "
			.. "so no controls were added. Use the F11 keys and comfyfog.ini instead.");
	end
end);
