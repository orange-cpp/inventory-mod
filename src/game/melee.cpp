// [melee]: the melee of No More Room in Hell 2 on the game's own swing: light and heavy swings, the wind-up, the queue
// and the cleave.

#include "game/melee.hpp"

#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/client_view.hpp"
#include "game/entities.hpp"
#include "game/lifecycle.hpp"
#include "game/players.hpp"
#include "game/stamina.hpp"
#include "inv/melee.hpp"
#include "inv/rules.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace game::detail
{

    using namespace sdk;

    // ---- the server's half --------------------------------------------------------------------------------------
    //
    // The game swings a melee weapon in CTerrorMeleeWeapon::PrimaryAttack (slot 282 of its vtable 0x106D611C,
    // 0x103EC3B0): it checks that the player can attack ("melee attack failed - !player->CanAttack()") and starts the
    // swing (0x103EC080), which sets m_flNextPrimaryAttack to curtime + the weapon's refire (and the secondary time:
    // no shove out of a swing).  It runs while the player's user command is processed, so the player's m_nButtons
    // are that command's: the mod's client marks a heavy swing there (inv::k_button_heavy).  The hook lets the game
    // swing, then, when the wait moved on (a swing started): the swing is heavy when the client says so, the server
    // saw it wind up (inv::k_button_winding in m_nButtons, sampled every GameFrame) for heavy_hold less a tick or
    // two, and the survivor is not exhausted.  A heavy swing stretches both waits to heavy_recovery % and uses
    // heavy_stamina in all ([stamina]'s detection in GameFrame takes its `swing`; this adds the rest).
    //
    // The damage: server.dll 0x10055C70 CBaseEntity::TakeDamage(const CTakeDamageInfo&) (thiscall, ret 4; it warns
    // "CBaseEntity::TakeDamage:  with inputInfo.GetDamageForce() == vec3_origin").  CTakeDamageInfo is 0x60 bytes
    // (its copy constructor 0x10010140): m_hInflictor +0x30, m_hAttacker +0x34 (what its GetAttacker, 0x1003D4B0,
    // reads), m_hWeapon +0x38, m_flDamage +0x3C, m_bitsDamageType +0x48.  A hit of the attacker's melee weapon (as
    // the weapon or the inflictor) within k_swing_hits_for of a swing's start belongs to that swing: light_damage %
    // or heavy_damage %, times the cleave for each target after the first (inv::cleave_scale), times
    // stamina_damage_scale (an exhausted survivor).  The game deals a swing's hits over the frames of the swing, each
    // target once (seen in the game: a heavy swing's hit 0.2 - 0.3 s after it started, a common's 50 became 100; one
    // light swing into a crowd hit four commons within 0.1 s, 50, 35, 25, 17).  Each target hit also gets a few
    // follow-up calls of 0 damage (-1 for one dying): those are left alone and do not count.  A teammate hit takes
    // the game's own friendly fire and does not count in the cleave either.  A hit to the head of an infected (its
    // CBaseCombatCharacter::m_LastHitGroup +0x1838 is HITGROUP_HEAD 1: the swing's trace attack sets it before the
    // damage) does head_damage % more.  With light_damage 40 a common (m_iHealth +0xEC: 50) takes three light hits
    // to the body, two to the head, as NMRiH2's light attacks.
    //
    // Bots swing through the same PrimaryAttack (never heavy); the cleave and light_damage apply to them too.

    constexpr float k_swing_hits_for = 1.5f; // a hit this long after its swing started is still that swing's
    constexpr float k_wind_slack = 0.15f; // GameFrame samples the wind-up bit once a tick (30 a second)

    core::Hook<PrimaryAttackFn> g_primary_attack;
    core::Hook<TakeDamageFn> g_take_damage;

    struct Swinger
    {
        void* player = nullptr;
        float winding_since = -1.f; // game time the client's wind-up bit was first seen (-1: not winding)
        float winding_last = -1.f; // ... and last seen
        float wound = 0.f; // the last wind-up's length
        float wound_at = -1000.f; // when it ended
        void* weapon = nullptr; // the last swing's
        float swing_at = -1000.f;
        bool heavy = false;
        std::vector<void*> victims; // what the last swing hit, in order
        int sent_hold = -1, sent_queue = -1; // what the player's client was last told
        bool sent_mod = false; // ... while they ran the mod
        ULONGLONG checked_at = 0; // the last look at whether they run the mod
        int user_id = 0;
        float applied = 1.f; // the m_flLaggedMovementValue melee_frame set (without [stamina]; 1: left alone)
        bool seen = false;
    };
    std::vector<Swinger> g_swingers;

    Swinger* swinger_of(void* player, bool create)
    {
        for (Swinger& s : g_swingers)
            if (s.player == player)
                return &s;
        if (!create || player == nullptr)
            return nullptr;
        g_swingers.push_back(Swinger{.player = player});
        return &g_swingers.back();
    }

    // The server's [melee] to the player: the host's own session, or "@mel@ <hold ms> <queue ms>" for their client
    // DLL (0 0: none), whenever it changes and once they are known to run the mod.
    void tell_melee(void* player, Swinger& s, const inv::MeleeRules& rules)
    {
        const int hold = rules.enabled ? static_cast<int>(std::lround(rules.heavy_hold * 1000.f)) : 0;
        const int queue = rules.enabled ? static_cast<int>(std::lround(rules.queue * 1000.f)) : 0;
        if (player == host_player())
        {
            if (hold == s.sent_hold && queue == s.sent_queue)
                return;
            Session& local = local_session();
            std::lock_guard lock(local.mutex);
            local.melee_hold = static_cast<float>(hold) / 1000.f;
            local.melee_queue = static_cast<float>(queue) / 1000.f;
        }
        else
        {
            const ULONGLONG now = GetTickCount64();
            if (hold == s.sent_hold && queue == s.sent_queue && (s.sent_mod || now - s.checked_at < 1000))
                return;
            s.checked_at = now;
            ServerPlayer* sp = server_player(player);
            void* edict = edict_of(player);
            if (sp == nullptr || !sp->has_mod || g_client_printf == nullptr || edict == nullptr)
            {
                s.sent_mod = false;
                return;
            }
            char line[48];
            std::snprintf(line, sizeof(line), "%s %d %d\n", k_melee_marker, hold, queue);
            mem::guarded([&] { g_client_printf(g_engine_server, edict, line); });
            s.sent_mod = true;
        }
        s.sent_hold = hold;
        s.sent_queue = queue;
    }

    // Winding up slows the player (NMRiH2's wind-up is a slow walk): from 1 to wind_speed % over heavy_hold.  The
    // speed scale is CBasePlayer::m_flLaggedMovementValue +0x20D0, the stamina's too (see stamina.cpp: it slows the
    // whole movement clock, so it is set on the ground only); with [stamina] its tick multiplies this in, without it
    // melee_frame sets it.  Let go, it is back at once.
    float melee_move_scale(void* player)
    {
        const inv::MeleeRules& rules = inv::melee_rules();
        const Swinger* s = rules.enabled ? swinger_of(player, false) : nullptr;
        if (s == nullptr || s->winding_since < 0.f || rules.heavy_hold <= 0.f)
            return 1.f;
        const float charge = std::clamp((game_time() - s->winding_since) / rules.heavy_hold, 0.f, 1.f);
        return 1.f - (1.f - rules.wind_speed / 100.f) * charge;
    }

    void set_lagged_movement(void* player, float value)
    {
        if (mem::guarded([&] { *reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(player) + k_off_lagged_movement) = value; }))
            state_changed(player);
    }

    void melee_frame()
    {
        if (g_globals == nullptr || !g_primary_attack.installed())
            return;
        const inv::MeleeRules& rules = inv::melee_rules();
        const float now = game_time();
        for (Swinger& s : g_swingers)
            s.seen = false;
        for (int index = 1; index <= 64; ++index)
        {
            void* player = entity_at_index(index);
            char name[16];
            if (player == nullptr || !classname_of(player, name, sizeof(name)) || std::strcmp(name, "player") != 0 ||
                team_of(player) != k_team_survivors || !alive(player))
                continue;
            Swinger& s = *swinger_of(player, true);
            s.seen = true;
            const int buttons = mem::read_or<int>(reinterpret_cast<std::uintptr_t>(player) + k_off_buttons);
            if (rules.enabled && (buttons & inv::k_button_winding) != 0)
            {
                if (s.winding_since < 0.f)
                    s.winding_since = now;
                s.winding_last = now;
            }
            else if (s.winding_since >= 0.f)
            {
                s.wound = s.winding_last - s.winding_since;
                s.wound_at = now;
                s.winding_since = -1.f;
            }
            if (!is_bot(player))
                tell_melee(player, s, rules);
            s.user_id = user_id_of(player);
            // the wind-up's slowdown, when the stamina does not apply it
            if (!inv::stamina_rules().enabled)
            {
                const bool on_ground = (mem::read_or<int>(reinterpret_cast<std::uintptr_t>(player) + k_off_flags) & 1) != 0;
                const float scale = on_ground ? melee_move_scale(player) : 1.f;
                if (scale != s.applied)
                {
                    set_lagged_movement(player, scale);
                    s.applied = scale;
                }
            }
            else
                s.applied = 1.f; // the stamina's now
        }
        for (const Swinger& s : g_swingers)
            if (!s.seen && s.applied != 1.f && player_by_user_id(s.user_id) == s.player)
                set_lagged_movement(s.player, 1.f); // died while winding up
        std::erase_if(g_swingers, [](const Swinger& s) { return !s.seen; });
    }

    void swing_started(void* weapon, float before)
    {
        const inv::MeleeRules& rules = inv::melee_rules();
        const auto w = reinterpret_cast<std::uintptr_t>(weapon);
        const float now = game_time();
        const float primary = mem::read_or<float>(w + k_off_next_primary);
        if (!(primary > before + 0.05f && primary > now))
            return; // it did not swing (not ready, or the player cannot attack)
        void* player = entity_from_handle(mem::read_or<std::uint32_t>(w + k_off_weapon_owner, k_invalid_handle));
        Swinger* s = swinger_of(player, true);
        if (s == nullptr)
            return;
        const int buttons = mem::read_or<int>(reinterpret_cast<std::uintptr_t>(player) + k_off_buttons);
        const float wound = s->winding_since >= 0.f ? s->winding_last - s->winding_since : now - s->wound_at < 1.f ? s->wound : 0.f;
        const bool claimed = (buttons & inv::k_button_heavy) != 0;
        const bool exhausted = stamina_exhausted(player);
        s->weapon = weapon;
        s->swing_at = now;
        s->heavy = claimed && !exhausted && wound + k_wind_slack >= rules.heavy_hold;
        s->victims.clear();
        if (claimed && !s->heavy)
            INV_LOG("melee: userid %d: a heavy swing refused (wound up %.2f s of %.2f%s)", user_id_of(player), wound,
                    rules.heavy_hold, exhausted ? ", exhausted" : "");
        if (!s->heavy)
            return;
        const float stretch = rules.heavy_recovery / 100.f;
        const float secondary = mem::read_or<float>(w + k_off_next_secondary);
        if (mem::guarded([&] {
                *reinterpret_cast<float*>(w + k_off_next_primary) = now + (primary - now) * stretch;
                if (secondary > now)
                    *reinterpret_cast<float*>(w + k_off_next_secondary) = now + (secondary - now) * stretch;
            }))
            state_changed(weapon);
        if (inv::stamina_rules().enabled)
            stamina_spend(player, rules.heavy_stamina - inv::stamina_rules().swing, "a heavy swing");
        INV_LOG("melee: userid %d: a heavy swing (wound up %.2f s; the wait %.2f s)", user_id_of(player), wound,
                (primary - now) * stretch);
    }

    void __fastcall hk_primary_attack(void* weapon, void* edx)
    {
        const float before = mem::read_or<float>(reinterpret_cast<std::uintptr_t>(weapon) + k_off_next_primary);
        g_primary_attack.original()(weapon, edx);
        if (inv::melee_rules().enabled && g_globals != nullptr)
            mem::guarded([&] { swing_started(weapon, before); });
    }

    // What a hit does, as a factor (1: as the game would).
    float melee_damage_scale(void* victim, const void* info)
    {
        const auto i = reinterpret_cast<std::uintptr_t>(info);
        void* attacker = entity_from_handle(mem::read_or<std::uint32_t>(i + k_off_damage_attacker, k_invalid_handle));
        if (attacker == nullptr || attacker == victim)
            return 1.f;
        void* active = entity_from_handle(handle_at(attacker, k_off_active_weapon));
        if (active == nullptr ||
            (entity_from_handle(mem::read_or<std::uint32_t>(i + k_off_damage_weapon, k_invalid_handle)) != active &&
             entity_from_handle(mem::read_or<std::uint32_t>(i + k_off_damage_inflictor, k_invalid_handle)) != active))
            return 1.f;
        float scale = stamina_damage_scale(attacker, active); // checks that it is a melee weapon itself
        const inv::MeleeRules& rules = inv::melee_rules();
        Swinger* s = rules.enabled ? swinger_of(attacker, false) : nullptr;
        if (s == nullptr || s->weapon != active || game_time() - s->swing_at > k_swing_hits_for)
            return scale; // not a melee swing the hook saw
        if (team_of(victim) == k_team_survivors)
            return scale; // a teammate: the game's own friendly fire, not counted in the cleave
        if (mem::read_or<float>(i + k_off_damage) <= 0.f)
            return 1.f; // the game's follow-ups to a target already hit (0, and -1 for one dying): left as they are
        const auto it = std::find(s->victims.begin(), s->victims.end(), victim);
        const int earlier = static_cast<int>(it - s->victims.begin());
        if (it == s->victims.end())
            s->victims.push_back(victim);
        const float cleave = inv::cleave_scale(earlier, rules.cleave, rules.cleave_min);
        scale *= (s->heavy ? rules.heavy_damage : rules.light_damage) / 100.f * cleave;
        // the head: the infected are combat characters (commons, the witch, the specials), whose m_LastHitGroup the
        // swing's trace attack has just set
        const auto v = reinterpret_cast<std::uintptr_t>(victim);
        const bool head = team_of(victim) == 3 && mem::read_or<int>(v + k_off_last_hit_group) == k_hitgroup_head;
        if (head)
            scale *= rules.head_damage / 100.f;
        if (!is_bot(attacker))
            INV_LOG("melee: userid %d hit target %d of the swing%s%s: %.1f damage x%.2f (health %d)", user_id_of(attacker),
                    earlier + 1, s->heavy ? " (heavy)" : "", head ? " (head)" : "", mem::read_or<float>(i + k_off_damage), scale,
                    mem::read_or<int>(v + k_off_health));
        return scale;
    }

    int __fastcall hk_take_damage(void* self, void* edx, const void* info)
    {
        if (info != nullptr && (!g_swingers.empty() || inv::stamina_rules().enabled))
        {
            float scale = 1.f;
            mem::guarded([&] { scale = melee_damage_scale(self, info); });
            if (scale != 1.f)
            {
                alignas(16) unsigned char copy[k_damage_info_size];
                std::memcpy(copy, info, sizeof(copy));
                *reinterpret_cast<float*>(copy + k_off_damage) *= scale;
                return g_take_damage.original()(self, edx, copy);
            }
        }
        return g_take_damage.original()(self, edx, info);
    }

    void install_melee()
    {
        const std::uintptr_t take_damage = g.server.base + k_rva_take_damage;
        if (g_globals == nullptr || mem::find_pattern(take_damage, 11, "55 8B EC 83 EC 6C 57 8B F9 8B 0D") != take_damage ||
            !g_take_damage.create("TakeDamage", reinterpret_cast<void*>(take_damage), hk_take_damage) || !g_take_damage.enable())
        {
            INV_LOG("game: TakeDamage does not look as expected (or no gpGlobals): no [melee], and an exhausted survivor's "
                    "melee weapon does its full damage");
            return;
        }
        const std::uintptr_t primary = g.server.base + k_rva_melee_primary_attack;
        if (game_frame_installed() &&
            mem::read_or<std::uintptr_t>(g.server.base + k_rva_melee_vtable + 4u * k_vt_primary_attack) == primary &&
            mem::find_pattern(primary, 25, "56 57 8B F1 E8 ?? ?? ?? ?? 8B F8 85 FF 0F 84 ?? ?? ?? ?? 8B 87 D8 33 00 00") == primary &&
            g_primary_attack.create("MeleePrimaryAttack", reinterpret_cast<void*>(primary), hk_primary_attack) &&
            g_primary_attack.enable())
            INV_LOG("game: melee ready (on when inventory_mod.toml has [melee])");
        else
            INV_LOG("game: the melee weapon's PrimaryAttack does not look as expected: no [melee]");
    }

    // ---- the client's half --------------------------------------------------------------------------------------
    //
    // The attack button of each user command, while a melee weapon is in the hands and the server has [melee]
    // (inv::MeleeInput): held back while winding up, given when let go until the weapon swings.  It has to be the
    // client's input: the game predicts the swing on the client from the same user command, so a swing the server
    // alone held back would still play on the client.  CInput::CreateMove (slot 3 of CInput's vtable 0x1057575C,
    // 0x101341F0) builds the command and stores this->GetButtonBits(1) (slot 2, 0x10131570, thiscall, ret 4: the
    // buttons from the kbuttons +attack ...) in CUserCmd::buttons (+0x24); ExtraMouseSample asks GetButtonBits(0)
    // between commands.  The hook changes what it returns, so the prediction and the server see the same buttons.
    // CUserCmd::buttons goes to the server as 32 bits: the two bits the mod adds (inv::k_button_winding /
    // k_button_heavy, 1 << 28 and 1 << 29, which the game does not use) arrive in the player's m_nButtons.
    //
    //   A melee weapon in the hands: the client's active weapon's vtable is C_TerrorMeleeWeapon's (0x10609BFC, the one
    //       of its complete object locator with offset 0; checked by its RTTI name).  Its m_flNextPrimaryAttack
    //       (+0x960) moves on when the client predicts the swing: the attack is given until then.  It also moves on
    //       when the server's longer wait after a heavy swing reaches the client (a prediction correction, a round
    //       trip after the swing): that is told apart by the client's clock, gpGlobals->curtime (the pointer that
    //       C_BaseCombatWeapon::ReloadOrSwitchWeapons reads at 0x10017922 before comparing it with +0x960): the
    //       weapon was not ready yet (seen in the game: a swing queued behind a heavy one was dropped without it).

    core::Hook<GetButtonBitsFn> g_get_button_bits;
    core::Hook<CalcViewModelViewFn> g_calc_viewmodel_view;
    inv::MeleeInput g_melee_input;
    std::uintptr_t g_c_melee_vtable = 0;
    std::uintptr_t g_c_globals = 0; // the client's CGlobalVars**
    ULONGLONG g_released_at = 0; // for the log: when the last swing was let go, and the weapon's wait then
    float g_released_next = 0.f;

    // A smooth clock in seconds (the input and the animation; GetTickCount64 steps by 15.6 ms).
    double seconds()
    {
        static LARGE_INTEGER frequency{};
        if (frequency.QuadPart == 0)
            QueryPerformanceFrequency(&frequency);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
    }

    // What the animation needs of the input (main thread: both run there).
    struct MeleeAnimation
    {
        float hold = 0.f; // the server's heavy_hold
        bool can_heavy = true;
        double let_go_at = -10.0; // a swing let go (seconds())
        bool let_go_heavy = false;
        float pose = 0.f; // the wind-up pose shown, eased: 0 .. 1
        double last = 0.0; // the last frame
    };
    MeleeAnimation g_anim;

    int melee_command(int bits)
    {
        float hold = 0.f, queue = 0.f;
        bool exhausted = false;
        {
            Session& s = local_session();
            std::lock_guard lock(s.mutex);
            hold = s.melee_hold;
            queue = s.melee_queue;
            exhausted = s.stamina_exhausted;
        }
        void* weapon = hold > 0.f ? client_active_weapon(client_local_player()) : nullptr;
        const auto w = reinterpret_cast<std::uintptr_t>(weapon);
        const bool melee = weapon != nullptr && mem::read_or<std::uintptr_t>(w) == g_c_melee_vtable;
        const float next = melee ? mem::read_or<float>(w + k_cw_next_primary) : 0.f;
        const float clock = mem::read_or<float>(mem::read_or<std::uintptr_t>(g_c_globals) + k_off_curtime);
        const ULONGLONG now = GetTickCount64();
        const double t = seconds();
        const inv::MeleeInput::State before = g_melee_input.state();
        const double since = g_melee_input.winding_since();
        const int out = g_melee_input.command(bits, t, melee, !exhausted, next, clock, hold, queue);
        const inv::MeleeInput::State after = g_melee_input.state();
        g_anim.hold = hold;
        g_anim.can_heavy = !exhausted;
        if (before != after || after == inv::MeleeInput::State::winding)
        {
            const bool winding = after == inv::MeleeInput::State::winding;
            const bool heavy_let_go = before == inv::MeleeInput::State::winding && after == inv::MeleeInput::State::releasing &&
                                      g_melee_input.heavy();
            Session& s = local_session();
            std::lock_guard lock(s.mutex);
            // (the HUD's clock is GetTickCount64)
            s.melee_winding_since =
                winding ? now - static_cast<ULONGLONG>(std::max(0.0, t - g_melee_input.winding_since()) * 1000.0) : 0;
            s.melee_can_heavy = !exhausted;
            if (heavy_let_go)
                s.melee_heavy_at = now;
        }
        if (before == inv::MeleeInput::State::winding && after == inv::MeleeInput::State::releasing)
        {
            INV_LOG("melee: let go after %.2f s: a %s swing", t - since, g_melee_input.heavy() ? "heavy" : "light");
            g_released_at = now;
            g_anim.let_go_at = t;
            g_anim.let_go_heavy = g_melee_input.heavy();
        }
        else if (before == inv::MeleeInput::State::releasing && after != inv::MeleeInput::State::releasing)
            INV_LOG("melee: %s %.2f s after it was let go", melee && next != g_released_next ? "it swung" : "dropped",
                    static_cast<double>(now - g_released_at) / 1000.0);
        if (after == inv::MeleeInput::State::releasing && before != inv::MeleeInput::State::releasing)
            g_released_next = next;
        return out;
    }

    int __fastcall hk_get_button_bits(void* input, void* edx, int reset)
    {
        const int bits = g_get_button_bits.original()(input, edx, reset);
        if (reset == 0)
            return g_melee_input.peek(bits);
        int out = bits;
        mem::guarded([&] { out = melee_command(bits); });
        return out;
    }

    // ---- the animation (client) ----------------------------------------------------------------------------------
    //
    // NMRiH2's wind-up has its own animations (Windup, Loop, Release, Recover sections per weapon class); L4D2's melee
    // viewmodels have only their swings.  So the wind-up is a pose laid over the viewmodel: while the attack is held
    // the weapon is drawn back, raised and turned to the right as the charge grows (eased: quick at first, slower
    // towards full), and trembles once a heavy swing is ready.  Let go, it snaps forward into the game's own swing
    // animation with a follow-through (a lunge forward and down, longer and deeper for a heavy swing).  A shove that
    // calls it off eases it back.
    //
    // C_TerrorViewModel::CalcViewModelView (slot 222 of its vtable 0x105C1054, 0x10288F40) places the viewmodel each
    // frame from the eye position and angles (copies of them get the bob and lag, then SetLocalOrigin /
    // SetLocalAngles); the hook gives it moved ones.  The camera itself is not moved.

    void angle_vectors(const Vec3& angles, Vec3& forward, Vec3& right, Vec3& up)
    {
        constexpr float k_rad = 3.14159265f / 180.f;
        const float sp = std::sin(angles.x * k_rad), cp = std::cos(angles.x * k_rad);
        const float sy = std::sin(angles.y * k_rad), cy = std::cos(angles.y * k_rad);
        const float sr = std::sin(angles.z * k_rad), cr = std::cos(angles.z * k_rad);
        forward = {cp * cy, cp * sy, -sp};
        right = {-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp};
        up = {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
    }

    void pose_viewmodel(Vec3& position, Vec3& angles)
    {
        const double now = seconds();
        const float dt = static_cast<float>(std::clamp(now - g_anim.last, 0.0, 0.1));
        g_anim.last = now;
        const inv::MeleeInput::State state = g_melee_input.state();
        float charge = 0.f, target = 0.f;
        if (state == inv::MeleeInput::State::winding && g_anim.hold > 0.f)
        {
            charge = std::clamp(static_cast<float>(now - g_melee_input.winding_since()) / g_anim.hold, 0.f, 1.f);
            target = 1.f - (1.f - charge) * (1.f - charge); // quick at first
            if (!g_anim.can_heavy)
                target *= 0.45f; // exhausted: no heavy swing, a tired half wind-up
        }
        // drawn back as the charge grows; let go it snaps forward, called off it eases back
        const float rate = target > g_anim.pose ? 14.f : state == inv::MeleeInput::State::cancelled ? 6.f : 28.f;
        g_anim.pose += (target - g_anim.pose) * std::min(1.f, dt * rate);
        const float pose = g_anim.pose;
        // the follow-through after letting go
        const float since_let_go = static_cast<float>(now - g_anim.let_go_at);
        const float length = g_anim.let_go_heavy ? 0.42f : 0.22f;
        const float through = since_let_go >= 0.f && since_let_go < length ? std::sin(3.14159265f * since_let_go / length) : 0.f;
        const float depth = g_anim.let_go_heavy ? 1.f : 0.35f;
        // a heavy swing ready: the arms tremble with the effort
        float tremble = 0.f;
        if (state == inv::MeleeInput::State::winding && charge >= 1.f && g_anim.can_heavy)
            tremble = std::sin(static_cast<float>(now) * 47.f) * 0.5f + std::sin(static_cast<float>(now) * 31.f) * 0.5f;
        if (pose < 0.001f && through == 0.f)
            return;
        Vec3 forward, right, up;
        angle_vectors(angles, forward, right, up);
        const float f = -2.8f * pose + 3.5f * through * depth;
        const float r = 2.6f * pose - 0.8f * through * depth;
        const float u = 1.6f * pose - 1.4f * through * depth + 0.08f * tremble;
        position.x += forward.x * f + right.x * r + up.x * u;
        position.y += forward.y * f + right.y * r + up.y * u;
        position.z += forward.z * f + right.z * r + up.z * u;
        angles.x += -11.f * pose + 9.f * through * depth + 0.35f * tremble; // pitch: up as it is drawn back
        angles.y += -9.f * pose + 4.f * through * depth + 0.25f * tremble; // yaw: turned to the right
        angles.z += 14.f * pose - 7.f * through * depth; // roll
    }

    void __fastcall hk_calc_viewmodel_view(void* viewmodel, void* edx, void* owner, const Vec3* eye_position, const Vec3* eye_angles)
    {
        if (eye_position != nullptr && eye_angles != nullptr)
        {
            Vec3 position = *eye_position, angles = *eye_angles;
            mem::guarded([&] { pose_viewmodel(position, angles); });
            g_calc_viewmodel_view.original()(viewmodel, edx, owner, &position, &angles);
            return;
        }
        g_calc_viewmodel_view.original()(viewmodel, edx, owner, eye_position, eye_angles);
    }

    void apply_melee_line(const char* text)
    {
        int hold = 0, queue = 0;
        std::sscanf(text + sizeof(k_melee_marker) - 1, "%d %d", &hold, &queue);
        Session& s = local_session();
        std::lock_guard lock(s.mutex);
        s.melee_hold = std::clamp(hold, 0, 10000) / 1000.f;
        s.melee_queue = std::clamp(queue, 0, 10000) / 1000.f;
    }

    void install_client_melee(const mem::ModuleInfo& client)
    {
        const std::uintptr_t bits = client.base + k_rva_c_get_button_bits;
        const std::uintptr_t vtable = client.base + k_rva_c_melee_vtable;
        const std::uintptr_t globals_read = client.base + k_rva_c_globals_read;
        // C_TerrorMeleeWeapon's vtable: its complete object locator (just before it) names the class
        const std::uintptr_t locator = mem::read_or<std::uintptr_t>(vtable - 4);
        const std::uintptr_t type = mem::read_or<std::uintptr_t>(locator + 12);
        char name[32] = {};
        mem::safe_copy(name, reinterpret_cast<const void*>(type + 8), sizeof(name) - 1);
        if (mem::read_or<std::uintptr_t>(locator + 4, 1) == 0 && std::strcmp(name, ".?AVC_TerrorMeleeWeapon@@") == 0 &&
            mem::find_pattern(globals_read, 17, "A1 ?? ?? ?? ?? F3 0F 10 40 0C 0F 2F 86 60 09 00 00") == globals_read &&
            mem::read_or<std::uintptr_t>(client.base + k_rva_c_input_vtable + 4u * k_c_vt_get_button_bits) == bits &&
            mem::find_pattern(bits, 11, "55 8B EC 83 EC 08 56 8B F1 8B 0D") == bits &&
            g_get_button_bits.create("GetButtonBits", reinterpret_cast<void*>(bits), hk_get_button_bits) && g_get_button_bits.enable())
        {
            g_c_melee_vtable = vtable;
            g_c_globals = mem::read_or<std::uintptr_t>(globals_read + 1);
        }
        else
        {
            INV_LOG("game: the client's GetButtonBits or melee weapon does not look as expected: no wind-up (every swing is light)");
            return;
        }
        const std::uintptr_t calc = client.base + k_rva_c_calc_viewmodel_view;
        if (mem::read_or<std::uintptr_t>(client.base + k_rva_c_viewmodel_vtable + 4u * k_c_vt_calc_viewmodel_view) == calc &&
            mem::find_pattern(calc, 19, "55 8B EC 83 EC 48 A1 ?? ?? ?? ?? 33 C5 89 45 FC 8B 45 10") == calc &&
            g_calc_viewmodel_view.create("CalcViewModelView", reinterpret_cast<void*>(calc), hk_calc_viewmodel_view) &&
            g_calc_viewmodel_view.enable())
            INV_LOG("game: the melee's wind-up animation ready");
        else
            INV_LOG("game: the viewmodel's CalcViewModelView does not look as expected: no wind-up animation");
    }

} // namespace game::detail
