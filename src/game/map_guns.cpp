// [map_guns]: a gun nobody has held yet has a part of its magazine; the rounds are the player's, not the gun's.

#include "game/map_guns.hpp"

#include "core/log.hpp"
#include "game/weapons.hpp"
#include "inv/rules.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <set>

namespace game::detail
{

    // ---- the map's guns, a part of a magazine ([map_guns]) ---------------------------------------------------------
    //
    // With [map_guns] on, a gun nobody has held yet (one lying in the map, or a spawner's fresh copy) has only
    // min_clip..max_clip % of its magazine (m_iClip1, full on such a gun) when a survivor first picks it up.  A gun
    // is known once anyone has held it (every survivor's m_hMyWeapons, ten times a second: see cap_reserves), the
    // mod dropped it from a box or gave it out, or it was cut down here; a known gun keeps its magazine.  By EHANDLE;
    // a round starting (on_round_start), or the clock going back by more than a second (a new map), forgets them.
    // Not any step back: while a player's command runs (a +use, the pick-up) gpGlobals->curtime is that player's
    // tick base, which can be a little behind the frame's clock that GameFrame sees, and forgetting on that cut the
    // same gun again on every swap (seen in the log 2026-10-17: one auto shotgun cut 5 -> 2 -> 1 -> 0 rounds in
    // its magazine within three seconds of swapping it back and forth).

    std::set<std::uint32_t> g_known_guns; // main thread only
    float g_known_guns_time = 0.f;


    void known_guns_clock()
    {
        const float now = game_time();
        if (now + 1.f < g_known_guns_time)
        {
            g_known_guns.clear(); // a new map
            g_known_guns_time = now;
        }
        else
            g_known_guns_time = std::max(now, g_known_guns_time);
    }

    void know_gun(void* weapon)
    {
        if (weapon == nullptr || !inv::map_gun_rules().enabled)
            return;
        known_guns_clock();
        if (const std::uint32_t handle = handle_of(weapon); handle != k_invalid_handle)
            g_known_guns.insert(handle);
    }

    void forget_map_guns()
    {
        g_known_guns.clear();
        g_known_guns_time = 0.f;
    }

    // Every gun a survivor holds is known (cap_reserves).
    void know_held_guns(void* player)
    {
        if (!inv::map_gun_rules().enabled)
            return;
        known_guns_clock();
        for (int i = 0; i < k_max_weapons; ++i)
        {
            const std::uint32_t handle = handle_at(player, k_off_my_weapons + 4u * static_cast<unsigned>(i));
            if (handle != k_invalid_handle && entity_from_handle(handle) != nullptr)
                g_known_guns.insert(handle);
        }
    }

