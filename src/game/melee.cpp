// [melee], the server's half: No More Room in Hell 2's melee on the game's own swing.  Light and heavy swings, the
// wind-up (slower, tiring), the weapon classes, the damage (a full hit's share, the head, the cleave), wear and
// breaking, blocking and deflecting, the heavy shove, the throw, the hit's impact, a swing into a wall, the overhead
// heavy swing and the wind-up others see.  The client's half is melee_client.cpp.

#include "game/melee.hpp"

#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"
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
#include <map>
#include <string>
#include <vector>

namespace game::detail
{

    using namespace sdk;

    // ---- the swing -----------------------------------------------------------------------------------------------
    //
    // The game swings a melee weapon in CTerrorMeleeWeapon::PrimaryAttack (slot 282 of its vtable 0x106D611C,
    // 0x103EC3B0): it checks that the player can attack ("melee attack failed - !player->CanAttack()") and starts the
    // swing (0x103EC080), which sets m_flNextPrimaryAttack to curtime + the weapon's refire (and the secondary time:
    // no shove out of a swing).  It runs while the player's user command is processed, so the player's m_nButtons
    // are that command's: the mod's client marks a heavy swing there (inv::k_button_heavy).  The hook lets the game
    // swing, then, when the wait moved on (a swing started): the swing is heavy when the client says so, the server
    // saw it wind up (inv::k_button_winding in m_nButtons, sampled every GameFrame) for the weapon's heavy_hold less a
    // tick or two, and the survivor is not exhausted.  A heavy swing stretches both waits to heavy_recovery % and uses
    // heavy_stamina in all ([stamina]'s detection in GameFrame takes its `swing`; this adds the rest), each times the
    // weapon class's stamina %.  The shove is CTerrorMeleeWeapon slot 283 (0x103EB1F0), hooked the same way: the
    // client marks a heavy shove (inv::k_button_heavy_shove).
    //
    // The weapon's numbers are its profile (inv::melee_profile: [melee]'s, scaled by its class, overridden by
    // [melee.weapons.<name>]), by its script name, CTerrorMeleeWeapon::m_strMapSetScriptName (+0x17E0, a char*).

    constexpr float k_swing_hits_for = 1.5f; // a hit this long after its swing started is still that swing's
    constexpr float k_wind_slack = 0.15f; // GameFrame samples the wind-up bit once a tick (30 a second)
    constexpr int k_team_infected = 3;

    core::Hook<PrimaryAttackFn> g_primary_attack;
    core::Hook<PrimaryAttackFn> g_secondary_attack;
    core::Hook<TakeDamageFn> g_take_damage;

    struct Swinger
    {
        void* player = nullptr;
        int user_id = 0;
        float winding_since = -1.f; // game time the client's wind-up bit was first seen (-1: not winding)
        float winding_last = -1.f; // ... and last seen
        float wound = 0.f; // the last wind-up's length
        float wound_at = -1000.f; // when it ended
        void* weapon = nullptr; // the last swing's
        float swing_at = -1000.f;
        bool heavy = false;
        bool hit_anything = false; // the last swing hit a target
        bool bounced = false; // ... and went into a wall
        std::vector<void*> victims; // what the last swing hit, in order
        float block_since = -1.f; // blocking since (-1: not)
        float thrown_at = -1000.f;
        int sent_hold = -1, sent_shove = -1, sent_features = -1; // what the player's client was last told
        bool sent_mod = false; // ... while they ran the mod
        ULONGLONG checked_at = 0; // the last look at whether they run the mod
        float applied = 1.f; // the m_flLaggedMovementValue melee_frame set (without [stamina]; 1: left alone)
        bool seen = false;
    };
    std::vector<Swinger> g_swingers;
    float g_frame_time = 0.f;

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

    bool is_class(void* entity, const char* classname)
    {
        char name[32];
        return entity != nullptr && classname_of(entity, name, sizeof(name)) && std::strcmp(name, classname) == 0;
    }

    // The melee weapon in the player's hands (nullptr: none) and its script name.
    void* held_melee(void* player, std::string* name = nullptr)
    {
        void* weapon = entity_from_handle(handle_at(player, k_off_active_weapon));
        if (!is_class(weapon, "weapon_melee"))
            return nullptr;
        if (name != nullptr)
        {
            char text[64] = {};
            const char* script = mem::read_or<const char*>(reinterpret_cast<std::uintptr_t>(weapon) + k_off_melee_name);
            if (script != nullptr && mem::safe_copy(text, script, sizeof(text) - 1))
                *name = text;
        }
        return weapon;
    }

    inv::MeleeProfile profile_of(void* weapon)
    {
        char text[64] = {};
        const char* script = weapon != nullptr ? mem::read_or<const char*>(reinterpret_cast<std::uintptr_t>(weapon) + k_off_melee_name) : nullptr;
        if (script != nullptr)
            mem::safe_copy(text, script, sizeof(text) - 1);
        return inv::melee_profile(text, inv::melee_rules());
    }

    Vec3 origin_of(void* entity)
    {
        Vec3 v{};
        mem::read(reinterpret_cast<std::uintptr_t>(entity) + k_off_origin, v);
        return v;
    }

    // The yaw the player looks along (degrees), and their eyes.
    bool eyes_of(void* player, Vec3& position, Vec3& angles)
    {
        const auto eye_position = reinterpret_cast<EyePositionFn>(mem::vslot(player, k_vt_eye_position));
        const auto eye_angles = reinterpret_cast<EyeAnglesFn>(mem::vslot(player, k_vt_eye_angles));
        if (eye_position == nullptr || eye_angles == nullptr)
            return false;
        return mem::guarded([&] {
            eye_position(player, &position);
            angles = *eye_angles(player);
        });
    }

    Vec3 forward_of(const Vec3& angles)
    {
        constexpr float k_rad = 3.14159265f / 180.f;
        const float sp = std::sin(angles.x * k_rad), cp = std::cos(angles.x * k_rad);
        const float sy = std::sin(angles.y * k_rad), cy = std::cos(angles.y * k_rad);
        return {cp * cy, cp * sy, -sp};
    }

    // ---- what the game does for us (found by melee_install_extras; each optional) --------------------------------

    // (defined below, with what they call: found by install_melee_extras)
    void stagger_common(void* common, void* source, const Vec3& from);
    void break_melee_weapon(void* player, void* weapon);
    void raise_weapon_gesture(void* player);
    void install_melee_extras();

