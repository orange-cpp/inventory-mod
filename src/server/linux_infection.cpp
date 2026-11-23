#include "server/linux_game.hpp"
#include "game/infection_sounds.hpp"
#include "inv/rules.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <random>

#include "eiface.h"
#include "game/server/iplayerinfo.h"
#include "igameevents.h"
#include "toolframework/itoolentity.h"

namespace game
{
    using linux_memory::field;
    using namespace std::chrono_literals;

    namespace
    {
        std::minstd_rand& infection_random()
        {
            static std::minstd_rand random(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
            return random;
        }

        std::chrono::milliseconds voice_delay(float seconds, float minimum)
        {
            const float spread = std::uniform_real_distribution<float>(0.6f, 1.4f)(infection_random());
            return std::chrono::milliseconds(static_cast<int>(1000.f * std::max(minimum, seconds * spread)));
        }
    }

    bool LinuxGame::install_infection()
    {
        if (!inv::infection_rules().enabled && !inv::mutator_rules().enabled)
            return true;
        return ammo_game_ == this && commit_suicide_ != nullptr && spawn_special_ != nullptr &&
               rescue_hook_.install(rescue_call_, rescue_call_hook);
    }

    void LinuxGame::uninstall_infection()
    {
        clear_infection(true);
        if (!rescue_hook_.remove())
            std::fprintf(stderr, "inventory_mod: could not restore the rescue closet handler\n");
    }

    CBaseEntity* LinuxGame::infection_player(int user_id, edict_t* edict, std::uint32_t handle) const
    {
        if (edict == nullptr || edict->IsFree() || engine_->GetPlayerUserId(edict) != user_id)
            return nullptr;
        auto* player = entities_->EdictToBaseEntity(edict);
        return player != nullptr && field<std::uint32_t>(player, linux_sdk::entity_handle) == handle ? player : nullptr;
    }

    bool LinuxGame::tell_infection(int user_id, const Infection& infection, bool turned, bool clear)
    {
        if (infection_player(user_id, infection.edict, infection.player) == nullptr || !inventory_lookup_(infection.edict))
            return false;
        const int remaining = clear ? 0 : static_cast<int>(std::max(0ms,
            std::chrono::duration_cast<std::chrono::milliseconds>(infection.until - InfectionClock::now())).count());
        char line[64];
        std::snprintf(line, sizeof(line), "@inf@ %d %d %d\n", remaining, clear ? 0 : infection.total_ms, turned ? 1 : 0);
        engine_->ClientPrintf(infection.edict, line);
        return true;
    }

    void LinuxGame::infect(edict_t* edict)
    {
        const auto& rules = inv::infection_rules();
        if (!rules.enabled || !rescue_hook_.original() || edict == nullptr)
            return;
        auto* info = players_->GetPlayerInfo(edict);
        if (info == nullptr || !info->IsConnected() || info->GetTeamIndex() != 2 ||
            info->IsDead() || (!rules.bots && info->IsFakeClient()))
            return;
        auto* player = entities_->EdictToBaseEntity(edict);
        const int user_id = engine_->GetPlayerUserId(edict);
        if (player == nullptr || user_id < 1 || turning_.contains(user_id))
            return;
        const auto handle = field<std::uint32_t>(player, linux_sdk::entity_handle);
        const auto now = InfectionClock::now();
        auto entry = infections_.find(user_id);
        if (entry == infections_.end() || entry->second.player != handle)
            entry = infections_.insert_or_assign(user_id, Infection{
                edict, handle, now + std::chrono::seconds(rules.seconds), rules.seconds * 1000,
                now + voice_delay(26.f, 2.5f), std::nullopt}).first;
        else
            entry->second.until = std::max(now + 1s, entry->second.until - std::chrono::seconds(rules.hit_seconds));
        entry->second.told = tell_infection(user_id, entry->second, false, false);
    }

    void LinuxGame::infection_pills(int user_id)
    {
        const auto entry = infections_.find(user_id);
        if (entry == infections_.end())
            return;
        if (inv::infection_rules().no_cure)
        {
            entry->second.until = InfectionClock::now() + std::chrono::milliseconds(entry->second.total_ms);
            entry->second.next_insane.reset();
            entry->second.told = tell_infection(user_id, entry->second, false, false);
        }
        else
            forget_infection(user_id);
    }

    void LinuxGame::forget_infection(int user_id)
    {
        const auto entry = infections_.find(user_id);
        if (entry != infections_.end())
        {
            tell_infection(user_id, entry->second, false, true);
            infections_.erase(entry);
        }
    }

    void LinuxGame::clear_infection(bool notify, bool reset_round)
    {
        if (notify)
            for (const auto& [user_id, infection] : infections_)
                tell_infection(user_id, infection, false, true);
        infections_.clear();
        if (reset_round)
        {
            turning_.clear();
            no_rescue_.clear();
            infection_corpses_.clear();
        }
    }

