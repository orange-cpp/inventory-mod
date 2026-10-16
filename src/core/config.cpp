#include "core/config.hpp"

#include "core/log.hpp"
#include "inv/rules.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <windows.h>

namespace core
{

    namespace
    {
        Config g_config;

        std::wstring ini_path()
        {
            return std::wstring(module_dir()) + L"inventory_mod.ini";
        }
    } // namespace

    Config& config()
    {
        return g_config;
    }

    void config_load()
    {
        const std::wstring path = ini_path();
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"r") != 0 || f == nullptr)
        {
            INV_LOG("no inventory_mod.ini, using defaults");
            return;
        }
        char line[256];
        int layout = 0;
        while (fgets(line, sizeof(line), f) != nullptr)
        {
            char* eq = strchr(line, '=');
            if (eq == nullptr)
                continue;
            *eq = 0;
            const char* key = line;
            const char* value = eq + 1;
            const float number = static_cast<float>(atof(value));
            const int integer = atoi(value);
            if (!strcmp(key, "key_toggle"))
                g_config.key_toggle = integer;
            else if (!strcmp(key, "key_prev"))
                g_config.key_prev = integer;
            else if (!strcmp(key, "key_next"))
                g_config.key_next = integer;
            else if (!strcmp(key, "key_use"))
                g_config.key_use = integer;
            else if (!strcmp(key, "key_drop"))
                g_config.key_drop = integer;
            else if (!strcmp(key, "key_store"))
                g_config.key_store = integer;
            else if (!strcmp(key, "key_pickup"))
                g_config.key_pickup = integer;
            else if (!strcmp(key, "slots"))
                g_config.slots = integer < 1 ? 1 : (integer > 24 ? 24 : integer);
            else if (!strcmp(key, "max_weight"))
                g_config.max_weight = number < 0.f ? 0.f : number;
            else if (!strcmp(key, "show_hotbar"))
                g_config.show_hotbar = integer != 0;
            else if (!strcmp(key, "auto_store"))
                g_config.auto_store = integer != 0;
            else if (!strcmp(key, "hud_scale"))
                g_config.hud_scale = number < 0.5f ? 0.5f : (number > 3.f ? 3.f : number);
            else if (!strcmp(key, "replace_native"))
                g_config.replace_native = integer != 0;
            else if (!strcmp(key, "layout"))
                layout = integer;
        }
        fclose(f);
        if (layout < 2) // files from before the number keys: G was "pick up" and "/" was "drop"
        {
            g_config.key_drop = 'G';
            g_config.key_pickup = 'H';
        }
        if (layout < 3) // files from before the column beside the game's panel, which is the default now
        {
            g_config.replace_native = false;
            config_save();
        }
        INV_LOG("config loaded");
        rules_load();
    }

    void rules_load()
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, (std::wstring(module_dir()) + L"inventory_mod.toml").c_str(), L"rb") != 0 || f == nullptr)
            return; // no rules file: the ini's limits and the default item weights
        std::string text;
        char chunk[512];
        for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0;)
            text.append(chunk, n);
        fclose(f);

        const inv::Rules rules = inv::apply_rules(text);
        for (const std::string& error : rules.errors)
            INV_LOG("inventory_mod.toml: %s", error.c_str());
        if (rules.slots)
            g_config.slots = *rules.slots;
        if (rules.max_weight)
            g_config.max_weight = *rules.max_weight;
        g_config.rules_file = true;
        INV_LOG("inventory_mod.toml: %d slots, max weight %.1f, %d item settings, %d problems", g_config.slots,
                g_config.max_weight, rules.items_changed, static_cast<int>(rules.errors.size()));
    }

    void config_save()
    {
        const std::wstring path = ini_path();
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"w") != 0 || f == nullptr)
        {
            INV_LOG("cannot write inventory_mod.ini");
            return;
        }
        fprintf(f, "key_toggle=%d\nkey_prev=%d\nkey_next=%d\nkey_use=%d\nkey_drop=%d\nkey_store=%d\nkey_pickup=%d\n",
                g_config.key_toggle, g_config.key_prev, g_config.key_next, g_config.key_use, g_config.key_drop,
                g_config.key_store, g_config.key_pickup);
        fprintf(f, "slots=%d\nmax_weight=%.2f\nshow_hotbar=%d\nauto_store=%d\nhud_scale=%.2f\nreplace_native=%d\nlayout=3\n",
                g_config.slots, g_config.max_weight, g_config.show_hotbar ? 1 : 0, g_config.auto_store ? 1 : 0,
                g_config.hud_scale, g_config.replace_native ? 1 : 0);
        fclose(f);
    }

    std::string inventory_file_load()
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, (std::wstring(module_dir()) + L"inventory_mod.inventory").c_str(), L"rb") != 0 || f == nullptr)
            return {};
        std::string text;
        char chunk[512];
        for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0;)
            text.append(chunk, n);
        fclose(f);
        return text;
    }

    void inventory_file_save(const std::string& text)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, (std::wstring(module_dir()) + L"inventory_mod.inventory").c_str(), L"wb") != 0 || f == nullptr)
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
