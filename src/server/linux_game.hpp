#pragma once

#include "game/remote_state.hpp"
#include "server/linux_sdk.hpp"
#include "server/linux_memory.hpp"

#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class IServerGameEnts;
class IServerTools;
class IVEngineServer;
class IPlayerInfoManager;
class IEngineTrace;
class IGameEvent;
class ICvar;
struct edict_t;
namespace inv { struct StaminaRules; }

namespace game
{
    class LinuxGame
    {
    public:
        using Reserve = std::array<int, 32>;
        using Position = std::array<float, 3>;
        struct PingTarget
        {
            Position position;
            int kind;
            std::string name;
            std::optional<std::uint32_t> follow;
        };

        bool bind(IServerGameEnts* entities, IServerTools* tools);
        bool install_ammo(IVEngineServer* engine, IPlayerInfoManager* players,
                          std::function<inv::Inventory*(edict_t*)> inventory_lookup);
        void uninstall_ammo();
        bool install_pickups(bool auto_store, std::function<void(edict_t*, std::string)> pickup_notice);
        void uninstall_pickups();
        void find_pickup_entities();
        void restart_pickups();
        void stamina_frame(edict_t* edicts, int clients);
        void clear_stamina(bool restore);
        void uninstall_stamina();
        bool install_actions(ICvar* cvars);
        void uninstall_actions();
        void clear_actions(bool restore);
        void actions_frame(edict_t* edicts, int clients);
        bool trigger_panic() const;
        bool install_infection();
        void uninstall_infection();
        void infection_event(IGameEvent& event, edict_t* edicts, int clients);
        void infection_frame();
        bool install_melee(IEngineTrace* trace);
        void uninstall_melee();
        void melee_frame(edict_t* edicts, int clients);
        void clear_melee(bool restore);
        void clear_infection(bool notify, bool reset_round = true);
        void infect(edict_t* player);
        void forget_infection(int user_id);
        std::optional<PingTarget> ping_target(IEngineTrace* trace, edict_t* player, int team, float range) const;
        std::optional<Position> ping_follow(std::uint32_t handle) const;
        bool light_ping_target(std::uint32_t handle);
        void unlight_ping_target(std::uint32_t handle);
        void forget_ping_glows();
        void clear_piles(bool remove_spawned = false);
        void place_map_piles();
        std::array<NativeWeapon, k_native_slots> native_weapons(edict_t* player) const;
        float rounds_weight(edict_t* player) const;
        Reserve read_reserve(edict_t* player) const;
        void restore_reserve(edict_t* player, const Reserve& reserve) const;
        std::string select(edict_t* player, std::string_view classname, inv::Inventory& inventory);
        std::string drop_weapon(edict_t* player, std::string_view classname) const;
        std::string pickup(edict_t* player, inv::Inventory& inventory) const;
        std::string spawn(edict_t* player, std::string_view classname) const;
        std::string use(edict_t* player, inv::Inventory& inventory, int slot);
        std::string store(edict_t* player, inv::Inventory& inventory) const;
        std::string drop(edict_t* player, inv::Inventory& inventory, int slot, int count) const;
        std::string unload(edict_t* player, const inv::Inventory& inventory);
        std::string drop_ammo(edict_t* player, std::string_view kind, int count);
        void normalize_ammo(edict_t* player) const;
        void remember_carried_weapons(edict_t* player) const;
        std::optional<Position> player_position(edict_t* player) const;
        void drop_on_death(edict_t* player, const Position& position, inv::Inventory& inventory);

