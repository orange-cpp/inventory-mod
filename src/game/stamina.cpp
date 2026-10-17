// [stamina]: running, jumping, swinging and shoving use it; exhausted, a survivor is slow and weak.

#include "game/stamina.hpp"

#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/lifecycle.hpp"
#include "game/players.hpp"
#include "inv/rules.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace game::detail
{

    // ---- stamina ([stamina] in the rules) -------------------------------------------------------------------------
    //
    // Every server tick (GameFrame), for each living survivor: running (on the ground, faster than a walk), a jump
    // (leaving the ground going up), a melee swing (the melee weapon's next attack time moves on) and a shove (the
    // active weapon's next secondary attack time moves on) use stamina; regen_delay seconds after the last use it
    // comes back.  At 0 the survivor is exhausted until it is back to `recover`: their m_flLaggedMovementValue
    // (the game's own movement speed scale) eases down to `slow` while on the ground, the wait after each swing or
    // shove is stretched to `swing_speed`, and CBaseEntity::TakeDamage scales the damage their melee weapon does
    // to `damage`.  The game's own shove fatigue (m_iShovePenalty) is kept at 0 meanwhile.
    //
    // Checked in the game (2026-10-16): a shove waits 0.77 s (pistol or melee) and moves both attack times on, a
    // melee swing too (with the shove penalty not raised); a reload moves the secondary time a tenth or two;
    // gunfire only the primary.  Exhausted, a swing's 0.99 s wait became 1.65 s and a common's 50 damage 25.
    //   CTerrorPlayer m_iShovePenalty +0x2CF0 (SendProp), SetShovePenalty(int) 0x102CB750 (thiscall), which
    //       marks the edict changed; CBasePlayer m_nButtons +0x1CB4, CBaseEntity m_vecAbsVelocity +0x260 (datamap).
    //
    //   CBasePlayer m_fFlags +0x13C (FL_ONGROUND 1), CBaseEntity m_vecVelocity +0x2D8, CBasePlayer
    //       m_flLaggedMovementValue +0x20D0 (the SendProp / datamap definitions: push <offset> / push "<name>").
    //   CBaseCombatWeapon m_flNextPrimaryAttack +0x13F0, m_flNextSecondaryAttack +0x13F4 (game time).
    //   gpGlobals: server.dll PlayerInfoManager002 slot 1 GetGlobalVars (0x101897E0: mov eax, [0x107F7690]);
    //       CGlobalVars curtime +0xC (what most of server.dll reads of it, compared with m_flNextPrimaryAttack).
    //   server.dll 0x10055C70 CBaseEntity::TakeDamage(const CTakeDamageInfo&) (thiscall, ret 4; it warns
    //       "CBaseEntity::TakeDamage:  with inputInfo.GetDamageForce() == vec3_origin").  CTakeDamageInfo is 0x60
    //       bytes (its copy constructor 0x10010140): m_hInflictor +0x30, m_hAttacker +0x34 (what its GetAttacker,
    //       0x1003D4B0, reads), m_hWeapon +0x38, m_flDamage +0x3C, m_bitsDamageType +0x48.

    constexpr float k_run_speed = 130.f; // faster than this on the ground is running (a walk is 85, a run 220, a limp 150)

    core::Hook<TakeDamageFn> g_take_damage;

    SetShovePenaltyFn g_set_shove_penalty = nullptr;

    struct Stamina
    {
        void* player = nullptr;
        float value = 100.f; // 0 .. 100
        bool exhausted = false;
        float used_at = -1000.f; // game time of the last use
        bool on_ground = true;
        void* weapon = nullptr; // in the hands at the last tick
        bool melee = false; // it is a weapon_melee
        float next_primary = 0.f, next_secondary = 0.f;
        float speed = 1.f; // the slowdown, eased (1: none)
        float applied = 1.f; // the m_flLaggedMovementValue set (1: left alone)
        int sent = -2; // what the player was last told (per mille, -1: none)
        bool sent_exhausted = false;
        ULONGLONG sent_at = 0;
        bool seen = false;
    };
    std::map<int, Stamina> g_stamina; // by userid
    float g_stamina_time = 0.f;
    bool g_any_exhausted = false;

    void write_float(void* entity, std::uintptr_t offset, float value)
    {
        if (mem::guarded([&] { *reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(entity) + offset) = value; }))
            state_changed(entity);
    }

    // The stamina to the player: the host's own session, or "@sta@ <per mille, -1: none> <1: exhausted>" for their
    // client DLL, at most ten times a second while it changes.
    void tell_stamina(void* player, Stamina& st, bool gone)
    {
        const int permille = gone ? -1 : static_cast<int>(std::lround(st.value * 10.f));
        const bool exhausted = !gone && st.exhausted;
        if (permille == st.sent && exhausted == st.sent_exhausted)
            return;
        const ULONGLONG now = GetTickCount64();
        if (player == host_player())
        {
            Session& s = local_session();
            std::lock_guard lock(s.mutex);
            s.stamina = permille < 0 ? -1.f : static_cast<float>(permille) / 1000.f;
            s.stamina_exhausted = exhausted;
        }
        else
        {
            const bool edge = gone || exhausted != st.sent_exhausted || permille == 0 || permille == 1000;
            if (!edge && now - st.sent_at < 100)
                return;
            ServerPlayer* sp = server_player(player);
            void* edict = edict_of(player);
            if (sp != nullptr && sp->has_mod && g_client_printf != nullptr && edict != nullptr)
            {
                char line[48];
                std::snprintf(line, sizeof(line), "%s %d %d\n", k_stamina_marker, permille, exhausted ? 1 : 0);
                mem::guarded([&] { g_client_printf(g_engine_server, edict, line); });
            }
        }
        st.sent = permille;
        st.sent_exhausted = exhausted;
        st.sent_at = now;
    }

    // Done with a player (gone, dead, stamina switched off): their speed back, their bar away.
    void stamina_release(int user_id, Stamina& st)
    {
        void* player = player_by_user_id(user_id);
        if (player == nullptr || player != st.player)
            return;
        if (st.applied != 1.f)
            write_float(player, k_off_lagged_movement, 1.f);
        tell_stamina(player, st, true);
    }

    void stamina_clear()
    {
        for (auto& [user_id, st] : g_stamina)
            stamina_release(user_id, st);
        g_stamina.clear();
        g_any_exhausted = false;
    }

    void stamina_use(int user_id, Stamina& st, float cost, float now, const char* what)
    {
        if (cost <= 0.f)
            return;
        st.value = std::max(0.f, st.value - cost);
        st.used_at = now;
        if (st.value <= 0.f && !st.exhausted)
        {
            st.exhausted = true;
            INV_LOG("stamina: userid %d is exhausted (%s)", user_id, what);
        }
    }

    void stamina_tick(void* player, int user_id, Stamina& st, const inv::StaminaRules& rules, float now, float dt)
    {
        const std::uintptr_t p = reinterpret_cast<std::uintptr_t>(player);
        const bool on_ground = (mem::read_or<int>(p + k_off_flags) & 1) != 0;
        Vec3 velocity{};
        mem::read(p + k_off_velocity, velocity);
        const bool running = on_ground && std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y) > k_run_speed;
        const bool was_exhausted = st.exhausted;

        if (running && !st.exhausted && rules.run > 0.f)
            stamina_use(user_id, st, rules.run * dt, now, "running");
        if (st.on_ground && !on_ground && velocity.z > 150.f)
            stamina_use(user_id, st, rules.jump, now, "a jump");
        // Leaving the ground slowed (a jump, a step off a ledge): the slowdown is m_flLaggedMovementValue, which
        // slows the player's whole movement clock, falls too, so in the air it is 1 and the slowness goes into
        // the speed they leave the ground with instead.
        if (st.on_ground && !on_ground && st.applied < 1.f)
        {
            const float keep = st.applied;
            mem::guarded([&] {
                for (const std::uintptr_t offset : {k_off_velocity, k_off_abs_velocity})
                {
                    float* v = reinterpret_cast<float*>(p + offset);
                    v[0] *= keep;
                    v[1] *= keep;
                }
            });
            state_changed(player);
        }
        st.on_ground = on_ground;

        // the game's own shove fatigue (each shove in a row waits longer) is off: stamina is the limit.  Every
        // shove adds to it, so it also tells a shove from a melee swing (both move both attack times on).
        const int shove_penalty = mem::read_or<int>(p + k_off_shove_penalty);
        if (g_set_shove_penalty != nullptr && shove_penalty != 0)
            mem::guarded([&] { g_set_shove_penalty(player, 0); });

        // a swing or a shove: the weapon's wait for its next (primary / secondary) attack moved on
        void* weapon = entity_from_handle(handle_at(player, k_off_active_weapon));
        const std::uintptr_t w = reinterpret_cast<std::uintptr_t>(weapon);
        if (weapon != st.weapon)
        {
            char name[32];
            st.weapon = weapon;
            st.melee = weapon != nullptr && classname_of(weapon, name, sizeof(name)) && std::strcmp(name, "weapon_melee") == 0;
            st.next_primary = weapon != nullptr ? mem::read_or<float>(w + k_off_next_primary) : 0.f;
            st.next_secondary = weapon != nullptr ? mem::read_or<float>(w + k_off_next_secondary) : 0.f;
        }
        else if (weapon != nullptr)
        {
            float primary = mem::read_or<float>(w + k_off_next_primary);
            float secondary = mem::read_or<float>(w + k_off_next_secondary);
            // a melee swing moves both on, so the button held tells them apart (a shove: IN_ATTACK2)
            const int buttons = mem::read_or<int>(p + k_off_buttons);
            const bool primary_moved = primary > st.next_primary + 0.05f && primary > now;
            // (a shove waits 0.77 s; a reload moves the secondary time on by a tenth or two, so it is not one)
            const bool secondary_moved = secondary > st.next_secondary + 0.05f && secondary > now + 0.4f;
            const bool shove =
                secondary_moved && (shove_penalty != 0 || (buttons & k_in_attack2) != 0 || !st.melee || !primary_moved);
            const bool swing = !shove && st.melee && primary_moved;
            if (shove || swing)
            {
                if (was_exhausted && rules.swing_speed < 100.f)
                {
                    // the wait for the next one, longer
                    const float stretch = 100.f / rules.swing_speed;
                    if (primary > now)
                        write_float(weapon, k_off_next_primary, primary = now + (primary - now) * stretch);
                    if (shove && secondary > now)
                        write_float(weapon, k_off_next_secondary, secondary = now + (secondary - now) * stretch);
                }
                stamina_use(user_id, st, shove ? rules.shove : rules.swing, now, shove ? "a shove" : "a swing");
            }
            st.next_primary = primary;
            st.next_secondary = secondary;
        }

        // rest: it comes back (at half the rate while an exhausted survivor keeps running)
        if (now - st.used_at >= rules.regen_delay && st.value < 100.f)
            st.value = std::min(100.f, st.value + rules.regen * dt * (running ? 0.5f : 1.f));
        if (st.exhausted && st.value >= std::max(rules.recover, 0.1f))
        {
            st.exhausted = false;
            INV_LOG("stamina: userid %d got their breath back (%.0f)", user_id, st.value);
        }

        // the speed eases to `slow` and back; on the ground only (see above)
        const float target = st.exhausted ? rules.slow / 100.f : 1.f;
        const float step = 1.5f * dt;
        st.speed = st.speed < target ? std::min(target, st.speed + step) : std::max(target, st.speed - step);
        const float apply = on_ground ? st.speed : 1.f;
        if (apply != st.applied || (apply != 1.f && mem::read_or<float>(p + k_off_lagged_movement, apply) != apply))
        {
            write_float(player, k_off_lagged_movement, apply);
            st.applied = apply;
        }
        tell_stamina(player, st, false);
    }

    void stamina_frame()
    {
        const inv::StaminaRules& rules = inv::stamina_rules();
        if (!rules.enabled || g_globals == nullptr)
        {
            if (!g_stamina.empty())
                stamina_clear();
            return;
        }
        const float now = game_time();
        float dt = now - g_stamina_time;
        if (dt == 0.f)
            return; // the same tick
        g_stamina_time = now;
        if (dt < 0.f || dt > 0.25f)
            dt = 0.f; // a new map, a long hitch
        for (auto& [user_id, st] : g_stamina)
            st.seen = false;
        for (int index = 1; index <= 64; ++index)
        {
            void* player = entity_at_index(index);
            char name[16];
            if (player == nullptr || !classname_of(player, name, sizeof(name)) || std::strcmp(name, "player") != 0 ||
                team_of(player) != 2 || !alive(player) || (!rules.bots && is_bot(player)))
                continue;
            const int user_id = user_id_of(player);
            if (user_id <= 0)
                continue;
            Stamina& st = g_stamina[user_id];
            if (st.player != player)
            {
                st = Stamina{};
                st.player = player;
                st.applied = mem::read_or<float>(reinterpret_cast<std::uintptr_t>(player) + k_off_lagged_movement, 1.f);
                if (st.applied != 1.f)
                    INV_LOG("stamina: userid %d starts with a speed scale of %.2f", user_id, st.applied);
            }
            st.seen = true;
            stamina_tick(player, user_id, st, rules, now, dt);
        }
        g_any_exhausted = false;
        for (auto it = g_stamina.begin(); it != g_stamina.end();)
        {
            if (!it->second.seen)
            {
                stamina_release(it->first, it->second);
                it = g_stamina.erase(it);
                continue;
            }
            g_any_exhausted |= it->second.exhausted;
            ++it;
        }
    }

    // An exhausted survivor's melee weapon does less damage.
    int __fastcall hk_take_damage(void* self, void* edx, const void* info)
    {
        if (!g_stamina.empty() && info != nullptr)
        {
            alignas(16) unsigned char copy[k_damage_info_size];
            bool scaled = false;
            mem::guarded([&] {
                const std::uintptr_t i = reinterpret_cast<std::uintptr_t>(info);
                void* attacker = entity_from_handle(*reinterpret_cast<const std::uint32_t*>(i + k_off_damage_attacker));
                if (attacker == nullptr)
                    return;
                for (const auto& entry : g_stamina)
                {
                    const Stamina& st = entry.second;
                    if (st.player != attacker)
                        continue;
                    if (!st.exhausted || !st.melee || st.weapon == nullptr)
                        return;
                    if (entity_from_handle(*reinterpret_cast<const std::uint32_t*>(i + k_off_damage_weapon)) != st.weapon &&
                        entity_from_handle(*reinterpret_cast<const std::uint32_t*>(i + k_off_damage_inflictor)) != st.weapon)
                        return;
                    std::memcpy(copy, info, sizeof(copy));
                    // (seen in the game: a common infected's 50 became 25, so it took two hits)
                    *reinterpret_cast<float*>(copy + k_off_damage) *= inv::stamina_rules().damage / 100.f;
                    scaled = true;
                    return;
                }
            });
            if (scaled)
                return g_take_damage.original()(self, edx, copy);
        }
        return g_take_damage.original()(self, edx, info);
    }

    void install_stamina()
    {
        // the game's clock, and TakeDamage for an exhausted survivor's melee damage
        const auto server_create = reinterpret_cast<CreateInterfaceFn>(
            GetProcAddress(reinterpret_cast<HMODULE>(g.server.base), "CreateInterface"));
        void* info_manager = server_create != nullptr ? server_create("PlayerInfoManager002", nullptr) : nullptr;
        const std::uintptr_t get_globals = info_manager != nullptr ? mem::vslot(info_manager, 1) : 0;
        const std::uintptr_t take_damage = g.server.base + k_rva_take_damage;
        if (get_globals != 0 && mem::find_pattern(get_globals, 6, "A1 ?? ?? ?? ?? C3") == get_globals)
            g_globals = mem::read_or<void*>(mem::read_or<std::uintptr_t>(get_globals + 1));
        const std::uintptr_t set_shove_penalty = g.server.base + k_rva_set_shove_penalty;
        const std::uintptr_t accessor_call = g.server.base + k_rva_change_accessor_call;
        // the server's own engine pointer (what 0x100ED4B0 calls slot 97 of) is the interface used here
        const std::uintptr_t change_accessor =
            g_engine_server != nullptr ? mem::vslot(g_engine_server, k_vt_change_accessor) : 0;
        const bool same_engine = mem::read_or<void*>(g.server.base + k_rva_server_engine) == g_engine_server;
        if (g_globals != nullptr && same_engine && change_accessor != 0 &&
            mem::find_pattern(accessor_call, 18, "51 8B 0D ?? ?? ?? ?? 8B 11 8B 82 84 01 00 00 FF D0 C3") == accessor_call &&
            mem::find_pattern(set_shove_penalty, 16, "55 8B EC 56 57 8B 7D 08 8B F1 39 BE F0 2C 00 00") == set_shove_penalty &&
            game_frame_installed() &&
            mem::find_pattern(take_damage, 11, "55 8B EC 83 EC 6C 57 8B F9 8B 0D") == take_damage &&
            g_take_damage.create("TakeDamage", reinterpret_cast<void*>(take_damage), hk_take_damage) && g_take_damage.enable())
        {
            g_change_accessor = reinterpret_cast<ChangeAccessorFn>(change_accessor);
            g_set_shove_penalty = reinterpret_cast<SetShovePenaltyFn>(set_shove_penalty);
            INV_LOG("game: stamina ready (gpGlobals %p, game time %.1f; on when inventory_mod.toml has [stamina])", g_globals,
                    game_time());
        }
        else
        {
            g_globals = nullptr;
            INV_LOG("game: the stamina's functions do not look as expected: no stamina");
        }
    }

} // namespace game::detail
