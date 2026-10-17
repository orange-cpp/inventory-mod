#pragma once
// The game's world as the rest of game/ sees it: the functions and interfaces game::init found (Env), the entity list,
// "the local player", and what every module asks of a player or an entity.  Server side, main thread only.

#include "game/mem.hpp"
#include "game/sdk.hpp"
#include "inv/items.hpp"

#include <cstdint>
#include <string>

namespace game::detail
{

    using namespace sdk;

    // How close an item must be to be taken (or unloaded) off the ground.
    constexpr float k_pickup_reach = 160.f;

    struct Env
    {
        mem::ModuleInfo server;
        void* tools = nullptr;
        std::uintptr_t ent_list = 0;
        std::uintptr_t player_vtable = 0;
        GetKeyValueFn get_key_value = nullptr;
        SetKeyValueFn set_key_value = nullptr;
        CreateEntityFn create_entity = nullptr;
        DispatchSpawnFn dispatch_spawn = nullptr;
        RemoveFn util_remove = nullptr;
        DropToFloorFn drop_to_floor = nullptr; // optional
        WeaponDropFn quiet_drop = nullptr; // CBaseCombatCharacter::Weapon_Drop: no "weapon_drop" event (optional)
        void* vgui_input = nullptr; // vgui2.dll's VGUI_Input005, probed for keyboard focus (optional)
    };
    extern Env g;

    extern bool g_dedicated; // running in srcds.exe
    extern void* g_engine_server; // engine.dll VEngineServer022 (see "other players")
    extern void* g_active_player; // the player an Activation serves (see "other players"); nullptr: the host
    extern void* g_globals; // CGlobalVars (gpGlobals), for the game's clock and the map's name
    extern UserIdFn g_user_id; // VEngineServer022 slot 16 GetPlayerUserId
    extern NetworkIdFn g_network_id; // VEngineServer022 slot 17 GetPlayerNetworkIDString
    extern ChangeAccessorFn g_change_accessor; // VEngineServer022 slot 97 GetChangeAccessor

    // Finds server.dll's IServerTools, the entity list, UTIL_Remove and the rest of Env (see game::init).  False, with
    // the reason in the log, when the build or a function does not look as expected.
    bool init_env();

    void* entity_at_index(int index);
    void* entity_from_handle(std::uint32_t handle);
    std::uint32_t handle_of(void* entity); // k_invalid_handle: not there
    void* host_player();
    void* local_player();
    bool classname_of(void* entity, char* out, int length);
    const inv::ItemDef* item_of(void* entity);
    std::uint32_t handle_at(void* player, std::uintptr_t offset);
    std::string carried_list(void* player);
    bool player_has_slot(void* player, int game_slot);
    void* carried_in_slot(void* player, int game_slot, const inv::ItemDef** def_out);

    void* edict_of(void* player);
    int user_id_of(void* player);
    void* player_by_user_id(int user_id);
    int team_of(void* entity);
    bool alive(void* player);
    bool is_bot(void* player);

    float game_time();
    void state_changed(void* entity);

} // namespace game::detail