    private:
        bool can_handle_items(CBaseEntity* player) const;
        bool release_carried(CBaseEntity* player, CBaseEntity* weapon, const inv::ItemDef& def,
                             linux_sdk::QuietDrop drop_method) const;
        bool store_item(CBaseEntity* player, CBaseEntity* weapon, const inv::ItemDef& def, inv::Inventory& inventory) const;
        void put_away_carried(CBaseEntity* player, std::string_view switching_to, inv::Inventory& inventory) const;
        std::string use_weapon(CBaseEntity* player, inv::Inventory& inventory, int slot, const inv::Stack& stack) const;
        inv::WeaponState weapon_state(CBaseEntity* weapon, const inv::ItemDef& def, bool carried) const;
        void apply_weapon_state(CBaseEntity* weapon, const inv::ItemDef& def, const inv::WeaponState& state, bool on_ground) const;
        void restore_rounds(CBaseEntity* player, const int (&pool)[32]) const;
        void add_rounds(CBaseEntity* player, CBaseEntity* weapon, const inv::ItemDef& def, int rounds) const;
        CBaseEntity* spawn_item(const inv::ItemDef& def, const inv::WeaponState& state, const Vector& position,
                                bool fresh_map_weapon = false) const;
        CBaseEntity* item_in_front(const Vector& eye, const Vector& forward, float min_facing,
                                   bool (*eligible)(const inv::ItemDef&), bool include_spawners = false) const;
        bool weapon_spawner(CBaseEntity* entity) const;
        const inv::ItemDef* spawner_item(CBaseEntity* spawner) const;
        std::optional<PingTarget> ping_entity(CBaseEntity* target, int team) const;
        CBaseEntity* find_entity(std::uint32_t handle) const;
        void give_to_player(CBaseEntity* player, const char* name) const;
        void remember_weapon(CBaseEntity* weapon) const;
        bool cut_map_weapon(CBaseEntity* gun, const inv::ItemDef& def) const;
        bool store_world_item(CBaseEntity* player, CBaseEntity* item, const inv::ItemDef& def, inv::Inventory& inventory) const;
        bool wants_to_store(CBaseEntity* player, const inv::ItemDef& def) const;
        edict_t* survivor_edict(CBaseEntity* player) const;
        bool weapon_over_limit(CBaseEntity* player, const inv::ItemDef& def, const inv::Inventory& inventory) const;
        void finish_weapon_pickup(edict_t* edict, CBaseEntity* player, CBaseEntity* incoming, CBaseEntity* previous,
                                  const inv::ItemDef& def, const Reserve& before, int incoming_rounds, bool fresh) const;
        bool bump_weapon(CBaseEntity* player, CBaseEntity* weapon);
        void use_world_weapon(CBaseEntity* weapon, CBaseEntity* player, CBaseEntity* caller, int use_type, float value);
        void use_weapon_spawner(CBaseEntity* spawner, CBaseEntity* player, CBaseEntity* caller, int use_type, float value);
        static bool bump_weapon_hook(CBaseEntity* player, CBaseEntity* weapon);
        static void world_weapon_use_hook(CBaseEntity* weapon, CBaseEntity* player, CBaseEntity* caller, int use_type, float value);
        static void weapon_spawner_use_hook(CBaseEntity* spawner, CBaseEntity* player, CBaseEntity* caller, int use_type, float value);
        bool spawn_pile(int type, int count, const Vector& position, bool placed_on_map = false);
        void tint_pile(CBaseEntity* pile, int type) const;
        void use_pile(CBaseEntity* pile, CBaseEntity* activator, CBaseEntity* caller, int use_type, float value);
        static void pile_use_hook(CBaseEntity* pile, CBaseEntity* activator, CBaseEntity* caller, int use_type, float value);
        bool install_reload(CBaseEntity* gun);
        static bool reload_hook(CBaseEntity* gun);
        static void fire_empty_hook(CBaseEntity* gun);
        static LinuxGame* ammo_game_;

        struct AmmoPile
        {
            int type;
            int count;
            bool map;
            bool spawned;
        };
        struct GlowState
        {
            int users = 1;
            int type;
            int range;
            int minimum;
            std::uint32_t color;
            unsigned char flashing;
        };
        struct ReloadTable
        {
            linux_sdk::Reload reload;
            linux_sdk::FireOnEmpty fire;
        };

        struct StaminaState
        {
            edict_t* edict = nullptr;
            std::uint32_t handle = 0xffffffffu;
            std::uint32_t weapon = 0xffffffffu;
            float value = 100.f;
            float used_at = -1000.f;
            float speed = 1.f;
            float base_speed = 1.f;
            float applied = 1.f;
            float next_primary = 0.f;
            float next_secondary = 0.f;
            float sent_at = -1000.f;
            int sent = -2;
            bool exhausted = false;
            bool sent_exhausted = false;
            bool on_ground = true;
            bool melee = false;
            bool seen = false;
        };

