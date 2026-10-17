// [infection]: infected by the infected, a survivor turns when the clock runs out.

#include "game/infection.hpp"

#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/lifecycle.hpp"
#include "game/players.hpp"
#include "inv/rules.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string_view>
#include <vector>

namespace game::detail
{

    // ---- the infection ([infection] in the rules) -----------------------------------------------------------------
    //
    // A survivor knocked down by the infected, grabbed by a special or vomited on by a boomer is infected; a new hit
    // takes time off the clock, pain pills reset it.  When it runs out the survivor dies (CommitSuicide) and a random
    // special infected rises where they stood (the game's VScript ZSpawn); they cannot be revived before the next
    // chapter: their body (survivor_death_model) is removed, so no defibrillator, and no rescue closet calls for them
    // (CSurvivorRescue::SetSurvivor is told "nobody").  All of it runs on the server's main thread: game events
    // through IGameEventManager2::FireEvent, the clock in IServerGameDLL::GameFrame.
    //
    //   engine.dll IGameEventManager2 slot 7 FireEvent(IGameEvent*, bool), 0x10180830: logs "Game Event Fired: %s"
    //       with the event's GetName.  CGameEvent (vtable 0x103881E4): slot 1 GetName(), slot 6 GetInt(key, default)
    //       (both forward to the event's KeyValues at +8).
    //   engine.dll VEngineServer022 slot 16 GetPlayerUserId(edict), 0x10131FB0: the client of that edict's +0x38.
    //   server.dll ServerGameDLL005 (CServerGameDLL, vtable 0x1060C904) slot 4 GameFrame(bool), 0x100EF4B0 (slot 3
    //       takes three arguments, ServerActivate; 4 and 5 take one bool: GameFrame, PreClientUpdate).
    //   CTerrorPlayer slot 476 CommitSuicide(bool explode, bool force), 0x10174210: what the `kill` command calls
    //       (0x1008EAB0: `call [vtable + 0x770]`).
    //   server.dll 0x1021CB90: the `script` command's handler (cdecl, const CCommand&; the command is FCVAR_CHEAT, its
    //       handler is not): it runs what follows "script " in the command's text in the server's VScript VM.
    //   server.dll 0x102CEAF0 CSurvivorRescue::SetSurvivor(CTerrorPlayer*): a rescue closet starts (or with nullptr
    //       stops) calling for that dead survivor ("(RESCUE) Rescue spot at ... activated").
    //   CBaseEntity m_iTeamNum +0x238 (2 survivors, 3 infected), CBasePlayer m_lifeState +0xF0 (0 alive).

    core::Hook<SetRescueSurvivorFn> g_rescue_set_survivor;

    ScriptCommandFn g_script_command = nullptr;

    struct Infection
    {
        ULONGLONG until = 0;
        int total_ms = 0;
        ULONGLONG next_cough = 0; // [infection] cough
        ULONGLONG next_insane = 0; // [infection] insane: the next laugh or scream (0: not that far gone yet)
    };
    std::map<int, Infection> g_infections; // by userid
    std::vector<int> g_no_rescue; // userids that died of it: no defibrillator, no rescue closet before the next chapter
    struct BodyRemoval
    {
        Vec3 at;
        ULONGLONG when;
        int tries;
    };
    std::vector<BodyRemoval> g_body_removals;

    bool contains(const std::vector<int>& list, int value)
    {
        return std::find(list.begin(), list.end(), value) != list.end();
    }

    // Runs VScript in the server's VM through the `script` command's handler, with a CCommand made here: argc at +0,
    // the whole line at +8 (the handler reads past "script "), argv[] at +0x408 (it checks argv[1]).
    bool run_script(const std::string& code)
    {
        if (g_script_command == nullptr || code.size() > 480)
            return false;
        struct Command
        {
            int argc;
            int argv0_size;
            char args[512];
            char argv_buffer[512];
            const char* argv[64];
        };
        auto command = std::make_unique<Command>();
        std::memset(command.get(), 0, sizeof(Command));
        std::snprintf(command->args, sizeof(command->args), "script %s", code.c_str());
        std::snprintf(command->argv_buffer, sizeof(command->argv_buffer), "script");
        command->argc = 2;
        command->argv0_size = 6;
        command->argv[0] = command->argv_buffer;
        command->argv[1] = command->args + 7;
        return mem::guarded([&] { g_script_command(command.get()); });
    }

