#include "inv/rules.hpp"

#include "inv/items.hpp"
#include "inv/melee.hpp"
#include "inv/mutators.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <cstdio>
#include <toml++/toml.hpp>

namespace inv
{

    namespace
    {
        std::string at_line(const toml::node& node, const std::string& what)
        {
            char line[32];
            std::snprintf(line, sizeof(line), "line %u: ", static_cast<unsigned>(node.source().begin.line));
            return line + what;
        }

        // "first_aid_kit" and "weapon_first_aid_kit" both name weapon_first_aid_kit
        std::string classname_of(std::string_view key)
        {
            if (key.starts_with("weapon_"))
                return std::string(key);
            return "weapon_" + std::string(key);
        }

        // An integer or a float as a float (TOML keeps them apart: `1` is an integer).
        std::optional<double> number(const toml::node& node)
        {
            if (const auto f = node.value<double>())
                return f;
            return std::nullopt;
        }

        InfectionRules g_infection;

        // [infection] (see rules.hpp)
        void read_infection(const toml::table& table, Rules& rules)
        {
            InfectionRules infection;
            infection.enabled = true; // a table at all switches it on, unless it says otherwise
            for (const auto& [key, node] : table)
            {
                if (key == "enabled")
                {
                    if (const auto on = node.value<bool>())
                        infection.enabled = *on;
                    else
                        rules.errors.push_back(at_line(node, "infection.enabled must be true or false"));
                }
                else if (key == "seconds" || key == "hit_seconds")
                {
                    const auto n = node.value<std::int64_t>();
                    if (!n || *n < 0 || *n > 3600)
                        rules.errors.push_back(at_line(node, "infection." + std::string(key.str()) + " must be a whole number from 0 to 3600"));
                    else if (key == "seconds")
                        infection.seconds = std::max(1, static_cast<int>(*n));
                    else
                        infection.hit_seconds = static_cast<int>(*n);
                }
                else if (key == "insane_at")
                {
                    const auto n = node.value_exact<std::int64_t>();
                    if (!n || *n < 0 || *n > 100)
                        rules.errors.push_back(at_line(node, "infection.insane_at must be a whole number from 0 to 100 (% of the clock)"));
                    else
                        infection.insane_at = static_cast<int>(*n);
                }
                else if (key == "bots" || key == "no_cure" || key == "cough" || key == "insane")
                {
                    if (const auto on = node.value_exact<bool>()) // (value<bool> would take 3 as true)
                        (key == "bots"      ? infection.bots
                         : key == "no_cure" ? infection.no_cure
                         : key == "cough"   ? infection.cough
                                            : infection.insane) = *on;
                    else
                        rules.errors.push_back(at_line(node, "infection." + std::string(key.str()) + " must be true or false"));
                }
                else if (key == "specials")
                {
                    static constexpr const char* k_names[] = {"smoker", "boomer", "hunter", "spitter", "jockey", "charger"};
                    std::vector<int> specials;
                    if (const toml::array* list = node.as_array())
                        for (const toml::node& item : *list)
                        {
                            const std::string name = item.value_or(std::string());
                            int found = 0;
                            for (int i = 0; i < 6; ++i)
                                if (name == k_names[i])
                                    found = i + 1;
                            if (found != 0)
                                specials.push_back(found);
                            else
                                rules.errors.push_back(at_line(item, "infection.specials: unknown special \"" + name +
                                                                         "\" (smoker, boomer, hunter, spitter, jockey, charger)"));
                        }
                    else
                        rules.errors.push_back(at_line(node, "infection.specials must be a list, like [\"hunter\", \"jockey\"]"));
                    if (!specials.empty())
                        infection.specials = specials;
                }
                else
                    rules.errors.push_back(at_line(node, "unknown setting infection." + std::string(key.str())));
            }
            g_infection = infection;
        }

        StaminaRules g_stamina;

