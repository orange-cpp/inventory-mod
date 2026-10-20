#pragma once
// [melee] (see rules.hpp): the client's half of No More Room in Hell 2's melee, as the buttons of each user command;
// the weapon classes and each weapon's numbers; the cleave.  No game code: game/melee.cpp feeds it the game's buttons
// and the weapon's state.
//
// NMRiH2's melee is a state machine (its combat states Windup, Holding, Release, ComboWindow, Recovery: see
// docs/nmrih2-melee-notes.md).  Here the game's own swing is the Release: the attack button is held back from the game
// while it is held (Windup / Holding), and given to it when it is let go, until the weapon swings (if it is not ready
// within k_release_grace, the swing is dropped: no queue).  The shove is the same: held back while held, a heavy shove when held long
// enough.  The server is told, in button bits the game does not use, that the player is winding up, that the swing or
// shove given is a heavy one, and that the weapon is thrown; it checks the time held itself.

#include "inv/rules.hpp"

#include <string_view>

namespace inv
{

    // CUserCmd::buttons bits (the game's IN_ATTACK, IN_ATTACK2, IN_RELOAD) and the ones the mod adds
    constexpr int k_button_attack = 0x1;
    constexpr int k_button_attack2 = 1 << 11;
    constexpr int k_button_reload = 0x2000; // with a melee weapon (no reload): block; while winding up: throw
    constexpr int k_button_throw = 1 << 27; // the weapon is thrown (sent for a moment)
    constexpr int k_button_winding = 1 << 28; // the attack is held back: winding up
    constexpr int k_button_heavy = 1 << 29; // with the attack let through: the swing is a heavy one
    constexpr int k_button_heavy_shove = 1 << 30; // with the shove let through: a heavy shove

    // One user command, as the client sees it.
    struct MeleeFrame
    {
        int buttons = 0; // the ones the game's input made
        double now = 0.0; // a clock in seconds
        bool melee = false; // a melee weapon in the hands
        bool can_heavy = true; // not exhausted
        float next_attack = 0.f; // that weapon's m_flNextPrimaryAttack as the client has it (it moves on with a swing)
        float next_shove = 0.f; // ... m_flNextSecondaryAttack (it moves on with a shove)
        float game_time = 0.f; // the client's clock they are compared with
        float hold = 0.f; // heavy_hold of the weapon in the hands (0: no [melee])
        float shove_hold = 0.f; // heavy_shove_hold (0: no heavy shove)
        bool throwing = true; // the server allows the throw
    };

    class MeleeInput
    {
    public:
        enum class State
        {
            idle, // nothing held back
            winding, // the attack is held; the game does not see it
            cancelled, // a shove or a throw called the swing off: nothing until the attack is let go
            releasing // let go: the game sees the attack until the weapon swings, or k_release_grace runs out
        };
        enum class Shove
        {
            idle,
            holding, // the shove is held; the game does not see it
            passing, // the game's own shove (it called a wind-up off)
            releasing // let go: the game sees it until the weapon shoves
        };

        // One user command: returns the buttons to send.
        int command(const MeleeFrame& frame);
        // The buttons of the command being built, without moving on (the game asks again between commands).
        int peek(int buttons) const;

        State state() const
        {
            return state_;
        }
        Shove shove() const
        {
            return shove_;
        }
        // While winding: since when (the frame's clock).  While holding the shove: shove_since().
        double winding_since() const
        {
            return since_;
        }
        double shove_since() const
        {
            return shove_since_;
        }
        // While releasing: whether it is a heavy swing / shove.
        bool heavy() const
        {
            return heavy_;
        }
        bool heavy_shove() const
        {
            return shove_heavy_;
        }
        // The last throw (the frame's clock; < 0: none).
        double thrown_at() const
        {
            return thrown_at_;
        }

        // The weapon's wait moving on while it was not yet over is not a swing but the server's correction of it (a
        // heavy swing's longer wait reaching the client): within this much of the client's clock it is a swing (the
        // client's clock is behind its predicted one by the interpolation).
        static constexpr float k_ready_slack = 0.3f;
        // How long a swing or shove let go is given to the game: long enough to reach a weapon that is ready (the
        // prediction runs it a command later), not through its wait (no queue: let go during the recovery, it is
        // dropped).
        static constexpr double k_release_grace = 0.1;
        // How long the throw bit is sent (the server samples the buttons once a tick).
        static constexpr double k_throw_for = 0.15;

    private:
        State state_ = State::idle;
        double since_ = 0.0;
        bool heavy_ = false;
        bool can_heavy_ = true;
        double released_at_ = 0.0;
        float released_next_ = 0.f;
        double pressed_again_at_ = -1.0; // pressed again while releasing: the next wind-up started then
        Shove shove_ = Shove::idle;
        double shove_since_ = 0.0;
        bool shove_heavy_ = false;
        double shove_released_at_ = 0.0;
        float shove_released_next_ = 0.f;
        double thrown_at_ = -1.0;
        double throw_until_ = -1.0;
        int last_buttons_ = 0;
    };

    // What the n-th target (0: the first) one swing hits takes, as a factor: cleave % of the one before, never under
    // cleave_min % of the first.
    float cleave_scale(int earlier_targets, float cleave, float cleave_min);

    // NMRiH2's weapon classes (its animation sets and cleave curves: 1hBlade, 1hBlunt, 2hBlade, 2hBlunt).
    enum class MeleeClass
    {
        blade_1h,
        blunt_1h,
        blade_2h,
        blunt_2h
    };
    const char* melee_class_name(MeleeClass c);
    bool melee_class_from_name(std::string_view name, MeleeClass& out);
    // The game's melee weapons' classes ("fireaxe" ...); an unknown one (a custom script) is blunt_1h.
    MeleeClass melee_class_of(std::string_view weapon);

    // One melee weapon's numbers: [melee]'s, scaled by its class, overridden by [melee.weapons.<name>].
    struct MeleeProfile
    {
        MeleeClass klass = MeleeClass::blunt_1h;
        float heavy_hold = 0.6f;
        float light_damage = 40.f;
        float heavy_damage = 200.f;
        float cleave = 70.f;
        float cleave_min = 30.f;
        float stamina = 100.f; // % of the stamina costs (swing, heavy swing, per hit, winding)
        int durability = 200; // hits before it breaks (0: never)
    };
    MeleeProfile melee_profile(std::string_view weapon, const MeleeRules& rules);

    // The heavy swing's pick among a melee script's primary attacks, by their start directions (the game's codes: N 0,
    // S 1, E 2, W 3, NE 4, SE 5, SW 6, NW 7): one from straight above, else from above to a side; -1: none (the weapon
    // only swings sideways, as the fireaxe and the katana).
    int melee_overhead_swing(const int* start_dirs, int count);

    // A weapon's wear (0 new .. k_wear_broken broken; WeaponState::wear) after `hits` more hits, by its durability.
    constexpr int k_wear_broken = 10000;
    int melee_wear_after(int wear, float hits, int durability);

} // namespace inv
