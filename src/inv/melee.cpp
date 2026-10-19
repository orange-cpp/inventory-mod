#include "inv/melee.hpp"

#include <algorithm>
#include <cmath>

namespace inv
{

    int MeleeInput::command(int buttons, double now, bool melee, bool can_heavy, float next_attack, float game_time, float hold,
                            float queue)
    {
        const bool attack = (buttons & k_button_attack) != 0;
        const bool pressed = attack && !attack_was_down_;
        attack_was_down_ = attack;
        if (hold <= 0.f || !melee)
        {
            // off, or no melee weapon in the hands: the game's own buttons (a swing let go and then a weapon switch is
            // dropped)
            state_ = State::idle;
            pressed_again_at_ = -1.0;
            return buttons;
        }
        if (!can_heavy)
            can_heavy_ = false;

        if (state_ == State::releasing)
        {
            if (pressed)
                pressed_again_at_ = now;
            // the server corrected the wait (a heavy swing's): still waiting for the weapon
            if (next_attack != released_next_ && released_next_ > game_time + k_ready_slack)
                released_next_ = next_attack;
            // the weapon swung (its wait moved on: the client's prediction runs the swing), or it did not in time
            if (next_attack != released_next_ || now - released_at_ > queue)
            {
                state_ = State::idle;
                if (attack && pressed_again_at_ >= 0.0)
                {
                    state_ = State::winding;
                    since_ = pressed_again_at_;
                    can_heavy_ = can_heavy;
                }
                pressed_again_at_ = -1.0;
            }
        }
        if (state_ == State::winding && (buttons & k_button_attack2) != 0)
            state_ = State::cancelled; // a shove calls it off
        if (state_ == State::cancelled && !attack)
            state_ = State::idle;
        if (state_ == State::winding && !attack)
        {
            state_ = State::releasing;
            heavy_ = can_heavy_ && can_heavy && now - since_ >= hold;
            released_at_ = now;
            released_next_ = next_attack;
            pressed_again_at_ = -1.0;
        }
        if (state_ == State::idle && attack)
        {
            state_ = State::winding;
            since_ = now;
            can_heavy_ = can_heavy;
        }
        return peek(buttons);
    }

    int MeleeInput::peek(int buttons) const
    {
        switch (state_)
        {
        case State::winding:
            return (buttons & ~k_button_attack) | k_button_winding;
        case State::cancelled:
            return buttons & ~k_button_attack;
        case State::releasing:
            return buttons | k_button_attack | (heavy_ ? k_button_heavy : 0);
        case State::idle:
            break;
        }
        return buttons;
    }

    float cleave_scale(int earlier_targets, float cleave, float cleave_min)
    {
        if (earlier_targets <= 0)
            return 1.f;
        const float scale = std::pow(std::clamp(cleave, 0.f, 100.f) / 100.f, static_cast<float>(earlier_targets));
        return std::max(scale, std::clamp(cleave_min, 0.f, 100.f) / 100.f);
    }

} // namespace inv