        // [stamina] (see rules.hpp)
        void read_stamina(const toml::table& table, Rules& rules)
        {
            StaminaRules stamina;
            stamina.enabled = true; // a table at all switches it on, unless it says otherwise
            struct Number
            {
                const char* key;
                float StaminaRules::*field;
                double low, high;
            };
            static constexpr Number k_numbers[] = {
                {"run", &StaminaRules::run, 0, 100},           {"jump", &StaminaRules::jump, 0, 100},
                {"swing", &StaminaRules::swing, 0, 100},       {"shove", &StaminaRules::shove, 0, 100},
                {"regen", &StaminaRules::regen, 0, 1000},      {"regen_delay", &StaminaRules::regen_delay, 0, 60},
                {"recover", &StaminaRules::recover, 0, 100},   {"slow", &StaminaRules::slow, 10, 100},
                {"swing_speed", &StaminaRules::swing_speed, 10, 100}, {"damage", &StaminaRules::damage, 0, 100},
            };
            for (const auto& [key, node] : table)
            {
                if (key == "enabled" || key == "bots")
                {
                    if (const auto on = node.value_exact<bool>())
                        (key == "enabled" ? stamina.enabled : stamina.bots) = *on;
                    else
                        rules.errors.push_back(at_line(node, "stamina." + std::string(key.str()) + " must be true or false"));
                    continue;
                }
                const Number* found = nullptr;
                for (const Number& n : k_numbers)
                    if (key == n.key)
                        found = &n;
                if (found == nullptr)
                {
                    rules.errors.push_back(at_line(node, "unknown setting stamina." + std::string(key.str())));
                    continue;
                }
                const auto value = node.is_boolean() ? std::nullopt : number(node);
                char range[64];
                std::snprintf(range, sizeof(range), " must be a number from %g to %g", found->low, found->high);
                if (!value || *value < found->low || *value > found->high)
                    rules.errors.push_back(at_line(node, "stamina." + std::string(key.str()) + range));
                else
                    stamina.*found->field = static_cast<float>(*value);
            }
            g_stamina = stamina;
        }

        MeleeRules g_melee;

        void read_melee_weapon(const std::string& name, const toml::table& table, MeleeWeaponRules& out, Rules& rules)
        {
            for (const auto& [key, node] : table)
            {
                const std::string where = "melee.weapons." + name + "." + std::string(key.str());
                if (key == "class")
                {
                    MeleeClass c;
                    if (const auto text = node.value_exact<std::string>(); text && melee_class_from_name(*text, c))
                        out.klass = static_cast<MeleeClassId>(c);
                    else
                        rules.errors.push_back(at_line(node, where + " must be \"blade_1h\", \"blunt_1h\", \"blade_2h\" or \"blunt_2h\""));
                    continue;
                }
                if (key == "durability")
                {
                    const auto n = node.value_exact<std::int64_t>();
                    if (!n || *n < 0 || *n > 100000)
                        rules.errors.push_back(at_line(node, where + " must be a whole number of hits, 0 or more (0: never breaks)"));
                    else
                        out.durability = static_cast<int>(*n);
                    continue;
                }
                struct Number
                {
                    const char* key;
                    std::optional<float> MeleeWeaponRules::*field;
                    double low, high;
                };
                static constexpr Number k_numbers[] = {
                    {"heavy_hold", &MeleeWeaponRules::heavy_hold, 0.1, 3},
                    {"light_damage", &MeleeWeaponRules::light_damage, 0, 1000},
                    {"heavy_damage", &MeleeWeaponRules::heavy_damage, 0, 1000},
                    {"cleave", &MeleeWeaponRules::cleave, 0, 100},
                    {"stamina", &MeleeWeaponRules::stamina, 0, 1000},
                };
                const Number* found = nullptr;
                for (const Number& n : k_numbers)
                    if (key == n.key)
                        found = &n;
                const auto value = node.is_boolean() ? std::nullopt : number(node);
                if (found == nullptr)
                    rules.errors.push_back(at_line(node, "unknown setting " + where));
                else if (!value || *value < found->low || *value > found->high)
                {
                    char range[64];
                    std::snprintf(range, sizeof(range), " must be a number from %g to %g", found->low, found->high);
                    rules.errors.push_back(at_line(node, where + range));
                }
                else
                    out.*found->field = static_cast<float>(*value);
            }
        }