    StaggerPlayerFn g_stagger_player = nullptr; // a survivor or special staggers away from a point
    bool g_stagger_commons = false; // a common's INextBot OnShoved was found
    ViewPunchFn g_view_punch = nullptr;
    TraceLineFn g_trace_line = nullptr;
    core::Hook<WeaponSoundFn> g_weapon_sound;

    void stagger(void* victim, void* source, const Vec3& from)
    {
        if (victim == nullptr)
            return;
        if (is_class(victim, "player"))
        {
            if (g_stagger_player != nullptr)
                mem::guarded([&] { g_stagger_player(victim, nullptr, source, &from); });
        }
        else if (g_stagger_commons && source != nullptr && is_class(victim, "infected"))
            mem::guarded([&] { stagger_common(victim, source, from); });
    }

    // ---- telling the client ---------------------------------------------------------------------------------------

    // The server's [melee] to the player: the host's own session, or "@mel@ <hold ms> <shove hold ms> <features>" for
    // their client DLL (0 0 0: none), whenever it changes (the hold is the weapon in the hands') and once they are
    // known to run the mod.
    void tell_melee(void* player, Swinger& s, const inv::MeleeRules& rules)
    {
        const float weapon_hold = profile_of(held_melee(player)).heavy_hold;
        // (heavy off: no wind-up at all, the client gives the game its own buttons)
        const int hold = rules.enabled && rules.heavy ? static_cast<int>(std::lround(weapon_hold * 1000.f)) : 0;
        const int shove = rules.enabled && rules.heavy_shove ? static_cast<int>(std::lround(rules.heavy_shove_hold * 1000.f)) : 0;
        // the features the client takes part in: 1 the overhead swing, 2 the throw
        const int features = (rules.overhead ? 1 : 0) | (rules.throwing ? 2 : 0);
        const bool same = hold == s.sent_hold && shove == s.sent_shove && features == s.sent_features;
        if (player == host_player())
        {
            if (same)
                return;
            char line[64]; // the host's client half reads the same line
            std::snprintf(line, sizeof(line), "%s %d %d %d", k_melee_marker, hold, shove, features);
            apply_melee_line(line);
        }
        else
        {
            const ULONGLONG now = GetTickCount64();
            if (same && (s.sent_mod || now - s.checked_at < 1000))
                return;
            s.checked_at = now;
            ServerPlayer* sp = server_player(player);
            void* edict = edict_of(player);
            if (sp == nullptr || !sp->has_mod || g_client_printf == nullptr || edict == nullptr)
            {
                s.sent_mod = false;
                return;
            }
            char line[64];
            std::snprintf(line, sizeof(line), "%s %d %d %d\n", k_melee_marker, hold, shove, features);
            mem::guarded([&] { g_client_printf(g_engine_server, edict, line); });
            s.sent_mod = true;
        }
        s.sent_hold = hold;
        s.sent_shove = shove;
        s.sent_features = features;
    }

    // The impact of a hit, for the attacker: their viewmodel jolts (melee_client.cpp), their view kicks.
    void tell_hit(void* player, bool heavy)
    {
        const inv::MeleeRules& rules = inv::melee_rules();
        if (g_view_punch != nullptr && rules.view_kick && rules.view_punch > 0.f)
        {
            // the weapon stopped by what it hit: the view kicks down and a little aside
            const float kick = rules.view_punch * (heavy ? 1.f : 0.33f);
            const Vec3 punch{kick, (std::rand() % 2 == 0 ? -0.35f : 0.35f) * kick, 0.f};
            mem::guarded([&] { g_view_punch(player, nullptr, &punch); });
        }
        if (player == host_player())
        {
            melee_hit_feedback(heavy);
            return;
        }
        ServerPlayer* sp = server_player(player);
        void* edict = edict_of(player);
        if (sp != nullptr && sp->has_mod && g_client_printf != nullptr && edict != nullptr)
        {
            char line[32];
            std::snprintf(line, sizeof(line), "%s %d\n", k_melee_hit_marker, heavy ? 1 : 0);
            mem::guarded([&] { g_client_printf(g_engine_server, edict, line); });
        }
    }

    // ---- wear --------------------------------------------------------------------------------------------------------
    //
    // A melee weapon wears with each target it hits (a heavy hit 1.5, a swing into a wall 3, a thrown hit 2) and
    // breaks at its profile's durability (NMRiH2's DisposableWeaponComponent).  The wear of a weapon in the world is
    // kept here by its handle; a stored one's in the inventory (WeaponState::wear, through weapon_state /
    // apply_weapon_state / spawn_dropped).  A broken weapon leaves the player's hands with a sound and a notice.

    std::map<std::uint32_t, int> g_wear;
    struct Breaking
    {
        std::uint32_t player;
        std::uint32_t weapon;
    };
    std::vector<Breaking> g_breaking;

    int melee_wear(void* weapon)
    {
        const auto it = g_wear.find(handle_of(weapon));
        return it != g_wear.end() ? it->second : 0;
    }

    void set_melee_wear(void* weapon, int wear)
    {
        const std::uint32_t handle = handle_of(weapon);
        if (handle == k_invalid_handle)
            return;
        if (wear <= 0)
            g_wear.erase(handle);
        else
            g_wear[handle] = std::min(wear, inv::k_wear_broken);
    }

    void wear_out(void* player, void* weapon, float hits)
    {
        const inv::MeleeProfile profile = profile_of(weapon);
        if (!inv::melee_rules().breaking || profile.durability <= 0 || weapon == nullptr)
            return;
        const int before = melee_wear(weapon);
        const int after = inv::melee_wear_after(before, hits, profile.durability);
        set_melee_wear(weapon, after);
        if (before < inv::k_wear_broken * 8 / 10 && after >= inv::k_wear_broken * 8 / 10 && player == host_player())
            notify("Your weapon is about to break");
        if (after >= inv::k_wear_broken && player != nullptr)
            g_breaking.push_back({handle_of(player), handle_of(weapon)});
    }

    void break_weapons()
    {
        for (const Breaking& b : g_breaking)
        {
            void* player = entity_from_handle(b.player);
            void* weapon = entity_from_handle(b.weapon);
            if (player == nullptr || weapon == nullptr)
                continue;
            std::string name;
            if (held_melee(player, &name) != weapon && entity_from_handle(mem::read_or<std::uint32_t>(
                                                          reinterpret_cast<std::uintptr_t>(weapon) + k_off_weapon_owner, k_invalid_handle)) != player)
                continue; // no longer theirs
            INV_LOG("melee: userid %d's %s broke", user_id_of(player), name.c_str());
            set_melee_wear(weapon, 0);
            break_melee_weapon(player, weapon);
            if (player == host_player())
                notify("Your " + (name.empty() ? std::string("weapon") : name) + " broke");
            else if (Activation a(player); a.ok())
                notify("Your " + (name.empty() ? std::string("weapon") : name) + " broke");
        }
        g_breaking.clear();
    }

