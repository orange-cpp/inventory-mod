// The unload key: a gun's magazine into the player's reserve, in the hands, on the ground or from a spawner.

#include "game/unload.hpp"

#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/map_guns.hpp"
#include "game/pickup.hpp"
#include "game/rounds.hpp"
#include "game/weapons.hpp"
#include "inv/rules.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <intrin.h>
#include <vector>

namespace game::detail
{

    // ---- unloading a gun (the unload key) ---------------------------------------------------------------------------
    //
    // The gun in the player's hands, or one lying in front of them, is emptied: its magazine (m_iClip1) goes into the
    // player's reserve of its ammo type, as many rounds as fit (the type's most, the weight limit); the rest is
    // dropped in front of them as a pile of rounds (do_drop_ammo), so nothing is lost and nothing stays in the gun.
    // Incendiary / explosive rounds loaded (m_nUpgradedPrimaryAmmoLoaded) are rounds of that magazine: they come out
    // as plain rounds, and the gun loses that upgrade (its laser sight stays).  See "Unloading a gun" in
    // docs/game-notes.md.

    // CBaseCombatWeapon::ScriptSetClip1(int) (server.dll 0x103F0650, the VScript "SetClip1": registered at 0x103F2F1C
    // with the name "SetClip1"): thiscall, ret 4; it writes m_iClip1 (+0x1414) and marks the edict changed, as the
    // game's own writes of it do.

    SetClip1Fn set_clip1_fn()
    {
        static SetClip1Fn fn = [] {
            const std::uintptr_t at = g.server.base + k_rva_set_clip1;
            if (mem::find_pattern(at, 16, "55 8B EC 56 57 8B 7D 08 8B F1 39 BE 14 14 00 00") == at)
                return reinterpret_cast<SetClip1Fn>(at);
            INV_LOG("game: SetClip1 does not look as expected: unloading writes m_iClip1 itself");
            return static_cast<SetClip1Fn>(nullptr);
        }();
        return fn;
    }

    // A gun emptied in the hands would be filled again at once: the game reloads an empty gun by itself (its
    // ItemPostFrame calls Reload, vtable slot 281, while m_iClip1 is 0; seen in the game: a Sniper Rifle unloaded
    // from 8 rounds held 28 a moment later, the reserve 0).  So until its owner presses reload or fire
    // (CBasePlayer::m_nButtons +0x1CB4: IN_ATTACK 1, IN_RELOAD 0x2000) the mod's Reload hooks answer false for it:
    // CTerrorGun::Reload (0x103E8860, slot 281 of CTerrorGun's vtable 0x106D5544 and of every rifle, SMG, sniper
    // rifle's ...) and the shotguns' (0x103C2F80, slot 281 of CPumpShotgun 0x106CC994, CShotgun_Chrome, CAutoShotgun,
    // CShotgun_SPAS).  Both are bool __thiscall Reload().  A gun nobody holds any more is let go.
    core::Hook<ReloadFn> g_gun_reload, g_shotgun_reload;
    std::vector<std::uint32_t> g_unloaded_guns; // EHANDLEs; main thread (Reload runs there too)

