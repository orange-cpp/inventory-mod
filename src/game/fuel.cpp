// [chainsaw]: the chainsaw runs on the fuel of the gas cans the survivors carry.
//
// The chainsaw's fuel is its m_iClip1 (a full tank is the weapon script's clip_size, GetMaxClip1: 30).  The game burns
// it while the motor runs and, once it is 0, takes the chainsaw away at the start of its next ItemPostFrame and hands
// out a pistol (see sdk.hpp).  With [chainsaw] that ItemPostFrame is not run at all while the tank is empty: the
// chainsaw stays in the hands, dead (no motor, no swing, no shove), until it is refuelled.
//
// A refuel is the reload key with the chainsaw in hand (the server reads m_nButtons, so the player's client needs no
// mod; their inventory does): the game's progress bar for `seconds`, then the tank is filled from the gas cans in the
// inventory, the open ones first (inv::Inventory::take_fuel).  Switching away, going down or being pinned calls it off.
//
// A gas can's fuel goes with it out of the inventory: what an entity lacks is kept here by its EHANDLE (set_can_spent)
// when the mod puts one into the world or the hands, and read back when one comes into a box.  A can that is poured
// into a generator is used up the game's way, whatever it held.

#include "game/fuel.hpp"

#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/entities.hpp"
#include "game/players.hpp"
#include "game/revive.hpp"
#include "inv/rules.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace game::detail
{

    namespace
    {
        std::map<std::uint32_t, int> g_spent; // a gas can's EHANDLE -> what it lacks
        ULONGLONG g_pruned_at = 0;

        core::Hook<ItemPostFrameFn> g_post_frame;

        struct Refuel
        {
            int user_id = 0;
            std::uint32_t saw = k_invalid_handle;
            float started = 0.f; // game time
            float done_at = 0.f;
        };
        std::vector<Refuel> g_refuels; // main thread only
        std::map<int, bool> g_reload_down; // by userid: the reload key was down last frame

        // An empty chainsaw stays in the hands: its ItemPostFrame would throw it away (see the top of this file)
        void __fastcall hk_post_frame(void* weapon, void* edx)
        {
            if (inv::chainsaw_rules().enabled &&
                mem::read_or<int>(reinterpret_cast<std::uintptr_t>(weapon) + k_off_clip, 1) <= 0)
                return;
            g_post_frame.original()(weapon, edx);
        }

        int max_fuel(void* saw)
        {
            int max = 0;
            if (const auto get_max = reinterpret_cast<GetMaxClip1Fn>(mem::vslot(saw, k_vt_get_max_clip1)))
                mem::guarded([&] { max = get_max(saw); });
            return max > 0 && max <= 1000 ? max : 30;
        }

        int fuel_of(void* saw)
        {
            return mem::read_or<int>(reinterpret_cast<std::uintptr_t>(saw) + k_off_clip, 0);
        }

        bool is_gascan(const inv::Stack& stack)
        {
            return !stack.empty() && std::strcmp(stack.def->classname, "weapon_gascan") == 0;
        }

        void tell(void* player, const std::string& text)
        {
            with_inventory_of(player, [&] { notify(text); });
        }

        // The tank filled from the inventory's gas cans: the open ones first, so the half-empty ones go before the full.
        void pour(void* player, void* saw)
        {
            const int max = max_fuel(saw), before = std::max(0, fuel_of(saw));
            const int need = max - before;
            int taken = 0, left = 0;
            with_inventory_of(player, [&] {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                inv::Inventory& inventory = s.inventory;
                for (int open = 1; open >= 0; --open)
                    for (int i = 0; i < inventory.slot_count() && taken < need; ++i)
                        if (is_gascan(inventory.slot(i)) && (inventory.slot(i).spent > 0) == (open != 0))
                            taken += inventory.take_fuel(i, need - taken);
                for (int i = 0; i < inventory.slot_count(); ++i)
                    if (is_gascan(inventory.slot(i)))
                        left += inventory.slot(i).fuel();
            });
            if (taken <= 0)
            {
                tell(player, "No fuel: carry a gas can in your inventory");
                return;
            }
            if (mem::guarded([&] { *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(saw) + k_off_clip) = before + taken; }))
                state_changed(saw);
            char text[96];
            std::snprintf(text, sizeof(text), "Chainsaw refuelled: %d / %d (%d left in your gas cans)", before + taken, max, left);
            tell(player, text);
            INV_LOG("chainsaw: userid %d poured %d fuel (now %d / %d, %d left in the cans)", user_id_of(player), taken,
                    before + taken, max, left);
        }

        void start_refuel(void* player, void* saw)
        {
            const int user_id = user_id_of(player);
            for (const Refuel& r : g_refuels)
                if (r.user_id == user_id)
                    return;
            if (incapacitated(player) || pinned(player))
                return;
            if (fuel_of(saw) >= max_fuel(saw))
            {
                tell(player, "The chainsaw is full");
                return;
            }
            int fuel = 0;
            const bool has_inventory = with_inventory_of(player, [&] {
                Session& s = session();
                std::lock_guard lock(s.mutex);
                for (int i = 0; i < s.inventory.slot_count(); ++i)
                    if (is_gascan(s.inventory.slot(i)))
                        fuel += s.inventory.slot(i).fuel();
            });
            if (!has_inventory) // (a player without the mod has none)
                return;
            if (fuel <= 0)
            {
                tell(player, "No fuel: carry a gas can in your inventory");
                return;
            }
            const float seconds = inv::chainsaw_rules().seconds;
            if (seconds <= 0.f)
            {
                pour(player, saw);
                return;
            }
            g_refuels.push_back({user_id, handle_of(saw), game_time(), game_time() + seconds});
            show_progress(player, seconds);
            tell(player, "Refuelling the chainsaw");
        }

        void refuels_frame()
        {
            const float now = game_time();
            for (std::size_t i = 0; i < g_refuels.size();)
            {
                const Refuel r = g_refuels[i];
                void* player = player_by_user_id(r.user_id);
                void* saw = entity_from_handle(r.saw);
                const bool holding = player != nullptr && saw != nullptr && alive(player) &&
                                     handle_at(player, k_off_active_weapon) == r.saw && !incapacitated(player) &&
                                     !pinned(player);
                if (holding && now < r.done_at)
                {
                    ++i;
                    continue;
                }
                g_refuels.erase(g_refuels.begin() + static_cast<std::ptrdiff_t>(i));
                if (player == nullptr)
                    continue;
                clear_progress(player, r.started);
                if (holding)
                    pour(player, saw);
                else
                    INV_LOG("chainsaw: userid %d's refuel was called off", r.user_id);
            }
        }
    } // namespace

    int can_spent(void* can)
    {
        const auto it = g_spent.find(handle_of(can));
        return it != g_spent.end() ? it->second : 0;
    }

    void set_can_spent(void* can, int spent)
    {
        const std::uint32_t handle = handle_of(can);
        if (handle == k_invalid_handle)
            return;
        if (spent > 0)
            g_spent[handle] = spent;
        else
            g_spent.erase(handle);
    }

    void chainsaw_frame()
    {
        // the cans that are gone (used, removed, a new map) are forgotten now and then
        if (const ULONGLONG now = GetTickCount64(); now - g_pruned_at > 5000)
        {
            g_pruned_at = now;
            for (auto it = g_spent.begin(); it != g_spent.end();)
                it = entity_from_handle(it->first) == nullptr ? g_spent.erase(it) : std::next(it);
        }
        if (!inv::chainsaw_rules().enabled || !g_post_frame.installed())
        {
            g_refuels.clear();
            return;
        }
        for (int index = 1; index <= 64; ++index)
        {
            void* player = entity_at_index(index);
            char name[16];
            if (player == nullptr || !classname_of(player, name, sizeof(name)) || std::strcmp(name, "player") != 0 ||
                team_of(player) != k_team_survivors || !alive(player) || is_bot(player))
                continue;
            const bool down = (mem::read_or<int>(reinterpret_cast<std::uintptr_t>(player) + k_off_buttons, 0) & k_in_reload) != 0;
            bool& was_down = g_reload_down[user_id_of(player)];
            const bool pressed = down && !was_down;
            was_down = down;
            if (!pressed)
                continue;
            void* saw = entity_from_handle(handle_at(player, k_off_active_weapon));
            char held[32];
            if (saw != nullptr && classname_of(saw, held, sizeof(held)) && std::strcmp(held, "weapon_chainsaw") == 0)
                start_refuel(player, saw);
        }
        refuels_frame();
    }

    void install_chainsaw()
    {
        const std::uintptr_t post_frame = g.server.base + k_rva_chainsaw_post_frame;
        if (mem::read_or<std::uintptr_t>(g.server.base + k_rva_chainsaw_vtable + 4u * k_vt_item_post_frame) == post_frame &&
            mem::find_pattern(post_frame, 28, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC A8 01 00 00") ==
                post_frame &&
            g_post_frame.create("ChainsawPostFrame", reinterpret_cast<void*>(post_frame), hk_post_frame) && g_post_frame.enable())
            INV_LOG("game: chainsaw fuel ready (on when inventory_mod.toml has [chainsaw])");
        else
            INV_LOG("game: the chainsaw's ItemPostFrame does not look as expected: no [chainsaw]");
    }

} // namespace game::detail
