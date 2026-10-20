// A death, a new game: the inventories follow the game's own rules; the game's events and its frame.

#include "game/lifecycle.hpp"

#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/ammo_piles.hpp"
#include "game/backend.hpp"
#include "game/infection.hpp"
#include "game/map_guns.hpp"
#include "game/players.hpp"
#include "game/rounds.hpp"
#include "game/melee.hpp"
#include "game/stamina.hpp"
#include "game/weapons.hpp"
#include "inv/rules.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace game::detail
{

    // ---- a death, a new game: the inventory follows the game's own rules --------------------------------------------
    //
    // A survivor who dies drops what they carried, in the game; their inventory does too.  From the next GameFrame
    // (not inside the death's event) its items fall around the spot where they died (spawn_dropped), each ammo
    // type of their reserve lies there as a pile of dropped rounds (spawn_pile), and the inventory and the reserve
    // are empty: a survivor rescued from a closet, or back in the next chapter, starts with nothing of it.
    // A round starting (the event round_start) is one of three things, as the game sees it:
    //   - the next chapter (the event map_transition came before it): the survivors keep what they carry, and the
    //     inventories theirs;
    //   - the same map again (a lost round, a restarted chapter, versus' second round): the game gives the survivors
    //     back what they had when the chapter began, and the inventories get back theirs (a copy taken then);
    //   - another map without a transition (a new campaign, a chapter chosen in the menu, the first map after the
    //     game started): everyone starts with nothing, and the inventories are emptied.
    // The map is CGlobalVars::mapname (+0x3C, a string_t: the const char*; server.dll reads it so at 0x1009B903 for
    // "maps/%s_commentary.txt", with "" for null).  The inventories: the host's (the local one) and every one the
    // server keeps; a player's file not loaded yet that was last written before the new game is not read
    // (g_new_game, see server_player).
    //
    // A chapter transition and the reserve.  A stored gun keeps only its magazine: its reserve is the player's
    // m_iAmmo of its type, shared with the guns in the hands (see "rounds").  At a transition the game saves each
    // survivor (CTerrorPlayer 0x102B7E20, "Saving %s for transition": the KeyValues of 0x102B6C40, one "weapon"
    // subkey per carried weapon with "currentMagazine" and "extraAmmo", the reserve of that gun's type), and in the
    // next chapter puts it back onto their new player entity (CTransitioningPlayer::Restore, 0x102B8B60, from the
    // takeover of the survivor bot that stood in for them: "currentMagazine" onto m_iClip1, "extraAmmo" with
    // SetAmmoCount).  So only the types of the guns in the hands come back; the reserve of every gun in a box was
    // 0 in the next chapter (seen on the dedicated server 2026-10-18: a stored hunting rifle's rounds gone from
    // c2m1 to c2m2).  Now the event map_transition copies every survivor's m_iAmmo into their session
    // (transition_reserve), and the hook on the restore writes it back for the types no carried gun uses (the carried
    // ones are the game's: they may have been fired between the event and the game's save), up to the type's most.
    // On a dedicated server the players leave and come back during the change: their session (by SteamID) waits for
    // them.  A new game (round_start without the transition) drops what waits.

    core::Hook<FireEventFn> g_fire_event;
    core::Hook<GameFrameFn> g_game_frame;
    core::Hook<TransitionRestoreFn> g_transition_restore;

    struct Death
    {
        int user_id;
        Vec3 at; // where they died (the player's origin then: a dead player's moves with the spectator camera)
    };
    std::vector<Death> g_deaths; // main thread only: for the next GameFrame
    bool g_transition = false; // map_transition came: the next round_start is the next chapter
    std::string g_round_map; // the map of the last round_start
    std::map<Session*, std::string> g_chapter_start; // each inventory when the chapter began (Inventory::to_text)

    std::string map_name()
    {
        char name[64] = {};
        const char* text = g_globals != nullptr ? mem::read_or<const char*>(reinterpret_cast<std::uintptr_t>(g_globals) + k_off_mapname)
                                                : nullptr;
        if (text != nullptr)
            mem::safe_copy(name, text, sizeof(name) - 1);
        return name;
    }

    // Every inventory this game keeps: the local one (not while it mirrors a dedicated server's), the players'.
    template <class F>
    void each_session(F&& f)
    {
        if (Session& local = local_session(); !local.remote)
            f(local);
        for (auto& [id, sp] : g_players)
            f(sp->session);
    }

    // The inventory of a player of this game: the host's (the local one), or a served player's when their game runs
    // the mod; nullptr for a bot or a player without the mod.  Main thread.
    Session* session_of_player(void* player)
    {
        if (player != nullptr && player == host_player())
            return &local_session();
        if (ServerPlayer* sp = server_player(player); sp != nullptr && sp->has_mod)
            return &sp->session;
        return nullptr;
    }

    void on_round_start()
    {
        g_deaths.clear();
        forget_map_guns(); // (the map's guns are new ones)
        const std::string map = map_name();
        const bool next_chapter = g_transition;
        const bool again = !next_chapter && !map.empty() && map == g_round_map;
        int sessions = 0;
        each_session([&](Session& s) {
            std::lock_guard lock(s.mutex);
            if (again)
            {
                if (const auto it = g_chapter_start.find(&s); it != g_chapter_start.end())
                    s.inventory.from_text(it->second);
            }
            else if (!next_chapter)
                s.inventory.clear();
            if (!next_chapter)
                s.transition_pending = false; // (a reserve waiting for a next chapter that is not this round)
            g_chapter_start[&s] = s.inventory.to_text();
            ++sessions;
        });
        if (!next_chapter && !again)
            GetSystemTimeAsFileTime(&g_new_game);
        INV_LOG("round start on %s: %s, %d inventories %s", map.c_str(),
                next_chapter ? "the next chapter" : again ? "the same map again" : "a new game", sessions,
                next_chapter ? "kept" : again ? "back as the chapter began" : "emptied");
        g_transition = false;
        g_round_map = map;
    }

    // The event map_transition: the chapter ends.  Every survivor's reserve goes with their inventory to the next
    // chapter (restore_transition_reserve), since the game's own save keeps only the reserve of the guns in the hands.
    void on_map_transition()
    {
        g_transition = true;
        int saved = 0;
        for (int index = 1; index <= 64; ++index)
        {
            void* player = entity_at_index(index);
            char name[16];
            if (player == nullptr || !classname_of(player, name, sizeof(name)) || std::strcmp(name, "player") != 0 ||
                team_of(player) != 2)
                continue;
            Session* s = session_of_player(player);
            const AmmoPool pool = read_pool(player);
            if (s == nullptr || !pool.read)
                continue;
            std::memcpy(s->transition_reserve, pool.count, sizeof(s->transition_reserve));
            s->transition_pending = true;
            ++saved;
        }
        INV_LOG("chapter transition: the reserve of %d survivors with an inventory waits for the next chapter", saved);
    }

    // The event player_death: a survivor's inventory falls where they died, a moment later (drop_dead_inventories).
    void on_death(int user_id)
    {
        void* player = player_by_user_id(user_id);
        Vec3 at{};
        if (player != nullptr && team_of(player) == 2 && mem::read(reinterpret_cast<std::uintptr_t>(player) + k_off_origin, at))
            g_deaths.push_back({user_id, at});
    }

    // The active session's inventory and the player's reserve onto the ground around `at`.  Main thread.
    void drop_inventory(void* player, const Vec3& at)
    {
        std::vector<std::pair<const inv::ItemDef*, inv::WeaponState>> items;
        {
            Session& s = session();
            std::lock_guard lock(s.mutex);
            for (int i = 0; i < s.inventory.total_slots(); ++i)
            {
                const inv::Stack& stack = s.inventory.slot(i);
                for (int n = 0; !stack.empty() && n < stack.count; ++n)
                    items.emplace_back(stack.def, stack.weapon);
            }
            s.inventory.clear();
        }
        // the items close around the spot (they are loose weapons: they may touch), the piles on a ring around them
        // and apart from each other (piles inside each other crashed the game's physics, see spawn_pile)
        int dropped = 0;
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            const float angle = static_cast<float>(i) * 2.39996f; // the golden angle: no two in one place
            const float radius = std::min(4.f + 3.f * static_cast<float>(i), 18.f);
            const Vec3 position{at.x + std::cos(angle) * radius, at.y + std::sin(angle) * radius, at.z + 16.f};
            if (spawn_dropped(*items[i].first, items[i].second, position) != nullptr)
                ++dropped;
            else
                INV_LOG("death: %s could not be dropped: it is lost", items[i].first->classname);
        }
        AmmoPool pool = read_pool(player);
        std::vector<const char*> kinds;
        for (const char* kind : inv::ammo_kinds())
            if (const int type = inv::ammo_index(kind); pool.read && type >= 0 && pool.count[type] > 0)
                kinds.push_back(kind);
        int piles = 0;
        const float step = kinds.empty() ? 0.f : 6.28319f / static_cast<float>(kinds.size());
        const float ring = kinds.size() < 2 ? 28.f : std::max(28.f, 12.f / std::sin(step / 2.f));
        for (std::size_t i = 0; i < kinds.size(); ++i)
        {
            const std::string kind = kinds[i];
            const int type = inv::ammo_index(kind);
            const float angle = step * static_cast<float>(i);
            const Vec3 position{at.x + std::cos(angle) * ring, at.y + std::sin(angle) * ring, at.z + 16.f};
            std::string model;
            bool loaded = false;
            void* pile = spawn_pile(kind, pool.count[type], position, angle * 57.29578f, false, model, loaded);
            if (pile == nullptr)
                continue;
            track_dropped_pile(pile, position);
            INV_LOG("death: %d %s rounds as pile %p", pool.count[type], kind.c_str(), pile);
            pool.count[type] = 0;
            ++piles;
        }
        if (piles > 0)
        {
            write_pool(player, pool);
            state_changed(player);
        }
        INV_LOG("death: userid %d dropped %d of %zu items and %d piles of rounds at %.0f %.0f %.0f", user_id_of(player), dropped,
                items.size(), piles, at.x, at.y, at.z);
        if (dropped > 0 || piles > 0)
            notify("Your inventory fell where you died");
    }

    // From GameFrame: the inventories of the survivors who died a moment ago (the host's, or a served player's).
    void drop_dead_inventories()
    {
        if (g_deaths.empty())
            return;
        std::vector<Death> deaths;
        deaths.swap(g_deaths);
        for (const Death& death : deaths)
        {
            void* player = player_by_user_id(death.user_id);
            if (player == nullptr || alive(player))
                continue;
            if (player == host_player())
                drop_inventory(player, death.at);
            else if (served_remotely(player))
            {
                Activation active(player);
                if (active.ok())
                    drop_inventory(player, death.at);
            }
        }
    }

    void on_game_event(void* event)
    {
        const auto name_fn = reinterpret_cast<EventNameFn>(mem::vslot(event, 1));
        const auto int_fn = reinterpret_cast<EventIntFn>(mem::vslot(event, 6));
        const char* name = name_fn != nullptr ? name_fn(event) : nullptr;
        if (name == nullptr || int_fn == nullptr)
            return;
        if (std::strcmp(name, "round_start") == 0)
            on_round_start_piles(); // (the map's packs: see place_map_packs)
        // (the inventories: see "a death, a new game")
        if (std::strcmp(name, "round_start") == 0)
            on_round_start();
        else if (std::strcmp(name, "map_transition") == 0)
            on_map_transition();
        else if (std::strcmp(name, "player_death") == 0)
            on_death(int_fn(event, "userid", 0));
        infection_on_event(name, event, int_fn);
    }

    bool __fastcall hk_fire_event(void* self, void* edx, void* event, bool dont_broadcast)
    {
        if (event != nullptr)
            mem::guarded([&] { on_game_event(event); });
        return g_fire_event.original()(self, edx, event, dont_broadcast);
    }

    // Ammo piles (and guns picked up) fill the reserve as the game does, up to the type's most; with a weight limit
    // only what fits by weight is kept (see "rounds"): ten times a second the reserves of the players who have an
    // inventory here (the host, the served players) are compared with the last look, and a rise that takes the load
    // past the limit is taken back down to what fits.
    struct ReserveLook
    {
        void* player = nullptr;
        int pool[32] = {};
    };
    std::map<int, ReserveLook> g_reserves; // by userid
    ULONGLONG g_reserves_at = 0;

    void cap_reserve(void* player, int user_id, Session& s, bool host)
    {
        AmmoPool pool = read_pool(player);
        if (!pool.read)
            return;
        ReserveLook& look = g_reserves[user_id];
        if (look.player != player)
        {
            look.player = player;
            std::memcpy(look.pool, pool.count, sizeof(look.pool));
            return;
        }
        float max_weight = 0.f, load = 0.f;
        {
            std::lock_guard lock(s.mutex);
            max_weight = s.inventory.limits().max_weight;
            load = s.inventory.weight() - s.inventory.ammo_weight() + inv::ammo_weight(pool.count);
        }
        bool changed = false;
        if (max_weight > 0.f && load > max_weight + 1e-3f)
            for (const char* kind : inv::ammo_kinds())
            {
                const int type = inv::ammo_index(kind);
                const int rise = pool.count[type] - look.pool[type];
                const float each = inv::round_weight(kind);
                if (rise <= 0 || each <= 0.f)
                    continue;
                const int back = std::min(rise, static_cast<int>(std::ceil((load - max_weight) / each - 1e-4f)));
                pool.count[type] -= back;
                load -= static_cast<float>(back) * each;
                changed = true;
                INV_LOG("rounds: userid %d got %d %s, %d fit by weight (%.1f of %.1f)", user_id, rise, kind, rise - back,
                        load, max_weight);
                if (host)
                    notify("Too heavy: only " + std::to_string(rise - back) + " " + inv::ammo_title(kind) + " fit");
                if (load <= max_weight + 1e-3f)
                    break;
            }
        if (changed)
        {
            write_pool(player, pool);
            state_changed(player);
        }
        std::memcpy(look.pool, pool.count, sizeof(look.pool));
    }

    void cap_reserves()
    {
        const ULONGLONG now = GetTickCount64();
        if (now - g_reserves_at < 100)
            return;
        g_reserves_at = now;
        void* host = host_player();
        for (int index = 1; index <= 64; ++index)
        {
            void* player = entity_at_index(index);
            char name[16];
            if (player == nullptr || !classname_of(player, name, sizeof(name)) || std::strcmp(name, "player") != 0 ||
                team_of(player) != 2 || !alive(player))
                continue;
            know_held_guns(player);
            Session* s = session_of_player(player);
            const int user_id = s != nullptr ? user_id_of(player) : -1;
            if (user_id > 0)
                cap_reserve(player, user_id, *s, player == host);
        }
    }

    // After the game restored a transitioned survivor onto their new player entity (see "a chapter transition"): the
    // game gave back the reserve of the guns in their hands; every other type gets what the player had when the
    // chapter ended, for the guns in their boxes, up to the type's most.
    void restore_transition_reserve(void* player)
    {
        Session* s = session_of_player(player);
        if (s == nullptr || !s->transition_pending)
            return;
        s->transition_pending = false;
        AmmoPool pool = read_pool(player);
        if (!pool.read)
            return;
        std::string back;
        for (const char* kind : inv::ammo_kinds())
        {
            const int type = inv::ammo_index(kind);
            if (type < 0 || type >= 32 || carries_ammo_type(player, type))
                continue;
            int rounds = s->transition_reserve[type];
            if (const int max = inv::ammo_max(kind); max >= 0)
                rounds = std::min(rounds, max);
            if (rounds <= pool.count[type])
                continue;
            pool.count[type] = rounds;
            back += (back.empty() ? "" : ", ") + std::to_string(rounds) + ' ' + kind;
        }
        if (back.empty())
        {
            INV_LOG("chapter transition: userid %d restored, no stored gun's reserve to give back", user_id_of(player));
            return;
        }
        write_pool(player, pool);
        state_changed(player);
        g_reserves.erase(user_id_of(player)); // (cap_reserve: a first look at this reserve, not a rise)
        INV_LOG("chapter transition: userid %d restored, the reserve of the stored guns back: %s", user_id_of(player),
                back.c_str());
    }

    void __fastcall hk_transition_restore(void* transitioning, void* edx, void* player)
    {
        g_transition_restore.original()(transitioning, edx, player);
        if (player != nullptr)
            mem::guarded([&] { restore_transition_reserve(player); });
    }

    void __fastcall hk_game_frame(void* self, void* edx, bool simulating)
    {
        g_game_frame.original()(self, edx, simulating);
        if (simulating)
            mem::guarded([] {
                stamina_frame();
                melee_frame();
                merge_shotgun_shells();
                drop_dead_inventories();
                cap_reserves();
                place_map_packs_due();
            });
        infection_frame();
    }

    void install_fire_event(void* events)
    {
        // the infection's triggers, deaths and round starts: FireEvent (see "the infection", "a death, a
        // new game")
        const std::uintptr_t fire_event = mem::vslot(events, 7);
        if (fire_event != 0 && mem::find_pattern(fire_event, 12, "55 8B EC A1 ?? ?? ?? ?? 83 78 30 00") == fire_event &&
            g_fire_event.create("FireEvent", reinterpret_cast<void*>(fire_event), hk_fire_event))
            g_fire_event.enable();
        else
            INV_LOG("game: IGameEventManager2::FireEvent (%p) does not look as expected: no infection, the "
                    "inventories do not follow deaths and new games",
                    reinterpret_cast<void*>(fire_event));
    }

    bool fire_event_installed()
    {
        return g_fire_event.installed();
    }

    void install_game_frame(std::uintptr_t at)
    {
        if (g_game_frame.create("GameFrame", reinterpret_cast<void*>(at), hk_game_frame))
            g_game_frame.enable();
    }

    bool game_frame_installed()
    {
        return g_game_frame.installed();
    }

    void install_transition_restore()
    {
        // CTransitioningPlayer::Restore (see "a chapter transition"): push ebp; mov ebp, esp; sub esp, 0x60; push ebx;
        // mov ebx, [ebp+8]; mov eax, [ebx+0x28] (the player's edict); push esi; mov esi, ecx
        const std::uintptr_t at = g.server.base + k_rva_transition_restore;
        if (mem::find_pattern(at, 16, "55 8B EC 83 EC 60 53 8B 5D 08 8B 43 28 56 8B F1") == at &&
            g_transition_restore.create("TransitionRestore", reinterpret_cast<void*>(at), hk_transition_restore))
            g_transition_restore.enable();
        else
            INV_LOG("game: CTransitioningPlayer::Restore (%p) does not look as expected: the reserve of the stored guns "
                    "is lost at a chapter transition",
                    reinterpret_cast<void*>(at));
    }

} // namespace game::detail