        void tick_stamina(CBaseEntity* player, StaminaState& state, const inv::StaminaRules& rules, float now, float dt);
        void send_stamina(StaminaState& state, float now, bool gone);
        void release_stamina(int user_id, StaminaState& state);
        void find_damage_targets();
        static int stamina_damage_hook(CBaseEntity* victim, const void* info);
        float melee_damage(CBaseEntity* victim, const void* info, float damage);
        bool exhausted(CBaseEntity* player) const;
        void spend_stamina(CBaseEntity* player, float amount, float now);
        float melee_move_scale(CBaseEntity* player) const;
        int weapon_wear(CBaseEntity* weapon) const;
        void set_weapon_wear(CBaseEntity* weapon, int wear) const;
        void wear_out(CBaseEntity* weapon, float hits);
        void tell_melee_hit(CBaseEntity* player, bool heavy) const;
        bool in_front(CBaseEntity* player, CBaseEntity* target) const;
        void stagger_enemy(CBaseEntity* enemy, CBaseEntity* source) const;
        static void melee_primary_hook(CBaseEntity* weapon);
        static void melee_secondary_hook(CBaseEntity* weapon);
        static void melee_sound_hook(CBaseEntity* weapon, int type, float soundtime);
        void melee_attack(CBaseEntity* weapon, bool shove);
        void raise_melee(CBaseEntity* player, CBaseEntity* weapon) const;
        int overhead_index(CBaseEntity* weapon) const;
        bool wall_ahead(CBaseEntity* player) const;
        bool throw_melee(CBaseEntity* player, CBaseEntity* weapon, float now);
        void fly_thrown_melee(float now);
        struct MeleeState
        {
            float winding = -1.f;
            float winding_last = 0.f;
            float wound = 0.f;
            float wound_at = -1000.f;
            float block_since = -1.f;
            float swing_at = -1000.f;
            std::uint32_t swung_weapon = 0xffffffffu;
            float move_scale = 1.f;
            float applied = 1.f;
            float base_speed = 1.f;
            float sent_at = -1000.f;
            int sent_hold = -1;
            int sent_shove = -1;
            bool heavy = false;
            bool bounced = false;
            bool seen = false;
            bool controls_speed = false;
            float thrown_at = -1000.f;
            std::vector<std::uint32_t> victims;
        };
        std::unordered_map<std::uint32_t, MeleeState> melee_;
        mutable std::unordered_map<std::uint32_t, int> melee_wear_;
        std::unordered_set<std::uint32_t> breaking_melee_;
        std::optional<float> last_melee_frame_;
        float prune_wear_at_ = 0.f;
        linux_sdk::ItemPostFrame original_melee_primary_ = nullptr;
        linux_sdk::ItemPostFrame original_melee_secondary_ = nullptr;
        linux_sdk::ItemPostFrame* melee_primary_slot_ = nullptr;
        linux_sdk::ItemPostFrame* melee_secondary_slot_ = nullptr;
        linux_sdk::ViewPunch view_punch_ = nullptr;
        linux_sdk::StaggerPlayer stagger_player_ = nullptr;
        linux_sdk::ShoveResponder shove_responder_ = nullptr;
        linux_sdk::MeleeInfo melee_info_ = nullptr;
        linux_sdk::AnimationEvent animation_event_ = nullptr;
        linux_sdk::WeaponSound original_melee_sound_ = nullptr;
        linux_sdk::WeaponSound* melee_sound_slot_ = nullptr;
        linux_sdk::Teleport teleport_ = nullptr;
        linux_sdk::DamageInfo damage_info_ = nullptr;
        linux_sdk::OnTakeDamage take_damage_ = nullptr;
        IEngineTrace* melee_trace_ = nullptr;
        struct ThrownMelee
        {
            std::uint32_t weapon;
            std::uint32_t player;
            float started;
            Position last;
            std::vector<std::uint32_t> targets;
        };
        std::vector<ThrownMelee> thrown_melee_;
        bool melee_installed_ = false;