    // A gun nobody has held does not fill the reserve either.  A gun stored into a box adds the rounds it carries
    // (m_iExtraPrimaryAmmo, zeroed here), and a map's weapon spawner fills the player's reserve of its gun's type
    // after it has handed the gun out (seen: the pick-up returned, then the reserve was at its most), also when
    // the player already has that gun and gets none.  So the player's reserve of the gun's ammo type is read before,
    // and afterwards anything above that plus min_reserve..max_reserve magazines (rolled; 0 when no new gun came) is
    // taken back.  An object of this lives through the pick-up, or through the spawner's whole Use (g_spawn_reserve:
    // the pick-up inside it fills that one in).
    MapGunReserve::~MapGunReserve()
    {
        if (player == nullptr || type < 0 || type >= 32)
            return;
        int* reserve = reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(player) + k_off_player_ammo + 4u * static_cast<unsigned>(type));
        const int after = mem::read_or<int>(reinterpret_cast<std::uintptr_t>(reserve), before);
        const int allowed = before + bonus;
        if (after <= allowed)
            return;
        mem::guarded([&] { *reserve = allowed; });
        state_changed(player);
        INV_LOG("map gun: the reserve of %s would have gone %d -> %d, kept at %d (+%d)", kind.c_str(), before, after,
                allowed, bonus);
    }

    MapGunReserve* g_spawn_reserve = nullptr; // main thread: a spawner's Use is running (see spawn_use)

    // The rounds are the player's, not the gun's, also through the game's own pick-up and swap.  The game moves
    // them with the guns: a gun dropped (server.dll 0x102476B0, when no other carried gun takes its ammo type) gets
    // the player's whole reserve of its type (GetAmmoCount 0x10046DF0 -> the gun's SetExtraAmmo 0x102455E0, which
    // writes m_iExtraPrimaryAmmo +0x14F4) and the player's goes to 0 (SetAmmoCount(count, type) 0x10046DC0); a gun
    // picked up with rounds of its own (the slot-426 pick-up 0x1031D080 at 0x1031D186 and 0x1031D34C,
    // CTerrorGun::Use at 0x103E87DB) has them zeroed and SETS the player's reserve of its type to them, whatever
    // the player had.  So swapping a rifle for a shotgun lost every rifle round, and taking a dropped gun replaced
    // the reserve with its rounds.  An object of this lives through such a pick-up or drop: the reserve is read
    // before, and afterwards no type is below what it was; a gun's own rounds that were taken are added to what
    // the player had (up to the type's most, as a box adds them); and a gun the player let go of lies there with
    // its magazine only (its m_iExtraPrimaryAmmo zeroed: the rounds did not leave).  `player` nullptr: nothing.
    const char* KeepRounds::gun_kind(void* weapon, char* name, int length)
    {
        const char* kind = weapon != nullptr && classname_of(weapon, name, length) ? inv::ammo_kind(name) : nullptr;
        return kind != nullptr && std::strcmp(kind, "chainsaw") != 0 ? kind : nullptr;
    }

    KeepRounds::KeepRounds(void* player_, void* gun_) : player(player_), gun(gun_)
    {
        if (player == nullptr)
            return;
        before = read_pool(player);
        char name[64];
        if (gun_kind(gun, name, sizeof(name)) != nullptr)
            gun_extra = mem::read_or<int>(reinterpret_cast<std::uintptr_t>(gun) + k_off_extra_ammo, 0);
        for (int i = 0; i < k_max_weapons; ++i)
            carried[i] = handle_at(player, k_off_my_weapons + 4u * static_cast<unsigned>(i));
    }

    KeepRounds::~KeepRounds()
    {
        if (player != nullptr && before.read)
            mem::guarded([&] { settle(); });
    }

    void KeepRounds::settle()
    {
        AmmoPool after = read_pool(player);
        if (!after.read)
            return;
        bool changed = false;
        bool kept[32] = {};
        for (int type = 0; type < 32; ++type)
            if (after.count[type] < before.count[type])
            {
                INV_LOG("rounds: the game took %d rounds of ammo type %d with a gun, they stay with the player",
                        before.count[type] - after.count[type], type);
                after.count[type] = before.count[type];
                kept[type] = changed = true;
            }
        char name[64];
        const char* kind = gun_extra > 0 ? gun_kind(gun, name, sizeof(name)) : nullptr;
        const int type = gun != nullptr ? ammo_type_of(gun) : -1;
        if (kind != nullptr && type >= 0 && mem::read_or<int>(reinterpret_cast<std::uintptr_t>(gun) + k_off_extra_ammo, -1) == 0)
        {
            const int max = inv::ammo_max(kind);
            int want = before.count[type] + gun_extra;
            if (max >= 0)
                want = std::min(want, std::max(before.count[type], max));
            if (after.count[type] < want)
            {
                INV_LOG("rounds: the %s's own %d rounds join the %d the player had (%d)", name, gun_extra, before.count[type], want);
                after.count[type] = want;
                changed = true;
            }
        }
        if (changed)
        {
            write_pool(player, after);
            state_changed(player);
        }
        for (const std::uint32_t handle : carried)
        {
            void* weapon = handle != k_invalid_handle ? entity_from_handle(handle) : nullptr;
            if (weapon == nullptr || gun_kind(weapon, name, sizeof(name)) == nullptr)
                continue;
            const int dropped_type = ammo_type_of(weapon);
            if (dropped_type < 0 || !kept[dropped_type])
                continue;
            bool still = false;
            for (int i = 0; i < k_max_weapons && !still; ++i)
                still = handle_at(player, k_off_my_weapons + 4u * static_cast<unsigned>(i)) == handle;
            if (still)
                continue;
            *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(weapon) + k_off_extra_ammo) = 0;
            state_changed(weapon);
            INV_LOG("rounds: the %s let go of keeps its magazine only", name);
        }
    }

    std::minstd_rand& map_gun_random()
    {
        static std::minstd_rand random(static_cast<unsigned>(GetTickCount64() * 3266489917u));
        return random;
    }

    // [map_guns]: min_clip..max_clip % (rolled) of a `full` magazine.
    int rolled_map_clip(int full)
    {
        const inv::MapGunRules& rules = inv::map_gun_rules();
        const int percent =
            rules.min_clip + static_cast<int>(map_gun_random()() % static_cast<unsigned>(rules.max_clip - rules.min_clip + 1));
        return std::clamp(static_cast<int>(std::lround(full * percent / 100.0)), 0, full);
    }

    // A survivor is picking `weapon` up: a gun nobody has held gets a part of its magazine, and `reserve` (or the
    // running spawner's) is set to limit what the pick-up adds to the reserve.
    void cut_map_gun(void* player, void* weapon, MapGunReserve& own_reserve)
    {
        MapGunReserve& reserve = g_spawn_reserve != nullptr && g_spawn_reserve->player == player ? *g_spawn_reserve : own_reserve;
        const inv::MapGunRules& rules = inv::map_gun_rules();
        if (!rules.enabled || weapon == nullptr || player == nullptr || team_of(player) != 2)
            return;
        if (g_giving)
        {
            know_gun(weapon);
            return;
        }
        char name[64];
        const char* kind = classname_of(weapon, name, sizeof(name)) ? inv::ammo_kind(name) : nullptr;
        if (kind == nullptr || std::strcmp(kind, "chainsaw") == 0)
            return; // (not a gun with rounds: a melee weapon, a pistol, an item)
        known_guns_clock();
        const std::uint32_t handle = handle_of(weapon);
        if (handle == k_invalid_handle || !g_known_guns.insert(handle).second)
            return; // known
        const auto at = reinterpret_cast<std::uintptr_t>(weapon) + k_off_clip;
        const int full = mem::read_or<int>(at, -1);
        if (full <= 0)
            return;
        std::minstd_rand& random = map_gun_random();
        const int clip = rolled_map_clip(full);
        const int magazines =
            rules.min_reserve + static_cast<int>(random() % static_cast<unsigned>(rules.max_reserve - rules.min_reserve + 1));
        mem::guarded([&] {
            *reinterpret_cast<int*>(at) = clip;
            // (CTerrorGun::m_iExtraPrimaryAmmo: what a gun lying around carries for the reserve; every gun here is a
            // CTerrorGun, the pistols and the chainsaw are left out above)
            *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(weapon) + k_off_extra_ammo) = 0;
        });
        state_changed(weapon);
        reserve.player = player;
        reserve.type = ammo_type_of(weapon);
        reserve.kind = kind;
        reserve.bonus = magazines * std::max(1, inv::ammo_magazine(kind));
        if (reserve.spawner)
            reserve.before = reserve.type >= 0 && reserve.type < 32 ? reserve.pool_before[reserve.type] : 0;
        else
            reserve.before = reserve.type >= 0 ? mem::read_or<int>(reinterpret_cast<std::uintptr_t>(player) + k_off_player_ammo +
                                                                    4u * static_cast<unsigned>(reserve.type))
                                               : 0;
        INV_LOG("map gun %s (%p): %d of %d in the magazine (%d%%), %d reserve rounds at most", name, weapon, clip, full,
                full > 0 ? clip * 100 / full : 0, reserve.bonus);
    }

} // namespace game::detail