    // The infection clock to the player: the host's own session, or a line their client DLL reads
    // ("@inf@ <ms left> <ms in all> <1: turned now>").
    void tell_infection(void* player, const Infection* infection, bool turned = false)
    {
        const ULONGLONG now = GetTickCount64();
        const int remaining = infection != nullptr && infection->until > now ? static_cast<int>(infection->until - now) : 0;
        const int total = infection != nullptr ? infection->total_ms : 0;
        if (player == host_player())
        {
            Session& s = local_session();
            std::lock_guard lock(s.mutex);
            s.infection_until = remaining > 0 ? now + static_cast<ULONGLONG>(remaining) : 0;
            s.infection_total_ms = total;
            if (turned)
                s.infection_turned_at = now;
            return;
        }
        ServerPlayer* sp = server_player(player);
        if (sp == nullptr || !sp->has_mod || g_client_printf == nullptr)
            return;
        char line[64];
        std::snprintf(line, sizeof(line), "%s %d %d %d\n", k_infection_marker, remaining, total, turned ? 1 : 0);
        void* edict = edict_of(player);
        if (edict != nullptr)
            mem::guarded([&] { g_client_printf(g_engine_server, edict, line); });
    }

    ULONGLONG cough_delay(float severity);

    void infect(void* player, const char* how)
    {
        const inv::InfectionRules& rules = inv::infection_rules();
        if (!rules.enabled)
            return;
        const char* skip = player == nullptr ? "no such player"
                           : team_of(player) != 2 ? "not a survivor"
                           : !alive(player)       ? "dead"
                           : !rules.bots && is_bot(player) ? "a bot (bots = false)"
                                                            : nullptr;
        const int user_id = player != nullptr ? user_id_of(player) : -1;
        if (skip == nullptr && user_id <= 0)
            skip = "no userid";
        if (skip != nullptr)
        {
            INV_LOG("infection: %s not infected (%s): %s", how, skip, player != nullptr ? "" : "the event's victim was not found");
            return;
        }
        const ULONGLONG now = GetTickCount64();
        Infection& infection = g_infections[user_id];
        if (infection.until == 0)
        {
            infection.total_ms = rules.seconds * 1000;
            infection.until = now + static_cast<ULONGLONG>(infection.total_ms);
            infection.next_cough = now + cough_delay(0.f);
            INV_LOG("infection: userid %d infected (%s), %d s", user_id, how, rules.seconds);
        }
        else
        {
            const ULONGLONG cut = static_cast<ULONGLONG>(rules.hit_seconds) * 1000;
            infection.until = infection.until > now + cut + 1000 ? infection.until - cut : now + 1000;
            INV_LOG("infection: userid %d hit again (%s), %llu s left", user_id, how, (infection.until - now) / 1000);
        }
        tell_infection(player, &infection);
    }

    // Pain pills: the clock back to its full time (no_cure, the default), or cured.
    void cure(void* player, const char* how)
    {
        const int user_id = user_id_of(player);
        const auto it = g_infections.find(user_id);
        if (it == g_infections.end())
            return;
        if (inv::infection_rules().no_cure)
        {
            it->second.until = GetTickCount64() + static_cast<ULONGLONG>(it->second.total_ms);
            it->second.next_insane = 0;
            INV_LOG("infection: userid %d took pills (%s): the clock is back to %d s", user_id, how, it->second.total_ms / 1000);
            tell_infection(player, &it->second);
            return;
        }
        g_infections.erase(it);
        INV_LOG("infection: userid %d cured (%s)", user_id, how);
        tell_infection(player, nullptr);
    }

    // How bad it is: 0 just infected .. 1 turning.
    float severity_of(const Infection& infection, ULONGLONG now)
    {
        if (infection.total_ms <= 0 || infection.until <= now)
            return 1.f;
        return std::clamp(1.f - static_cast<float>(infection.until - now) / static_cast<float>(infection.total_ms), 0.f, 1.f);
    }

