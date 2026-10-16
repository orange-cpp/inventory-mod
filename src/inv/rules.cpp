#include "inv/rules.hpp"

#include "inv/items.hpp"

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
    } // namespace

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

        for (const auto& [key, node] : root)
            if (key != "limits" && key != "weight" && key != "stack")
                rules.errors.push_back(at_line(node, "unknown table " + std::string(key.str())));
        return rules;
    }

} // namespace inv
