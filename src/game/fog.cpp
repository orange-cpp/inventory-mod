// A fog over the map (the Debug tab): see fog.hpp.

#include "game/fog.hpp"

#include "core/log.hpp"
#include "game/client_remote.hpp"
#include "game/entities.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

namespace game::detail
{

    using namespace sdk;

    // ---- the fog --------------------------------------------------------------------------------------------------
    //
    // client.dll registers the fog ConVars as static initializers (`push 0x4000; push "<default>"; push "<name>";
    // mov ecx, <ConVar>; call ConVar::ConVar`; 0x4000 is FCVAR_CHEAT; fog_override at 0x105084b0 -> ConVar 0x107A5780,
    // fog_start 0x107A57C8, fog_end 0x107A5810, fog_color, fog_enable, fog_maxdensity ...).  fog_override 1 makes
    // the client draw the ConVars' fog (fog_enable, fog_start, fog_end in units, fog_color "r g b", fog_maxdensity
    // 0 .. 1) in place of the map's env_fog_controller; fog_override 0 gives the map's own back.  The engine does
    // the rest per pixel from the depth, so it is a real fog in the 3D world, not an overlay.  The commands are
    // cheats: the console refuses them while sv_cheats is off, so the flag is taken off (ConCommandBase::m_nFlags at
    // +0x14, and the parent ConVar's at +0x1C, as unlock_convar in players.cpp does for the ammo maxima).

    namespace
    {
        constexpr int k_convar_cheat = 0x4000;
        constexpr const char* k_names[] = {"fog_override",   "fog_enable",         "fog_start",        "fog_end",
                                           "fog_color",      "fog_maxdensity",     "fog_enableskybox", "fog_startskybox",
                                           "fog_endskybox",  "fog_colorskybox",    "fog_maxdensityskybox"};
        std::uintptr_t g_color_convar = 0; // fog_color: the map's colour, read to give the sky's the same
        bool g_found = false;
        bool g_on = false;

        // The ConVar `name` of client.dll, found by its registration; 0 when not found.
        std::uintptr_t find_convar(const mem::ModuleInfo& module, const char* name)
        {
            std::string text_pattern;
            char hex[4];
            for (const char* c = name;; ++c)
            {
                std::snprintf(hex, sizeof(hex), "%02X ", static_cast<unsigned char>(*c));
                text_pattern += hex;
                if (*c == '\0')
                    break;
            }
            const std::uintptr_t text = mem::find_pattern(module.base, module.size, text_pattern.c_str());
            if (text == 0)
                return 0;
            char code[64];
            std::snprintf(code, sizeof(code), "68 %02X %02X %02X %02X B9", static_cast<unsigned>(text & 0xFF),
                          static_cast<unsigned>((text >> 8) & 0xFF), static_cast<unsigned>((text >> 16) & 0xFF),
                          static_cast<unsigned>((text >> 24) & 0xFF));
            const std::uintptr_t push = mem::find_pattern(module.base, module.size, code);
            return push != 0 ? mem::read_or<std::uintptr_t>(push + 6) : 0;
        }

        void unlock(std::uintptr_t convar)
        {
            mem::guarded([&] {
                *reinterpret_cast<int*>(convar + 0x14) &= ~k_convar_cheat;
                if (const auto parent = mem::read_or<std::uintptr_t>(convar + 0x1C); parent != 0 && parent != convar)
                    *reinterpret_cast<int*>(parent + 0x14) &= ~k_convar_cheat;
            });
        }

        void command(const char* text)
        {
            if (g_client_cmd != nullptr && g_engine_client != nullptr)
                mem::guarded([&] { g_client_cmd(g_engine_client, text); });
        }
    } // namespace

    void install_client_fog(const mem::ModuleInfo& client)
    {
        g_found = true;
        for (const char* name : k_names)
        {
            const std::uintptr_t convar = find_convar(client, name);
            if (convar == 0)
            {
                INV_LOG("fog: %s not found in client.dll: the Debug tab's fog is off", name);
                g_found = false;
                continue;
            }
            unlock(convar);
            if (std::string_view(name) == "fog_color")
                g_color_convar = convar;
        }
        if (g_found)
            INV_LOG("fog: the fog ConVars are unlocked");
    }

    // The map's own colour: fog_override -1 copies the map's fog (its env_fog_controller's, as the client has it) into
    // the ConVars, on the next frame; only then are the ConVars switched on, with the map's colour left as it is and
    // the distances and density the Debug tab's.
    namespace
    {
        ULONGLONG g_apply_at = 0; // when the map's values are in the ConVars (0: nothing waits)
        float g_density = 0.f;

        // fog_color's value, "r g b" (the map's, once fog_override -1 has copied it): the value lives in the parent ConVar
        // (m_pParent +0x1C), the string at +0x24 (m_pszString).
        std::string map_color()
        {
            char text[48] = {};
            if (g_color_convar != 0)
                mem::guarded([&] {
                    std::uintptr_t convar = g_color_convar;
                    if (const auto parent = mem::read_or<std::uintptr_t>(convar + 0x1C); parent != 0)
                        convar = parent;
                    const char* value = mem::read_or<const char*>(convar + 0x24);
                    if (value != nullptr)
                        mem::safe_copy(text, value, sizeof(text) - 1);
                });
            return text;
        }

        void apply()
        {
            // the thicker, the nearer it starts and the sooner it is solid: 0 -> clear to 300 units, 1 -> clear to
            // 10 and solid at 200
            const float d = std::clamp(g_density, 0.f, 1.f);
            const float density = 1.f;
            const int start = static_cast<int>(300.f - 290.f * d), end = static_cast<int>(1200.f - 1000.f * d);
            char text[384];
            std::snprintf(text, sizeof(text), "fog_override 1;fog_enable 1;fog_maxdensity %.2f;fog_start %d;fog_end %d\n", density, start,
                          end);
            command(text);
            // The sky has ConVars of its own, and fog_override -1 does not always fill them (a sky left with its default
            // black fog was seen: a black sky over the fogged map).  It gets the map's colour and the same fog; the 3D
            // sky's units are 1/16 of the world's.
            const std::string color = map_color();
            if (!color.empty())
            {
                std::snprintf(text, sizeof(text),
                              "fog_enableskybox 1;fog_colorskybox \"%s\";fog_maxdensityskybox %.2f;fog_startskybox %d;fog_endskybox %d\n",
                              color.c_str(), density, start / 16, end / 16);
                command(text);
            }
        }
    } // namespace

    void set_fog(bool on, float density)
    {
        if (!g_found)
            return;
        g_density = density;
        if (!on)
        {
            if (g_on)
                command("fog_override 0\n");
            g_on = false;
            g_apply_at = 0;
            return;
        }
        if (!g_on)
        {
            command("fog_override -1\n");
            g_apply_at = GetTickCount64() + 300;
            g_on = true;
        }
        else if (g_apply_at == 0)
            apply(); // (the slider)
    }

    void fog_tick()
    {
        if (g_apply_at != 0 && GetTickCount64() >= g_apply_at)
        {
            g_apply_at = 0;
            apply();
        }
    }

} // namespace game::detail