    // The infected cough now and then, more often as the clock runs out: about every 26 s at first, every 4 s at
    // the end, each wait 60 .. 140 % of that.
    ULONGLONG cough_delay(float severity)
    {
        static std::minstd_rand random(static_cast<unsigned>(GetTickCount64()));
        const float mean = 26.f - 22.f * severity;
        const float spread = 0.6f + 0.8f * static_cast<float>(random() % 1000u) / 1000.f;
        return static_cast<ULONGLONG>(std::max(2.5f, mean * spread) * 1000.f);
    }

    // One of the survivor's own coughs (the soundscripts they cough with in smoke), played on them for everyone to
    // hear.  The survivor is told apart by their model: survivor_coach.mdl -> Coach_Cough01..08, the L4D1 four's
    // entries start with "Player.".  (VScript: GetPlayerFromUserID, GetModelName, PrecacheScriptSound, EmitSoundOn,
    // all in server.dll 2.2.4.3.)  One script line (run_script takes at most 480 characters; a function defined
    // with "::name <- function" by one line was not there for the next, so it is all in one).  An error is written
    // to left4dead2/ems/inv_cough_error.txt.
    void cough(int user_id)
    {
        char code[512];
        const int n = std::snprintf(
            code, sizeof(code),
            "try{local p=GetPlayerFromUserID(%d);if(p){local m=p.GetModelName();"
            "foreach(s in [[\"coac\",\"Coach\",8],[\"gamb\",\"Gambler\",4],[\"mech\",\"Mechanic\",4],[\"prod\",\"Producer\",7],"
            "[\"bike\",\"Player.Biker\",7],[\"mana\",\"Player.Manager\",5],[\"namv\",\"Player.NamVet\",6],"
            "[\"teen\",\"Player.TeenGirl\",6]])if(m.find(s[0])!=null){local n=s[1]+\"_Cough0\"+RandomInt(1,s[2]);"
            "p.PrecacheScriptSound(n);EmitSoundOn(n,p);break;}}}"
            "catch(e){StringToFile(\"inv_cough_error.txt\",\"\"+e)}",
            user_id);
        if (n <= 0 || n >= 480 || !run_script(code))
            INV_LOG("infection: userid %d could not cough (the script failed)", user_id);
    }

