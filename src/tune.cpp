// tune: the /atmos command, for tuning any comfyatmos.ini value from the game.
//
//   /atmos                          the commands
//   /atmos <section>                every value in a section
//   /atmos <section>.<key>          one value, and where it came from
//   /atmos <section>.<key> <value>  set it, at once; the key alone will do when no other section has it
//   /atmos list                     what /atmos has set
//   /atmos reset                    drop those, back to comfyatmos.ini
//   /atmos save                     write them into comfyatmos.ini, keeping its comments
//   /atmos probe                    log the next frame, as F12 does
//   /atmos bench                    run the benchmark, as Alt+F12 does
//   /atmos framelog [seconds]       time every frame for that long (10), then log the slowest
//
// The ComfyAtmosphere addon puts the command text in the CVar comfyTune, with a number in front; cvars.cpp
// reads it and registers the answer as new CVars, which the addon prints (see cvars.cpp).
//
// A value set here wins over comfyatmos.ini on every reload, F11 too, until reset or saved. The keys are
// the ones LoadSettings read (ConfigKeys), so every ini setting can be tuned and a misspelt one is
// refused rather than ignored. The values a control on the Atmosphere page sets are refused too: the
// control is laid over the ini, so a value set here would not show.

#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include "bench.h"
#include "common.h"
#include "config.h"
#include "rays.h"
#include "timeofday.h"
#include "tune.h"
#include "volume.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    struct Control { const char* key; const char* name; const char* where = "on the Atmosphere page"; };
    const Control kControls[] = {
        { "general.enabled",     "Atmosphere Effects" },
        { "general.hotkeys",     "Developer keys", "in the debug panel" },
        { "volume.enabled",      "Volumetric Light" },
        { "volume.strength",     "Volumetric Light Strength" },
        { "volume.quality",      "Volumetric Light Quality" },
        { "volume.density",      "Light Density" },
        { "volume.maxDistance",  "Light Distance" },
        { "volume.anisotropy",   "Light Toward the Sun" },
        { "shadow.size",         "Shadow Resolution" },
        { "sunshadows.softness", "Shadow Softness" },
        { "shadow.mapEvery",     "Shadow Redraw" },
        { "shadow.nearRange",    "Near Shadow Distance" },
        { "sunshadows.leafShade", "Tree Shadow Strength" },
        { "water.enabled",       "Water Effects" },
        { "water.colour",        "Water Colour" },
        { "water.zone",          "Zone Colour" },
        { "water.sunset",        "Sunset Water" },
        { "water.clarity",       "Water Clarity" },
        { "water.reflection",    "Sky Reflection" },
        { "water.refraction",    "Underwater Distortion" },
        { "water.cover",         "Underwater Cover" },
        { "water.wake",          "Wake" },
        { "water.foam",          "Foam" },
        { "water.swash",         "Swash" },
        { "water.swashHeight",   "Swash Height" },
        { "water.swashLength",   "Swash Length" },
        { "water.swashSpeed",    "Swash Speed" },
        { "water.glint",         "Sun Glint" },
        { "water.moonGlint",     "Moon Glint" },
        { "water.glintSize",     "Glint Size" },
        { "water.edgeLine",      "Edge Line" },
        { "water.edgeWidth",     "Edge Line Width" },
        { "water.brightness",    "Water Brightness" },
        { "water.rippleDepth",   "Standing Ripple Depth" },
        { "water.rippleDepthMoving", "Moving Ripple Depth" },
        { "water.rippleSpread",  "Standing Ripple Spread" },
        { "water.foamDrawn",     "Drawn Foam" },
        { "water.foamCell",      "Foam Size" },
        { "water.foamLife",      "Foam Reach" },
        { "water.foamEdge",      "Foam Breakup" },
        { "water.wakeFoam",      "Wake Foam" },
        { "water.shipWake",      "Ship Wake" },
        { "water.shipWakeForward", "Ship Wake Forward" },
        { "water.shipWakeDepth", "Ship Wake Depth" },
        { "water.objectFoam",    "Object Foam" },
        { "water.lakeSwash",     "Lake Swash" },
        { "water.lakeFoam",      "Lake Foam" },
        { "water.lakeWaves",     "Lake Waves" },
        { "water.rain",          "Rain on Water" },
        { "water.wetSand",       "Wet Sand" },
        { "lighthouse.enabled",  "Lighthouses" },
        { "lighthouse.beam",     "Lighthouse Beam" },
        { "lighthouse.beacon",   "Lighthouse Beacon" },
        { "lighthouse.beamLength", "Beam Length" },
        { "lighthouse.reach",   "Lighthouse Distance" },
        { "lighthouse.beamWidth", "Beam Width" },
        { "lighthouse.beamSpread", "Beam Spread" },
        { "lighthouse.beamSpeed", "Beam Speed" },
        { "lighthouse.beamCount", "Two Beams" },
        { "lighthouse.glint",    "Lighthouse Glint" },
        { "lighthouse.faceStrength", "Wave Face Strength" },
        { "lighthouse.faceTilt", "Wave Face Tilt" },
        { "lighthouse.faceSoft", "Wave Face Softness" },
        { "lighthouse.waterWidth", "Beam Width on Water" },
        { "water.objectFoamWidth", "Object Foam Width" },
        { "water.openFoam",      "Open Water Foam" },
        { "water.whitecaps",     "Open Water Foam Amount" },
        { "water.rippleSpreadMoving", "Moving Ripple Spread" },
        { "water.waveHeight",    "Wave Height" },
        { "water.waveScale",     "Wave Size" },
        { "sunshadows.enabled",  "Sun Shadows" },
        { "sunshadows.world",    "World / Object Shadows" },
        { "sunshadows.units",    "Player / Creature Shadows" },
        { "sunshadows.lock",     "Lock Shadow Angle" },
        { "sunshadows.lockTilt", "Shadow Angle" },
        { "sunshadows.shadeTint", "Shade Colour" },
        { "sunshadows.sunTint",  "Sunlight Warmth" },
        { "fog.enabled",         "Fog" },
        { "fog.density",         "Fog Density" },
        { "fog.height",          "Fog Height" },
        { "fog.brightness",      "Fog Brightness" },
        { "fog.sunLight",        "Fog Sunlight" },
        { "fog.toward",          "Fog Toward the Sun" },
        { "fog.reach",           "Fog Reach" },
        { "fog.skyDistance",     "Fog on Sky" },
        { "fog.patchiness",      "Fog Patchiness" },
        { "fog.lowGround",       "Low Ground Mist" },
        { "fog.water",           "Water Mist" },
        { "fog.morning",         "Morning Mist" },
        { "fog.lampMist",        "Lamps in Mist" },
        { "fog.windSpeed",       "Wind Speed" },
        { "fog.windDeg",         "Wind Direction" },
        { "lamps.enabled",       "Lamps" },
        { "lamps.strength",      "Lamp Glow" },
        { "lamps.fogReach",      "Lamp Distance" },
        { "lamps.lanternLight",  "Lantern Light" },
        { "lamps.torchLight",    "Torch Light" },
        { "lamps.indoors",       "Indoor Lamps" },
        { "lamps.day",           "Lamps by Day" },
        { "sunshadows.strength", "Sun Shadow Strength" },
        { "sunshadows.night",    "Night Shadows" },
        { "sunshadows.unitStrength", "Character Shadow Strength" },
        { "sunshadows.bodyShade", "Character Backside Shadow" },
        { "sunshadows.water",    "Shadow on Water" },
        { "sun.glide",           "Sun Smoothing" },
        { "sunshadows.sunlight", "Sunlight" },
        { "rays.enabled",        "Sun Rays" },
        { "rays.strength",       "Sun Rays Strength" },
        { "rays.soften",         "Sun Rays Softness" },
        { "rays.smooth",         "Sun Rays Smoothing" },
        { "night.strength",      "Night Strength" },
        { "night.darkness",      "Night Darkness" },
        { "night.rain",          "Rain Darkness" },
        { "night.tint",          "Moonlight Colour" },
        { "colour.enabled",      "Color Effects" },
        { "colour.day",          "Day Saturation" },
        { "colour.night",        "Night Saturation" },
        { "sky.clouds",          "Clouds" },
    };

    const size_t kLine = 200;   // characters in one chat line

    bool Same(const std::string& a, const std::string& b)
    {
        return _stricmp(a.c_str(), b.c_str()) == 0;
    }

    std::string Name(const ConfigKey& k) { return k.section + "." + k.key; }

    const Control* ControlFor(const std::string& name)
    {
        for (const Control& c : kControls)
            if (Same(name, c.key))
                return &c;
        return nullptr;
    }

    std::vector<std::string> Words(const std::string& s)
    {
        std::vector<std::string> out;
        size_t i = 0;
        while (i < s.size())
        {
            while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i;
            size_t j = i;
            while (j < s.size() && !isspace(static_cast<unsigned char>(s[j]))) ++j;
            if (j > i)
                out.push_back(s.substr(i, j - i));
            i = j;
        }
        return out;
    }

    // The section as the code spells it, or empty.
    std::string Section(const std::string& word)
    {
        for (const ConfigKey& k : ConfigKeys())
            if (Same(k.section, word))
                return k.section;
        return {};
    }

    // The key a word names: "section.key", or a key no other section has. -1 when none, -2 when the
    // key alone is in more than one section.
    int Find(const std::string& word)
    {
        const std::vector<ConfigKey>& keys = ConfigKeys();
        const size_t dot = word.find('.');
        if (dot != std::string::npos)
        {
            const std::string sec = word.substr(0, dot), key = word.substr(dot + 1);
            for (size_t i = 0; i < keys.size(); ++i)
                if (Same(keys[i].section, sec) && Same(keys[i].key, key))
                    return static_cast<int>(i);
            return -1;
        }
        int found = -1;
        for (size_t i = 0; i < keys.size(); ++i)
            if (Same(keys[i].key, word))
            {
                if (found >= 0 && !Same(keys[found].section, keys[i].section))
                    return -2;
                if (found < 0)
                    found = static_cast<int>(i);
            }
        return found;
    }

    // What comfyatmos.ini itself says for a key, without its comment, or empty.
    std::string IniText(const ConfigKey& k)
    {
        char buf[128] = {};
        char ini[MAX_PATH] = {};
        WideCharToMultiByte(CP_ACP, 0, ConfigIniPath(), -1, ini, MAX_PATH, nullptr, nullptr);
        GetPrivateProfileStringA(k.section.c_str(), k.key.c_str(), "", buf, sizeof(buf), ini);
        std::string s = buf;
        const size_t semi = s.find(';');
        if (semi != std::string::npos)
            s.resize(semi);
        while (!s.empty() && isspace(static_cast<unsigned char>(s.back())))
            s.pop_back();
        return s;
    }

    std::string Describe(const ConfigKey& k)
    {
        std::string s = Name(k) + " = " + k.value;
        switch (k.source)
        {
        case kFromDefault: s += " (not in comfyatmos.ini: the built-in value)"; break;
        case kFromIni:     s += " (comfyatmos.ini)"; break;
        case kFromTune:
        {
            const std::string ini = IniText(k);
            s += ini.empty() ? " (set by /atmos; not in comfyatmos.ini)" : " (set by /atmos; comfyatmos.ini has " + ini + ")";
            break;
        }
        }
        if (const Control* c = ControlFor(Name(k)))
            s += std::string(". The ") + c->name + " control " + c->where + " sets it";
        return s;
    }

    // A value as the ini would hold it: a number, hex, or on/off (1/0). False when it is none of those.
    bool Value(const std::string& in, std::string& out)
    {
        if (Same(in, "on") || Same(in, "true") || Same(in, "yes"))  { out = "1"; return true; }
        if (Same(in, "off") || Same(in, "false") || Same(in, "no")) { out = "0"; return true; }
        char* end = nullptr;
        if (in.size() > 2 && in[0] == '0' && (in[1] == 'x' || in[1] == 'X'))
            strtoul(in.c_str(), &end, 16);
        else
            strtod(in.c_str(), &end);
        if (!end || *end || end == in.c_str())
            return false;
        out = in;
        return true;
    }

    // Lines of "key = value", comma separated, wrapped to chat lines.
    void Wrap(const std::vector<std::string>& items, const std::string& head, std::vector<std::string>& out)
    {
        std::string line = head;
        for (const std::string& item : items)
        {
            if (line.size() + item.size() + 2 > kLine && line != head)
            {
                out.push_back(line);
                line.clear();
            }
            line += line.empty() || line == head ? item : ", " + item;
        }
        if (!line.empty())
            out.push_back(line);
    }

    // Writes each value into its line of the ini, keeping the spacing and the comment after it. A key
    // missing from the ini is added below the last key of its section; a missing section is added at
    // the end.
    bool SaveToIni(const std::map<std::string, std::string>& values, std::string& error)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, ConfigIniPath(), L"rb") || !f)
        {
            error = "could not open comfyatmos.ini";
            return false;
        }
        std::string text;
        char chunk[4096];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
            text.append(chunk, n);
        fclose(f);

        const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        std::vector<std::string> lines;
        size_t start = 0;
        while (start <= text.size())
        {
            size_t end = text.find('\n', start);
            if (end == std::string::npos)
            {
                if (start < text.size())
                    lines.push_back(text.substr(start));
                break;
            }
            std::string line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(line);
            start = end + 1;
        }

        auto trim = [](std::string s) {
            while (!s.empty() && isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
            while (!s.empty() && isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
            return s;
        };

        for (const auto& kv : values)
        {
            const size_t dot = kv.first.find('.');
            const std::string sec = kv.first.substr(0, dot), key = kv.first.substr(dot + 1);
            int inSection = -1, lastKey = -1;
            bool done = false;
            for (size_t i = 0; i < lines.size() && !done; ++i)
            {
                const std::string t = trim(lines[i]);
                if (!t.empty() && t[0] == '[')
                {
                    if (inSection >= 0)
                        break;                      // the next section: the key is not in this one
                    if (Same(t, "[" + sec + "]"))
                        inSection = static_cast<int>(i), lastKey = static_cast<int>(i);
                    continue;
                }
                if (inSection < 0 || t.empty() || t[0] == ';')
                    continue;
                const size_t eq = lines[i].find('=');
                if (eq == std::string::npos)
                    continue;
                lastKey = static_cast<int>(i);
                if (!Same(trim(lines[i].substr(0, eq)), key))
                    continue;
                // key = <spaces> value <spaces> ; comment
                std::string& line = lines[i];
                size_t v = eq + 1;
                while (v < line.size() && line[v] == ' ') ++v;
                size_t ve = v;
                while (ve < line.size() && !isspace(static_cast<unsigned char>(line[ve])) && line[ve] != ';') ++ve;
                std::string rest = line.substr(ve);
                const int grow = static_cast<int>(kv.second.size()) - static_cast<int>(ve - v);
                if (!rest.empty() && rest.find(';') != std::string::npos)
                {
                    // Keep the comment in its column: take spaces away, or add them, leaving one at least.
                    size_t sp = 0;
                    while (sp < rest.size() && rest[sp] == ' ') ++sp;
                    const int keep = (std::max)(1, static_cast<int>(sp) - grow);
                    rest = std::string(keep, ' ') + rest.substr(sp);
                }
                line = line.substr(0, v) + kv.second + rest;
                done = true;
            }
            if (done)
                continue;
            if (inSection >= 0)
                lines.insert(lines.begin() + lastKey + 1, key + " = " + kv.second);
            else
            {
                lines.push_back("");
                lines.push_back("[" + sec + "]");
                lines.push_back(key + " = " + kv.second);
            }
        }

        std::string out;
        for (size_t i = 0; i < lines.size(); ++i)
            out += lines[i] + eol;
        if (_wfopen_s(&f, ConfigIniPath(), L"wb") || !f)
        {
            error = "could not write comfyatmos.ini";
            return false;
        }
        const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
        fclose(f);
        if (!ok)
            error = "could not write all of comfyatmos.ini";
        return ok;
    }

    void Help(std::vector<std::string>& out)
    {
        out.push_back("/atmos <section>.<key> <value>: set a comfyatmos.ini value now. The key alone will do "
                      "if no other section has it.");
        out.push_back("/atmos <section>.<key>: show one value. /atmos <section>: show a section.");
        out.push_back("/atmos list: the values set this way. /atmos reset: drop them. /atmos save: write "
                      "them into comfyatmos.ini. /atmos probe: log a frame, as F12 does.");
        out.push_back("/atmos reload: read comfyatmos.ini again, as F11 does. /atmos rays, /atmos volume: the sun "
                      "rays or the volumetric light on or off, as Ctrl+F11 and Alt+F11 do.");
        std::vector<std::string> sections;
        for (const ConfigKey& k : ConfigKeys())
        {
            bool seen = false;
            for (const std::string& s : sections)
                seen = seen || s == k.section;
            if (!seen)
                sections.push_back(k.section);
        }
        Wrap(sections, "Sections: ", out);
    }
}

