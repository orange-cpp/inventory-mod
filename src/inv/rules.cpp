#include "inv/rules.hpp"

#include "inv/items.hpp"

#include <algorithm>
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
                                                                         "\" (assault_rifle, smg, shotgun, auto_shotgun, hunting_rifle, "
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
            if (guns.max_clip < guns.min_clip)
            {
                rules.errors.push_back("map_guns: max_clip is less than min_clip (taken as the same)");
                guns.max_clip = guns.min_clip;
            }
            g_map_guns = guns;
        }
    } // namespace

    const MapGunRules& map_gun_rules()
    {
        return g_map_guns;
    }

    const AmmoPileRules& ammo_pile_rules()
    {
        return g_ammo_piles;
    }

    const InfectionRules& infection_rules()
    {
        return g_infection;
    }

    const StaminaRules& stamina_rules()
    {
        return g_stamina;
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
                                                             " (assault_rifle, smg, shotgun, auto_shotgun, hunting_rifle, "
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
                                                             " (assault_rifle, smg, shotgun, auto_shotgun, hunting_rifle, "
                                                             "sniper_rifle, grenade_launcher, m60, chainsaw)"));
            }
        }

        g_infection = InfectionRules{};
        if (const toml::table* infection = root["infection"].as_table())
            read_infection(*infection, rules);

        g_stamina = StaminaRules{};
        if (const toml::table* stamina = root["stamina"].as_table())
            read_stamina(*stamina, rules);

        g_ammo_piles = AmmoPileRules{};
        if (const toml::table* piles = root["ammo_piles"].as_table())
            read_ammo_piles(*piles, rules);

        g_map_guns = MapGunRules{};
        if (const toml::table* guns = root["map_guns"].as_table())
            read_map_guns(*guns, rules);

        for (const auto& [key, node] : root)
            if (key != "limits" && key != "weight" && key != "stack" && key != "ammo" && key != "infection" &&
                key != "stamina" && key != "round_weight" && key != "ammo_piles" &&
                key != "map_guns")
                rules.errors.push_back(at_line(node, "unknown table " + std::string(key.str())));
        return rules;
    }

} // namespace inv