    // The last stage ([infection] insane, from insane_at % of the clock gone): the survivor goes insane, laughing
    // for no reason or screaming (their death screams, and "get it off!" at a special that is not there), about
    // every 18 s at first and every 5 s at the end, screams more often as it gets worse.  The soundscripts are
    // the survivors' own (scripts/game_sounds_<survivor>.txt: "<Hearty / Short / Nervous laugh>", "[Loud painful
    // / Short quick death scream]", ScreamWhilePounced); the survivor is told apart by their model:
    // CBaseEntity::m_ModelName +0x24C (datamap "m_ModelName", FIELD_MODELNAME 18: a pointer to
    // "models/survivors/survivor_gambler.mdl" ...; IServerTools GetKeyValue "model" gives that pointer's bytes, not
    // the text).  An error goes to left4dead2/ems/inv_insane_error.txt.
    struct InsaneVoice
    {
        const char* model; // a part of the model's name
        std::vector<const char*> laughs, screams;
    };
    const std::vector<InsaneVoice>& insane_voices()
    {
        static const std::vector<InsaneVoice> voices = {
            {"coach",
             {"Coach_Laughter01", "Coach_Laughter04", "Coach_Laughter06", "Coach_Laughter07", "Coach_Laughter13",
              "Coach_Laughter14", "Coach_Laughter22"},
             {"Coach_DeathScream01", "Coach_DeathScream02", "Coach_DeathScream03", "Coach_DeathScream04",
              "Coach_DeathScream05", "Coach_ScreamWhilePounced01", "Coach_ScreamWhilePounced03",
              "Coach_ScreamWhilePounced05"}},
            {"gambler",
             {"Gambler_Laughter01", "Gambler_Laughter03", "Gambler_Laughter06", "Gambler_Laughter15",
              "Gambler_Laughter16", "Gambler_Laughter17"},
             {"Gambler_DeathScream01", "Gambler_DeathScream02", "Gambler_DeathScream03", "Gambler_DeathScream05",
              "Gambler_ScreamWhilePounced01", "Gambler_ScreamWhilePounced03", "Gambler_ScreamWhilePounced05"}},
            {"mechanic",
             {"Mechanic_Laughter04", "Mechanic_Laughter05", "Mechanic_Laughter06", "Mechanic_Laughter09",
              "Mechanic_Laughter13b", "Mechanic_Laughter13c", "Mechanic_Laughter14"},
             {"Mechanic_DeathScream01", "Mechanic_DeathScream02", "Mechanic_DeathScream03", "Mechanic_DeathScream04",
              "Mechanic_ScreamWhilePounced01", "Mechanic_ScreamWhilePounced04", "Mechanic_ScreamWhilePounced04a"}},
            {"producer",
             {"Producer_Laughter01", "Producer_Laughter04", "Producer_Laughter12", "Producer_Laughter13",
              "Producer_Laughter14", "Producer_Laughter17"},
             {"Producer_DeathScream01", "Producer_DeathScream02", "Producer_ScreamWhilePounced01",
              "Producer_ScreamWhilePounced02", "Producer_ScreamWhilePounced03", "Producer_ScreamWhilePounced05"}},
            {"biker",
             {"npc.Biker_Laughter01", "npc.Biker_Laughter02", "npc.Biker_Laughter04", "npc.Biker_Laughter05",
              "npc.Biker_Laughter06", "npc.Biker_Laughter12", "npc.Biker_Laughter13"},
             {"Player.Biker_DeathScream06", "Player.Biker_DeathScream07", "Player.Biker_DeathScream08",
              "Player.Biker_DeathScream09", "Player.Biker_ScreamWhilePounced01", "Player.Biker_ScreamWhilePounced02",
              "Player.Biker_ScreamWhilePounced04"}},
            {"manager",
             {"npc.Manager_Laughter01", "npc.Manager_Laughter02", "npc.Manager_Laughter04", "npc.Manager_Laughter05",
              "npc.Manager_Laughter13", "npc.Manager_Laughter14", "npc.Manager_Laughter20"},
             {"Player.Manager_DeathScream02", "Player.Manager_DeathScream03", "Player.Manager_DeathScream04",
              "Player.Manager_DeathScream06", "Player.Manager_ScreamWhilePounced01", "Player.Manager_ScreamWhilePounced02"}},
            {"namvet",
             {"npc.NamVet_Laughter01", "npc.NamVet_Laughter02", "npc.NamVet_Laughter04", "npc.NamVet_Laughter05",
              "npc.NamVet_Laughter06", "npc.NamVet_Laughter07"},
             {"Player.NamVet_DeathScream01", "Player.NamVet_DeathScream03", "Player.NamVet_DeathScream04",
              "Player.NamVet_DeathScream05", "Player.NamVet_ScreamWhilePounced01", "Player.NamVet_ScreamWhilePounced03"}},
            {"teenangst",
             {"npc.TeenGirl_Laughter02", "npc.TeenGirl_Laughter04", "npc.TeenGirl_Laughter05", "npc.TeenGirl_Laughter09",
              "npc.TeenGirl_Laughter10", "npc.TeenGirl_Laughter12", "npc.TeenGirl_Laughter13"},
             {"Player.TeenGirl_DeathScream05", "Player.TeenGirl_DeathScream07", "Player.TeenGirl_DeathScream09",
              "Player.TeenGirl_DeathScream10", "Player.TeenGirl_ScreamWhilePounced01",
              "Player.TeenGirl_ScreamWhilePounced02", "Player.TeenGirl_ScreamWhilePounced06"}},
        };
        return voices;
    }

    std::minstd_rand& insane_random()
    {
        static std::minstd_rand random(static_cast<unsigned>(GetTickCount64() * 2654435761u));
        return random;
    }

    // stage: 0 when it starts (insane_at) .. 1 turning
    ULONGLONG insane_delay(float stage)
    {
        const float mean = 18.f - 13.f * stage;
        const float spread = 0.6f + 0.8f * static_cast<float>(insane_random()() % 1000u) / 1000.f;
        return static_cast<ULONGLONG>(std::max(3.f, mean * spread) * 1000.f);
    }

