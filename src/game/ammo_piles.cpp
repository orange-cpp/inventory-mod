// [ammo_piles]: the map's ammo piles hold a set amount, and the mod's own packs on the map.

#include "game/ammo_piles.hpp"

#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/players.hpp"
#include "game/rounds.hpp"
#include "game/weapons.hpp"
#include "inv/rules.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string_view>
#include <vector>

namespace game::detail
{

    // +use on an ammo pile: CWeaponAmmoSpawn::Use (0x1029F5F0, slot 106 of its vtable 0x10682CF4; thiscall
    // (activator, caller, type, value), ret 0x10) is its own, not CWeaponSpawn's.  A pile of rounds the mod dropped
    // gives its rounds (take_pile), never the endless refill; any other pile is the game's.
    core::Hook<SpawnUseFn> g_ammo_spawn_use;

    // ---- the map's ammo piles, finite ([ammo_piles]) ---------------------------------------------------------------
    //
    // With [ammo_piles] on, a map's ammo pile (not one of dropped rounds) holds a set amount: on its first +use it
    // gets a random ammo type of ammo_piles.kinds and min..max magazines of it (inv::ammo_magazine), and it is tinted
    // that type's colour.  Anyone takes as many as fit (take_pile's rule: the type's most, the weight limit; one
    // survivor can empty it), the rest stays; empty, it is removed.  A bot takes only rounds for a gun it carries.
    // What is left is kept here by pile, with its EHANDLE (another entity made at the same address is a new pile);
    // the clock going back (a new map) forgets it all.

    struct FinitePile
    {
        std::uint32_t handle = k_invalid_handle;
        std::string kind;
        int rounds = 0; // left
    };
    std::map<void*, FinitePile> g_finite_piles; // main thread only
    float g_finite_piles_time = 0.f;

    std::minstd_rand& pile_random()
    {
        static std::minstd_rand random(static_cast<unsigned>(GetTickCount64() * 2246822519u));
        return random;
    }

    void use_finite_pile(void* pile, void* activator)
    {
        if (activator == nullptr || team_of(activator) != 2)
            return;
        const inv::AmmoPileRules& rules = inv::ammo_pile_rules();
        // (more than a second back: a new map; a player's command runs on their tick base, a little behind; see
        // known_guns_clock.  A round starting forgets them too.)
        const float now = game_time();
        if (now + 1.f < g_finite_piles_time)
        {
            g_finite_piles.clear(); // a new map
            g_finite_piles_time = now;
        }
        else
            g_finite_piles_time = std::max(now, g_finite_piles_time);
        const std::uint32_t handle = handle_of(pile);
        FinitePile& state = g_finite_piles[pile];
        if (state.handle != handle || state.kind.empty())
        {
            // its first +use: what it holds
            std::minstd_rand& random = pile_random();
            const std::string& kind = rules.kinds[random() % rules.kinds.size()];
            const int magazines =
                rules.min_magazines + static_cast<int>(random() % static_cast<unsigned>(rules.max_magazines - rules.min_magazines + 1));
            state = FinitePile{handle, kind, magazines * std::max(1, inv::ammo_magazine(kind))};
            const unsigned color = inv::ammo_color(kind);
            char tint[16];
            std::snprintf(tint, sizeof(tint), "%u %u %u 255", (color >> 16) & 255u, (color >> 8) & 255u, color & 255u);
            mem::guarded([&] { g.set_key_value(g.tools, pile, "rendercolor", tint); });
            INV_LOG("ammo pile %p: holds %d %s (%d magazine(s))", pile, state.rounds, kind.c_str(), magazines);
        }

        const int type = inv::ammo_index(state.kind);
        const char* title = inv::ammo_title(state.kind);
        const bool bot = is_bot(activator);
        const bool own = !bot && activator == local_player(); // the host, or the player being served: their weight counts
        if (bot && !carries_ammo_type(activator, type))
            return; // (no gun for these rounds)
        const int n = rounds_that_fit(activator, state.kind, state.rounds, own);
        if (n <= 0)
        {
            const int have = mem::read_or<int>(reinterpret_cast<std::uintptr_t>(reserve_of(activator, type)), 0);
            if (own)
                notify(have >= inv::ammo_max(state.kind) ? std::string("You carry all the ") + title + " you can"
                                                         : std::string("Too heavy for more ") + title);
            return;
        }
        mem::guarded([&] { *reserve_of(activator, type) += n; });
        state_changed(activator);
        state.rounds -= n;
        INV_LOG("ammo pile %p: %s took %d %s, %d left", pile, bot ? "a bot" : "a survivor", n, state.kind.c_str(), state.rounds);
        if (state.rounds <= 0)
        {
            mem::guarded([&] { g.util_remove(pile); });
            g_finite_piles.erase(pile);
        }
    }

