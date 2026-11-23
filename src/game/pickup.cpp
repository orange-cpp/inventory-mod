// +use on a weapon or an item in the world: the pick-up hook, a gun's own Use, the map's item spawners.

#include "game/pickup.hpp"

#include "core/config.hpp"
#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/fuel.hpp"
#include "game/map_guns.hpp"
#include "game/native_slots.hpp"
#include "game/players.hpp"
#include "game/weapons.hpp"
#include "inv/rules.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace game::detail
{

    // ---- the pickup hook ----------------------------------------------------------------------------------------

    // CTerrorPlayer vtable slot 426 (server.dll + 0x31D080) is what a +use on a weapon entity calls (seen in the
    // game by hooking it together with slots 280 and 281):
    //   bool __thiscall (CTerrorPlayer* this, CBaseEntity* weapon), ret 4
    // It asks slot 280 (Weapon_CanUse, 0x10300600), on success equips with slot 281 and fires "weapon_given"; it
    // returns false when the weapon is refused, e.g. when the player's slot for it is already taken.  That is where
    // the inventory steps in.
    core::Hook<BumpWeaponFn> g_bump_weapon;

    // Takes `weapon` (a catalogue item lying in the world) into the inventory.
    bool store_world_item(void* weapon, const inv::ItemDef& def)
    {
        int stored = 0;
        int weapon_box = -1;
        int reserve = -1, reserve_type = -1; // a gun's own reserve: into the player's (shared by its ammo type)
        {
            // a gun keeps its magazine, a melee weapon which one it is
            inv::WeaponState state = def.weapon() ? weapon_state(nullptr, weapon, def, false) : inv::WeaponState{};
            reserve = state.ammo;
            reserve_type = ammo_type_of(weapon);
            state.ammo = -1;
            Session& s = session();
            std::lock_guard lock(s.mutex);
            if (def.weapon())
            {
                weapon_box = s.inventory.add_weapon(def, state);
                stored = weapon_box >= 0 ? 1 : 0;
            }
            else
                stored = s.inventory.add(def, 1, can_spent(weapon)); // (a gas can: what it lacks)
        }
        if (stored == 0)
        {
            notify(refusal(def));
            return false;
        }
        if (!mem::guarded([&] { g.util_remove(weapon); }))
        {
            if (weapon_box >= 0)
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                s.inventory.remove_from_slot(weapon_box, 1);
            }
            else
                take_from_inventory(def, 1);
            notify("The game refused to take the item away");
            return false;
        }
        if (weapon_box >= 0)
            add_to_pool(local_player(), def.classname, reserve_type, reserve);
        INV_LOG("stored %s", def.name);
        return true;
    }


    // A gun or melee weapon the local player is about to pick up would take them over the weight limit (the one it
    // replaces in its game slot, if any, is dropped by the game, so only the difference counts).  Says why.
    bool weapon_too_heavy(void* weapon)
    {
        char name[64];
        if (!classname_of(weapon, name, sizeof(name)))
            return false;
        const float weight = inv::weapon_weight(name);
        if (weight <= 0.f)
            return false;
        mem::guarded([] { refresh_native(); }); // what they carry right now (a server player's may be stale)
        char text[128] = {};
        {
            Session& s = session();
            std::lock_guard lock(s.mutex);
            const float max_weight = s.inventory.limits().max_weight;
            if (max_weight <= 0.f)
                return false;
            const std::size_t slot = static_cast<std::size_t>(native_slot_of(name));
            const float replaced = slot < s.native.size() ? inv::weapon_weight(s.native[slot].classname) : 0.f;
            const float total = s.inventory.weight() - replaced + weight;
            INV_LOG("+use on %s: weight %.1f - %.1f (replaced) + %.1f = %.1f of %.1f", name, s.inventory.weight(),
                    replaced, weight, total, max_weight);
            if (total <= max_weight + 1e-3f)
                return false;
            std::snprintf(text, sizeof(text), "Too heavy: %.1f / %.1f, %s weighs %.1f", s.inventory.weight(), max_weight,
                          describe_weapon(name).name.c_str(), weight);
        }
        INV_LOG("+use on %s: too heavy to carry, refused", name);
        notify(text);
        return true;
    }

    // A stored copy from a map spawner names the player as its owner (m_hOwner), so GiveNamedItem reports success
    // and the spawner counts it (see bump_weapon).
    void claim_for(void* player, void* weapon)
    {
        for (int index = 1; index <= 64; ++index) // the player's EHANDLE: players are entities 1..maxplayers
        {
            EntInfo info{};
            if (!mem::read(g.ent_list + 4 + static_cast<std::uintptr_t>(index) * sizeof(EntInfo), info) ||
                info.entity != player)
                continue;
            const std::uint32_t handle = static_cast<std::uint32_t>(index) | (info.serial << 12);
            mem::guarded([&] {
                *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(weapon) + k_off_weapon_owner) = handle;
            });
            return;
        }
    }

    // +use on a gun or melee weapon by the local (or served) player: into the hands when that game slot is free (the
    // game's own pick-up); when the hands have one there already, into a free weapon box with its rounds; with no
    // free box (or too heavy for one) the game's own swap, as long as the swap is not too heavy.
    enum class WeaponPickup
    {
        Game, // the game's own pick-up or swap
        Stored, // into a box: the game must not equip it
        Refused, // too heavy
    };

    WeaponPickup weapon_pickup(void* player, void* weapon, const inv::ItemDef* def)
    {
        if (def != nullptr && player_has_slot(player, def->game_slot))
        {
            bool box = false;
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                box = s.inventory.has_box_for(*def) && s.inventory.room_for(*def) >= 1;
            }
            INV_LOG("+use on %s: that game slot is taken, %s", def->classname, box ? "into a weapon box" : "no weapon box: the game's swap");
            if (box && store_world_item(weapon, *def))
            {
                claim_for(player, weapon);
                return WeaponPickup::Stored;
            }
        }
        return weapon_too_heavy(weapon) ? WeaponPickup::Refused : WeaponPickup::Game;
    }

    bool bump_weapon(void* player, void* edx, void* weapon)
    {
        MapGunReserve map_gun;
        cut_map_gun(player, weapon, map_gun);
        const inv::ItemDef* def = nullptr;
        const bool local = !g_giving && player != nullptr && player == local_player();
        if (local)
            def = item_of(weapon);
        // a gun or melee weapon: see weapon_pickup
        if (def == nullptr || def->weapon())
        {
            const WeaponPickup pickup = local ? weapon_pickup(player, weapon, def) : WeaponPickup::Game;
            if (pickup != WeaponPickup::Game)
                return pickup == WeaponPickup::Stored;
            KeepRounds keep(local ? player : nullptr, weapon); // (the game's pick-up or swap: its rounds stay)
            return g_bump_weapon.original()(player, edx, weapon);
        }

        const bool always = core::config().auto_store;
        const bool full = player_has_slot(player, def->game_slot);
        INV_LOG("+use on %s (%p): slot %s, auto_store %d", def->classname, weapon, full ? "full" : "free",
                always ? 1 : 0);
        // Handled: the game must not equip it as well, but the answer is "picked up".  A map's item spawner
        // (weapon_*_spawn, CWeaponSpawn::Use 0x1029DCB0) gives a fresh copy through GiveItem (0x1029E410) ->
        // GiveNamedItem(name, 0, 1, 0), which calls this; with that third argument GiveNamedItem removes the copy and
        // reports failure unless the copy's owner (m_hOwner) is the player (0x10311773), and the spawner takes one
        // off its count (and disappears when empty) only on success.  Answering false, or true without the owner,
        // left the spawner full, so its item could be stored again and again.  So the stored copy (already marked
        // for deletion) names the player as its owner.  A weapon lying in the world (CBaseCombatWeapon::Use
        // 0x1024F4A0) ignores both.
        const bool tried = always || full;
        if (tried && store_world_item(weapon, *def))
        {
            claim_for(player, weapon);
            return true;
        }
        // not stored: the game equips it into the free slot, and refreshing the game slots gives it a box, so it
        // counts against the weight limit like a stored one.  Too heavy: refused here, before the game equips it.
        bool too_heavy = false;
        {
            Session& s = session();
            std::lock_guard lock(s.mutex);
            too_heavy = !full && s.inventory.room_for(*def) == 0;
        }
        if (too_heavy)
        {
            INV_LOG("+use on %s: too heavy to carry, refused", def->classname);
            if (!tried) // (a failed store has said why already)
                notify(refusal(*def));
            return false;
        }
        return g_bump_weapon.original()(player, edx, weapon);
    }

    // A map's item spawner refuses by itself when the player already carries the same item: CWeaponSpawn::Use
    // (0x1029DCB0) asks Weapon_GetSlot(player, weaponId) (0x102C98F0) and, finding that item, takes a branch that
    // gives nothing, so the +use pickup hook never runs.  While the spawner serves the local player an item the
    // inventory takes (slot full, or auto_store), that one question answers "nothing there": the spawner then gives
    // through GiveNamedItem as for an empty slot, the +use hook stores the copy, and the spawner counts it.
    core::Hook<SpawnUseFn> g_spawn_use;
    core::Hook<GetSlotFn> g_get_slot;
    WeaponIdNameFn g_weapon_id_name = nullptr;
    std::uintptr_t g_spawner_use = 0; // CWeaponSpawn::Use (0x1029DCB0): slot 106 of every weapon_*_spawn but the ammo pile's
    int g_hidden_weapon_id = 0; // main thread only: Weapon_GetSlot answers null for it (0: none)

    void* __fastcall hk_get_slot(void* player, void* edx, int weapon_id)
    {
        if (g_hidden_weapon_id != 0 && weapon_id == g_hidden_weapon_id && player == local_player())
            return nullptr;
        return g_get_slot.original()(player, edx, weapon_id);
    }

    // The weapons a player holds (their EHANDLEs), sorted.
    std::vector<std::uint32_t> held_handles(void* player)
    {
        std::vector<std::uint32_t> out;
        for (int i = 0; i < k_max_weapons; ++i)
        {
            const std::uint32_t handle = handle_at(player, k_off_my_weapons + 4u * static_cast<unsigned>(i));
            if (handle != k_invalid_handle && entity_from_handle(handle) != nullptr)
                out.push_back(handle);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    void spawn_use(void* spawner, void* edx, void* activator, void* caller, int type, float value)
    {
        int weapon_id = 0;
        const inv::ItemDef* def = nullptr;
        char item[64] = {};
        const bool survivor = activator != nullptr && team_of(activator) == 2;
        if (survivor && g_weapon_id_name != nullptr)
        {
            const auto id_of = reinterpret_cast<WeaponIdFn>(mem::vslot(spawner, k_vt_spawn_weapon_id));
            mem::guarded([&] {
                weapon_id = id_of != nullptr ? id_of(spawner) : 0;
                if (const char* name = weapon_id > 0 ? g_weapon_id_name(weapon_id) : nullptr)
                {
                    mem::safe_copy(item, name, sizeof(item) - 1);
                    def = inv::find_item(item);
                }
            });
        }
        // [map_guns] spawners_once: a weapon spawner (a gun, a melee weapon: not an item) is gone once it has given one
        const bool once = survivor && item[0] != '\0' && inv::map_gun_rules().enabled && inv::map_gun_rules().spawners_once &&
                          (def == nullptr || def->weapon());
        const bool local = activator != nullptr && activator == local_player();
        // [map_guns]: what the spawner adds to the reserve (see MapGunReserve); a gun's spawner only
        MapGunReserve reserve;
        MapGunReserve* outer_reserve = g_spawn_reserve;
        const char* ammo_kind = item[0] != '\0' ? inv::ammo_kind(item) : nullptr;
        if (survivor && inv::map_gun_rules().enabled && ammo_kind != nullptr && std::strcmp(ammo_kind, "chainsaw") != 0)
        {
            const AmmoPool pool = read_pool(activator);
            if (pool.read)
            {
                reserve.player = activator;
                reserve.spawner = true;
                reserve.kind = ammo_kind;
                reserve.type = inv::ammo_index(ammo_kind);
                std::memcpy(reserve.pool_before, pool.count, sizeof(pool.count));
                reserve.before = reserve.type >= 0 ? pool.count[reserve.type] : 0;
                g_spawn_reserve = &reserve;
            }
        }
        std::vector<std::uint32_t> before;
        int stored_before = 0;
        if (once)
        {
            before = held_handles(activator);
            if (local && def != nullptr)
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                stored_before = s.inventory.count_of(*def);
            }
        }
        if (!local)
            def = nullptr; // (what follows is the inventory's: the local or served player's)
        bool take = false;
        if (def != nullptr && !def->weapon() && player_has_slot(activator, def->game_slot))
        {
            Session& s = session();
            std::lock_guard lock(s.mutex);
            // what the +use hook will do: store it (slot full); only when it has room, else the game refuses as usual
            inv::Inventory probe = s.inventory;
            take = probe.add(*def, 1) == 1;
        }
        if (take)
        {
            INV_LOG("spawner %p: %s with the slot full, the game is told the slot is free", spawner, def->classname);
            g_hidden_weapon_id = weapon_id;
        }
        g_spawn_use.original()(spawner, edx, activator, caller, type, value);
        g_hidden_weapon_id = 0;
        g_spawn_reserve = outer_reserve; // (`reserve` limits the reserve when this function returns)
        if (once)
        {
            // given: a weapon in the hands that was not there, or a copy stored in a box
            const std::vector<std::uint32_t> after = held_handles(activator);
            bool given = false;
            for (const std::uint32_t handle : after)
                given |= !std::binary_search(before.begin(), before.end(), handle);
            if (!given && local)
                if (const inv::ItemDef* stored = inv::find_item(item))
                {
                    Session& s = session();
                    std::lock_guard lock(s.mutex);
                    given = s.inventory.count_of(*stored) > stored_before;
                }
            if (given)
            {
                INV_LOG("spawner %p (%s): gave one, it is gone (spawners_once)", spawner, item);
                mem::guarded([&] { g.util_remove(spawner); });
            }
        }
    }

    bool __fastcall hk_bump_weapon(void* player, void* edx, void* weapon)
    {
        if (!g_giving && served_remotely(player))
        {
            Activation active(player);
            if (!active.ok())
                return g_bump_weapon.original()(player, edx, weapon);
            return bump_weapon(player, edx, weapon);
        }
        return bump_weapon(player, edx, weapon);
    }

    void __fastcall hk_spawn_use(void* spawner, void* edx, void* activator, void* caller, int type, float value)
    {
        if (served_remotely(activator))
        {
            Activation active(activator);
            if (!active.ok())
            {
                g_spawn_use.original()(spawner, edx, activator, caller, type, value);
                return;
            }
            spawn_use(spawner, edx, activator, caller, type, value);
            return;
        }
        spawn_use(spawner, edx, activator, caller, type, value);
    }

    // +use on a gun lying in the world does not always come through the pick-up above (slot 426): with incendiary
    // or explosive rounds in the gun on the ground or in the hands (and for guns a player dropped), the game calls
    // the gun's own Use instead,
    // CTerrorGun::Use (0x103E8700, slot 106 of every gun's vtable; thiscall (activator, caller, type, value),
    // ret 0x10), which equips it with Weapon_Equip (player slot 281) and so swaps out, and drops, the gun in the
    // hands.  It gets the same choice (weapon_pickup).
    core::Hook<SpawnUseFn> g_gun_use;

    void gun_use(void* gun, void* edx, void* activator, void* caller, int type, float value)
    {
        MapGunReserve map_gun;
        cut_map_gun(activator, gun, map_gun);
        const bool local = !g_giving && activator != nullptr && activator == local_player();
        if (local)
        {
            const WeaponPickup pickup = weapon_pickup(activator, gun, item_of(gun));
            if (pickup != WeaponPickup::Game)
                return;
        }
        KeepRounds keep(local ? activator : nullptr, gun); // (the game's pick-up or swap: its rounds stay)
        g_gun_use.original()(gun, edx, activator, caller, type, value);
    }

    void __fastcall hk_gun_use(void* gun, void* edx, void* activator, void* caller, int type, float value)
    {
        if (served_remotely(activator))
        {
            Activation active(activator);
            if (!active.ok())
            {
                g_gun_use.original()(gun, edx, activator, caller, type, value);
                return;
            }
            gun_use(gun, edx, activator, caller, type, value);
            return;
        }
        gun_use(gun, edx, activator, caller, type, value);
    }

    void install_pickup_hook()
    {
        // the pickup hook (optional): the function must start the way it did when it was read
        const std::uintptr_t pickup = mem::read_or<std::uintptr_t>(g.player_vtable + 4u * k_vt_use_pickup);
        if (pickup != 0 && mem::find_pattern(pickup, 11, "55 8B EC 83 EC 08 53 57 8B 7D 08") == pickup)
        {
            if (g_bump_weapon.create("UsePickup", reinterpret_cast<void*>(pickup), hk_bump_weapon))
                g_bump_weapon.enable();
        }
        else
            INV_LOG("game: the +use pickup function (%p) does not look as expected: no pickup hook",
                    reinterpret_cast<void*>(pickup));
    }

    void install_spawner_hooks()
    {
        // map item spawners (optional): CWeaponSpawn::Use and Weapon_GetSlot, both checked against what was read
        {
            const std::uintptr_t use = mem::read_or<std::uintptr_t>(g.server.base + k_rva_spawn_vtable + 4u * k_vt_entity_use);
            const std::uintptr_t get_slot = g.server.base + k_rva_weapon_get_slot;
            const std::uintptr_t id_name = g.server.base + k_rva_weapon_id_name;
            if (use != 0 && mem::find_pattern(use, 13, "55 8B EC 83 EC 2C 53 57 8B 7D 08 8B D9") == use &&
                mem::find_pattern(get_slot, 14, "55 8B EC 8B 45 08 0F B6 84 08 00 18 00 00") == get_slot &&
                mem::find_pattern(id_name, 12, "55 8B EC 8B 45 08 8D 48 FF 83 F9 36") == id_name)
            {
                g_weapon_id_name = reinterpret_cast<WeaponIdNameFn>(id_name);
                g_spawner_use = use; // (what a spawner's slot 106 holds: the unload key finds the map's gun spawners by it)
                if (g_get_slot.create("WeaponGetSlot", reinterpret_cast<void*>(get_slot), hk_get_slot) &&
                    g_spawn_use.create("WeaponSpawnUse", reinterpret_cast<void*>(use), hk_spawn_use))
                {
                    g_get_slot.enable();
                    g_spawn_use.enable();
                }
            }
            else
                INV_LOG("game: the item spawner functions do not look as expected: a spawner refuses when the slot is full");
        }
    }

    void install_gun_use()
    {
        // a gun's own Use (optional, see hk_gun_use): slot 106 of CTerrorGun's vtable (0x106D5544, RTTI)
        {
            const std::uintptr_t gun_use = g.server.base + k_rva_gun_use;
            if (mem::read_or<std::uintptr_t>(g.server.base + 0x6D5544 + 4u * 106) == gun_use &&
                mem::find_pattern(gun_use, 15, "55 8B EC 83 EC 08 56 57 8B 7D 08 8B F1 85 FF") == gun_use &&
                g_gun_use.create("GunUse", reinterpret_cast<void*>(gun_use), hk_gun_use))
                g_gun_use.enable();
            else
                INV_LOG("game: CTerrorGun::Use does not look as expected: +use on a gun with special rounds swaps it");
        }
    }

} // namespace game::detail
