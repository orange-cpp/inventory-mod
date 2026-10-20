// [melee], the client's half: the attack and shove buttons of each user command (the wind-up, the heavy
// shove, the throw) and the animation laid over the viewmodel.

#include "game/melee.hpp"

#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/client_view.hpp"
#include "game/entities.hpp"
#include "inv/melee.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace game::detail
{

    using namespace sdk;

    // ---- the buttons --------------------------------------------------------------------------------------------
    //
    // The attack button of each user command, while a melee weapon is in the hands and the server has [melee]
    // (inv::MeleeInput): held back while winding up, given when let go until the weapon swings; the shove the same
    // (a heavy shove when held long enough); the reload key while winding up throws the weapon.  It has to be the
    // client's input: the game predicts the swing on the client from the same user command, so a swing the server
    // alone held back would still play on the client.  CInput::CreateMove (slot 3 of CInput's vtable 0x1057575C,
    // 0x101341F0) builds the command and stores this->GetButtonBits(1) (slot 2, 0x10131570, thiscall, ret 4: the
    // buttons from the kbuttons +attack ...) in CUserCmd::buttons (+0x24); ExtraMouseSample asks GetButtonBits(0)
    // between commands.  The hook changes what it returns, so the prediction and the server see the same buttons.
    // CUserCmd::buttons goes to the server as 32 bits: the bits the mod adds (inv::k_button_throw, _winding, _heavy,
    // _heavy_shove: 1 << 27 .. 1 << 30, which the game does not use) arrive in the player's m_nButtons.
    //
    //   A melee weapon in the hands: the client's active weapon's vtable is C_TerrorMeleeWeapon's (0x10609BFC, the one
    //       of its complete object locator with offset 0; checked by its RTTI name).  Its m_flNextPrimaryAttack
    //       (+0x960) moves on when the client predicts the swing, m_flNextSecondaryAttack (+0x964) with a shove: the
    //       button is given until then.  They also move on when the server's longer wait after a heavy swing reaches
    //       the client (a prediction correction, a round trip after the swing): that is told apart by the client's
    //       clock, gpGlobals->curtime (the pointer that C_BaseCombatWeapon::ReloadOrSwitchWeapons reads at
    //       0x10017922 before comparing it with +0x960 and +0x964): the weapon was not ready yet (seen in the game,
    //       when there was a queue: a swing let go during a heavy one's wait was dropped without it).

    core::Hook<GetButtonBitsFn> g_get_button_bits;
    core::Hook<CalcViewModelViewFn> g_calc_viewmodel_view;
    core::Hook<ChooseAttackFn> g_c_choose_attack;
    inv::MeleeInput g_melee_input;
    std::uintptr_t g_c_melee_vtable = 0;
    std::uintptr_t g_c_globals = 0; // the client's CGlobalVars**
    bool g_throwing = true; // the server allows the throw (its features, apply_melee_line)
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

    // What the animation needs of the input and the server (main thread: they all run there).
    struct MeleeAnimation
    {
        float hold = 0.f; // heavy_hold of the weapon in the hands
        float shove_hold = 0.f;
        bool can_heavy = true;
        bool blocking = false; // the reload key held with a melee weapon, nothing else going on
        double let_go_at = -10.0; // a swing let go (seconds())
        bool let_go_heavy = false;
        double shoved_at = -10.0; // a shove let go
        bool shoved_heavy = false;
        double hit_at = -10.0; // the server said the last swing hit (seconds())
        bool hit_heavy = false;
        float pose = 0.f; // the wind-up pose shown, eased: 0 .. 1
        float block = 0.f; // the block pose, eased
        float brace = 0.f; // a heavy shove held: the arms drawn in, eased
        double last = 0.0; // the last frame
    };
    MeleeAnimation g_anim;

    int melee_command(int bits)
    {
        float hold = 0.f, shove_hold = 0.f;
        bool exhausted = false;
        {
            Session& s = local_session();
            std::lock_guard lock(s.mutex);
            hold = s.melee_hold;
            shove_hold = s.melee_shove_hold;
            exhausted = s.stamina_exhausted;
        }
        void* weapon = hold > 0.f ? client_active_weapon(client_local_player()) : nullptr;
        const auto w = reinterpret_cast<std::uintptr_t>(weapon);
        inv::MeleeFrame f;
        f.buttons = bits;
        f.now = seconds();
        f.melee = weapon != nullptr && mem::read_or<std::uintptr_t>(w) == g_c_melee_vtable;
        f.can_heavy = !exhausted;
        f.next_attack = f.melee ? mem::read_or<float>(w + k_cw_next_primary) : 0.f;
        f.next_shove = f.melee ? mem::read_or<float>(w + k_cw_next_secondary) : 0.f;
        f.game_time = mem::read_or<float>(mem::read_or<std::uintptr_t>(g_c_globals) + k_off_curtime);
        f.hold = hold;
        f.shove_hold = shove_hold;
        f.throwing = g_throwing;
        const ULONGLONG now = GetTickCount64();
        const inv::MeleeInput::State before = g_melee_input.state();
        const inv::MeleeInput::Shove shove_before = g_melee_input.shove();
        const double since = g_melee_input.winding_since();
        const double thrown_before = g_melee_input.thrown_at();
        const int out = g_melee_input.command(f);
        const inv::MeleeInput::State after = g_melee_input.state();
        g_anim.hold = hold;
        g_anim.shove_hold = shove_hold;
        g_anim.can_heavy = !exhausted;
        g_anim.blocking = f.melee && (bits & inv::k_button_reload) != 0 && after == inv::MeleeInput::State::idle &&
                          g_melee_input.shove() == inv::MeleeInput::Shove::idle;
        if (before != after || after == inv::MeleeInput::State::winding)
        {
            const bool winding = after == inv::MeleeInput::State::winding;
            const bool heavy_let_go = before == inv::MeleeInput::State::winding && after == inv::MeleeInput::State::releasing &&
                                      g_melee_input.heavy();
            Session& s = local_session();
            std::lock_guard lock(s.mutex);
            // (the HUD's clock is GetTickCount64)
            s.melee_winding_since =
                winding ? now - static_cast<ULONGLONG>(std::max(0.0, f.now - g_melee_input.winding_since()) * 1000.0) : 0;
            s.melee_can_heavy = !exhausted;
            if (heavy_let_go)
                s.melee_heavy_at = now;
        }
        if (before == inv::MeleeInput::State::winding && after == inv::MeleeInput::State::releasing)
        {
            INV_LOG("melee: let go after %.2f s: a %s swing", f.now - since, g_melee_input.heavy() ? "heavy" : "light");
            g_released_at = now;
            g_anim.let_go_at = f.now;
            g_anim.let_go_heavy = g_melee_input.heavy();
        }
        else if (before == inv::MeleeInput::State::releasing && after != inv::MeleeInput::State::releasing)
            INV_LOG("melee: %s %.2f s after it was let go", f.melee && f.next_attack != g_released_next ? "it swung" : "dropped",
                    static_cast<double>(now - g_released_at) / 1000.0);
        if (after == inv::MeleeInput::State::releasing && before != inv::MeleeInput::State::releasing)
            g_released_next = f.next_attack;
        if (shove_before == inv::MeleeInput::Shove::holding && g_melee_input.shove() == inv::MeleeInput::Shove::releasing)
        {
            g_anim.shoved_at = f.now;
            g_anim.shoved_heavy = g_melee_input.heavy_shove();
            if (g_melee_input.heavy_shove())
                INV_LOG("melee: a heavy shove let go");
        }
        if (g_melee_input.thrown_at() != thrown_before)
        {
            INV_LOG("melee: thrown after winding up %.2f s", f.now - since);
            g_anim.let_go_at = f.now; // the throw's own lunge
            g_anim.let_go_heavy = true;
        }
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

    // The overhead heavy swing (see melee.cpp): the client's prediction picks the same swing as the server, from the
    // same buttons (the local player's m_nButtons, +0x13A4, is the user command's while it is predicted).  Only when
    // the server has [melee] (a heavy_hold was sent) and its rules say overhead (melee_overhead below).
    bool g_overhead = false;

    int __fastcall hk_c_choose_attack(void* weapon, void* edx, void* info)
    {
        mem::guarded([&] {
            void* player = client_local_player();
            if (!g_overhead || player == nullptr ||
                (mem::read_or<int>(reinterpret_cast<std::uintptr_t>(player) + k_c_buttons) & inv::k_button_heavy) == 0)
                return;
            const auto i = reinterpret_cast<std::uintptr_t>(info);
            const int count = std::min(mem::read_or<int>(i + k_off_info_attack_count), 8);
            const auto attacks = mem::read_or<std::uintptr_t>(i + k_off_info_attacks);
            int dirs[8] = {};
            for (int n = 0; n < count; ++n)
                dirs[n] = mem::read_or<int>(attacks + n * k_attack_size + k_off_attack_start_dir, -1);
            const int index = count > 0 && attacks != 0 ? inv::melee_overhead_swing(dirs, count) : -1;
            if (index < 0)
                return;
            const auto w = reinterpret_cast<std::uintptr_t>(weapon);
            *reinterpret_cast<int*>(w + k_cw_melee_next_attack) = index;
            *reinterpret_cast<float*>(w + k_cw_melee_last_attack) =
                mem::read_or<float>(mem::read_or<std::uintptr_t>(g_c_globals) + k_off_curtime);
        });
        return g_c_choose_attack.original()(weapon, edx, info);
    }

    // ---- what the server says -----------------------------------------------------------------------------------

    void apply_melee_line(const char* text)
    {
        int hold = 0, shove_hold = 0, features = 0;
        std::sscanf(text + sizeof(k_melee_marker) - 1, "%d %d %d", &hold, &shove_hold, &features);
        g_overhead = hold > 0 && (features & 1) != 0;
        g_throwing = (features & 2) != 0;
        Session& s = local_session();
        std::lock_guard lock(s.mutex);
        s.melee_hold = std::clamp(hold, 0, 10000) / 1000.f;
        s.melee_shove_hold = std::clamp(shove_hold, 0, 10000) / 1000.f;
    }

    void melee_hit_feedback(bool heavy)
    {
        g_anim.hit_at = seconds();
        g_anim.hit_heavy = heavy;
    }

    void apply_melee_hit_line(const char* text)
    {
        int heavy = 0;
        std::sscanf(text + sizeof(k_melee_hit_marker) - 1, "%d", &heavy);
        melee_hit_feedback(heavy != 0);
    }

    // ---- the animation --------------------------------------------------------------------------------------------
    //
    // NMRiH2's wind-up has its own animations (Windup, Loop, Release, Recover sections per weapon class); L4D2's melee
    // viewmodels have only their swings.  So the wind-up is a pose laid over the viewmodel: while the attack is held
    // the weapon is drawn back, raised and turned to the right as the charge grows (eased: quick at first, slower
    // towards full), and trembles once a heavy swing is ready.  Let go, it snaps forward into the game's own swing
    // animation with a follow-through (a lunge forward and down, longer and deeper for a heavy swing or a throw).  A
    // shove that calls it off eases it back.  Blocking (the reload key) holds the weapon up across the body; holding a
    // heavy shove draws the arms in.  A hit the server reports jolts the weapon back for a moment (the impact).
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

    // 0 .. 1 .. 0 over `length` seconds after `at`
    float pulse(double now, double at, float length)
    {
        const float t = static_cast<float>(now - at);
        return t >= 0.f && t < length ? std::sin(3.14159265f * t / length) : 0.f;
    }

    void ease(float& value, float target, float rate, float dt)
    {
        value += (target - value) * std::min(1.f, dt * rate);
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
        ease(g_anim.pose, target, target > g_anim.pose ? 14.f : state == inv::MeleeInput::State::cancelled ? 6.f : 28.f, dt);
        ease(g_anim.block, g_anim.blocking ? 1.f : 0.f, g_anim.blocking ? 16.f : 10.f, dt);
        float brace_target = 0.f;
        if (g_melee_input.shove() == inv::MeleeInput::Shove::holding && g_anim.shove_hold > 0.f)
            brace_target = std::clamp(static_cast<float>(now - g_melee_input.shove_since()) / g_anim.shove_hold, 0.f, 1.f);
        ease(g_anim.brace, brace_target, brace_target > g_anim.brace ? 10.f : 25.f, dt);
        const float pose = g_anim.pose, block = g_anim.block, brace = g_anim.brace;
        // the follow-through after letting go, the heavy shove's push, the impact of a hit
        const float depth = g_anim.let_go_heavy ? 1.f : 0.35f;
        const float through = pulse(now, g_anim.let_go_at, g_anim.let_go_heavy ? 0.42f : 0.22f) * depth;
        const float push = g_anim.shoved_heavy ? pulse(now, g_anim.shoved_at, 0.3f) : 0.f;
        const float impact = pulse(now, g_anim.hit_at, g_anim.hit_heavy ? 0.16f : 0.1f) * (g_anim.hit_heavy ? 1.f : 0.5f);
        // a heavy swing ready: the arms tremble with the effort
        float tremble = 0.f;
        if (state == inv::MeleeInput::State::winding && charge >= 1.f && g_anim.can_heavy)
            tremble = std::sin(static_cast<float>(now) * 47.f) * 0.5f + std::sin(static_cast<float>(now) * 31.f) * 0.5f;
        if (pose < 0.001f && block < 0.001f && brace < 0.001f && through == 0.f && push == 0.f && impact == 0.f)
            return;
        Vec3 forward, right, up;
        angle_vectors(angles, forward, right, up);
        const float f = -2.8f * pose + 3.5f * through + 1.0f * block - 1.5f * brace + 3.0f * push - 1.2f * impact;
        const float r = 2.6f * pose - 0.8f * through - 1.8f * block - 0.6f * brace;
        const float u = 1.6f * pose - 1.4f * through + 2.2f * block + 0.5f * brace - 0.6f * push + 0.08f * tremble;
        position.x += forward.x * f + right.x * r + up.x * u;
        position.y += forward.y * f + right.y * r + up.y * u;
        position.z += forward.z * f + right.z * r + up.z * u;
        angles.x += -11.f * pose + 9.f * through - 6.f * block + 4.f * brace + 5.f * push - 3.f * impact + 0.35f * tremble;
        angles.y += -9.f * pose + 4.f * through + 18.f * block + 0.25f * tremble;
        angles.z += 14.f * pose - 7.f * through - 38.f * block + 6.f * brace + 2.f * impact;
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
        const std::uintptr_t choose = client.base + k_rva_c_choose_attack;
        if (mem::find_pattern(choose, 17, "55 8B EC 56 8B 75 08 8B 86 2C 0C 00 00 85 C0 7F 0A") != choose ||
            !g_c_choose_attack.create("ClientChooseMeleeAttack", reinterpret_cast<void*>(choose), hk_c_choose_attack) ||
            !g_c_choose_attack.enable())
            INV_LOG("game: the client's melee swing choice does not look as expected: the overhead heavy swing is mispredicted");
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