    // ---- the wind-up ---------------------------------------------------------------------------------------------
    //
    // Winding up slows the player (NMRiH2's wind-up is a slow walk): from 1 to wind_speed % over heavy_hold; blocking
    // too (block_speed %).  The speed scale is CBasePlayer::m_flLaggedMovementValue +0x20D0, the stamina's too (see
    // stamina.cpp: it slows the whole movement clock, so it is set on the ground only); with [stamina] its tick
    // multiplies this in, without it melee_frame sets it.  Let go, it is back at once.  Winding up also uses
    // wind_stamina a second (running out lets the swing go by itself: inv::MeleeInput), and others see the weapon
    // raised (a gesture, when the game's DoAnimationEvent was found).

    float melee_move_scale(void* player)
    {
        const inv::MeleeRules& rules = inv::melee_rules();
        const Swinger* s = rules.enabled ? swinger_of(player, false) : nullptr;
        if (s == nullptr)
            return 1.f;
        float scale = 1.f;
        if (s->winding_since >= 0.f && rules.wind_slowdown)
        {
            const float hold = std::max(0.1f, profile_of(held_melee(player)).heavy_hold);
            const float charge = std::clamp((game_time() - s->winding_since) / hold, 0.f, 1.f);
            scale = 1.f - (1.f - rules.wind_speed / 100.f) * charge;
        }
        if (s->block_since >= 0.f)
            scale = std::min(scale, rules.block_speed / 100.f);
        return scale;
    }