        // [melee] (see rules.hpp)
        void read_melee(const toml::table& table, Rules& rules)
        {
            MeleeRules melee;
            melee.enabled = true; // a table at all switches it on, unless it says otherwise
            struct Number
            {
                const char* key;
                float MeleeRules::*field;
                double low, high;
            };
            static constexpr Number k_numbers[] = {
                {"heavy_hold", &MeleeRules::heavy_hold, 0.1, 3},
                {"heavy_damage", &MeleeRules::heavy_damage, 0, 1000},
                {"heavy_recovery", &MeleeRules::heavy_recovery, 50, 400},
                {"heavy_stamina", &MeleeRules::heavy_stamina, 0, 100},
                {"light_damage", &MeleeRules::light_damage, 0, 1000},
                {"head_damage", &MeleeRules::head_damage, 0, 1000},
                {"cleave", &MeleeRules::cleave, 0, 100},
                {"cleave_min", &MeleeRules::cleave_min, 0, 100},
                {"wind_speed", &MeleeRules::wind_speed, 10, 100},
                {"wind_stamina", &MeleeRules::wind_stamina, 0, 100},
                {"hit_stamina", &MeleeRules::hit_stamina, 0, 100},
                {"durability", &MeleeRules::durability, 0, 1000},
                {"block", &MeleeRules::block, 0, 100},
                {"block_stamina", &MeleeRules::block_stamina, 0, 100},
                {"block_speed", &MeleeRules::block_speed, 10, 100},
                {"deflect", &MeleeRules::deflect, 0, 2},
                {"heavy_shove_hold", &MeleeRules::heavy_shove_hold, 0, 3},
                {"heavy_shove_stamina", &MeleeRules::heavy_shove_stamina, 0, 100},
                {"throw_damage", &MeleeRules::throw_damage, 0, 1000},
                {"throw_speed", &MeleeRules::throw_speed, 100, 3000},
                {"view_punch", &MeleeRules::view_punch, 0, 30},
            };
            struct Flag
            {
                const char* key;
                bool MeleeRules::*field;
            };
            static constexpr Flag k_flags[] = {
                {"enabled", &MeleeRules::enabled},         {"head_only", &MeleeRules::head_only},
                {"wall_bounce", &MeleeRules::wall_bounce}, {"overhead", &MeleeRules::overhead},
                {"gesture", &MeleeRules::gesture},         {"heavy", &MeleeRules::heavy},
                {"wind_slowdown", &MeleeRules::wind_slowdown}, {"blocking", &MeleeRules::blocking},
                {"deflecting", &MeleeRules::deflecting},   {"heavy_shove", &MeleeRules::heavy_shove},
                {"throwing", &MeleeRules::throwing},       {"breaking", &MeleeRules::breaking},
                {"hit_stagger", &MeleeRules::hit_stagger}, {"view_kick", &MeleeRules::view_kick},
            };
            for (const auto& [key, node] : table)
            {
                if (key == "weapons")
                {
                    if (const toml::table* weapons = node.as_table())
                        for (const auto& [name, entry] : *weapons)
                        {
                            if (const toml::table* one = entry.as_table())
                                read_melee_weapon(std::string(name.str()), *one, melee.weapons[std::string(name.str())], rules);
                            else
                                rules.errors.push_back(at_line(entry, "melee.weapons." + std::string(name.str()) +
                                                                          " must be a table, like { class = \"blade_2h\" }"));
                        }
                    else
                        rules.errors.push_back(at_line(node, "melee.weapons must be a table of weapons"));
                    continue;
                }
                const Flag* flag = nullptr;
                for (const Flag& f : k_flags)
                    if (key == f.key)
                        flag = &f;
                if (flag != nullptr)
                {
                    if (const auto on = node.value_exact<bool>())
                        melee.*flag->field = *on;
                    else
                        rules.errors.push_back(at_line(node, "melee." + std::string(key.str()) + " must be true or false"));
                    continue;
                }
                const Number* found = nullptr;
                for (const Number& n : k_numbers)
                    if (key == n.key)
                        found = &n;
                if (found == nullptr)
                {
                    rules.errors.push_back(at_line(node, "unknown setting melee." + std::string(key.str())));
                    continue;
                }
                const auto value = node.is_boolean() ? std::nullopt : number(node);
                char range[64];
                std::snprintf(range, sizeof(range), " must be a number from %g to %g", found->low, found->high);
                if (!value || *value < found->low || *value > found->high)
                    rules.errors.push_back(at_line(node, "melee." + std::string(key.str()) + range));
                else
                    melee.*found->field = static_cast<float>(*value);
            }
            g_melee = melee;
        }

        AmmoPileRules g_ammo_piles;

        // [ammo_piles] (see rules.hpp)
        void read_ammo_piles(const toml::table& table, Rules& rules)
        {
            AmmoPileRules piles;
            piles.enabled = true; // a table at all switches it on, unless it says otherwise
            for (const auto& [key, node] : table)
            {
                if (key == "enabled" || key == "replace_map_piles")
                {
                    if (const auto on = node.value_exact<bool>())
                        (key == "enabled" ? piles.enabled : piles.replace_map_piles) = *on;
                    else
                        rules.errors.push_back(at_line(node, "ammo_piles." + std::string(key.str()) + " must be true or false"));
                }
                else if (key == "extra")
                {
                    const auto n = node.value_exact<std::int64_t>();
                    if (!n || *n < 0 || *n > 50)
                        rules.errors.push_back(at_line(node, "ammo_piles.extra must be a whole number from 0 to 50"));
                    else
                        piles.extra = static_cast<int>(*n);
                }
                else if (key == "min_magazines" || key == "max_magazines")
                {
                    const auto n = node.value_exact<std::int64_t>();
                    if (!n || *n < 1 || *n > 50)
                        rules.errors.push_back(at_line(node, "ammo_piles." + std::string(key.str()) + " must be a whole number from 1 to 50"));
                    else
                        (key == "min_magazines" ? piles.min_magazines : piles.max_magazines) = static_cast<int>(*n);
                }
                else if (key == "kinds")
                {
                    std::vector<std::string> kinds;
                    if (const toml::array* list = node.as_array())
                        for (const toml::node& item : *list)
                        {
                            std::string kind = item.value_or(std::string());
                            const std::string name = kind;
                            kind.erase(std::remove(kind.begin(), kind.end(), '_'), kind.end());
                            if (ammo_index(kind) >= 0)
                                kinds.push_back(kind);
                            else
                                rules.errors.push_back(at_line(item, "ammo_piles.kinds: unknown ammo type \"" + name +
                                                                         "\" (assault_rifle, smg, shotgun, hunting_rifle, "
                                                                         "sniper_rifle, grenade_launcher, m60, chainsaw)"));
                        }
                    else
                        rules.errors.push_back(at_line(node, "ammo_piles.kinds must be a list, like [\"smg\", \"shotgun\"]"));
                    if (!kinds.empty())
                        piles.kinds = kinds;
                }
                else
                    rules.errors.push_back(at_line(node, "unknown setting ammo_piles." + std::string(key.str())));
            }
            if (piles.max_magazines < piles.min_magazines)
            {
                rules.errors.push_back("ammo_piles: max_magazines is less than min_magazines (taken as the same)");
                piles.max_magazines = piles.min_magazines;
            }
            g_ammo_piles = piles;
        }

