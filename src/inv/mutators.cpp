#include "inv/mutators.hpp"

#include <algorithm>

namespace inv
{

    namespace
    {
        constexpr MutatorInfo k_mutators[] = {
            {"fog", "Fog"},
            {"scarcity", "Scarcity"},
            {"heavy_load", "Heavy load"},
            {"contagion", "Contagion"},
            {"brittle", "Brittle"},
            {"horde_tide", "Horde tide"},
            {"hunted", "Hunted"},
            {"crowd", "Crowd"},
            {"witch", "Witch"},
            {"tank", "Tank"},
        };

        // A small generator of its own (a 32-bit xorshift): the pick follows from `random` alone, the same on every
        // standard library.
        unsigned next(unsigned& state)
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        }
    } // namespace

    std::span<const MutatorInfo> mutators()
    {
        return k_mutators;
    }

    const MutatorInfo* find_mutator(std::string_view id)
    {
        for (const MutatorInfo& m : k_mutators)
            if (id == m.id)
                return &m;
        return nullptr;
    }

    std::vector<std::string> pick_mutators(const MutatorRules& rules, std::string_view map, unsigned random)
    {
        if (!rules.enabled)
            return {};
        if (const auto it = rules.maps.find(std::string(map)); it != rules.maps.end())
            return it->second;
        unsigned state = random != 0 ? random : 0x9E3779B9u; // (xorshift stays at 0 from 0)
        if (static_cast<int>(next(state) % 100u) >= rules.chance)
            return {};
        // `count` of the pool's entries, each taken once (the pool's order kept)
        std::vector<std::string> left;
        for (const std::string& id : rules.pool)
            if (std::find(left.begin(), left.end(), id) == left.end())
                left.push_back(id);
        std::vector<bool> taken(left.size(), false);
        const int count = std::min(rules.count, static_cast<int>(left.size()));
        for (int n = 0; n < count; ++n)
        {
            int index = static_cast<int>(next(state) % static_cast<unsigned>(left.size() - n));
            for (std::size_t i = 0; i < left.size(); ++i)
                if (!taken[i] && index-- == 0)
                {
                    taken[i] = true;
                    break;
                }
        }
        std::vector<std::string> picked;
        for (std::size_t i = 0; i < left.size(); ++i)
            if (taken[i])
                picked.push_back(left[i]);
        return picked;
    }

} // namespace inv
