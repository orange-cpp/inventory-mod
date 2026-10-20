#pragma once
// The chapter mutators ([mutators] in the rules, see rules.hpp): the ones there are, and the roll that picks a
// chapter's.  The game applies them (game/mutators.cpp).

#include "inv/rules.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace inv
{

    struct MutatorInfo
    {
        const char* id; // as the rules name it ("fog")
        const char* title; // as the HUD shows it ("Fog")
    };

    // Every mutator there is.
    std::span<const MutatorInfo> mutators();
    // The mutator `id`, or nullptr.
    const MutatorInfo* find_mutator(std::string_view id);

    // A chapter's mutators on `map`: the map's own list when [mutators.maps] has one, otherwise the roll (with
    // `chance` %, `count` different ones of `pool`, in the pool's order).  `random` is any random number; the same one
    // gives the same pick.  None while the rules are disabled.
    std::vector<std::string> pick_mutators(const MutatorRules& rules, std::string_view map, unsigned random);

} // namespace inv
