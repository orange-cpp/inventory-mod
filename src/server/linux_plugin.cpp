#include "game/remote_state.hpp"
#include "inv/rules.hpp"
#include "server/linux_game.hpp"
#include "server/linux_mutators.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <toml++/toml.hpp>

#include "eiface.h"
#include "engine/IEngineTrace.h"
#include "engine/iserverplugin.h"
#include "game/server/iplayerinfo.h"
#include "igameevents.h"
#include "icvar.h"
#include "tier1/convar.h"
#include "toolframework/itoolentity.h"

namespace
{
    namespace fs = std::filesystem;

    std::string read_file(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

    bool auto_store_enabled(const fs::path& path)
    {
        const std::string text = read_file(path);
        if (text.empty())
            return false;
        try
        {
            const auto settings = toml::parse(text);
            return settings["play"]["auto_store"].value_or(false);
        }
        catch (const toml::parse_error& error)
        {
            std::fprintf(stderr, "inventory_mod: invalid settings file: %s\n", error.what());
            return false;
        }
    }

    bool write_inventory(const fs::path& path, const inv::Inventory& inventory)
    {
        fs::path temporary = path;
        temporary += ".tmp";
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << inventory.to_text();
        stream.close();
        if (stream)
        {
            std::error_code error;
            fs::rename(temporary, path, error);
            if (!error)
                return true;
        }
        std::error_code ignored;
        fs::remove(temporary, ignored);
        std::fprintf(stderr, "inventory_mod: could not save %s\n", path.c_str());
        return false;
    }

    fs::path player_file(const fs::path& directory, std::string_view network_id)
    {
        if (!network_id.starts_with("STEAM_"))
            return {};
        network_id.remove_prefix(6);
        if (network_id.find_first_not_of("0123456789:") != std::string_view::npos)
            return {};
        const auto first = network_id.find(':');
        const auto second = first == std::string_view::npos ? first : network_id.find(':', first + 1);
        if (first == std::string_view::npos || first == 0 || second != first + 2 || second + 1 >= network_id.size() ||
            network_id.find(':', second + 1) != std::string_view::npos)
            return {};

        std::string filename = "inventory_STEAM_";
        for (const char character : network_id)
            filename += character == ':' ? '_' : character;
        return directory / (filename + ".inventory");
    }

    int slot_argument(const CCommand& arguments, int index)
    {
        const std::string_view text(arguments.Arg(index));
        int slot = -1;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), slot);
        return error == std::errc{} && end == text.data() + text.size() ? slot : -1;
    }

    struct PlayerInventory
    {
        inv::Inventory inventory;
        fs::path file;
        std::string chapter_start;
        std::string pending_notice;
        std::optional<game::LinuxGame::Reserve> transition_reserve;
        edict_t* entity = nullptr;
        std::optional<game::LinuxGame::Position> death_position;
        std::optional<std::chrono::steady_clock::time_point> last_ping;
        std::chrono::steady_clock::time_point next_mutators{};
        std::uint32_t saved_revision;

        PlayerInventory(inv::Limits limits, fs::path path, const std::optional<fs::file_time_type>& campaign_start)
            : inventory(limits), file(std::move(path))
        {
            std::error_code error;
            const bool previous_campaign = !file.empty() && campaign_start &&
                fs::last_write_time(file, error) < *campaign_start && !error;
            if (!file.empty() && !previous_campaign)
            {
                inventory.set_max_weight(0.f);
                inventory.from_text(read_file(file));
                inventory.set_max_weight(limits.max_weight);
            }
            chapter_start = inventory.to_text();
            saved_revision = inventory.revision();
        }

        void save()
        {
            if (!file.empty() && inventory.revision() != saved_revision && write_inventory(file, inventory))
                saved_revision = inventory.revision();
        }
    };

    class InventoryPlugin final : public IServerPluginCallbacks, public IGameEventListener2
    {
    public:
        bool Load(CreateInterfaceFn engine_factory, CreateInterfaceFn game_factory) override
        {
            if (engine_factory == nullptr || game_factory == nullptr)
                return false;
            engine_ = static_cast<IVEngineServer*>(engine_factory(INTERFACEVERSION_VENGINESERVER, nullptr));
            players_ = static_cast<IPlayerInfoManager*>(game_factory(INTERFACEVERSION_PLAYERINFOMANAGER, nullptr));
            if (engine_ == nullptr || players_ == nullptr || !engine_->IsDedicatedServer())
                return false;
            auto* entities = static_cast<IServerGameEnts*>(game_factory(INTERFACEVERSION_SERVERGAMEENTS, nullptr));
            auto* tools = static_cast<IServerTools*>(game_factory(VSERVERTOOLS_INTERFACE_VERSION, nullptr));
            if (!game_.bind(entities, tools))
            {
                std::fprintf(stderr, "inventory_mod: unsupported server_srv.so build or unavailable game interfaces\n");
                return false;
            }

            char game_directory[4096]{};
            engine_->GetGameDir(game_directory, sizeof(game_directory));
            if (game_directory[0] == '\0')
                return false;
            directory_ = fs::path(game_directory) / "addons" / "inventory_mod";
            std::error_code error;
            fs::create_directories(directory_, error);
            if (error)
            {
                std::fprintf(stderr, "inventory_mod: could not open the dedicated server's data directory\n");
                return false;
            }

            const inv::Rules rules = inv::apply_rules(read_file(directory_ / "inventory_mod.toml"));
            for (const std::string& message : rules.errors)
                std::fprintf(stderr, "inventory_mod: %s\n", message.c_str());
            limits_ = {.slots = rules.slots.value_or(24),
                       .max_weight = rules.max_weight.value_or(0.f),
                       .weapon_slots = rules.weapon_slots.value_or(8)};
            events_ = static_cast<IGameEventManager2*>(engine_factory(INTERFACEVERSION_GAMEEVENTSMANAGER2, nullptr));
            if (events_ == nullptr)
                return false;
            trace_ = static_cast<IEngineTrace*>(engine_factory(INTERFACEVERSION_ENGINETRACE_SERVER, nullptr));
            if (!game_.install_ammo(engine_, players_, [this](edict_t* entity) -> inv::Inventory* {
                    const auto player = inventories_.find(engine_->GetPlayerUserId(entity));
                    return player == inventories_.end() ? nullptr : &player->second.inventory;
                }))
                std::fprintf(stderr, "inventory_mod: ammo-pile handling could not be installed\n");
            if (!game_.install_pickups(auto_store_enabled(directory_ / "inventory_mod_settings.toml"),
                                       [this](edict_t* entity, std::string notice) {
                    if (const auto player = inventories_.find(engine_->GetPlayerUserId(entity)); player != inventories_.end())
                    {
                        player->second.pending_notice = std::move(notice);
                        player->second.save();
                    }
                }))
                std::fprintf(stderr, "inventory_mod: automatic pickups could not be installed\n");
            auto* cvars = static_cast<ICvar*>(engine_factory(CVAR_INTERFACE_VERSION, nullptr));
            if (!game_.install_actions(cvars))
                std::fprintf(stderr, "inventory_mod: a configured survivor action could not be installed\n");
            mutators_.configure(cvars);
            if (!game_.install_infection())
                std::fprintf(stderr, "inventory_mod: infection handling could not be installed\n");
            if (!game_.install_melee(trace_))
                std::fprintf(stderr, "inventory_mod: expanded melee could not be installed\n");
            listen_for_events();
            return true;
        }

        void Unload() override
        {
            events_->RemoveListener(this);
            clear_followed_pings();
            mutators_.clear();
            for (const auto& [id, player] : inventories_)
                if (player.entity != nullptr && !player.entity->IsFree())
                    engine_->ClientPrintf(player.entity, mutators_.line().c_str());
            game_.uninstall_actions();
            game_.uninstall_infection();
            game_.uninstall_melee();
            game_.uninstall_stamina();
            game_.uninstall_pickups();
            game_.uninstall_ammo();
            LevelShutdown();
            inventories_.clear();
            map_.clear();
            round_map_.clear();
            transition_ = false;
            campaign_start_.reset();
        }
        void Pause() override {}
        void UnPause() override {}
        const char* GetPluginDescription() override { return "Inventory Mod (Linux dedicated server)"; }
        void LevelInit(const char* map) override
        {
            map_ = map ? map : "";
            listen_for_events();
        }
        void ServerActivate(edict_t* edicts, int, int clients) override
        {
            edicts_ = edicts;
            max_clients_ = clients;
            game_.find_pickup_entities();
        }
        void GameFrame(bool simulating) override
        {
            if (!simulating)
                return;
            const auto now = std::chrono::steady_clock::now();
            game_.melee_frame(edicts_, max_clients_);
            game_.stamina_frame(edicts_, max_clients_);
            game_.actions_frame(edicts_, max_clients_);
            game_.infection_frame();
            if (const auto* globals = players_->GetGlobalVars())
                mutators_.frame(game_, globals->curtime);
            advance_pings(now);
            if (now >= scan_pickups_at_)
            {
                game_.find_pickup_entities();
                scan_pickups_at_ = now + std::chrono::milliseconds(350);
            }
            if (place_piles_at_ && now >= *place_piles_at_)
            {
                place_piles_at_.reset();
                game_.place_map_piles();
            }
            if (edicts_ != nullptr)
            {
                for (int index = 1; index <= max_clients_; ++index)
                {
                    edict_t* edict = edicts_ + index;
                    IPlayerInfo* info = players_->GetPlayerInfo(edict);
                    if (info != nullptr && info->IsConnected() && info->GetTeamIndex() == 2 && !info->IsDead())
                    {
                        game_.normalize_ammo(edict);
                        game_.remember_carried_weapons(edict);
                    }
                }
            }
            for (auto& [user_id, player] : inventories_)
            {
                if (player.entity != nullptr && !player.entity->IsFree() && now >= player.next_mutators &&
                    !mutators_.line().empty())
                {
                    engine_->ClientPrintf(player.entity, mutators_.line().c_str());
                    player.next_mutators = now + std::chrono::seconds(2);
                }
                if (!player.death_position || player.entity == nullptr)
                    continue;
                IPlayerInfo* info = players_->GetPlayerInfo(player.entity);
                if (info == nullptr || engine_->GetPlayerUserId(player.entity) != user_id)
                {
                    player.death_position.reset();
                    continue;
                }
                if (!info->IsDead())
                    continue;
                game_.drop_on_death(player.entity, *player.death_position, player.inventory);
                player.death_position.reset();
                player.save();
            }
        }
        void LevelShutdown() override
        {
            game_.clear_infection(false);
            game_.clear_melee(false);
            game_.clear_actions(false);
            game_.clear_stamina(false);
            game_.clear_piles();
            game_.restart_pickups();
            followed_pings_.clear();
            game_.forget_ping_glows();
            place_piles_at_.reset();
            edicts_ = nullptr;
            max_clients_ = 0;
            for (auto& [user_id, player] : inventories_)
            {
                player.save();
                player.entity = nullptr;
                player.death_position.reset();
                player.last_ping.reset();
                player.next_mutators = {};
            }
        }
        void ClientActive(edict_t*) override {}
        void ClientDisconnect(edict_t* entity) override
        {
            const int user_id = engine_->GetPlayerUserId(entity);
            game_.forget_infection(user_id);
            remove_followed_ping(user_id);
            if (auto player = inventories_.find(user_id); player != inventories_.end())
            {
                player->second.save();
                inventories_.erase(player);
            }
        }
        void ClientPutInServer(edict_t*, const char*) override {}
        void SetCommandClient(int) override {}
        void ClientSettingsChanged(edict_t*) override {}
        PLUGIN_RESULT ClientConnect(bool*, edict_t*, const char*, const char*, char*, int) override
        {
            return PLUGIN_CONTINUE;
        }
        PLUGIN_RESULT NetworkIDValidated(const char*, const char*) override { return PLUGIN_CONTINUE; }
        void OnQueryCvarValueFinished(QueryCvarCookie_t, edict_t*, EQueryCvarValueStatus, const char*, const char*) override {}

        void FireGameEvent(IGameEvent* event) override
        {
            game_.infection_event(*event, edicts_, max_clients_);
            const std::string_view name(event->GetName());
            if (name == "map_transition")
            {
                transition_ = true;
                for (auto& [user_id, player] : inventories_)
                {
                    IPlayerInfo* info = player.entity == nullptr ? nullptr : players_->GetPlayerInfo(player.entity);
                    if (info != nullptr && info->GetTeamIndex() == 2)
                        player.transition_reserve = game_.read_reserve(player.entity);
                }
            }
            else if (name == "round_start")
            {
                game_.clear_actions(true);
                game_.clear_melee(true);
                clear_followed_pings();
                game_.clear_piles(true);
                game_.restart_pickups();
                const bool restart = !transition_ && !map_.empty() && round_map_ == map_;
                mutators_.start_round(map_, restart);
                place_piles_at_.reset();
                if (inv::ammo_pile_rules().enabled)
                    place_piles_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
                const bool new_campaign = !restart && !transition_ && !round_map_.empty();
                if (new_campaign)
                    campaign_start_ = fs::file_time_type::clock::now();
                for (auto& [user_id, player] : inventories_)
                {
                    player.death_position.reset();
                    if (restart)
                    {
                        player.inventory.set_max_weight(0.f);
                        player.inventory.from_text(player.chapter_start);
                    }
                    else if (new_campaign)
                        player.inventory.clear();
                    player.inventory.set_max_weight(inv::mutated_max_weight(limits_.max_weight));
                    player.next_mutators = {};
                    if (!transition_)
                        player.transition_reserve.reset();
                    player.chapter_start = player.inventory.to_text();
                    player.save();
                }
                round_map_ = map_;
                transition_ = false;
            }
            else if (name == "player_left_start_area")
            {
                if (const auto* globals = players_->GetGlobalVars())
                    mutators_.left_start_area(globals->curtime);
            }
            else if (name == "player_death")
            {
                auto player = inventories_.find(event->GetInt("userid"));
                if (player != inventories_.end() && player->second.entity != nullptr)
                {
                    IPlayerInfo* info = players_->GetPlayerInfo(player->second.entity);
                    if (info != nullptr && info->GetTeamIndex() == 2)
                        player->second.death_position = game_.player_position(player->second.entity);
                }
            }
            else if (name == "player_spawn")
            {
                const auto player = inventories_.find(event->GetInt("userid"));
                if (player != inventories_.end())
                    player->second.death_position.reset();
            }
        }

        int GetEventDebugID() override { return EVENT_DEBUG_ID_INIT; }

        PLUGIN_RESULT ClientCommand(edict_t* entity, const CCommand& arguments) override
        {
            const std::string_view command(arguments.Arg(0));
            if (command != "inv_sync" && command != "inv_move" && command != "inv_select" &&
                command != "inv_pickup" && command != "inv_store" && command != "inv_use" && command != "inv_drop" &&
                command != "inv_dropweapon" && command != "inv_unload" && command != "inv_dropammo" &&
                command != "inv_ping" && command != "inv_infect" && command != "inv_spawn")
                return PLUGIN_CONTINUE;

            IPlayerInfo* info = entity == nullptr ? nullptr : players_->GetPlayerInfo(entity);
            if (info == nullptr || !info->IsConnected() || info->IsFakeClient())
                return PLUGIN_STOP;
            const int user_id = engine_->GetPlayerUserId(entity);
            if (user_id < 1)
                return PLUGIN_STOP;

            const char* raw_id = engine_->GetPlayerNetworkIDString(entity);
            const std::string_view network_id = raw_id == nullptr ? "" : raw_id;
            if (network_id == "STEAM_ID_PENDING")
                return PLUGIN_STOP;
            auto effective_limits = limits_;
            effective_limits.max_weight = inv::mutated_max_weight(limits_.max_weight);
            const auto entry = inventories_.try_emplace(
                user_id, effective_limits, player_file(directory_, network_id), campaign_start_).first;
            PlayerInventory& player = entry->second;
            player.entity = entity;
            const bool survivor = info->GetTeamIndex() == 2 && !info->IsDead();
            if (survivor && player.transition_reserve && !transition_)
            {
                game_.restore_reserve(entity, *player.transition_reserve);
                player.transition_reserve.reset();
            }
            if (survivor)
                game_.normalize_ammo(entity);
            auto native = game_.native_weapons(entity);
            game::detail::reconcile_native_inventory(player.inventory, native, game_.rounds_weight(entity));
            std::string message = std::exchange(player.pending_notice, {});
            if (command == "inv_move")
                player.inventory.move(slot_argument(arguments, 1), slot_argument(arguments, 2));
            else if (command == "inv_ping")
                message = ping(entity, user_id, *info, player);
            else if (command != "inv_sync" && survivor)
            {
                if (command == "inv_select")
                    message = game_.select(entity, arguments.Arg(1), player.inventory);
                else if (command == "inv_pickup")
                    message = game_.pickup(entity, player.inventory);
                else if (command == "inv_store")
                    message = game_.store(entity, player.inventory);
                else if (command == "inv_use")
                    message = game_.use(entity, player.inventory, slot_argument(arguments, 1));
                else if (command == "inv_drop")
                    message = game_.drop(entity, player.inventory, slot_argument(arguments, 1), slot_argument(arguments, 2));
                else if (command == "inv_dropweapon")
                    message = game_.drop_weapon(entity, arguments.Arg(1));
                else if (command == "inv_unload")
                    message = game_.unload(entity, player.inventory);
                else if (command == "inv_dropammo")
                    message = game_.drop_ammo(entity, arguments.Arg(1), slot_argument(arguments, 2));
                else if (command == "inv_infect")
                    game_.infect(entity);
                else if (command == "inv_spawn")
                    message = game_.spawn(entity, arguments.Arg(1));
                native = game_.native_weapons(entity);
                game::detail::reconcile_native_inventory(player.inventory, native, game_.rounds_weight(entity));
            }
            player.save();
            const std::string line = game::detail::encode_remote_state(player.inventory, native, message);
            engine_->ClientPrintf(entity, line.c_str());
            return PLUGIN_STOP;
        }

    private:
        using Clock = std::chrono::steady_clock;

        struct FollowedPing
        {
            std::uint32_t target;
            int team;
            Clock::time_point expires;
            std::optional<Clock::time_point> glow_until;
        };

        void remove_followed_ping(int owner)
        {
            const auto entry = followed_pings_.find(owner);
            if (entry == followed_pings_.end())
                return;
            if (entry->second.glow_until)
                game_.unlight_ping_target(entry->second.target);
            followed_pings_.erase(entry);
        }

        void clear_followed_pings()
        {
            for (const auto& [owner, follow] : followed_pings_)
                if (follow.glow_until)
                    game_.unlight_ping_target(follow.target);
            followed_pings_.clear();
        }

        void send_ping(int owner, int team, int kind, const game::LinuxGame::Position& position, const char* name,
                       int life_ms, int per_player, const char* speech = "")
        {
            const CGlobalVars* globals = players_->GetGlobalVars();
            const int tenths = globals == nullptr ? 0 : static_cast<int>(globals->curtime * 10.f);
            for (const auto& [recipient, tracked] : inventories_)
            {
                if (tracked.entity == nullptr)
                    continue;
                IPlayerInfo* info = players_->GetPlayerInfo(tracked.entity);
                if (info == nullptr || !info->IsConnected() || info->GetTeamIndex() != team)
                    continue;
                char line[256];
                std::snprintf(line, sizeof(line), "@png@ %d %d %d %d %d %.1f %.1f %.1f|%s|%s|%d\n",
                              owner, recipient == owner ? 1 : 0, kind, life_ms, per_player,
                              position[0], position[1], position[2], name, recipient == owner ? speech : "", tenths);
                engine_->ClientPrintf(tracked.entity, line);
            }
        }

        std::string ping(edict_t* entity, int owner, IPlayerInfo& info, PlayerInventory& player)
        {
            const auto& rules = inv::ping_rules();
            if (!rules.enabled)
                return "Pings are off on this server";
            const int team = info.GetTeamIndex();
            if (info.IsDead() || (team != 2 && team != 3) || trace_ == nullptr)
                return {};
            const auto now = Clock::now();
            if (player.last_ping && now - *player.last_ping < std::chrono::duration<float>(rules.cooldown))
                return {};
            const auto mark = game_.ping_target(trace_, entity, team, rules.range);
            if (!mark)
                return {};
            player.last_ping = now;
            const auto lifetime = std::chrono::milliseconds(static_cast<int>(rules.lifetime * 1000.f));
            const char* speech = team != 2 || mark->kind == 0 ? "" :
                                 mark->name == "Ammo" ? "PlayerSpotAmmo" : "smartlook";
            send_ping(owner, team, mark->kind, mark->position, mark->name.c_str(),
                      static_cast<int>(lifetime.count()), rules.per_player, speech);
            std::fprintf(stderr, "inventory_mod: player %d marked %s at %.0f %.0f %.0f\n", owner,
                         mark->name.empty() ? "a spot" : mark->name.c_str(),
                         mark->position[0], mark->position[1], mark->position[2]);
            remove_followed_ping(owner);
            if (mark->follow)
            {
                FollowedPing follow{*mark->follow, team, now + lifetime, std::nullopt};
                if (game_.light_ping_target(follow.target))
                    follow.glow_until = now + std::min(lifetime, std::chrono::milliseconds(5000));
                followed_pings_.emplace(owner, follow);
            }
            return {};
        }

        void advance_pings(Clock::time_point now)
        {
            if (followed_pings_.empty() || now < next_ping_update_)
                return;
            next_ping_update_ = now + std::chrono::milliseconds(100);
            for (auto it = followed_pings_.begin(); it != followed_pings_.end();)
            {
                const int owner = it->first;
                auto& follow = it->second;
                const auto position = game_.ping_follow(follow.target);
                const bool gone = !position || now >= follow.expires;
                if (follow.glow_until && (gone || now >= *follow.glow_until))
                {
                    game_.unlight_ping_target(follow.target);
                    follow.glow_until.reset();
                }
                if (gone)
                {
                    it = followed_pings_.erase(it);
                    continue;
                }
                send_ping(owner, follow.team, 3, *position, "", 1000, 1);
                ++it;
            }
        }

        void listen_for_events()
        {
            for (const char* name : {"round_start", "map_transition", "player_death", "player_spawn", "player_left_start_area",
                                     "round_end", "mission_lost", "finale_vehicle_leaving", "finale_win", "tongue_grab",
                                     "lunge_pounce", "jockey_ride", "charger_carry_start", "charger_pummel_start",
                                     "player_now_it", "player_incapacitated", "pills_used"})
                if (!events_->FindListener(this, name))
                    events_->AddListener(this, name, true);
        }

        IVEngineServer* engine_ = nullptr;
        IPlayerInfoManager* players_ = nullptr;
        IGameEventManager2* events_ = nullptr;
        IEngineTrace* trace_ = nullptr;
        game::LinuxGame game_;
        game::LinuxMutators mutators_;
        fs::path directory_;
        inv::Limits limits_;
        std::unordered_map<int, PlayerInventory> inventories_;
        std::string map_;
        std::string round_map_;
        bool transition_ = false;
        std::optional<fs::file_time_type> campaign_start_;
        std::optional<std::chrono::steady_clock::time_point> place_piles_at_;
        std::chrono::steady_clock::time_point scan_pickups_at_{};
        Clock::time_point next_ping_update_{};
        std::unordered_map<int, FollowedPing> followed_pings_;
        edict_t* edicts_ = nullptr;
        int max_clients_ = 0;
    };

    InventoryPlugin plugin;
}

extern "C" __attribute__((visibility("default"))) void* CreateInterface(const char* name, int* result)
{
    const bool supported = name != nullptr && std::strcmp(name, INTERFACEVERSION_ISERVERPLUGINCALLBACKS) == 0;
    if (result != nullptr)
        *result = supported ? IFACE_OK : IFACE_FAILED;
    return supported ? static_cast<IServerPluginCallbacks*>(&plugin) : nullptr;
}