    void go_insane(void* player, int user_id, float stage)
    {
        char model[128] = {};
        const char* name = mem::read_or<const char*>(reinterpret_cast<std::uintptr_t>(player) + k_off_model_name);
        if (name == nullptr || !mem::safe_copy(model, name, sizeof(model) - 1))
            return;
        for (char* c = model; *c != '\0'; ++c)
            *c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
        const InsaneVoice* voice = nullptr;
        for (const InsaneVoice& v : insane_voices())
            if (std::strstr(model, v.model) != nullptr)
                voice = &v;
        if (voice == nullptr)
        {
            INV_LOG("infection: userid %d cannot go insane: no voice for the model \"%s\"", user_id, model);
            return;
        }
        std::minstd_rand& random = insane_random();
        const bool scream = static_cast<float>(random() % 1000u) / 1000.f < 0.3f + 0.4f * stage;
        const std::vector<const char*>& list = scream ? voice->screams : voice->laughs;
        const char* sound = list[random() % list.size()];
        char code[256];
        std::snprintf(code, sizeof(code),
                      "try{local p=GetPlayerFromUserID(%d);if(p){p.PrecacheScriptSound(\"%s\");EmitSoundOn(\"%s\",p);}}"
                      "catch(e){StringToFile(\"inv_insane_error.txt\",\"\"+e)}",
                      user_id, sound, sound);
        const bool ok = run_script(code);
        INV_LOG("infection: userid %d goes insane: %s (%s, stage %.0f%%)", user_id, sound, ok ? "played" : "the script failed",
                stage * 100.f);
    }

    // The clock ran out: the survivor dies and a random special rises where they stood.
    void turn(void* player, int user_id)
    {
        Vec3 at{};
        mem::read(reinterpret_cast<std::uintptr_t>(player) + k_off_origin, at);
        g_infections.erase(user_id);
        tell_infection(player, nullptr, true);
        const auto suicide = reinterpret_cast<CommitSuicideFn>(mem::vslot(player, k_vt_commit_suicide));
        if (suicide != nullptr)
            mem::guarded([&] { suicide(player, false, true); });
        if (alive(player))
        {
            INV_LOG("infection: userid %d did not die (CommitSuicide refused)", user_id);
            return;
        }
        if (!contains(g_no_rescue, user_id))
            g_no_rescue.push_back(user_id);
        g_body_removals.push_back({at, GetTickCount64() + 200, 0});
        const std::vector<int>& specials = inv::infection_rules().specials;
        const int type = specials.empty() ? 3 : specials[static_cast<std::size_t>(GetTickCount64() % specials.size())];
        // (seen in the game: ZSpawn answered true, by a StringToFile of its result into left4dead2/ems)
        char code[160];
        std::snprintf(code, sizeof(code), "ZSpawn({type = %d, pos = Vector(%.1f, %.1f, %.1f)})", type, at.x, at.y, at.z + 4.f);
        const bool spawned = run_script(code);
        INV_LOG("infection: userid %d turned at %.0f %.0f %.0f: %s (%s)", user_id, at.x, at.y, at.z, code,
                spawned ? "run" : "failed");
    }

    // The body a defibrillator would use: survivor_death_model, the one nearest to where they died.
    void remove_bodies(ULONGLONG now)
    {
        for (auto it = g_body_removals.begin(); it != g_body_removals.end();)
        {
            if (it->when > now)
            {
                ++it;
                continue;
            }
            void* best = nullptr;
            float best_distance = 250.f;
            for (int index = 1; index < 0x1000; ++index)
            {
                void* entity = entity_at_index(index);
                char name[32];
                if (entity == nullptr || !classname_of(entity, name, sizeof(name)) || std::strcmp(name, "survivor_death_model") != 0)
                    continue;
                Vec3 origin{};
                if (!mem::read(reinterpret_cast<std::uintptr_t>(entity) + k_off_origin, origin))
                    continue;
                const float dx = origin.x - it->at.x, dy = origin.y - it->at.y, dz = origin.z - it->at.z;
                const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (distance < best_distance)
                {
                    best_distance = distance;
                    best = entity;
                }
            }
            if (best != nullptr && mem::guarded([&] { g.util_remove(best); }))
            {
                INV_LOG("infection: body removed (no defibrillator)");
                it = g_body_removals.erase(it);
            }
            else if (++it->tries >= 15)
            {
                INV_LOG("infection: no body found to remove");
                it = g_body_removals.erase(it);
            }
            else
                (it++)->when = now + 200;
        }
    }