bool TuneWriteIni(const std::map<std::string, std::string>& values, std::string& error)
{
    return SaveToIni(values, error);
}

std::vector<std::string> TuneRun(const std::string& command, bool& reloaded)
{
    reloaded = false;
    std::vector<std::string> out;
    const std::vector<std::string> w = Words(command);
    std::map<std::string, std::string>& over = ConfigOverrides();

    if (w.empty() || Same(w[0], "help"))
    {
        Help(out);
    }
    else if (w.size() == 1 && Same(w[0], "list"))
    {
        if (over.empty())
            out.push_back("Nothing is set with /atmos. comfyatmos.ini applies.");
        else
        {
            std::vector<std::string> items;
            for (const auto& kv : over)
                items.push_back(kv.first + " = " + kv.second);
            Wrap(items, "Set with /atmos: ", out);
        }
    }
    else if (w.size() == 1 && Same(w[0], "probe"))
    {
        ProbeArm();
        out.push_back("The next frame is logged to Logs\\comfyatmos.log, as F12 does.");
    }
    // The key bindings' commands (2026-10-07): what F11, Ctrl+F11 and Alt+F11 do, for a player who binds them.
    else if (w.size() == 1 && Same(w[0], "reload"))
    {
        BenchCancel("/atmos reload", false);
        LoadSettings(ConfigIniPath());
        reloaded = true;
        RaysReload();
        TimeReload();
        out.push_back(over.empty() ? "comfyatmos.ini read again." :
                      "comfyatmos.ini read again. The values set with /atmos stay on top (/atmos list).");
    }
    else if (w.size() == 1 && Same(w[0], "rays"))
    {
        BenchCancel("/atmos rays", true);
        out.push_back(RaysToggle() ? "Sun rays on." : "Sun rays off.");
    }
    else if (w.size() == 1 && Same(w[0], "volume"))
    {
        BenchCancel("/atmos volume", true);
        out.push_back(VolumeToggle() ? "Volumetric light on." : "Volumetric light off.");
    }
    else if (w.size() == 1 && Same(w[0], "bench"))
    {
        BenchArm();
        out.push_back("The benchmark starts on the next frame. Stand still and face the sun.");
    }
    else if ((w.size() == 1 || w.size() == 2) && Same(w[0], "framelog"))
    {
        const double seconds = w.size() == 2 ? atof(w[1].c_str()) : 10.0;
        FrameLogStart(seconds > 0.5 && seconds <= 120.0 ? seconds : 10.0);
        out.push_back("Every frame is timed now. The slowest go to Logs\\comfyatmos.log when it ends.");
    }
    else if (w.size() == 1 && Same(w[0], "reset"))
    {
        const size_t n = over.size();
        over.clear();
        LoadSettings(ConfigIniPath());
        reloaded = true;
        char line[96];
        snprintf(line, sizeof(line), "Dropped %u value%s. comfyatmos.ini applies.", static_cast<unsigned>(n),
                 n == 1 ? "" : "s");
        out.push_back(line);
    }
    else if (w.size() == 1 && Same(w[0], "save"))
    {
        if (over.empty())
            out.push_back("Nothing to save: nothing is set with /atmos.");
        else
        {
            std::string error;
            if (!SaveToIni(over, error))
                out.push_back("Not saved: " + error + ".");
            else
            {
                std::vector<std::string> items;
                for (const auto& kv : over)
                    items.push_back(kv.first + " = " + kv.second);
                over.clear();
                LoadSettings(ConfigIniPath());
                reloaded = true;
                Wrap(items, "Written to comfyatmos.ini: ", out);
            }
        }
    }
    else if (w.size() == 1 && Find(w[0]) < 0 && !Section(w[0]).empty())
    {
        const std::string sec = Section(w[0]);
        std::vector<std::string> items;
        for (const ConfigKey& k : ConfigKeys())
            if (k.section == sec)
                items.push_back(k.key + " = " + k.value + (k.source == kFromTune ? " (/atmos)" : ""));
        Wrap(items, "[" + sec + "] ", out);
    }
    else
    {
        // "<section>.<key> [value]", "<key> [value]" or "<section> <key> [value]".
        size_t next = 1;
        int i = Find(w[0]);
        if (i == -1 && w.size() >= 2 && !Section(w[0]).empty())
        {
            i = Find(Section(w[0]) + "." + w[1]);
            next = 2;
        }
        if (i == -2)
            out.push_back("\"" + w[0] + "\" is in more than one section. Name it as <section>.<key>.");
        else if (i < 0)
            out.push_back("No setting \"" + command + "\". /atmos lists the sections, /atmos <section> "
                          "its settings.");
        else if (w.size() <= next)
            out.push_back(Describe(ConfigKeys()[i]));
        else
        {
            const ConfigKey k = ConfigKeys()[i];
            const std::string name = Name(k);
            std::string value;
            if (const Control* c = ControlFor(name))
                out.push_back(name + " is set by the " + c->name + " control " + c->where + ". Use that.");
            else if (!Value(w[next], value))
                out.push_back("\"" + w[next] + "\" is not a number (or on, off).");
            else
            {
                over[name] = value;
                LoadSettings(ConfigIniPath());
                reloaded = true;
                if (k.section == "time")
                    TimeReload();   // the hour set is shown, also when it equals the last (a test's hour)
                const int j = Find(name);
                const std::string now = j >= 0 ? ConfigKeys()[j].value : value;
                out.push_back(name + " = " + now + " (was " + k.value + "). /atmos save writes it into "
                              "comfyatmos.ini.");
            }
        }
    }

    Log("--- /atmos %s ---", command.c_str());
    for (const std::string& line : out)
        Log("/atmos: %s", line.c_str());
    return out;
}