        PingRules g_ping;

        // [ping] (see rules.hpp)
        void read_ping(const toml::table& table, Rules& rules)
        {
            PingRules ping;
            ping.enabled = true; // a table at all switches it on, unless it says otherwise
            for (const auto& [key, node] : table)
            {
                if (key == "enabled")
                {
                    if (const auto on = node.value_exact<bool>())
                        ping.enabled = *on;
                    else
                        rules.errors.push_back(at_line(node, "ping.enabled must be true or false"));
                }
                else if (key == "per_player")
                {
                    const auto n = node.value_exact<std::int64_t>();
                    if (!n || *n < 1 || *n > 8)
                        rules.errors.push_back(at_line(node, "ping.per_player must be a whole number from 1 to 8"));
                    else
                        ping.per_player = static_cast<int>(*n);
                }
                else if (key == "lifetime" || key == "cooldown" || key == "range")
                {
                    const double low = key == "lifetime" ? 1.0 : key == "cooldown" ? 0.0 : 100.0;
                    const double high = key == "lifetime" ? 60.0 : key == "cooldown" ? 10.0 : 20000.0;
                    const auto value = node.is_boolean() ? std::nullopt : number(node);
                    if (!value || *value < low || *value > high)
                    {
                        char range[96];
                        std::snprintf(range, sizeof(range), " must be a number from %g to %g", low, high);
                        rules.errors.push_back(at_line(node, "ping." + std::string(key.str()) + range));
                    }
                    else
                        (key == "lifetime" ? ping.lifetime : key == "cooldown" ? ping.cooldown : ping.range) = static_cast<float>(*value);
                }
                else
                    rules.errors.push_back(at_line(node, "unknown setting ping." + std::string(key.str())));
            }
            g_ping = ping;
        }

        MutatorRules g_mutators;

        // A list of mutator names (pool, a map's list); false (and an error) when it is not one.
        bool read_mutator_list(const toml::node& node, const std::string& what, std::vector<std::string>& out, Rules& rules)
        {
            const toml::array* list = node.as_array();
            if (list == nullptr)
            {
                rules.errors.push_back(at_line(node, what + " must be a list, like [\"fog\"]"));
                return false;
            }
            std::vector<std::string> ids;
            for (const toml::node& entry : *list)
            {
                const auto id = entry.value_exact<std::string>();
                if (!id || find_mutator(*id) == nullptr)
                {
                    std::string known;
                    for (const MutatorInfo& m : mutators())
                        known += (known.empty() ? "" : ", ") + std::string(m.id);
                    rules.errors.push_back(at_line(entry, what + ": unknown mutator (" + known + ")"));
                    return false;
                }
                if (std::find(ids.begin(), ids.end(), *id) == ids.end())
                    ids.push_back(*id);
            }
            out = std::move(ids);
            return true;
        }

        // "r g b", each 0 .. 255
        bool is_color(const std::string& text)
        {
            int r = -1, g = -1, b = -1;
            char rest = 0;
            return std::sscanf(text.c_str(), "%d %d %d %c", &r, &g, &b, &rest) == 3 && r >= 0 && r <= 255 && g >= 0 &&
                   g <= 255 && b >= 0 && b <= 255;
        }