    // Nobody is infected any more: a new round or map, the escape.  The host's own effect goes in any case (the host
    // may not be a player yet, or any more).
    void infection_all_clear(const char* why)
    {
        if (!g_infections.empty() || !g_no_rescue.empty())
            INV_LOG("infection: %s, all clear", why);
        for (const auto& [user_id, infection] : g_infections)
            if (void* player = player_by_user_id(user_id))
                tell_infection(player, nullptr);
        g_infections.clear();
        g_no_rescue.clear();
        g_body_removals.clear();
        if (!g_dedicated)
        {
            Session& s = local_session();
            std::lock_guard lock(s.mutex);
            s.infection_until = 0;
            s.infection_total_ms = 0;
        }
    }

    void __fastcall hk_rescue_set_survivor(void* rescue, void* edx, void* survivor)
    {
        if (survivor != nullptr && !g_no_rescue.empty() && contains(g_no_rescue, user_id_of(survivor)))
        {
            static ULONGLONG logged = 0;
            if (GetTickCount64() - logged > 10000)
            {
                logged = GetTickCount64();
                INV_LOG("infection: a rescue closet was going to call for a survivor who died of it: nobody instead");
            }
            survivor = nullptr;
        }
        g_rescue_set_survivor.original()(rescue, edx, survivor);
    }

    // The infection's game events: who is infected, cured, gone; all clear.
    void infection_on_event(const char* name, void* event, EventIntFn int_fn)
    {
        if (!inv::infection_rules().enabled)
            return;
        const auto get = [&](const char* key) { return int_fn(event, key, 0); };
        const std::string_view n(name);
        if (n == "tongue_grab" || n == "lunge_pounce" || n == "jockey_ride" || n == "charger_carry_start" ||
            n == "charger_pummel_start" || n == "player_now_it" || n == "player_incapacitated" || n == "pills_used")
            INV_LOG("infection: event %s (userid %d, victim %d, subject %d, attacker %d)", name, get("userid"),
                    get("victim"), get("subject"), get("attacker"));
        if (n == "tongue_grab" || n == "lunge_pounce" || n == "jockey_ride" || n == "charger_carry_start" ||
            n == "charger_pummel_start")
            infect(player_by_user_id(get("victim")), name);
        else if (n == "player_now_it" && (get("by_boomer") != 0 || get("exploded") != 0)) // a boomer's vomit or burst
            infect(player_by_user_id(get("userid")), name);
        else if (n == "player_incapacitated")
        {
            // by the infected: a special (a player on team 3), a common infected or a witch
            bool by_infected = false;
            if (void* attacker = player_by_user_id(get("attacker")))
                by_infected = team_of(attacker) == 3;
            else if (void* entity = entity_at_index(get("attackerentid")))
            {
                char cls[32];
                by_infected = classname_of(entity, cls, sizeof(cls)) &&
                              (std::strcmp(cls, "infected") == 0 || std::strcmp(cls, "witch") == 0);
            }
            if (by_infected)
                infect(player_by_user_id(get("userid")), name);
        }
        else if (n == "pills_used")
        {
            const int subject = get("subject");
            cure(player_by_user_id(subject > 0 ? subject : get("userid")), name);
        }
        else if (n == "player_death")
        {
            const int user_id = get("userid");
            if (g_infections.erase(user_id) != 0)
                if (void* player = player_by_user_id(user_id))
                    tell_infection(player, nullptr);
        }
        else if (n == "round_start" || n == "map_transition" || n == "round_end" || n == "mission_lost" ||
                 n == "finale_vehicle_leaving" || n == "finale_win") // (the last two: the escape, its cutscene)
            infection_all_clear(name);
    }

