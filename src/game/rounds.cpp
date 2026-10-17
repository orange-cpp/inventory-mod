// The player's reserve as a thing of its own: dropped as a pile of rounds, taken from one.

#include "game/rounds.hpp"

#include "core/log.hpp"
#include "game/addons.hpp"
#include "game/backend.hpp"
#include "game/weapons.hpp"
#include "inv/rules.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace game::detail
{

    // ---- rounds: the reserve as a thing of its own -----------------------------------------------------------------
    //
    // The player's reserve (m_iAmmo, per ammo type, shared by the guns of a type) weighs (inv::round_weight,
    // [round_weight] in the rules: refresh_native counts it in) and can be dropped, a magazine's worth at a time or
    // all of a type.  The rounds land as one of the game's own ammo piles, weapon_ammo_spawn (CWeaponAmmoSpawn, see
    // below), as an incendiary ammo pack tinted the colour of its type, with a targetname that says what it holds:
    // "inv_rounds_<kind>_<rounds>".
    // +use on it goes through the pile's own Use, hooked (hk_ammo_spawn_use): instead of the endless refill it gives
    // those rounds, as many as fit (the type's most, the weight limit); what does not fit stays in the pile.  The
    // bots do not take them.  Ammo piles and guns picked up fill a reserve as the game does; cap_reserve takes a rise
    // past the weight limit back down to what fits.

    constexpr char k_pile_prefix[] = "inv_rounds_"; // a pile a player dropped
    constexpr char k_map_pile_prefix[] = "inv_maprounds_"; // one the mod put on the map ([ammo_piles], see place_map_packs)
    // weapon_ammo_spawn is CWeaponAmmoSpawn (vtable 0x10682CF4, set by its factory 0x102A22F0 over CWeaponSpawn's):
    // its Precache (slot 26, 0x1029F5A0) precaches models/props/terror/ammo_stack.mdl, its model (slot 239,
    // 0x1029F560) is the "model" keyvalue or that one, its Use (slot 106, 0x1029F5F0) is its own (the refill).
    // The pile is its round type's own ammo pack, models/inv_mod/rounds_<kind>.mdl, when the game has it: the addon
    // inventory_mod_rounds.vpk in the addons folder next to the DLL, which the mod mounts (see mount_addons), or in
    // left4dead2/addons (tools/make_rounds_addon.py: the incendiary pack with the texture of assets/textures for
    // that type).  Without it (or for a type with no pack: the M60, the chainsaw) it
    // is the closed incendiary ammo pack (the upgrade pack's world model, loaded with every map), tinted the colour of
    // its round type (inv::ammo_color, the entity's "rendercolor" as "r g b 255": three numbers leave the alpha 0 and
    // it is invisible); ammo_stack.mdl when that cannot be precached.  Every player needs the addon to see the packs:
    // a model the server has and a player does not is the ERROR model for that player.
    constexpr const char* k_pile_model = "models/w_models/weapons/w_eq_incendiary_ammopack.mdl";

    constexpr const char* k_pile_model_fallback = "models/props/terror/ammo_stack.mdl";

    PrecacheModelFn precache_model_fn()
    {
        static PrecacheModelFn fn = [] {
            const std::uintptr_t at = g.server.base + k_rva_precache_model;
            return mem::find_pattern(at, 11, "55 8B EC 56 8B 75 08 85 F6 74 ??") == at ? reinterpret_cast<PrecacheModelFn>(at)
                                                                                     : nullptr;
        }();
        return fn;
    }


    // What a pile the mod made holds (`map`: one it put on the map, not one a player dropped); false for any other
    // entity.
    bool pile_contents(void* entity, std::string& kind, int& rounds, bool* map)
    {
        // CBaseEntity::m_iName +0x140 (datamap "m_iName": a pooled string; IServerTools GetKeyValue "targetname"
        // gives the pointer's bytes, not the text, as for the model)
        char name[96] = {};
        const char* text = mem::read_or<const char*>(reinterpret_cast<std::uintptr_t>(entity) + k_off_entity_name);
        if (text == nullptr || !mem::safe_copy(name, text, sizeof(name) - 1))
            return false;
        const bool on_map = std::strncmp(name, k_map_pile_prefix, sizeof(k_map_pile_prefix) - 1) == 0;
        if (!on_map && std::strncmp(name, k_pile_prefix, sizeof(k_pile_prefix) - 1) != 0)
            return false;
        if (map != nullptr)
            *map = on_map;
        const char* rest = name + (on_map ? sizeof(k_map_pile_prefix) : sizeof(k_pile_prefix)) - 1;
        const char* separator = std::strrchr(rest, '_');
        if (separator == nullptr)
            return false;
        kind.assign(rest, static_cast<std::size_t>(separator - rest));
        rounds = std::atoi(separator + 1);
        return inv::ammo_index(kind) >= 0 && rounds > 0;
    }

    void set_pile_contents(void* entity, const std::string& kind, int rounds, bool map = false)
    {
        const std::string name = (map ? k_map_pile_prefix : k_pile_prefix) + kind + "_" + std::to_string(rounds);
        mem::guarded([&] { g.set_key_value(g.tools, entity, "targetname", name.c_str()); });
    }

    // How many of `rounds` of `kind` the player can take: up to the type's most and, for the player whose inventory
    // session() is, the weight limit.
    int rounds_that_fit(void* player, const std::string& kind, int rounds, bool weigh)
    {
        const int type = inv::ammo_index(kind);
        const AmmoPool pool = read_pool(player);
        if (type < 0 || !pool.read)
            return 0;
        const int max = inv::ammo_max(kind);
        int n = max >= 0 ? std::min(rounds, std::max(0, max - pool.count[type])) : rounds;
        const float each = inv::round_weight(kind);
        if (weigh && each > 0.f)
        {
            Session& s = session();
            std::lock_guard lock(s.mutex);
            const float max_weight = s.inventory.limits().max_weight;
            const float load = s.inventory.weight() - s.inventory.ammo_weight() + inv::ammo_weight(pool.count);
            if (max_weight > 0.f)
                n = std::min(n, max_weight <= load ? 0 : static_cast<int>(std::floor((max_weight - load) / each + 1e-4f)));
        }
        return std::max(0, n);
    }

    // The piles dropped this map (by EHANDLE: a removed one, or one of another map, is no entity any more), with where
    // they were put: rounds dropped next to a pile of their type join it, and a new pile is put beside the others,
    // not into them.
    struct DroppedPile
    {
        std::uint32_t handle;
        Vec3 at;
    };
    std::vector<DroppedPile> g_dropped_piles; // main thread only

    void track_dropped_pile(void* pile, const Vec3& at)
    {
        if (const std::uint32_t handle = handle_of(pile); handle != k_invalid_handle)
            g_dropped_piles.push_back({handle, at});
    }


    // Drops `count` (0: all) of the player's reserve of `kind` in front of them, as an ammo pile.
    void do_drop_ammo(const std::string& kind, int count)
    {
        void* player = local_player();
        const int type = inv::ammo_index(kind);
        const char* title = inv::ammo_title(kind);
        if (player == nullptr || type < 0 || title == nullptr)
        {
            notify("Not in a game");
            return;
        }
        Vec3 eye{}, forward{};
        if (!player_view(player, eye, forward))
        {
            notify("Cannot read where you are looking");
            return;
        }
        const int have = mem::read_or<int>(reinterpret_cast<std::uintptr_t>(reserve_of(player, type)), 0);
        const int n = count <= 0 ? have : std::min(count, have);
        if (n <= 0)
        {
            notify(std::string("No ") + title + " to drop");
            return;
        }
        Vec3 position{eye.x + forward.x * 48.f, eye.y + forward.y * 48.f, eye.z + forward.z * 48.f - 14.f};

        // a pile of this type close by takes them; the piles that are gone are forgotten
        g_dropped_piles.erase(std::remove_if(g_dropped_piles.begin(), g_dropped_piles.end(),
                                             [](const DroppedPile& d) { return entity_from_handle(d.handle) == nullptr; }),
                              g_dropped_piles.end());
        const auto close_to = [](const Vec3& a, const Vec3& b, float radius) {
            const float dx = a.x - b.x, dy = a.y - b.y;
            return dx * dx + dy * dy < radius * radius && std::fabs(a.z - b.z) < 72.f;
        };
        for (const DroppedPile& d : g_dropped_piles)
        {
            std::string held;
            int rounds = 0;
            void* other = entity_from_handle(d.handle);
            if (!close_to(d.at, position, 40.f) || !pile_contents(other, held, rounds) || held != kind)
                continue;
            set_pile_contents(other, kind, rounds + n);
            mem::guarded([&] { *reserve_of(player, type) = have - n; });
            state_changed(player);
            INV_LOG("rounds: dropped %d %s (%d left) onto pile %p, which holds %d now", n, kind.c_str(), have - n, other, rounds + n);
            notify("Dropped " + std::to_string(n) + " " + title + " (onto the pile there)");
            return;
        }
        // not into another pile (piles inside each other crashed the game's physics, see below): beside it
        for (int step = 1; step <= 8; ++step)
        {
            bool clear = true;
            for (const DroppedPile& d : g_dropped_piles)
                clear = clear && !close_to(d.at, position, 22.f);
            if (clear)
                break;
            const float side = (step % 2 == 1 ? 1.f : -1.f) * 24.f * static_cast<float>((step + 1) / 2);
            position = Vec3{eye.x + forward.x * 48.f - forward.y * side, eye.y + forward.y * 48.f + forward.x * side,
                            eye.z + forward.z * 48.f - 14.f};
        }
        const float yaw = std::atan2(forward.y, forward.x) * 57.29578f;
        std::string model;
        bool loaded = false;
        void* pile = spawn_pile(kind, n, position, yaw, false, model, loaded);
        if (pile == nullptr)
        {
            notify(std::string("Could not drop ") + title);
            return;
        }
        track_dropped_pile(pile, position);
        mem::guarded([&] { *reserve_of(player, type) = have - n; });
        state_changed(player);
        INV_LOG("rounds: dropped %d %s (%d left) as pile %p (%s, %s) at %.1f %.1f %.1f", n, kind.c_str(), have - n, pile,
                model.c_str(), loaded ? "loaded with the map" : "precached now", position.x, position.y, position.z);
        notify("Dropped " + std::to_string(n) + " " + title);
    }

    // The floor under `at` (256 units down at most), or `at` itself when there is none: a throwaway info_target (no
    // physics) is put there and dropped to the floor (UTIL_DropToFloor: 1 when it landed).  Main thread.
    Vec3 on_floor(const Vec3& at)
    {
        Vec3 floor = at;
        if (g.drop_to_floor == nullptr)
            return floor;
        mem::guarded([&] {
            void* probe = g.create_entity(g.tools, "info_target");
            if (probe == nullptr)
                return;
            char origin[64];
            std::snprintf(origin, sizeof(origin), "%.2f %.2f %.2f", at.x, at.y, at.z);
            g.set_key_value(g.tools, probe, "origin", origin);
            g.dispatch_spawn(g.tools, probe);
            Vec3 landed{};
            if (g.drop_to_floor(probe, 0x200400Bu, nullptr) == 1 &&
                mem::read(reinterpret_cast<std::uintptr_t>(probe) + k_off_origin, landed))
                floor = landed;
            g.util_remove(probe);
        });
        return floor;
    }

    // An ammo pile of `rounds` of `kind` on the floor under `position` (`map_spot`: exactly there, the spot of one of
    // the map's piles), turned to `yaw`: its type's own pack when the game has it, else the tinted incendiary pack
    // (see k_pile_model).  `map`: one the mod put on the map.  nullptr when it could not be made.  Main thread.
    void* spawn_pile(const std::string& kind, int rounds, const Vec3& position, float yaw, bool map, std::string& model_used,
                     bool& loaded, bool map_spot)
    {
        const PrecacheModelFn precache_model = precache_model_fn();
        if (precache_model == nullptr)
        {
            INV_LOG("rounds: the game's PrecacheModel does not look as expected: no pile");
            return nullptr;
        }
        const std::string own_model = "models/inv_mod/rounds_" + kind + ".mdl";
        const bool own = game_file_exists(own_model.c_str()); // its type's pack (the addon)
        const char* model = own ? own_model.c_str() : k_pile_model;
        loaded = false; // by the map already (else it is precached now, in the middle of the round)
        if (const auto is_precached = g_engine_server != nullptr
                                          ? reinterpret_cast<IsModelPrecachedFn>(mem::vslot(g_engine_server, k_vt_is_model_precached))
                                          : nullptr)
            mem::guarded([&] { loaded = is_precached(g_engine_server, model); });
        int model_index = -1;
        mem::guarded([&] { model_index = precache_model(model); });
        if (model_index < 0 && own)
        {
            model = k_pile_model;
            mem::guarded([&] { model_index = precache_model(model); });
        }
        if (model_index < 0)
        {
            model = k_pile_model_fallback;
            mem::guarded([&] { model_index = precache_model(model); });
        }
        if (model_index < 0)
        {
            INV_LOG("rounds: no ammo pile model could be precached");
            return nullptr;
        }
        const unsigned color = own ? 0xFFFFFFu : inv::ammo_color(kind); // (its own pack has its colour)
        char tint[16];
        std::snprintf(tint, sizeof(tint), "%u %u %u 255", (color >> 16) & 255u, (color >> 8) & 255u, color & 255u); // (a missing alpha is 0: invisible)
        char angles[32];
        std::snprintf(angles, sizeof(angles), "0 %.0f 0", yaw);
        // A pile moved after it spawned (Teleport, DropToFloor) crashed the game's physics a moment later, in
        // vphysics.dll +0xF3B1C (IVP's collision code, a null compact edge): its physics object is a static one,
        // made where the pile stood when it spawned (the map's origin), and a static IVP object is not meant to move.
        // ent_create does the same to a spawner (spawn, then teleport): that crashed it too.  So the floor under
        // `position` is found first (on_floor) and the pile is made there, the model's origin at its bottom.
        const Vec3 at = map_spot ? position : on_floor(position);
        char origin[64];
        std::snprintf(origin, sizeof(origin), "%.2f %.2f %.2f", at.x, at.y, at.z + 0.3f);
        void* pile = nullptr;
        mem::guarded([&] {
            pile = g.create_entity(g.tools, "weapon_ammo_spawn");
            if (pile == nullptr)
                return;
            g.set_key_value(g.tools, pile, "model", model);
            g.set_key_value(g.tools, pile, "rendercolor", tint);
            // what a map's own pile has (c2m1_highway: "solid" "6", "spawnflags" "2", "disableshadows" "1"): without
            // "solid" the game crashed in vphysics.dll (+0xF3B1C, a null edge in IVP's collision code) a while later,
            // as with a spawner made by ent_create
            g.set_key_value(g.tools, pile, "solid", "6");
            g.set_key_value(g.tools, pile, "spawnflags", "2");
            g.set_key_value(g.tools, pile, "disableshadows", "1");
            g.set_key_value(g.tools, pile, "angles", angles);
            // where it stands, before it spawns: its physics object (static) is made there and must never move
            g.set_key_value(g.tools, pile, "origin", origin);
            set_pile_contents(pile, kind, rounds, map);
            if (const auto precache = reinterpret_cast<PrecacheFn>(mem::vslot(pile, k_vt_precache)))
                precache(pile);
            g.dispatch_spawn(g.tools, pile);
        });
        model_used = model;
        return pile;
    }


    // +use on a pile the mod made: its rounds, as many as fit.  A bot takes from one the mod put on the map (`map`),
    // and only rounds for a gun it has; never what a player dropped.
    void take_pile(void* pile, void* player, const std::string& kind, int rounds, bool map)
    {
        if (player == nullptr || team_of(player) != 2)
            return;
        const int type = inv::ammo_index(kind);
        const bool bot = is_bot(player);
        if (bot && (!map || !carries_ammo_type(player, type)))
            return;
        const char* title = inv::ammo_title(kind);
        const bool own = !bot && player == local_player(); // the host, or the player being served: their weight counts
        const int n = rounds_that_fit(player, kind, rounds, own);
        if (n <= 0)
        {
            const int have = mem::read_or<int>(reinterpret_cast<std::uintptr_t>(reserve_of(player, type)), 0);
            if (own)
                notify(have >= inv::ammo_max(kind) ? std::string("You carry all the ") + title + " you can"
                                                   : std::string("Too heavy for more ") + title);
            return;
        }
        mem::guarded([&] { *reserve_of(player, type) += n; });
        state_changed(player);
        if (n >= rounds)
            mem::guarded([&] { g.util_remove(pile); });
        else
            set_pile_contents(pile, kind, rounds - n, map);
        INV_LOG("rounds: %s took %d of the %d %s in pile %p", bot ? "a bot" : "a survivor", n, rounds, kind.c_str(), pile);
        if (own)
            notify("Picked up " + std::to_string(n) + " " + title + (n < rounds ? " (the rest is too much)" : ""));
    }

} // namespace game::detail