        struct Progress
        {
            edict_t* edict;
            std::uint32_t player;
            float start;
            float duration;
        };
        struct Refuel
        {
            Progress timer;
            std::uint32_t saw;
        };
        struct Revive
        {
            Progress timer;
            const inv::ItemDef* item;
            int slot;
        };

        static void chainsaw_post_frame(CBaseEntity* saw);
        int can_spent(CBaseEntity* can) const;
        void set_can_spent(CBaseEntity* can, int spent) const;
        int max_fuel(CBaseEntity* saw) const;
        std::string pour_fuel(CBaseEntity* saw, inv::Inventory& inventory) const;
        void fuel_input(edict_t* edict, CBaseEntity* player, int user_id, float now);
        Progress start_progress(edict_t* edict, CBaseEntity* player, float now, float duration) const;
        void clear_progress(CBaseEntity* player, const Progress& timer) const;
        CBaseEntity* progress_player(int user_id, const Progress& timer) const;
        std::optional<std::string> start_revive(edict_t* edict, CBaseEntity* player, const inv::ItemDef& item,
                                                int slot, inv::Inventory& inventory);
        std::string finish_revive(CBaseEntity* player, const Revive& revive, inv::Inventory& inventory) const;
        bool use_revive_item(CBaseEntity* player, const Revive& revive, inv::Inventory& inventory, bool consume) const;
        bool down(CBaseEntity* player) const;

        ICvar* cvars_ = nullptr;
        linux_sdk::ItemPostFrame original_chainsaw_frame_ = nullptr;
        linux_sdk::ItemPostFrame* chainsaw_frame_slot_ = nullptr;
        linux_sdk::OnRevived on_revived_ = nullptr;
        linux_sdk::OnAdrenalineUsed on_adrenaline_used_ = nullptr;
        linux_sdk::PanicEvent panic_event_ = nullptr;
        linux_sdk::CannotHandleItems hanging_from_ledge_ = nullptr;
        std::unordered_map<int, Refuel> refuels_;
        std::unordered_map<int, Revive> revives_;
        std::unordered_map<int, bool> reload_down_;
        mutable std::unordered_map<std::uint32_t, int> spent_cans_;
        float prune_cans_at_ = 0.f;
        bool fuel_hook_installed_ = false;

        using InfectionClock = std::chrono::steady_clock;
        struct Infection
        {
            edict_t* edict;
            std::uint32_t player;
            InfectionClock::time_point until;
            int total_ms;
            InfectionClock::time_point next_cough;
            std::optional<InfectionClock::time_point> next_insane;
            bool told = false;
        };
        struct TurningSurvivor
        {
            int user_id;
            edict_t* edict;
            std::uint32_t player;
            Position position;
            int character;
            InfectionClock::time_point deadline;
        };
        struct InfectionCorpse
        {
            Position position;
            int character;
            InfectionClock::time_point deadline;
        };
        void infection_pills(int user_id);
        bool tell_infection(int user_id, const Infection& infection, bool turned, bool clear);
        void infect_turn(int user_id, const Infection& infection);
        void resolve_turns(InfectionClock::time_point now);
        void remove_infection_corpses(InfectionClock::time_point now);
        void infection_voice(const Infection& infection, CBaseEntity* player, bool cough, float stage) const;
        CBaseEntity* infection_player(int user_id, edict_t* edict, std::uint32_t handle) const;
        static void rescue_call_hook(CBaseEntity* rescue, CBaseEntity* survivor);

        linux_sdk::RescueCall rescue_call_ = nullptr;
        linux_memory::SavedFrameHook<linux_sdk::RescueCall> rescue_hook_;
        linux_sdk::CommitSuicide commit_suicide_ = nullptr;
        linux_sdk::SpawnSpecial spawn_special_ = nullptr;
        linux_sdk::PrecacheSound precache_sound_ = nullptr;
        linux_sdk::EmitSound emit_sound_ = nullptr;
        std::unordered_map<int, Infection> infections_;
        std::unordered_map<int, TurningSurvivor> turning_;
        std::unordered_set<int> no_rescue_;
        std::vector<InfectionCorpse> infection_corpses_;