    bool reload_held_back(void* gun, void* caller)
    {
        if (g_unloaded_guns.empty())
            return false;
        const auto it = std::find(g_unloaded_guns.begin(), g_unloaded_guns.end(), handle_of(gun));
        if (it == g_unloaded_guns.end())
            return false;
        void* owner = entity_from_handle(handle_at(gun, k_off_weapon_owner));
        const int buttons = owner != nullptr ? mem::read_or<int>(reinterpret_cast<std::uintptr_t>(owner) + k_off_buttons, 0) : 0;
        if (owner == nullptr || (buttons & (k_in_attack | k_in_reload)) != 0)
        {
            INV_LOG("unloaded gun %p (server): reload let through (buttons %X, called from server.dll+0x%X)", gun,
                    static_cast<unsigned>(buttons), static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(caller) - g.server.base));
            g_unloaded_guns.erase(it);
            return false;
        }
        return true;
    }

    bool __fastcall hk_gun_reload(void* gun, void* edx)
    {
        return reload_held_back(gun, _ReturnAddress()) ? false : g_gun_reload.original()(gun, edx);
    }

    bool __fastcall hk_shotgun_reload(void* gun, void* edx)
    {
        return reload_held_back(gun, _ReturnAddress()) ? false : g_shotgun_reload.original()(gun, edx);
    }

    // Fire on a gun emptied by the unload key, with its rounds in the reserve: the guns' frame update, the base
    // CBaseCombatWeapon::ItemPostFrame (0x1004D7C0), sees IN_ATTACK with m_iClip1 <= 0 (0x1004D9F8) and calls
    // HandleFireOnEmpty (vtable +0x450, 0x1004ABD0).  While m_bFireOnEmpty (datamap, FIELD_BOOLEAN, +0x144E) is false
    // it plays the game's dry fire and sets it; only after that does it call ReloadOrSwitchWeapons (+0x434,
    // 0x1004D590: m_iClip1 0 and both attack times passed -> Reload, +0x464; seen: Reload called from
    // server.dll+0x4D714).  The game never gets there with rounds in the reserve (it has reloaded by then), so both
    // sides hook HandleFireOnEmpty (server 0x1004ABD0, prologue 56 8B F1 80 BE 4E 14 00 00 00; client 0x100156D0, the
    // same code with +0x9BE, its datamap entry "m_bFireOnEmpty" at 0x106BCECC; both void __thiscall) and set the flag
    // first when the magazine is empty and the owner has rounds of the gun's type: fire reloads at once, on both sides
    // alike.  (The "ghost shot" seen while this was worked out was the client's reload hook on the wrong function;
    // see the client's half of reload_held_back.  Fire went straight to the reload once that was fixed, with this.)
    core::Hook<FireOnEmptyFn> g_fire_on_empty;

    void __fastcall hk_fire_on_empty(void* weapon, void* edx)
    {
        const auto w = reinterpret_cast<std::uintptr_t>(weapon);
        if (mem::read_or<int>(w + k_off_clip, -1) == 0)
        {
            void* owner = entity_from_handle(handle_at(weapon, k_off_weapon_owner));
            const int type = ammo_type_of(weapon);
            if (owner != nullptr && type >= 0 && mem::read_or<int>(reinterpret_cast<std::uintptr_t>(reserve_of(owner, type)), 0) > 0)
                mem::guarded([&] { *reinterpret_cast<std::uint8_t*>(w + k_off_fire_on_empty) = 1; });
        }
        g_fire_on_empty.original()(weapon, edx);
    }

    void install_reload_hooks()
    {
        const std::uintptr_t gun = g.server.base + k_rva_gun_reload;
        if (mem::read_or<std::uintptr_t>(g.server.base + 0x6D5544 + 4u * k_vt_reload) == gun &&
            mem::find_pattern(gun, 11, "55 8B EC 83 EC 08 53 56 8B F1 E8") == gun &&
            g_gun_reload.create("GunReload", reinterpret_cast<void*>(gun), hk_gun_reload))
            g_gun_reload.enable();
        else
            INV_LOG("game: CTerrorGun::Reload does not look as expected: a gun unloaded in the hands reloads at once");
        const std::uintptr_t shotgun = g.server.base + k_rva_shotgun_reload;
        if (mem::read_or<std::uintptr_t>(g.server.base + k_rva_pump_shotgun_vtable + 4u * k_vt_reload) == shotgun &&
            mem::find_pattern(shotgun, 11, "55 8B EC 83 EC 08 56 57 8B F1 E8") == shotgun &&
            g_shotgun_reload.create("ShotgunReload", reinterpret_cast<void*>(shotgun), hk_shotgun_reload))
            g_shotgun_reload.enable();
        else
            INV_LOG("game: the shotguns' Reload does not look as expected: a shotgun unloaded in the hands reloads at once");
        const std::uintptr_t fire_on_empty = g.server.base + k_rva_fire_on_empty;
        if (mem::find_pattern(fire_on_empty, 10, "56 8B F1 80 BE 4E 14 00 00 00") == fire_on_empty &&
            g_fire_on_empty.create("FireOnEmpty", reinterpret_cast<void*>(fire_on_empty), hk_fire_on_empty))
            g_fire_on_empty.enable();
        else
            INV_LOG("game: HandleFireOnEmpty does not look as expected: fire on an unloaded gun dry-fires first");
    }

    // What the player can still carry by the weight limit of session()'s inventory; -1 for no limit.
    float weight_room(void* player)
    {
        const AmmoPool pool = read_pool(player);
        Session& s = session();
        std::lock_guard lock(s.mutex);
        const float max_weight = s.inventory.limits().max_weight;
        if (max_weight <= 0.f)
            return -1.f;
        const float load = s.inventory.weight() - s.inventory.ammo_weight() + (pool.read ? inv::ammo_weight(pool.count) : 0.f);
        return std::max(0.f, max_weight - load);
    }

    // A map's gun spawner (weapon_rifle_spawn ...: its slot 106 is CWeaponSpawn::Use) of a gun that can be unloaded
    // lying in front of the eyes, as item_in_front finds items; nullptr when there is none.
    void* spawner_in_front(const Vec3& eye, const Vec3& forward, float min_facing, const inv::ItemDef** found_def,
                           float* found_score)
    {
        void* best = nullptr;
        *found_score = 1e30f;
        if (g_spawner_use == 0 || g_weapon_id_name == nullptr)
            return nullptr;
        for (int index = 1; index < 0x1000; ++index)
        {
            void* entity = entity_at_index(index);
            Vec3 origin{};
            if (entity == nullptr || mem::vslot(entity, k_vt_entity_use) != g_spawner_use ||
                !mem::read(reinterpret_cast<std::uintptr_t>(entity) + k_off_origin, origin))
                continue;
            const Vec3 d{origin.x - eye.x, origin.y - eye.y, origin.z - eye.z};
            const float distance = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (distance > k_pickup_reach || distance < 1.f)
                continue;
            const float facing = (d.x * forward.x + d.y * forward.y + d.z * forward.z) / distance;
            if (facing < min_facing)
                continue;
            const inv::ItemDef* def = nullptr;
            const auto id_of = reinterpret_cast<WeaponIdFn>(mem::vslot(entity, k_vt_spawn_weapon_id));
            mem::guarded([&] {
                const int id = id_of != nullptr ? id_of(entity) : 0;
                if (const char* name = id > 0 ? g_weapon_id_name(id) : nullptr)
                    def = inv::find_item(name);
            });
            if (def == nullptr || !inv::unloadable(def->classname))
                continue;
            const float score = distance * (2.f - facing);
            if (score < *found_score)
            {
                *found_score = score;
                best = entity;
                *found_def = def;
            }
        }
        return best;
    }

    // One gun out of a map's spawner, as a real gun lying on the map that the unload key then empties: it is made
    // with "origin" and "angles" set before DispatchSpawn (never moved: see the vphysics notes), with the magazine
    // a pick-up from that spawner would give ([map_guns] cuts it).  The spawner gives one less: one with a count of
    // 1 (or any with [map_guns] spawners_once) is removed and the gun lies where it stood; one that stays (more in
    // it, or endless) gets the gun 24 units beside it, on the floor, not inside it.  nullptr when the game would not
    // make the gun.
    void* gun_from_spawner(void* spawner, const inv::ItemDef& def)
    {
        const auto sp = reinterpret_cast<std::uintptr_t>(spawner);
        Vec3 origin{}, angles{};
        mem::read(sp + k_off_origin, origin);
        mem::read(sp + k_off_spawner_angles, angles);
        const int count = mem::read_or<int>(sp + k_off_spawn_count, 0);
        const inv::MapGunRules& rules = inv::map_gun_rules();
        const bool stays = count != 1 && !(rules.enabled && rules.spawners_once);
        Vec3 at = origin;
        if (stays)
        {
            const float yaw = (angles.y + 90.f) * 3.14159265f / 180.f;
            at = on_floor(Vec3{origin.x + 24.f * std::cos(yaw), origin.y + 24.f * std::sin(yaw), origin.z + 16.f});
        }
        void* gun = nullptr;
        mem::guarded([&] {
            gun = g.create_entity(g.tools, def.classname);
            if (gun == nullptr)
                return;
            char text[96];
            std::snprintf(text, sizeof(text), "%.2f %.2f %.2f", at.x, at.y, at.z);
            g.set_key_value(g.tools, gun, "origin", text);
            std::snprintf(text, sizeof(text), "%.2f %.2f %.2f", angles.x, angles.y, angles.z);
            g.set_key_value(g.tools, gun, "angles", text);
            if (const auto precache = reinterpret_cast<PrecacheFn>(mem::vslot(gun, k_vt_precache)))
                precache(gun);
            g.dispatch_spawn(g.tools, gun);
        });
        if (gun == nullptr)
            return nullptr;
        const auto w = reinterpret_cast<std::uintptr_t>(gun);
        const int full = mem::read_or<int>(w + k_off_clip, -1);
        if (rules.enabled && full > 0)
            mem::guarded([&] {
                *reinterpret_cast<int*>(w + k_off_clip) = rolled_map_clip(full);
                *reinterpret_cast<int*>(w + k_off_extra_ammo) = 0;
            });
        know_gun(gun);
        if (!stays)
            mem::guarded([&] { g.util_remove(spawner); });
        else if (count > 1)
            mem::guarded([&] { *reinterpret_cast<int*>(sp + k_off_spawn_count) = count - 1; });
        INV_LOG("unload: spawner %p (%s, count %d) gave %p at %.1f %.1f %.1f with %d of %d rounds; the spawner %s", spawner,
                def.classname, count, gun, at.x, at.y, at.z, mem::read_or<int>(w + k_off_clip, -1), full,
                stays ? "stays" : "is gone");
        return gun;
    }

    void do_unload()
    {
        void* player = local_player();
        Vec3 eye{}, forward{};
        if (player == nullptr || !player_view(player, eye, forward))
        {
            notify("Not in a game");
            return;
        }
        // a gun on the ground the player looks straight at (a narrower cone than the pick-up's: the gun in the hands
        // is the usual one), else the gun in the hands
        const inv::ItemDef* def = nullptr;
        float gun_score = 1e30f, spawner_score = 1e30f;
        void* gun = item_in_front(eye, forward, 0.9f, [](const inv::ItemDef& d) { return inv::unloadable(d.classname); }, &def,
                                  &gun_score);
        // or a map's gun spawner (it hands out a fresh gun: one comes out of it, empty)
        const inv::ItemDef* spawner_def = nullptr;
        if (void* spawner = spawner_in_front(eye, forward, 0.9f, &spawner_def, &spawner_score);
            spawner != nullptr && spawner_score < gun_score)
        {
            gun = gun_from_spawner(spawner, *spawner_def);
            def = spawner_def;
            if (gun == nullptr)
            {
                notify(std::string("Cannot unload ") + spawner_def->name);
                return;
            }
        }
        const bool on_ground = gun != nullptr;
        if (gun == nullptr)
        {
            gun = entity_from_handle(handle_at(player, k_off_active_weapon));
            def = item_of(gun);
        }
        if (gun == nullptr || def == nullptr || !inv::unloadable(def->classname))
        {
            notify(def != nullptr && def->weapon() ? std::string(def->name) + " cannot be unloaded" : "No gun to unload");
            return;
        }
        const char* kind = inv::ammo_kind(def->classname);
        const char* title = inv::ammo_title(kind);
        const int type = ammo_type_of(gun);
        const auto w = reinterpret_cast<std::uintptr_t>(gun);
        const int clip = mem::read_or<int>(w + k_off_clip, -1);
        if (type < 0 || title == nullptr || type != inv::ammo_index(kind))
        {
            INV_LOG("unload: %s has ammo type %d, not %s's %d", def->classname, type, kind, inv::ammo_index(kind));
            notify(std::string("Cannot unload ") + def->name);
            return;
        }
        if (!on_ground && mem::read_or<std::uint8_t>(w + k_off_in_reload, 0) != 0)
        {
            notify("Wait for the reload to finish");
            return;
        }
        if (clip <= 0)
        {
            notify(std::string(def->name) + " is empty");
            return;
        }

        const AmmoPool pool = read_pool(player);
        const inv::UnloadSplit split = inv::split_unload(kind, clip, pool.read ? pool.count[type] : 0, weight_room(player));
        const int upgrades = mem::read_or<int>(w + k_off_upgrades, 0);
        const int special = mem::read_or<int>(w + k_off_special_loaded, 0);

        // the magazine out: m_iClip1 0 through the game's setter (it marks the edict changed), and no special rounds
        bool emptied = false;
        if (const SetClip1Fn set_clip = set_clip1_fn())
            emptied = mem::guarded([&] { set_clip(gun, 0); });
        if (!emptied)
            emptied = mem::guarded([&] { *reinterpret_cast<int*>(w + k_off_clip) = 0; });
        if (!emptied)
        {
            notify(std::string("Cannot unload ") + def->name);
            return;
        }
        if (special > 0 || (upgrades & 3) != 0)
            mem::guarded([&] {
                *reinterpret_cast<int*>(w + k_off_special_loaded) = 0;
                *reinterpret_cast<int*>(w + k_off_upgrades) = upgrades & ~3;
            });
        state_changed(gun);
        if (on_ground)
            know_gun(gun); // [map_guns] must not cut (or fill) its magazine when someone picks it up
        else if (const std::uint32_t handle = handle_of(gun); handle != k_invalid_handle)
        {
            // no reload by itself (see reload_held_back); the guns that are gone are forgotten
            std::erase_if(g_unloaded_guns, [](std::uint32_t h) { return entity_from_handle(h) == nullptr; });
            if (std::find(g_unloaded_guns.begin(), g_unloaded_guns.end(), handle) == g_unloaded_guns.end())
                g_unloaded_guns.push_back(handle);
        }

        // the rounds into the reserve; what does not fit through it onto the ground (do_drop_ammo drops from it)
        mem::guarded([&] { *reserve_of(player, type) += clip; });
        state_changed(player);
        INV_LOG("unload: %s %p (%s) had %d rounds (%d special, upgrades %d): %d into the reserve (%d before), %d to the "
                "ground",
                def->classname, gun, on_ground ? "on the ground" : "in the hands", clip, special, upgrades,
                split.to_reserve, pool.read ? pool.count[type] : -1, split.to_ground);
        if (split.to_ground > 0)
            do_drop_ammo(kind, split.to_ground);
        std::string text = "Unloaded " + std::to_string(clip) + " " + title;
        if (split.to_ground > 0)
            text += " (" + std::to_string(split.to_ground) + " dropped: no room)";
        notify(text);
    }

} // namespace game::detail
