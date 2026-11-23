#include "server/linux_mutators.hpp"
#include "server/linux_game.hpp"

#include "inv/mutators.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "icvar.h"
#include "tier1/convar.h"

namespace game
{
    void LinuxMutators::configure(ICvar* cvars)
    {
        cvars_ = cvars;
        random_.seed(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
        generation_ = random_();
    }

    void LinuxMutators::set(const char* name, float amount, bool whole, bool absolute)
    {
        auto* variable = cvars_ == nullptr ? nullptr : cvars_->FindVar(name);
        if (variable == nullptr)
        {
            std::fprintf(stderr, "inventory_mod: mutator setting %s is unavailable\n", name);
            return;
        }
        const float original = variable->GetFloat();
        settings_.push_back({variable, original});
        const float value = absolute ? amount : original * amount / 100.f;
        if (whole)
            variable->SetValue(static_cast<int>(std::max(0.f, std::round(value))));
        else
            variable->SetValue(value);
    }

    void LinuxMutators::restore_settings()
    {
        for (const auto& setting : settings_)
            setting.variable->SetValue(setting.original);
        settings_.clear();
    }

    void LinuxMutators::start_round(std::string_view map, bool restart)
    {
        horde_at_.reset();
        if (restart && !line_.empty())
            return;

        restore_settings();
        hunted_class_ = 0;
        const auto& rules = inv::mutator_rules();
        inv::set_active_mutators(inv::pick_mutators(rules, map, random_()));
        ++generation_;
        if (inv::mutator_active("hunted"))
        {
            constexpr std::array limits{"z_smoker_limit", "z_boomer_limit", "z_hunter_limit",
                                        "z_spitter_limit", "z_jockey_limit", "z_charger_limit"};
            hunted_class_ = rules.hunted.klass != 0 ? rules.hunted.klass : static_cast<int>(random_() % 6) + 1;
            set(limits[hunted_class_ - 1], static_cast<float>(rules.hunted.limit), true, true);
        }
        if (inv::mutator_active("crowd"))
        {
            set("z_common_limit", rules.crowd.commons, true);
            for (const char* name : {"z_mob_spawn_min_size", "z_mob_spawn_max_size", "z_mega_mob_size"})
                set(name, rules.crowd.mobs, true);
        }
        if (inv::mutator_active("witch"))
        {
            set("z_witch_health", rules.witch.health, true);
            set("z_witch_anger_rate", rules.witch.anger, false);
            set("z_witch_personal_space", rules.witch.space, false);
            set("z_witch_damage", rules.witch.damage, false);
        }
        if (inv::mutator_active("tank"))
        {
            set("z_tank_health", rules.tank.health, true);
            set("z_tank_speed", rules.tank.speed, false);
        }
        build_line();
    }

    void LinuxMutators::left_start_area(float now)
    {
        if (!horde_at_ && inv::mutator_active("horde_tide"))
            horde_at_ = now + static_cast<float>(inv::mutator_rules().horde_tide.first);
    }

    void LinuxMutators::frame(const LinuxGame& game, float now)
    {
        if (!horde_at_ || now < *horde_at_)
            return;
        horde_at_ = now + static_cast<float>(inv::mutator_rules().horde_tide.every);
        if (game.trigger_panic())
            std::fprintf(stderr, "inventory_mod: triggered a horde tide\n");
    }

    void LinuxMutators::build_line()
    {
        const auto& rules = inv::mutator_rules();
        line_ = "@mut@ " + std::to_string(generation_) + (rules.announce ? " 1|" : " 0|");
        bool first = true;
        for (const std::string& id : inv::active_mutators())
        {
            if (!first)
                line_ += ';';
            first = false;
            line_ += id;
            if (id == "fog")
                line_ += ' ' + std::to_string(static_cast<int>(std::lround(rules.fog.density * 1000.f))) +
                         ' ' + (rules.fog.color.empty() ? "-" : rules.fog.color);
            else if (id == "hunted")
                line_ += ' ' + std::to_string(hunted_class_);
        }
        line_ += '\n';
    }

    void LinuxMutators::clear()
    {
        restore_settings();
        horde_at_.reset();
        inv::set_active_mutators({});
        ++generation_;
        build_line();
    }
}