    void set_lagged_movement(void* player, float value)
    {
        if (mem::guarded([&] { *reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(player) + k_off_lagged_movement) = value; }))
            state_changed(player);
    }

    // ---- the throw ------------------------------------------------------------------------------------------------
    //
    // The reload key while winding up (inv::k_button_throw): the weapon leaves the hands (Weapon_Drop, slot 283 of
    // the player, with a velocity along the view) and flies; each frame its path since the last is checked against the
    // infected near it when it was thrown; the first one it passes within reach of takes throw_damage % of a full hit
    // and staggers, and the weapon wears.  It then lies where it falls, to be picked up again.

    constexpr float k_throw_reach = 36.f;
    constexpr float k_throw_flies_for = 1.6f;

    struct Thrown
    {
        std::uint32_t weapon;
        std::uint32_t thrower;
        float at;
        Vec3 last;
        std::vector<std::uint32_t> targets; // the infected near enough to be hit
    };
    std::vector<Thrown> g_thrown;

    // What a CTakeDamageInfo looks like: a copy of the first real melee hit seen (its damage type, ammo type and
    // the rest), for the hits the mod deals itself.
    alignas(16) unsigned char g_info_template[k_damage_info_size] = {};
    bool g_have_template = false;

    void deal_damage(void* victim, void* attacker, void* weapon, float damage, const Vec3& force)
    {
        alignas(16) unsigned char info[k_damage_info_size];
        std::memcpy(info, g_info_template, sizeof(info));
        if (!g_have_template)
            *reinterpret_cast<int*>(info + k_off_damage_type) = 0x80; // DMG_CLUB
        const auto put_handle = [&](std::uintptr_t at, void* entity) { *reinterpret_cast<std::uint32_t*>(info + at) = handle_of(entity); };
        put_handle(k_off_damage_inflictor, weapon);
        put_handle(k_off_damage_attacker, attacker);
        put_handle(k_off_damage_weapon, weapon);
        *reinterpret_cast<float*>(info + k_off_damage) = damage;
        *reinterpret_cast<Vec3*>(info + k_off_damage_force) = force;
        *reinterpret_cast<Vec3*>(info + k_off_damage_position) = origin_of(victim);
        mem::guarded([&] { g_take_damage.original()(victim, nullptr, info); });
    }

    // A common's full hit is its health when new (the game's swing kills it outright); anything else's the game's
    // own damage.
    float full_hit(void* victim, float game_damage)
    {
        if (!is_class(victim, "infected"))
            return game_damage;
        const int max = mem::read_or<int>(reinterpret_cast<std::uintptr_t>(victim) + k_off_max_health, 0);
        return max > 0 ? static_cast<float>(max) : 50.f; // (z_health)
    }

    void throw_weapon(void* player, Swinger& s, void* weapon)
    {
        const inv::MeleeRules& rules = inv::melee_rules();
        Vec3 eye{}, angles{};
        if (g.quiet_drop == nullptr || !eyes_of(player, eye, angles))
            return;
        const Vec3 f = forward_of(angles);
        const Vec3 velocity{f.x * rules.throw_speed, f.y * rules.throw_speed, f.z * rules.throw_speed + 120.f};
        const Vec3 target{eye.x + f.x * 200.f, eye.y + f.y * 200.f, eye.z + f.z * 200.f};
        if (!mem::guarded([&] { g.quiet_drop(player, weapon, &target, &velocity); }))
            return;
        // Weapon_Drop keeps its own throw (a short toss at the feet: the velocity given is not used, seen in the
        // game; with the Teleport it flew about 880 units a second); the entity's Teleport (slot 118, 0x10031090 -> CBaseEntity::Teleport 0x10066940) moves its physics
        // object too, so the weapon starts a little in front of the eyes with the throw's velocity
        const Vec3 start{eye.x + f.x * 24.f, eye.y + f.y * 24.f, eye.z + f.z * 24.f - 4.f};
        if (const auto teleport = reinterpret_cast<TeleportFn>(mem::vslot(weapon, k_vt_teleport)))
            mem::guarded([&] { teleport(weapon, &start, &angles, &velocity); });
        s.thrown_at = game_time();
        Thrown t{handle_of(weapon), handle_of(player), game_time(), origin_of(weapon), {}};
        for (int index = 1; index < 2048; ++index)
        {
            void* e = entity_at_index(index);
            if (e == nullptr || e == player ||
                !(is_class(e, "infected") || is_class(e, "witch") || (team_of(e) == k_team_infected && is_class(e, "player"))))
                continue;
            const Vec3 o = origin_of(e);
            const float dx = o.x - eye.x, dy = o.y - eye.y;
            if (dx * dx + dy * dy < 1500.f * 1500.f)
                t.targets.push_back(handle_of(e));
        }
        INV_LOG("melee: userid %d threw their weapon (%d infected near)", user_id_of(player), static_cast<int>(t.targets.size()));
        g_thrown.push_back(std::move(t));
    }

    // the distance from p to the segment a..b
    float segment_distance(const Vec3& p, const Vec3& a, const Vec3& b)
    {
        const Vec3 ab{b.x - a.x, b.y - a.y, b.z - a.z}, ap{p.x - a.x, p.y - a.y, p.z - a.z};
        const float len = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
        const float t = len > 0.f ? std::clamp((ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / len, 0.f, 1.f) : 0.f;
        const Vec3 d{ap.x - ab.x * t, ap.y - ab.y * t, ap.z - ab.z * t};
        return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    }

    void fly_thrown()
    {
        const inv::MeleeRules& rules = inv::melee_rules();
        const float now = game_time();
        std::erase_if(g_thrown, [&](Thrown& t) {
            void* weapon = entity_from_handle(t.weapon);
            if (weapon == nullptr || now - t.at > k_throw_flies_for)
                return true;
            // picked up again: done
            if (entity_from_handle(mem::read_or<std::uint32_t>(reinterpret_cast<std::uintptr_t>(weapon) + k_off_weapon_owner, k_invalid_handle)) != nullptr)
                return true;
            const Vec3 now_at = origin_of(weapon);
            const Vec3 from = t.last;
            t.last = now_at;
            for (const std::uint32_t h : t.targets)
            {
                void* victim = entity_from_handle(h);
                if (victim == nullptr || !alive(victim))
                    continue;
                Vec3 centre = origin_of(victim);
                centre.z += 40.f; // their chest (the origin is at the feet)
                if (segment_distance(centre, from, now_at) > k_throw_reach)
                    continue;
                void* thrower = entity_from_handle(t.thrower);
                const float damage = full_hit(victim, 100.f) * rules.throw_damage / 100.f;
                const Vec3 force{(now_at.x - from.x) * 300.f, (now_at.y - from.y) * 300.f, 0.f};
                INV_LOG("melee: a thrown weapon hit (%.0f damage)", damage);
                deal_damage(victim, thrower, weapon, damage, force);
                stagger(victim, thrower, from);
                wear_out(nullptr, weapon, 2.f);
                if (thrower != nullptr)
                    tell_hit(thrower, true);
                return true; // it hit one: it falls
            }
            return false;
        });
    }

    // ---- blocking ---------------------------------------------------------------------------------------------------
    //
    // The reload key held with a melee weapon in the hands (a melee weapon has no reload; NMRiH2's Block ability): a
    // common's hit from the front (within 60 degrees of where the survivor looks) takes block % and uses block_stamina;
    // raised within `deflect` seconds before the hit (NMRiH2's MeleeDeflect) it takes nothing and the common
    // staggers.  An exhausted survivor cannot block.  Specials and the witch are not blocked: they grab, pounce and
    // tear.

    bool in_front(void* player, void* other)
    {
        Vec3 eye{}, angles{};
        if (!eyes_of(player, eye, angles))
            return false;
        const Vec3 o = origin_of(other);
        const float dx = o.x - eye.x, dy = o.y - eye.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.f)
            return true;
        const Vec3 f = forward_of({0.f, angles.y, 0.f});
        return (dx * f.x + dy * f.y) / len > 0.5f; // cos 60
    }

    // A survivor's block: the damage they take (`damage` as it was, or less).
    float blocked(void* victim, void* attacker, float damage)
    {
        const inv::MeleeRules& rules = inv::melee_rules();
        Swinger* s = swinger_of(victim, false);
        if (s == nullptr || s->block_since < 0.f || damage <= 0.f || !is_class(attacker, "infected") || stamina_exhausted(victim) ||
            !in_front(victim, attacker))
            return damage;
        const float raised = game_time() - s->block_since;
        const inv::MeleeProfile profile = profile_of(held_melee(victim));
        if (rules.deflecting && raised <= rules.deflect)
        {
            INV_LOG("melee: userid %d deflected a hit (%.0f)", s->user_id, damage);
            stagger(attacker, victim, origin_of(victim));
            stamina_spend(victim, rules.block_stamina * 0.5f * profile.stamina / 100.f, "a deflect");
            return 0.f;
        }
        stamina_spend(victim, rules.block_stamina * profile.stamina / 100.f, "a block");
        return damage * rules.block / 100.f;
    }

    // ---- the frame ------------------------------------------------------------------------------------------------

    void melee_frame()
    {
        if (g_globals == nullptr || !g_primary_attack.installed())
            return;
        const inv::MeleeRules& rules = inv::melee_rules();
        const float now = game_time();
        float dt = now - g_frame_time;
        g_frame_time = now;
        if (dt < 0.f || dt > 0.25f)
            dt = 0.f;
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
            s.user_id = user_id_of(player);
            const int buttons = mem::read_or<int>(reinterpret_cast<std::uintptr_t>(player) + k_off_buttons);
            void* weapon = held_melee(player);
            const bool winding = rules.enabled && weapon != nullptr && (buttons & inv::k_button_winding) != 0;
            if (winding)
            {
                if (s.winding_since < 0.f)
                {
                    s.winding_since = now;
                    if (rules.gesture)
                        raise_weapon_gesture(player);
                }
                s.winding_last = now;
                if (inv::stamina_rules().enabled && rules.wind_stamina > 0.f)
                    stamina_spend(player, rules.wind_stamina * profile_of(weapon).stamina / 100.f * dt, "winding up");
            }
            else if (s.winding_since >= 0.f)
            {
                s.wound = s.winding_last - s.winding_since;
                s.wound_at = now;
                s.winding_since = -1.f;
            }
            // blocking: the reload key with a melee weapon, not winding up, not exhausted
            const bool blocking = rules.enabled && rules.blocking && weapon != nullptr && (buttons & inv::k_button_reload) != 0 && !winding &&
                                  !stamina_exhausted(player);
            if (blocking && s.block_since < 0.f)
                s.block_since = now;
            else if (!blocking)
                s.block_since = -1.f;
            // the throw
            if (rules.enabled && rules.throwing && weapon != nullptr && (buttons & inv::k_button_throw) != 0 && now - s.thrown_at > 0.5f)
                throw_weapon(player, s, weapon);
            if (!is_bot(player))
                tell_melee(player, s, rules);
            // the wind-up's and the block's slowdown, when the stamina does not apply it
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
        fly_thrown();
        break_weapons();
        // weapons gone from the world forget their wear
        static float s_swept = 0.f;
        if (now - s_swept > 10.f)
        {
            s_swept = now;
            std::erase_if(g_wear, [](const auto& entry) { return entity_from_handle(entry.first) == nullptr; });
        }
    }

    // ---- the swing's start, the shove ------------------------------------------------------------------------------

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
        const inv::MeleeProfile profile = profile_of(weapon);
        const int buttons = mem::read_or<int>(reinterpret_cast<std::uintptr_t>(player) + k_off_buttons);
        const float wound = s->winding_since >= 0.f ? s->winding_last - s->winding_since : now - s->wound_at < 1.f ? s->wound : 0.f;
        const bool claimed = (buttons & inv::k_button_heavy) != 0;
        const bool exhausted = stamina_exhausted(player);
        s->weapon = weapon;
        s->swing_at = now;
        s->heavy = rules.heavy && claimed && !exhausted && wound + k_wind_slack >= profile.heavy_hold;
        s->hit_anything = false;
        s->bounced = false;
        s->victims.clear();
        const float swing_cost = inv::stamina_rules().swing;
        if (claimed && !s->heavy)
            INV_LOG("melee: userid %d: a heavy swing refused (wound up %.2f s of %.2f%s)", user_id_of(player), wound,
                    profile.heavy_hold, exhausted ? ", exhausted" : "");
        if (!s->heavy)
        {
            // the class's share of a swing's cost ([stamina] took its `swing`)
            if (inv::stamina_rules().enabled && profile.stamina != 100.f)
                stamina_spend(player, swing_cost * (profile.stamina - 100.f) / 100.f, "a swing");
            return;
        }
        const float stretch = rules.heavy_recovery / 100.f;
        const float secondary = mem::read_or<float>(w + k_off_next_secondary);
        if (mem::guarded([&] {
                *reinterpret_cast<float*>(w + k_off_next_primary) = now + (primary - now) * stretch;
                if (secondary > now)
                    *reinterpret_cast<float*>(w + k_off_next_secondary) = now + (secondary - now) * stretch;
            }))
            state_changed(weapon);
        if (inv::stamina_rules().enabled)
            stamina_spend(player, rules.heavy_stamina * profile.stamina / 100.f - swing_cost, "a heavy swing");
        INV_LOG("melee: userid %d: a heavy swing (wound up %.2f s; the wait %.2f s)", user_id_of(player), wound,
                (primary - now) * stretch);
    }

    void __fastcall hk_primary_attack(void* weapon, void* edx)
    {
        const inv::MeleeRules& rules = inv::melee_rules();
        const float before = mem::read_or<float>(reinterpret_cast<std::uintptr_t>(weapon) + k_off_next_primary);
        g_primary_attack.original()(weapon, edx);
        if (rules.enabled && g_globals != nullptr)
            mem::guarded([&] { swing_started(weapon, before); });
    }

    // The heavy shove: held long enough (the client's inv::k_button_heavy_shove), it costs heavy_shove_stamina,
    // waits longer, and knocks down every infected in front within reach (staggered from the shover).
    void shove_started(void* weapon, float before)
    {
        const inv::MeleeRules& rules = inv::melee_rules();
        const auto w = reinterpret_cast<std::uintptr_t>(weapon);
        const float now = game_time();
        const float secondary = mem::read_or<float>(w + k_off_next_secondary);
        if (!(secondary > before + 0.05f && secondary > now))
            return;
        void* player = entity_from_handle(mem::read_or<std::uint32_t>(w + k_off_weapon_owner, k_invalid_handle));
        if (player == nullptr)
            return;
        const int buttons = mem::read_or<int>(reinterpret_cast<std::uintptr_t>(player) + k_off_buttons);
        if (!rules.heavy_shove || (buttons & inv::k_button_heavy_shove) == 0 || stamina_exhausted(player))
            return;
        if (mem::guarded([&] { *reinterpret_cast<float*>(w + k_off_next_secondary) = now + (secondary - now) * 1.5f; }))
            state_changed(weapon);
        if (inv::stamina_rules().enabled)
            stamina_spend(player, rules.heavy_shove_stamina * profile_of(weapon).stamina / 100.f - inv::stamina_rules().shove,
                          "a heavy shove");
        Vec3 eye{}, angles{};
        if (!eyes_of(player, eye, angles))
            return;
        int knocked = 0;
        for (int index = 1; index < 2048; ++index)
        {
            void* e = entity_at_index(index);
            if (e == nullptr || e == player || team_of(e) != k_team_infected || !alive(e) || !(is_class(e, "infected") || is_class(e, "player")))
                continue;
            const Vec3 o = origin_of(e);
            const float dx = o.x - eye.x, dy = o.y - eye.y;
            if (dx * dx + dy * dy > 120.f * 120.f || !in_front(player, e))
                continue;
            stagger(e, player, eye);
            ++knocked;
        }
        INV_LOG("melee: userid %d: a heavy shove (%d knocked back)", user_id_of(player), knocked);
    }

    void __fastcall hk_secondary_attack(void* weapon, void* edx)
    {
        const float before = mem::read_or<float>(reinterpret_cast<std::uintptr_t>(weapon) + k_off_next_secondary);
        g_secondary_attack.original()(weapon, edx);
        if (inv::melee_rules().enabled && g_globals != nullptr)
            mem::guarded([&] { shove_started(weapon, before); });
    }

    // ---- the damage -------------------------------------------------------------------------------------------------
    //
    // server.dll 0x10055C70 CBaseEntity::TakeDamage(const CTakeDamageInfo&) (thiscall, ret 4; it warns
    // "CBaseEntity::TakeDamage:  with inputInfo.GetDamageForce() == vec3_origin").  CTakeDamageInfo is 0x60 bytes
    // (its copy constructor 0x10010140): m_vecDamageForce +0, m_vecDamagePosition +0xC, m_hInflictor +0x30,
    // m_hAttacker +0x34 (what its GetAttacker, 0x1003D4B0, reads), m_hWeapon +0x38, m_flDamage +0x3C,
    // m_bitsDamageType +0x48.
    //
    // A hit of the attacker's melee weapon (as the weapon or the inflictor) within k_swing_hits_for of a swing's start
    // belongs to that swing.  The game's swing kills a common outright: its damage is the common's health left (seen:
    // 50 on a new one, 16 on one hit before).  So the damage is a full hit's share instead (full_hit: a common's
    // health when new, m_iMaxHealth; anything else the game's own damage): light_damage % or heavy_damage % of the
    // weapon's profile, times the cleave for each target after the first, times head_damage % on a hit to the head
    // (an infected's CBaseCombatCharacter::m_LastHitGroup +0x1838 is HITGROUP_HEAD 1: the swing's trace attack sets it
    // before the damage), times stamina_damage_scale (an exhausted survivor).  With head_only the body alone leaves a
    // common at 1 health.  The game deals a swing's hits over the frames of the swing, each target once (seen: a heavy
    // swing's hit 0.2 - 0.3 s after it started; one light swing into a crowd hit four commons within 0.1 s); each
    // target hit also gets a few follow-up calls of 0 damage (-1 for one dying): those are left alone.  A teammate hit
    // takes the game's own friendly fire.  Each target hit uses hit_stamina, wears the weapon, kicks the attacker's
    // view (the first of a swing) and, for a heavy hit, staggers the target.  A survivor hit by a common may block it.

    // The damage to deal (`damage` when it is not the mod's business).
    float melee_damage(void* victim, const void* info, float damage)
    {
        const auto i = reinterpret_cast<std::uintptr_t>(info);
        void* attacker = entity_from_handle(mem::read_or<std::uint32_t>(i + k_off_damage_attacker, k_invalid_handle));
        if (attacker == nullptr || attacker == victim)
            return damage;
        const inv::MeleeRules& rules = inv::melee_rules();
        // a survivor's block
        if (rules.enabled && team_of(victim) == k_team_survivors && team_of(attacker) == k_team_infected)
            return blocked(victim, attacker, damage);
        void* active = entity_from_handle(handle_at(attacker, k_off_active_weapon));
        if (active == nullptr ||
            (entity_from_handle(mem::read_or<std::uint32_t>(i + k_off_damage_weapon, k_invalid_handle)) != active &&
             entity_from_handle(mem::read_or<std::uint32_t>(i + k_off_damage_inflictor, k_invalid_handle)) != active))
            return damage;
        const float exhausted = stamina_damage_scale(attacker, active); // checks that it is a melee weapon itself
        Swinger* s = rules.enabled ? swinger_of(attacker, false) : nullptr;
        if (s == nullptr || s->weapon != active || game_time() - s->swing_at > k_swing_hits_for)
            return damage * exhausted; // not a melee swing the hook saw
        if (team_of(victim) == k_team_survivors)
            return damage * exhausted; // a teammate: the game's own friendly fire, not counted in the cleave
        if (damage <= 0.f)
            return damage; // the game's follow-ups to a target already hit (0, and -1 for one dying): left as they are
        if (!g_have_template)
        {
            std::memcpy(g_info_template, info, sizeof(g_info_template));
            g_have_template = true;
        }
        const inv::MeleeProfile profile = profile_of(active);
        const auto it = std::find(s->victims.begin(), s->victims.end(), victim);
        const int earlier = static_cast<int>(it - s->victims.begin());
        const bool first_hit = it == s->victims.end();
        if (first_hit)
            s->victims.push_back(victim);
        const auto v = reinterpret_cast<std::uintptr_t>(victim);
        const bool common = is_class(victim, "infected");
        const bool head = team_of(victim) == k_team_infected && mem::read_or<int>(v + k_off_last_hit_group) == k_hitgroup_head;
        float dealt = full_hit(victim, damage) * (s->heavy ? profile.heavy_damage : profile.light_damage) / 100.f *
                      inv::cleave_scale(earlier, profile.cleave, profile.cleave_min) * exhausted;
        if (head)
            dealt *= rules.head_damage / 100.f;
        const int health = mem::read_or<int>(v + k_off_health);
        if (common && rules.head_only && !head && health > 0)
            dealt = std::min(dealt, static_cast<float>(health - 1)); // only the head kills
        if (first_hit)
        {
            s->hit_anything = true;
            if (inv::stamina_rules().enabled && rules.hit_stamina > 0.f)
                stamina_spend(attacker, rules.hit_stamina * profile.stamina / 100.f, "a hit");
            wear_out(attacker, active, s->heavy ? 1.5f : 1.f);
            if (earlier == 0 && !is_bot(attacker))
                tell_hit(attacker, s->heavy);
            if (s->heavy && rules.hit_stagger && dealt < static_cast<float>(health))
                stagger(victim, attacker, origin_of(attacker)); // a heavy hit knocks it back
        }
        if (!is_bot(attacker))
            INV_LOG("melee: userid %d hit target %d of the swing%s%s: %.0f of %d health (the game's %.0f)", user_id_of(attacker),
                    earlier + 1, s->heavy ? " (heavy)" : "", head ? " (head)" : "", dealt, health, damage);
        return dealt;
    }

    int __fastcall hk_take_damage(void* self, void* edx, const void* info)
    {
        if (info != nullptr && (!g_swingers.empty() || inv::stamina_rules().enabled))
        {
            const float damage = mem::read_or<float>(reinterpret_cast<std::uintptr_t>(info) + k_off_damage);
            float dealt = damage;
            mem::guarded([&] { dealt = melee_damage(self, info, damage); });
            if (dealt != damage)
            {
                alignas(16) unsigned char copy[k_damage_info_size];
                std::memcpy(copy, info, sizeof(copy));
                *reinterpret_cast<float*>(copy + k_off_damage) = dealt;
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
        {
            INV_LOG("game: the melee weapon's PrimaryAttack does not look as expected: no [melee]");
            return;
        }
        const std::uintptr_t secondary = g.server.base + k_rva_melee_secondary_attack;
        if (mem::read_or<std::uintptr_t>(g.server.base + k_rva_melee_vtable + 4u * k_vt_secondary_attack) == secondary &&
            mem::find_pattern(secondary, k_melee_secondary_prologue_length, k_melee_secondary_prologue) == secondary &&
            g_secondary_attack.create("MeleeSecondaryAttack", reinterpret_cast<void*>(secondary), hk_secondary_attack) &&
            g_secondary_attack.enable())
            INV_LOG("game: the heavy shove ready");
        else
            INV_LOG("game: the melee weapon's SecondaryAttack does not look as expected: no heavy shove");
        install_melee_extras();
    }

    // ---- what the extras call -----------------------------------------------------------------------------------
    //
    // A common stumbles away from its pusher (`source`'s centre) the way a shove makes it: its INextBot OnShoved,
    // with the push-only flag up so a push from behind does not kill it (see k_off_infected_push_only).
    void stagger_common(void* common, void* source, const Vec3&)
    {
        const auto my_infected = reinterpret_cast<MyInfectedFn>(mem::vslot(common, k_vt_my_infected));
        void* infected = my_infected != nullptr ? my_infected(common) : nullptr;
        if (infected == nullptr)
            return;
        const auto i = reinterpret_cast<std::uintptr_t>(infected);
        void* nextbot = reinterpret_cast<void*>(i + k_off_infected_nextbot);
        if (mem::read_or<std::uintptr_t>(i + k_off_infected_nextbot) != g.server.base + k_rva_infected_nextbot_vtable)
            return;
        const auto on_shoved = reinterpret_cast<OnShovedFn>(mem::vslot(nextbot, k_vt_nextbot_on_shoved));
        auto& push_only = *reinterpret_cast<std::uint8_t*>(i + k_off_infected_push_only);
        const std::uint8_t was = push_only;
        push_only = 1;
        on_shoved(nextbot, source);
        push_only = was;
    }

    // Is there a wall right in front of the player (the world, within reach, standing up: not the floor they aim
    // at)?  A world-only trace along their view, level.
    bool wall_ahead(void* player)
    {
        Vec3 eye{}, angles{};
        if (g_trace_line == nullptr || !eyes_of(player, eye, angles))
            return true; // (no trace: trust the game's sound)
        const Vec3 f = forward_of({std::clamp(angles.x, -20.f, 20.f), angles.y, 0.f});
        const Vec3 end{eye.x + f.x * 72.f, eye.y + f.y * 72.f, eye.z + f.z * 72.f};
        alignas(16) unsigned char trace[k_trace_size] = {};
        const std::uintptr_t filter = g.server.base + k_rva_trace_filter_world_only; // a bare object: just its vtable
        const std::uintptr_t* filter_object = &filter;
        if (!mem::guarded([&] { g_trace_line(&eye, &end, k_mask_solid, filter_object, trace); }))
            return false;
        const float fraction = *reinterpret_cast<float*>(trace + k_off_trace_fraction);
        const Vec3 normal = *reinterpret_cast<Vec3*>(trace + k_off_trace_normal);
        return fraction < 1.f && trace[k_off_trace_start_solid] == 0 && std::fabs(normal.z) < 0.6f;
    }

    // A swing into a wall (NMRiH's bounce): the weapon's trace hit something that is not a character, before any
    // target, and there is a wall right ahead (not the floor, not a prop: the game plays the same sound for those):
    // the view kicks and the weapon wears 3 hits.
    void __fastcall hk_weapon_sound(void* weapon, void* edx, int type, float soundtime)
    {
        g_weapon_sound.original()(weapon, edx, type, soundtime);
        if (type != k_sound_melee_hit_world || mem::read_or<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(weapon)) !=
                                                    g.server.base + k_rva_melee_vtable)
            return;
        const inv::MeleeRules& rules = inv::melee_rules();
        if (!rules.enabled || !rules.wall_bounce)
            return;
        mem::guarded([&] {
            void* player = entity_from_handle(mem::read_or<std::uint32_t>(reinterpret_cast<std::uintptr_t>(weapon) + k_off_weapon_owner,
                                                                           k_invalid_handle));
            Swinger* s = swinger_of(player, false);
            if (s == nullptr || s->weapon != weapon || s->bounced || s->hit_anything || game_time() - s->swing_at > k_swing_hits_for ||
                !wall_ahead(player))
                return;
            s->bounced = true;
            wear_out(player, weapon, 3.f);
            if (!is_bot(player))
                tell_hit(player, true);
            INV_LOG("melee: userid %d's swing went into a wall", s->user_id);
        });
    }

    // A broken weapon leaves the hands: dropped (the game switches to the next weapon) and removed.
    void break_melee_weapon(void* player, void* weapon)
    {
        if (g.quiet_drop != nullptr && g.util_remove != nullptr)
            mem::guarded([&] {
                g.quiet_drop(player, weapon, nullptr, nullptr);
                g.util_remove(weapon);
            });
    }

    // ---- the overhead heavy swing, the gesture ----------------------------------------------------------------------
    //
    // A heavy swing comes down from above when the weapon has such a swing among its own (inv::melee_overhead_swing):
    // the hook on the game's choice (0x103EAF30) sets the next index to it first (and the last attack's time to now,
    // so melee_combo_reset_time does not put it back to 0).  The client's prediction makes the same choice (its own
    // hook, melee_client.cpp), from the same buttons.  The light swings go on from there.

    void* g_melee_info_db = nullptr;
    MeleeInfoLookupFn g_melee_info_lookup = nullptr;
    core::Hook<ChooseAttackFn> g_choose_attack;
    DoAnimationEventFn g_do_animation_event = nullptr;

    // The overhead swing's index in `info`, or -1.
    int overhead_index(const void* info)
    {
        const auto i = reinterpret_cast<std::uintptr_t>(info);
        const int count = std::min(mem::read_or<int>(i + k_off_info_attack_count), 8);
        const auto attacks = mem::read_or<std::uintptr_t>(i + k_off_info_attacks);
        int dirs[8] = {};
        for (int n = 0; n < count; ++n)
            dirs[n] = mem::read_or<int>(attacks + n * k_attack_size + k_off_attack_start_dir, -1);
        return count > 0 && attacks != 0 ? inv::melee_overhead_swing(dirs, count) : -1;
    }

    int __fastcall hk_choose_attack(void* weapon, void* edx, void* info)
    {
        mem::guarded([&] {
            const inv::MeleeRules& rules = inv::melee_rules();
            if (!rules.enabled || !rules.overhead)
                return;
            const auto w = reinterpret_cast<std::uintptr_t>(weapon);
            void* player = entity_from_handle(mem::read_or<std::uint32_t>(w + k_off_weapon_owner, k_invalid_handle));
            if (player == nullptr || (mem::read_or<int>(reinterpret_cast<std::uintptr_t>(player) + k_off_buttons) & inv::k_button_heavy) == 0)
                return;
            if (const int index = overhead_index(info); index >= 0)
            {
                *reinterpret_cast<int*>(w + k_off_melee_next_attack) = index;
                *reinterpret_cast<float*>(w + k_off_melee_last_attack) = game_time();
            }
        });
        return g_choose_attack.original()(weapon, edx, info);
    }

    // Winding up: the others see the survivor raise the weapon (its next swing's idle activity, as a gesture).
    void raise_weapon_gesture(void* player)
    {
        void* weapon = held_melee(player);
        if (weapon == nullptr || g_do_animation_event == nullptr || g_melee_info_lookup == nullptr)
            return;
        const auto w = reinterpret_cast<std::uintptr_t>(weapon);
        const auto info = reinterpret_cast<std::uintptr_t>(
            g_melee_info_lookup(g_melee_info_db, mem::read_or<std::uint32_t>(w + k_off_melee_info_handle, k_invalid_handle)));
        const int count = info != 0 ? mem::read_or<int>(info + k_off_info_attack_count) : 0;
        const auto attacks = info != 0 ? mem::read_or<std::uintptr_t>(info + k_off_info_attacks) : 0;
        if (count <= 0 || attacks == 0)
            return;
        int index = mem::read_or<int>(w + k_off_melee_next_attack);
        if (index < 0 || index >= count)
            index = 0;
        const int activity = mem::read_or<int>(attacks + index * k_attack_size + k_off_attack_idle_activity, -1);
        if (activity > 0)
            g_do_animation_event(player, k_anim_event_gesture, activity);
    }

    void install_melee_extras()
    {
        const std::uintptr_t base = g.server.base;
        // staggering a survivor or a special
        const std::uintptr_t stagger_at = base + k_rva_stagger_player;
        if (mem::find_pattern(stagger_at, 36, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 83 EC 48 56 57 8B F1 E8 ?? ?? ?? ?? 84 C0") ==
            stagger_at)
            g_stagger_player = reinterpret_cast<StaggerPlayerFn>(stagger_at);
        else
            INV_LOG("game: OnStaggered does not look as expected: no stagger of a survivor or special");
        // a common's stumble
        const std::uintptr_t on_shoved = mem::read_or<std::uintptr_t>(base + k_rva_infected_nextbot_vtable + 4u * k_vt_nextbot_on_shoved);
        g_stagger_commons = on_shoved != 0 &&
                            mem::find_pattern(on_shoved, 29, "55 8B EC 56 57 8B F9 8B 07 8B 50 04 FF D2 8B F0 85 F6 74 1F 53 8B 5D 08 8B 06 8B 50 70") ==
                                on_shoved;
        if (!g_stagger_commons)
            INV_LOG("game: the common's OnShoved does not look as expected: commons are not staggered");
        // the view's kick
        const std::uintptr_t punch = base + k_rva_view_punch;
        if (mem::find_pattern(punch, 20, "55 8B EC A1 ?? ?? ?? ?? 83 EC 0C 83 78 30 00 56 8B F1 0F 85") == punch)
            g_view_punch = reinterpret_cast<ViewPunchFn>(punch);
        else
            INV_LOG("game: ViewPunch does not look as expected: no view kick on a hit");
        // a swing into a wall
        const std::uintptr_t trace = base + k_rva_trace_line;
        const std::uintptr_t filter_vtable = base + k_rva_trace_filter_world_only;
        if (mem::find_pattern(trace, 32, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 83 EC 5C 56 8B 43 08 8B 4B 0C") == trace &&
            mem::find_pattern(mem::read_or<std::uintptr_t>(filter_vtable), 5, "32 C0 C2 08 00") != 0 &&
            mem::find_pattern(mem::read_or<std::uintptr_t>(filter_vtable + 4), 6, "B8 01 00 00 00 C3") != 0)
            g_trace_line = reinterpret_cast<TraceLineFn>(trace);
        else
            INV_LOG("game: UTIL_TraceLine does not look as expected: every swing into the world bounces");
        const std::uintptr_t sound = base + k_rva_weapon_sound;
        if (mem::read_or<std::uintptr_t>(base + k_rva_melee_vtable + 4u * k_vt_weapon_sound) == sound &&
            mem::find_pattern(sound, 34, "55 8B EC 81 EC CC 00 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 53 8B 5D 08 56 8B F1 8B 06 8B 90 3C 05 00 00") == sound &&
            g_weapon_sound.create("WeaponSound", reinterpret_cast<void*>(sound), hk_weapon_sound) && g_weapon_sound.enable())
            ;
        else
            INV_LOG("game: WeaponSound does not look as expected: a swing into a wall does not bounce");
        // the overhead swing: the game's choice, and the info database (from the swing start's own lookup)
        const std::uintptr_t start = base + k_rva_melee_swing_start;
        const std::uintptr_t choose = base + k_rva_choose_attack;
        if (mem::find_pattern(start, 20, "55 8B EC 53 56 8B F1 8B 86 DC 17 00 00 50 B9 ?? ?? ?? ?? E8") == start)
        {
            g_melee_info_db = mem::read_or<void*>(start + 15);
            g_melee_info_lookup = reinterpret_cast<MeleeInfoLookupFn>(start + 24 + mem::read_or<std::int32_t>(start + 20));
        }
        if (mem::find_pattern(choose, 19, "55 8B EC 53 8B 5D 08 56 57 8B BB 2C 0C 00 00 8B F1 85 FF") != choose ||
            !g_choose_attack.create("ChooseMeleeAttack", reinterpret_cast<void*>(choose), hk_choose_attack) || !g_choose_attack.enable())
            INV_LOG("game: the melee weapon's swing choice does not look as expected: no overhead heavy swing");
        // the gesture
        const std::uintptr_t anim = base + k_rva_do_animation_event;
        if (mem::read_or<std::uintptr_t>(g.player_vtable + 4u * k_vt_do_animation_event) == anim &&
            mem::find_pattern(anim, 20, "55 8B EC 56 8B 75 08 57 8B F9 83 FE 63 74 29 8B 8F 1C 29 00") == anim && g_melee_info_lookup != nullptr)
            g_do_animation_event = reinterpret_cast<DoAnimationEventFn>(anim);
        else
            INV_LOG("game: DoAnimationEvent does not look as expected: no wind-up gesture");
        INV_LOG("game: melee extras: stagger %s, commons %s, view kick %s, wall %s, overhead %s, gesture %s",
                g_stagger_player ? "yes" : "no", g_stagger_commons ? "yes" : "no", g_view_punch ? "yes" : "no",
                g_weapon_sound.installed() ? "yes" : "no", g_choose_attack.installed() ? "yes" : "no",
                g_do_animation_event ? "yes" : "no");
    }

} // namespace game::detail
