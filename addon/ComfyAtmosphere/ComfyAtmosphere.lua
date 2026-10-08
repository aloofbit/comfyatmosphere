-- Adds the comfyatmosphere controls to the options window, on a page of their own: Video -> Atmosphere.
--
-- This client's options panel is data-driven, as the ComfyGrass addon describes: GameOptions is a global
-- table, and OptionsFrame.lua builds each page from it (both in patch-9.mpq). An entry with `options` is
-- a page in the list on the left, and one without is a heading (Video, Sound, Interface). So the page is
-- one entry, put after Shaders, and each control is one table entry with a cvar. The list shows 18 rows
-- and the client uses 15, so the page fits without scrolling the list. Until 2026-09-29 the controls
-- were added to the Shaders page.
--
-- The panel calls SetCVar while a slider moves, and comfyatmos.dll reads the CVars a few times a second, so
-- a change shows in the world at once. Cancel puts the old values back the same way.
--
-- comfyatmos.dll registers the CVars, with the values in comfyatmos.ini as their defaults. So the page's
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
-- still follows comfyatmos.ini, and the addon keeps no copy of its own.

COMFYATMOSPHERE_CATEGORY       = "Atmosphere";
COMFYATMOSPHERE_MASTER         = "Atmosphere Effects";
COMFYATMOSPHERE_VOLUME         = "Volumetric Light";
COMFYATMOSPHERE_VOLUME_STRENGTH = "Volumetric Light Strength";
COMFYATMOSPHERE_VOLUME_QUALITY = "Volumetric Light Quality";
COMFYATMOSPHERE_VOLUME_DENSITY = "Light Density";
COMFYATMOSPHERE_VOLUME_DISTANCE = "Light Distance";
COMFYATMOSPHERE_VOLUME_DIRECTION = "Light Toward the Sun";
COMFYATMOSPHERE_LAMPS          = "Lamps";
COMFYATMOSPHERE_LAMP_GLOW      = "Lamp Glow";
COMFYATMOSPHERE_LAMP_DISTANCE  = "Lamp Distance";
COMFYATMOSPHERE_LANTERN_LIGHT  = "Lantern Light";
COMFYATMOSPHERE_TORCH_LIGHT    = "Torch Light";
COMFYATMOSPHERE_INDOOR_LAMPS   = "Indoor Lamps";
COMFYATMOSPHERE_LAMPS_DAY      = "Lamps by Day";
COMFYATMOSPHERE_SUN_SHADOWS    = "Sun Shadows";
COMFYATMOSPHERE_SUN_SHADOW_STRENGTH = "Sun Shadow Strength";
COMFYATMOSPHERE_SHADOWS_NIGHT  = "Night Shadows";
COMFYATMOSPHERE_SHADOWS_UNIT_STRENGTH = "Character Shadow Strength";
COMFYATMOSPHERE_SHADOWS_BODY   = "Character Backside Shadow";
COMFYATMOSPHERE_SHADOW_ON_WATER = "Shadow on Water";
COMFYATMOSPHERE_SUN_GLIDE      = "Sun Smoothing";
COMFYATMOSPHERE_SHADOWS_WORLD  = "World / Object Shadows";
COMFYATMOSPHERE_SHADOWS_UNITS  = "Player / Creature Shadows";
COMFYATMOSPHERE_SHADOW_LOCK    = "Lock Shadow Angle";
COMFYATMOSPHERE_SHADOW_TILT    = "Shadow Angle";
COMFYATMOSPHERE_SUNLIGHT       = "Sunlight";
COMFYATMOSPHERE_TREE_SHADE     = "Tree Shadow Strength";
COMFYATMOSPHERE_WATER          = "Water Effects";
COMFYATMOSPHERE_WATER_COLOUR   = "Water Colour";
COMFYATMOSPHERE_WATER_ZONE     = "Zone Water";
COMFYATMOSPHERE_WATER_CLARITY  = "Water Clarity";
COMFYATMOSPHERE_WATER_BRIGHT   = "Water Brightness";
COMFYATMOSPHERE_WATER_REFLECT  = "Sky Reflection";
COMFYATMOSPHERE_WATER_BEND     = "Underwater Distortion";
COMFYATMOSPHERE_WATER_COVER    = "Underwater Cover";
COMFYATMOSPHERE_WATER_WAKE     = "Wake";
COMFYATMOSPHERE_WATER_RIPPLE_DEPTH = "Standing Ripple Depth";
COMFYATMOSPHERE_WATER_RIPPLE_MOVING = "Moving Ripple Depth";
COMFYATMOSPHERE_WATER_RIPPLE_SPREAD = "Standing Ripple Spread";
COMFYATMOSPHERE_WATER_RIPPLE_SPREAD_MOVING = "Moving Ripple Spread";
COMFYATMOSPHERE_WATER_FOAM     = "Foam";
COMFYATMOSPHERE_WATER_FOAM_DRAWN = "Drawn Foam";
COMFYATMOSPHERE_WATER_FOAM_SIZE = "Foam Size";
COMFYATMOSPHERE_WATER_FOAM_REACH = "Foam Reach";
COMFYATMOSPHERE_WATER_FOAM_EDGE = "Foam Breakup";
COMFYATMOSPHERE_WATER_WAKE_FOAM = "Wake Foam";
COMFYATMOSPHERE_WATER_SHIP_WAKE = "Ship Wake";
COMFYATMOSPHERE_WATER_SHIP_FORWARD = "Ship Wake Forward";
COMFYATMOSPHERE_WATER_SHIP_DEPTH = "Ship Wake Depth";
COMFYATMOSPHERE_WATER_OBJECT_FOAM = "Object Foam";
COMFYATMOSPHERE_WATER_LAKE_SWASH = "Lake Swash";
COMFYATMOSPHERE_WATER_LAKE_FOAM = "Lake Foam";
COMFYATMOSPHERE_WATER_LAKE_WAVES = "Lake Waves";
COMFYATMOSPHERE_WATER_RAIN = "Rain on Water";
COMFYATMOSPHERE_LIGHTHOUSES = "Lighthouses";
COMFYATMOSPHERE_LIGHTHOUSE_BEAM = "Lighthouse Beam";
COMFYATMOSPHERE_LH_BEACON = "Lighthouse Beacon";
COMFYATMOSPHERE_LH_LENGTH = "Beam Length";
COMFYATMOSPHERE_LH_DISTANCE = "Lighthouse Distance";
COMFYATMOSPHERE_LH_WIDTH = "Beam Width";
COMFYATMOSPHERE_LH_SPREAD = "Beam Spread";
COMFYATMOSPHERE_LH_SPEED = "Beam Speed";
COMFYATMOSPHERE_LH_TWO = "Two Beams";
COMFYATMOSPHERE_LH_GLINT = "Lighthouse Glint";
COMFYATMOSPHERE_LH_FACE = "Wave Face Strength";
COMFYATMOSPHERE_LH_TILT = "Wave Face Tilt";
COMFYATMOSPHERE_LH_SOFT = "Wave Face Softness";
COMFYATMOSPHERE_LH_WEDGE = "Beam Width on Water";
COMFYATMOSPHERE_WATER_OBJECT_FOAM_WIDTH = "Object Foam Width";
COMFYATMOSPHERE_WATER_OPEN_FOAM = "Open Water Foam";
COMFYATMOSPHERE_WATER_OPEN_FOAM_AMOUNT = "Open Water Foam Amount";
COMFYATMOSPHERE_WATER_SWASH    = "Swash";
COMFYATMOSPHERE_WATER_SWASH_HEIGHT = "Swash Height";
COMFYATMOSPHERE_WATER_SWASH_LENGTH = "Swash Length";
COMFYATMOSPHERE_WATER_SWASH_SPEED  = "Swash Speed";
COMFYATMOSPHERE_WATER_WET_SAND = "Wet Sand";
COMFYATMOSPHERE_WATER_GLINT    = "Sun Glint";
COMFYATMOSPHERE_WATER_MOON_GLINT = "Moon Glint";
COMFYATMOSPHERE_WATER_GLINT_SIZE = "Glint Size";
COMFYATMOSPHERE_WATER_EDGE     = "Edge Line";
COMFYATMOSPHERE_WATER_EDGE_WIDTH = "Edge Line Width";
COMFYATMOSPHERE_WAVE_HEIGHT    = "Wave Height";
COMFYATMOSPHERE_WAVE_SIZE      = "Wave Size";
COMFYATMOSPHERE_SHADE_TINT     = "Shade Colour";
COMFYATMOSPHERE_SUN_TINT       = "Sunlight Warmth";
COMFYATMOSPHERE_SHADOW_RESOLUTION = "Shadow Resolution";
COMFYATMOSPHERE_SHADOW_SOFTNESS = "Shadow Softness";
COMFYATMOSPHERE_SHADOW_EVERY   = "Shadow Redraw";
COMFYATMOSPHERE_SHADOW_NEAR    = "Near Shadow Distance";
COMFYATMOSPHERE_RAYS           = "Sun Rays";
COMFYATMOSPHERE_RAYS_STRENGTH  = "Sun Rays Strength";
COMFYATMOSPHERE_RAYS_SOFTEN    = "Sun Rays Softness";
COMFYATMOSPHERE_RAYS_SMOOTH    = "Sun Rays Smoothing";
COMFYATMOSPHERE_DEBUG_VIEW     = "Debug View";
COMFYATMOSPHERE_NIGHT_STRENGTH = "Night Strength";
COMFYATMOSPHERE_NIGHT_DARKNESS = "Night Darkness";
COMFYATMOSPHERE_RAIN_DARKNESS = "Rain Darkness";
COMFYATMOSPHERE_MOONLIGHT      = "Moonlight Colour";
COMFYATMOSPHERE_COLOR          = "Color Effects";
COMFYATMOSPHERE_DAY_SATURATION = "Day Saturation";
COMFYATMOSPHERE_NIGHT_SATURATION = "Night Saturation";
COMFYATMOSPHERE_CLOUDS         = "Clouds";
COMFYATMOSPHERE_MIST           = "Fog";
COMFYATMOSPHERE_MIST_DENSITY   = "Fog Density";
COMFYATMOSPHERE_MIST_HEIGHT    = "Fog Height";
COMFYATMOSPHERE_MIST_BRIGHTNESS = "Fog Brightness";
COMFYATMOSPHERE_MIST_SUN       = "Fog Sunlight";
COMFYATMOSPHERE_MIST_REACH     = "Fog Reach";
COMFYATMOSPHERE_MIST_SKY       = "Fog on Sky";
COMFYATMOSPHERE_MIST_PATCHES   = "Fog Patchiness";
COMFYATMOSPHERE_MIST_LOW       = "Low Ground Mist";
COMFYATMOSPHERE_MIST_WATER     = "Water Mist";
COMFYATMOSPHERE_MIST_MORNING   = "Morning Mist";
COMFYATMOSPHERE_MIST_LAMPS     = "Lamps in Mist";
COMFYATMOSPHERE_MIST_WIND      = "Wind Speed";
COMFYATMOSPHERE_MIST_WIND_DIR  = "Wind Direction";
COMFYATMOSPHERE_GRASS          = "Grass";
COMFYATMOSPHERE_GRASS_WIND     = "Grass Wind";
COMFYATMOSPHERE_GRASS_SPEED    = "Grass Wave Speed";
COMFYATMOSPHERE_GRASS_WAVE     = "Grass Wave Length";
COMFYATMOSPHERE_GRASS_WIND_DIR = "Grass Wind Direction";
COMFYATMOSPHERE_GRASS_LEAN     = "Grass Lean";
COMFYATMOSPHERE_GRASS_PARTING  = "Grass Parting";
COMFYATMOSPHERE_GRASS_RADIUS   = "Grass Parting Radius";
COMFYATMOSPHERE_FOLIAGE_DENSITY = "Foliage Density";

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
		-- Thousandths: comfyatmos.dll divides by 1000 for [volume] density (15 is 0.015).
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
		-- Thousandths: comfyatmos.dll divides by 1000 for [volume] anisotropy (25 is 0.025).
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
		-- The fog rides on the light's march. Without the light the march runs for the fog alone, with no
		-- shadow map (2026-10-02).
		name = "COMFYATMOSPHERE_MIST",
		desc = "Fog that lies on the ground and thins higher up. The sun lights it. With Volumetric Light on, it shows shafts where trees and walls shade it.",
		type = "checkbutton",
		cvar = "comfyMist",
	},
	{
		-- Ten-thousandths a yard: comfyatmos.dll divides by 10000 for [fog] density (40 is 0.004).
		name = "COMFYATMOSPHERE_MIST_DENSITY",
		desc = "How thick the fog is at the ground. Higher, you see less far.",
		type = "slider",
		cvar = "comfyMistDensity",
		dependency = { "comfyMist", "1" },
		minval = 0,
		maxval = 200,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Yards, [fog] height: the fog thins by 2.7 times every this many yards up.
		name = "COMFYATMOSPHERE_MIST_HEIGHT",
		desc = "How deep the fog layer is, in yards. Lower keeps it on the ground; higher fills the air.",
		type = "slider",
		cvar = "comfyMistHeight",
		dependency = { "comfyMist", "1" },
		minval = 5,
		maxval = 200,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percent: [fog] brightness, the sky's light on the fog (the game's fog colour times this).
		name = "COMFYATMOSPHERE_MIST_BRIGHTNESS",
		desc = "How bright the fog is in the shade. 100 is the game's own fog colour.",
		type = "slider",
		cvar = "comfyMistBrightness",
		dependency = { "comfyMist", "1" },
		minval = 0,
		maxval = 200,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Tenths: comfyatmos.dll divides by 10 for [fog] sunLight (40 is 4).
		name = "COMFYATMOSPHERE_MIST_SUN",
		desc = "How brightly the sun lights the fog.",
		type = "slider",
		cvar = "comfyMistSun",
		dependency = { "comfyMist", "1" },
		minval = 0,
		maxval = 200,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Yards, [fog] reach. Past it the game's own fog is the far wall.
		name = "COMFYATMOSPHERE_MIST_REACH",
		desc = "How far away, in yards, the fog still gathers. Lower lets you see further across open land and sea.",
		type = "slider",
		cvar = "comfyMistReach",
		dependency = { "comfyMist", "1" },
		minval = 100,
		maxval = 2000,
		step = 50,
		numberLabels = 1,
	},
	{
		-- Yards, [fog] skyDistance: how far a line of sight to the sky gathers fog.
		name = "COMFYATMOSPHERE_MIST_SKY",
		desc = "How much the fog covers the sky, in yards of fog. 0 leaves the sky clear; higher hides the horizon.",
		type = "slider",
		cvar = "comfyMistSky",
		dependency = { "comfyMist", "1" },
		minval = 0,
		maxval = 1000,
		step = 25,
		numberLabels = 1,
	},
	{
		-- Percent: [fog] patchiness.
		name = "COMFYATMOSPHERE_MIST_PATCHES",
		desc = "0 is an even fog. Higher breaks it into patches with clear air between them.",
		type = "slider",
		cvar = "comfyMistPatches",
		dependency = { "comfyMist", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percent: [fog] lowGround. 150 makes a valley 25 yards deep hold 2.5 times the fog.
		name = "COMFYATMOSPHERE_MIST_LOW",
		desc = "Extra mist in valleys and hollows. 0 is none.",
		type = "slider",
		cvar = "comfyMistLow",
		dependency = { "comfyMist", "1" },
		minval = 0,
		maxval = 400,
		step = 10,
		numberLabels = 1,
	},
	{
		-- Percent: [fog] water. 100 doubles the fog over water.
		name = "COMFYATMOSPHERE_MIST_WATER",
		desc = "Extra mist over rivers, lakes and the sea. 0 is none.",
		type = "slider",
		cvar = "comfyMistWater",
		dependency = { "comfyMist", "1" },
		minval = 0,
		maxval = 400,
		step = 10,
		numberLabels = 1,
	},
	{
		-- Percent: [fog] morning. 100 doubles the fog at dawn and adds half at dusk.
		name = "COMFYATMOSPHERE_MIST_MORNING",
		desc = "Extra mist at dawn, and half as much at dusk. 0 is none.",
		type = "slider",
		cvar = "comfyMistMorning",
		dependency = { "comfyMist", "1" },
		minval = 0,
		maxval = 400,
		step = 10,
		numberLabels = 1,
	},
	{
		-- Percent: [fog] lampMist.
		name = "COMFYATMOSPHERE_MIST_LAMPS",
		desc = "How much brighter lamps glow in thick mist. 0 is no change.",
		type = "slider",
		cvar = "comfyMistLamps",
		dependency = { "comfyMist", "1" },
		minval = 0,
		maxval = 200,
		step = 10,
		numberLabels = 1,
	},
	{
		-- Tenths of a yard a second: comfyatmos.dll divides by 10 for [fog] windSpeed (20 is 2).
		name = "COMFYATMOSPHERE_MIST_WIND",
		desc = "How fast the wind carries the fog patches.",
		type = "slider",
		cvar = "comfyMistWind",
		dependency = { "comfyMist", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Degrees, [fog] windDeg: the way the wind blows. 0 north, 90 east.
		name = "COMFYATMOSPHERE_MIST_WIND_DIR",
		desc = "The way the wind blows, in degrees: 0 north, 90 east, 180 south, 270 west.",
		type = "slider",
		cvar = "comfyMistWindDir",
		dependency = { "comfyMist", "1" },
		minval = 0,
		maxval = 360,
		step = 15,
		numberLabels = 1,
	},
	{
		-- [lamps] enabled. Night Darkness is drawn in the lamps' pass but stays on without them.
		name = "COMFYATMOSPHERE_LAMPS",
		desc = "Lamps, candles and torches glow in the air and light the walls and ground near them. Needs Volumetric Light on.",
		type = "checkbutton",
		cvar = "comfyLamps",
		dependency = { "comfyVolume", "1" },
	},
	{
		-- [lamps] strength: the glow around lamps, candles and torches and their light on the walls near them.
		name = "COMFYATMOSPHERE_LAMP_GLOW",
		desc = "How strongly lamps, candles and torches glow and light the walls and ground near them.",
		type = "slider",
		cvar = "comfyLampGlow",
		dependency = { "comfyLamps", "1" },
		minval = 0,
		maxval = 50,
		step = 1,
		numberLabels = 1,
	},
	{
		-- Percent: [lamps] fogReach. 100 fades a lamp's glow as the game fades its lamp sprites in the fog.
		name = "COMFYATMOSPHERE_LAMP_DISTANCE",
		desc = "How far away lamps still glow. At 100 they fade into the fog as the game's own lamp glows do.",
		type = "slider",
		cvar = "comfyLampDistance",
		dependency = { "comfyLamps", "1" },
		minval = 50,
		maxval = 400,
		step = 10,
		numberLabels = 1,
	},
	{
		-- Percent: [lamps] lanternLight.
		name = "COMFYATMOSPHERE_LANTERN_LIGHT",
		desc = "How brightly lampposts, lanterns, candles and chandeliers glow and light the ground and walls near them.",
		type = "slider",
		cvar = "comfyLanternLight",
		dependency = { "comfyLamps", "1" },
		minval = 0,
		maxval = 200,
		step = 1,
		numberLabels = 1,
	},
	{
		-- Percent: [lamps] torchLight. The game lights its models with its own torches already.
		name = "COMFYATMOSPHERE_TORCH_LIGHT",
		desc = "How brightly torches, braziers, campfires and the torches NPCs carry glow and light the ground and walls near them. The game lights characters near them already, so at 100 they are lit twice.",
		type = "slider",
		cvar = "comfyTorchLight",
		dependency = { "comfyLamps", "1" },
		minval = 0,
		maxval = 200,
		step = 1,
		numberLabels = 1,
	},
	{
		-- Percent: [lamps] indoors.
		name = "COMFYATMOSPHERE_INDOOR_LAMPS",
		desc = "How bright lamps, candles and fires are while you are inside a building. A room holds many lights close together.",
		type = "slider",
		cvar = "comfyIndoorLamps",
		dependency = { "comfyLamps", "1" },
		minval = 0,
		maxval = 100,
		step = 1,
		numberLabels = 1,
	},
	{
		-- Percent: [lamps] day, of the night's strength. The change follows the game clock at dusk and dawn.
		name = "COMFYATMOSPHERE_LAMPS_DAY",
		desc = "How bright lamps, candles and torches are by day, as a share of their night brightness. 0 puts them out by day. Inside buildings, Indoor Lamps applies instead.",
		type = "slider",
		cvar = "comfyLampsDay",
		dependency = { "comfyLamps", "1" },
		minval = 0,
		maxval = 100,
		step = 1,
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
		name = "COMFYATMOSPHERE_SHADOWS_NIGHT",
		desc = "The shadows the moon casts at night. 100 is as dark as by day. 0 is off.",
		type = "slider",
		cvar = "comfySunShadowsNight",
		dependency = { "comfySunShadows", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		name = "COMFYATMOSPHERE_SHADOWS_UNIT_STRENGTH",
		desc = "Makes the shadows of players and creatures darker than the world's, also inside the shade of a building. 0 makes them as dark as the world's.",
		type = "slider",
		cvar = "comfySunShadowsUnitStrength",
		dependency = { "comfySunShadowsUnits", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percent: comfyatmos.dll takes it as [sunshadows] bodyShade.
		name = "COMFYATMOSPHERE_SHADOWS_BODY",
		desc = "How dark the shade is on the body of a player or creature, on its side away from the sun. 100 is the darkest. 0 is off: the body keeps only the game's own lighting. The shadow it casts does not change.",
		type = "slider",
		cvar = "comfySunShadowsBody",
		dependency = { "comfySunShadowsUnits", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percent: comfyatmos.dll divides by 100 for [sunshadows] water.
		name = "COMFYATMOSPHERE_SHADOW_ON_WATER",
		desc = "How dark a shadow is on the water's surface, against one on the ground, with a soft edge. The bed under clear water keeps its own shadow. 0 is none on the surface.",
		type = "slider",
		cvar = "comfyShadowOnWater",
		dependency = { "comfySunShadows", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Seconds: comfyatmos.dll takes it as [sun] glide.
		name = "COMFYATMOSPHERE_SUN_GLIDE",
		desc = "Seconds at least that the shadows, sun rays and light take to follow a step of the sun. The sky moves the sun once a game minute, and each move is spread over the minute. 0 follows at once.",
		type = "slider",
		cvar = "comfySunGlide",
		dependency = { "comfyVolume", "1" },
		minval = 0,
		maxval = 30,
		step = 1,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [sunshadows] leafShade.
		name = "COMFYATMOSPHERE_TREE_SHADE",
		desc = "How dark the shadows of trees and bushes are. They still show inside the shadow of a hill or a mountain.",
		type = "slider",
		cvar = "comfyTreeShade",
		dependency = { "comfyVolume", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- comfyatmos.dll takes it as [water] enabled.
		name = "COMFYATMOSPHERE_WATER",
		desc = "The water's colour, waves and sun glint, the foam and wet sand at the shore, and ripples round anyone in the water. Off, the game draws its own water.",
		type = "checkbutton",
		cvar = "comfyWater",
	},
	{
		-- comfyatmos.dll takes it as [water] colour.
		name = "COMFYATMOSPHERE_WATER_COLOUR",
		desc = "The colour deep water turns. 0 is green, 50 is teal, 100 is blue.",
		type = "slider",
		cvar = "comfyWaterColour",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- comfyatmos.dll takes it as [water] zone (2026-10-07).
		name = "COMFYATMOSPHERE_WATER_ZONE",
		desc = "How far the water's colour leans to the game's own water colour for the zone and the time of day. 0 is Water Colour alone.",
		type = "slider",
		cvar = "comfyWaterZone",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [water] brightness.
		name = "COMFYATMOSPHERE_WATER_BRIGHT",
		desc = "How light or dark the water is. The glint and the foam keep their brightness.",
		type = "slider",
		cvar = "comfyWaterBright",
		dependency = { "comfyWater", "1" },
		minval = 25,
		maxval = 200,
		step = 5,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [water] clarity.
		name = "COMFYATMOSPHERE_WATER_CLARITY",
		desc = "How far you see into the water. 200 is twice as far as 100.",
		type = "slider",
		cvar = "comfyWaterClarity",
		dependency = { "comfyWater", "1" },
		minval = 25,
		maxval = 400,
		step = 25,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [water] reflection.
		name = "COMFYATMOSPHERE_WATER_REFLECT",
		desc = "How much of the sky the water shows when you look across it. 0 shows only the water's own colour.",
		type = "slider",
		cvar = "comfyWaterReflect",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- comfyatmos.dll takes it as [water] refraction: 100 is half a yard.
		name = "COMFYATMOSPHERE_WATER_BEND",
		desc = "How much the waves bend what you see under the water. 0 is off.",
		type = "slider",
		cvar = "comfyWaterBend",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [water] foam.
		name = "COMFYATMOSPHERE_WATER_FOAM",
		desc = "How white the foam is: at the shore, on the ripples and in the wake. Lower lets the water show through it. 0 is off.",
		type = "slider",
		cvar = "comfyWaterFoam",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- [water] foamDrawn (2026-10-05): foam made by comfyatmos.dll, or the game's waterfall texture as before.
		name = "COMFYATMOSPHERE_WATER_FOAM_DRAWN",
		desc = "Foam in clean, soft shapes that break up as they age. Off, the game's own foam texture.",
		type = "checkbutton",
		cvar = "comfyWaterFoamDrawn",
		dependency = { "comfyWater", "1" },
	},
	{
		-- Tenths of a yard: comfyatmos.dll divides by 10 for [water] foamCell.
		name = "COMFYATMOSPHERE_WATER_FOAM_SIZE",
		desc = "How large the shapes of the foam are, in tenths of a yard.",
		type = "slider",
		cvar = "comfyWaterFoamSize",
		dependency = { "comfyWaterFoamDrawn", "1" },
		minval = 3,
		maxval = 30,
		step = 1,
		numberLabels = 1,
	},
	{
		-- Tenths of a yard: comfyatmos.dll divides by 10 for [water] foamLife.
		name = "COMFYATMOSPHERE_WATER_FOAM_REACH",
		desc = "How far out from the water's edge the foam lasts, in tenths of a yard.",
		type = "slider",
		cvar = "comfyWaterFoamReach",
		dependency = { "comfyWaterFoamDrawn", "1" },
		minval = 5,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides by 100 for [water] foamEdge.
		name = "COMFYATMOSPHERE_WATER_FOAM_EDGE",
		desc = "How broken the foam is at the water's edge. Low makes a solid band of foam along the sand; high leaves it in pieces, as farther out.",
		type = "slider",
		cvar = "comfyWaterFoamEdge",
		dependency = { "comfyWaterFoamDrawn", "1" },
		minval = 0,
		maxval = 90,
		step = 5,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides by 100 for [water] wakeFoam.
		name = "COMFYATMOSPHERE_WATER_WAKE_FOAM",
		desc = "The splash of foam at anyone walking or swimming through the water. 0 is none.",
		type = "slider",
		cvar = "comfyWaterWakeFoam",
		dependency = { "comfyWaterFoamDrawn", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- [water] shipWake: a ship's wake as a body's this many times the size; 0 off.
		name = "COMFYATMOSPHERE_WATER_SHIP_WAKE",
		desc = "The wake behind a ship under way, as a swimmer's wake this many times the size. 0 is none.",
		type = "slider",
		cvar = "comfyWaterShipWake",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 20,
		step = 1,
		numberLabels = 1,
	},
	{
		-- [water] shipWakeForward: yards ahead of the hull's origin, along its way.
		name = "COMFYATMOSPHERE_WATER_SHIP_FORWARD",
		desc = "How far forward along the ship the wake starts, in yards.",
		type = "slider",
		cvar = "comfyWaterShipForward",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 60,
		step = 1,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides by 100 for [water] shipWakeDepth.
		name = "COMFYATMOSPHERE_WATER_SHIP_DEPTH",
		desc = "How deep the waves of a ship's wake are, against a swimmer's.",
		type = "slider",
		cvar = "comfyWaterShipDepth",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 600,
		step = 10,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides by 100 for [water] lakeSwash.
		name = "COMFYATMOSPHERE_WATER_LAKE_SWASH",
		desc = "How much of the sea's swash and shore waves a lake, a pond or a river gets. 0 is none, 100 as much as the sea.",
		type = "slider",
		cvar = "comfyWaterLakeSwash",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides by 100 for [water] lakeFoam.
		name = "COMFYATMOSPHERE_WATER_LAKE_FOAM",
		desc = "How much of the sea's shore foam a lake, a pond or a river gets. 100 is as much as the sea.",
		type = "slider",
		cvar = "comfyWaterLakeFoam",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides by 100 for [water] lakeWaves.
		name = "COMFYATMOSPHERE_WATER_LAKE_WAVES",
		desc = "How much of the sea's waves (Wave Height) a lake, a pond or a river gets. 0 lies flat, 100 rises as the sea.",
		type = "slider",
		cvar = "comfyWaterLakeWaves",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- [lighthouse] enabled: a beacon and turning beams on the lighthouses at night.
		name = "COMFYATMOSPHERE_LIGHTHOUSES",
		desc = "A light in each lighthouse at night, with beams turning round.",
		type = "checkbutton",
		cvar = "comfyLighthouses",
	},
	{
		-- A percentage: comfyatmos.dll divides by 100 for [lighthouse] beam.
		name = "COMFYATMOSPHERE_LIGHTHOUSE_BEAM",
		desc = "How bright the lighthouses' beams are. 0 leaves the light alone.",
		type = "slider",
		cvar = "comfyLighthouseBeam",
		dependency = { "comfyLighthouses", "1" },
		minval = 0,
		maxval = 100,
		step = 2,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides by 100 for [lighthouse] beacon.
		name = "COMFYATMOSPHERE_LH_BEACON",
		desc = "How bright the light in the lamp room is.",
		type = "slider",
		cvar = "comfyLighthouseBeacon",
		dependency = { "comfyLighthouses", "1" },
		minval = 0,
		maxval = 300,
		step = 10,
		numberLabels = 1,
	},
	{
		-- Yards: [lighthouse] beamLength.
		name = "COMFYATMOSPHERE_LH_LENGTH",
		desc = "How far the beam reaches, in yards.",
		type = "slider",
		cvar = "comfyLighthouseLength",
		dependency = { "comfyLighthouses", "1" },
		minval = 50,
		maxval = 1000,
		step = 25,
		numberLabels = 1,
	},
	{
		-- Yards: [lighthouse] reach.
		name = "COMFYATMOSPHERE_LH_DISTANCE",
		desc = "How far off a lighthouse's light shows, in yards. It fades out over the last quarter.",
		type = "slider",
		cvar = "comfyLighthouseDistance",
		dependency = { "comfyLighthouses", "1" },
		minval = 100,
		maxval = 1500,
		step = 50,
		numberLabels = 1,
	},
	{
		-- Tenths of a yard: comfyatmos.dll divides by 10 for [lighthouse] beamWidth.
		name = "COMFYATMOSPHERE_LH_WIDTH",
		desc = "How thick the beam is at the lamp, in tenths of a yard.",
		type = "slider",
		cvar = "comfyLighthouseWidth",
		dependency = { "comfyLighthouses", "1" },
		minval = 1,
		maxval = 50,
		step = 1,
		numberLabels = 1,
	},
	{
		-- Thousandths: comfyatmos.dll divides by 1000 for [lighthouse] beamSpread.
		name = "COMFYATMOSPHERE_LH_SPREAD",
		desc = "How much wider the beam gets as it goes out. 0 is a straight beam.",
		type = "slider",
		cvar = "comfyLighthouseSpread",
		dependency = { "comfyLighthouses", "1" },
		minval = 0,
		maxval = 150,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Seconds: [lighthouse] beamSpeed.
		name = "COMFYATMOSPHERE_LH_SPEED",
		desc = "Seconds the beam takes to go round once.",
		type = "slider",
		cvar = "comfyLighthouseSpeed",
		dependency = { "comfyLighthouses", "1" },
		minval = 2,
		maxval = 60,
		step = 1,
		numberLabels = 1,
	},
	{
		-- [lighthouse] beamCount: 2 with this on, else 1.
		name = "COMFYATMOSPHERE_LH_TWO",
		desc = "A second beam, opposite the first.",
		type = "checkbutton",
		cvar = "comfyLighthouseTwoBeams",
		dependency = { "comfyLighthouses", "1" },
	},
	{
		-- A percentage: comfyatmos.dll divides by 100 for [lighthouse] glint.
		name = "COMFYATMOSPHERE_LH_GLINT",
		desc = "How much light the lighthouse puts on the water: its glitter and the waves its beam lights.",
		type = "slider",
		cvar = "comfyLighthouseGlint",
		dependency = { "comfyLighthouses", "1" },
		minval = 0,
		maxval = 300,
		step = 10,
		numberLabels = 1,
	},
	{
		-- A percentage: [lighthouse] faceStrength.
		name = "COMFYATMOSPHERE_LH_FACE",
		desc = "How bright the waves are where the beam crosses the water.",
		type = "slider",
		cvar = "comfyLighthouseFace",
		dependency = { "comfyLighthouses", "1" },
		minval = 0,
		maxval = 400,
		step = 10,
		numberLabels = 1,
	},
	{
		-- Thousandths: [lighthouse] faceTilt.
		name = "COMFYATMOSPHERE_LH_TILT",
		desc = "How far a wave must tilt toward the lighthouse to catch the beam. Lower lights more of them.",
		type = "slider",
		cvar = "comfyLighthouseFaceTilt",
		dependency = { "comfyLighthouses", "1" },
		minval = 0,
		maxval = 300,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Thousandths: [lighthouse] faceSoft.
		name = "COMFYATMOSPHERE_LH_SOFT",
		desc = "How soft the edge is between the waves the beam lights and the dark ones.",
		type = "slider",
		cvar = "comfyLighthouseFaceSoft",
		dependency = { "comfyLighthouses", "1" },
		minval = 5,
		maxval = 300,
		step = 5,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides by 100 for [lighthouse] waterWidth.
		name = "COMFYATMOSPHERE_LH_WEDGE",
		desc = "How wide the patch of water the beam lights is, as a share of the beam's own width. Beam Spread widens it too.",
		type = "slider",
		cvar = "comfyLighthouseWaterWidth",
		dependency = { "comfyLighthouses", "1" },
		minval = 25,
		maxval = 400,
		step = 5,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides by 100 for [water] rain.
		name = "COMFYATMOSPHERE_WATER_RAIN",
		desc = "Rings on the water where the rain falls, while it rains. 0 is none.",
		type = "slider",
		cvar = "comfyWaterRain",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 200,
		step = 10,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides by 100 for [water] objectFoam.
		name = "COMFYATMOSPHERE_WATER_OBJECT_FOAM",
		desc = "Foam round anything standing in the water: posts, rocks, piers. 0 is none.",
		type = "slider",
		cvar = "comfyWaterObjectFoam",
		dependency = { "comfyWaterFoamDrawn", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Tenths of a yard: comfyatmos.dll divides by 10 for [water] objectFoamWidth.
		name = "COMFYATMOSPHERE_WATER_OBJECT_FOAM_WIDTH",
		desc = "How far out from an object in the water its foam reaches, in tenths of a yard.",
		type = "slider",
		cvar = "comfyWaterObjectFoamWidth",
		dependency = { "comfyWaterFoamDrawn", "1" },
		minval = 2,
		maxval = 30,
		step = 1,
		numberLabels = 1,
	},
	{
		-- [water] openFoam: foam on the swell's crests out in deep water.
		name = "COMFYATMOSPHERE_WATER_OPEN_FOAM",
		desc = "Foam on the tops of the waves out in deep water.",
		type = "checkbutton",
		cvar = "comfyWaterOpenFoam",
		dependency = { "comfyWater", "1" },
	},
	{
		-- A percentage: comfyatmos.dll divides by 100 for [water] whitecaps.
		name = "COMFYATMOSPHERE_WATER_OPEN_FOAM_AMOUNT",
		desc = "How much foam the waves out in deep water carry.",
		type = "slider",
		cvar = "comfyWaterOpenFoamAmount",
		dependency = { "comfyWaterOpenFoam", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- comfyatmos.dll takes it as [water] swash, on or off (2026-10-05: a slider whose 5 to 100 changed nothing).
		name = "COMFYATMOSPHERE_WATER_SWASH",
		desc = "The water runs up the beach and slides back, with foam on its edge.",
		type = "checkbutton",
		cvar = "comfyWaterSwash",
		dependency = { "comfyWater", "1" },
	},
	{
		-- Hundredths of a yard: comfyatmos.dll divides it by 100 for [water] swashHeight.
		name = "COMFYATMOSPHERE_WATER_SWASH_HEIGHT",
		desc = "How far up the shore the water runs. 30 climbs 0.3 yards, and 1.5 yards up a gentle beach.",
		type = "slider",
		cvar = "comfyWaterSwashHeight",
		dependency = { "comfyWaterSwash", "1" },
		minval = 5,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Yards: comfyatmos.dll takes it as [water] swashLength.
		name = "COMFYATMOSPHERE_WATER_SWASH_LENGTH",
		desc = "How many yards along the shore one surge spans. Lower gives more, shorter surges side by side.",
		type = "slider",
		cvar = "comfyWaterSwashLength",
		dependency = { "comfyWaterSwash", "1" },
		minval = 5,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [water] swashSpeed.
		name = "COMFYATMOSPHERE_WATER_SWASH_SPEED",
		desc = "How fast the surges come. 100 is one every 11 seconds.",
		type = "slider",
		cvar = "comfyWaterSwashSpeed",
		dependency = { "comfyWaterSwash", "1" },
		minval = 10,
		maxval = 300,
		step = 10,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [water] wetSand (2026-10-07).
		name = "COMFYATMOSPHERE_WATER_WET_SAND",
		desc = "How much darker the sand is just above the water, where the waves have wet it. 0 is off.",
		type = "slider",
		cvar = "comfyWaterWetSand",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [water] edgeLine.
		name = "COMFYATMOSPHERE_WATER_EDGE",
		desc = "How bright the thin white line is where the water meets the sand. 0 is off.",
		type = "slider",
		cvar = "comfyWaterEdge",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Tenths of a yard: comfyatmos.dll divides it by 10 for [water] edgeWidth.
		name = "COMFYATMOSPHERE_WATER_EDGE_WIDTH",
		desc = "How wide that line is, in tenths of a yard along the sand.",
		type = "slider",
		cvar = "comfyWaterEdgeWidth",
		dependency = { "comfyWater", "1" },
		minval = 1,
		maxval = 20,
		step = 1,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [water] glint.
		name = "COMFYATMOSPHERE_WATER_GLINT",
		desc = "How bright the sun's reflection on the waves is. 0 is off.",
		type = "slider",
		cvar = "comfyWaterGlint",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 300,
		step = 10,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [water] moonGlint.
		name = "COMFYATMOSPHERE_WATER_MOON_GLINT",
		desc = "How bright the two moons' reflections on the waves are at night. 0 is off.",
		type = "slider",
		cvar = "comfyWaterMoonGlint",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 300,
		step = 10,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [water] glintSize.
		name = "COMFYATMOSPHERE_WATER_GLINT_SIZE",
		desc = "How wide the sun's and moons' reflections spread. 200 is twice as wide as 100.",
		type = "slider",
		cvar = "comfyWaterGlintSize",
		dependency = { "comfyWater", "1" },
		minval = 25,
		maxval = 400,
		step = 25,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [water] wake.
		name = "COMFYATMOSPHERE_WATER_WAKE",
		desc = "The wake behind anyone moving through the water. 0 is off.",
		type = "slider",
		cvar = "comfyWaterWake",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [water] rippleDepth.
		name = "COMFYATMOSPHERE_WATER_RIPPLE_DEPTH",
		desc = "How deep the rings round anyone standing still in the water look. 100 is as it was; their white line stays the same.",
		type = "slider",
		cvar = "comfyWaterRippleDepth",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 400,
		step = 10,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [water] rippleDepthMoving.
		name = "COMFYATMOSPHERE_WATER_RIPPLE_MOVING",
		desc = "How deep the rings look that anyone walking or swimming leaves behind. 100 is as it was; their white line stays the same.",
		type = "slider",
		cvar = "comfyWaterRippleMoving",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 400,
		step = 10,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [water] rippleSpread.
		name = "COMFYATMOSPHERE_WATER_RIPPLE_SPREAD",
		desc = "How large the rings round anyone standing still in the water grow. 100 is as it was.",
		type = "slider",
		cvar = "comfyWaterRippleSpread",
		dependency = { "comfyWater", "1" },
		minval = 10,
		maxval = 300,
		step = 10,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [water] rippleSpreadMoving.
		name = "COMFYATMOSPHERE_WATER_RIPPLE_SPREAD_MOVING",
		desc = "How large the rings grow that anyone walking or swimming leaves behind. 100 is as it was.",
		type = "slider",
		cvar = "comfyWaterRippleSpreadMoving",
		dependency = { "comfyWater", "1" },
		minval = 10,
		maxval = 300,
		step = 10,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [water] cover.
		name = "COMFYATMOSPHERE_WATER_COVER",
		desc = "How much the water hides a character standing in it. 0 shows the legs as clear as the sand beside them.",
		type = "slider",
		cvar = "comfyWaterCover",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Tenths of a yard: comfyatmos.dll divides it by 10 for [water] waveHeight.
		name = "COMFYATMOSPHERE_WAVE_HEIGHT",
		desc = "How tall the waves rise out on deep water, in tenths of a yard. They calm toward the shore. 0 is flat water.",
		type = "slider",
		cvar = "comfyWaveHeight",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 30,
		step = 1,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [water] waveScale.
		name = "COMFYATMOSPHERE_WAVE_SIZE",
		desc = "How long the waves are, in percent. 200 is twice as long as 100.",
		type = "slider",
		cvar = "comfyWaveSize",
		dependency = { "comfyWater", "1" },
		minval = 50,
		maxval = 400,
		step = 25,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [sunshadows] sunlight.
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
		-- Percent: comfyatmos.dll divides by 100 for [sunshadows] shadeTint.
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
		-- Percent: comfyatmos.dll divides by 100 for [sunshadows] sunTint.
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
		-- Yards, [shadow] nearRange: the sharpest of the three shadow maps reaches this far either side of you.
		name = "COMFYATMOSPHERE_SHADOW_NEAR",
		desc = "How far around you, in yards, shadows have the sharpest edges. Higher reaches further, and the edges near you are coarser.",
		type = "slider",
		cvar = "comfyShadowNear",
		dependency = { "comfyVolume", "1" },
		minval = 16,
		maxval = 128,
		step = 8,
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
		-- A percentage: comfyatmos.dll divides it by 100 for [rays] smooth.
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
		-- Percent: comfyatmos.dll divides by 100 for [night] darkness. It is drawn with the lamps, from the
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
		-- Percent: comfyatmos.dll divides by 100 for [night] rain.
		name = "COMFYATMOSPHERE_RAIN_DARKNESS",
		desc = "How much darker again the night is while it rains, the sky too. The game's rain makes the night a lighter grey.",
		type = "slider",
		cvar = "comfyRainDarkness",
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
		-- [colour] enabled: off, the saturation pass does not run at all.
		name = "COMFYATMOSPHERE_COLOR",
		desc = "Day and night saturation. Off costs nothing.",
		type = "checkbutton",
		cvar = "comfyColor",
	},
	{
		-- Percent: [colour] day. The change to Night Saturation follows the game clock, as Night Darkness.
		name = "COMFYATMOSPHERE_DAY_SATURATION",
		desc = "How strong the colours are by day. 100 is the game's own. 0 is grey.",
		type = "slider",
		cvar = "comfyDaySaturation",
		dependency = { "comfyColor", "1" },
		minval = 0,
		maxval = 200,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percent: [colour] night.
		name = "COMFYATMOSPHERE_NIGHT_SATURATION",
		desc = "How strong the colours are at night. 100 is the game's own. 0 is grey. The change from day is slow, from dusk to full night.",
		type = "slider",
		cvar = "comfyNightSaturation",
		dependency = { "comfyColor", "1" },
		minval = 0,
		maxval = 200,
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
		name = "COMFYATMOSPHERE_GRASS",
		desc = "The grass sways in the wind and leans away from your character. Rocks, pebbles, shells and bones stay still.",
		type = "checkbutton",
		cvar = "comfyGrass",
	},
	{
		-- Percent: comfyatmos.dll divides it by 100 for [grass] scale. The parting does not follow it.
		name = "COMFYATMOSPHERE_GRASS_WIND",
		desc = "How strongly the wind sways the grass, in percent. 0 is still.",
		type = "slider",
		cvar = "comfyGrassWind",
		dependency = { "comfyGrass", "1" },
		minval = 0,
		maxval = 500,
		step = 10,
		numberLabels = 1,
	},
	{
		-- Tenths of a yard a second: [grass] speed.
		name = "COMFYATMOSPHERE_GRASS_SPEED",
		desc = "How fast the waves run through the grass, in tenths of a yard a second.",
		type = "slider",
		cvar = "comfyGrassSpeed",
		dependency = { "comfyGrass", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Yards: [grass] wavelength.
		name = "COMFYATMOSPHERE_GRASS_WAVE",
		desc = "Yards from one wave to the next. Small values sway small patches; large values make wide waves.",
		type = "slider",
		cvar = "comfyGrassWaveLength",
		dependency = { "comfyGrass", "1" },
		minval = 4,
		maxval = 60,
		step = 1,
		numberLabels = 1,
	},
	{
		-- Degrees: [grass] directionDeg.
		name = "COMFYATMOSPHERE_GRASS_WIND_DIR",
		desc = "Where the wind blows the grass, in degrees.",
		type = "slider",
		cvar = "comfyGrassWindDir",
		dependency = { "comfyGrass", "1" },
		minval = 0,
		maxval = 360,
		step = 15,
		numberLabels = 1,
	},
	{
		-- Percent of the first wave: [grass] lean.
		name = "COMFYATMOSPHERE_GRASS_LEAN",
		desc = "How far the grass leans down the wind all the time, in percent of its sway.",
		type = "slider",
		cvar = "comfyGrassLean",
		dependency = { "comfyGrass", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percent: [grass] forceCenter.
		name = "COMFYATMOSPHERE_GRASS_PARTING",
		desc = "How far the grass leans away from your character, in percent. 0 is none.",
		type = "slider",
		cvar = "comfyGrassParting",
		dependency = { "comfyGrass", "1" },
		minval = 0,
		maxval = 150,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Tenths of a yard: [grass] radius.
		name = "COMFYATMOSPHERE_GRASS_RADIUS",
		desc = "How far round your character the grass parts, in tenths of a yard.",
		type = "slider",
		cvar = "comfyGrassPartingRadius",
		dependency = { "comfyGrass", "1" },
		minval = 5,
		maxval = 60,
		step = 5,
		numberLabels = 1,
	},
	{
		-- The client's own frillDensity, not ours: it works without comfyatmos.dll and is saved by the client. The
		-- ComfyGrass addon put it under World Appearance until 2026-10-06. The client plants
		-- min(frillDensity * 64, 8192) models, so above 128 nothing changes.
		name = "COMFYATMOSPHERE_FOLIAGE_DENSITY",
		desc = "How much grass and ground clutter the world plants. Costs frame rate, not memory.",
		type = "slider",
		cvar = "frillDensity",
		minval = 8,
		maxval = 128,
		step = 8,
		numberLabels = 1,
	},
	{
		-- The panel cannot draw a dropdown, so each number is one view, named here and in comfyatmos.log.
		-- The list is kDebugViews in comfyatmos's cvars.cpp: keep the two in the same order.
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
			.. "11 sun rays: the sky kept before the clouds\n"
			.. "12 fog: how much gets through (white = clear)\n"
			.. "13 fog: the sky's light on it alone\n"
			.. "14 fog: where mist collects (low ground, water)\n"
			.. "15 sun shadows: the bodies it finds\n"
			.. "16 water: as drawn, over what lies under it: magenta the far slice, orange untextured draws, cyan terrain past 80 yards\n"
			.. "17 water: the foam alone\n"
			.. "18 water: the wet sand alone\n"
			.. "19 water: what lies under it (red: the far terrain, blue: only sky behind)\n"
			.. "20 water: ripples (red, green) and the wake (blue)\n"
			.. "21 water: the game's own water drawn (red), other liquid (magenta), ours off\n"
			.. "22 water: the height over the water, in contours every 0.1 yards (green above, red below, blue the dry cells)\n"
			.. "23 water: the slope the swash uses (dark to bright up to 0.3, a line every 0.05)\n"
			.. "24 sun shadows: where the shade comes from (red solid, green leaves, blue hills)\n"
			.. "25 sun shadows: shade taken off behind a hill (red solid, green leaves, blue where the check runs)\n"
			.. "26 sun shadows: the hill check's depths (red the solid caster before the hill, green behind it, blue the point behind the hill)\n"
			.. "27 water: the drawn foam (white) over its age (red), the open water's foam (blue)\n"
			.. "28 sun shadows: the water it finds (blue: the water's depth over a bed, grey: none, so the shade falls on what the depth shows)\n"
			.. "29 water: the depth under it (0 to 256 yards, blue to red, a line at 1, 2, 4, 8 ... 256; hatched: the map's depth)\n"
			.. "30 grass: the bend (black still, white the tips; blue where it comes from the texture)",
		type = "slider",
		cvar = "comfyDebugView",
		minval = 0,
		maxval = 30,
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
	return HasCVar("comfyAtmosphere");
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
-- A control sets its CVar as it moves, as the page does, and comfyatmos.dll reads it at once. There is no
-- Cancel: close the window to keep the values. A control whose `dependency` is not met is greyed and does
-- not move.
--
-- The controls are in sections, one word each across the top, as the ComfyUI bank's filters; the chosen
-- one's controls show below (2026-10-01; until then one long list with each control indented under the one it
-- depends on; buttons along the top and then down the left made the window too wide). Atmosphere Effects, the
-- box for all of them, sits above the sections. A control no section names goes to the last section, so a new
-- one is never lost. Debug View is left out: the debug window has it.
local WINDOW_VALUE_TEXT = {
	comfyVolumeQuality = { "Low", "Medium", "High" },
	comfyShadowResolution = { "1024", "2048", "4096" },
};
local WINDOW_SECTIONS = {
	{ "Light", { "comfyVolume", "comfyVolumeStrength", "comfyVolumeQuality", "comfyVolumeDensity",
	             "comfyVolumeDistance", "comfyVolumeDirection" } },
	{ "Fog", { "comfyMist", "comfyMistDensity", "comfyMistHeight", "comfyMistBrightness", "comfyMistSun",
	           "comfyMistReach", "comfyMistSky", "comfyMistPatches", "comfyMistLow", "comfyMistWater",
	           "comfyMistMorning", "comfyMistLamps", "comfyMistWind", "comfyMistWindDir" } },
	{ "Lamps", { "comfyLamps", "comfyLampGlow", "comfyLampDistance", "comfyLanternLight", "comfyTorchLight",
	             "comfyIndoorLamps", "comfyLampsDay", "comfyLighthouses", "comfyLighthouseBeam", "comfyLighthouseBeacon", "comfyLighthouseLength", "comfyLighthouseDistance", "comfyLighthouseWidth", "comfyLighthouseSpread", "comfyLighthouseSpeed", "comfyLighthouseTwoBeams", "comfyLighthouseGlint", "comfyLighthouseFace", "comfyLighthouseFaceTilt", "comfyLighthouseFaceSoft", "comfyLighthouseWaterWidth" } },
	{ "Shadows", { "comfySunShadows", "comfySunShadowsWorld", "comfySunShadowsUnits", "comfyShadowLock",
	               "comfyShadowTilt", "comfySunShadowStrength", "comfySunShadowsNight",
	               "comfySunShadowsUnitStrength", "comfySunShadowsBody", "comfyShadowOnWater", "comfySunGlide", "comfyTreeShade", "comfySunlight", "comfyShadeTint",
	               "comfySunTint", "comfyShadowResolution", "comfyShadowSoftness", "comfyShadowEvery",
	               "comfyShadowNear" } },
	{ "Sky", { "comfyRays", "comfyRaysStrength", "comfyRaysSoften", "comfyRaysSmooth", "comfyNightStrength",
	           "comfyNightDarkness", "comfyRainDarkness", "comfyMoonlight", "comfyClouds" } },
	{ "Color", { "comfyColor", "comfyDaySaturation", "comfyNightSaturation" } },
	{ "Grass", { "comfyGrass", "comfyGrassWind", "comfyGrassSpeed", "comfyGrassWaveLength", "comfyGrassWindDir",
	             "comfyGrassLean", "comfyGrassParting", "comfyGrassPartingRadius", "frillDensity" } },
	{ "Water", { "comfyWater", "comfyWaterColour", "comfyWaterZone", "comfyWaterBright", "comfyWaterClarity", "comfyWaterReflect", "comfyWaterBend", "comfyWaterCover", "comfyWaterFoam", "comfyWaterFoamDrawn", "comfyWaterFoamSize", "comfyWaterFoamReach", "comfyWaterFoamEdge", "comfyWaterWakeFoam", "comfyWaterShipWake", "comfyWaterShipForward", "comfyWaterShipDepth", "comfyWaterLakeSwash", "comfyWaterLakeFoam", "comfyWaterLakeWaves", "comfyWaterRain", "comfyWaterObjectFoam", "comfyWaterObjectFoamWidth", "comfyWaterOpenFoam", "comfyWaterOpenFoamAmount", "comfyWaterSwash", "comfyWaterSwashHeight", "comfyWaterSwashLength", "comfyWaterSwashSpeed", "comfyWaterWetSand", "comfyWaterWake", "comfyWaterRippleDepth", "comfyWaterRippleMoving", "comfyWaterRippleSpread", "comfyWaterRippleSpreadMoving",
	             "comfyWaterEdge", "comfyWaterEdgeWidth",
	             "comfyWaterGlint", "comfyWaterMoonGlint", "comfyWaterGlintSize",
	             "comfyWaveHeight",
	             "comfyWaveSize" } },
};
local WINDOW_MASTER = "comfyAtmosphere";
-- Left out of this window: Debug View has the debug window (/atmos debug, the Debug button below).
local WINDOW_LEFT_OUT = { comfyDebugView = true };
local WINDOW_WIDTH = 246;
local WINDOW_HEIGHT = 480;
local CONTENT_WIDTH = 198;
local CHIP_HEIGHT = 20;     -- a section's word, as the ComfyUI bank's filters
local CHIP_GAP = 0;

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

-- A grey button, as Turtle's options frame draws its Defaults (UIPanelButtonGrayTemplate, patch-9.mpq): the
-- stock disabled button art with white text. Drawn here from the art itself, so a client without Turtle's
-- template gets the same button.
local function GreyButton(button)
	button:SetNormalTexture("Interface\\Buttons\\UI-Panel-Button-Disabled");
	button:GetNormalTexture():SetTexCoord(0, 0.625, 0, 0.6875);
	button:SetPushedTexture("Interface\\Buttons\\UI-Panel-Button-Disabled-Down");
	button:GetPushedTexture():SetTexCoord(0, 0.625, 0, 0.6875);
	button:SetTextColor(1, 1, 1);
end

local function WindowBuild()
	window = CreateFrame("Frame", "ComfyAtmosphereWindow", UIParent);
	window:SetWidth(WINDOW_WIDTH);
	window:SetHeight(WINDOW_HEIGHT);
	-- At the left of the screen, so the character and the world in the middle stay in view while a value
	-- is moved (2026-10-01).
	window:SetPoint("LEFT", UIParent, "LEFT", 40, 0);
	window:SetFrameStrata("DIALOG");
	window:SetBackdrop({
		bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
		tile = true, tileSize = 32,
		insets = { left = 11, right = 12, top = 12, bottom = 11 },
	});
	window:EnableMouse(true);
	window:SetMovable(true);
	window:RegisterForDrag("LeftButton");
	window:SetScript("OnDragStart", function() this:StartMoving(); end);
	window:SetScript("OnDragStop", function() this:StopMovingOrSizing(); end);
	window:SetScript("OnShow", WindowRefresh);
	table.insert(UISpecialFrames, "ComfyAtmosphereWindow");    -- Escape closes it

	-- The window's border on a frame of its own, above the panels: a frame draws its own backdrop under its
	-- children, so the panels reach under this and it covers their outer edges. It takes no mouse. The title
	-- and the close button go above it.
	local rim = CreateFrame("Frame", "ComfyAtmosphereWindowRim", window);
	rim:SetAllPoints(window);
	rim:SetBackdrop({
		edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
		edgeSize = 32,
		insets = { left = 11, right = 12, top = 12, bottom = 11 },
	});
	rim:SetFrameLevel(window:GetFrameLevel() + 10);

	local header = rim:CreateTexture(nil, "ARTWORK");
	header:SetTexture("Interface\\DialogFrame\\UI-DialogBox-Header");
	header:SetWidth(256);
	header:SetHeight(64);
	header:SetPoint("TOP", window, "TOP", 0, 12);
	local title = rim:CreateFontString(nil, "OVERLAY", "GameFontNormal");
	title:SetPoint("TOP", header, "TOP", 0, -14);
	title:SetText(COMFYATMOSPHERE_CATEGORY);

	local close = CreateFrame("Button", "ComfyAtmosphereWindowClose", window, "UIPanelCloseButton");
	close:SetPoint("TOPRIGHT", window, "TOPRIGHT", -3, -3);
	close:SetFrameLevel(rim:GetFrameLevel() + 1);

	-- The controls in a panel with no border, and the scroll bar in a bordered column of its own along the
	-- panel's right edge, as Turtle's options frame has its scroll bar.
	local function Box(name, parent, edge, alpha)
		local box = CreateFrame("Frame", name, parent);
		box:SetBackdrop({
			bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
			edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
			tile = true, tileSize = 16, edgeSize = edge,
			insets = { left = edge / 4, right = edge / 4, top = edge / 4, bottom = edge / 4 },
		});
		box:SetBackdropColor(0, 0, 0, alpha);
		box:SetBackdropBorderColor(0.6, 0.6, 0.6, 1);
		return box;
	end
	-- The panel's top is set under the section words, once they are laid out.
	local panel = CreateFrame("Frame", "ComfyAtmosphereWindowPanel", window);   -- holds the layout; no border

	-- A bar across the window, as thick as the window's own frame: the top edge of UI-DialogBox-Border, the
	-- third of its eight pieces, stored on its side, so it is drawn a quarter turn round. The visible bar is
	-- the outer 12 of the piece's 32 pixels, so the strip starts 6 above the line. On the window, under its
	-- border, which covers the bar's ends. (The tooltip border's edge, used first, was far thinner.)
	local function Divider()
		local line = window:CreateTexture(nil, "ARTWORK");
		line:SetTexture("Interface\\DialogFrame\\UI-DialogBox-Border");
		line:SetTexCoord(0.25, 0, 0.375, 0, 0.25, 1, 0.375, 1);
		line:SetHeight(32);
		return line;
	end
	-- Above the buttons.
	local lower = Divider();
	lower:SetPoint("TOPLEFT", window, "BOTTOMLEFT", 6, 46);
	lower:SetPoint("TOPRIGHT", window, "BOTTOMRIGHT", -6, 46);
	panel:SetPoint("BOTTOMRIGHT", window, "BOTTOMRIGHT", -6, 37);

	local scroll = CreateFrame("ScrollFrame", "ComfyAtmosphereWindowScroll", panel, "UIPanelScrollFrameTemplate");
	scroll:SetPoint("TOPLEFT", panel, "TOPLEFT", 6, -4);
	scroll:SetPoint("BOTTOMRIGHT", panel, "BOTTOMRIGHT", -30, 4);
	-- The scroll bar's column: the template puts the bar 6 to 22 pixels right of the frame, so the column runs
	-- from 2 right of it to the panel's inner edge.
	local track = Box("ComfyAtmosphereWindowScrollTrack", panel, 12, 0.3);
	track:SetPoint("TOPLEFT", panel, "TOPRIGHT", -28, -3);
	track:SetPoint("BOTTOMRIGHT", panel, "BOTTOMRIGHT", -3, 3);
	scroll:EnableMouseWheel(true);
	scroll:SetScript("OnMouseWheel", function()
		local bar = getglobal(this:GetName() .. "ScrollBar");
		bar:SetValue(bar:GetValue() - arg1 * 40);
	end);

	-- Each CVar's tab: its section, or the last.
	local sectionOf = {};
	for i, section in ipairs(WINDOW_SECTIONS) do
		for _, cvar in ipairs(section[2]) do
			sectionOf[cvar] = i;
		end
	end
	local count = table.getn(WINDOW_SECTIONS);

	-- One control, at y in its frame; returns the y under it.
	local function Add(option, i, parent, x, y, width)
		local name = "ComfyAtmosphereWindowControl" .. i;
		local control = { option = option };
		if option.type == "checkbutton" then
			local box = CreateFrame("CheckButton", name, parent, "UICheckButtonTemplate");
			box:SetWidth(26);
			box:SetHeight(26);
			box:SetPoint("TOPLEFT", parent, "TOPLEFT", x, y);
			getglobal(name .. "Text"):SetText(WindowLabel(option));
			box:SetScript("OnClick", function()
				SetCVar(this.comfyOption.cvar, this:GetChecked() and "1" or "0");
				WindowRefresh();
			end);
			control.frame = box;
			y = y - 30;
		else
			local slider = CreateFrame("Slider", name, parent, "OptionsSliderTemplate");
			slider:SetWidth(width - 16);
			slider:SetHeight(17);
			slider:SetPoint("TOPLEFT", parent, "TOPLEFT", x + 8, y - 16);
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
		return y;
	end

	-- Atmosphere Effects, at the top, over every section.
	local chipTop = -32;
	for i, option in ipairs(ENTRIES) do
		if option.cvar == WINDOW_MASTER and HasCVar(option.cvar) then
			Add(option, i, window, 12, chipTop, CONTENT_WIDTH);
			chipTop = chipTop - 30;
		end
	end

	-- The sections: a word each, as the ComfyUI bank's filters are (Storage.lua), flowing across and
	-- wrapping. The chosen one has a gold ground of its own: LockHighlight draws what hovering draws.
	local chips = {};
	local x, line = 0, 0;
	for t = 1, count do
		local chip = CreateFrame("Button", "ComfyAtmosphereWindowSection" .. t, window);
		chip:SetHeight(CHIP_HEIGHT);
		local text = chip:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall");
		text:SetPoint("CENTER", chip, "CENTER", 0, 0);
		text:SetText(WINDOW_SECTIONS[t][1]);
		chip.text = text;
		local w = text:GetStringWidth() + 10;
		chip:SetWidth(w);
		if x > 0 and x + w > WINDOW_WIDTH - 24 then
			x = 0;
			line = line + 1;
		end
		chip:SetPoint("TOPLEFT", window, "TOPLEFT", 12 + x, chipTop - line * (CHIP_HEIGHT + CHIP_GAP));
		x = x + w + CHIP_GAP;
		local sel = chip:CreateTexture(nil, "BACKGROUND");
		sel:SetAllPoints(chip);
		sel:SetTexture(1, 0.82, 0, 0.25);
		sel:Hide();
		chip.sel = sel;
		chip:SetHighlightTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight", "ADD");
		chip.comfyTab = t;
		chips[t] = chip;
	end
	panel:SetPoint("TOPLEFT", window, "TOPLEFT", 6, chipTop - (line + 1) * (CHIP_HEIGHT + CHIP_GAP) - 2);
	-- Under the section words.
	local upper = Divider();
	upper:SetPoint("TOPLEFT", window, "TOPLEFT", 6, chipTop - (line + 1) * (CHIP_HEIGHT + CHIP_GAP) + 4);
	upper:SetPoint("TOPRIGHT", window, "TOPRIGHT", -6, chipTop - (line + 1) * (CHIP_HEIGHT + CHIP_GAP) + 4);

	-- A frame for each section, in the scroll frame's place while it is chosen.
	local contents, heights = {}, {};
	for t = 1, count do
		local content = CreateFrame("Frame", "ComfyAtmosphereWindowContent" .. t, scroll);
		content:SetWidth(CONTENT_WIDTH);
		content:Hide();
		contents[t] = content;
		heights[t] = -4;
	end
	for i, option in ipairs(ENTRIES) do
		if option.cvar ~= WINDOW_MASTER and not WINDOW_LEFT_OUT[option.cvar] and HasCVar(option.cvar) then
			local t = sectionOf[option.cvar] or count;
			heights[t] = Add(option, i, contents[t], 0, heights[t], CONTENT_WIDTH);
		end
	end
	for t = 1, count do
		contents[t]:SetHeight(-heights[t] + 8);
	end

	local function Choose(t)
		for k = 1, count do
			contents[k]:Hide();
			if k == t then
				chips[k].sel:Show();
				chips[k].text:SetTextColor(1, 1, 1);
			else
				chips[k].sel:Hide();
				chips[k].text:SetTextColor(0.7, 0.7, 0.7);
			end
		end
		contents[t]:Show();
		scroll:SetScrollChild(contents[t]);
		scroll:UpdateScrollChildRect();
		getglobal(scroll:GetName() .. "ScrollBar"):SetValue(0);
		window.comfyTab = t;
	end
	for t = 1, count do
		chips[t]:SetScript("OnClick", function() Choose(this.comfyTab); end);
	end
	Choose(window.comfyTab or 1);

	-- The values comfyatmos.dll registered the CVars with, which are comfyatmos.ini's. GetCVarDefault is
	-- not in every 1.12 client, so the button is only there where it is.
	if GetCVarDefault then
		local defaults = CreateFrame("Button", "ComfyAtmosphereWindowDefaults", window, "UIPanelButtonTemplate");
		defaults:SetWidth(72);
		defaults:SetHeight(22);
		defaults:SetPoint("BOTTOMLEFT", window, "BOTTOMLEFT", 14, 12);
		defaults:SetText(DEFAULTS or "Defaults");
		GreyButton(defaults);
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
	-- The debug panel (/atmos debug), between the two.
	local debug = CreateFrame("Button", "ComfyAtmosphereWindowDebug", window, "UIPanelButtonTemplate");
	debug:SetWidth(72);
	debug:SetHeight(22);
	debug:SetPoint("BOTTOM", window, "BOTTOM", 0, 12);
	debug:SetText("Debug");
	GreyButton(debug);
	debug:SetScript("OnClick", function() ComfyAtmosphere_DebugToggle(); end);
	local done = CreateFrame("Button", "ComfyAtmosphereWindowDone", window, "UIPanelButtonTemplate");
	done:SetWidth(72);
	done:SetHeight(22);
	done:SetPoint("BOTTOMRIGHT", window, "BOTTOMRIGHT", -14, 12);
	done:SetText(CLOSE or "Close");
	done:SetScript("OnClick", function() window:Hide(); end);

	-- ShaguTweaks' dark mode darkens the frames there when it loads, and this window is built later, on the
	-- first /atmos options: it asks for the same. Nothing happens without ShaguTweaks or with dark mode off.
	if ShaguTweaks and ShaguTweaks.DarkenFrame then
		ShaguTweaks.DarkenFrame(window);
	end

	window:Hide();
end

local function WindowToggle()
	if not DllLoaded() then
		DEFAULT_CHAT_FRAME:AddMessage("|cff88cc88atmos|r: comfyatmos.dll is not loaded, so there are no settings to show.");
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

-- /atmos: read and set any comfyatmos.ini value (tune.cpp in comfyatmos.dll). /atmos options is the window
-- above and stays in the addon. The command goes to the DLL in
-- the CVar comfyTune as "<number> <text>"; the DLL answers by registering comfyTuneReply<number>, the
-- count of lines, and comfyTuneReply<number>_1 and on, the lines. CVars stay registered until the
-- client closes, /reload included, so each command takes the next number that has no answer yet.
local tuneNumber = 0;
local tuneWaiting = nil;
local tuneUntil = 0;
-- Commands given while one waits for its answer, sent in order as the answers come (2026-10-04). They were
-- refused ("still waiting for the last answer"): a macro or a test sending several at once lost some, and a
-- test's sun elevation went missing that way.
local tuneQueue = {};
local TuneSend;

local function Say(text)
	DEFAULT_CHAT_FRAME:AddMessage("|cff88cc88atmos|r: " .. text);
end

-- Notices: lines the DLL starts on its own (the benchmark's start and end, F12), as comfyNotice1, comfyNotice2
-- and on. Those already registered when the addon loads were printed before a /reload, so they are skipped.
local noticeNext = 1;
while HasCVar("comfyNotice" .. noticeNext) do
	noticeNext = noticeNext + 1;
end
local noticeFrame = CreateFrame("Frame");
local noticeWait = 0;
noticeFrame:SetScript("OnUpdate", function()
	noticeWait = noticeWait - arg1;
	if noticeWait > 0 then
		return;
	end
	noticeWait = 0.5;
	while HasCVar("comfyNotice" .. noticeNext) do
		Say(GetCVar("comfyNotice" .. noticeNext));
		noticeNext = noticeNext + 1;
	end
end);

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
		Say("no answer from comfyatmos.dll.");
	end
	tuneWaiting = nil;
	SetCVar("comfyTune", "");
	if table.getn(tuneQueue) > 0 then
		TuneSend(table.remove(tuneQueue, 1));
		return;
	end
	this:Hide();
end);

TuneSend = function(msg)
	repeat
		tuneNumber = tuneNumber + 1;
	until not HasCVar("comfyTuneReply" .. tuneNumber);
	tuneWaiting = tuneNumber;
	tuneUntil = GetTime() + 3;
	SetCVar("comfyTune", tuneNumber .. " " .. (msg or ""));
	tuneFrame:Show();
end;

SLASH_COMFYATMOS1 = "/atmos";
-- /atmos stats: comfyatmos.dll's figures on screen, for finding faults. The DLL writes them into the CVar
-- comfyStats once a second as name=value; pairs, padded with spaces, and this lays them out in sections.
-- /atmos debug: a panel of buttons for finding faults (probe, stats, debug view, trace, benchmark), and the time
-- of day.
-- Every action can be put on a key in Key Bindings > ComfyAtmosphere (Bindings.xml), with no key bound at first.
-- comfyatmos.dll's own keys (F11, F12, Ctrl+PageUp and the rest) are read past the game's key bindings, so they
-- are off unless the debug panel's Developer keys box is ticked (2026-10-07).

BINDING_HEADER_COMFYATMOSPHERE           = "ComfyAtmosphere";
BINDING_NAME_COMFYATMOSPHERE_PROBE       = "Probe (log one frame)";
BINDING_NAME_COMFYATMOSPHERE_STATS       = "Show or hide the stats";
BINDING_NAME_COMFYATMOSPHERE_DEBUG       = "Show or hide the debug panel";
BINDING_NAME_COMFYATMOSPHERE_RELOAD      = "Read comfyatmos.ini again";
BINDING_NAME_COMFYATMOSPHERE_RAYS        = "Sun rays on or off";
BINDING_NAME_COMFYATMOSPHERE_VOLUME      = "Volumetric light on or off";
BINDING_NAME_COMFYATMOSPHERE_BENCH       = "Run the benchmark";
BINDING_NAME_COMFYATMOSPHERE_TIME_LATER  = "Time of day later";
BINDING_NAME_COMFYATMOSPHERE_TIME_EARLIER = "Time of day earlier";
BINDING_NAME_COMFYATMOSPHERE_DAYNIGHT    = "Day or night";

-- The debug views, as kDebugViews in comfyatmos's cvars.cpp and the Debug View slider's tooltip above: keep
-- the three in the same order. The first is view 0.
local DEBUG_VIEWS = {
	"off",
	"volumetric light: the glow alone",
	"volumetric light: share of each line in sun",
	"volumetric light: the shadow map",
	"volumetric light: the depth it reads",
	"sun shadows: the shade alone",
	"lamps: the glow alone",
	"lamps: the distance read",
	"lamps: the light on surfaces alone",
	"sun rays: the mask",
	"sun rays: the rays alone",
	"sun rays: the sky kept before the clouds",
	"fog: how much gets through (white = clear)",
	"fog: the sky's light on it alone",
	"fog: where mist collects (low ground, water)",
	"sun shadows: the bodies it finds",
	"water: as drawn, over what lies under it: magenta the far slice, orange untextured draws, cyan terrain past 80 yards",
	"water: the foam alone",
	"water: the wet sand alone",
	"water: what lies under it (red: the far terrain, blue: only sky behind)",
	"water: ripples (red, green) and the wake (blue)",
	"water: the game's own water drawn (red), other liquid (magenta), ours off",
	"water: the height over the water, in contours every 0.1 yards (green above, red below, blue the dry cells)",
	"water: the slope the swash uses (dark to bright up to 0.3, a line every 0.05)",
	"sun shadows: where the shade comes from (red solid, green leaves, blue hills)",
	"sun shadows: shade taken off behind a hill (red solid, green leaves, blue where the check runs)",
	"sun shadows: the hill check's depths (red the solid caster before the hill, green behind it, blue the point behind the hill)",
	"water: the drawn foam (white) over its age (red), the open water's foam (blue)",
	"sun shadows: the water it finds (blue: the water's depth over a bed, grey: none, so the shade falls on what the depth shows)",
	"water: the depth under it (0 to 256 yards, blue to red, a line at 1, 2, 4, 8 ... 256; hatched: the map's depth)",
	"grass: the bend (black still, white the tips; blue where it comes from the texture)",
};

local PANEL_BACKDROP = {
	bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
	edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
	tile = true, tileSize = 16, edgeSize = 16,
	insets = { left = 4, right = 4, top = 4, bottom = 4 },
};

-- A small panel that can be dragged, with a title and a close button.
local function PanelMake(name, title, width, x, y)
	local f = CreateFrame("Frame", name, UIParent);
	f:SetWidth(width);
	f:SetHeight(100);
	f:SetPoint("TOPLEFT", UIParent, "TOPLEFT", x, y);
	f:SetBackdrop(PANEL_BACKDROP);
	f:SetBackdropColor(0, 0, 0, 0.75);
	f:SetBackdropBorderColor(0.6, 0.6, 0.6, 1);
	f:SetFrameStrata("MEDIUM");
	f:EnableMouse(true);
	f:SetMovable(true);
	f:RegisterForDrag("LeftButton");
	f:SetScript("OnDragStart", function() this:StartMoving(); end);
	f:SetScript("OnDragStop", function() this:StopMovingOrSizing(); end);
	local t = f:CreateFontString(nil, "OVERLAY", "GameFontNormal");
	t:SetPoint("TOPLEFT", f, "TOPLEFT", 12, -10);
	t:SetText(title);
	local close = CreateFrame("Button", name .. "Close", f, "UIPanelCloseButton");
	close:SetPoint("TOPRIGHT", f, "TOPRIGHT", 0, 0);
	f:Hide();
	return f;
end

-- ---- the stats panel ------------------------------------------------------------------------------

local STATS_ROWS = 40;
local statsFrame = nil;

local function StatsParse(raw)
	local v = {};
	for k, val in string.gfind(raw, "([^;=]+)=([^;]*)") do
		v[k] = val;
	end
	return v;
end

-- The rows to show: { header } or { label, value, warn }.
local function StatsRows(v)
	local rows = {};
	local function G(k)
		return v[k] or "?";
	end
	local function Head(text)
		table.insert(rows, { text });
	end
	local function Row(label, value, warn)
		table.insert(rows, { label, value, warn });
	end

	Head("Position");
	local zone = GetRealZoneText() or "";
	local sub = GetSubZoneText() or "";
	Row("Zone", (sub ~= "" and sub ~= zone) and (zone .. ", " .. sub) or zone);
	if not (WorldMapFrame and WorldMapFrame:IsVisible()) then
		SetMapToCurrentZone();
	end
	local mx, my = GetPlayerMapPosition("player");
	if mx and my and (mx > 0 or my > 0) then
		Row("Map", string.format("%.1f, %.1f", mx * 100, my * 100));
	end
	if v.pos == "1" then
		Row("World", G("x") .. ", " .. G("y") .. ", " .. G("z") .. ((v.map and v.map ~= "") and ("  (" .. v.map .. ")") or ""));
	end
	-- The DLL's indoor test (MapIndoors): the answer, and the step that decided it.
	Row("Inside building", v["in"] == "1" and "true" or (v["in"] == "0" and "false" or "?"));
	local _, _, building, reason = string.find(G("inwhy"), "^(.-): (.*)$");
	if building then
		Row("Building", building);
		Row("Why", reason);
	else
		Row("Why", G("inwhy"));
	end

	Head("Frame");
	Row("Frame rate", G("fps") .. " fps");

	-- The sun three ways, in degrees (azimuth, elevation): what the sky shows (or the clock, before the sky
	-- is seen), the glided sun that the light, the rays, the water and the fog use, and the held sun the
	-- shadow maps are drawn with.
	Head("Sun");
	Row("Game time", G("gametime"));
	Row("Measured (" .. G("sunsrc") .. ")", G("sunmaz") .. ", " .. G("sunmel"));
	Row("Glided (light, water)", G("sungaz") .. ", " .. G("sungel"));
	if v.sunheld == "1" then
		Row("Held (shadows)", G("sunhaz") .. ", " .. G("sunhel"));
		local ago = tonumber(v.sunstepago) or -1;
		Row("Shadow steps", G("sunsteps") .. " in the last second, the last " ..
			(ago >= 0 and string.format("%.1f s ago", ago) or "never") .. "  (step " .. G("sunstep") .. " deg)",
			(tonumber(v.sunsteps) or 0) > 0);
	else
		Row("Held (shadows)", "not drawn yet");
	end

	Head("Shadow casters");
	Row("Held", G("held") .. "  (" .. G("models") .. " models, " .. G("fixed") .. " buildings)");
	Row("Moving", G("moving"));
	Row("Drawn into the map", G("drawn"));
	Row("Recorded a frame", G("rec") .. " of " .. G("seen") .. " draws");
	Row("Turned away a frame", "depth writes off " .. G("zw") .. ", blended " .. G("blend") .. ", dynamic " .. G("dyn"));

	Head("Last second");
	Row("Added", G("added"));
	Row("Dropped", G("dropped") .. "  (aged " .. G("dage") .. ", in view " .. G("dview") .. ", overwritten " .. G("dover") .. ")");

	Head("Within 40 yards of you");
	Row("Added", G("nadd"));
	Row("Dropped", G("ndrop"), (tonumber(v.ndrop) or 0) > 0);
	if v.nwhy and v.nwhy ~= "" then
		Row("Why", v.nwhy);
	end
	Row("Refused (map files)", G("nref"));

	Head("Fog");
	if v.fog == "1" then
		Row("Density", G("fogd") .. " a yard  (x" .. G("morning") .. " at this hour)");
		local ground = G("ground");
		if (tonumber(v.notile) or 0) > 0 then
			ground = ground .. "  (" .. v.notile .. " cells without a tile)";
		end
		Row("Ground", ground);
		Row("Wet cells", G("wet"));
		Row("Patches", v.patches == "1" and "on" or "off");
	else
		Row("Fog", v.fog and "off" or "?");
	end
	return rows;
end

local function StatsBuild()
	local f = PanelMake("ComfyAtmosphereStats", "Atmosphere stats", 420, 20, -120);
	f.labels, f.values = {}, {};
	for i = 1, STATS_ROWS do
		local label = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall");
		label:SetPoint("TOPLEFT", f, "TOPLEFT", 14, -26 - (i - 1) * 13);
		label:SetJustifyH("LEFT");
		local value = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall");
		value:SetPoint("TOPLEFT", f, "TOPLEFT", 140, -26 - (i - 1) * 13);
		value:SetWidth(270);
		value:SetJustifyH("LEFT");
		f.labels[i], f.values[i] = label, value;
	end
	local wait = 0;
	f:SetScript("OnUpdate", function()
		wait = wait - arg1;
		if wait > 0 then
			return;
		end
		wait = 0.25;
		local raw = string.gsub(GetCVar("comfyStats") or "", "%s+$", "");
		local rows;
		if raw == "" then
			rows = { { "Waiting for comfyatmos.dll (a second)..." } };
		else
			rows = StatsRows(StatsParse(raw));
		end
		local n = table.getn(rows);
		for i = 1, STATS_ROWS do
			local r = rows[i];
			local label, value = this.labels[i], this.values[i];
			if not r or i > n then
				label:SetText("");
				value:SetText("");
			elseif r[2] == nil then
				label:SetText(r[1]);
				label:SetTextColor(1, 0.82, 0);          -- a section: gold
				value:SetText("");
			else
				label:SetText(r[1]);
				label:SetTextColor(0.7, 0.7, 0.7);
				value:SetText(r[2]);
				if r[3] then
					value:SetTextColor(1, 0.5, 0.25);    -- worth a look: orange
				else
					value:SetTextColor(1, 1, 1);
				end
			end
		end
		this:SetHeight(26 + math.min(n, STATS_ROWS) * 13 + 12);
	end);
	return f;
end

function ComfyAtmosphere_StatsToggle()
	if not HasCVar("comfyStats") then
		Say("this comfyatmos.dll has no stats.");
		return;
	end
	if not statsFrame then
		statsFrame = StatsBuild();
	end
	if statsFrame:IsShown() then
		statsFrame:Hide();
	else
		-- The DLL writes into the CVar's own string, so it must hold 1000 characters (kStatsLen in cvars.cpp). The game saves it in
		-- Config.wtf, and at the next start it came back empty, with no room: it is set again here.
		SetCVar("comfyStats", string.rep(" ", 1000));
		statsFrame:Show();
	end
end

-- ---- the time of day (2026-10-07, from comfytime) --------------------------------------------------
--
-- The addon sends "<number> <verb> [value]" in comfyTimeSet: set <hour>, step <steps>, daynight, lock <0 or 1>. A new
-- number each time, so the same command twice runs twice; it starts from the clock, so a /reload does not repeat
-- the last number. comfyatmos.dll reads it every frame. It writes "<hour> <state>" into comfyTimeShown in place,
-- which needs the CVar to hold 16 characters, as comfyStats does. State: 0 the server's time ([time] enabled 0),
-- 1 ours, 2 an old comfytime.dll sets it, 3 the addresses did not check out.

local TIME_LEN = 16;
local timeNumber = math.floor(GetTime() * 1000);

local function TimeSend(verb, value)
	if not HasCVar("comfyTimeSet") then
		Say("this comfyatmos.dll does not set the time of day.");
		return;
	end
	timeNumber = timeNumber + 1;
	SetCVar("comfyTimeSet", timeNumber .. " " .. verb .. (value and (" " .. value) or ""));
end

-- The hour shown and the state, or nil while the DLL has not written them yet.
local function TimeShown()
	if not HasCVar("comfyTimeShown") then
		return nil;
	end
	local raw = GetCVar("comfyTimeShown") or "";
	if string.len(raw) ~= TIME_LEN then
		-- The game saved it in Config.wtf, and it came back at another length, with no room.
		SetCVar("comfyTimeShown", string.rep(" ", TIME_LEN));
		return nil;
	end
	local _, _, hour, state = string.find(raw, "^%s*([%d%.]+)%s+(%d)");
	if not hour then
		return nil;
	end
	return tonumber(hour), tonumber(state);
end

local function TimeText(hour)
	local minutes = math.floor(hour * 60 + 0.5);
	return string.format("%02d:%02d", math.mod(math.floor(minutes / 60), 24), math.mod(minutes, 60));
end

-- A held key steps the time as Ctrl+PageUp does: one step at once, then after 0.35 s one every 0.03 s. With
-- runOnUp the binding gets "up" when the key is let go. A release can be lost (a window over the game), so the
-- repeat also ends after 15 s.
local timeHeld, timeHeldSince, timeLastStep = 0, 0, 0;
local timeRepeat = CreateFrame("Frame");
timeRepeat:Hide();
timeRepeat:SetScript("OnUpdate", function()
	local now = GetTime();
	if timeHeld == 0 or now - timeHeldSince > 15 then
		timeHeld = 0;
		this:Hide();
		return;
	end
	if now - timeHeldSince > 0.35 and now - timeLastStep >= 0.03 then
		TimeSend("step", timeHeld);
		timeLastStep = now;
	end
end);

function ComfyAtmosphere_TimeKey(direction, state)
	if state == "up" then
		if timeHeld == direction then
			timeHeld = 0;
		end
		return;
	end
	TimeSend("step", direction);
	if state == "down" then
		timeHeld, timeHeldSince, timeLastStep = direction, GetTime(), GetTime();
		timeRepeat:Show();
	end
end

function ComfyAtmosphere_DayNight()
	TimeSend("daynight");
end


function ComfyAtmosphere_Command(command)
	SlashCmdList["COMFYATMOS"](command);
end

-- ---- the debug panel ------------------------------------------------------------------------------

local debugFrame = nil;
local traceOn = false;

function ComfyAtmosphere_Probe()
	SlashCmdList["COMFYATMOS"]("probe");
end

local function DebugButton(f, name, text, width, x, y, onClick)
	local b = CreateFrame("Button", "ComfyAtmosphereDebug" .. name, f, "UIPanelButtonTemplate");
	b:SetWidth(width);
	b:SetHeight(22);
	b:SetPoint("TOPLEFT", f, "TOPLEFT", x, y);
	b:SetText(text);
	b:SetScript("OnClick", onClick);
	return b;
end

local function DebugViewNow()
	local v = tonumber(GetCVar("comfyDebugView") or "0") or 0;
	return math.max(0, math.min(v, table.getn(DEBUG_VIEWS) - 1));
end

local function DebugRefresh()
	if not debugFrame then
		return;
	end
	local v = DebugViewNow();
	debugFrame.view:SetText("View " .. v .. ": " .. DEBUG_VIEWS[v + 1]);
	ComfyAtmosphereDebugTrace:SetText(traceOn and "Trace: on" or "Trace: off");

	local hour, state = TimeShown();
	local slider = ComfyAtmosphereDebugTime;
	if not hour then
		debugFrame.timeText:SetText("Time of day: waiting for comfyatmos.dll");
	elseif state == 1 then
		debugFrame.timeText:SetText("Time of day: " .. TimeText(hour));
	elseif state == 0 then
		debugFrame.timeText:SetText("Time of day: the server's. Tick Lock time to set it");
	elseif state == 2 then
		debugFrame.timeText:SetText("Time of day: comfytime.dll sets it. Remove it");
	else
		debugFrame.timeText:SetText("Time of day: not set (another WoW.exe? See the log)");
	end
	if hour and not slider.dragging then
		slider.refreshing = true;
		slider:SetValue(hour);
		slider.refreshing = false;
	end
	-- Unlocked, the slider and the buttons do nothing, so they are dimmed.
	local ours = state == 1;
	slider:EnableMouse(ours);
	slider:SetAlpha(ours and 1 or 0.4);
	ComfyAtmosphereDebugTimeOn:SetChecked(state ~= 0);
	if HasCVar("comfyHotkeys") then
		ComfyAtmosphereDebugHotkeys:SetChecked(GetCVar("comfyHotkeys") == "1");
	end
end

local function DebugViewStep(step)
	local count = table.getn(DEBUG_VIEWS);
	SetCVar("comfyDebugView", math.mod(DebugViewNow() + step + count, count));
	DebugRefresh();
end

local function DebugBuild()
	local f = PanelMake("ComfyAtmosphereDebug", "Atmosphere debug", 300, 400, -120);
	DebugButton(f, "Probe", "Probe", 132, 12, -30, function() ComfyAtmosphere_Probe(); end);
	DebugButton(f, "Stats", "Stats", 132, 152, -30, function() ComfyAtmosphere_StatsToggle(); end);
	DebugButton(f, "Bench", "Benchmark", 132, 12, -56, function() SlashCmdList["COMFYATMOS"]("bench"); end);
	DebugButton(f, "Trace", "Trace: off", 132, 152, -56, function()
		-- The next probe also traces 180 frames, line by line. The game runs slowly while it does.
		traceOn = not traceOn;
		SlashCmdList["COMFYATMOS"]("general.trace " .. (traceOn and "1" or "0"));
		DebugRefresh();
	end);
	local heading = f:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall");
	heading:SetPoint("TOPLEFT", f, "TOPLEFT", 14, -88);
	heading:SetText("Debug view");
	DebugButton(f, "Prev", "<", 30, 12, -102, function() DebugViewStep(-1); end);
	DebugButton(f, "Next", ">", 30, 254, -102, function() DebugViewStep(1); end);
	local view = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall");
	view:SetPoint("TOPLEFT", f, "TOPLEFT", 48, -102);
	view:SetWidth(200);
	view:SetHeight(22);
	view:SetJustifyH("CENTER");
	f.view = view;

	-- The time of day: a slider in quarter hours, a step either way ([time] step), and Day/Night from comfytime's
	-- keys. The slider follows the hour the keys and the bindings move.
	local timeText = f:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall");
	timeText:SetPoint("TOPLEFT", f, "TOPLEFT", 14, -134);
	f.timeText = timeText;
	local slider = CreateFrame("Slider", "ComfyAtmosphereDebugTime", f, "OptionsSliderTemplate");
	slider:SetPoint("TOPLEFT", f, "TOPLEFT", 16, -154);
	slider:SetWidth(268);
	slider:SetMinMaxValues(0, 24);
	slider:SetValueStep(0.25);
	ComfyAtmosphereDebugTimeLow:SetText("0");
	ComfyAtmosphereDebugTimeHigh:SetText("24");
	ComfyAtmosphereDebugTimeText:SetText("");
	slider:SetScript("OnMouseDown", function() this.dragging = true; end);
	slider:SetScript("OnMouseUp", function() this.dragging = false; end);
	slider:SetScript("OnValueChanged", function()
		if this.refreshing then
			return;
		end
		TimeSend("set", string.format("%.4f", this:GetValue()));
	end);
	DebugButton(f, "Earlier", "<", 30, 12, -180, function() TimeSend("step", -1); end);
	DebugButton(f, "Later", ">", 30, 46, -180, function() TimeSend("step", 1); end);
	DebugButton(f, "DayNight", "Day/Night", 100, 82, -180, function() ComfyAtmosphere_DayNight(); end);

	-- Lock time: ticked, the hour stays where it is put, and the DLL keeps it in comfyatmos.ini ([time] enabled
	-- and hour), so the next start shows it. Unticked, the server's time shows, and that is kept too.
	local timeOn = CreateFrame("CheckButton", "ComfyAtmosphereDebugTimeOn", f, "UICheckButtonTemplate");
	timeOn:SetPoint("TOPLEFT", f, "TOPLEFT", 10, -206);
	timeOn:SetWidth(24);
	timeOn:SetHeight(24);
	ComfyAtmosphereDebugTimeOnText:SetText("Lock time");
	timeOn:SetScript("OnClick", function()
		TimeSend("lock", this:GetChecked() and "1" or "0");
	end);

	-- comfyatmos.dll's own keys, read past the game's key bindings ([general] hotkeys). A control CVar, so the
	-- game keeps it in Config.wtf.
	local hotkeys = CreateFrame("CheckButton", "ComfyAtmosphereDebugHotkeys", f, "UICheckButtonTemplate");
	hotkeys:SetPoint("TOPLEFT", f, "TOPLEFT", 10, -230);
	hotkeys:SetWidth(24);
	hotkeys:SetHeight(24);
	ComfyAtmosphereDebugHotkeysText:SetText("Developer keys (F11, F12, Ctrl+PageUp)");
	hotkeys:SetScript("OnClick", function()
		if not HasCVar("comfyHotkeys") then
			Say("this comfyatmos.dll has no Developer keys switch: its keys are always on.");
			return;
		end
		SetCVar("comfyHotkeys", this:GetChecked() and "1" or "0");
	end);
	f:SetHeight(266);

	local wait = 0;
	f:SetScript("OnUpdate", function()
		wait = wait - arg1;
		if wait > 0 then
			return;
		end
		wait = 0.1;
		DebugRefresh();
	end);
	return f;
end

function ComfyAtmosphere_DebugToggle()
	if not DllLoaded() then
		Say("comfyatmos.dll is not loaded.");
		return;
	end
	if not debugFrame then
		debugFrame = DebugBuild();
	end
	if debugFrame:IsShown() then
		debugFrame:Hide();
	else
		TimeShown();
		debugFrame:Show();
		DebugRefresh();
	end
end

SlashCmdList["COMFYATMOS"] = function(msg)
	local command = string.lower((string.gsub(msg or "", "^%s*(.-)%s*$", "%1")));
	if command == "options" then
		WindowToggle();
		return;
	end
	if command == "stats" then
		ComfyAtmosphere_StatsToggle();
		return;
	end
	if command == "debug" then
		ComfyAtmosphere_DebugToggle();
		return;
	end
	if command == "" then
		Say("/atmos options: the settings window. /atmos debug: buttons for finding faults. /atmos stats: figures on screen.");
	end
	if not HasCVar("comfyTune") then
		Say("this comfyatmos.dll has no /atmos. It needs the version from 2026-09-29 or later.");
		return;
	end
	if tuneWaiting then
		table.insert(tuneQueue, msg or "");
		return;
	end
	TuneSend(msg);
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
		if HasCVar("comfyTimeSet") then
			SetCVar("comfyTimeSet", "");
		end
		return;
	end
	if not AddControls() then
		DEFAULT_CHAT_FRAME:AddMessage(
			"|cff88cc88comfyatmosphere|r: this client's options panel has no page for the settings. "
			.. "Type /atmos options to open them.");
	end
	syncFrame:Show();
end);