    void LinuxGame::infection_event(IGameEvent& event, edict_t* edicts, int clients)
    {
        const std::string_view name(event.GetName());
        if (name == "round_start" || name == "map_transition")
        {
            clear_infection(true);
            return;
        }
        if (name == "round_end" || name == "mission_lost" || name == "finale_vehicle_leaving" || name == "finale_win")
        {
            // End the countdowns now. The current map still owns its bodies and rescue closets until the next round.
            clear_infection(true, false);
            return;
        }
        if (!inv::infection_rules().enabled || !rescue_hook_.original())
            return;
        const auto edict_for_user = [&](int user_id) -> edict_t* {
            if (user_id > 0 && edicts != nullptr)
                for (int index = 1; index <= clients; ++index)
                    if (!(edicts + index)->IsFree() && engine_->GetPlayerUserId(edicts + index) == user_id)
                        return edicts + index;
            return nullptr;
        };
        if (name == "tongue_grab" || name == "lunge_pounce" || name == "jockey_ride" ||
            name == "charger_carry_start" || name == "charger_pummel_start")
            infect(edict_for_user(event.GetInt("victim")));
        else if (name == "player_now_it" && (event.GetBool("by_boomer") || event.GetBool("exploded")))
            infect(edict_for_user(event.GetInt("userid")));
        else if (name == "player_incapacitated")
        {
            bool infected = false;
            if (auto* attacker = edict_for_user(event.GetInt("attacker")))
            {
                auto* info = players_->GetPlayerInfo(attacker);
                infected = info != nullptr && info->GetTeamIndex() == 3;
            }
            else if (const int attacker_index = event.GetInt("attackerentid"); attacker_index > 0 && attacker_index < 4096)
            {
                for (void* current = tools_->FirstEntity(); current != nullptr; current = tools_->NextEntity(current))
                {
                    auto* entity = static_cast<CBaseEntity*>(current);
                    if ((field<std::uint32_t>(entity, linux_sdk::entity_handle) & 0xfffu) != static_cast<unsigned>(attacker_index))
                        continue;
                    const std::string_view classname(classname_(entity));
                    infected = classname == "infected" || classname == "witch";
                    break;
                }
            }
            if (infected)
                infect(edict_for_user(event.GetInt("userid")));
        }
        else if (name == "pills_used")
        {
            const int subject = event.GetInt("subject");
            infection_pills(subject > 0 ? subject : event.GetInt("userid"));
        }
        else if (name == "player_death")
            forget_infection(event.GetInt("userid"));
    }

    void LinuxGame::infection_voice(const Infection& infection, CBaseEntity* player, bool cough, float stage) const
    {
        if (!precache_sound_ || !emit_sound_)
            return;
        auto* info = players_->GetPlayerInfo(infection.edict);
        const char* model = info == nullptr ? nullptr : info->GetModelName();
        if (model == nullptr)
            return;
        const char* sound = nullptr;
        char cough_name[64];
        auto& random = infection_random();
        if (cough)
        {
            struct CoughVoice { const char* model; const char* prefix; int count; };
            constexpr std::array voices{
                CoughVoice{"coach", "Coach", 8}, CoughVoice{"gambler", "Gambler", 4},
                CoughVoice{"mechanic", "Mechanic", 4}, CoughVoice{"producer", "Producer", 7},
                CoughVoice{"biker", "Player.Biker", 7}, CoughVoice{"manager", "Player.Manager", 5},
                CoughVoice{"namvet", "Player.NamVet", 6}, CoughVoice{"teenangst", "Player.TeenGirl", 6}};
            for (const auto& voice : voices)
                if (std::strstr(model, voice.model) != nullptr)
                {
                    std::snprintf(cough_name, sizeof(cough_name), "%s_Cough0%d", voice.prefix,
                                  std::uniform_int_distribution<int>(1, voice.count)(random));
                    sound = cough_name;
                    break;
                }
        }
        else
        {
            for (const auto& voice : detail::insane_voices())
                if (std::strstr(model, voice.model) != nullptr)
                {
                    const auto& sounds = std::bernoulli_distribution(0.3 + 0.4 * stage)(random) ? voice.screams : voice.laughs;
                    sound = sounds[random() % sounds.size()];
                    break;
                }
        }
        if (sound != nullptr)
        {
            precache_sound_(player, sound);
            emit_sound_(player, sound, 0.f, nullptr);
        }
    }

    void LinuxGame::infect_turn(int user_id, const Infection& infection)
    {
        auto* player = infection_player(user_id, infection.edict, infection.player);
        if (player == nullptr || field<unsigned char>(player, linux_sdk::life_state) != 0)
            return;
        const auto& origin = origin_(player);
        turning_.emplace(user_id, TurningSurvivor{
            user_id, infection.edict, infection.player, {origin.x, origin.y, origin.z},
            field<int>(player, linux_sdk::survivor_character), InfectionClock::now() + 1s});
        no_rescue_.insert(user_id);
        tell_infection(user_id, infection, true, true);
        commit_suicide_(player, false, true);
    }