        IServerGameEnts* entities_ = nullptr;
        IServerTools* tools_ = nullptr;
        IVEngineServer* engine_ = nullptr;
        IPlayerInfoManager* players_ = nullptr;
        std::function<inv::Inventory*(edict_t*)> inventory_lookup_;
        std::unordered_map<std::uint32_t, AmmoPile> piles_;
        std::unordered_map<std::uint32_t, GlowState> ping_glows_;
        std::unordered_map<void*, ReloadTable> reload_tables_;
        std::unordered_map<std::uint32_t, std::uint32_t> unloaded_;
        std::unordered_map<int, StaminaState> stamina_;
        std::unordered_map<linux_sdk::OnTakeDamage*, linux_sdk::OnTakeDamage> damage_vtables_;
        std::optional<float> last_stamina_frame_;
        float scan_damage_at_ = -1.f;
        bool any_exhausted_ = false;
        linux_sdk::PileUse* pile_use_slot_ = nullptr;
        linux_sdk::PileUse original_pile_use_ = nullptr;
        linux_sdk::DropToFloor drop_to_floor_ = nullptr;
        linux_sdk::PileUse weapon_spawn_use_ = nullptr;
        linux_sdk::Classname spawned_weapon_name_ = nullptr;
        linux_sdk::Classname melee_spawn_name_ = nullptr;
        linux_sdk::PileUse gun_use_ = nullptr;
        linux_sdk::BumpWeapon original_bump_ = nullptr;
        linux_sdk::BumpWeapon* bump_slot_ = nullptr;
        linux_sdk::ZombieClass zombie_class_ = nullptr;
        linux_sdk::SetShovePenalty set_shove_penalty_ = nullptr;
        linux_sdk::AdrenalineActive adrenaline_active_ = nullptr;
        linux_sdk::SetGlowValue set_glow_state_ = nullptr;
        linux_sdk::SetGlowValue set_glow_range_ = nullptr;
        linux_sdk::SetGlowValue set_glow_minimum_ = nullptr;
        linux_sdk::SetGlowColor set_glow_color_ = nullptr;
        std::function<void(edict_t*, std::string)> pickup_notice_;
        std::unordered_map<linux_sdk::PileUse*, linux_sdk::PileUse> pickup_vtables_;
        mutable std::unordered_set<std::uint32_t> known_weapons_;
        mutable CBaseEntity* giving_to_ = nullptr;
        CBaseEntity* using_weapon_ = nullptr;
        bool auto_store_ = false;
        std::uintptr_t server_base_ = 0;
        linux_sdk::Reload gun_reload_ = nullptr;
        linux_sdk::Reload shotgun_reload_ = nullptr;
        linux_sdk::FireOnEmpty fire_empty_ = nullptr;
        linux_sdk::WeaponInSlot weapon_in_slot_ = nullptr;
        linux_sdk::ActiveWeapon active_weapon_ = nullptr;
        linux_sdk::Classname classname_ = nullptr;
        linux_sdk::SelectItem select_item_ = nullptr;
        linux_sdk::EyeVectors eye_vectors_ = nullptr;
        linux_sdk::Origin origin_ = nullptr;
        linux_sdk::Owner owner_ = nullptr;
        linux_sdk::NetworkProperty network_property_ = nullptr;
        linux_sdk::MarkedForDeletion marked_for_deletion_ = nullptr;
        linux_sdk::Remove remove_ = nullptr;
        linux_sdk::QuietDrop quiet_drop_ = nullptr;
        linux_sdk::QuietDrop weapon_drop_ = nullptr;
        linux_sdk::GiveItem give_item_ = nullptr;
        linux_sdk::CannotHandleItems incapacitated_ = nullptr;
        linux_sdk::CannotHandleItems dominated_ = nullptr;
        linux_sdk::SetAmmoCount set_ammo_count_ = nullptr;
        linux_sdk::Equip equip_ = nullptr;
        linux_sdk::NetworkChanged network_changed_ = nullptr;
        linux_sdk::Classname melee_script_name_ = nullptr;
        linux_sdk::SetOrigin set_origin_ = nullptr;
        linux_sdk::DispatchSpawn dispatch_spawn_ = nullptr;
    };
}
