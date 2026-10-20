#include "inv/melee.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace inv
{

    int MeleeInput::command(const MeleeFrame& f)
    {
        const int buttons = f.buttons;
        const bool attack = (buttons & k_button_attack) != 0;
        const bool attack2 = (buttons & k_button_attack2) != 0;
        const bool pressed = attack && (last_buttons_ & k_button_attack) == 0;
        const bool pressed2 = attack2 && (last_buttons_ & k_button_attack2) == 0;
        const bool pressed_reload = (buttons & k_button_reload) != 0 && (last_buttons_ & k_button_reload) == 0;
        last_buttons_ = buttons;
        if (f.hold <= 0.f || !f.melee)
        {
            // off, or no melee weapon in the hands: the game's own buttons (a swing let go and then a weapon switch is
            // dropped)
            state_ = State::idle;
            shove_ = Shove::idle;
            pressed_again_at_ = -1.0;
            throw_until_ = -1.0;
            return buttons;
        }

        // ---- the swing ----
        if (state_ == State::releasing)
        {
            if (pressed)
                pressed_again_at_ = f.now;
            // the server corrected the wait (a heavy swing's): still waiting for the weapon
            if (f.next_attack != released_next_ && released_next_ > f.game_time + k_ready_slack)
                released_next_ = f.next_attack;
            // the weapon swung (its wait moved on: the client's prediction runs the swing), or it did not in time
            if (f.next_attack != released_next_ || f.now - released_at_ > k_release_grace)
            {
                state_ = State::idle;
                if (attack && pressed_again_at_ >= 0.0)
                {
                    state_ = State::winding;
                    since_ = pressed_again_at_;
                    can_heavy_ = f.can_heavy;
                }
                pressed_again_at_ = -1.0;
            }
        }
        if (state_ == State::winding && pressed_reload && f.throwing)
        {
            // the reload key while winding up throws the weapon
            state_ = State::cancelled;
            thrown_at_ = f.now;
            throw_until_ = f.now + k_throw_for;
        }
        if (state_ == State::winding && pressed2)
        {
            state_ = State::cancelled; // a shove calls it off, and is the game's own
            shove_ = Shove::passing;
        }
        if (state_ == State::cancelled && !attack)
            state_ = State::idle;
        // out of stamina while winding up a heavy swing: it goes off by itself, a light one
        const bool spent = state_ == State::winding && can_heavy_ && !f.can_heavy;
        if (state_ == State::winding && (!attack || spent))
        {
            state_ = State::releasing;
            heavy_ = !spent && can_heavy_ && f.can_heavy && f.now - since_ >= f.hold;
            released_at_ = f.now;
            released_next_ = f.next_attack;
            pressed_again_at_ = -1.0;
        }
        if (state_ == State::idle && attack)
        {
            state_ = State::winding;
            since_ = f.now;
            can_heavy_ = f.can_heavy;
        }

        // ---- the shove ----
        if (shove_ == Shove::releasing)
        {
            if (f.next_shove != shove_released_next_ && shove_released_next_ > f.game_time + k_ready_slack)
                shove_released_next_ = f.next_shove;
            if (f.next_shove != shove_released_next_ || f.now - shove_released_at_ > k_release_grace)
                shove_ = Shove::idle;
        }
        if (shove_ == Shove::passing && !attack2)
            shove_ = Shove::idle;
        if (shove_ == Shove::holding && !attack2)
        {
            shove_ = Shove::releasing;
            shove_heavy_ = f.shove_hold > 0.f && f.can_heavy && f.now - shove_since_ >= f.shove_hold;
            shove_released_at_ = f.now;
            shove_released_next_ = f.next_shove;
        }
        if (shove_ == Shove::idle && pressed2)
        {
            if (f.shove_hold > 0.f && state_ != State::winding)
            {
                shove_ = Shove::holding;
                shove_since_ = f.now;
            }
            else
                shove_ = Shove::passing;
        }
        if (f.now >= throw_until_)
            throw_until_ = -1.0;
        return peek(buttons);
    }

    int MeleeInput::peek(int buttons) const
    {
        switch (state_)
        {
        case State::winding:
            buttons = (buttons & ~k_button_attack) | k_button_winding;
            break;
        case State::cancelled:
            buttons &= ~k_button_attack;
            break;
        case State::releasing:
            buttons |= k_button_attack | (heavy_ ? k_button_heavy : 0);
            break;
        case State::idle:
            break;
        }
        switch (shove_)
        {
        case Shove::holding:
            buttons &= ~k_button_attack2;
            break;
        case Shove::releasing:
            buttons |= k_button_attack2 | (shove_heavy_ ? k_button_heavy_shove : 0);
            break;
        case Shove::idle:
        case Shove::passing:
            break;
        }
        if (throw_until_ >= 0.0)
            buttons = (buttons | k_button_throw) & ~k_button_reload;
        return buttons;
    }

    float cleave_scale(int earlier_targets, float cleave, float cleave_min)
    {
        if (earlier_targets <= 0)
            return 1.f;
        const float scale = std::pow(std::clamp(cleave, 0.f, 100.f) / 100.f, static_cast<float>(earlier_targets));
        return std::max(scale, std::clamp(cleave_min, 0.f, 100.f) / 100.f);
    }

    namespace
    {
        struct ClassInfo
        {
            MeleeClass klass;
            const char* name;
            // against [melee]'s numbers: the wind-up's length, the damage, the heavy damage, the stamina (%)
            float hold, light, heavy, stamina;
            float cleave, cleave_min; // the class's own
            int durability; // hits
        };
        // Blades cut through a crowd and wear out sooner; blunt weapons hit one target hard and last; two-handed ones
        // wind up slower, hit harder and tire more (NMRiH2's FC_*_CleaveDamage curves and per-class timings).
        constexpr ClassInfo k_classes[] = {
            {MeleeClass::blade_1h, "blade_1h", 0.85f, 100.f, 90.f, 80.f, 65.f, 30.f, 150},
            {MeleeClass::blunt_1h, "blunt_1h", 0.9f, 95.f, 100.f, 90.f, 35.f, 15.f, 250},
            {MeleeClass::blade_2h, "blade_2h", 1.15f, 115.f, 115.f, 125.f, 85.f, 40.f, 200},
            {MeleeClass::blunt_2h, "blunt_2h", 1.25f, 110.f, 125.f, 135.f, 55.f, 25.f, 300},
        };

        const ClassInfo& info(MeleeClass c)
        {
            for (const ClassInfo& i : k_classes)
                if (i.klass == c)
                    return i;
            return k_classes[1];
        }

        struct WeaponClass
        {
            const char* weapon;
            MeleeClass klass;
        };
        constexpr WeaponClass k_weapons[] = {
            {"fireaxe", MeleeClass::blade_2h},      {"katana", MeleeClass::blade_2h},      {"pitchfork", MeleeClass::blade_2h},
            {"machete", MeleeClass::blade_1h},      {"knife", MeleeClass::blade_1h},       {"crowbar", MeleeClass::blunt_1h},
            {"frying_pan", MeleeClass::blunt_1h},   {"tonfa", MeleeClass::blunt_1h},       {"riotshield", MeleeClass::blunt_1h},
            {"baseball_bat", MeleeClass::blunt_2h}, {"cricket_bat", MeleeClass::blunt_2h}, {"electric_guitar", MeleeClass::blunt_2h},
            {"golfclub", MeleeClass::blunt_2h},     {"shovel", MeleeClass::blunt_2h},
        };
    } // namespace

    const char* melee_class_name(MeleeClass c)
    {
        return info(c).name;
    }

    bool melee_class_from_name(std::string_view name, MeleeClass& out)
    {
        for (const ClassInfo& i : k_classes)
            if (name == i.name)
            {
                out = i.klass;
                return true;
            }
        return false;
    }

    MeleeClass melee_class_of(std::string_view weapon)
    {
        for (const WeaponClass& w : k_weapons)
            if (weapon == w.weapon)
                return w.klass;
        return MeleeClass::blunt_1h;
    }

    MeleeProfile melee_profile(std::string_view weapon, const MeleeRules& rules)
    {
        const auto over = rules.weapons.find(std::string(weapon));
        const MeleeWeaponRules* o = over != rules.weapons.end() ? &over->second : nullptr;
        MeleeProfile p;
        p.klass = o != nullptr && o->klass ? static_cast<MeleeClass>(*o->klass) : melee_class_of(weapon);
        const ClassInfo& c = info(p.klass);
        p.heavy_hold = o != nullptr && o->heavy_hold ? *o->heavy_hold : rules.heavy_hold * c.hold;
        p.light_damage = o != nullptr && o->light_damage ? *o->light_damage : rules.light_damage * c.light / 100.f;
        p.heavy_damage = o != nullptr && o->heavy_damage ? *o->heavy_damage : rules.heavy_damage * c.heavy / 100.f;
        // [melee]'s cleave (70) and cleave_min (30) are the scale: at those, each class has its own
        p.cleave = o != nullptr && o->cleave ? *o->cleave : std::clamp(c.cleave * rules.cleave / 70.f, 0.f, 100.f);
        p.cleave_min = std::clamp(c.cleave_min * rules.cleave_min / 30.f, 0.f, 100.f);
        p.stamina = o != nullptr && o->stamina ? *o->stamina : c.stamina;
        // a weapon's own durability is in hits; the class's is scaled by [melee]'s durability %
        p.durability = o != nullptr && o->durability
                           ? *o->durability
                           : static_cast<int>(std::lround(static_cast<float>(c.durability) * rules.durability / 100.f));
        return p;
    }

    int melee_overhead_swing(const int* start_dirs, int count)
    {
        for (int i = 0; i < count; ++i)
            if (start_dirs[i] == 0)
                return i;
        for (int i = 0; i < count; ++i)
            if (start_dirs[i] == 4 || start_dirs[i] == 7)
                return i;
        return -1;
    }

    int melee_wear_after(int wear, float hits, int durability)
    {
        if (durability <= 0 || hits <= 0.f)
            return std::clamp(wear, 0, k_wear_broken);
        const float per_hit = static_cast<float>(k_wear_broken) / static_cast<float>(durability);
        return std::clamp(wear + static_cast<int>(std::lround(per_hit * hits)), 0, k_wear_broken);
    }

} // namespace inv