    // ---- the map's ammo packs ([ammo_piles] replace_map_piles, extra) -----------------------------------------------
    //
    // A moment after a round starts (the event round_start: a new map, or the same one again after a lost round;
    // see place_map_packs_due), the mod puts its packs on the map, once a round (not again while a pack of its own is
    // on the map):
    //   replace_map_piles: every ammo pile of the map (weapon_ammo_spawn: its m_vecOrigin +0x388 and m_angRotation
    //     +0x394, the SendProps' offsets) becomes a pack of a random type of `kinds` with min..max magazines, where it
    //     stood; the map's pile is removed.  What it holds shows before anyone walks to it.
    //   extra: that many more packs beside the map's item spots (weapon_*_spawn, weapon_item_spawn: not the ammo
    //     piles), 20 units off in a random direction and dropped to the floor, none within 700 units of a survivor
    //     (the start), 300 of another extra pack or 150 of a pile.
    // They are piles as a drop makes them (spawn_pile), named "inv_maprounds_<kind>_<rounds>": anyone takes from them,
    // a bot too (rounds for a gun it has).

    ULONGLONG g_place_packs_at = 0; // main thread: when place_map_packs runs (0: not due)

    void place_map_packs()
    {
        const inv::AmmoPileRules& rules = inv::ammo_pile_rules();
        if (!rules.enabled || (!rules.replace_map_piles && rules.extra <= 0) || rules.kinds.empty())
            return;
        struct Spot
        {
            void* entity;
            Vec3 at;
            float yaw;
        };
        std::vector<Spot> piles, items;
        std::vector<Vec3> survivors;
        for (int index = 1; index < 0x1000; ++index)
        {
            void* entity = entity_at_index(index);
            char name[64];
            if (entity == nullptr || !classname_of(entity, name, sizeof(name)))
                continue;
            std::string kind;
            int rounds = 0;
            if (pile_contents(entity, kind, rounds))
            {
                INV_LOG("map packs: the mod's piles are on the map already, nothing to place");
                return;
            }
            const auto e = reinterpret_cast<std::uintptr_t>(entity);
            Vec3 at{}, angles{};
            if (!mem::read(e + k_off_origin, at))
                continue;
            mem::read(e + k_off_angles, angles);
            const std::string_view cls(name);
            if (cls == "player")
            {
                if (team_of(entity) == 2)
                    survivors.push_back(at);
            }
            else if (cls == "weapon_ammo_spawn")
                piles.push_back({entity, at, angles.y});
            else if (cls.starts_with("weapon_") && cls.ends_with("_spawn"))
                items.push_back({entity, at, angles.y});
        }
        std::minstd_rand& random = pile_random();
        const auto roll = [&](std::string& kind, int& rounds) {
            kind = rules.kinds[random() % rules.kinds.size()];
            const int magazines =
                rules.min_magazines + static_cast<int>(random() % static_cast<unsigned>(rules.max_magazines - rules.min_magazines + 1));
            rounds = magazines * std::max(1, inv::ammo_magazine(kind));
        };
        const auto distance2 = [](const Vec3& a, const Vec3& b) {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz;
        };
        std::vector<Vec3> placed;
        int replaced = 0, extra = 0;
        if (rules.replace_map_piles)
            for (const Spot& spot : piles)
            {
                std::string kind, model;
                int rounds = 0;
                bool loaded = false;
                roll(kind, rounds);
                mem::guarded([&] { g.util_remove(spot.entity); }); // (first: not two piles in one place)
                if (spawn_pile(kind, rounds, spot.at, spot.yaw, true, model, loaded, true) != nullptr)
                {
                    ++replaced;
                    placed.push_back(spot.at);
                    INV_LOG("map packs: the map's pile at %.0f %.0f %.0f is %d %s", spot.at.x, spot.at.y, spot.at.z, rounds,
                            kind.c_str());
                }
            }
        else
            for (const Spot& spot : piles)
                placed.push_back(spot.at);
        std::shuffle(items.begin(), items.end(), random);
        std::vector<Vec3> extras;
        for (const Spot& spot : items)
        {
            if (extra >= rules.extra)
                break;
            const float angle = static_cast<float>(random() % 360u) * 0.0174533f;
            const Vec3 at{spot.at.x + 20.f * std::cos(angle), spot.at.y + 20.f * std::sin(angle), spot.at.z + 8.f};
            const auto near_any = [&](const std::vector<Vec3>& list, float radius) {
                return std::any_of(list.begin(), list.end(), [&](const Vec3& v) { return distance2(v, at) < radius * radius; });
            };
            if (near_any(survivors, 700.f) || near_any(extras, 300.f) || near_any(placed, 150.f))
                continue;
            std::string kind, model;
            int rounds = 0;
            bool loaded = false;
            roll(kind, rounds);
            if (spawn_pile(kind, rounds, at, static_cast<float>(random() % 360u), true, model, loaded) != nullptr)
            {
                ++extra;
                extras.push_back(at);
                INV_LOG("map packs: an extra pack at %.0f %.0f %.0f: %d %s", at.x, at.y, at.z, rounds, kind.c_str());
            }
        }
        INV_LOG("map packs: %d of the map's %zu ammo piles replaced, %d extra packs (of %d wanted, %zu item spots)", replaced,
                piles.size(), extra, rules.extra, items.size());
    }

