// The chapter mutators: see mutators.hpp.

#include "game/mutators.hpp"

#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/client_view.hpp"
#include "game/entities.hpp"
#include "game/fog.hpp"
#include "game/players.hpp"
#include "inv/mutators.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace game::detail
{

    using namespace sdk;

    // ---- the server: the roll and the line ------------------------------------------------------------------------
    //
    // A round start is the next chapter, a new game or the same map again (lifecycle.cpp's on_round_start tells them
    // apart).  The first two roll the chapter's mutators (inv::pick_mutators); the same map again keeps them, so a lost
    // round never rolls a different chapter.  The fog is drawn by each client (client.dll's fog ConVars, fog.cpp), so
    // the server only says what the chapter has: a ClientPrintf line to every player whose client runs the mod, as
    // tell_melee does with [melee], told again every few seconds because a client forgets it whenever it leaves a game
    // (and a player may join late).  The host of a listen server reads the same line in-process.

    namespace
    {
        std::minstd_rand& mutator_random()
        {
            static std::minstd_rand random(static_cast<unsigned>(GetTickCount64() * 2654435761u));
            return random;
        }

        std::vector<std::string> g_active; // the chapter's mutators (inv::mutators ids)
        // Changes with every roll; it starts at a random number so a client that moves to another server announces its
        // first roll too.
        unsigned g_generation = mutator_random()();
        bool g_rolled = false; // a round has started since the mod loaded

        struct Told
        {
            unsigned generation = 0;
            ULONGLONG at = 0;
        };
        std::map<ServerPlayer*, Told> g_told;
        Told g_host_told;

        constexpr ULONGLONG k_tell_again_ms = 2000;

        // ---- the director: horde_tide, hunted, crowd, witch, tank ---------------------------------------------------
        //
        // All go through the game's own console on the server (ServerCommand), as set_ammo_limits does: horde_tide runs
        // director_force_panic_event (its callback 0x10287900 is TheDirector's ForcePanicEvent, 0x1025CC60, with no
        // check of who asked), every `every` seconds of game time from `first` seconds after the event
        // player_left_start_area.  The others set the game's ConVars for the chapter: hunted z_<class>_limit, which the
        // director's limit of a class (0x1025C7C0) reads when no director script gives one (a finale's or a mutation's
        // script still wins); crowd the commons' and mobs' sizes; witch and tank theirs.  What each was is read off the
        // ConVar (its parent's m_fValue, +0x2C) and put back at the next roll, unless the next chapter changes it too:
        // then it is worked out from the same value (a command queued by the last roll may not have run yet).  All of
        // them are FCVAR_CHEAT, which the console refuses with sv_cheats off: the flag is taken off them
        // (ConCommandBase::m_nFlags +0x14; a ConVar's parent's too).  See sdk.hpp.

        constexpr const char* k_limit_names[] = {nullptr,          "z_smoker_limit", "z_boomer_limit", "z_hunter_limit",
                                                 "z_spitter_limit", "z_jockey_limit", "z_charger_limit"};
        constexpr const char* k_class_plurals[] = {"", "smokers", "boomers", "hunters", "spitters", "jockeys", "chargers"};

        int g_hunted_class = 0; // the chapter's hunted class (1 smoker ... 6 charger), 0: none
        bool g_horde_armed = false; // the survivors have left the start area: the hordes come
        float g_horde_at = 0.f; // the game time of the next one

        struct Setting
        {
            std::string name;
            float before; // what it was before any mutator
            bool whole; // a whole number (a count, a health)
        };
        std::vector<Setting> g_settings; // the chapter's changed settings
        std::vector<Setting> g_prior; // during a roll: the last chapter's

        // The director's ConVar or ConCommand `name`, with the cheat flag off; 0 when not found.
        std::uintptr_t director_object(const char* name, bool command)
        {
            const std::uintptr_t object = find_server_convar(name);
            if (object == 0)
            {
                INV_LOG("mutators: %s not found in server.dll", name);
                return 0;
            }
            if (command) // (a ConCommand has no parent: +0x1C is its completion callback)
                mem::guarded([&] { *reinterpret_cast<int*>(object + k_off_convar_flags) &= ~k_fcvar_cheat; });
            else
                unlock_convar(object, k_fcvar_cheat);
            return object;
        }

        // A ConVar's value (m_fValue, read from the parent as the game does); -1 when unreadable.
        float convar_value(std::uintptr_t convar)
        {
            std::uintptr_t holder = mem::read_or<std::uintptr_t>(convar + k_off_convar_parent, 0);
            if (holder == 0)
                holder = convar;
            return mem::read_or<float>(holder + k_off_convar_float, -1.f);
        }

        void write_setting(const std::string& name, float value, bool whole)
        {
            char command[96];
            if (whole)
                std::snprintf(command, sizeof(command), "%s %ld\n", name.c_str(), std::lround(value));
            else
                std::snprintf(command, sizeof(command), "%s %g\n", name.c_str(), value);
            if (server_command(command))
                INV_LOG("mutators: %s", command);
        }

        // Sets the director's ConVar `name` for the chapter to to(what it was before any mutator).
        template <class F>
        void change_setting(const char* name, bool whole, F&& to)
        {
            const std::uintptr_t convar = director_object(name, false);
            if (convar == 0)
                return;
            float before = -1.f;
            for (const Setting& prior : g_prior)
                if (prior.name == name)
                    before = prior.before;
            if (before < 0.f)
                before = convar_value(convar);
            if (before < 0.f)
            {
                INV_LOG("mutators: %s could not be read: left as it is", name);
                return;
            }
            float value = to(before);
            if (whole)
                value = std::max(0.f, std::round(value));
            g_settings.push_back({name, before, whole});
            write_setting(name, value, whole);
        }

        // % of a setting
        auto percent(int value)
        {
            return [value](float before) { return before * static_cast<float>(value) / 100.f; };
        }

        // The chapter's settings (hunted, crowd, witch, tank); the last chapter's that this one leaves alone go back.
        void apply_settings()
        {
            g_prior = std::move(g_settings);
            g_settings.clear();
            g_hunted_class = 0;
            const inv::MutatorRules& rules = inv::mutator_rules();
            if (inv::mutator_active("hunted"))
            {
                const int klass = rules.hunted.klass != 0 ? rules.hunted.klass : static_cast<int>(mutator_random()() % 6u) + 1;
                change_setting(k_limit_names[klass], true, [&](float) { return static_cast<float>(rules.hunted.limit); });
                g_hunted_class = klass;
                INV_LOG("mutators: hunted by %s (%d at once)", k_class_plurals[klass], rules.hunted.limit);
            }
            if (inv::mutator_active("crowd"))
            {
                change_setting("z_common_limit", true, percent(rules.crowd.commons));
                for (const char* name : {"z_mob_spawn_min_size", "z_mob_spawn_max_size", "z_mega_mob_size"})
                    change_setting(name, true, percent(rules.crowd.mobs));
            }
            if (inv::mutator_active("witch"))
            {
                change_setting("z_witch_health", true, percent(rules.witch.health));
                change_setting("z_witch_anger_rate", false, percent(rules.witch.anger));
                change_setting("z_witch_personal_space", false, percent(rules.witch.space));
                change_setting("z_witch_damage", false, percent(rules.witch.damage));
            }
            if (inv::mutator_active("tank"))
            {
                change_setting("z_tank_health", true, percent(rules.tank.health));
                change_setting("z_tank_speed", false, percent(rules.tank.speed));
            }
            for (const Setting& prior : g_prior)
                if (std::none_of(g_settings.begin(), g_settings.end(), [&](const Setting& s) { return s.name == prior.name; }))
                    write_setting(prior.name, prior.before, prior.whole);
            g_prior.clear();
        }

        void horde_tide_frame()
        {
            if (!g_horde_armed || !inv::mutator_active("horde_tide") || game_time() < g_horde_at)
                return;
            g_horde_at = game_time() + static_cast<float>(inv::mutator_rules().horde_tide.every);
            static bool unlocked = false;
            if (!unlocked)
                unlocked = director_object("director_force_panic_event", true) != 0;
            if (unlocked && server_command("director_force_panic_event\n"))
                INV_LOG("mutators: horde tide, the next in %d seconds", inv::mutator_rules().horde_tide.every);
        }

        std::string mutator_line()
        {
            const inv::MutatorRules& rules = inv::mutator_rules();
            std::string line = std::string(k_mutator_marker) + " " + std::to_string(g_generation) + " " +
                               (rules.announce ? "1" : "0") + "|";
            bool first = true;
            for (const std::string& id : g_active)
            {
                char entry[64] = {};
                if (id == "fog")
                    std::snprintf(entry, sizeof(entry), "fog %d %s", static_cast<int>(rules.fog.density * 1000.f + 0.5f),
                                  rules.fog.color.empty() ? "-" : rules.fog.color.c_str());
                else if (id == "hunted")
                    std::snprintf(entry, sizeof(entry), "hunted %d", g_hunted_class);
                else // (a gameplay mutator: the server's own; the client only shows its name)
                    std::snprintf(entry, sizeof(entry), "%s", id.c_str());
                line += (first ? "" : ";") + std::string(entry);
                first = false;
            }
            return line;
        }
    } // namespace

    void mutators_on_round_start(const char* map, bool again)
    {
        g_horde_armed = false; // (from when the survivors leave the start area again)
        if (again && g_rolled)
        {
            INV_LOG("mutators: the same map again, the chapter keeps its %zu", g_active.size());
            return;
        }
        g_rolled = true;
        g_active = inv::pick_mutators(inv::mutator_rules(), map != nullptr ? map : "", mutator_random()());
        ++g_generation;
        // the gameplay ones: the rules read with their changes from now on (inv::set_active_mutators), and every
        // inventory gets the chapter's carry limit (heavy_load), keeping what it holds (as session_apply_limits does)
        const float weight_before = config_limits().max_weight;
        inv::set_active_mutators(g_active);
        apply_settings(); // the director's (hunted, crowd, witch, tank); the last chapter's back
        if (config_limits().max_weight != weight_before)
        {
            auto rebuild = [](Session& s) {
                std::lock_guard lock(s.mutex);
                inv::Inventory rebuilt(config_limits());
                rebuilt.from_text(s.inventory.to_text());
                s.inventory = std::move(rebuilt);
            };
            if (Session& local = local_session(); !local.remote)
                rebuild(local);
            for (auto& [id, sp] : g_players)
                rebuild(sp->session);
            INV_LOG("mutators: the carry limit is %.1f for this chapter", config_limits().max_weight);
        }
        std::string names;
        for (const std::string& id : g_active)
            names += (names.empty() ? "" : ", ") + id;
        if (inv::mutator_rules().enabled)
            INV_LOG("mutators: %s gets %s", map != nullptr ? map : "?", names.empty() ? "none" : names.c_str());
    }

    void mutators_on_event(const char* name)
    {
        if (std::strcmp(name, "player_left_start_area") == 0 && !g_horde_armed && inv::mutator_active("horde_tide"))
        {
            g_horde_armed = true;
            g_horde_at = game_time() + static_cast<float>(inv::mutator_rules().horde_tide.first);
            INV_LOG("mutators: the survivors left the start area: a horde tide in %d seconds",
                    inv::mutator_rules().horde_tide.first);
        }
    }

    void mutators_frame()
    {
        if (!g_rolled)
            return;
        horde_tide_frame();
        const ULONGLONG now = GetTickCount64();
        std::string line;
        auto due = [&](const Told& told) { return told.generation != g_generation || now - told.at >= k_tell_again_ms; };

        if (!g_dedicated && host_player() != nullptr && due(g_host_told))
        {
            line = mutator_line();
            mem::guarded([&] { apply_mutator_line(line.c_str()); });
            g_host_told = {g_generation, now};
        }
        if (g_client_printf == nullptr)
            return;
        for (int index = 1; index <= 64; ++index)
        {
            void* player = entity_at_index(index);
            char name[16];
            if (player == nullptr || !classname_of(player, name, sizeof(name)) || std::strcmp(name, "player") != 0 ||
                is_bot(player) || (!g_dedicated && player == host_player()))
                continue;
            ServerPlayer* sp = server_player(player);
            void* edict = edict_of(player);
            if (sp == nullptr || !sp->has_mod || edict == nullptr)
                continue;
            Told& told = g_told[sp];
            if (!due(told))
                continue;
            if (line.empty())
                line = mutator_line();
            const std::string text = line + "\n";
            mem::guarded([&] { g_client_printf(g_engine_server, edict, text.c_str()); });
            told = {g_generation, now};
        }
    }

    // ---- the client: what the line asks for -----------------------------------------------------------------------
    //
    // The line is kept as what is wanted; mutators_client_tick applies it once the player has been in a game for a
    // moment (fog_override -1 copies the map's env_fog_controller values into the ConVars, and those reach the client
    // with the map's entities), and leaving a game takes it off again: the ConVars outlive a map change, and the next
    // map's own fog has to be read first.

    namespace
    {
        struct Wanted
        {
            bool fog = false;
            float density = 0.f;
            std::string color;
            bool operator==(const Wanted&) const = default;
        };
        Wanted g_wanted, g_applied;
        bool g_seen = false; // a line has come since the mod loaded
        unsigned g_seen_generation = 0;
        std::string g_announce; // "Mutator: Fog", waiting for the player to be in a game
        ULONGLONG g_in_game_since = 0;

        constexpr ULONGLONG k_settle_ms = 1500;
    } // namespace

    void apply_mutator_line(const char* text)
    {
        unsigned generation = 0;
        int announce = 0;
        if (std::sscanf(text + sizeof(k_mutator_marker) - 1, "%u %d", &generation, &announce) != 2)
            return;
        const char* bar = std::strchr(text, '|');
        Wanted wanted;
        std::string titles;
        for (const char* entry = bar != nullptr ? bar + 1 : ""; *entry != '\0' && *entry != '\n';)
        {
            char id[16] = {};
            int value = 0, red = -1, green = -1, blue = -1; // (the fog's density per mille, hunted's class)
            const int read = std::sscanf(entry, "%15[^ ;\n] %d", id, &value);
            if (read == 2 && std::strcmp(id, "fog") == 0)
            {
                wanted.fog = true;
                wanted.density = static_cast<float>(value) / 1000.f;
                const char* rest = std::strchr(entry, ' ');
                rest = rest != nullptr ? std::strchr(rest + 1, ' ') : nullptr;
                if (rest != nullptr && std::sscanf(rest + 1, "%d %d %d", &red, &green, &blue) == 3)
                    wanted.color = std::to_string(red) + " " + std::to_string(green) + " " + std::to_string(blue);
            }
            if (const inv::MutatorInfo* info = inv::find_mutator(id))
            {
                titles += (titles.empty() ? "" : ", ") + std::string(info->title);
                if (read == 2 && std::strcmp(id, "hunted") == 0 && value >= 1 && value <= 6)
                    titles += std::string(" (") + k_class_plurals[value] + ")";
            }
            const char* next = std::strchr(entry, ';');
            if (next == nullptr)
                break;
            entry = next + 1;
        }
        g_wanted = wanted;
        {
            Session& s = local_session();
            std::lock_guard lock(s.mutex);
            s.mutators = titles;
        }
        if (!g_seen || generation != g_seen_generation)
        {
            g_seen = true;
            g_seen_generation = generation;
            g_announce = announce != 0 && !titles.empty() ? "Mutator: " + titles : "";
            INV_LOG("mutators: the server's chapter has %s", titles.empty() ? "none" : titles.c_str());
        }
    }

    void mutators_client_tick()
    {
        if (client_local_player() == nullptr)
        {
            g_in_game_since = 0;
            return;
        }
        const ULONGLONG now = GetTickCount64();
        if (g_in_game_since == 0)
            g_in_game_since = now;
        if (now - g_in_game_since < k_settle_ms)
            return;
        if (!(g_wanted == g_applied))
        {
            if (g_wanted.fog)
                set_fog(true, g_wanted.density, g_wanted.color);
            else if (g_applied.fog)
                set_fog(false, 0.f);
            g_applied = g_wanted;
        }
        if (!g_announce.empty())
        {
            notify(g_announce);
            g_announce.clear();
        }
    }

    void mutators_client_left()
    {
        if (g_applied.fog)
        {
            INV_LOG("mutators: not in a game: the fog is off");
            set_fog(false, 0.f);
        }
        g_applied = {};
        g_wanted = {};
        g_in_game_since = 0;
    }

} // namespace game::detail