    // From GameFrame: the clocks, the coughs, the insane, turning, the bodies.
    void infection_frame()
    {
        // a new map without the events above (a new campaign from the menu): the game's clock went back
        static float last_time = 0.f;
        if (g_globals != nullptr)
        {
            const float time = game_time();
            if (time + 1.f < last_time && (!g_infections.empty() || !g_no_rescue.empty()))
                infection_all_clear("a new map");
            last_time = time;
        }
        if (g_infections.empty() && g_body_removals.empty())
            return;
        const ULONGLONG now = GetTickCount64();
        mem::guarded([&] {
            std::vector<int> due;
            for (auto& [user_id, infection] : g_infections)
            {
                if (infection.until <= now)
                {
                    due.push_back(user_id);
                    continue;
                }
                const inv::InfectionRules& rules = inv::infection_rules();
                const float severity = severity_of(infection, now);
                if (rules.cough && infection.next_cough <= now)
                {
                    infection.next_cough = now + cough_delay(severity);
                    if (void* player = player_by_user_id(user_id); player != nullptr && alive(player))
                    {
                        cough(user_id);
                        infection.next_insane = std::max(infection.next_insane, now + 2500); // not over each other
                        INV_LOG("infection: userid %d coughs (%.0f%%, the next in %llu s)", user_id, severity * 100.f,
                                (infection.next_cough - now) / 1000);
                    }
                }
                // the last stage: insane (see go_insane)
                if (!rules.insane || severity * 100.f < static_cast<float>(rules.insane_at))
                    infection.next_insane = 0;
                else if (infection.next_insane == 0)
                    infection.next_insane = now + 2000 + static_cast<ULONGLONG>(GetTickCount64() % 4000); // soon after
                else if (infection.next_insane <= now)
                {
                    const float stage =
                        rules.insane_at >= 100 ? 1.f
                                               : std::clamp((severity * 100.f - rules.insane_at) / (100.f - rules.insane_at), 0.f, 1.f);
                    infection.next_insane = now + insane_delay(stage);
                    if (void* player = player_by_user_id(user_id); player != nullptr && alive(player))
                    {
                        go_insane(player, user_id, stage);
                        infection.next_cough = std::max(infection.next_cough, now + 3000);
                    }
                }
            }
            for (const int user_id : due)
            {
                void* player = player_by_user_id(user_id);
                if (player == nullptr || !alive(player))
                    g_infections.erase(user_id);
                else
                    turn(player, user_id);
            }
            remove_bodies(now);
        });
    }

    void install_infection()
    {
        // who a game event means, the clock, turning, the rescue closets
        const std::uintptr_t user_id = g_engine_server != nullptr ? mem::vslot(g_engine_server, 16) : 0;
        const std::uintptr_t script = g.server.base + k_rva_script_command;
        const std::uintptr_t rescue = g.server.base + k_rva_rescue_set_survivor;
        const std::uintptr_t suicide = mem::read_or<std::uintptr_t>(g.player_vtable + 4u * k_vt_commit_suicide);
        const auto server_create = reinterpret_cast<CreateInterfaceFn>(
            GetProcAddress(reinterpret_cast<HMODULE>(g.server.base), "CreateInterface"));
        void* game_dll = server_create != nullptr ? server_create("ServerGameDLL005", nullptr) : nullptr;
        const std::uintptr_t game_frame = game_dll != nullptr ? mem::vslot(game_dll, 4) : 0;
        if (user_id != 0 && mem::find_pattern(user_id, 12, "55 8B EC 83 3D ?? ?? ?? ?? 02 56 7C") == user_id &&
            mem::find_pattern(script, 12, "55 8B EC 56 8B 75 08 83 3E 01 7E 0B") == script &&
            mem::find_pattern(rescue, 14, "55 8B EC 51 8B 15 ?? ?? ?? ?? 53 56 8B F1") == rescue &&
            suicide == g.server.base + 0x174210 && mem::find_pattern(suicide, 6, "55 8B EC 83 EC 64") == suicide &&
            game_frame == g.server.base + 0xEF4B0 && mem::find_pattern(game_frame, 6, "55 8B EC 83 EC 14") == game_frame &&
            fire_event_installed())
        {
            g_user_id = reinterpret_cast<UserIdFn>(user_id);
            g_script_command = reinterpret_cast<ScriptCommandFn>(script);
            install_game_frame(game_frame);
            if (g_rescue_set_survivor.create("RescueSetSurvivor", reinterpret_cast<void*>(rescue), hk_rescue_set_survivor))
                g_rescue_set_survivor.enable();
            INV_LOG("game: infection ready (GameFrame %p; on when inventory_mod.toml has [infection])",
                    reinterpret_cast<void*>(game_frame));
        }
        else
            INV_LOG("game: the infection's functions do not look as expected: no infection");
    }

} // namespace game::detail