    void LinuxGame::resolve_turns(InfectionClock::time_point now)
    {
        std::vector<TurningSurvivor> completed;
        for (auto it = turning_.begin(); it != turning_.end();)
        {
            const auto& turn = it->second;
            auto* player = infection_player(turn.user_id, turn.edict, turn.player);
            if (player != nullptr && field<unsigned char>(player, linux_sdk::life_state) != 0)
            {
                completed.push_back(turn);
                it = turning_.erase(it);
            }
            else if (player == nullptr || now >= turn.deadline)
            {
                no_rescue_.erase(turn.user_id);
                it = turning_.erase(it);
            }
            else
                ++it;
        }
        for (const auto& turn : completed)
        {
            infection_corpses_.push_back({turn.position, turn.character, now + 3s});
            auto* manager = *reinterpret_cast<void* const*>(server_base_ + linux_sdk::zombie_manager);
            const auto& specials = inv::infection_rules().specials;
            const int type = specials.empty() ? 3 : specials[infection_random()() % specials.size()];
            const Vector position(turn.position[0], turn.position[1], turn.position[2] + 4.f);
            const QAngle facing(0.f, 0.f, 0.f);
            if (manager != nullptr && spawn_special_(manager, type, position, facing))
                std::fprintf(stderr, "inventory_mod: player %d turned into special infected %d\n", turn.user_id, type);
        }
    }

    void LinuxGame::remove_infection_corpses(InfectionClock::time_point now)
    {
        std::erase_if(infection_corpses_, [&](const InfectionCorpse& corpse) {
            CBaseEntity* nearest = nullptr;
            float nearest_distance = 250.f * 250.f;
            for (void* current = tools_->FirstEntity(); current != nullptr; current = tools_->NextEntity(current))
            {
                auto* entity = static_cast<CBaseEntity*>(current);
                if (std::strcmp(classname_(entity), "survivor_death_model") != 0 ||
                    marked_for_deletion_(network_property_(entity)) ||
                    field<int>(entity, linux_sdk::corpse_character) != corpse.character)
                    continue;
                const auto& origin = origin_(entity);
                const float x = origin.x - corpse.position[0], y = origin.y - corpse.position[1], z = origin.z - corpse.position[2];
                const float distance = x * x + y * y + z * z;
                if (distance < nearest_distance)
                {
                    nearest = entity;
                    nearest_distance = distance;
                }
            }
            if (nearest != nullptr)
            {
                remove_(nearest);
                std::fprintf(stderr, "inventory_mod: removed the turned survivor's body\n");
                return true;
            }
            return now >= corpse.deadline;
        });
    }

    void LinuxGame::infection_frame()
    {
        const auto& rules = inv::infection_rules();
        const auto now = InfectionClock::now();
        std::vector<std::pair<int, Infection>> expired;
        for (auto it = infections_.begin(); it != infections_.end();)
        {
            auto& [user_id, state] = *it;
            auto* player = infection_player(user_id, state.edict, state.player);
            auto* info = player == nullptr ? nullptr : players_->GetPlayerInfo(state.edict);
            if (info == nullptr || !info->IsConnected() || info->GetTeamIndex() != 2 ||
                field<unsigned char>(player, linux_sdk::life_state) != 0)
            {
                tell_infection(user_id, state, false, true);
                it = infections_.erase(it);
                continue;
            }
            if (now >= state.until)
            {
                expired.emplace_back(user_id, state);
                it = infections_.erase(it);
                continue;
            }
            if (!state.told)
                state.told = tell_infection(user_id, state, false, false);
            const float remaining = std::chrono::duration<float, std::milli>(state.until - now).count();
            const float severity = std::clamp(1.f - remaining / static_cast<float>(state.total_ms), 0.f, 1.f);
            if (rules.cough && now >= state.next_cough)
            {
                infection_voice(state, player, true, severity);
                state.next_cough = now + voice_delay(26.f - 22.f * severity, 2.5f);
                if (state.next_insane)
                    state.next_insane = std::max(*state.next_insane, now + 2500ms);
            }
            if (!rules.insane || severity * 100.f < rules.insane_at)
                state.next_insane.reset();
            else if (!state.next_insane)
                state.next_insane = now + std::chrono::milliseconds(std::uniform_int_distribution<int>(2000, 5999)(infection_random()));
            else if (now >= *state.next_insane)
            {
                const float stage = rules.insane_at >= 100 ? 1.f :
                    (severity * 100.f - rules.insane_at) / (100.f - rules.insane_at);
                infection_voice(state, player, false, stage);
                state.next_insane = now + voice_delay(18.f - 13.f * stage, 3.f);
                state.next_cough = std::max(state.next_cough, now + 3s);
            }
            ++it;
        }
        for (const auto& [user_id, state] : expired)
            infect_turn(user_id, state);
        resolve_turns(now);
        remove_infection_corpses(now);
    }

    void LinuxGame::rescue_call_hook(CBaseEntity* rescue, CBaseEntity* survivor)
    {
        auto& game = *ammo_game_;
        if (survivor != nullptr && !game.no_rescue_.empty())
            if (auto* edict = game.entities_->BaseEntityToEdict(survivor);
                edict != nullptr && game.no_rescue_.contains(game.engine_->GetPlayerUserId(edict)))
                survivor = nullptr;
        game.rescue_hook_.original()(rescue, survivor);
    }
}
