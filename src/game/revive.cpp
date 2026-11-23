// [self_revive]: an incapacitated survivor gets up by themselves with an item from their inventory.
//
// The game has every piece of it: CTerrorPlayer::OnRevived (vtable slot 583, what the VScript ReviveFromIncap calls)
// is the end of a friend's revive, and with no reviver (m_reviveOwner empty) it counts the survivor as their own: up
// off a ledge, or up from the ground with the next incap counted (the last one leaves them black and white), and
// "revive_success" fired.  The wait is the game's own progress bar (m_flProgressBarStartTime / _Duration), which the
// client draws by itself, so a player without the mod on the server sees it too.  The item goes only when the survivor
// is up; being pinned, revived by a friend first, or dying calls it off and the item stays.  See sdk.hpp.

#include "game/revive.hpp"

#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/entities.hpp"
#include "game/players.hpp"
#include "game/weapons.hpp"
#include "inv/rules.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace game::detail
{

    namespace
    {
        IsIncapacitatedFn g_is_incapacitated = nullptr;
        OnRevivedFn g_on_revived = nullptr;
        IsDominatedFn g_is_dominated = nullptr;
        OnAdrenalineUsedFn g_on_adrenaline_used = nullptr;

        struct Pending
        {
            int user_id = 0;
            const inv::ItemDef* def = nullptr;
            int box = -1; // the inventory box it was taken from, -1: the player's own slot for it
            float started = 0.f; // game time
            float done_at = 0.f;
        };
        std::vector<Pending> g_pending; // main thread only

        Pending* pending_of(int user_id)
        {
            for (Pending& p : g_pending)
                if (p.user_id == user_id)
                    return &p;
            return nullptr;
        }

        // adrenaline_duration (15 s): what the item's rush lasts when it is used the game's way
        float adrenaline_seconds()
        {
            static std::uintptr_t convar = find_server_convar("adrenaline_duration");
            std::uintptr_t holder = convar != 0 ? mem::read_or<std::uintptr_t>(convar + k_off_convar_parent, 0) : 0;
            if (holder == 0)
                holder = convar;
            const float seconds = holder != 0 ? mem::read_or<float>(holder + k_off_convar_float, 15.f) : 15.f;
            return seconds > 0.f && seconds < 600.f ? seconds : 15.f;
        }

        // One `def` off the player: from box `box` when it still has one, else any box, else the one in their own slot
        // for it (taken out of their hands and removed).  False when they have none any more.  With `look` only: whether
        // they have one, nothing taken.
        bool use_up(void* player, const inv::ItemDef& def, int box, bool look = false)
        {
            bool taken = false;
            const auto from_boxes = [&] {
                with_inventory_of(player, [&] {
                    Session& s = session();
                    std::lock_guard lock(s.mutex);
                    const auto take = [&](int i) { return look || s.inventory.remove_from_slot(i, 1) == 1; };
                    if (box >= 0 && s.inventory.slot(box).def == &def && s.inventory.slot(box).count > 0)
                        taken = take(box);
                    for (int i = 0; !taken && i < s.inventory.slot_count(); ++i)
                        if (s.inventory.slot(i).def == &def && s.inventory.slot(i).count > 0)
                            taken = take(i);
                });
            };
            const auto from_hands = [&] {
                const inv::ItemDef* carried_def = nullptr;
                void* carried = carried_in_slot(player, def.game_slot, &carried_def);
                if (carried == nullptr || carried_def != &def)
                    return;
                if (look)
                {
                    taken = true;
                    return;
                }
                const auto drop = g.quiet_drop != nullptr ? g.quiet_drop
                                                          : reinterpret_cast<WeaponDropFn>(mem::vslot(player, k_vt_weapon_drop));
                taken = drop != nullptr && mem::guarded([&] {
                    drop(player, carried, nullptr, nullptr);
                    g.util_remove(carried);
                });
            };
            if (box >= 0)
            {
                from_boxes();
                if (!taken)
                    from_hands();
            }
            else
            {
                from_hands();
                if (!taken)
                    from_boxes();
            }
            return taken;
        }

        void tell(void* player, const std::string& text)
        {
            with_inventory_of(player, [&] { notify(text); });
        }

        // Up, and the item used: only once the game has them up (OnRevived checks for itself and may do nothing).
        void finish(void* player, const Pending& p)
        {
            if (!use_up(player, *p.def, p.box, true))
            {
                tell(player, std::string("You have no ") + p.def->name + " left to get up with");
                return;
            }
            mem::guarded([&] { g_on_revived(player); });
            if (incapacitated(player) || hanging_from_ledge(player))
            {
                INV_LOG("self-revive: userid %d is still down after OnRevived: the %s stays", p.user_id, p.def->classname);
                tell(player, "You could not get up");
                return;
            }
            use_up(player, *p.def, p.box);
            if (std::strcmp(p.def->classname, "weapon_adrenaline") == 0 && inv::self_revive_rules().rush &&
                g_on_adrenaline_used != nullptr)
                mem::guarded([&] { g_on_adrenaline_used(player, adrenaline_seconds()); });
            state_changed(player);
            INV_LOG("self-revive: userid %d got up with %s", p.user_id, p.def->classname);
        }
    } // namespace

    bool incapacitated(void* player)
    {
        bool down = false;
        if (g_is_incapacitated != nullptr && player != nullptr)
            mem::guarded([&] { down = g_is_incapacitated(player); });
        return down;
    }

    bool hanging_from_ledge(void* player)
    {
        return player != nullptr &&
               mem::read_or<std::uint8_t>(reinterpret_cast<std::uintptr_t>(player) + k_off_hanging_from_ledge, 0) != 0;
    }

    bool pinned(void* player)
    {
        bool held = false;
        if (g_is_dominated != nullptr && player != nullptr)
            mem::guarded([&] { held = g_is_dominated(player); });
        return held;
    }

    void show_progress(void* player, float seconds)
    {
        const auto base = reinterpret_cast<std::uintptr_t>(player);
        if (player != nullptr && mem::guarded([&] {
                *reinterpret_cast<float*>(base + k_off_progress_start) = game_time();
                *reinterpret_cast<float*>(base + k_off_progress_duration) = seconds;
            }))
            state_changed(player);
    }

    void clear_progress(void* player, float since)
    {
        const auto base = reinterpret_cast<std::uintptr_t>(player);
        if (player != nullptr && mem::read_or<float>(base + k_off_progress_start, -1.f) == since)
            show_progress(player, 0.f);
    }

    bool start_self_revive(void* player, const inv::ItemDef& def, int box)
    {
        if (g_on_revived == nullptr || player == nullptr || !inv::revives(def.classname) || !alive(player) ||
            !(incapacitated(player) || hanging_from_ledge(player)))
            return false;
        const inv::SelfReviveRules& rules = inv::self_revive_rules();
        const int user_id = user_id_of(player);
        if (pending_of(user_id) != nullptr)
            return true; // (getting up already)
        if (hanging_from_ledge(player) && !rules.ledge)
        {
            notify("You cannot pull yourself up off a ledge");
            return true;
        }
        if (pinned(player))
        {
            notify("Not while you are pinned");
            return true;
        }
        const Pending p{user_id, &def, box, game_time(), game_time() + rules.seconds};
        INV_LOG("self-revive: userid %d gets up with %s (box %d) in %.1f s", user_id, def.classname, box, rules.seconds);
        if (rules.seconds <= 0.f)
        {
            finish(player, p);
            return true;
        }
        g_pending.push_back(p);
        show_progress(player, rules.seconds);
        notify(std::string("Getting up with your ") + def.name);
        return true;
    }

    void revive_frame()
    {
        if (g_pending.empty())
            return;
        const float now = game_time();
        for (std::size_t i = 0; i < g_pending.size();)
        {
            const Pending p = g_pending[i];
            void* player = player_by_user_id(p.user_id);
            const bool down = player != nullptr && alive(player) && (incapacitated(player) || hanging_from_ledge(player));
            const bool held = down && pinned(player);
            if (down && !held && now < p.done_at)
            {
                ++i;
                continue;
            }
            g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
            if (player == nullptr)
                continue;
            clear_progress(player, p.started);
            if (!down) // up by a friend first, or dead: the item stays
                INV_LOG("self-revive: userid %d is no longer down: called off", p.user_id);
            else if (held)
            {
                INV_LOG("self-revive: userid %d was pinned: called off", p.user_id);
                tell(player, "Pinned: you did not get up");
            }
            else
                finish(player, p);
        }
    }

    void install_revive()
    {
        const std::uintptr_t base = g.server.base;
        const std::uintptr_t incap = base + k_rva_is_incapacitated, revived = base + k_rva_on_revived;
        const std::uintptr_t dominated = base + k_rva_is_dominated, adrenaline = base + k_rva_on_adrenaline_used;
        if (mem::read_or<std::uintptr_t>(g.player_vtable + 4u * k_vt_is_incapacitated) != incap ||
            mem::find_pattern(incap, 16, "80 B9 F0 00 00 00 00 75 0F 80 B9 A8 2D 00 00 00") != incap ||
            mem::read_or<std::uintptr_t>(g.player_vtable + 4u * k_vt_on_revived) != revived ||
            mem::find_pattern(revived, 21, "55 8B EC 83 EC 3C 53 56 8B F1 8B 06 8B 90 28 01 00 00 57 FF D2") != revived ||
            mem::find_pattern(dominated, 15, "8B 15 ?? ?? ?? ?? 56 8B F1 8B 86 5C 3E 00 00") != dominated)
        {
            INV_LOG("game: the revive functions do not look as expected: no [self_revive]");
            return;
        }
        g_is_incapacitated = reinterpret_cast<IsIncapacitatedFn>(incap);
        g_on_revived = reinterpret_cast<OnRevivedFn>(revived);
        g_is_dominated = reinterpret_cast<IsDominatedFn>(dominated);
        if (mem::find_pattern(adrenaline, 14, "55 8B EC 51 53 56 8B F1 8D 9E 80 32 00 00") == adrenaline)
            g_on_adrenaline_used = reinterpret_cast<OnAdrenalineUsedFn>(adrenaline);
        else
            INV_LOG("game: OnAdrenalineUsed does not look as expected: a self-revive with adrenaline gives no rush");
        INV_LOG("game: self-revive ready (on when inventory_mod.toml has [self_revive])");
    }

} // namespace game::detail