        // [mutators.fog]
        void read_fog_mutator(const toml::table& table, FogMutatorRules& fog, Rules& rules)
        {
            for (const auto& [key, node] : table)
            {
                if (key == "density")
                {
                    const auto value = node.is_boolean() ? std::nullopt : number(node);
                    if (!value || *value < 0.0 || *value > 1.0)
                        rules.errors.push_back(at_line(node, "mutators.fog.density must be a number from 0 to 1"));
                    else
                        fog.density = static_cast<float>(*value);
                }
                else if (key == "color")
                {
                    const auto text = node.value_exact<std::string>();
                    if (!text || (!text->empty() && !is_color(*text)))
                        rules.errors.push_back(at_line(node, "mutators.fog.color must be \"r g b\" (0 to 255 each), or \"\" for the map's own"));
                    else
                        fog.color = *text;
                }
                else
                    rules.errors.push_back(at_line(node, "unknown setting mutators.fog." + std::string(key.str())));
            }
        }

        // A mutator's whole-number settings ([mutators.scarcity] ...), each within its range.
        struct MutatorNumber
        {
            const char* key;
            int low, high;
            int* out;
        };
        void read_mutator_numbers(const toml::table& table, const char* id, std::initializer_list<MutatorNumber> numbers,
                                  Rules& rules)
        {
            for (const auto& [key, node] : table)
            {
                const MutatorNumber* setting = nullptr;
                for (const MutatorNumber& n : numbers)
                    if (key == n.key)
                        setting = &n;
                const std::string name = "mutators." + std::string(id) + "." + std::string(key.str());
                if (setting == nullptr)
                {
                    rules.errors.push_back(at_line(node, "unknown setting " + name));
                    continue;
                }
                const auto n = node.value_exact<std::int64_t>();
                if (!n || *n < setting->low || *n > setting->high)
                    rules.errors.push_back(at_line(node, name + " must be a whole number from " + std::to_string(setting->low) +
                                                             " to " + std::to_string(setting->high)));
                else
                    *setting->out = static_cast<int>(*n);
            }
        }

        // [mutators] (see rules.hpp)
        void read_mutators(const toml::table& table, Rules& rules)
        {
            MutatorRules m;
            m.enabled = true; // a table at all switches it on, unless it says otherwise
            for (const auto& [key, node] : table)
            {
                if (key == "enabled" || key == "announce")
                {
                    if (const auto on = node.value_exact<bool>())
                        (key == "enabled" ? m.enabled : m.announce) = *on;
                    else
                        rules.errors.push_back(at_line(node, "mutators." + std::string(key.str()) + " must be true or false"));
                }
                else if (key == "chance" || key == "count")
                {
                    const std::int64_t high = key == "chance" ? 100 : 8;
                    const auto n = node.value_exact<std::int64_t>();
                    if (!n || *n < 0 || *n > high)
                        rules.errors.push_back(at_line(node, "mutators." + std::string(key.str()) + " must be a whole number from 0 to " +
                                                                 std::to_string(high)));
                    else
                        (key == "chance" ? m.chance : m.count) = static_cast<int>(*n);
                }
                else if (key == "pool")
                    read_mutator_list(node, "mutators.pool", m.pool, rules);
                else if (key == "maps")
                {
                    if (const toml::table* maps = node.as_table())
                        for (const auto& [map, list] : *maps)
                        {
                            std::vector<std::string> ids;
                            if (read_mutator_list(list, "mutators.maps." + std::string(map.str()), ids, rules))
                                m.maps[std::string(map.str())] = std::move(ids);
                        }
                    else
                        rules.errors.push_back(at_line(node, "mutators.maps must be a table: [mutators.maps]"));
                }
                else if (const MutatorInfo* info = find_mutator(key.str()); info != nullptr && node.is_table())
                {
                    const std::string_view id = info->id;
                    const toml::table& settings = *node.as_table();
                    if (id == "fog")
                        read_fog_mutator(settings, m.fog, rules);
                    else if (id == "scarcity")
                        read_mutator_numbers(settings, "scarcity",
                                             {{"ammo", 1, 100, &m.scarcity.ammo}, {"clip", 0, 100, &m.scarcity.clip},
                                              {"extra", 0, 50, &m.scarcity.extra}},
                                             rules);
                    else if (id == "heavy_load")
                    {
                        int limit = static_cast<int>(m.heavy_load.limit);
                        read_mutator_numbers(settings, "heavy_load",
                                             {{"weight", 10, 100, &m.heavy_load.weight}, {"limit", 1, 1000, &limit},
                                              {"stamina", 100, 500, &m.heavy_load.stamina}},
                                             rules);
                        m.heavy_load.limit = static_cast<float>(limit);
                    }
                    else if (id == "contagion")
                        read_mutator_numbers(settings, "contagion", {{"clock", 10, 100, &m.contagion.clock}}, rules);
                    else if (id == "brittle")
                        read_mutator_numbers(settings, "brittle", {{"wear", 100, 1000, &m.brittle.wear}}, rules);
                    else if (id == "horde_tide")
                        read_mutator_numbers(settings, "horde_tide",
                                             {{"first", 0, 3600, &m.horde_tide.first}, {"every", 30, 3600, &m.horde_tide.every}},
                                             rules);
                    else if (id == "hunted")
                    {
                        // class is a name; the rest are numbers
                        toml::table numbers = settings;
                        if (const toml::node* klass = settings.get("class"))
                        {
                            static constexpr const char* k_classes[] = {"random", "smoker", "boomer", "hunter",
                                                                        "spitter", "jockey", "charger"};
                            const auto name = klass->value_exact<std::string>();
                            int found = -1;
                            for (int i = 0; name && i < 7; ++i)
                                if (*name == k_classes[i])
                                    found = i;
                            if (found < 0)
                                rules.errors.push_back(at_line(*klass, "mutators.hunted.class must be random, smoker, boomer, "
                                                                       "hunter, spitter, jockey or charger"));
                            else
                                m.hunted.klass = found;
                            numbers.erase("class");
                        }
                        read_mutator_numbers(numbers, "hunted", {{"limit", 0, 10, &m.hunted.limit}}, rules);
                    }
                }
                else
                    rules.errors.push_back(at_line(node, "unknown setting mutators." + std::string(key.str())));
            }
            g_mutators = m;
        }

