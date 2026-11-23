// Guns, melee weapons and items between the player's hands, the inventory's boxes and the ground.

#include "game/weapons.hpp"

#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/map_guns.hpp"
#include "game/melee.hpp"
#include "game/native_slots.hpp"
#include "game/fuel.hpp"
#include "game/revive.hpp"
#include "inv/rules.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace game::detail
{

    // ---- the actions --------------------------------------------------------------------------------------------

    void take_from_inventory(const inv::ItemDef& def, int count)
    {
        Session& s = session();
        std::lock_guard lock(s.mutex);
        s.inventory.remove_item(def, count);
    }

    // ---- guns and melee weapons --------------------------------------------------------------------------------
    //
    // A stored weapon keeps its magazine (a melee weapon: which one it is).  The reserve is the player's: m_iAmmo of
    // each ammo type, shared by every gun that takes it (M16, AK-47, SCAR and SG552 share rifle rounds ...), the way
    // the game shares it between the guns a player picks up.  So a stored gun leaves its reserve with the player, a
    // gun taken out uses what the player has of its type, and a gun picked up off the ground into a box adds what it
    // carried (m_iExtraPrimaryAmmo) to the player's.

    // What `weapon` is in: its magazine, and for a gun lying in the world the reserve it carries (-1: none, or a
    // fresh gun from the map, which has 0 there).  A carried gun's reserve is the player's, so not part of it.
    // A gun (CTerrorGun): not the melee weapon or the chainsaw.
    bool is_gun(const inv::ItemDef& def)
    {
        return def.weapon() && std::strcmp(def.classname, "weapon_melee") != 0 && std::strcmp(def.classname, "weapon_chainsaw") != 0;
    }


    // A gun's upgrades and special rounds onto `weapon` (one just given, or dropped into the world).
    void write_upgrades(void* weapon, const inv::ItemDef& def, const inv::WeaponState& state)
    {
        if (!is_gun(def) || (state.upgrades == 0 && state.special == 0))
            return;
        const auto w = reinterpret_cast<std::uintptr_t>(weapon);
        if (mem::guarded([&] {
                *reinterpret_cast<int*>(w + k_off_upgrades) = state.upgrades;
                *reinterpret_cast<int*>(w + k_off_special_loaded) = state.special;
            }))
            state_changed(weapon);
    }

    inv::WeaponState weapon_state(void* player, void* weapon, const inv::ItemDef& def, bool carried)
    {
        (void)player;
        const bool is_melee = std::strcmp(def.classname, "weapon_melee") == 0;
        inv::WeaponState state;
        const auto w = reinterpret_cast<std::uintptr_t>(weapon);
        mem::guarded([&] {
            state.clip = mem::read_or<int>(w + k_off_clip, -1);
            if (!carried && !is_melee)
                if (const int extra = mem::read_or<int>(w + k_off_extra_ammo, 0); extra > 0)
                    state.ammo = extra;
            char name[64] = {};
            if (const char* melee = is_melee ? mem::read_or<const char*>(w + k_off_melee_name) : nullptr;
                melee != nullptr && mem::safe_copy(name, melee, sizeof(name) - 1) && name[0] != '\0')
                state.melee = name;
            if (is_melee)
                state.wear = melee_wear(weapon); // [melee] durability
            if (is_gun(def))
            {
                state.upgrades = mem::read_or<int>(w + k_off_upgrades, 0) & 7;
                state.special = std::max(0, mem::read_or<int>(w + k_off_special_loaded, 0));
            }
        });
        return state;
    }

    // The ammo type the mod makes a gun of game type `type` take (retarget_ammo_types); `type` itself for the rest.
    static int shared_ammo_type(int type)
    {
        switch (type)
        {
        case k_ammo_type_autoshotgun: return k_ammo_type_shotgun;
        case k_ammo_type_pistol: return k_ammo_type_smg;
        case k_ammo_type_magnum: return k_ammo_type_sniperrifle;
        default: return type;
        }
    }

    int ammo_type_of(void* weapon)
    {
        const int type = shared_ammo_type(mem::read_or<int>(reinterpret_cast<std::uintptr_t>(weapon) + k_off_ammo_type, -1));
        return type >= 0 && type < 32 ? type : -1; // (a gun nobody has carried yet: retarget_ammo_types)
    }

    AmmoPool read_pool(void* player)
    {
        AmmoPool pool;
        pool.read = mem::safe_copy(pool.count, reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(player) + k_off_player_ammo),
                                   sizeof(pool.count));
        return pool;
    }

    // Puts the reserve back as it was: whatever the game did to it while weapons were dropped and given (a given
    // gun would come with the game's own amount, a dropped one may take the reserve along).
    void write_pool(void* player, const AmmoPool& pool)
    {
        if (!pool.read)
            return;
        mem::guarded([&] {
            std::memcpy(reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(player) + k_off_player_ammo), pool.count,
                        sizeof(pool.count));
        });
    }

    // `rounds` of ammo type `type` (the ammo of gun `classname`) into the player's reserve, up to the most a player
    // can carry of it (inv::ammo_max: the rules file's [ammo], else the game's ammo_<type>_max default).
    void add_to_pool(void* player, const char* classname, int type, int rounds)
    {
        if (player == nullptr || type < 0 || type >= 32 || rounds <= 0)
            return;
        const char* kind = classname != nullptr ? inv::ammo_kind(classname) : nullptr;
        const int max = kind != nullptr ? inv::ammo_max(kind) : -1;
        mem::guarded([&] {
            int& reserve = *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(player) + k_off_player_ammo +
                                                   4u * static_cast<unsigned>(type));
            const int before = reserve;
            reserve = max >= 0 ? std::min(before + rounds, std::max(before, max)) : before + rounds;
            if (reserve - before < rounds)
                INV_LOG("ammo: %d of %d %s rounds taken (the most is %d)", reserve - before, rounds, kind, max);
        });
    }

    // A stored weapon's state onto the one the player was just given: its magazine; a reserve of its own (a gun
    // stored before the reserves were shared) joins the player's.
    void apply_weapon_state(void* player, void* weapon, const inv::WeaponState& state)
    {
        mem::guarded([&] {
            if (state.clip >= 0)
                *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(weapon) + k_off_clip) = state.clip;
        });
        char name[64];
        const bool named = classname_of(weapon, name, sizeof(name));
        if (const inv::ItemDef* def = named ? inv::find_item(name) : nullptr)
            write_upgrades(weapon, *def, state); // incendiary / explosive rounds, laser sight
        add_to_pool(player, named ? name : nullptr, ammo_type_of(weapon), state.ammo);
        if (!state.melee.empty())
            set_melee_wear(weapon, state.wear); // [melee] durability
    }

    // What GiveNamedItem gets for a stored weapon: a melee weapon by its script name (as `give fireaxe` does).
    std::string give_name(const inv::ItemDef& def, const inv::WeaponState& state)
    {
        return def.weapon() && !state.melee.empty() ? state.melee : std::string(def.classname);
    }

    // Takes one stored `def` out of inventory slot `slot`; false when the slot no longer has one (the HUD changed it).
    bool take_from_slot(int slot, const inv::ItemDef& def)
    {
        Session& s = session();
        std::lock_guard lock(s.mutex);
        return s.inventory.slot(slot).def == &def && s.inventory.remove_from_slot(slot, 1) == 1;
    }

    // Every item keeps its own box.  An item in the player's hands is shown in its box (Stack::held); when it leaves
    // the hands for the inventory it goes back into that box, and an item taken in hand from a box makes that box its
    // home.  So nothing moves between boxes when the player switches items.

    // Takes `weapon` (carried in `def`'s game slot) off the player and back into its box (or anywhere it fits when
    // it has none).  UTIL_Remove alone is not enough: it only deletes at the end of the frame, so the weapon would
    // still be in m_hMyWeapons when GiveNamedItem runs right after, and the game would drop what occupies the slot
    // onto the ground.  Weapon_Drop (slot 283) takes it out of m_hMyWeapons now; the dropped entity is then removed.
    bool swap_out(void* player, void* weapon, const inv::ItemDef& def)
    {
        if (const auto cannot = reinterpret_cast<CannotDropFn>(mem::vslot(player, k_vt_cannot_drop));
            cannot != nullptr && cannot(player) != 0)
            return false;
        // the base class's Weapon_Drop: CTerrorPlayer's own one also fires "weapon_drop", and the client then pins a
        // "dropped item" icon in the world where the item never was
        const auto drop = g.quiet_drop != nullptr ? g.quiet_drop
                                                  : reinterpret_cast<WeaponDropFn>(mem::vslot(player, k_vt_weapon_drop));
        if (drop == nullptr)
            return false;
        int home = -1;
        bool full = false;
        const int spent = can_spent(weapon); // a gas can's: what it lacks comes back with it
        {
            Session& s = session();
            std::lock_guard lock(s.mutex);
            home = s.inventory.held_slot(def);
            full = home < 0 && s.inventory.add(def, 1, spent) == 0;
        }
        if (full)
        {
            notify(refusal(def)); // notify() takes the session lock itself
            return false;
        }
        if (!mem::guarded([&] { drop(player, weapon, nullptr, nullptr); }) || player_has_slot(player, def.game_slot))
        {
            if (home < 0)
                take_from_inventory(def, 1);
            return false;
        }
        mem::guarded([&] { g.util_remove(weapon); });
        if (home >= 0)
        {
            Session& s = session();
            std::lock_guard lock(s.mutex);
            s.inventory.release_held(home, spent);
        }
        INV_LOG("swap: %s went back into box %d", def.classname, home);
        return true;
    }

    // Takes the carried weapon `weapon` (a gun or melee weapon, `def`) off the player into inventory box `index`
    // (-1: the first empty one), with its rounds; the player keeps none of its ammo type.  Weapon_Drop first, as
    // in swap_out, so it is out of m_hMyWeapons before anything is given.  The box, or -1 (said why).
    int store_carried_weapon(void* player, void* weapon, const inv::ItemDef& def, int index = -1)
    {
        if (const auto cannot = reinterpret_cast<CannotDropFn>(mem::vslot(player, k_vt_cannot_drop));
            cannot != nullptr && cannot(player) != 0)
        {
            notify("The game does not let you put that away now");
            return -1;
        }
        const auto drop = g.quiet_drop != nullptr ? g.quiet_drop
                                                  : reinterpret_cast<WeaponDropFn>(mem::vslot(player, k_vt_weapon_drop));
        if (drop == nullptr)
            return -1;
        const inv::WeaponState state = weapon_state(player, weapon, def, true);
        int box = -1;
        {
            Session& s = session();
            std::lock_guard lock(s.mutex);
            // it leaves the hands for the box: what it weighs in the hands does not count twice
            const float carried = s.inventory.weapons_weight();
            s.inventory.set_weapons_weight(std::max(0.f, carried - def.weight));
            box = s.inventory.add_weapon(def, state, index);
            s.inventory.set_weapons_weight(carried);
        }
        if (box < 0)
        {
            notify(index >= 0 ? std::string("No room for your ") + def.name : refusal(def));
            return -1;
        }
        // the reserve stays the player's (shared by the guns of its ammo type), whatever the drop does with it
        const AmmoPool pool = read_pool(player);
        const bool dropped = mem::guarded([&] { drop(player, weapon, nullptr, nullptr); }) && !player_has_slot(player, def.game_slot);
        write_pool(player, pool);
        if (!dropped)
        {
            Session& s = session();
            std::lock_guard lock(s.mutex);
            s.inventory.remove_from_slot(box, 1);
            return -1;
        }
        mem::guarded([&] { g.util_remove(weapon); });
        INV_LOG("stored %s (%s) into box %d: magazine %d, upgrades %d, %d special rounds (the reserve stays with the player)",
                def.classname, state.melee.c_str(), box, state.clip, state.upgrades, state.special);
        return box;
    }

    // A carried item (gas can, propane / oxygen tank, fireworks, gnome, cola: game slot 5) cannot stay with the
    // player while the hands hold something else: the game drops it as soon as they switch.  Before the mod switches
    // (wheel, keys, the item boxes), it goes back into its box instead, the way the store key puts it away; with no
    // room for it the game drops it as before.
    void put_away_carried(void* player, const char* switching_to)
    {
        void* active = entity_from_handle(handle_at(player, k_off_active_weapon));
        const inv::ItemDef* def = item_of(active);
        if (def == nullptr || def->game_slot != 5 || (switching_to != nullptr && std::strcmp(switching_to, def->classname) == 0))
            return;
        if (swap_out(player, active, *def))
            INV_LOG("switching to %s: %s put away", switching_to != nullptr ? switching_to : "?", def->classname);
    }

    bool g_giving = false; // main thread only: do_use is handing an item out
    bool g_switching = false; // main thread only: do_use is swapping / handing out (game events are muted)
    ULONGLONG g_switch_mute_until = 0; // main thread only: "weapon_pickup" is muted until then (see hk_create_event)

    // engine.dll's IGameEventManager2 (GAMEEVENTSMANAGER002, vtable 0x10388774) slot 6 CreateEvent(name, force,
    // cookie), ret 0xC.  The server's code checks the result for null and then skips firing.  While the mod swaps
    // items the events the game creates for it ("item_pickup", ...) are not real pickups, and the client answers
    // them with item icons floating in the world, so they are not created at all.
    core::Hook<CreateEventFn> g_create_event;

    void* __fastcall hk_create_event(void* self, void* edx, const char* name, bool force, int* cookie)
    {
        if (g_switching && name != nullptr)
        {
            INV_LOG("event %s muted during a switch", name);
            return nullptr;
        }
        // the client's own "weapon_pickup" comes a frame or two later, when the new weapon reaches it; that is the
        // one the floating pickup icon belongs to
        if (name != nullptr && GetTickCount64() < g_switch_mute_until && std::strcmp(name, "weapon_pickup") == 0)
        {
            INV_LOG("event %s muted after a switch", name);
            return nullptr;
        }
        return g_create_event.original()(self, edx, name, force, cookie);
    }

    // A stored gun or melee weapon into the hands.  The one in its game slot goes into the same box (the two swap
    // places), then the stored one is given and gets its rounds back.
    void do_use_weapon(void* player, int slot)
    {
        const inv::ItemDef* def = nullptr;
        inv::WeaponState state;
        {
            Session& s = session();
            std::lock_guard lock(s.mutex);
            const inv::Stack& stack = s.inventory.slot(slot);
            if (stack.empty() || stack.def == nullptr || !stack.def->weapon())
                return;
            def = stack.def;
            state = stack.weapon;
            s.inventory.remove_from_slot(slot, 1); // the box is free for the weapon it replaces
        }
        const auto put_back = [&] {
            Session& s = session();
            std::lock_guard lock(s.mutex);
            if (s.inventory.add_weapon(*def, state, slot) < 0 && s.inventory.add_weapon(*def, state) < 0)
                INV_LOG("use %s: no box to put it back in", def->classname); // (cannot happen: the box was just freed)
        };
        INV_LOG("use %s (box %d, %s): the player carries [%s]", def->classname, slot, state.melee.c_str(),
                carried_list(player).c_str());
        g_switching = true;
        struct Unmute
        {
            ~Unmute()
            {
                g_switching = false;
                g_switch_mute_until = GetTickCount64() + 500;
            }
        } unmute;
        const inv::ItemDef* held_def = nullptr;
        if (void* held = carried_in_slot(player, def->game_slot, &held_def))
        {
            if (store_carried_weapon(player, held, *held_def, slot) < 0)
            {
                put_back();
                return;
            }
        }
        const auto give = reinterpret_cast<GiveFn>(mem::vslot(player, k_vt_give_named_item));
        const std::string name = give_name(*def, state);
        // the gun takes what the player has of its ammo type: not the game's own amount for a given gun
        const AmmoPool pool = read_pool(player);
        g_giving = true;
        const bool gave = give != nullptr && mem::guarded([&] { give(player, name.c_str(), 0, 0, 0); });
        g_giving = false;
        write_pool(player, pool);
        const inv::ItemDef* given_def = nullptr;
        void* given = carried_in_slot(player, def->game_slot, &given_def);
        if (!gave || given == nullptr)
        {
            // the one that went into the box stays there; this one goes into the first free box
            Session& s = session();
            std::lock_guard lock(s.mutex);
            if (s.inventory.add_weapon(*def, state, slot) < 0 && s.inventory.add_weapon(*def, state) < 0)
                INV_LOG("use %s: the game refused it and no box is free: it is lost", def->classname);
            notify(std::string("The game refused the ") + def->name);
            return;
        }
        apply_weapon_state(player, given, state);
        if (state.upgrades != 0 || state.special != 0)
            INV_LOG("upgrades back on %s: %d (now %d), %d special rounds (now %d)", given_def->classname, state.upgrades,
                    mem::read_or<int>(reinterpret_cast<std::uintptr_t>(given) + k_off_upgrades),
                    state.special, mem::read_or<int>(reinterpret_cast<std::uintptr_t>(given) + k_off_special_loaded));
        do_select_weapon(given_def->classname);
        INV_LOG("equipped %s", state.melee.empty() ? def->name : state.melee.c_str()); // (in the log only: the hands show it)
    }

    void do_use(int slot)
    {
        const inv::ItemDef* def = nullptr;
        bool stored = false;
        {
            Session& s = session();
            std::lock_guard lock(s.mutex);
            const inv::Stack& stack = s.inventory.slot(slot);
            if (stack.empty())
                return;
            def = stack.def;
            stored = stack.count > 0;
        }
        void* player = local_player();
        if (player == nullptr)
        {
            notify("Not in a game");
            return;
        }
        // down, with an item that gets them up ([self_revive]): it is used where it is, not taken in hand
        if (start_self_revive(player, *def, slot))
            return;
        put_away_carried(player, def->classname);
        if (def->weapon())
        {
            do_use_weapon(player, slot);
            return;
        }
        INV_LOG("use %s (box %d): the player carries [%s]", def->classname, slot, carried_list(player).c_str());
        g_switching = true;
        struct Unmute
        {
            ~Unmute()
            {
                g_switching = false;
                g_switch_mute_until = GetTickCount64() + 500;
            }
        } unmute;
        const inv::ItemDef* held_def = nullptr;
        void* held = carried_in_slot(player, def->game_slot, &held_def);
        // that item is in the hands already (this box shows the held one, or the same item is held): just switch to it
        if (!stored || (held != nullptr && held_def == def))
        {
            do_select_weapon(def->classname);
            INV_LOG("equipped %s", def->name); // (in the log only: the hands show it)
            return;
        }
        // like the game's panel switching slots: what occupies the slot goes back into its own box, then ours comes out
        if (held != nullptr && !swap_out(player, held, *held_def))
        {
            notify(std::string("Your hands are full: no room for ") + def->name);
            return;
        }
        int spent = 0; // a gas can's: what it lacks goes with it (the box's full ones go first)
        {
            Session& s = session();
            std::lock_guard lock(s.mutex);
            spent = s.inventory.next_spent(slot);
        }
        if (!take_from_slot(slot, *def))
            return;
        const auto give = reinterpret_cast<GiveFn>(mem::vslot(player, k_vt_give_named_item));
        g_giving = true; // the +use hook must not store what we are handing out
        const bool gave = give != nullptr && mem::guarded([&] { give(player, def->classname, 0, 0, 0); });
        g_giving = false;
        // GiveNamedItem's result is not reliable: look whether the player has it now
        const inv::ItemDef* given_def = nullptr;
        void* given = gave ? carried_in_slot(player, def->game_slot, &given_def) : nullptr;
        if (given == nullptr)
        {
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                if (s.inventory.add_to_slot(slot, *def, 1, spent) == 0)
                    s.inventory.add(*def, 1, spent);
            }
            notify(gave ? std::string("Could not equip ") + def->name : std::string("The game refused the item"));
            return;
        }
        if (spent > 0)
            set_can_spent(given, spent);
        {
            Session& s = session();
            std::lock_guard lock(s.mutex);
            s.inventory.set_held(slot, *def); // this box is the held item's home
        }
        do_select_weapon(def->classname); // the game does not switch to a given item by itself
        INV_LOG("equipped %s", def->name); // (in the log only: the hands show it)
    }

    bool player_view(void* player, Vec3& eye, Vec3& forward)
    {
        const auto eye_position = reinterpret_cast<EyePositionFn>(mem::vslot(player, k_vt_eye_position));
        const auto eye_angles = reinterpret_cast<EyeAnglesFn>(mem::vslot(player, k_vt_eye_angles));
        Vec3 angles{};
        if (eye_position == nullptr || eye_angles == nullptr ||
            !mem::guarded([&] {
                Vec3 tmp{};
                eye = *eye_position(player, &tmp);
                angles = *eye_angles(player);
            }))
            return false;
        constexpr float k_pi = 3.14159265f;
        const float pitch = angles.x * k_pi / 180.f;
        const float yaw = angles.y * k_pi / 180.f;
        forward = {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
        return true;
    }

    // What the survivors hold (their m_hMyWeapons): it is not on the ground.
    std::vector<void*> carried_by_players()
    {
        std::vector<void*> carried;
        for (int index = 1; index <= 32; ++index)
        {
            void* other = entity_at_index(index);
            if (other == nullptr ||
                mem::read_or<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(other)) != g.player_vtable)
                continue;
            for (int i = 0; i < k_max_weapons; ++i)
                if (void* weapon =
                        entity_from_handle(handle_at(other, k_off_my_weapons + 4u * static_cast<unsigned>(i))))
                    carried.push_back(weapon);
        }
        return carried;
    }

    void* item_in_front(const Vec3& eye, const Vec3& forward, float min_facing, bool (*wanted)(const inv::ItemDef&),
                        const inv::ItemDef** found_def, float* found_score)
    {
        const std::vector<void*> carried = carried_by_players();
        void* best = nullptr;
        float best_score = 1e30f;
        for (int index = 1; index < 0x1000; ++index)
        {
            void* entity = entity_at_index(index);
            Vec3 origin{};
            if (entity == nullptr || !mem::read(reinterpret_cast<std::uintptr_t>(entity) + k_off_origin, origin))
                continue;
            const Vec3 d{origin.x - eye.x, origin.y - eye.y, origin.z - eye.z};
            const float distance = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (distance > k_pickup_reach || distance < 1.f)
                continue;
            const float facing = (d.x * forward.x + d.y * forward.y + d.z * forward.z) / distance;
            if (facing < min_facing)
                continue;
            const inv::ItemDef* def = item_of(entity);
            if (def == nullptr || !wanted(*def) || std::find(carried.begin(), carried.end(), entity) != carried.end())
                continue;
            const float score = distance * (2.f - facing);
            if (score < best_score)
            {
                best_score = score;
                best = entity;
                *found_def = def;
            }
        }
        if (found_score != nullptr)
            *found_score = best_score;
        return best;
    }

    // Takes the closest inventory item that lies in front of the player and is not carried by anyone.
    void do_pickup_nearby()
    {
        void* player = local_player();
        Vec3 eye{}, forward{};
        if (player == nullptr || !player_view(player, eye, forward))
        {
            notify("Not in a game");
            return;
        }
        const inv::ItemDef* best_def = nullptr;
        void* best = item_in_front(eye, forward, 0.6f, [](const inv::ItemDef&) { return true; }, &best_def);
        if (best == nullptr)
        {
            notify("Nothing to pick up in front of you");
            return;
        }

        int stored = 0;
        int weapon_box = -1;
        int reserve = -1, reserve_type = -1; // a gun's own reserve: into the player's (shared by its ammo type)
        {
            inv::WeaponState state = best_def->weapon() ? weapon_state(player, best, *best_def, false) : inv::WeaponState{};
            reserve = state.ammo;
            reserve_type = ammo_type_of(best);
            state.ammo = -1;
            Session& s = session();
            std::lock_guard lock(s.mutex);
            if (best_def->weapon())
            {
                weapon_box = s.inventory.add_weapon(*best_def, state); // its magazine stays with it
                stored = weapon_box >= 0 ? 1 : 0;
            }
            else
                stored = s.inventory.add(*best_def, 1, can_spent(best));
        }
        if (stored == 0)
        {
            notify(refusal(*best_def));
            return;
        }
        if (!mem::guarded([&] { g.util_remove(best); }))
        {
            if (weapon_box >= 0)
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                s.inventory.remove_from_slot(weapon_box, 1);
            }
            else
                take_from_inventory(*best_def, 1);
            notify("The game refused to take the item away");
            return;
        }
        if (weapon_box >= 0)
            add_to_pool(player, best_def->classname, reserve_type, reserve);
        INV_LOG("picked up %s", best_def->name);
    }


    // A stored item put into the world at `position` (and dropped to the floor): a gun with its magazine, upgrades
    // and any reserve of its own, a melee weapon as the one it was, a gas can lacking `spent`.  nullptr when the game
    // would not make it.
    void* spawn_dropped(const inv::ItemDef& def, const inv::WeaponState& state, const Vec3& position, int spent)
    {
        void* entity = nullptr;
        const bool ok = mem::guarded([&] {
            entity = g.create_entity(g.tools, def.classname);
            if (entity == nullptr)
                return;
            // which melee weapon: its keyvalue, read when it spawns
            if (def.weapon() && !state.melee.empty())
                g.set_key_value(g.tools, entity, "melee_script_name", state.melee.c_str());
            if (const auto precache = reinterpret_cast<PrecacheFn>(mem::vslot(entity, k_vt_precache)))
                precache(entity);
            g.dispatch_spawn(g.tools, entity);
            if (const auto teleport = reinterpret_cast<TeleportFn>(mem::vslot(entity, k_vt_teleport)))
                teleport(entity, &position, nullptr, nullptr);
            if (g.drop_to_floor != nullptr)
                g.drop_to_floor(entity, 0x200400Bu, nullptr);
            // a gun keeps its rounds: the magazine, and the reserve the way a dropped gun keeps it
            // (m_iClip1 is every weapon's; m_iExtraPrimaryAmmo only a CTerrorGun's: not the melee weapon, the
            // chainsaw or the pistols, whose reserve is endless)
            if (def.weapon() && std::strcmp(def.classname, "weapon_melee") != 0)
            {
                const auto e = reinterpret_cast<std::uintptr_t>(entity);
                if (state.clip >= 0)
                    *reinterpret_cast<int*>(e + k_off_clip) = state.clip;
                if (state.ammo >= 0 && std::strcmp(def.classname, "weapon_chainsaw") != 0 &&
                    std::strstr(def.classname, "pistol") == nullptr)
                    *reinterpret_cast<int*>(e + k_off_extra_ammo) = state.ammo;
            }
            write_upgrades(entity, def, state);
        });
        if (!ok)
            return nullptr;
        if (entity != nullptr && def.weapon())
            know_gun(entity);
        if (entity != nullptr && !state.melee.empty())
            set_melee_wear(entity, state.wear); // [melee] durability
        if (entity != nullptr && spent > 0)
            set_can_spent(entity, spent);
        return entity;
    }

    void do_drop(int slot, int count)
    {
        const inv::ItemDef* def = nullptr;
        int stored = 0;
        int spent = 0; // the box's gas cans: what the open one lacks (it goes last, the full ones first)
        inv::WeaponState state;
        {
            Session& s = session();
            std::lock_guard lock(s.mutex);
            const inv::Stack& stack = s.inventory.slot(slot);
            if (stack.empty())
                return;
            def = stack.def;
            stored = stack.count;
            spent = stack.spent;
            state = stack.weapon;
        }
        // the box only shows the item in the player's hands: that one is dropped the game's way
        if (stored <= 0)
        {
            do_drop_weapon(def->classname);
            return;
        }
        count = std::min(count, stored);
        void* player = local_player();
        if (player == nullptr)
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

        int dropped = 0;
        for (int i = 0; i < count; ++i)
        {
            // in front of the eyes, a little lower, and a little further for each one so they do not overlap
            const float distance = 48.f + 6.f * static_cast<float>(i);
            const Vec3 position{eye.x + forward.x * distance, eye.y + forward.y * distance,
                                eye.z + forward.z * distance - 14.f};
            void* entity = spawn_dropped(*def, state, position, i == stored - 1 ? spent : 0);
            if (entity == nullptr)
            {
                notify(std::string("Could not drop ") + def->name);
                break;
            }
            INV_LOG("drop %s: entity %p at %.1f %.1f %.1f", def->classname, entity, position.x, position.y, position.z);
            ++dropped;
        }
        if (dropped > 0)
        {
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                const int taken = s.inventory.slot(slot).def == def ? s.inventory.remove_from_slot(slot, dropped) : 0;
                if (taken < dropped)
                    s.inventory.remove_item(*def, dropped - taken);
            }
            INV_LOG("dropped %s x%d", def->name, dropped);
        }
    }

    // Development: puts a new catalogue item on the ground in front of the player, as a drop does, without
    // taking anything from the inventory (the server command inv_spawn <classname>).
    void do_spawn(const inv::ItemDef& def)
    {
        void* player = local_player();
        Vec3 eye{}, forward{};
        if (player == nullptr || !player_view(player, eye, forward))
            return;
        const Vec3 position{eye.x + forward.x * 56.f, eye.y + forward.y * 56.f, eye.z + forward.z * 56.f - 14.f};
        void* entity = nullptr;
        const bool ok = mem::guarded([&] {
            entity = g.create_entity(g.tools, def.classname);
            if (entity == nullptr)
                return;
            if (const auto precache = reinterpret_cast<PrecacheFn>(mem::vslot(entity, k_vt_precache)))
                precache(entity);
            g.dispatch_spawn(g.tools, entity);
            if (const auto teleport = reinterpret_cast<TeleportFn>(mem::vslot(entity, k_vt_teleport)))
                teleport(entity, &position, nullptr, nullptr);
            if (g.drop_to_floor != nullptr)
                g.drop_to_floor(entity, 0x200400Bu, nullptr);
        });
        if (!ok || entity == nullptr)
        {
            notify(std::string("Could not spawn ") + def.name);
            return;
        }
        INV_LOG("spawn %s: entity %p at %.1f %.1f %.1f", def.classname, entity, position.x, position.y, position.z);
        notify(std::string("Spawned ") + def.name);
    }

    // Takes the item in the player's hands into the inventory; when they hold a gun, the first utility item they
    // carry instead.
    void do_store_held()
    {
        void* player = local_player();
        if (player == nullptr)
        {
            notify("Not in a game");
            return;
        }
        INV_LOG("store: the player carries [%s]", carried_list(player).c_str());

        void* item = entity_from_handle(handle_at(player, k_off_active_weapon));
        const inv::ItemDef* def = item_of(item);
        for (int i = 0; def == nullptr && i < k_max_weapons; ++i)
        {
            void* weapon = entity_from_handle(handle_at(player, k_off_my_weapons + 4u * static_cast<unsigned>(i)));
            if (const inv::ItemDef* candidate = item_of(weapon); candidate != nullptr && !candidate->weapon())
            {
                item = weapon;
                def = candidate;
            }
        }
        if (def == nullptr)
        {
            notify("Nothing to store");
            return;
        }
        // a gun or melee weapon in the hands: into a box of its own, with its rounds
        if (def->weapon())
        {
            if (store_carried_weapon(player, item, *def) >= 0)
                INV_LOG("stored %s", def->name);
            return;
        }

        // back into its own box when it has one (a gas can with what it lacks)
        int home = -1;
        bool full = false;
        const int spent = can_spent(item);
        {
            Session& s = session();
            std::lock_guard lock(s.mutex);
            home = s.inventory.held_slot(*def);
            if (home >= 0)
                s.inventory.release_held(home, spent);
            else
                full = s.inventory.add(*def, 1, spent) == 0;
        }
        if (full)
        {
            notify(refusal(*def)); // notify() takes the session lock itself
            return;
        }
        if (!mem::guarded([&] { g.util_remove(item); }))
        {
            {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                if (home >= 0 && s.inventory.remove_from_slot(home, 1) == 1)
                    s.inventory.set_held(home, *def); // the item is still in the player's hands
                else
                    s.inventory.remove_item(*def, 1);
            }
            notify("The game refused to take the item away");
            return;
        }
        INV_LOG("stored %s", def->name);
    }

    int* reserve_of(void* player, int type)
    {
        return reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(player) + k_off_player_ammo + 4u * static_cast<unsigned>(type));
    }

    // ---- one kind of shotgun shell ----------------------------------------------------------------------------------
    //
    // The game gives the auto shotguns (weapon_autoshotgun, weapon_shotgun_spas) a reserve of their own,
    // AMMO_TYPE_AUTOSHOTGUN (8, ammo_autoshotgun_max 90), apart from the pump shotguns' AMMO_TYPE_SHOTGUN (7,
    // ammo_shotgun_max 72): CBaseCombatWeapon::Precache sets m_iPrimaryAmmoType from the weapon script.  The mod has
    // one shell for both (inv::ammo_kind: "shotgun").  Every tick, each survivor's guns of type 8 are set to 7
    // (m_iPrimaryAmmoType +0x140C is a SendProp, 0x1004AFEC: the client predicts reloads and draws the reserve with
    // the server's value), and whatever the game put into the player's m_iAmmo[8] (a gun picked up, an ammo pile
    // used before the change) joins m_iAmmo[7], up to its most.  The game's own code reads the field (Reload,
    // GiveAmmo, the ammo piles' refill), so from then on the auto shotguns load and refill from the shells.
    //
    // The same is done to the pistols (type 1) and the magnums (type 2), whose reserve is endless: they take the
    // SMG rounds (5) and the sniper rounds (10).  Nothing is merged for them (the game keeps no reserve of its own
    // in m_iAmmo[1] and [2]).  Not yet seen in the game.
    void retarget_ammo_types()
    {
        const int max = inv::ammo_max("shotgun");
        for (int index = 1; index <= 64; ++index)
        {
            void* player = entity_at_index(index);
            char name[16];
            if (player == nullptr || !classname_of(player, name, sizeof(name)) || std::strcmp(name, "player") != 0 ||
                team_of(player) != 2 || !alive(player))
                continue;
            for (int i = 0; i < k_max_weapons; ++i)
            {
                void* weapon = entity_from_handle(handle_at(player, k_off_my_weapons + 4u * static_cast<unsigned>(i)));
                const auto w = reinterpret_cast<std::uintptr_t>(weapon);
                if (weapon == nullptr)
                    continue;
                const int type = mem::read_or<int>(w + k_off_ammo_type, -1);
                const int shared = shared_ammo_type(type);
                if (shared != type && mem::guarded([&] { *reinterpret_cast<int*>(w + k_off_ammo_type) = shared; }))
                    state_changed(weapon);
            }
            const int extra = mem::read_or<int>(reinterpret_cast<std::uintptr_t>(reserve_of(player, k_ammo_type_autoshotgun)), 0);
            if (extra <= 0)
                continue;
            mem::guarded([&] {
                int& shotgun = *reserve_of(player, k_ammo_type_shotgun);
                const int before = shotgun;
                shotgun = max >= 0 ? std::min(before + extra, std::max(before, max)) : before + extra;
                *reserve_of(player, k_ammo_type_autoshotgun) = 0;
                if (shotgun - before < extra)
                    INV_LOG("ammo: %d of %d auto shotgun shells joined the shells (the most is %d)", shotgun - before,
                            extra, max);
            });
            state_changed(player);
        }
    }

    // The player carries a gun that shoots ammo type `type`.
    bool carries_ammo_type(void* player, int type)
    {
        for (int i = 0; i < k_max_weapons; ++i)
            if (void* weapon = entity_from_handle(handle_at(player, k_off_my_weapons + 4u * static_cast<unsigned>(i))))
                if (ammo_type_of(weapon) == type)
                    return true;
        return false;
    }

    void install_create_event(void* events)
    {
        const std::uintptr_t create_event = mem::vslot(events, 6);
        if (create_event != 0 && mem::find_pattern(create_event, 11, "55 8B EC 83 EC 08 53 56 57 8B F9") == create_event)
        {
            if (g_create_event.create("CreateEvent", reinterpret_cast<void*>(create_event), hk_create_event))
                g_create_event.enable();
        }
        else
            INV_LOG("game: IGameEventManager2::CreateEvent (%p) does not look as expected: switches show icons",
                    reinterpret_cast<void*>(create_event));
    }

} // namespace game::detail