    void on_round_start_piles()
    {
        if (inv::ammo_pile_rules().enabled)
            g_place_packs_at = GetTickCount64() + 1500; // (the map's entities are not all there yet)
        g_finite_piles.clear(); // (the map's piles are new ones)
        g_finite_piles_time = 0.f;
    }

    // From GameFrame: the packs, when round_start asked for them a moment ago (the map's entities are all there).
    void place_map_packs_due()
    {
        if (g_place_packs_at == 0 || GetTickCount64() < g_place_packs_at)
            return;
        g_place_packs_at = 0;
        place_map_packs();
    }

    void __fastcall hk_ammo_spawn_use(void* pile, void* edx, void* activator, void* caller, int type, float value)
    {
        std::string kind;
        int rounds = 0;
        bool map = false;
        if (!pile_contents(pile, kind, rounds, &map))
        {
            if (!inv::ammo_pile_rules().enabled)
            {
                g_ammo_spawn_use.original()(pile, edx, activator, caller, type, value);
                return;
            }
            if (served_remotely(activator) && !is_bot(activator))
            {
                Activation active(activator); // (their weight counts when it is theirs)
                use_finite_pile(pile, activator);
                return;
            }
            use_finite_pile(pile, activator);
            return;
        }
        if (served_remotely(activator) && !is_bot(activator))
        {
            Activation active(activator); // (their weight counts when it is theirs)
            take_pile(pile, activator, kind, rounds, map);
            return;
        }
        take_pile(pile, activator, kind, rounds, map);
    }

    void install_ammo_spawn_use()
    {
        // an ammo pile's own Use (optional, see hk_ammo_spawn_use): what a pile of dropped rounds gives
        {
            const std::uintptr_t ammo_use = g.server.base + k_rva_ammo_spawn_use;
            if (mem::read_or<std::uintptr_t>(g.server.base + k_rva_ammo_spawn_vtable + 4u * k_vt_entity_use) == ammo_use &&
                mem::find_pattern(ammo_use, 12, "55 8B EC 51 53 8B 5D 08 85 DB 0F 84") == ammo_use &&
                g_ammo_spawn_use.create("AmmoSpawnUse", reinterpret_cast<void*>(ammo_use), hk_ammo_spawn_use))
                g_ammo_spawn_use.enable();
            else
                INV_LOG("game: CWeaponAmmoSpawn::Use does not look as expected: dropped rounds cannot be picked up");
        }
    }

} // namespace game::detail