        MapGunRules g_map_guns;

        // [map_guns] (see rules.hpp)
        void read_map_guns(const toml::table& table, Rules& rules)
        {
            MapGunRules guns;
            guns.enabled = true; // a table at all switches it on, unless it says otherwise
            for (const auto& [key, node] : table)
            {
                if (key == "enabled" || key == "spawners_once")
                {
                    if (const auto on = node.value_exact<bool>())
                        (key == "enabled" ? guns.enabled : guns.spawners_once) = *on;
                    else
                        rules.errors.push_back(at_line(node, "map_guns." + std::string(key.str()) + " must be true or false"));
                }
                else if (key == "min_reserve" || key == "max_reserve")
                {
                    const auto n = node.value_exact<std::int64_t>();
                    if (!n || *n < 0 || *n > 20)
                        rules.errors.push_back(at_line(node, "map_guns." + std::string(key.str()) + " must be a whole number from 0 to 20 (magazines)"));
                    else
                        (key == "min_reserve" ? guns.min_reserve : guns.max_reserve) = static_cast<int>(*n);
                }
                else if (key == "min_clip" || key == "max_clip")
                {
                    const auto n = node.value_exact<std::int64_t>();
                    if (!n || *n < 0 || *n > 100)
                        rules.errors.push_back(at_line(node, "map_guns." + std::string(key.str()) + " must be a whole number from 0 to 100 (% of a magazine)"));
                    else
                        (key == "min_clip" ? guns.min_clip : guns.max_clip) = static_cast<int>(*n);
                }
                else
                    rules.errors.push_back(at_line(node, "unknown setting map_guns." + std::string(key.str())));
            }
            if (guns.max_reserve < guns.min_reserve)
            {
                rules.errors.push_back("map_guns: max_reserve is less than min_reserve (taken as the same)");
                guns.max_reserve = guns.min_reserve;
            }
            if (guns.max_clip < guns.min_clip)
            {
                rules.errors.push_back("map_guns: max_clip is less than min_clip (taken as the same)");
                guns.max_clip = guns.min_clip;
            }
            g_map_guns = guns;
        }
        // ---- the chapter's mutators over the host's rules ----------------------------------------------------------
        //
        // The g_* above are the host's rules as the file has them; the g_now_* are what the accessors give: the same,
        // changed by the chapter's mutators.  Made again whenever either changes (apply_rules, set_active_mutators).

        std::vector<std::string> g_active;
        InfectionRules g_now_infection;
        StaminaRules g_now_stamina;
        MeleeRules g_now_melee;
        AmmoPileRules g_now_ammo_piles;
        MapGunRules g_now_map_guns;

        int percent_of(int value, int percent)
        {
            return static_cast<int>(std::lround(static_cast<double>(value) * percent / 100.0));
        }

