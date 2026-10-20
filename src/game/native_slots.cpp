// The game's own weapon slots: what the player carries there, for the HUD and the weight; switching and dropping.

#include "game/native_slots.hpp"

#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/map_guns.hpp"
#include "game/weapons.hpp"

#include <algorithm>
#include <cstring>

namespace game::detail
{

    // ---- the game's own slots -----------------------------------------------------------------------------------

    // "weapon_rifle_ak47" -> name "Rifle Ak47", tag "RA"
    NativeWeapon describe_weapon(const char* classname)
    {
        NativeWeapon out;
        out.classname = classname;
        const char* p = classname;
        if (strncmp(p, "weapon_", 7) == 0)
            p += 7;
        bool word_start = true;
        int tag_len = 0;
        for (; *p != '\0'; ++p)
        {
            const char c = *p == '_' ? ' ' : *p;
            out.name += word_start ? static_cast<char>(toupper(c)) : c;
            if (word_start && c != ' ' && tag_len < 2)
                out.tag[tag_len++] = static_cast<char>(toupper(c));
            word_start = c == ' ';
        }
        if (tag_len < 2 && out.name.size() > 1)
            out.tag[tag_len++] = static_cast<char>(toupper(out.name[1]));
        return out;
    }

    // The game slot a carried weapon is in: the catalogue knows the items; of the rest, pistols and melee weapons are
    // the secondary slot (1), everything else is a primary (0).
    int native_slot_of(const char* classname)
    {
        if (const inv::ItemDef* def = inv::find_item(classname))
            return def->game_slot;
        if (std::strstr(classname, "pistol") != nullptr || std::strcmp(classname, "weapon_melee") == 0 ||
            std::strcmp(classname, "weapon_chainsaw") == 0)
            return 1;
        return 0;
    }

    // Main thread: what the player carries in m_hMyWeapons, into the session for the HUD, one entry per game slot.
    void refresh_native()
    {
        std::vector<NativeWeapon> list(k_native_slots);
        float rounds_weight = 0.f; // the reserve's (see "rounds")
        if (void* player = local_player())
        {
            const AmmoPool pool = read_pool(player);
            if (pool.read)
                rounds_weight = inv::ammo_weight(pool.count);
            const std::uint32_t active = handle_at(player, k_off_active_weapon);
            for (int i = 0; i < k_max_weapons; ++i)
            {
                const std::uint32_t handle = handle_at(player, k_off_my_weapons + 4u * static_cast<unsigned>(i));
                void* weapon = entity_from_handle(handle);
                char name[64];
                if (weapon == nullptr || !classname_of(weapon, name, sizeof(name)))
                    continue;
                const int slot = native_slot_of(name);
                if (slot < 0 || slot >= k_native_slots || !list[static_cast<std::size_t>(slot)].classname.empty())
                    continue;
                NativeWeapon entry = describe_weapon(name);
                entry.active = handle == active;
                if (std::strcmp(name, "weapon_melee") == 0) // which melee weapon, for its icon
                    if (const inv::ItemDef* melee = inv::find_item(name))
                    {
                        const inv::WeaponState state = weapon_state(player, weapon, *melee, true);
                        entry.melee = state.melee;
                        entry.wear = state.wear;
                    }
                list[static_cast<std::size_t>(slot)] = std::move(entry);
            }
        }
        Session& s = session();
        std::lock_guard lock(s.mutex);
        // the "in hand" marks of the inventory's boxes follow what the player really carries
        inv::Inventory& inventory = s.inventory;
        for (int i = 0; i < inventory.slot_count(); ++i)
        {
            const inv::Stack& stack = inventory.slot(i);
            if (!stack.held)
                continue;
            const bool carried = std::any_of(list.begin(), list.end(), [&](const NativeWeapon& w) {
                return w.classname == stack.def->classname;
            });
            if (!carried || inventory.held_slot(*stack.def) != i)
                inventory.clear_held(i);
        }
        for (const NativeWeapon& weapon : list)
        {
            const inv::ItemDef* def = weapon.classname.empty() ? nullptr : inv::find_item(weapon.classname);
            if (def != nullptr && !def->weapon() && inventory.held_slot(*def) < 0)
                inventory.set_held(inventory.home_for(*def), *def); // picked up in the game: it gets a box
        }
        // the gun and the melee / pistol count against the weight limit too
        float weapons = 0.f;
        for (const NativeWeapon& weapon : list)
            weapons += inv::weapon_weight(weapon.classname);
        inventory.set_weapons_weight(weapons);
        inventory.set_ammo_weight(rounds_weight);
        s.native = std::move(list);
    }

    // Drops a carried weapon the way the game's own DropItem does.
    void do_drop_weapon(const std::string& classname)
    {
        void* player = local_player();
        if (player == nullptr)
            return;
        if (const auto cannot = reinterpret_cast<CannotDropFn>(mem::vslot(player, k_vt_cannot_drop));
            cannot != nullptr && cannot(player) != 0)
        {
            notify("The game does not let you drop now");
            return;
        }
        for (int i = 0; i < k_max_weapons; ++i)
        {
            void* weapon = entity_from_handle(handle_at(player, k_off_my_weapons + 4u * static_cast<unsigned>(i)));
            char name[64];
            if (weapon == nullptr || !classname_of(weapon, name, sizeof(name)) || classname != name)
                continue;
            const auto drop = reinterpret_cast<WeaponDropFn>(mem::vslot(player, k_vt_weapon_drop));
            if (drop == nullptr)
                return;
            {
                KeepRounds keep(player, nullptr); // the reserve stays the player's (see KeepRounds)
                drop(player, weapon, nullptr, nullptr);
            }
            INV_LOG("dropped %s", name);
            notify(std::string("Dropped ") + describe_weapon(name).name);
            return;
        }
    }

    char g_name_buffer[64];

    // `use <classname>`: the console command calls CBasePlayer::SelectItem (slot 429) with the name
    void do_select_weapon(const std::string& classname)
    {
        void* player = local_player();
        if (player == nullptr)
            return;
        put_away_carried(player, classname.c_str());
        const auto select_item = reinterpret_cast<SelectItemFn>(mem::vslot(player, k_vt_select_item));
        if (select_item != nullptr)
        {
            select_item(player, classname.c_str(), 0);
            INV_LOG("select %s: active weapon is now %s", classname.c_str(),
                    classname_of(entity_from_handle(handle_at(player, k_off_active_weapon)), g_name_buffer, sizeof(g_name_buffer))
                        ? g_name_buffer
                        : "?");
            // TEMP: why a thrown weapon would swap away
            void* active = entity_from_handle(handle_at(player, k_off_active_weapon));
            if (active != nullptr)
            {
                const auto w = reinterpret_cast<std::uintptr_t>(active);
                const int type = mem::read_or<int>(w + k_off_ammo_type, -9), clip = mem::read_or<int>(w + k_off_clip, -9);
                INV_LOG("TEMP select: ammo type %d, clip %d, reserve %d, next primary %.2f, game time %.2f", type, clip,
                        type >= 0 && type < 32 ? mem::read_or<int>(reinterpret_cast<std::uintptr_t>(player) + k_off_player_ammo + 4u * type, -9) : -9,
                        mem::read_or<float>(w + k_off_next_primary), game_time());
            }
        }
    }

} // namespace game::detail
