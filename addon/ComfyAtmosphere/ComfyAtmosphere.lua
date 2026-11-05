-- Adds the comfyatmosphere controls to the options window, on a page of their own: Video -> Atmosphere. The
-- clouds' controls are on it too (2026-10-11, the owner); from 2026-10-10 they had a Clouds page under it.
--
-- This client's options panel is data-driven, as the ComfyGrass addon describes: GameOptions is a global
-- table, and OptionsFrame.lua builds each page from it (both in patch-9.mpq). An entry with `options` is
-- a page in the list on the left, and one without is a heading (Video, Sound, Interface). So each page is
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

-- ---- LANGUAGE: English stays and, on a Chinese client (zhCN/zhTW), Chinese is shown as well. ----
-- The addon keeps the English text as the key and shows a translation where there is one, so a client in
-- any other language still gets English. There is no GetLocaleValue in classic 1.12: the game window's
-- locale is read, and the matching list is kept in this file, under the `local` table below.
local COMFYATMOSPHERE_LOCALE = GetLocale();
local COMFYATMOSPHERE_ZH = {
	["Atmosphere"] = "大气环境",
	["Atmosphere Effects"] = "大气环境特效",
	["Volumetric Light"] = "体积光",
	["Volumetric Light Strength"] = "体积光强度",
	["Volumetric Light Quality"] = "体积光质量",
	["Light Density"] = "光密度",
	["Light Distance"] = "光距离",
	["Light Toward the Sun"] = "朝向太阳的光",
	["Lamps"] = "灯光",
	["Lamp Glow"] = "灯光辉光",
	["Lamp Distance"] = "灯光距离",
	["Lantern Light"] = "灯笼灯光",
	["Torch Light"] = "火把灯光",
	["Indoor Lamps"] = "室内灯光",
	["Lamps by Day"] = "白昼灯光",
	["Sun Shadows"] = "太阳阴影",
	["Sun Shadow Strength"] = "太阳阴影强度",
	["Night Shadows"] = "夜间阴影",
	["Character Shadow Strength"] = "角色阴影强度",
	["Character Backside Shadow"] = "角色背面阴影",
	["Shadow on Water"] = "水面阴影",
	["Sun Smoothing"] = "太阳平滑",
	["World / Object Shadows"] = "世界 / 物体阴影",
	["Player / Creature Shadows"] = "玩家 / 生物阴影",
	["Lock Shadow Angle"] = "锁定阴影角度",
	["Shadow Angle"] = "阴影角度",
	["Low Sun From"] = "低太阳起点",
	["Highest Sun"] = "太阳最高点",
	["Sunlight"] = "阳光",
	["Tree Shadow Strength"] = "树影强度",
	["Foliage Shade Size"] = "植被阴影大小",
	["Foliage Shade Edge"] = "植被阴影边缘",
	["Foliage Self Shade"] = "植被自阴影",
	["Creature Shadow on Foliage"] = "植被上的生物阴影",
	["Water Effects"] = "水体特效",
	["Sunset Water"] = "日落水面",
	["Water Saturation"] = "水饱和度",
	["Water Clarity"] = "水清澈度",
	["Water Brightness"] = "水亮度",
	["Sky Reflection"] = "天空倒影",
	["Underwater Distortion"] = "水下扭曲",
	["Underwater Cover"] = "水下遮盖",
	["Wake"] = "尾流",
	["Standing Ripple Depth"] = "站立涟漪深度",
	["Moving Ripple Depth"] = "移动涟漪深度",
	["Standing Ripple Spread"] = "站立涟漪扩散",
	["Moving Ripple Spread"] = "移动涟漪扩散",
	["Jump Ripple"] = "跳跃涟漪",
	["Game Glow"] = "游戏辉光",
	["Foam"] = "泡沫",
	["Drawn Foam"] = "绘制泡沫",
	["Foam Size"] = "泡沫大小",
	["Foam Reach"] = "泡沫范围",
	["Foam Breakup"] = "泡沫破碎",
	["Wake Foam"] = "尾流泡沫",
	["Ship Wake"] = "船尾流",
	["Ship Wake Forward"] = "船尾流前向",
	["Ship Wake Depth"] = "船尾流深度",
	["Object Foam"] = "物体泡沫",
	["Lake Swash"] = "湖泊拍岸",
	["Lake Foam"] = "湖泊泡沫",
	["Lake Waves"] = "湖泊波浪",
	["Rain on Water"] = "水中降雨",
	["Lighthouses"] = "灯塔",
	["Lighthouse Beam"] = "灯塔光束",
	["Lighthouse Beacon"] = "灯塔信号灯",
	["Beam Length"] = "光束长度",
	["Lighthouse Distance"] = "灯塔距离",
	["Beam Width"] = "光束宽度",
	["Beam Spread"] = "光束扩散",
	["Beam Speed"] = "光束速度",
	["Two Beams"] = "双光束",
	["Lighthouse Glint"] = "灯塔闪光",
	["Wave Face Strength"] = "波面强度",
	["Wave Face Tilt"] = "波面倾斜",
	["Wave Face Softness"] = "波面柔和度",
	["Beam Width on Water"] = "水面光束宽度",
	["Object Foam Width"] = "物体泡沫宽度",
	["Open Water Foam"] = "开阔水域泡沫",
	["Open Water Foam Amount"] = "开阔水域泡沫量",
	["Swash"] = "拍岸浪",
	["Swash Height"] = "拍岸浪高度",
	["Swash Length"] = "拍岸浪长度",
	["Swash Speed"] = "拍岸浪速度",
	["Wet Sand"] = "湿沙",
	["Wet Sand Distance"] = "湿沙距离",
	["Sun Glint"] = "太阳闪光",
	["Moon Glint"] = "月光反光",
	["Glint Size"] = "闪光大小",
	["Edge Line"] = "边缘线",
	["Edge Line Width"] = "边缘线宽度",
	["Wave Height"] = "浪高",
	["Wave Size"] = "浪大小",
	["Shade Colour"] = "阴影颜色",
	["Sunlight Warmth"] = "阳光暖色",
	["Shadow Resolution"] = "阴影分辨率",
	["Shadow Softness"] = "阴影柔和度",
	["Shadow Redraw"] = "阴影重绘",
	["Near Shadow Distance"] = "近处阴影距离",
	["Sun Rays"] = "太阳光线",
	["Sun Rays Strength"] = "太阳光线强度",
	["Sun Rays Softness"] = "太阳光线柔和度",
	["Sun Rays Smoothing"] = "太阳光线平滑",
	["Debug View"] = "调试视图",
	["Night Strength"] = "夜间强度",
	["Night Darkness"] = "夜间黑暗度",
	["Rain Darkness"] = "降雨黑暗度",
	["Moonlight Colour"] = "月光颜色",
	["Color Effects"] = "色彩特效",
	["Day Saturation"] = "白昼饱和度",
	["Night Saturation"] = "夜间饱和度",
	["Clouds"] = "云层",
	["Cloud Cover"] = "云量",
	["Cloud Shapes"] = "云朵形状",
	["Cloud Shape Amount"] = "云形数量",
	["Cloud Softness"] = "云层柔和度",
	["Cloud Shape Billows"] = "云形起伏",
	["Cloud Shape Cutout"] = "云形镂空",
	["Cloud Shape Cutout Width"] = "云形镂空宽度",
	["Cloud Shape Size"] = "云形大小",
	["Cloud Reshaping"] = "云层重塑",
	["Cloud Puffiness"] = "云的卷积感",
	["Cloud Speed"] = "云层速度",
	["Cloud Transition Time"] = "云层过渡时间",
	["Cloud Scale"] = "云层规模",
	["Cloud Layer Softness"] = "云层细节柔和度",
	["Fog"] = "雾气",
	["Fog Density"] = "雾气密度",
	["Fog Height"] = "雾气高度",
	["Fog Brightness"] = "雾气亮度",
	["Fog Sunlight"] = "雾气阳光",
	["Fog Toward the Sun"] = "朝太阳方向雾气",
	["Fog Horizon Glow"] = "雾地平线辉光",
	["Fog Reach"] = "雾气范围",
	["Fog on Sky"] = "天空雾气",
	["Fog Patchiness"] = "雾气斑驳",
	["Low Ground Mist"] = "低洼地薄雾",
	["Water Mist"] = "水面薄雾",
	["Morning Mist"] = "晨雾",
	["Lamps in Mist"] = "雾中灯光",
	["Wind Speed"] = "风速",
	["Wind Direction"] = "风向",
	["Grass"] = "草地",
	["Grass Wind"] = "草地风",
	["Grass Wave Speed"] = "草浪速度",
	["Grass Wave Length"] = "草浪长度",
	["Grass Wind Direction"] = "草地风向",
	["Grass Lean"] = "草地倾斜",
	["Grass Parting"] = "草地开合",
	["Grass Parting Radius"] = "草地开合半径",
	["Foliage Density"] = "植被密度",
	-- Time's three choices.
	["Server"] = "服务器",
	["Server time"] = "服务器时间",
	["The time of day the server sends. All players see the same sky."] = "服务器发送的时刻。所有玩家看到同一片天空。",
	["Computer"] = "电脑",
	["Computer time"] = "电脑时间",
	["The computer's clock, with its timezone and daylight saving. Only your client shows it."] = "电脑本地时钟，含时区和夏时制。仅你的客户端显示。",
	["Fixed"] = "固定",
	["Fixed hour"] = "固定时刻",
	["The hour set with the slider below. It stays, also after a restart. Only your client shows it."] = "用下方滑块设置的时刻。重启后也保持不变。仅你的客户端显示。",
	["Hour"] = "小时",
	["Day/Night"] = "白天/黑夜",
	["Waiting for comfyatmos.dll"] = "正在等待 comfyatmos.dll",
	["Waiting for comfyatmos.dll (a second)..."] = "正在等待 comfyatmos.dll（一秒）……",
	["Server time: "] = "服务器时间：",
	["Fixed hour: "] = "固定时刻：",
	["Computer time: "] = "电脑时间：",
	["comfytime.dll sets the time. Remove it"] = "comfytime.dll 也在设置时间，请移除它",
	["Not set (another WoW.exe?). See the log"] = "未设置（另一个 WoW.exe？）。请查看日志",
	["Light"] = "光",
	["Lamps"] = "灯光",
	["Shadows"] = "阴影",
	["Sky"] = "天空",
	["Color"] = "色彩",
	["Water"] = "水",
	["Time"] = "时间",
	-- The /atmos window's value words, buttons and the key bindings.
	["Low"] = "低",
	["Medium"] = "中",
	["High"] = "高",
	["Defaults"] = "默认",
	["Close"] = "关闭",
	["Debug"] = "调试",
	["ComfyAtmosphere"] = "大气环境",
	["Probe (log one frame)"] = "探查（记录一帧）",
	["Show or hide the stats"] = "显示或隐藏统计",
	["Show or hide the debug panel"] = "显示或隐藏调试面板",
	["Read comfyatmos.ini again"] = "重新读取 comfyatmos.ini",
	["Sun rays on or off"] = "开启或关闭太阳光线",
	["Volumetric light on or off"] = "开启或关闭体积光",
	["Run the benchmark"] = "运行基准测试",
	["Time of day later"] = "时间往后",
	["Time of day earlier"] = "时间往前",
	["Day or night"] = "白天或夜晚",
	["Atmosphere stats"] = "大气环境统计",
	["Atmosphere debug"] = "大气环境调试",
	["Debug view"] = "调试视图",
	["Options"] = "选项",
	["Click: settings"] = "点击：设置",
	["Right-click: options, debug, stats, probe"] = "右键：选项、调试、统计、探查",
	["Drag: move"] = "拖动：移动",
	["Trace: on"] = "追踪：开启",
	["Trace: off"] = "追踪：关闭",
	["Trace"] = "追踪",
	["Benchmark"] = "基准测试",
	["Developer keys (F11, F12, Ctrl+PageUp)"] = "开发按键（F11、F12、Ctrl+PageUp）",
	["View "] = "视图 ",
	["Probe"] = "探查",
	["Stats"] = "统计",
	["Bench"] = "基准",
	["Stats view"] = "统计视图",
	["Foliage Density"] = "植被密度",
	["Position"] = "位置",
	["Zone"] = "区域",
	["Map"] = "地图",
	["World"] = "世界",
	["Inside building"] = "在建筑内",
	["Building"] = "建筑",
	["Why"] = "原因",
	["Frame"] = "帧",
	["Frame rate"] = "帧率",
	["Game time"] = "游戏时间",
	["Glided (light, water)"] = "平滑（光、水）",
	["Held (shadows)"] = "固定（阴影）",
	["Shadow steps"] = "阴影步数",
	["Shadow casters"] = "阴影投射体",
	["Held"] = "固定",
	["Moving"] = "移动",
	["Drawn into the map"] = "绘制进贴图",
	["Recorded a frame"] = "已记录一帧",
	["Turned away a frame"] = "拒录一帧",
	["Last second"] = "上一秒",
	["Added"] = "新增",
	["Dropped"] = "丢弃",
	["Within 40 yards of you"] = "距你 40 码内",
	["Refused (map files)"] = "拒绝（地图文件）",
	["Density"] = "密度",
	["Ground"] = "地面",
	["Wet cells"] = "湿润格",
	["Patches"] = "块状",
	["this comfyatmos.dll does not set the time of day."] = "此 comfyatmos.dll 不能设置游戏内时刻。",
	["no answer from comfyatmos.dll."] = "comfyatmos.dll 无应答。",
	["this comfyatmos.dll has no stats."] = "此 comfyatmos.dll 没有统计功能。",
	["this comfyatmos.dll has no Developer keys switch: its keys are always on."] = "此 comfyatmos.dll 没有“开发按键”开关：其按键始终开启。",
	["comfyatmos.dll is not loaded."] = "comfyatmos.dll 尚未加载。",
	["/atmos: the settings window. /atmos debug: buttons for finding faults. /atmos stats: figures on screen."] = "/atmos：设置窗口。/atmos debug：排障按钮。/atmos stats：屏幕上的数据。",
	["this comfyatmos.dll has no /atmos. It needs the version from 2026-09-29 or later."] = "此 comfyatmos.dll 不支持 /atmos。需要 2026-09-29 之后（含）的版本。",
	-- The options' descriptions. English stays, and a Chinese client shows these.
	["All the effects on this page at once. Off, the game looks as it does without the mod."] = "本页所有特效的集中开关。关闭后，游戏看起来就像没装这个模组一样。",
	["Fog glows where sunlight reaches it and stays dark where leaves and walls shade it."] = "阳光照到的雾气会发光，被树叶和墙壁遮挡的地方保持黑暗。",
	["How bright the lit fog is."] = "被照亮的雾气的亮度。",
	["Lower runs faster."] = "数值越低运行越快。",
	["How thick the air is. Higher gives brighter shafts through the trees."] = "空气的稠密程度。数值越高，穿过树林的光柱越明亮。",
	["How far away, in yards, the light still glows. Shorter makes the shafts near you sharper."] = "光线仍在发光的距离（码）。数值越短，靠近你的光柱越清晰。",
	["0 glows the same from every side. Higher glows more when you look toward the sun."] = "0 表示从各方向发光相同。数值越高，望向太阳时发光越强。",
	["Fog that lies on the ground and thins higher up. The sun lights it. With Volumetric Light on, it shows shafts where trees and walls shade it."] = "贴着地面并随高度变薄的雾气，被阳光照亮。开启体积光时，被树木和墙壁遮挡处会显现出光柱。",
	["How thick the fog is at the ground. Higher, you see less far."] = "地面处雾气的密度。数值越高，看得越近。",
	["How deep the fog layer is, in yards. Lower keeps it on the ground; higher fills the air."] = "雾气层的厚度（码）。数值越低雾气越贴地，数值越高雾气填满空气。",
	["How bright the fog is in the shade. 100 is the game's own fog colour."] = "阴影中雾气的亮度。100 为游戏自带的雾气颜色。",
	["How brightly the sun lights the fog."] = "阳光照亮雾气的强度。",
	["How much brighter the fog glows looking toward the sun, and darker looking away. 0 is the same from every side."] = "望向太阳时雾气更亮、背向太阳时更暗的程度。0 表示各方向相同。",
	["With the sun low, how brightly the fog glows along the horizon in the sun's colour, brightest under the sun. 0 is none."] = "太阳低垂时，雾气沿地平线以太阳颜色发光、在太阳正下方最亮的程度。0 为没有。",
	["How far away, in yards, the fog still gathers. Lower lets you see further across open land and sea."] = "雾气仍能聚集的距离（码）。数值越低，在开阔陆地和海面上看得越远。",
	["How much the fog covers the sky, in yards of fog. 0 leaves the sky clear; higher hides the horizon."] = "雾气遮盖天空的程度（码的雾气）。0 保持天空清澈，数值越高越能遮蔽地平线。",
	["0 is an even fog. Higher breaks it into patches with clear air between them."] = "0 为均匀的雾气。数值越高，雾气破碎成块，块与块之间空气清澈。",
	["Extra mist in valleys and hollows. 0 is none."] = "山谷和洼地中的额外薄雾。0 为没有。",
	["Extra mist over rivers, lakes and the sea. 0 is none."] = "河流、湖泊和海洋上方的额外薄雾。0 为没有。",
	["Extra mist at dawn, and half as much at dusk. 0 is none."] = "黎明时的额外薄雾，黄昏时为其一半。0 为没有。",
	["How much brighter lamps glow in thick mist. 0 is no change."] = "浓雾中灯光发光的增亮程度。0 为不变。",
	["How fast the wind carries the fog patches."] = "风携带雾气块的速度。",
	["The way the wind blows, in degrees: 0 north, 90 east, 180 south, 270 west."] = "风向（度）：0 北，90 东，180 南，270 西。",
	["Lamps, candles and torches glow in the air and light the walls and ground near them. Needs Volumetric Light on."] = "灯火、蜡烛和火把在空气中发光，并照亮周围的墙壁和地面。需要开启体积光。",
	["How strongly lamps, candles and torches glow and light the walls and ground near them."] = "灯火、蜡烛和火把发光并照亮周围墙壁和地面的强度。",
	["How far away lamps still glow. At 100 they fade into the fog as the game's own lamp glows do."] = "灯光仍在发光的距离。100 时灯光像游戏自身的灯辉光一样在雾中淡出。",
	["How brightly lampposts, lanterns, candles and chandeliers glow and light the ground and walls near them."] = "路灯、灯笼、蜡烛和吊灯发光并照亮周围地面和墙壁的亮度。",
	["How brightly torches, braziers, campfires and the torches NPCs carry glow and light the ground and walls near them. The game lights characters near them already, so at 100 they are lit twice."] = "火把、火盆、篝火和 NPC 手持火把发光并照亮周围地面和墙壁的亮度。游戏本身已照亮附近角色，因此 100 时会被照亮两次。",
	["How bright lamps, candles and fires are while you are inside a building. A room holds many lights close together."] = "你在建筑内部时，灯火、蜡烛和火焰的亮度。一间房间往往聚集许多光源。",
	["How bright lamps, candles and torches are by day, as a share of their night brightness. 0 puts them out by day. Inside buildings, Indoor Lamps applies instead."] = "白昼灯火、蜡烛和火把的亮度（相对夜间亮度的比例）。0 表示白天熄灭。在建筑内部则改用“室内灯光”。",
	["Trees, buildings and characters shade the ground and each other. Replaces the round shadow under characters. Needs Volumetric Light on."] = "树木、建筑和角色相互投下影子并遮蔽地面。取代角色脚下的圆形阴影。需要开启体积光。",
	["Terrain, buildings, trees and doodads cast shadows. Off, the ground keeps the game's own shadows."] = "地形、建筑、树木和装饰物投下阴影。关闭后，地面保留游戏自带的阴影。",
	["Players and creatures cast shadows. Off, they get the game's round shadow instead."] = "玩家和生物投下阴影。关闭后，它们改用游戏的圆形阴影。",
	["Shadows keep one angle all day, as the game's own shadows do, instead of following the sun."] = "阴影全天保持同一角度（如同游戏自带阴影），而不追随太阳。",
	["With Lock Shadow Angle: how far from straight down the light comes, in degrees. Lower gives shorter shadows."] = "在“锁定阴影角度”下：光线偏离正下方的角度（度）。数值越低，阴影越短。",
	["With the sun or moon lower than this, in degrees, the shadows' light climbs back up as it sinks, over the next 12 degrees down, so the shadows stay short. 0 is never."] = "当太阳或月亮低至该角度（度）以下时，阴影的光会随其继续下沉而在随后 12 度内回升，使阴影保持较短。0 表示从不。",
	["The highest the shadows' light stands, in degrees: at noon, and at the end of the climb under Low Sun From. Lower gives longer shadows; 90 is straight down."] = "阴影的光所站的最高角度（度）：正午时，以及“低太阳起点”回升结束时。数值越低阴影越长，90 为正下方。",
	["How dark the shadows are."] = "阴影的深暗程度。",
	["The shadows the moon casts at night. 100 is as dark as by day. 0 is off."] = "夜间月亮投下的阴影。100 与白天一样深。0 为关闭。",
	["Makes the shadows of players and creatures darker than the world's, also inside the shade of a building. 0 makes them as dark as the world's."] = "使玩家和生物的阴影比世界的阴影更深，包括在建筑阴影内。0 使其与世界的阴影一样深。",
	["How dark the shade is on the body of a player or creature, on its side away from the sun. 100 is the darkest. 0 is off: the body keeps only the game's own lighting. The shadow it casts does not change."] = "玩家或生物身体背向太阳一侧阴影的深浅。100 为最深。0 为关闭：身体仅保留游戏自身光照。其投下的阴影不变。",
	["How dark a shadow is on the water's surface, against one on the ground, with a soft edge. The bed under clear water keeps its own shadow. 0 is none on the surface."] = "影子落在地面与落在水面时，水面影子的深暗程度，带有柔和边缘。清澈水面下的水底保留自己的阴影。0 为水面上没有。",
	["Seconds at least that the shadows, sun rays and light take to follow a step of the sun. The sky moves the sun once a game minute, and each move is spread over the minute. 0 follows at once."] = "阴影、太阳光线和光追随太阳每步移动至少所需的秒数。天空每一游戏分钟移动一次太阳，每次移动分摊到这一分钟内。0 表示立即跟随。",
	["How dark the shadows of trees and bushes are. They still show inside the shadow of a hill or a mountain."] = "树木和灌木阴影的深浅。它们仍会显示在山丘或大山的阴影中。",
	["The ground's grass and ferns take the shade of the ground round them, this many tenths of a yard across, so a plant is dark under cover and lit in the open, with no dapples on it. 0 lets the shade fall on them as it falls."] = "地面草地和蕨类会按此宽度（十分之一码）取周围地面的阴影，使植物在遮蔽下变暗、在开阔处受光，不出现斑驳。0 使阴影如常落在它们身上。",
	["How softly a plant goes from lit to dark at the edge of a shadow. Low switches it at once; high blends it over a wider edge."] = "植物在阴影边缘由亮到暗的过渡柔和度。低值立即切换，高值在更宽的边缘上渐变。",
	["The ground's grass and ferns take no shade from leaves nearer than this many tenths of a yard: their own and their neighbours'. Higher stops a tall fern shading itself; a tree's crown higher up still shades it."] = "地面草地和蕨类不从距离此值（十分之一码）以内的叶片取阴影，包括自身和相邻的叶片。数值越高越能阻止高大的蕨类自遮；更高处的树冠仍会遮住它。",
	["How much of the shadows of characters and creatures falls on the ground's grass and ferns. 0 is none, so a passer-by does not print its shape on them."] = "角色和生物的阴影落在草地与蕨类上的比例。0 为没有，这样路人不会在草地上印出身影。",
	["The water's colour, waves and sun glint, the foam and wet sand at the shore, and ripples round anyone in the water. Off, the game draws its own water."] = "水体的颜色、波浪和太阳闪光，岸边的泡沫与湿沙，以及水中人物周围的涟漪。关闭后，游戏绘制自带的水面。",
	["How much more of the sky's red glow the water reflects with the sun low. 0 is none."] = "太阳低垂时水面额外反射天空中红光的程度。0 为没有。",
	["How strong the water's colour is. The water takes the sky's colour for the place and the time of day. 0 is grey, 100 the sky's own colour."] = "水色的浓度。水体会取当地与当前时刻天空的颜色。0 为灰色，100 为天空本色。",
	["How light or dark the water is. The glint and the foam keep their brightness."] = "水体的明暗。闪光和泡沫保持它们自身的亮度。",
	["How far you see into the water. 200 is twice as far as 100."] = "你能看清水下多深。200 是 100 的两倍。",
	["How much of the sky the water shows when you look across it. 0 shows only the water's own colour."] = "平视水面时水面倒映天空的程度。0 只显示水自身的颜色。",
	["How much the waves bend what you see under the water. 0 is off."] = "波浪扭曲你所见水下的程度。0 为关闭。",
	["How white the foam is: at the shore, on the ripples and in the wake. Lower lets the water show through it. 0 is off."] = "泡沫的洁白程度：岸边、涟漪和尾流上的泡沫。数值越低越透出水色。0 为关闭。",
	["Foam in clean, soft shapes that break up as they age. Off, the game's own foam texture."] = "用干净柔和的形状绘制泡沫，随老化而碎裂。关闭后使用游戏自带的泡沫纹理。",
	["How large the shapes of the foam are, in tenths of a yard."] = "泡沫形状的大小（十分之一码）。",
	["How far out from the water's edge the foam lasts, in tenths of a yard."] = "泡沫自水边向外持续多远（十分之一码）。",
	["How broken the foam is at the water's edge. Low makes a solid band of foam along the sand; high leaves it in pieces, as farther out."] = "水边泡沫破碎的程度。低值在沙滩边形成一条完整的泡沫带，高值使其碎块化，如同更远处。",
	["The splash of foam at anyone walking or swimming through the water. 0 is none."] = "有人在水中行走或游泳时溅起的泡沫。0 为没有。",
	["The wake behind a ship under way, as a swimmer's wake this many times the size. 0 is none."] = "航行中船只身后的尾流，大小为游泳者尾流的该倍数。0 为没有。",
	["How far forward along the ship the wake starts, in yards."] = "尾流自船身何处开始向前延伸（码）。",
	["How deep the waves of a ship's wake are, against a swimmer's."] = "相对于游泳者的尾流，船只尾流波浪的深度。",
	["How much of the sea's swash and shore waves a lake, a pond or a river gets. 0 is none, 100 as much as the sea."] = "湖泊、池塘或河流获得海洋拍岸浪和岸浪的程度。0 为没有，100 与海洋一样多。",
	["How much of the sea's shore foam a lake, a pond or a river gets. 100 is as much as the sea."] = "湖泊、池塘或河流获得海洋岸边泡沫的程度。100 与海洋一样多。",
	["How much of the sea's waves (Wave Height) a lake, a pond or a river gets. 0 lies flat, 100 rises as the sea."] = "湖泊、池塘或河流获得海洋波浪（浪高）的程度。0 为平静，100 与海洋一样起伏。",
	["A light in each lighthouse at night, with beams turning round."] = "夜间每座灯塔亮起灯光，并有旋转的光束。",
	["How bright the lighthouses' beams are. 0 leaves the light alone."] = "灯塔光束的亮度。0 只保留灯光本身。",
	["How bright the light in the lamp room is."] = "灯室中灯光的亮度。",
	["How far the beam reaches, in yards."] = "光束延伸的距离（码）。",
	["How far off a lighthouse's light shows, in yards. It fades out over the last quarter."] = "灯塔灯光在多远距离处可见（码）。其在最后四分之一处淡出。",
	["How thick the beam is at the lamp, in tenths of a yard."] = "光束在灯处的粗细（十分之一码）。",
	["How much wider the beam gets as it goes out. 0 is a straight beam."] = "光束向外延伸时加宽的程度。0 为笔直光束。",
	["Seconds the beam takes to go round once."] = "光束旋转一周所需的秒数。",
	["A second beam, opposite the first."] = "第二条光束，与第一条相反。",
	["How much light the lighthouse puts on the water: its glitter and the waves its beam lights."] = "灯塔洒在水面上的光量：其闪光以及光束照亮的波浪。",
	["How bright the waves are where the beam crosses the water."] = "光束掠过水面时波浪的亮度。",
	["How far a wave must tilt toward the lighthouse to catch the beam. Lower lights more of them."] = "波浪需向灯塔倾斜多少才能接住光束。数值越低能照亮更多波浪。",
	["How soft the edge is between the waves the beam lights and the dark ones."] = "光束照亮波浪与未照亮波浪之间边缘的柔和度。",
	["How wide the patch of water the beam lights is, as a share of the beam's own width. Beam Spread widens it too."] = "光束照亮水面的条带宽度（相对光束本身宽度的比例）。“光束扩散”也会使其加宽。",
	["Rings on the water where the rain falls, while it rains. 0 is none."] = "下雨时雨滴落点在水面形成的圆环。0 为没有。",
	["Foam round anything standing in the water: posts, rocks, piers. 0 is none."] = "水中的物体——柱子、岩石、码头——周围的泡沫。0 为没有。",
	["How far out from an object in the water its foam reaches, in tenths of a yard."] = "水中物体周围泡沫向外延伸多远（十分之一码）。",
	["Foam on the tops of the waves out in deep water."] = "深海区浪尖上的泡沫。",
	["How much foam the waves out in deep water carry."] = "深海区波浪携带的泡沫量。",
	["The water runs up the beach and slides back, with foam on its edge."] = "水冲上沙滩又退回，边缘带有泡沫。",
	["How far up the shore the water runs. 30 climbs 0.3 yards, and 1.5 yards up a gentle beach."] = "水冲上海岸的高度。30 爬升 0.3 码，在平缓海滩上可达 1.5 码。",
	["How many yards along the shore one surge spans. Lower gives more, shorter surges side by side."] = "一道浪沿海岸延伸多少码。数值越低会得到更多更短的浪并排推进。",
	["How fast the surges come. 100 is one every 11 seconds."] = "浪涌出现的速度。100 为每 11 秒一次。",
	["How much darker the sand is just above the water, where the waves have wet it. 0 is off."] = "紧邻水边、被波浪打湿处沙滩的变暗程度。0 为关闭。",
	["How far from you the wet sand shows, in yards. It fades out over the second half of this distance."] = "湿沙在距你多远内可见（码）。在此距离的后半段淡出。",
	["How bright the thin white line is where the water meets the sand. 0 is off."] = "水与沙滩相接处细细白线的亮度。0 为关闭。",
	["How wide that line is, in tenths of a yard along the sand."] = "该白线的宽度（沿沙滩十分之一码）。",
	["How bright the sun's reflection on the waves is. 0 is off."] = "太阳在波浪上反光的亮度。0 为关闭。",
	["How bright the two moons' reflections on the waves are at night. 0 is off."] = "夜间两颗月亮在波浪上反光的亮度。0 为关闭。",
	["How wide the sun's and moons' reflections spread. 200 is twice as wide as 100."] = "太阳和月亮反光散开的宽度。200 是 100 的两倍。",
	["The wake behind anyone moving through the water. 0 is off."] = "任何人在水中移动时身后的尾流。0 为关闭。",
	["How deep the rings round anyone standing still in the water look. 100 is as it was; their white line stays the same."] = "静止立于水中的人物周围圆环看起来的深度。100 为原样，其白线保持不变。",
	["How deep the rings look that anyone walking or swimming leaves behind. 100 is as it was; their white line stays the same."] = "行走或游泳者身后留下的圆环看起来的深度。100 为原样，其白线保持不变。",
	["How large the rings round anyone standing still in the water grow. 100 is as it was."] = "静止立于水中的人物周围圆环扩大的大小。100 为原样。",
	["How large the rings grow that anyone walking or swimming leaves behind. 100 is as it was."] = "行走或游泳者身后留下的圆环扩大的大小。100 为原样。",
	["How deep the ring is where anyone lands in the water from a jump or a fall. A harder landing makes a deeper one. 0 is none."] = "有人跳跃或坠落落入水面时圆环的深度。落得越重圆环越深。0 为没有。",
	["How much the water hides a character standing in it. 0 shows the legs as clear as the sand beside them."] = "水淹没站立其中人物的程度。0 使腿部像旁边的沙滩一样清晰可见。",
	["How tall the waves rise out on deep water, in tenths of a yard. They calm toward the shore. 0 is flat water."] = "深水区波浪的高度（十分之一码）。波浪在靠近岸边时趋于平静。0 为水面平静。",
	["How long the waves are, in percent. 200 is twice as long as 100."] = "波浪的长度（百分比）。200 是 100 的两倍。",
	["Makes what the sun reaches brighter, by up to this many percent. 0 leaves it as the game draws it."] = "使阳光照到之处更亮，最多提高该百分比。0 使画面保持游戏默认。",
	["Shade takes the sky's cool blue instead of only going darker."] = "阴影呈现天空的冷蓝色，而不只是变暗。",
	["Sunlit ground takes a warm colour."] = "被阳光照耀的地面呈现暖色。",
	["Higher gives sharper shadows and costs more frame rate. The shadows need Volumetric Light on."] = "数值越高阴影越清晰，同时更耗帧率。阴影需要开启体积光。",
	["How soft the edges of shadows are. 0 gives sharp edges."] = "阴影边缘的柔和程度。0 为锐利边缘。",
	["The shadow map is drawn again every this many frames. 1 is every frame. Higher costs less frame rate, and the shadows of moving things lag further behind."] = "阴影贴图每隔该帧数重绘一次。1 为每帧重绘。数值越高越省帧率，运动物体的阴影会拖得更远。",
	["How far around you, in yards, shadows have the sharpest edges. Higher reaches further, and the edges near you are coarser."] = "你周围多远处（码）的阴影边缘最清晰。数值越高范围越远，靠近你的边缘会更粗糙。",
	["Rays of light from the sun, through gaps in the trees and clouds."] = "穿过树冠和云层缝隙的太阳光束。",
	["How bright the rays are."] = "光束的亮度。",
	["Higher blurs the light the rays come from, so they hold still behind leaves. Lower gives sharper rays."] = "数值越高，光束来源的光越模糊，使其稳定在树叶后方。数值越低，光束更锐利。",
	["Higher keeps more of the last frame, so the rays change more gently as you move. Too high leaves a trail when you turn quickly."] = "数值越高保留越多上一帧，使你在移动时光束变化更平缓。过高时快速转身会留下残影。",
	["Sun rays and volumetric light at night. 100 is as strong as by day. 0 is off."] = "夜间太阳光线和体积光的强度。100 与白天一样强。0 为关闭。",
	["How much darker the world is at night. Lamps still light the ground near them. Not inside buildings. Needs Volumetric Light on."] = "夜间世界变暗的程度。灯火仍会照亮附近地面。不作用于建筑内部。需要开启体积光。",
	["How much darker again the night is while it rains, the sky too. The game's rain makes the night a lighter grey."] = "下雨时夜晚（连同天空）额外变暗的程度。游戏自带的雨会使夜晚呈更浅的灰色。",
	["How blue the night is, as moonlight. 0 only darkens."] = "夜晚呈月光的蓝色的程度。0 仅为变暗。",
	["Day and night saturation. Off costs nothing."] = "白昼与夜间饱和度。关闭不产生任何消耗。",
	["How strong the colours are by day. 100 is the game's own. 0 is grey."] = "白昼色彩的浓度。100 为游戏默认。0 为灰色。",
	["How strong the colours are at night. 100 is the game's own. 0 is grey. The change from day is slow, from dusk to full night."] = "夜间色彩的浓度。100 为游戏默认。0 为灰色。自白昼开始的变化较慢，出现在黄昏至入夜期间。",
	["How strong the game's Full Screen Glow is. 100 is the game's own. 0 is none. Needs Full Screen Glow on."] = "游戏全屏辉光的强度。100 为游戏默认。0 为没有。需要开启全屏辉光。",
	["Soft clouds that drift with the wind. Now and then one takes a shape. Every player in one place sees the same sky."] = "随风飘动的柔软云朵。偶尔一朵会变成形状。同一地点的每位玩家看到同一片天空。",
	["How much of the sky the clouds cover, in percent. Rain brings more."] = "云层遮盖天空的比例（百分比）。下雨会带来更多云。",
	["How soft the clouds' edges are, in percent."] = "云朵边缘的柔和程度（百分比）。",
	["How much the clouds are round, puffy cumulus, in percent. 0 gives smooth sheets and streaks."] = "云朵呈圆润蓬松积云状的程度（百分比）。0 为平滑的片状和条状。",
	["How big the clouds are, in yards across their largest billows. Bigger makes fewer, larger clouds. While the slider moves, the clouds jump."] = "云朵的大小（其最大翻卷处的码数）。数值越大云越少越大。拖动滑块时，云朵会跳动。",
	["How much of the clouds' fine detail is smoothed away, in percent. More gives big soft masses with fewer small bits; 0 is fine and broken."] = "云朵精细细节被平滑掉的程度（百分比）。数值越高得到少碎块的大团软云，0 为精细而破碎。",
	["How much the clouds change shape and thin out as they drift, in percent. 0 keeps their shape as they move."] = "云朵飘动时变形和变薄的程度（百分比）。0 保持其形状移动。",
	["How fast the clouds drift with the wind, in yards a second. While the slider moves, the clouds jump."] = "云朵随风飘动的速度（码/秒）。拖动滑块时，云朵会跳动。",
	["How many seconds the clouds take to change between clear and rain, both ways. After twice this time the change is nearly done."] = "云朵在晴雨之间转变所需的秒数（两个方向）。经过该时间的两倍后转变接近完成。",
	["Now and then a cloud takes a shape: a kodo, a whale, a crab. It comes in over the horizon on the wind."] = "偶尔一朵云变成形状：科多兽、鲸鱼、螃蟹。它沿着风从地平线飘来。",
	["How often a cloud takes a shape: a turtle, a kodo, a fish. 0 never."] = "云朵变形的频率：乌龟、科多兽、鱼。0 为从不。",
	["How big a shaped cloud is, in yards across. Bigger shapes also come further apart."] = "成形云朵的大小（横向码数）。更大的形状彼此离得也更远。",
	["How much the billows change a shaped cloud's outline, in percent. More makes a shape harder to see."] = "翻卷改变成形云朵外形的程度（百分比）。数值越高形状越难辨认。",
	["How much the clouds are cleared round a shaped cloud, in percent, so it stands on its own. The gap follows the shape's outline, with a fluffy rim."] = "成形云朵周围云层被清空的程度（百分比），使其独立显现。空隙沿形状轮廓展开，带蓬松边缘。",
	["How far the clear gap round a shaped cloud reaches out from its outline, in percent. 0 hugs the edge."] = "成形云朵周围清空空隙自其轮廓向外延伸的距离（百分比）。0 紧贴边缘。",
	["The grass sways in the wind and leans away from your character. Rocks, pebbles, shells and bones stay still."] = "草随风摆动，并远离你的角色倾斜。岩石、卵石、贝壳和骸骨保持静止。",
	["How strongly the wind sways the grass, in percent. 0 is still."] = "风摆动草地的强度（百分比）。0 为静止。",
	["How fast the waves run through the grass, in tenths of a yard a second."] = "草浪穿行的速度（十分之一码/秒）。",
	["Yards from one wave to the next. Small values sway small patches; large values make wide waves."] = "相邻两道草浪之间的距离（码）。小值让小块草地摆动，大值形成宽阔的波浪。",
	["Where the wind blows the grass, in degrees."] = "风将草吹向的方向（度）。",
	["How far the grass leans down the wind all the time, in percent of its sway."] = "草地持续顺风倾斜的程度（相对其摆动幅度的百分比）。",
	["How far the grass leans away from your character, in percent. 0 is none."] = "草远离你的角色倾斜的程度（百分比）。0 为没有。",
	["How far round your character the grass parts, in tenths of a yard."] = "角色周围草向两侧分开的距离（十分之一码）。",
	["How much grass and ground clutter the world plants. Costs frame rate, not memory."] = "世界中植被和地面杂物的数量。消耗帧率而非内存。",
};
local function L(text)
	if COMFYATMOSPHERE_LOCALE == "zhCN" or COMFYATMOSPHERE_LOCALE == "zhTW" then
		return COMFYATMOSPHERE_ZH[text] or text;
	end
	return text;