        void refresh_mutated()
        {
            g_now_infection = g_infection;
            g_now_stamina = g_stamina;
            g_now_melee = g_melee;
            g_now_ammo_piles = g_ammo_piles;
            g_now_map_guns = g_map_guns;
            for (const std::string& id : g_active)
            {
                if (id == "scarcity")
                {
                    const ScarcityMutatorRules& m = g_mutators.scarcity;
                    AmmoPileRules& piles = g_now_ammo_piles;
                    piles.enabled = true;
                    piles.min_magazines = std::max(1, percent_of(piles.min_magazines, m.ammo));
                    piles.max_magazines = std::max(piles.min_magazines, percent_of(piles.max_magazines, m.ammo));
                    piles.extra = m.extra;
                    MapGunRules& guns = g_now_map_guns;
                    guns.enabled = true;
                    guns.min_clip = percent_of(guns.min_clip, m.clip);
                    guns.max_clip = std::max(guns.min_clip, percent_of(guns.max_clip, m.clip));
                    guns.min_reserve = static_cast<int>(guns.min_reserve * m.ammo / 100);
                    guns.max_reserve = std::max(guns.min_reserve, static_cast<int>(guns.max_reserve * m.ammo / 100));
                }
                else if (id == "heavy_load")
                {
                    const float scale = static_cast<float>(g_mutators.heavy_load.stamina) / 100.f;
                    StaminaRules& stamina = g_now_stamina;
                    stamina.enabled = true;
                    stamina.run *= scale;
                    stamina.jump *= scale;
                    stamina.swing *= scale;
                    stamina.shove *= scale;
                }
                else if (id == "contagion")
                {
                    InfectionRules& infection = g_now_infection;
                    infection.enabled = true;
                    infection.seconds = std::max(10, percent_of(infection.seconds, g_mutators.contagion.clock));
                }
                else if (id == "brittle" && g_now_melee.enabled)
                {
                    // durability is what a weapon takes before it breaks: the wear of each hit times wear % is the
                    // durability divided by it
                    const int wear = std::max(1, g_mutators.brittle.wear);
                    g_now_melee.durability = g_now_melee.durability * 100.f / static_cast<float>(wear);
                    for (auto& [name, weapon] : g_now_melee.weapons)
                        if (weapon.durability)
                            weapon.durability = std::max(1, static_cast<int>(std::lround(*weapon.durability * 100.0 / wear)));
                }
            }
        }
    } // namespace

    const MutatorRules& mutator_rules()
    {
        return g_mutators;
    }

    void set_active_mutators(const std::vector<std::string>& ids)
    {
        g_active = ids;
        refresh_mutated();
    }

    const std::vector<std::string>& active_mutators()
    {
        return g_active;
    }

    bool mutator_active(std::string_view id)
    {
        return std::find(g_active.begin(), g_active.end(), id) != g_active.end();
    }

    float mutated_max_weight(float max_weight)
    {
        if (!mutator_active("heavy_load"))
            return max_weight;
        const HeavyLoadMutatorRules& m = g_mutators.heavy_load;
        return max_weight > 0.f ? max_weight * static_cast<float>(m.weight) / 100.f : m.limit;
    }

    const MapGunRules& map_gun_rules()
    {
        return g_now_map_guns;
    }

    const AmmoPileRules& ammo_pile_rules()
    {
        return g_now_ammo_piles;
    }

    const PingRules& ping_rules()
    {
        return g_ping;
    }

    const InfectionRules& infection_rules()
    {
        return g_now_infection;
    }

    const StaminaRules& stamina_rules()
    {
        return g_now_stamina;
    }

    const MeleeRules& melee_rules()
    {
        return g_now_melee;
    }

