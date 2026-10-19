#pragma once
// [melee] (see rules.hpp): the client's half of No More Room in Hell 2's melee, as the buttons of each user command, and
// the cleave.  No game code: game/melee.cpp feeds it the game's buttons and the weapon's state.
//
// NMRiH2's melee is a state machine (its combat states Windup, Holding, Release, ComboWindow, Recovery: see
// docs/nmrih2-melee-notes.md).  Here the game's own swing is the Release: the attack button is held back from the game
// while it is held (Windup / Holding), and given to it when it is let go, until the weapon swings (a swing asked for
// during the last one's wait is the queue).  The server is told, in two button bits the game does not use, that the
// player is winding up and that the swing given is a heavy one; it checks the time held itself.

namespace inv
{

    // CUserCmd::buttons bits (the game's IN_ATTACK, IN_ATTACK2) and the two the mod adds
    constexpr int k_button_attack = 0x1;
    constexpr int k_button_attack2 = 1 << 11;
    constexpr int k_button_winding = 1 << 28; // the attack is held back: winding up
    constexpr int k_button_heavy = 1 << 29; // with the attack let through: the swing is a heavy one

    class MeleeInput
    {
    public:
        enum class State
        {
            idle, // nothing held back
            winding, // the attack is held; the game does not see it
            cancelled, // a shove called the swing off: nothing until the attack is let go
            releasing // let go: the game sees the attack until the weapon swings, or `queue` runs out
        };

        // One user command.  `buttons` are the ones the game's input made, `now` a clock in seconds, `melee` whether a
        // melee weapon is in the hands, `can_heavy` whether a heavy swing is allowed (not exhausted), `next_attack`
        // that weapon's m_flNextPrimaryAttack as the client has it (it moves on when it swings) and `game_time` the
        // client's clock it is compared with.  hold <= 0: [melee] is off.  Returns the buttons to send.
        int command(int buttons, double now, bool melee, bool can_heavy, float next_attack, float game_time, float hold,
                    float queue);
        // The buttons of the command being built, without moving on (the game asks again between commands).
        int peek(int buttons) const;

        State state() const
        {
            return state_;
        }
        // While winding: since when (the clock of command()).
        double winding_since() const
        {
            return since_;
        }
        // While releasing: whether it is a heavy swing.
        bool heavy() const
        {
            return heavy_;
        }

        // The weapon's wait moving on while it was not yet over is not a swing but the server's correction of it (a
        // heavy swing's longer wait reaching the client): within this much of the client's clock it is a swing (the
        // client's clock is behind its predicted one by the interpolation).
        static constexpr float k_ready_slack = 0.3f;

    private:
        State state_ = State::idle;
        double since_ = 0.0;
        bool heavy_ = false;
        bool can_heavy_ = true;
        double released_at_ = 0.0;
        float released_next_ = 0.f;
        bool attack_was_down_ = false;
        double pressed_again_at_ = -1.0; // pressed again while releasing: the next wind-up started then
    };

    // What the n-th target (0: the first) one swing hits takes, as a factor: cleave % of the one before, never under
    // cleave_min % of the first.
    float cleave_scale(int earlier_targets, float cleave, float cleave_min);

} // namespace inv
