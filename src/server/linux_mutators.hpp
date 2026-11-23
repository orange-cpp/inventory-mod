#pragma once

#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

class ICvar;
class ConVar;

namespace game
{
    class LinuxGame;

    class LinuxMutators
    {
    public:
        void configure(ICvar* cvars);
        void start_round(std::string_view map, bool restart);
        void left_start_area(float now);
        void frame(const LinuxGame& game, float now);
        void clear();
        const std::string& line() const { return line_; }

    private:
        void set(const char* name, float amount, bool whole, bool absolute = false);
        void restore_settings();
        void build_line();

        struct Setting
        {
            ConVar* variable;
            float original;
        };

        ICvar* cvars_ = nullptr;
        std::minstd_rand random_;
        unsigned generation_ = 0;
        std::optional<float> horde_at_;
        std::vector<Setting> settings_;
        std::string line_;
        int hunted_class_ = 0;
    };
}