    Rules apply_rules(std::string_view text)
    {
        Rules rules;
        toml::table root;
        try
        {
            root = toml::parse(text);
        }
        catch (const toml::parse_error& error)
        {
            char line[32];
            std::snprintf(line, sizeof(line), "line %u: ", static_cast<unsigned>(error.source().begin.line));
            rules.errors.push_back(line + std::string(error.description()));
            return rules;
        }

        if (const toml::table* limits = root["limits"].as_table())
        {
            for (const auto& [key, node] : *limits)
            {
                if (key == "slots")
                {
                    const auto slots = node.value<std::int64_t>();
                    if (slots && *slots >= 1 && *slots <= 24)
                        rules.slots = static_cast<int>(*slots);
                    else
                        rules.errors.push_back(at_line(node, "slots must be a whole number from 1 to 24"));
                }
                else if (key == "weapon_slots")
                {
                    const auto slots = node.value<std::int64_t>();
                    if (slots && *slots >= 0 && *slots <= 8)
                        rules.weapon_slots = static_cast<int>(*slots);
                    else
                        rules.errors.push_back(at_line(node, "weapon_slots must be a whole number from 0 to 8"));
                }
                else if (key == "max_weight")
                {
                    const auto weight = number(node);
                    if (weight && *weight >= 0.0)
                        rules.max_weight = static_cast<float>(*weight);
                    else
                        rules.errors.push_back(at_line(node, "max_weight must be a number, 0 or more (0: no limit)"));
                }
                else
                    rules.errors.push_back(at_line(node, "unknown setting limits." + std::string(key.str())));
            }
        }

        if (const toml::table* weights = root["weight"].as_table())
        {
            for (const auto& [key, node] : *weights)
            {
                const auto weight = number(node);
                if (!weight)
                    rules.errors.push_back(at_line(node, "the weight of " + std::string(key.str()) + " must be a number"));
                else if (set_item_weight(classname_of(key.str()), static_cast<float>(*weight)))
                    ++rules.items_changed;
                else
                    rules.errors.push_back(at_line(node, "weight: unknown item or a negative weight: " + std::string(key.str())));
            }
        }

        if (const toml::table* stacks = root["stack"].as_table())
        {
            for (const auto& [key, node] : *stacks)
            {
                const auto stack = node.value<std::int64_t>();
                if (!stack || *stack < 1 || *stack > 99)
                    rules.errors.push_back(at_line(node, "the stack of " + std::string(key.str()) + " must be a whole number from 1 to 99"));
                else if (set_item_stack(classname_of(key.str()), static_cast<int>(*stack)))
                    ++rules.items_changed;
                else
                    rules.errors.push_back(at_line(node, "stack: unknown item, or a weapon (always one per box): " + std::string(key.str())));
            }
        }

        // [ammo]: the most reserve rounds a player can carry of each ammo type ("assault_rifle" or "assaultrifle" ...)
        if (const toml::table* ammo = root["ammo"].as_table())
        {
            for (const auto& [key, node] : *ammo)
            {
                std::string kind(key.str());
                kind.erase(std::remove(kind.begin(), kind.end(), '_'), kind.end());
                const auto rounds = node.value<std::int64_t>();
                if (!rounds || *rounds < 0 || *rounds > 10000)
                    rules.errors.push_back(at_line(node, "ammo: " + std::string(key.str()) + " must be a whole number from 0 to 10000"));
                else if (set_ammo_max(kind, static_cast<int>(*rounds)))
                    ++rules.ammo_changed;
                else
                    rules.errors.push_back(at_line(node, "ammo: unknown ammo type " + std::string(key.str()) +
                                                             " (assault_rifle, smg, shotgun, hunting_rifle, "
                                                             "sniper_rifle, grenade_launcher, m60, chainsaw)"));
            }
        }

        // [round_weight]: what one reserve round of each ammo type weighs (the same names as [ammo])
        reset_round_weights();
        if (const toml::table* rounds = root["round_weight"].as_table())
        {
            for (const auto& [key, node] : *rounds)
            {
                std::string kind(key.str());
                kind.erase(std::remove(kind.begin(), kind.end(), '_'), kind.end());
                const auto weight = node.is_boolean() ? std::nullopt : number(node);
                if (!weight || *weight < 0.0 || *weight > 10.0)
                    rules.errors.push_back(at_line(node, "round_weight: " + std::string(key.str()) + " must be a number from 0 to 10"));
                else if (!set_round_weight(kind, static_cast<float>(*weight)))
                    rules.errors.push_back(at_line(node, "round_weight: unknown ammo type " + std::string(key.str()) +
                                                             " (assault_rifle, smg, shotgun, hunting_rifle, "
                                                             "sniper_rifle, grenade_launcher, m60, chainsaw)"));
            }
        }

        g_infection = InfectionRules{};
        if (const toml::table* infection = root["infection"].as_table())
            read_infection(*infection, rules);

        g_stamina = StaminaRules{};
        if (const toml::table* stamina = root["stamina"].as_table())
            read_stamina(*stamina, rules);

        g_melee = MeleeRules{};
        if (const toml::table* melee = root["melee"].as_table())
            read_melee(*melee, rules);

        g_ammo_piles = AmmoPileRules{};
        if (const toml::table* piles = root["ammo_piles"].as_table())
            read_ammo_piles(*piles, rules);

        g_ping = PingRules{};
        if (const toml::table* ping = root["ping"].as_table())
            read_ping(*ping, rules);

        g_map_guns = MapGunRules{};
        if (const toml::table* guns = root["map_guns"].as_table())
            read_map_guns(*guns, rules);

        g_mutators = MutatorRules{};
        if (const toml::table* mutators = root["mutators"].as_table())
            read_mutators(*mutators, rules);

        for (const auto& [key, node] : root)
            if (key != "limits" && key != "weight" && key != "stack" && key != "ammo" && key != "infection" &&
                key != "stamina" && key != "melee" && key != "round_weight" && key != "ammo_piles" &&
                key != "map_guns" && key != "ping" && key != "mutators")
                rules.errors.push_back(at_line(node, "unknown table " + std::string(key.str())));

        // brittle wears [melee]'s weapons: without [melee] it would do nothing
        bool brittle = std::find(g_mutators.pool.begin(), g_mutators.pool.end(), "brittle") != g_mutators.pool.end();
        for (const auto& [map, ids] : g_mutators.maps)
            brittle = brittle || std::find(ids.begin(), ids.end(), "brittle") != ids.end();
        if (g_mutators.enabled && brittle && !g_melee.enabled)
            rules.errors.push_back("mutators: brittle needs [melee] (the weapons' wear); it does nothing without it");
        refresh_mutated();
        return rules;
    }

} // namespace inv
