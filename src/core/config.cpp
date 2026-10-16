#include "core/config.hpp"

#include "core/log.hpp"
#include "inv/rules.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <toml++/toml.hpp>
#include <windows.h>

namespace core
{

    namespace
    {
        Config g_config;
        // The settings file's own limits: inventory_mod.toml's (the rules) override them while the mod runs, and a save
        // must not write the rules into the settings.
        int g_own_slots = 24;
        int g_own_weapon_slots = 8;
        float g_own_max_weight = 0.f;

        constexpr const char* k_views[] = {"both", "slots", "wheel"};

        std::wstring path_of(const wchar_t* name)
        {
            return std::wstring(module_dir()) + name;
        }

        bool read_file(const std::wstring& path, std::string& out)
        {
            FILE* f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr)
                return false;
            char chunk[512];
            for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0;)
                out.append(chunk, n);
            fclose(f);
            return true;
        }

        int clamp_slots(std::int64_t n)
        {
            return static_cast<int>(std::clamp<std::int64_t>(n, 1, 24));
        }
        int clamp_weapon_slots(std::int64_t n)
        {
            return static_cast<int>(std::clamp<std::int64_t>(n, 0, 8));
        }

        // inventory_mod_settings.toml (see config_save for what is in it).
        bool load_settings(const std::string& text)
        {
            toml::table root;
            try
            {
                root = toml::parse(text);
            }
            catch (const toml::parse_error& error)
            {
                INV_LOG("inventory_mod_settings.toml, line %u: %s (the defaults are used)",
                        static_cast<unsigned>(error.source().begin.line), std::string(error.description()).c_str());
                return false;
            }
            Config& c = g_config;
            const toml::node_view keys = root["keys"];
            c.key_toggle = keys["toggle"].value_or(c.key_toggle);
            c.key_prev = keys["previous"].value_or(c.key_prev);
            c.key_next = keys["next"].value_or(c.key_next);
            c.key_use = keys["use"].value_or(c.key_use);
            c.key_drop = keys["drop"].value_or(c.key_drop);
            c.key_store = keys["store"].value_or(c.key_store);
            c.key_pickup = keys["pickup"].value_or(c.key_pickup);

            const toml::node_view hud = root["hud"];
            c.show_hotbar = hud["show"].value_or(c.show_hotbar);
            c.replace_native = hud["replace_native_panel"].value_or(c.replace_native);
            c.hud_scale = std::clamp(hud["scale"].value_or(static_cast<double>(c.hud_scale)), 0.5, 3.0);
            const std::string view = hud["view"].value_or(std::string(k_views[c.inventory_view]));
            for (int i = 0; i < 3; ++i)
                if (view == k_views[i])
                    c.inventory_view = i;

            c.auto_store = root["play"]["auto_store"].value_or(c.auto_store);

            const toml::node_view limits = root["limits"];
            c.slots = clamp_slots(limits["slots"].value_or(static_cast<std::int64_t>(c.slots)));
            c.weapon_slots = clamp_weapon_slots(limits["weapon_slots"].value_or(static_cast<std::int64_t>(c.weapon_slots)));
            c.max_weight = static_cast<float>(std::max(0.0, limits["max_weight"].value_or(static_cast<double>(c.max_weight))));
            return true;
        }

        // inventory_mod.ini, the settings file of the versions before: read once, then saved as TOML.
        void load_legacy_ini(const std::string& text)
        {
            int layout = 0;
            std::size_t start = 0;
            while (start < text.size())
            {
                std::size_t end = text.find('\n', start);
                if (end == std::string::npos)
                    end = text.size();
                std::string line = text.substr(start, end - start);
                start = end + 1;
                const std::size_t eq = line.find('=');
                if (eq == std::string::npos)
                    continue;
                const std::string key = line.substr(0, eq);
                const char* value = line.c_str() + eq + 1;
                const float number = static_cast<float>(atof(value));
                const int integer = atoi(value);
                Config& c = g_config;
                if (key == "key_toggle")
                    c.key_toggle = integer;
                else if (key == "key_prev")
                    c.key_prev = integer;
                else if (key == "key_next")
                    c.key_next = integer;
                else if (key == "key_use")
                    c.key_use = integer;
                else if (key == "key_drop")
                    c.key_drop = integer;
                else if (key == "key_store")
                    c.key_store = integer;
                else if (key == "key_pickup")
                    c.key_pickup = integer;
                else if (key == "slots")
                    c.slots = clamp_slots(integer);
                else if (key == "inventory_view")
                    c.inventory_view = integer < 0 || integer > 2 ? 0 : integer;
                else if (key == "weapon_slots")
                    c.weapon_slots = clamp_weapon_slots(integer);
                else if (key == "max_weight")
                    c.max_weight = number < 0.f ? 0.f : number;
                else if (key == "show_hotbar")
                    c.show_hotbar = integer != 0;
                else if (key == "auto_store")
                    c.auto_store = integer != 0;
                else if (key == "hud_scale")
                    c.hud_scale = number < 0.5f ? 0.5f : (number > 3.f ? 3.f : number);
                else if (key == "replace_native")
                    c.replace_native = integer != 0;
                else if (key == "layout")
                    layout = integer;
            }
            if (layout < 2) // files from before the number keys: G was "pick up" and "/" was "drop"
            {
                g_config.key_drop = 'G';
                g_config.key_pickup = 'H';
            }
            if (layout < 3) // files from before the column beside the game's panel, which is the default now
                g_config.replace_native = false;
            if (layout < 4) // files from before the boxes on demand: the weight is the limit, the box counts only a cap
            {
                g_config.slots = std::max(g_config.slots, 24);
                g_config.weapon_slots = std::max(g_config.weapon_slots, 8);
            }
        }
    } // namespace

    Config& config()
    {
        return g_config;
    }

    void config_load()
    {
        std::string text;
        if (read_file(path_of(L"inventory_mod_settings.toml"), text))
        {
            load_settings(text);
            INV_LOG("inventory_mod_settings.toml loaded");
        }
        else if (read_file(path_of(L"inventory_mod.ini"), text))
        {
            load_legacy_ini(text);
            g_own_slots = g_config.slots;
            g_own_weapon_slots = g_config.weapon_slots;
            g_own_max_weight = g_config.max_weight;
            config_save();
            MoveFileExW(path_of(L"inventory_mod.ini").c_str(), path_of(L"inventory_mod.ini.old").c_str(),
                        MOVEFILE_REPLACE_EXISTING);
            INV_LOG("inventory_mod.ini moved into inventory_mod_settings.toml (the old file is inventory_mod.ini.old)");
        }
        else
            INV_LOG("no inventory_mod_settings.toml, using defaults");
        g_own_slots = g_config.slots;
        g_own_weapon_slots = g_config.weapon_slots;
        g_own_max_weight = g_config.max_weight;
        rules_load();
    }

    void rules_load()
    {
        std::string text;
        if (!read_file(path_of(L"inventory_mod.toml"), text))
            return; // no rules file: the settings' limits and the default item weights

        const inv::Rules rules = inv::apply_rules(text);
        for (const std::string& error : rules.errors)
            INV_LOG("inventory_mod.toml: %s", error.c_str());
        if (rules.slots)
            g_config.slots = *rules.slots;
        if (rules.max_weight)
            g_config.max_weight = *rules.max_weight;
        if (rules.weapon_slots)
            g_config.weapon_slots = *rules.weapon_slots;
        g_config.rules_file = true;
        INV_LOG("inventory_mod.toml: %d slots, %d weapon slots, max weight %.1f, %d item settings, %d problems",
                g_config.slots, g_config.weapon_slots, g_config.max_weight, rules.items_changed,
                static_cast<int>(rules.errors.size()));
    }

    void config_save()
    {
        const Config& c = g_config;
        // with a rules file the limits in use are its own: the settings keep theirs
        if (!c.rules_file)
        {
            g_own_slots = c.slots;
            g_own_weapon_slots = c.weapon_slots;
            g_own_max_weight = c.max_weight;
        }
        FILE* f = nullptr;
        if (_wfopen_s(&f, path_of(L"inventory_mod_settings.toml").c_str(), L"w") != 0 || f == nullptr)
        {
            INV_LOG("cannot write inventory_mod_settings.toml");
            return;
        }
        const auto key = [&](const char* name, int vk) {
            fprintf(f, "%-9s= %-4d # %s\n", name, vk, key_name(vk).c_str());
        };
        fprintf(f, "# The inventory mod's settings.  The mod writes this file when something changes in its Settings tab;\n"
                   "# edit it with the game closed (or unload the mod first).  The rules (weight limit, item weights) are\n"
                   "# in inventory_mod.toml.\n\n");
        fprintf(f, "[keys]   # Windows virtual-key codes (the Settings tab rebinds them)\n");
        key("toggle", c.key_toggle);
        key("previous", c.key_prev);
        key("next", c.key_next);
        key("use", c.key_use);
        key("drop", c.key_drop);
        key("store", c.key_store);
        key("pickup", c.key_pickup);
        fprintf(f, "\n[hud]\n");
        fprintf(f, "show = %s                    # the boxes under the game's panel (or the hotbar)\n",
                c.show_hotbar ? "true" : "false");
        fprintf(f, "view = \"%s\"                  # \"both\": boxes and the wheel, \"slots\": boxes only, \"wheel\": the wheel only\n",
                k_views[std::clamp(c.inventory_view, 0, 2)]);
        fprintf(f, "replace_native_panel = %s   # true: the mod's hotbar instead of the game's weapon panel\n",
                c.replace_native ? "true" : "false");
        fprintf(f, "scale = %.2f                   # the hotbar's size\n", c.hud_scale);
        fprintf(f, "\n[play]\n");
        fprintf(f, "auto_store = %s   # E on an item always stores it (otherwise only when its game slot is full)\n",
                c.auto_store ? "true" : "false");
        fprintf(f, "\n[limits]   # when you play alone or host and there is no inventory_mod.toml\n");
        fprintf(f, "slots = %d          # the most item boxes\n", g_own_slots);
        fprintf(f, "weapon_slots = %d    # the most weapon boxes\n", g_own_weapon_slots);
        fprintf(f, "max_weight = %.1f   # 0: no weight limit\n", g_own_max_weight);
        fclose(f);
    }

    std::string inventory_file_load()
    {
        std::string text;
        read_file(path_of(L"inventory_mod.inventory"), text);
        return text;
    }

    void inventory_file_save(const std::string& text)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path_of(L"inventory_mod.inventory").c_str(), L"wb") != 0 || f == nullptr)
            return;
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }

    std::string key_name(int vk)
    {
        if (vk <= 0)
            return "None";
        const UINT scan = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
        char name[64] = "";
        if (scan != 0 && GetKeyNameTextA(static_cast<LONG>(scan << 16), name, sizeof(name)) > 0)
            return name;
        char fallback[16];
        snprintf(fallback, sizeof(fallback), "0x%02X", vk);
        return fallback;
    }

} // namespace core