end

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
COMFYATMOSPHERE_SHADOW_RISE_FROM = "Low Sun From";
COMFYATMOSPHERE_SHADOW_HIGHEST = "Highest Sun";
COMFYATMOSPHERE_SUNLIGHT       = "Sunlight";
COMFYATMOSPHERE_TREE_SHADE     = "Tree Shadow Strength";
COMFYATMOSPHERE_FOLIAGE_SIZE   = "Foliage Shade Size";
COMFYATMOSPHERE_FOLIAGE_EDGE   = "Foliage Shade Edge";
COMFYATMOSPHERE_FOLIAGE_SELF   = "Foliage Self Shade";
COMFYATMOSPHERE_FOLIAGE_UNITS  = "Creature Shadow on Foliage";
COMFYATMOSPHERE_WATER          = "Water Effects";
COMFYATMOSPHERE_WATER_SUNSET   = "Sunset Water";
COMFYATMOSPHERE_WATER_ZONE_SATURATION = "Water Saturation";
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
COMFYATMOSPHERE_WATER_JUMP_RIPPLE = "Jump Ripple";
COMFYATMOSPHERE_GAME_GLOW = "Game Glow";
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
COMFYATMOSPHERE_WATER_WET_SAND_DISTANCE = "Wet Sand Distance";
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
COMFYATMOSPHERE_CLOUD_COVER    = "Cloud Cover";
COMFYATMOSPHERE_CLOUD_SHAPES_ON = "Cloud Shapes";
COMFYATMOSPHERE_CLOUD_SHAPES   = "Cloud Shape Amount";
COMFYATMOSPHERE_CLOUD_SOFTNESS = "Cloud Softness";
COMFYATMOSPHERE_CLOUD_BILLOWS  = "Cloud Shape Billows";
COMFYATMOSPHERE_CLOUD_CLEARING = "Cloud Shape Cutout";
COMFYATMOSPHERE_CLOUD_CUTOUT_WIDTH = "Cloud Shape Cutout Width";
COMFYATMOSPHERE_CLOUD_SHAPE_SIZE = "Cloud Shape Size";
COMFYATMOSPHERE_CLOUD_RESHAPE  = "Cloud Reshaping";
COMFYATMOSPHERE_CLOUD_PUFFINESS = "Cloud Puffiness";
COMFYATMOSPHERE_CLOUD_SPEED    = "Cloud Speed";
COMFYATMOSPHERE_CLOUD_TRANSITION = "Cloud Transition Time";
COMFYATMOSPHERE_CLOUD_SCALE    = "Cloud Scale";
COMFYATMOSPHERE_CLOUD_LAYER_SOFTNESS = "Cloud Layer Softness";
COMFYATMOSPHERE_MIST           = "Fog";
COMFYATMOSPHERE_MIST_DENSITY   = "Fog Density";
COMFYATMOSPHERE_MIST_HEIGHT    = "Fog Height";
COMFYATMOSPHERE_MIST_BRIGHTNESS = "Fog Brightness";
COMFYATMOSPHERE_MIST_SUN       = "Fog Sunlight";
COMFYATMOSPHERE_MIST_TOWARD    = "Fog Toward the Sun";
COMFYATMOSPHERE_MIST_HORIZON   = "Fog Horizon Glow";
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
		-- A percentage: comfyatmos.dll divides it by 100 for [fog] toward (2026-10-08).
		name = "COMFYATMOSPHERE_MIST_TOWARD",
		desc = "How much brighter the fog glows looking toward the sun, and darker looking away. 0 is the same from every side.",
		type = "slider",
		cvar = "comfyMistToward",
		dependency = { "comfyMist", "1" },
		minval = 0,
		maxval = 90,
		step = 5,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [fog] horizonGlow (2026-10-08).
		name = "COMFYATMOSPHERE_MIST_HORIZON",
		desc = "With the sun low, how brightly the fog glows along the horizon in the sun's colour, brightest under the sun. 0 is none.",
		type = "slider",
		cvar = "comfyMistHorizon",
		dependency = { "comfyMist", "1" },
		minval = 0,
		maxval = 200,
		step = 10,
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
		-- Degrees: [sunshadows] riseFrom (2026-10-14).
		name = "COMFYATMOSPHERE_SHADOW_RISE_FROM",
		desc = "With the sun or moon lower than this, in degrees, the shadows' light climbs back up as it sinks, over the next 12 degrees down, so the shadows stay short. 0 is never.",
		type = "slider",
		cvar = "comfyShadowRiseFrom",
		dependency = { "comfySunShadows", "1", "comfyShadowLock", "0" },
		minval = 0,
		maxval = 30,
		step = 1,
		numberLabels = 1,
	},
	{
		-- Degrees: [sunshadows] highest (2026-10-15).
		name = "COMFYATMOSPHERE_SHADOW_HIGHEST",
		desc = "The highest the shadows' light stands, in degrees: at noon, and at the end of the climb under Low Sun From. Lower gives longer shadows; 90 is straight down.",
		type = "slider",
		cvar = "comfyShadowHighest",
		dependency = { "comfySunShadows", "1", "comfyShadowLock", "0" },
		minval = 10,
		maxval = 89,
		step = 1,
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
		-- Tenths of a yard: comfyatmos.dll divides it by 10 for [sunshadows] foliageRadius (2026-10-08).
		name = "COMFYATMOSPHERE_FOLIAGE_SIZE",
		desc = "The ground's grass and ferns take the shade of the ground round them, this many tenths of a yard across, so a plant is dark under cover and lit in the open, with no dapples on it. 0 lets the shade fall on them as it falls.",
		type = "slider",
		cvar = "comfyFoliageShadeSize",
		dependency = { "comfyVolume", "1" },
		minval = 0,
		maxval = 50,
		step = 1,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [sunshadows] foliageEdge (2026-10-08).
		name = "COMFYATMOSPHERE_FOLIAGE_EDGE",
		desc = "How softly a plant goes from lit to dark at the edge of a shadow. Low switches it at once; high blends it over a wider edge.",
		type = "slider",
		cvar = "comfyFoliageShadeEdge",
		dependency = { "comfyVolume", "1" },
		minval = 0,
		maxval = 50,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Tenths of a yard: comfyatmos.dll divides it by 10 for [sunshadows] foliageSlack (2026-10-08).
		name = "COMFYATMOSPHERE_FOLIAGE_SELF",
		desc = "The ground's grass and ferns take no shade from leaves nearer than this many tenths of a yard: their own and their neighbours'. Higher stops a tall fern shading itself; a tree's crown higher up still shades it.",
		type = "slider",
		cvar = "comfyFoliageSelfShade",
		dependency = { "comfyVolume", "1" },
		minval = 0,
		maxval = 50,
		step = 1,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [sunshadows] foliageUnits (2026-10-08).
		name = "COMFYATMOSPHERE_FOLIAGE_UNITS",
		desc = "How much of the shadows of characters and creatures falls on the ground's grass and ferns. 0 is none, so a passer-by does not print its shape on them.",
		type = "slider",
		cvar = "comfyFoliageCreatureShadow",
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
		-- A percentage: comfyatmos.dll divides it by 100 for [water] sunset (2026-10-07).
		name = "COMFYATMOSPHERE_WATER_SUNSET",
		desc = "How much more of the sky's red glow the water reflects with the sun low. 0 is none.",
		type = "slider",
		cvar = "comfyWaterSunset",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 100,
		step = 10,
		numberLabels = 1,
	},
	{
		-- A percentage: comfyatmos.dll divides it by 100 for [water] zoneSaturation (2026-10-14; the sky's colour since 2026-10-16).
		name = "COMFYATMOSPHERE_WATER_ZONE_SATURATION",
		desc = "How strong the water's colour is. The water takes the sky's colour for the place and the time of day. 0 is grey, 100 the sky's own colour.",
		type = "slider",
		cvar = "comfyWaterZoneSaturation",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 300,
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
		-- Yards: [water] wetSandReach (2026-10-13). Full to half of it, none at it.
		name = "COMFYATMOSPHERE_WATER_WET_SAND_DISTANCE",
		desc = "How far from you the wet sand shows, in yards. It fades out over the second half of this distance.",
		type = "slider",
		cvar = "comfyWaterWetSandDistance",
		dependency = { "comfyWater", "1" },
		minval = 20,
		maxval = 300,
		step = 10,
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
		-- A percentage: comfyatmos.dll divides it by 100 for [water] jumpRipple.
		name = "COMFYATMOSPHERE_WATER_JUMP_RIPPLE",
		desc = "How deep the ring is where anyone lands in the water from a jump or a fall. A harder landing makes a deeper one. 0 is none.",
		type = "slider",
		cvar = "comfyWaterJumpRipple",
		dependency = { "comfyWater", "1" },
		minval = 0,
		maxval = 600,
		step = 25,
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
		-- Percent: [colour] gameGlow (gameglow.cpp). Not under Color Effects: it scales the game's own pass.
		name = "COMFYATMOSPHERE_GAME_GLOW",
		desc = "How strong the game's Full Screen Glow is. 100 is the game's own. 0 is none. Needs Full Screen Glow on.",
		type = "slider",
		cvar = "comfyGameGlow",
		minval = 0,
		maxval = 200,
		step = 5,
		numberLabels = 1,
	},
	{
		name = "COMFYATMOSPHERE_CLOUDS",
		desc = "Soft clouds that drift with the wind. Now and then one takes a shape. Every player in one place sees the same sky.",
		type = "checkbutton",
		cvar = "comfyClouds",
	},
	{
		-- Percent: comfyatmos.dll divides it by 100 for [clouds] cover.
		name = "COMFYATMOSPHERE_CLOUD_COVER",
		desc = "How much of the sky the clouds cover, in percent. Rain brings more.",
		type = "slider",
		cvar = "comfyCloudCover",
		dependency = { "comfyClouds", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percent: [clouds] softness.
		name = "COMFYATMOSPHERE_CLOUD_SOFTNESS",
		desc = "How soft the clouds' edges are, in percent.",
		type = "slider",
		cvar = "comfyCloudSoftness",
		dependency = { "comfyClouds", "1" },
		minval = 5,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- [clouds] puffiness, as a percentage.
		name = "COMFYATMOSPHERE_CLOUD_PUFFINESS",
		desc = "How much the clouds are round, puffy cumulus, in percent. 0 gives smooth sheets and streaks.",
		type = "slider",
		cvar = "comfyCloudPuffiness",
		dependency = { "comfyClouds", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- [clouds] scale, yards.
		name = "COMFYATMOSPHERE_CLOUD_SCALE",
		desc = "How big the clouds are, in yards across their largest billows. Bigger makes fewer, larger clouds. While the slider moves, the clouds jump.",
		type = "slider",
		cvar = "comfyCloudScale",
		dependency = { "comfyClouds", "1" },
		minval = 1000,
		maxval = 8000,
		step = 100,
		numberLabels = 1,
	},
	{
		-- [clouds] layerSoftness, as a percentage.
		name = "COMFYATMOSPHERE_CLOUD_LAYER_SOFTNESS",
		desc = "How much of the clouds' fine detail is smoothed away, in percent. More gives big soft masses with fewer small bits; 0 is fine and broken.",
		type = "slider",
		cvar = "comfyCloudLayerSoftness",
		dependency = { "comfyClouds", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- [clouds] reshape, as a percentage.
		name = "COMFYATMOSPHERE_CLOUD_RESHAPE",
		desc = "How much the clouds change shape and thin out as they drift, in percent. 0 keeps their shape as they move.",
		type = "slider",
		cvar = "comfyCloudReshape",
		dependency = { "comfyClouds", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- [clouds] windSpeed, yards a second. Every player at one place must have the same to see the same sky.
		name = "COMFYATMOSPHERE_CLOUD_SPEED",
		desc = "How fast the clouds drift with the wind, in yards a second. While the slider moves, the clouds jump.",
		type = "slider",
		cvar = "comfyCloudSpeed",
		dependency = { "comfyClouds", "1" },
		minval = 0,
		maxval = 300,   -- 60 until 2026-10-11 (the owner: "a lot faster")
		step = 5,
		numberLabels = 1,
	},
	{
		-- [clouds] transitionTime, seconds.
		name = "COMFYATMOSPHERE_CLOUD_TRANSITION",
		desc = "How many seconds the clouds take to change between clear and rain, both ways. After twice this time the change is nearly done.",
		type = "slider",
		cvar = "comfyCloudTransition",
		dependency = { "comfyClouds", "1" },
		minval = 1,
		maxval = 60,
		step = 1,
		numberLabels = 1,
	},
	{
		-- [clouds] shapesOn: the shaped clouds at all; the sliders under it are theirs.
		name = "COMFYATMOSPHERE_CLOUD_SHAPES_ON",
		desc = "Now and then a cloud takes a shape: a kodo, a whale, a crab. It comes in over the horizon on the wind.",
		type = "checkbutton",
		cvar = "comfyCloudShapesOn",
		dependency = { "comfyClouds", "1" },
	},
	{
		-- [clouds] shapes: the percent chance for each patch of sky in each few minutes.
		name = "COMFYATMOSPHERE_CLOUD_SHAPES",
		desc = "How often a cloud takes a shape: a turtle, a kodo, a fish. 0 never.",
		type = "slider",
		cvar = "comfyCloudShapes",
		dependency = { "comfyCloudShapesOn", "1" },
		minval = 0,
		maxval = 100,   -- 50 until 2026-10-11
		step = 1,
		numberLabels = 1,
	},
	{
		-- [clouds] shapeSize, shown as the whole width in yards (the setting is half of it).
		name = "COMFYATMOSPHERE_CLOUD_SHAPE_SIZE",
		desc = "How big a shaped cloud is, in yards across. Bigger shapes also come further apart.",
		type = "slider",
		cvar = "comfyCloudShapeSize",
		dependency = { "comfyCloudShapesOn", "1" },
		minval = 1000,
		maxval = 6000,
		step = 100,
		numberLabels = 1,
	},
	{
		-- Percent: [clouds] erode.
		name = "COMFYATMOSPHERE_CLOUD_BILLOWS",
		desc = "How much the billows change a shaped cloud's outline, in percent. More makes a shape harder to see.",
		type = "slider",
		cvar = "comfyCloudShapeBillows",
		dependency = { "comfyCloudShapesOn", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- Percent: [clouds] clearing.
		name = "COMFYATMOSPHERE_CLOUD_CLEARING",
		desc = "How much the clouds are cleared round a shaped cloud, in percent, so it stands on its own. The gap follows the shape's outline, with a fluffy rim.",
		type = "slider",
		cvar = "comfyCloudClearing",
		dependency = { "comfyCloudShapesOn", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
	},
	{
		-- [clouds] cutoutWidth, as a percentage.
		name = "COMFYATMOSPHERE_CLOUD_CUTOUT_WIDTH",
		desc = "How far the clear gap round a shaped cloud reaches out from its outline, in percent. 0 hugs the edge.",
		type = "slider",
		cvar = "comfyCloudCutoutWidth",
		dependency = { "comfyCloudShapesOn", "1" },
		minval = 0,
		maxval = 100,
		step = 5,
		numberLabels = 1,
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
			.. "30 grass: the bend (black still, white the tips; blue where it comes from the texture)\n"
			.. "31 clouds: the shapes (red), the layer (blue)\n"
			.. "32 water: each foam by its source (red the shore, green round objects, blue the wake, yellow open water, white the lip and the ripples)\n"
			.. "33 water: the game's rain splashes left out; its other particles red under the water, green above\n"
			.. "34 sun: dots where the effects take it: yellow the light, red the shadows' sun, blue the other moon, white the sky's own sprites\n"
			.. "35 water: what draws each pixel (grey our water, yellow the shore, red the bed bare under water, cyan foam, green glint, white not a number; dots in their own colour: none of ours)\n"
			.. "36 water: the waves (red the slope, full at 0.2; green the swell's height, half where flat, a yard up full; blue the whitecaps)\n"
			.. "37 water: the mix (red the reflection's share x5, green what gets through to the bed, blue the path under the surface, full at 10 yards)",
		type = "slider",
		cvar = "comfyDebugView",
		minval = 0,
		maxval = 37,
		step = 1,
		numberLabels = 1,
	},
};

-- Translate the page's own labels and their descriptions, and the page's title. The game panel draws each
-- label by reading the global, so changing the global changes the page and this window together.
COMFYATMOSPHERE_CATEGORY = L(COMFYATMOSPHERE_CATEGORY);
for _, option in ipairs(ENTRIES) do
	local label = _G[option.name];
	if type(label) == "string" then
		_G[option.name] = L(label);
	end
	if option.desc then
		option.desc = L(option.desc);
	end
	if option.warning then
		option.warning = L(option.warning);
	end
end

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

local function Say(text)
	DEFAULT_CHAT_FRAME:AddMessage("|cff88cc88atmos|r: " .. text);
end

-- ---- the time of day (2026-10-07, from comfytime) --------------------------------------------------
--
-- The addon sends "<number> <verb> [value]" in comfyTimeSet: set <hour>, step <steps>, daynight, lock/local <0 or 1>. A new
-- number each time, so the same command twice runs twice; it starts from the clock, so a /reload does not repeat
-- the last number. comfyatmos.dll reads it every frame. It writes "<hour> <state>" into comfyTimeShown in place,
-- which needs the CVar to hold 16 characters, as comfyStats does. State: 0 the server's time ([time] enabled 0),
-- 1 locked, 2 an old comfytime.dll sets it, 3 the addresses did not check out, 4 the computer's local clock.

local TIME_LEN = 16;
local timeNumber = math.floor(GetTime() * 1000);

local function TimeSend(verb, value)
	if not HasCVar("comfyTimeSet") then
		Say(L("this comfyatmos.dll does not set the time of day."));
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

local function TimeText(hour, live)
	local minutes = math.floor(hour * 60 + (live and 0 or 0.5));
	return string.format("%02d:%02d", math.mod(math.floor(minutes / 60), 24), math.mod(minutes, 60));
end

-- "0" and the server's hour, for Server time. The client counts on from the last hour written, so the DLL writes
-- this one back. GetGameTime gives the server's time also while ours shows.
local function TimeOff()
	local h, m = GetGameTime();
	return "0 " .. string.format("%.4f", h + m / 60);
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

-- The Time section of the settings window (2026-10-12; the debug panel had these until then). One choice of three:
-- the server's time, the computer's clock, or a fixed hour. The slider and the buttons move the fixed hour, and
-- are dimmed in the other two. The modes are the DLL's states (TimeShown).
local TIME_MODES = {
	{ "Server", "Server time", 0, "The time of day the server sends. All players see the same sky." },
	{ "Computer", "Computer time", 4, "The computer's clock, with its timezone and daylight saving. Only your client shows it." },
	{ "Fixed", "Fixed hour", 1, "The hour set with the slider below. It stays, also after a restart. Only your client shows it." },
};
local timeSection = nil;

local function TimeSectionRefresh()
	if not timeSection then
		return;
	end
	local hour, state = TimeShown();
	local text = timeSection.text;
	if not hour then
		text:SetText(L("Waiting for comfyatmos.dll"));
	elseif state == 0 then
		local h, m = GetGameTime();
		text:SetText(L("Server time: ") .. string.format("%02d:%02d", h, m));
	elseif state == 1 then
		text:SetText(L("Fixed hour: ") .. TimeText(hour));
	elseif state == 4 then
		text:SetText(L("Computer time: ") .. TimeText(hour, true));
	elseif state == 2 then
		text:SetText(L("comfytime.dll sets the time. Remove it"));
	else
		text:SetText(L("Not set (another WoW.exe?). See the log"));
	end
	for _, mode in ipairs(TIME_MODES) do
		getglobal("ComfyAtmosphereTime" .. mode[1]):SetChecked(state == mode[3]);
	end
	-- In the other modes the slider shows the saved hour, which Fixed hour brings back.
	local slider = ComfyAtmosphereTimeHour;
	if hour and not slider.dragging then
		slider.refreshing = true;
		slider:SetValue(hour);
		slider.refreshing = false;
	end
	local fixed = state == 1;
	slider:EnableMouse(fixed);
	slider:SetAlpha(fixed and 1 or 0.4);
	for _, name in ipairs({ "Earlier", "Later", "DayNight" }) do
		local button = getglobal("ComfyAtmosphereTime" .. name);
		if fixed then button:Enable(); else button:Disable(); end
	end
end

-- The section's controls in parent, from y down; returns the y under them.
local function TimeSectionBuild(parent, y, width)
	local text = parent:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall");
	text:SetPoint("TOPLEFT", parent, "TOPLEFT", 4, y - 4);
	text:SetWidth(width - 8);
	text:SetJustifyH("LEFT");
	y = y - 22;

	for _, mode in ipairs(TIME_MODES) do
		local box = CreateFrame("CheckButton", "ComfyAtmosphereTime" .. mode[1], parent, "UICheckButtonTemplate");
		box:SetWidth(26);
		box:SetHeight(26);
		box:SetPoint("TOPLEFT", parent, "TOPLEFT", 0, y);
		getglobal(box:GetName() .. "Text"):SetText(L(mode[2]));
		box.comfyMode = mode;
		box:SetScript("OnClick", function()
			local _, state = TimeShown();
			local m = this.comfyMode;
			if state ~= m[3] then
				if m[3] == 0 then
					TimeSend("lock", TimeOff());
				elseif m[3] == 1 then
					TimeSend("lock", "1");
				else
					TimeSend("local", "1");
				end
			end
			-- Ticked at once; the next refresh shows what the DLL did.
			for _, other in ipairs(TIME_MODES) do
				getglobal("ComfyAtmosphereTime" .. other[1]):SetChecked(other == m);
			end
		end);
		box:SetScript("OnEnter", function()
			GameTooltip:SetOwner(this, "ANCHOR_RIGHT");
			GameTooltip:SetText(L(this.comfyMode[2]), 1, 1, 1);
			GameTooltip:AddLine(L(this.comfyMode[4]), NORMAL_FONT_COLOR.r, NORMAL_FONT_COLOR.g, NORMAL_FONT_COLOR.b, 1);
			GameTooltip:Show();
		end);
		box:SetScript("OnLeave", function() GameTooltip:Hide(); end);
		y = y - 30;
	end

	-- In quarter hours. The keys and the bindings move it too, so it follows the hour shown.
	local slider = CreateFrame("Slider", "ComfyAtmosphereTimeHour", parent, "OptionsSliderTemplate");
	slider:SetWidth(width - 16);
	slider:SetHeight(17);
	slider:SetPoint("TOPLEFT", parent, "TOPLEFT", 8, y - 16);
	slider:SetMinMaxValues(0, 24);
	slider:SetValueStep(0.25);
	ComfyAtmosphereTimeHourLow:SetText("0");
	ComfyAtmosphereTimeHourHigh:SetText("24");
	ComfyAtmosphereTimeHourText:SetText(L("Hour"));
	slider:SetScript("OnMouseDown", function() this.dragging = true; end);
	slider:SetScript("OnMouseUp", function() this.dragging = false; end);
	slider:SetScript("OnValueChanged", function()
		if this.refreshing then
			return;
		end
		TimeSend("set", string.format("%.4f", this:GetValue()));
	end);
	y = y - 50;

	-- A step either way ([time] step), and Day/Night.
	local function Button(name, label, w, x, onClick)
		local b = CreateFrame("Button", "ComfyAtmosphereTime" .. name, parent, "UIPanelButtonTemplate");
		b:SetWidth(w);
		b:SetHeight(22);
		b:SetPoint("TOPLEFT", parent, "TOPLEFT", x, y);
		b:SetText(label);
		b:SetScript("OnClick", onClick);
	end
	Button("Earlier", "<", 30, 4, function() TimeSend("step", -1); end);
	Button("Later", ">", 30, 38, function() TimeSend("step", 1); end);
	Button("DayNight", L("Day/Night"), 100, 72, function() TimeSend("daynight"); end);
	y = y - 28;

	timeSection = { text = text };
	local wait = 0;
	parent:SetScript("OnUpdate", function()
		wait = wait - arg1;
		if wait > 0 then
			return;
		end
		wait = 0.1;
		TimeSectionRefresh();
	end);
	return y;
end
-- The end of the Time section.

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
	{ "Fog", { "comfyMist", "comfyMistDensity", "comfyMistHeight", "comfyMistBrightness", "comfyMistSun", "comfyMistToward", "comfyMistHorizon",
	           "comfyMistReach", "comfyMistSky", "comfyMistPatches", "comfyMistLow", "comfyMistWater",
	           "comfyMistMorning", "comfyMistLamps", "comfyMistWind", "comfyMistWindDir" } },
	{ "Lamps", { "comfyLamps", "comfyLampGlow", "comfyLampDistance", "comfyLanternLight", "comfyTorchLight",
	             "comfyIndoorLamps", "comfyLampsDay", "comfyLighthouses", "comfyLighthouseBeam", "comfyLighthouseBeacon", "comfyLighthouseLength", "comfyLighthouseDistance", "comfyLighthouseWidth", "comfyLighthouseSpread", "comfyLighthouseSpeed", "comfyLighthouseTwoBeams", "comfyLighthouseGlint", "comfyLighthouseFace", "comfyLighthouseFaceTilt", "comfyLighthouseFaceSoft", "comfyLighthouseWaterWidth" } },
	{ "Shadows", { "comfySunShadows", "comfySunShadowsWorld", "comfySunShadowsUnits", "comfyShadowLock",
	               "comfyShadowTilt", "comfyShadowRiseFrom", "comfyShadowHighest", "comfySunShadowStrength", "comfySunShadowsNight",
	               "comfySunShadowsUnitStrength", "comfySunShadowsBody", "comfyShadowOnWater", "comfySunGlide", "comfyTreeShade", "comfyFoliageShadeSize", "comfyFoliageShadeEdge", "comfyFoliageSelfShade", "comfyFoliageCreatureShadow", "comfySunlight", "comfyShadeTint",
	               "comfySunTint", "comfyShadowResolution", "comfyShadowSoftness", "comfyShadowEvery",
	               "comfyShadowNear" } },
	{ "Sky", { "comfyRays", "comfyRaysStrength", "comfyRaysSoften", "comfyRaysSmooth", "comfyNightStrength",
	           "comfyNightDarkness", "comfyRainDarkness", "comfyMoonlight" } },
	{ "Clouds", { "comfyClouds", "comfyCloudCover", "comfyCloudSoftness", "comfyCloudPuffiness", "comfyCloudScale",
	              "comfyCloudLayerSoftness", "comfyCloudReshape", "comfyCloudSpeed", "comfyCloudTransition",
	              "comfyCloudShapesOn", "comfyCloudShapes", "comfyCloudShapeSize", "comfyCloudShapeBillows",
	              "comfyCloudClearing", "comfyCloudCutoutWidth" } },
	{ "Color", { "comfyColor", "comfyDaySaturation", "comfyNightSaturation", "comfyGameGlow" } },
	{ "Grass", { "comfyGrass", "comfyGrassWind", "comfyGrassSpeed", "comfyGrassWaveLength", "comfyGrassWindDir",
	             "comfyGrassLean", "comfyGrassParting", "comfyGrassPartingRadius", "frillDensity" } },
	{ "Water", { "comfyWater", "comfyWaterZoneSaturation", "comfyWaterSunset", "comfyWaterBright", "comfyWaterClarity", "comfyWaterReflect", "comfyWaterBend", "comfyWaterCover", "comfyWaterFoam", "comfyWaterFoamDrawn", "comfyWaterFoamSize", "comfyWaterFoamReach", "comfyWaterFoamEdge", "comfyWaterWakeFoam", "comfyWaterShipWake", "comfyWaterShipForward", "comfyWaterShipDepth", "comfyWaterLakeSwash", "comfyWaterLakeFoam", "comfyWaterLakeWaves", "comfyWaterRain", "comfyWaterObjectFoam", "comfyWaterObjectFoamWidth", "comfyWaterOpenFoam", "comfyWaterOpenFoamAmount", "comfyWaterSwash", "comfyWaterSwashHeight", "comfyWaterSwashLength", "comfyWaterSwashSpeed", "comfyWaterWetSand", "comfyWaterWetSandDistance", "comfyWaterWake", "comfyWaterRippleDepth", "comfyWaterRippleMoving", "comfyWaterRippleSpread", "comfyWaterRippleSpreadMoving", "comfyWaterJumpRipple",
	             "comfyWaterEdge", "comfyWaterEdgeWidth",
	             "comfyWaterGlint", "comfyWaterMoonGlint", "comfyWaterGlintSize",
	             "comfyWaveHeight",
	             "comfyWaveSize" } },
	{ "Time", {} },     -- controls of its own (TimeSectionBuild), not CVars
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
	local shown = names and names[value - control.option.minval + 1] and L(names[value - control.option.minval + 1]) or tostring(value);
	getglobal(control.frame:GetName() .. "Text"):SetText(WindowLabel(control.option) .. ": " .. shown);
end

local function WindowRefresh()
	if not window then
		return;
	end
	windowRefreshing = true;
	for _, control in ipairs(windowControls) do
		local option = control.option;
		-- A dependency is pairs of a CVar and the value it needs; every pair must hold.
		local enabled = true;
		if option.dependency then
			for i = 1, table.getn(option.dependency), 2 do
				if GetCVar(option.dependency[i]) ~= option.dependency[i + 1] then
					enabled = false;
				end
			end
		end
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

	-- Each CVar's tab: its section, or the last one with CVars (Time, the last, has controls of its own).
	local sectionOf = {};
	for i, section in ipairs(WINDOW_SECTIONS) do
		for _, cvar in ipairs(section[2]) do
			sectionOf[cvar] = i;
		end
	end
	local count = table.getn(WINDOW_SECTIONS);
	local others = count;
	while others > 1 and table.getn(WINDOW_SECTIONS[others][2]) == 0 do
		others = others - 1;
	end

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
		text:SetText(L(WINDOW_SECTIONS[t][1]));
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
			local t = sectionOf[option.cvar] or others;
			heights[t] = Add(option, i, contents[t], 0, heights[t], CONTENT_WIDTH);
		end
	end
	for t = 1, count do
		if WINDOW_SECTIONS[t][1] == "Time" then
			heights[t] = TimeSectionBuild(contents[t], heights[t], CONTENT_WIDTH);
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
		defaults:SetText(DEFAULTS or L("Defaults"));
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
	debug:SetText(L("Debug"));
	GreyButton(debug);
	debug:SetScript("OnClick", function() ComfyAtmosphere_DebugToggle(); end);
	local done = CreateFrame("Button", "ComfyAtmosphereWindowDone", window, "UIPanelButtonTemplate");
	done:SetWidth(72);
	done:SetHeight(22);
	done:SetPoint("BOTTOMRIGHT", window, "BOTTOMRIGHT", -14, 12);
	done:SetText(CLOSE or L("Close"));
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
		Say(L("no answer from comfyatmos.dll."));
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

-- True while an /atmos command waits for its answer or others wait behind it (2026-10-09): the test tool's
-- ComfyTest sends a run of them without a pause and waits on this before its next line.
function ComfyAtmosphere_TuneBusy()
	return tuneWaiting ~= nil or table.getn(tuneQueue) > 0;
end

SLASH_COMFYATMOS1 = "/atmos";
-- /atmos stats: comfyatmos.dll's figures on screen, for finding faults. The DLL writes them into the CVar
-- comfyStats once a second as name=value; pairs, padded with spaces, and this lays them out in sections.
-- /atmos debug: a panel of buttons for finding faults (probe, stats, debug view, trace, benchmark), and the time
-- of day.
-- Every action can be put on a key in Key Bindings > ComfyAtmosphere (Bindings.xml), with no key bound at first.
-- comfyatmos.dll's own keys (F11, F12, Ctrl+PageUp and the rest) are read past the game's key bindings, so they
-- are off unless the debug panel's Developer keys box is ticked (2026-10-07).

BINDING_HEADER_COMFYATMOSPHERE           = L("ComfyAtmosphere");
BINDING_NAME_COMFYATMOSPHERE_PROBE       = L("Probe (log one frame)");
BINDING_NAME_COMFYATMOSPHERE_STATS       = L("Show or hide the stats");
BINDING_NAME_COMFYATMOSPHERE_DEBUG       = L("Show or hide the debug panel");
BINDING_NAME_COMFYATMOSPHERE_RELOAD      = L("Read comfyatmos.ini again");
BINDING_NAME_COMFYATMOSPHERE_RAYS        = L("Sun rays on or off");
BINDING_NAME_COMFYATMOSPHERE_VOLUME      = L("Volumetric light on or off");
BINDING_NAME_COMFYATMOSPHERE_BENCH       = L("Run the benchmark");
BINDING_NAME_COMFYATMOSPHERE_TIME_LATER  = L("Time of day later");
BINDING_NAME_COMFYATMOSPHERE_TIME_EARLIER = L("Time of day earlier");
BINDING_NAME_COMFYATMOSPHERE_DAYNIGHT    = L("Day or night");

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
	"clouds: the shapes (red), the layer (blue)",
	"water: each foam by its source (red the shore, green round objects, blue the wake, yellow open water, white the lip and the ripples)",
	"water: the game's rain splashes left out; its other particles red under the water, green above",
	"sun: dots where the effects take it: yellow the light, red the shadows' sun, blue the other moon, white the sky's own sprites",
	"water: what draws each pixel (grey our water, yellow the shore, red the bed bare under water, cyan foam, green glint, white not a number; dots in their own colour: none of ours)",
	"water: the waves (red the slope, full at 0.2; green the swell's height, half where flat, a yard up full; blue the whitecaps)",
	"water: the mix (red the reflection's share x5, green what gets through to the bed, blue the path under the surface, full at 10 yards)",
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
		table.insert(rows, { L(text) });
	end
	local function Row(label, value, warn)
		table.insert(rows, { L(label), value, warn });
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
	local f = PanelMake("ComfyAtmosphereStats", L("Atmosphere stats"), 420, 20, -120);
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
			rows = { { L("Waiting for comfyatmos.dll (a second)...") } };
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
		Say(L("this comfyatmos.dll has no stats."));
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
	debugFrame.view:SetText(L("View ") .. v .. ": " .. DEBUG_VIEWS[v + 1]);
	ComfyAtmosphereDebugTrace:SetText(L(traceOn and "Trace: on" or "Trace: off"));

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
	local f = PanelMake("ComfyAtmosphereDebug", L("Atmosphere debug"), 300, 400, -120);
	DebugButton(f, "Probe", L("Probe"), 132, 12, -30, function() ComfyAtmosphere_Probe(); end);
	DebugButton(f, "Stats", L("Stats"), 132, 152, -30, function() ComfyAtmosphere_StatsToggle(); end);
	DebugButton(f, "Bench", L("Benchmark"), 132, 12, -56, function() SlashCmdList["COMFYATMOS"]("bench"); end);
	DebugButton(f, "Trace", L("Trace: off"), 132, 152, -56, function()
		-- The next probe also traces 180 frames, line by line. The game runs slowly while it does.
		traceOn = not traceOn;
		SlashCmdList["COMFYATMOS"]("general.trace " .. (traceOn and "1" or "0"));
		DebugRefresh();
	end);
	local heading = f:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall");
	heading:SetPoint("TOPLEFT", f, "TOPLEFT", 14, -88);
	heading:SetText(L("Debug view"));
	DebugButton(f, "Prev", "<", 30, 12, -102, function() DebugViewStep(-1); end);
	DebugButton(f, "Next", ">", 30, 254, -102, function() DebugViewStep(1); end);
	local view = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall");
	view:SetPoint("TOPLEFT", f, "TOPLEFT", 48, -102);
	view:SetWidth(200);
	view:SetHeight(22);
	view:SetJustifyH("CENTER");
	f.view = view;

	-- comfyatmos.dll's own keys, read past the game's key bindings ([general] hotkeys). A control CVar, so the
	-- game keeps it in Config.wtf.
	local hotkeys = CreateFrame("CheckButton", "ComfyAtmosphereDebugHotkeys", f, "UICheckButtonTemplate");
	hotkeys:SetPoint("TOPLEFT", f, "TOPLEFT", 10, -134);
	hotkeys:SetWidth(24);
	hotkeys:SetHeight(24);
	ComfyAtmosphereDebugHotkeysText:SetText(L("Developer keys (F11, F12, Ctrl+PageUp)"));
	hotkeys:SetScript("OnClick", function()
		if not HasCVar("comfyHotkeys") then
			Say(L("this comfyatmos.dll has no Developer keys switch: its keys are always on."));
			return;
		end
		SetCVar("comfyHotkeys", this:GetChecked() and "1" or "0");
	end);
	f:SetHeight(170);

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
		Say(L("comfyatmos.dll is not loaded."));
		return;
	end
	if not debugFrame then
		debugFrame = DebugBuild();
	end
	if debugFrame:IsShown() then
		debugFrame:Hide();
	else
		debugFrame:Show();
		DebugRefresh();
	end
end

SlashCmdList["COMFYATMOS"] = function(msg)
	local command = string.lower((string.gsub(msg or "", "^%s*(.-)%s*$", "%1")));
	-- /atmos alone opens the settings window, as /atmos options does (2026-10-14, the owner); /atmos help lists the
	-- commands.
	if command == "" or command == "options" then
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
	if command == "help" then
		Say(L("/atmos: the settings window. /atmos debug: buttons for finding faults. /atmos stats: figures on screen."));
	end
	if not HasCVar("comfyTune") then
		Say(L("this comfyatmos.dll has no /atmos. It needs the version from 2026-09-29 or later."));
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

-- The minimap button (2026-10-17, the owner). Left-click opens the settings window, as /atmos does.
-- Right-click opens a menu: the settings window, the debug panel, the stats and the probe. Drag moves
-- the button around the minimap's edge, and ComfyAtmosphereDB.minimapAngle keeps the place, in degrees
-- from the right, counter-clockwise. The button is made only when the DLL is loaded, as the page is.
local MINIMAP_RADIUS = 80;
local minimapButton = nil;

local function MinimapButtonPlace()
	local angle = math.rad(ComfyAtmosphereDB.minimapAngle);
	minimapButton:ClearAllPoints();
	minimapButton:SetPoint("CENTER", Minimap, "CENTER",
		MINIMAP_RADIUS * math.cos(angle), MINIMAP_RADIUS * math.sin(angle));
end

local function MinimapMenuInit()
	local entries = {
		{ L("Options"), WindowToggle, window and window:IsShown() },
		{ L("Debug"), ComfyAtmosphere_DebugToggle, debugFrame and debugFrame:IsShown() },
		{ L("Stats"), ComfyAtmosphere_StatsToggle, statsFrame and statsFrame:IsShown() },
		{ L("Probe"), ComfyAtmosphere_Probe },
	};
	-- Every row keeps the room for a tick, Probe and the title too: the client moves the text of a row
	-- with notCheckable 27 pixels to the left, out of line with the others.
	local info = UIDropDownMenu_CreateInfo();
	info.text = L("Atmosphere");
	info.isTitle = 1;
	UIDropDownMenu_AddButton(info);
	for _, entry in ipairs(entries) do
		info = UIDropDownMenu_CreateInfo();
		info.text = entry[1];
		info.func = entry[2];
		info.checked = entry[3] and 1 or nil;
		UIDropDownMenu_AddButton(info);
	end
end

local function MinimapButtonBuild()
	if minimapButton then
		return;
	end
	if not ComfyAtmosphereDB then
		ComfyAtmosphereDB = {};
	end
	if not ComfyAtmosphereDB.minimapAngle then
		ComfyAtmosphereDB.minimapAngle = 200;
	end

	local b = CreateFrame("Button", "ComfyAtmosphereMinimapButton", Minimap);
	minimapButton = b;
	b:SetWidth(31);
	b:SetHeight(31);
	b:SetFrameStrata("MEDIUM");
	b:SetFrameLevel(8);
	b:SetHighlightTexture("Interface\\Minimap\\UI-Minimap-ZoomButton-Highlight");
	local icon = b:CreateTexture(nil, "BACKGROUND");
	icon:SetTexture("Interface\\Icons\\Spell_Nature_MoonGlow");
	icon:SetWidth(20);
	icon:SetHeight(20);
	icon:SetPoint("TOPLEFT", b, "TOPLEFT", 7, -5);
	local border = b:CreateTexture(nil, "OVERLAY");
	border:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder");
	border:SetWidth(53);
	border:SetHeight(53);
	border:SetPoint("TOPLEFT", b, "TOPLEFT", 0, 0);

	local menu = CreateFrame("Frame", "ComfyAtmosphereMinimapMenu", UIParent, "UIDropDownMenuTemplate");
	UIDropDownMenu_Initialize(menu, MinimapMenuInit, "MENU");

	b:RegisterForClicks("LeftButtonUp", "RightButtonUp");
	b:RegisterForDrag("LeftButton");
	b:SetScript("OnClick", function()
		GameTooltip:Hide();
		if arg1 == "RightButton" then
			ToggleDropDownMenu(1, nil, menu, "cursor");
		else
			CloseDropDownMenus();
			WindowToggle();
		end
	end);
	b:SetScript("OnDragStart", function()
		GameTooltip:Hide();
		this:SetScript("OnUpdate", function()
			local mx, my = Minimap:GetCenter();
			local px, py = GetCursorPosition();
			local scale = Minimap:GetEffectiveScale();
			ComfyAtmosphereDB.minimapAngle = math.deg(math.atan2(py / scale - my, px / scale - mx));
			MinimapButtonPlace();
		end);
	end);
	b:SetScript("OnDragStop", function()
		this:SetScript("OnUpdate", nil);
	end);
	b:SetScript("OnEnter", function()
		GameTooltip:SetOwner(this, "ANCHOR_LEFT");
		GameTooltip:SetText(L("Atmosphere"), 1, 1, 1);
		GameTooltip:AddLine(L("Click: settings"), NORMAL_FONT_COLOR.r, NORMAL_FONT_COLOR.g, NORMAL_FONT_COLOR.b);
		GameTooltip:AddLine(L("Right-click: options, debug, stats, probe"),
			NORMAL_FONT_COLOR.r, NORMAL_FONT_COLOR.g, NORMAL_FONT_COLOR.b);
		GameTooltip:AddLine(L("Drag: move"), NORMAL_FONT_COLOR.r, NORMAL_FONT_COLOR.g, NORMAL_FONT_COLOR.b);
		GameTooltip:Show();
	end);
	b:SetScript("OnLeave", function() GameTooltip:Hide(); end);

	MinimapButtonPlace();
end

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
			.. "Type /atmos to open them.");
	end
	MinimapButtonBuild();
	syncFrame:Show();
end);
