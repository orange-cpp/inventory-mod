#include "server/linux_game.hpp"
#include "server/linux_memory.hpp"
#include "inv/rules.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <link.h>
#include <random>
#include <utility>
#include <vector>

#include "eiface.h"
#include "engine/IEngineTrace.h"
#include "game/server/iplayerinfo.h"
#include "toolframework/itoolentity.h"

namespace
{
    using game::linux_memory::field;
    using game::linux_memory::server_vtable_slot;
    using game::linux_memory::write_vtable_entry;
    constexpr int ping_glow_type = 3;
    constexpr std::uint32_t ping_glow_color = 235u | (70u << 8) | (60u << 16);

    class PingTraceFilter final : public CTraceFilter
    {
    public:
        explicit PingTraceFilter(IHandleEntity* ignored) : ignored_(ignored) {}
        bool ShouldHitEntity(IHandleEntity* entity, int) override { return entity != ignored_; }

    private:
        IHandleEntity* ignored_;
    };

    bool melee_weapon(const inv::ItemDef& def)
    {
        return std::strcmp(def.classname, "weapon_melee") == 0;
    }

    bool firearm(const inv::ItemDef& def)
    {
        return def.weapon() && !melee_weapon(def) && std::strcmp(def.classname, "weapon_chainsaw") != 0;
    }

    std::minstd_rand& pile_random()
    {
        static std::minstd_rand random(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
        return random;
    }

    std::pair<int, int> roll_pile(const inv::AmmoPileRules& rules)
    {
        auto& random = pile_random();
        const std::string& kind = rules.kinds[random() % rules.kinds.size()];
        std::uniform_int_distribution<int> magazines(rules.min_magazines, rules.max_magazines);
        return {inv::ammo_index(kind), magazines(random) * std::max(1, inv::ammo_magazine(kind))};
    }

    std::size_t align_note(std::size_t size)
    {
        return (size + 3u) & ~std::size_t{3};
    }

    bool correct_build(const dl_phdr_info& module)
    {
        for (int i = 0; i < module.dlpi_phnum; ++i)
        {
            const auto& segment = module.dlpi_phdr[i];
            if (segment.p_type != PT_NOTE)
                continue;

            const auto* position = reinterpret_cast<const unsigned char*>(module.dlpi_addr + segment.p_vaddr);
            std::size_t remaining = segment.p_filesz;
            while (remaining >= sizeof(ElfW(Nhdr)))
            {
                ElfW(Nhdr) note{};
                std::memcpy(&note, position, sizeof(note));
                position += sizeof(note);
                remaining -= sizeof(note);
                if (note.n_namesz > remaining)
                    break;
                const std::size_t name_size = align_note(note.n_namesz);
                if (name_size > remaining || note.n_descsz > remaining - name_size)
                    break;
                const auto& expected = game::linux_sdk::server_build_id;
                if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
                    std::memcmp(position, "GNU", 4) == 0)
                    return note.n_descsz == expected.size() &&
                           std::memcmp(position + name_size, expected.data(), expected.size()) == 0;

                const std::size_t descriptor_size = align_note(note.n_descsz);
                if (descriptor_size > remaining - name_size)
                    break;
                position += name_size + descriptor_size;
                remaining -= name_size + descriptor_size;
            }
        }
        return false;
    }

    template <typename Function>
    Function resolve(const dl_phdr_info& module, const game::linux_sdk::Entry<Function>& entry)
    {
        for (int i = 0; i < module.dlpi_phnum; ++i)
        {
            const auto& segment = module.dlpi_phdr[i];
            if (segment.p_type != PT_LOAD || (segment.p_flags & (PF_X | PF_R)) != (PF_X | PF_R) ||
                entry.offset < segment.p_vaddr || entry.prologue.size() > segment.p_memsz ||
                entry.offset - segment.p_vaddr > segment.p_memsz - entry.prologue.size())
                continue;
            const auto* address = reinterpret_cast<const void*>(module.dlpi_addr + entry.offset);
            if (std::memcmp(address, entry.prologue.begin(), entry.prologue.size()) == 0)
                return reinterpret_cast<Function>(module.dlpi_addr + entry.offset);
        }
        return nullptr;
    }
}

namespace game
{
    LinuxGame* LinuxGame::ammo_game_ = nullptr;

    bool LinuxGame::bind(IServerGameEnts* entities, IServerTools* tools)
    {
        entities_ = entities;
        tools_ = tools;
        if (entities_ == nullptr || tools_ == nullptr)
            return false;
        return dl_iterate_phdr([](dl_phdr_info* module, std::size_t, void* context) -> int {
            const std::string_view path(module->dlpi_name);
            if (path.substr(path.find_last_of('/') + 1) != "server_srv.so" || !correct_build(*module))
                return 0;
            auto& game = *static_cast<LinuxGame*>(context);
            game.server_base_ = module->dlpi_addr;
            game.weapon_in_slot_ = resolve(*module, linux_sdk::weapon_in_slot);
            game.active_weapon_ = resolve(*module, linux_sdk::active_weapon);
            game.classname_ = resolve(*module, linux_sdk::classname);
            game.select_item_ = resolve(*module, linux_sdk::select_item);
            game.eye_vectors_ = resolve(*module, linux_sdk::eye_vectors);
            game.origin_ = resolve(*module, linux_sdk::origin);
            game.owner_ = resolve(*module, linux_sdk::owner);
            game.network_property_ = resolve(*module, linux_sdk::network_property);
            game.marked_for_deletion_ = resolve(*module, linux_sdk::marked_for_deletion);
            game.remove_ = resolve(*module, linux_sdk::remove);
            game.quiet_drop_ = resolve(*module, linux_sdk::quiet_drop);
            game.weapon_drop_ = resolve(*module, linux_sdk::weapon_drop);
            game.give_item_ = resolve(*module, linux_sdk::give_item);
            game.incapacitated_ = resolve(*module, linux_sdk::incapacitated);
            game.dominated_ = resolve(*module, linux_sdk::dominated);
            game.set_ammo_count_ = resolve(*module, linux_sdk::set_ammo_count);
            game.equip_ = resolve(*module, linux_sdk::equip);
            game.network_changed_ = resolve(*module, linux_sdk::network_changed);
            game.melee_script_name_ = resolve(*module, linux_sdk::melee_script_name);
            game.set_origin_ = resolve(*module, linux_sdk::set_origin);
            game.dispatch_spawn_ = resolve(*module, linux_sdk::dispatch_spawn);
            game.drop_to_floor_ = resolve(*module, linux_sdk::drop_to_floor);
            game.weapon_spawn_use_ = resolve(*module, linux_sdk::weapon_spawn_use);
            game.spawned_weapon_name_ = resolve(*module, linux_sdk::spawned_weapon_name);
            game.gun_reload_ = resolve(*module, linux_sdk::gun_reload);
            game.shotgun_reload_ = resolve(*module, linux_sdk::shotgun_reload);
            game.fire_empty_ = resolve(*module, linux_sdk::fire_empty);
            game.original_bump_ = resolve(*module, linux_sdk::bump_weapon);
            game.gun_use_ = resolve(*module, linux_sdk::gun_use);
            game.melee_spawn_name_ = resolve(*module, linux_sdk::melee_spawn_name);
            game.zombie_class_ = resolve(*module, linux_sdk::zombie_class);
            game.set_shove_penalty_ = resolve(*module, linux_sdk::set_shove_penalty);
            game.adrenaline_active_ = resolve(*module, linux_sdk::adrenaline_active);
            game.original_chainsaw_frame_ = resolve(*module, linux_sdk::chainsaw_frame);
            auto* saw_slot = reinterpret_cast<linux_sdk::ItemPostFrame*>(module->dlpi_addr + linux_sdk::chainsaw_post_frame_slot);
            if (game.original_chainsaw_frame_ && *saw_slot == game.original_chainsaw_frame_)
                game.chainsaw_frame_slot_ = saw_slot;
            game.on_revived_ = resolve(*module, linux_sdk::on_revived);
            game.on_adrenaline_used_ = resolve(*module, linux_sdk::on_adrenaline_used);
            game.panic_event_ = resolve(*module, linux_sdk::panic_event);
            game.rescue_call_ = resolve(*module, linux_sdk::rescue_call);
            game.commit_suicide_ = resolve(*module, linux_sdk::commit_suicide);
            game.original_melee_primary_ = resolve(*module, linux_sdk::melee_primary);
            game.original_melee_secondary_ = resolve(*module, linux_sdk::melee_secondary);
            auto* primary_slot = reinterpret_cast<linux_sdk::ItemPostFrame*>(module->dlpi_addr + linux_sdk::melee_primary_slot);
            auto* secondary_slot = reinterpret_cast<linux_sdk::ItemPostFrame*>(module->dlpi_addr + linux_sdk::melee_secondary_slot);
            if (game.original_melee_primary_ && *primary_slot == game.original_melee_primary_)
                game.melee_primary_slot_ = primary_slot;
            if (game.original_melee_secondary_ && *secondary_slot == game.original_melee_secondary_)
                game.melee_secondary_slot_ = secondary_slot;
            game.view_punch_ = resolve(*module, linux_sdk::view_punch);
            game.stagger_player_ = resolve(*module, linux_sdk::stagger_player);
            game.shove_responder_ = resolve(*module, linux_sdk::shove_responder);
            game.melee_info_ = resolve(*module, linux_sdk::melee_info);
            game.animation_event_ = resolve(*module, linux_sdk::animation_event);
            game.original_melee_sound_ = resolve(*module, linux_sdk::weapon_sound);
            auto* sound_slot = reinterpret_cast<linux_sdk::WeaponSound*>(module->dlpi_addr + linux_sdk::melee_sound_slot);
            if (game.original_melee_sound_ && *sound_slot == game.original_melee_sound_)
                game.melee_sound_slot_ = sound_slot;
            game.teleport_ = resolve(*module, linux_sdk::teleport);
            game.damage_info_ = resolve(*module, linux_sdk::damage_info);
            game.take_damage_ = resolve(*module, linux_sdk::take_damage);
            game.spawn_special_ = resolve(*module, linux_sdk::spawn_special);
            game.precache_sound_ = resolve(*module, linux_sdk::precache_sound);
            game.emit_sound_ = resolve(*module, linux_sdk::emit_sound);
            game.hanging_from_ledge_ = resolve(*module, linux_sdk::hanging_from_ledge);
            game.set_glow_state_ = resolve(*module, linux_sdk::set_glow_state);
            game.set_glow_range_ = resolve(*module, linux_sdk::set_glow_range);
            game.set_glow_minimum_ = resolve(*module, linux_sdk::set_glow_minimum);
            game.set_glow_color_ = resolve(*module, linux_sdk::set_glow_color);
            auto* bump_slot = reinterpret_cast<linux_sdk::BumpWeapon*>(module->dlpi_addr + linux_sdk::bump_weapon_slot);
            if (game.original_bump_ != nullptr && *bump_slot == game.original_bump_)
                game.bump_slot_ = bump_slot;
            game.original_pile_use_ = resolve(*module, linux_sdk::pile_use);
            auto* slot = reinterpret_cast<linux_sdk::PileUse*>(module->dlpi_addr + linux_sdk::pile_use_slot);
            if (game.original_pile_use_ != nullptr && *slot == game.original_pile_use_)
                game.pile_use_slot_ = slot;
            return game.weapon_in_slot_ && game.active_weapon_ && game.classname_ && game.select_item_ &&
                   game.eye_vectors_ && game.origin_ && game.owner_ && game.network_property_ &&
                   game.marked_for_deletion_ && game.remove_ && game.quiet_drop_ && game.weapon_drop_ && game.give_item_ &&
                   game.incapacitated_ && game.dominated_ && game.set_ammo_count_ && game.equip_ &&
                   game.network_changed_ && game.melee_script_name_ && game.set_origin_ && game.dispatch_spawn_;
        }, this) != 0;
    }

    bool LinuxGame::trigger_panic() const
    {
        if (!panic_event_)
            return false;
        auto* director = *reinterpret_cast<void* const*>(server_base_ + linux_sdk::director);
        if (director == nullptr)
            return false;
        // The stock director_force_panic_event command passes no activator and reason 0.
        panic_event_(director, nullptr, 0);
        return true;
    }

    bool LinuxGame::install_pickups(bool auto_store, std::function<void(edict_t*, std::string)> pickup_notice)
    {
        pickup_notice_ = std::move(pickup_notice);
        if (ammo_game_ != this || bump_slot_ == nullptr || gun_use_ == nullptr || weapon_spawn_use_ == nullptr)
            return false;
        auto_store_ = auto_store;
        return write_vtable_entry(bump_slot_, bump_weapon_hook);
    }

    void LinuxGame::uninstall_pickups()
    {
        for (const auto& [slot, original] : pickup_vtables_)
            if (*slot == world_weapon_use_hook || *slot == weapon_spawner_use_hook)
                write_vtable_entry(slot, original);
        pickup_vtables_.clear();
        if (bump_slot_ != nullptr && *bump_slot_ == bump_weapon_hook)
            write_vtable_entry(bump_slot_, original_bump_);
        known_weapons_.clear();
    }

    void LinuxGame::find_pickup_entities()
    {
        if (bump_slot_ == nullptr || *bump_slot_ != bump_weapon_hook)
            return;
        for (void* current = tools_->FirstEntity(); current != nullptr; current = tools_->NextEntity(current))
        {
            auto* entity = static_cast<CBaseEntity*>(current);
            const std::string_view classname(classname_(entity));
            if (!classname.starts_with("weapon_"))
                continue;
            auto* slot = field<linux_sdk::PileUse*>(entity, 0) + linux_sdk::use_index;
            if (!server_vtable_slot(server_base_, slot))
                continue;
            const auto original = *slot;
            if (original != weapon_spawn_use_ && original != gun_use_)
                continue;
            const auto hook = original == weapon_spawn_use_ ? weapon_spawner_use_hook : world_weapon_use_hook;
            const auto saved = pickup_vtables_.emplace(slot, original);
            if (saved.second && !write_vtable_entry(slot, hook))
                pickup_vtables_.erase(saved.first);
        }
    }

    void LinuxGame::restart_pickups()
    {
        known_weapons_.clear();
    }

    std::optional<LinuxGame::PingTarget> LinuxGame::ping_target(IEngineTrace* trace, edict_t* player, int team, float range) const
    {
        auto* pinger = entities_->EdictToBaseEntity(player);
        if (trace == nullptr || pinger == nullptr)
            return std::nullopt;
        Vector eye, forward;
        eye_vectors_(pinger, &eye, &forward, nullptr, nullptr);
        Vector end(eye.x + forward.x * range, eye.y + forward.y * range, eye.z + forward.z * range);
        Ray_t ray;
        ray.Init(eye, end);
        PingTraceFilter filter(player->GetIServerEntity());
        trace_t hit{};
        trace->TraceRay(ray, MASK_SHOT, &filter, &hit);
        if (hit.startsolid)
            return std::nullopt;
        if (hit.m_pEnt != nullptr && hit.m_pEnt != pinger)
            if (auto direct = ping_entity(hit.m_pEnt, team))
                return direct;

        // A small pickup can sit just below the ray, and some spawners do not stop that ray.
        std::optional<PingTarget> best;
        float best_score = std::numeric_limits<float>::infinity();
        const float reach = range * std::clamp(hit.fraction, 0.f, 1.f) + 8.f;
        for (void* current = tools_->FirstEntity(); current != nullptr; current = tools_->NextEntity(current))
        {
            auto* entity = static_cast<CBaseEntity*>(current);
            if (entity == pinger)
                continue;
            auto candidate = ping_entity(entity, team);
            if (!candidate)
                continue;
            const auto& pos = candidate->position;
            const float x = pos[0] - eye.x, y = pos[1] - eye.y, z = pos[2] - eye.z;
            const float along = x * forward.x + y * forward.y + z * forward.z;
            if (along < 1.f || along > reach)
                continue;
            const float slack = (candidate->kind == 2 ? 30.f : 18.f) + std::min(along * 0.025f, 26.f);
            const float sideways = x * x + y * y + z * z - along * along;
            const float score = sideways + along * 0.1f;
            if (sideways <= slack * slack && (!best || candidate->kind > best->kind ||
                (candidate->kind == best->kind && score < best_score)))
            {
                best_score = score;
                best = std::move(candidate);
            }
        }
        if (best)
            return best;
        if (hit.fraction >= 1.f || (hit.surface.flags & SURF_SKY))
            return std::nullopt;
        return PingTarget{{hit.endpos.x + 6.f * hit.plane.normal.x, hit.endpos.y + 6.f * hit.plane.normal.y,
                           hit.endpos.z + 6.f * hit.plane.normal.z}, 0, {}, std::nullopt};
    }

    std::optional<LinuxGame::PingTarget> LinuxGame::ping_entity(CBaseEntity* target, int team) const
    {
        const std::string_view classname(classname_(target));
        PingTarget mark{{}, 0, {}, std::nullopt};
        const bool weapon = classname.starts_with("weapon_");
        if (classname == "weapon_ammo_spawn")
            mark.name = "Ammo";
        else if (weapon)
        {
            if (weapon_spawner(target))
            {
                if (const auto* def = spawner_item(target))
                    mark.name = def->name;
            }
            else if (const auto* def = inv::find_item(classname); def != nullptr && owner_(target) == nullptr)
                mark.name = def->name;
        }
        if (!mark.name.empty())
        {
            mark.kind = 1;
            const auto& pos = origin_(target);
            mark.position = {pos.x, pos.y, pos.z + 10.f};
            return mark;
        }

        if (team == 2 && (classname == "infected" || classname == "witch"))
            mark.name = classname == "witch" ? "Witch" : "Common";
        else if (classname == "player")
        {
            auto* other = entities_->BaseEntityToEdict(target);
            auto* info = other == nullptr ? nullptr : players_->GetPlayerInfo(other);
            if (info != nullptr && !info->IsDead() && info->GetTeamIndex() == (team == 2 ? 3 : 2))
            {
                constexpr std::array names{"Special infected", "Smoker", "Boomer", "Hunter", "Spitter",
                                           "Jockey", "Charger", "Witch", "Tank"};
                const int zombie = zombie_class_ ? zombie_class_(target) : 0;
                mark.name = team == 3 ? "Survivor" : names[std::clamp(zombie, 0, 8)];
            }
        }
        if (!mark.name.empty() && field<unsigned char>(target, linux_sdk::life_state) == 0)
        {
            mark.kind = 2;
            mark.follow = field<std::uint32_t>(target, linux_sdk::entity_handle);
            const auto& pos = origin_(target);
            mark.position = {pos.x, pos.y, pos.z + 40.f};
            return mark;
        }
        return std::nullopt;
    }

    CBaseEntity* LinuxGame::find_entity(std::uint32_t handle) const
    {
        if (handle == 0xffffffffu)
            return nullptr;
        const auto list = *reinterpret_cast<const std::uintptr_t*>(server_base_ + linux_sdk::entity_list);
        if (list == 0)
            return nullptr;
        const auto entry = list + 4u + 16u * (handle & 0xfffu);
        if (*reinterpret_cast<const std::uint32_t*>(entry + 4) != (handle >> 12))
            return nullptr;
        auto* entity = *reinterpret_cast<CBaseEntity* const*>(entry);
        return entity && !marked_for_deletion_(network_property_(entity)) ? entity : nullptr;
    }

    std::optional<LinuxGame::Position> LinuxGame::ping_follow(std::uint32_t handle) const
    {
        auto* entity = find_entity(handle);
        if (entity == nullptr || field<unsigned char>(entity, linux_sdk::life_state))
            return std::nullopt;
        const auto& origin = origin_(entity);
        return Position{origin.x, origin.y, origin.z + 40.f};
    }

    bool LinuxGame::light_ping_target(std::uint32_t handle)
    {
        if (auto entry = ping_glows_.find(handle); entry != ping_glows_.end())
        {
            ++entry->second.users;
            return true;
        }
        auto* entity = find_entity(handle);
        if (!entity || !set_glow_state_ || !set_glow_range_ || !set_glow_minimum_ || !set_glow_color_)
            return false;
        constexpr auto glow = linux_sdk::glow;
        ping_glows_.emplace(handle, GlowState{.type = field<int>(entity, glow + 4),
                                            .range = field<int>(entity, glow + 8),
                                            .minimum = field<int>(entity, glow + 12),
                                            .color = field<std::uint32_t>(entity, glow + 16),
                                            .flashing = field<unsigned char>(entity, glow + 20)});
        auto* property = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(entity) + glow);
        set_glow_state_(property, ping_glow_type);
        set_glow_range_(property, 0);
        set_glow_minimum_(property, 0);
        set_glow_color_(property, ping_glow_color);
        field<unsigned char>(entity, glow + 20) = 0;
        network_changed_(entity);
        return true;
    }

    void LinuxGame::unlight_ping_target(std::uint32_t handle)
    {
        const auto entry = ping_glows_.find(handle);
        if (entry == ping_glows_.end() || --entry->second.users > 0)
            return;
        const GlowState before = entry->second;
        ping_glows_.erase(entry);
        auto* entity = find_entity(handle);
        constexpr auto glow = linux_sdk::glow;
        if (!entity || field<int>(entity, glow + 4) != ping_glow_type ||
            field<std::uint32_t>(entity, glow + 16) != ping_glow_color)
            return;
        auto* property = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(entity) + glow);
        set_glow_state_(property, before.type);
        set_glow_range_(property, before.range);
        set_glow_minimum_(property, before.minimum);
        set_glow_color_(property, before.color);
        field<unsigned char>(entity, glow + 20) = before.flashing;
        network_changed_(entity);
    }

    void LinuxGame::forget_ping_glows()
    {
        ping_glows_.clear();
    }

    bool LinuxGame::install_ammo(IVEngineServer* engine, IPlayerInfoManager* players,
                                 std::function<inv::Inventory*(edict_t*)> inventory_lookup)
    {
        if (ammo_game_ != nullptr || pile_use_slot_ == nullptr || drop_to_floor_ == nullptr)
            return false;
        engine_ = engine;
        players_ = players;
        inventory_lookup_ = std::move(inventory_lookup);
        ammo_game_ = this;
        if (write_vtable_entry(pile_use_slot_, pile_use_hook))
            return true;
        ammo_game_ = nullptr;
        return false;
    }

    void LinuxGame::uninstall_ammo()
    {
        if (ammo_game_ != this)
            return;
        clear_piles(true);
        if (*pile_use_slot_ == pile_use_hook && !write_vtable_entry(pile_use_slot_, original_pile_use_))
            std::fprintf(stderr, "inventory_mod: couldn't restore the original ammo-pile handler\n");
        for (auto& [table, saved] : reload_tables_)
        {
            auto* reload = static_cast<linux_sdk::Reload*>(table) + linux_sdk::reload_index;
            auto* fire = static_cast<linux_sdk::FireOnEmpty*>(table) + linux_sdk::fire_empty_index;
            if (*reload == reload_hook)
                write_vtable_entry(reload, saved.reload);
            if (*fire == fire_empty_hook)
                write_vtable_entry(fire, saved.fire);
        }
        reload_tables_.clear();
        ammo_game_ = nullptr;
        inventory_lookup_ = {};
    }

    void LinuxGame::clear_piles(bool remove_spawned)
    {
        if (remove_spawned && !piles_.empty())
        {
            for (void* current = tools_->FirstEntity(); current != nullptr; current = tools_->NextEntity(current))
            {
                auto* entity = static_cast<CBaseEntity*>(current);
                const auto pile = piles_.find(field<std::uint32_t>(entity, linux_sdk::entity_handle));
                if (pile != piles_.end() && pile->second.spawned)
                    remove_(entity);
            }
        }
        piles_.clear();
        unloaded_.clear();
    }

    std::array<NativeWeapon, k_native_slots> LinuxGame::native_weapons(edict_t* player) const
    {
        std::array<NativeWeapon, k_native_slots> list;
        CBaseEntity* entity = entities_->EdictToBaseEntity(player);
        if (entity == nullptr)
            return list;
        const CBaseEntity* active = active_weapon_(entity);
        for (int slot = 0; slot < k_native_slots; ++slot)
        {
            if (CBaseEntity* weapon = weapon_in_slot_(entity, slot))
            {
                list[slot].classname = classname_(weapon);
                list[slot].active = weapon == active;
                if (list[slot].classname == "weapon_melee")
                {
                    list[slot].melee = melee_script_name_(weapon);
                    list[slot].wear = weapon_wear(weapon);
                }
            }
        }
        return list;
    }

    float LinuxGame::rounds_weight(edict_t* player) const
    {
        CBaseEntity* entity = entities_->EdictToBaseEntity(player);
        return entity == nullptr ? 0.f : inv::ammo_weight(field<int[32]>(entity, linux_sdk::player_ammo));
    }

    LinuxGame::Reserve LinuxGame::read_reserve(edict_t* player) const
    {
        Reserve reserve{};
        if (CBaseEntity* entity = entities_->EdictToBaseEntity(player))
            std::copy_n(field<int[32]>(entity, linux_sdk::player_ammo), reserve.size(), reserve.begin());
        return reserve;
    }

    void LinuxGame::restore_reserve(edict_t* player, const Reserve& reserve) const
    {
        if (CBaseEntity* entity = entities_->EdictToBaseEntity(player))
        {
            int pool[32];
            std::copy(reserve.begin(), reserve.end(), pool);
            restore_rounds(entity, pool);
        }
    }

    void LinuxGame::normalize_ammo(edict_t* player) const
    {
        CBaseEntity* entity = entities_->EdictToBaseEntity(player);
        if (entity == nullptr)
            return;
        for (int slot = 0; slot < 2; ++slot)
        {
            CBaseEntity* weapon = weapon_in_slot_(entity, slot);
            if (weapon == nullptr)
                continue;
            int& type = field<int>(weapon, linux_sdk::primary_ammo_type);
            const int shared = type == 8 ? 7 : type == 1 ? 5 : type == 2 ? 10 : type;
            if (type != shared)
            {
                type = shared;
                network_changed_(weapon);
            }
        }
        const int* reserve = field<int[32]>(entity, linux_sdk::player_ammo);
        if (reserve[8] > 0)
        {
            const int incoming = std::min(reserve[8], std::max(0, inv::ammo_max("shotgun") - reserve[7]));
            set_ammo_count_(entity, reserve[7] + incoming, 7);
            set_ammo_count_(entity, 0, 8);
        }
        for (const char* kind : inv::ammo_kinds())
        {
            const int type = inv::ammo_index(kind);
            const int cap = inv::ammo_max(kind);
            if (inv::ammo_max_set(kind) && reserve[type] > cap)
                set_ammo_count_(entity, cap, type);
        }
    }

    void LinuxGame::remember_carried_weapons(edict_t* player) const
    {
        if (!inv::map_gun_rules().enabled)
            return;
        CBaseEntity* entity = entities_->EdictToBaseEntity(player);
        if (entity != nullptr)
            for (int slot = 0; slot < 2; ++slot)
                if (auto* gun = weapon_in_slot_(entity, slot))
                    remember_weapon(gun);
    }

    void LinuxGame::remember_weapon(CBaseEntity* weapon) const
    {
        if (inv::map_gun_rules().enabled)
            known_weapons_.insert(field<std::uint32_t>(weapon, linux_sdk::entity_handle));
    }

    bool LinuxGame::cut_map_weapon(CBaseEntity* gun, const inv::ItemDef& def) const
    {
        const auto& rules = inv::map_gun_rules();
        if (!rules.enabled || !firearm(def) || inv::ammo_kind(def.classname) == nullptr ||
            !known_weapons_.insert(field<std::uint32_t>(gun, linux_sdk::entity_handle)).second)
            return false;
        auto& clip = field<int>(gun, linux_sdk::clip);
        if (clip > 0)
        {
            const int percent = std::uniform_int_distribution<int>(rules.min_clip, rules.max_clip)(pile_random());
            clip = static_cast<int>(std::lround(clip * (percent / 100.0)));
        }
        field<int>(gun, linux_sdk::extra_primary_ammo) = 0;
        network_changed_(gun);
        return true;
    }

    void LinuxGame::give_to_player(CBaseEntity* player, const char* name) const
    {
        const auto previous = std::exchange(giving_to_, player);
        give_item_(player, name, 0, false, nullptr);
        giving_to_ = previous;
    }

    inv::WeaponState LinuxGame::weapon_state(CBaseEntity* weapon, const inv::ItemDef& def, bool carried) const
    {
        inv::WeaponState state;
        state.clip = field<int>(weapon, linux_sdk::clip);
        if (melee_weapon(def))
        {
            state.melee = melee_script_name_(weapon);
            state.wear = weapon_wear(weapon);
        }
        if (firearm(def))
        {
            state.upgrades = field<int>(weapon, linux_sdk::upgrades) & 7;
            state.special = field<unsigned char>(weapon, linux_sdk::special_rounds);
            if (!carried)
                state.ammo = field<int>(weapon, linux_sdk::extra_primary_ammo);
        }
        return state;
    }

    void LinuxGame::apply_weapon_state(CBaseEntity* weapon, const inv::ItemDef& def, const inv::WeaponState& state,
                                       bool on_ground) const
    {
        if (melee_weapon(def))
            set_weapon_wear(weapon, state.wear);
        if (state.clip >= 0 && !melee_weapon(def))
            field<int>(weapon, linux_sdk::clip) = state.clip;
        if (firearm(def))
        {
            field<int>(weapon, linux_sdk::upgrades) = state.upgrades & 7;
            field<unsigned char>(weapon, linux_sdk::special_rounds) =
                static_cast<unsigned char>(std::clamp(state.special, 0, 255));
            if (on_ground && state.ammo >= 0)
                field<int>(weapon, linux_sdk::extra_primary_ammo) = state.ammo;
        }
        network_changed_(weapon);
    }

    void LinuxGame::restore_rounds(CBaseEntity* player, const int (&pool)[32]) const
    {
        const auto& current = field<int[32]>(player, linux_sdk::player_ammo);
        for (int type = 0; type < 32; ++type)
            if (current[type] != pool[type])
                set_ammo_count_(player, pool[type], type);
    }

    void LinuxGame::add_rounds(CBaseEntity* player, CBaseEntity* weapon, const inv::ItemDef& def, int rounds) const
    {
        int type = field<int>(weapon, linux_sdk::primary_ammo_type);
        if (type == 8)
            type = 7;
        if (rounds <= 0 || type < 0 || type >= 32)
            return;
        const char* kind = inv::ammo_kind(def.classname);
        const int cap = kind == nullptr ? -1 : inv::ammo_max(kind);
        const int count = field<int[32]>(player, linux_sdk::player_ammo)[type];
        if (cap >= 0)
            rounds = std::min(rounds, std::max(0, cap - count));
        else if (count > 0)
            rounds = std::min(rounds, std::numeric_limits<int>::max() - count);
        if (rounds > 0)
            set_ammo_count_(player, count + rounds, type);
    }

    std::string LinuxGame::select(edict_t* player, std::string_view classname, inv::Inventory& inventory)
    {
        CBaseEntity* entity = entities_->EdictToBaseEntity(player);
        if (entity != nullptr)
            if (const auto* item = inv::find_item(classname))
                if (auto revived = start_revive(player, entity, *item, -1, inventory))
                    return *revived;
        if (!can_handle_items(entity))
            return {};
        for (int slot = 0; slot < k_native_slots; ++slot)
        {
            CBaseEntity* weapon = weapon_in_slot_(entity, slot);
            if (weapon != nullptr && classname_(weapon) == classname)
            {
                put_away_carried(entity, classname, inventory);
                select_item_(entity, classname_(weapon), 0);
                return {};
            }
        }
        return {};
    }

    void LinuxGame::put_away_carried(CBaseEntity* player, std::string_view switching_to, inv::Inventory& inventory) const
    {
        constexpr int carry_slot = 5;
        if (CBaseEntity* carried = weapon_in_slot_(player, carry_slot))
        {
            const auto* def = inv::find_item(classname_(carried));
            if (def != nullptr && def->classname != switching_to)
                store_item(player, carried, *def, inventory);
        }
    }

    bool LinuxGame::can_handle_items(CBaseEntity* player) const
    {
        return player != nullptr && !incapacitated_(player) && !dominated_(player);
    }

    bool LinuxGame::release_carried(CBaseEntity* player, CBaseEntity* weapon, const inv::ItemDef& def,
                                    linux_sdk::QuietDrop drop_method) const
    {
        int pool[32];
        std::memcpy(pool, field<int[32]>(player, linux_sdk::player_ammo), sizeof(pool));
        drop_method(player, weapon, nullptr, nullptr);
        restore_rounds(player, pool);
        if (weapon_in_slot_(player, def.game_slot) == weapon)
            return false;
        if (firearm(def) && !marked_for_deletion_(network_property_(weapon)))
        {
            field<int>(weapon, linux_sdk::extra_primary_ammo) = 0;
            network_changed_(weapon);
        }
        return true;
    }

    std::string LinuxGame::drop_weapon(edict_t* player, std::string_view classname) const
    {
        CBaseEntity* entity = entities_->EdictToBaseEntity(player);
        if (!can_handle_items(entity))
            return "The game does not let you drop now";
        const inv::ItemDef* def = inv::find_item(classname);
        CBaseEntity* weapon = def == nullptr ? nullptr : weapon_in_slot_(entity, def->game_slot);
        if (weapon == nullptr || classname_(weapon) != classname)
            return {};
        if (!release_carried(entity, weapon, *def, weapon_drop_))
            return "The game does not let you drop now";
        return std::string("Dropped ") + def->name;
    }

    bool LinuxGame::weapon_spawner(CBaseEntity* entity) const
    {
        const auto* vtable = field<const linux_sdk::PileUse*>(entity, 0);
        return weapon_spawn_use_ != nullptr && spawned_weapon_name_ != nullptr && vtable != nullptr &&
               (vtable[linux_sdk::use_index] == weapon_spawn_use_ ||
                vtable[linux_sdk::use_index] == weapon_spawner_use_hook);
    }

    const inv::ItemDef* LinuxGame::spawner_item(CBaseEntity* spawner) const
    {
        const char* alias = spawned_weapon_name_(spawner);
        if (alias == nullptr || *alias == '\0')
            return nullptr;
        if (std::string_view(alias).starts_with("weapon_"))
            return inv::find_item(alias);
        return inv::find_item(std::string("weapon_") + alias);
    }

    CBaseEntity* LinuxGame::item_in_front(const Vector& eye, const Vector& forward, float min_facing,
                                         bool (*eligible)(const inv::ItemDef&), bool include_spawners) const
    {
        CBaseEntity* closest = nullptr;
        float best_score = 320.f;
        for (void* current = tools_->FirstEntity(); current != nullptr; current = tools_->NextEntity(current))
        {
            auto* item = static_cast<CBaseEntity*>(current);
            const std::string_view classname(classname_(item));
            const bool spawner = include_spawners && classname.starts_with("weapon_") &&
                                 classname.ends_with("_spawn") && weapon_spawner(item);
            const inv::ItemDef* def = spawner ? spawner_item(item) : inv::find_item(classname);
            if (def == nullptr || !eligible(*def) || (!spawner && owner_(item) != nullptr) ||
                marked_for_deletion_(network_property_(item)))
                continue;
            const Vector& position = origin_(item);
            const float x = position.x - eye.x, y = position.y - eye.y, z = position.z - eye.z;
            const float distance = std::sqrt(x * x + y * y + z * z);
            if (distance < 1.f || distance > 160.f)
                continue;
            const float facing = (x * forward.x + y * forward.y + z * forward.z) / distance;
            const float score = distance * (2.f - facing);
            if (facing >= min_facing && score < best_score)
            {
                best_score = score;
                closest = item;
            }
        }
        return closest;
    }

    std::string LinuxGame::pickup(edict_t* player, inv::Inventory& inventory) const
    {
        CBaseEntity* entity = entities_->EdictToBaseEntity(player);
        if (!can_handle_items(entity))
            return "The game does not let you pick up items now";
        Vector eye, forward;
        eye_vectors_(entity, &eye, &forward, nullptr, nullptr);
        auto* closest = item_in_front(eye, forward, 0.6f, [](const inv::ItemDef&) { return true; });
        if (closest == nullptr)
            return "Nothing to pick up in front of you";
        const auto* candidate = inv::find_item(classname_(closest));
        cut_map_weapon(closest, *candidate);
        if (!store_world_item(entity, closest, *candidate, inventory))
            return inventory.has_box_for(*candidate) ? "Too heavy" :
                   candidate->weapon() ? "No free weapon box" : "The inventory is full";
        return std::string("Picked up ") + candidate->name;
    }

    bool LinuxGame::store_world_item(CBaseEntity* player, CBaseEntity* item, const inv::ItemDef& def,
                                     inv::Inventory& inventory) const
    {
        if (def.weapon())
        {
            inv::WeaponState state = weapon_state(item, def, false);
            const int reserve = state.ammo;
            state.ammo = -1;
            if (inventory.add_weapon(def, state) < 0)
                return false;
            add_rounds(player, item, def, reserve);
        }
        else if (inventory.add(def, 1, can_spent(item)) != 1)
            return false;
        set_can_spent(item, 0);
        remove_(item);
        return true;
    }

    edict_t* LinuxGame::survivor_edict(CBaseEntity* player) const
    {
        auto* edict = player == nullptr ? nullptr : entities_->BaseEntityToEdict(player);
        auto* info = edict == nullptr ? nullptr : players_->GetPlayerInfo(edict);
        return info != nullptr && info->GetTeamIndex() == 2 && !info->IsDead() ? edict : nullptr;
    }

    bool LinuxGame::wants_to_store(CBaseEntity* player, const inv::ItemDef& def) const
    {
        return weapon_in_slot_(player, def.game_slot) != nullptr || (!def.weapon() && auto_store_);
    }

    bool LinuxGame::weapon_over_limit(CBaseEntity* player, const inv::ItemDef& def, const inv::Inventory& inventory) const
    {
        const float limit = inventory.limits().max_weight;
        if (limit <= 0.f)
            return false;
        CBaseEntity* previous = weapon_in_slot_(player, def.game_slot);
        if (!def.weapon() && previous != nullptr)
            return false;
        const float old = previous ? inv::weapon_weight(classname_(previous)) : 0.f;
        return inventory.weight() - old + def.weight > limit + 1e-3f;
    }

    void LinuxGame::finish_weapon_pickup(edict_t* edict, CBaseEntity* player, CBaseEntity* incoming,
                                         CBaseEntity* previous, const inv::ItemDef& def, const Reserve& before,
                                         int incoming_rounds, bool fresh) const
    {
        normalize_ammo(edict);
        Reserve after = read_reserve(edict);
        const char* kind = inv::ammo_kind(def.classname);
        if (kind != nullptr && firearm(def))
        {
            const int type = inv::ammo_index(kind);
            const int capacity = std::max(0, inv::ammo_max(kind) - before[type]);
            if (inv::map_gun_rules().enabled && fresh)
            {
                const auto& rules = inv::map_gun_rules();
                const int magazines = std::uniform_int_distribution<int>(rules.min_reserve, rules.max_reserve)(pile_random());
                const int bonus = std::min(capacity, magazines * std::max(1, inv::ammo_magazine(kind)));
                after[type] = std::min(after[type], before[type] + bonus);
            }
            else if (incoming != nullptr && incoming_rounds > 0)
                after[type] = std::max(after[type], before[type] + std::min(capacity, incoming_rounds));
        }
        for (std::size_t type = 0; type < before.size(); ++type)
            after[type] = std::max(after[type], before[type]);
        restore_reserve(edict, after);

        if (previous != nullptr && previous != incoming && !marked_for_deletion_(network_property_(previous)) &&
            owner_(previous) != player)
        {
            if (const auto* old = inv::find_item(classname_(previous)); old && firearm(*old))
            {
                field<int>(previous, linux_sdk::extra_primary_ammo) = 0;
                network_changed_(previous);
            }
        }
        if (incoming != nullptr)
            remember_weapon(incoming);
    }

    bool LinuxGame::bump_weapon_hook(CBaseEntity* player, CBaseEntity* weapon)
    {
        return ammo_game_->bump_weapon(player, weapon);
    }

    void LinuxGame::world_weapon_use_hook(CBaseEntity* weapon, CBaseEntity* player, CBaseEntity* caller, int use_type, float value)
    {
        ammo_game_->use_world_weapon(weapon, player, caller, use_type, value);
    }

    void LinuxGame::weapon_spawner_use_hook(CBaseEntity* spawner, CBaseEntity* player, CBaseEntity* caller, int use_type, float value)
    {
        ammo_game_->use_weapon_spawner(spawner, player, caller, use_type, value);
    }

    bool LinuxGame::bump_weapon(CBaseEntity* player, CBaseEntity* weapon)
    {
        if (weapon == nullptr || giving_to_ == player || using_weapon_ == weapon)
            return original_bump_(player, weapon);
        auto* edict = survivor_edict(player);
        const auto* def = inv::find_item(classname_(weapon));
        if (edict == nullptr || def == nullptr || owner_(weapon) != nullptr)
            return original_bump_(player, weapon);
        if (marked_for_deletion_(network_property_(weapon)))
            return false;
        inv::Inventory* inventory = inventory_lookup_(edict);
        const bool fresh = cut_map_weapon(weapon, *def);
        if (inventory && can_handle_items(player))
        {
            detail::reconcile_native_inventory(*inventory, native_weapons(edict), rounds_weight(edict));
            if (wants_to_store(player, *def) && store_world_item(player, weapon, *def, *inventory))
            {
                // GiveNamedItem can call BumpWeapon and checks the owner before reporting a successful pickup.
                field<std::uint32_t>(weapon, linux_sdk::weapon_owner) =
                    field<std::uint32_t>(player, linux_sdk::entity_handle);
                pickup_notice_(edict, std::string("Stored ") + def->name);
                return true;
            }
            if (weapon_over_limit(player, *def, *inventory))
            {
                pickup_notice_(edict, std::string("Too heavy to pick up ") + def->name);
                return false;
            }
        }
        auto* previous = weapon_in_slot_(player, def->game_slot);
        const auto before = read_reserve(edict);
        const int rounds = firearm(*def) ? field<int>(weapon, linux_sdk::extra_primary_ammo) : 0;
        const bool equipped = original_bump_(player, weapon);
        if (def->weapon() && (inventory != nullptr || fresh))
            finish_weapon_pickup(edict, player, equipped ? weapon : nullptr, previous, *def, before, rounds, fresh);
        return equipped;
    }

    void LinuxGame::use_world_weapon(CBaseEntity* weapon, CBaseEntity* player, CBaseEntity* caller, int use_type, float value)
    {
        if (marked_for_deletion_(network_property_(weapon)))
            return;
        auto* edict = survivor_edict(player);
        const auto* def = inv::find_item(classname_(weapon));
        if (!edict || !def || owner_(weapon) != nullptr)
        {
            gun_use_(weapon, player, caller, use_type, value);
            return;
        }
        inv::Inventory* inventory = inventory_lookup_(edict);
        const bool fresh = cut_map_weapon(weapon, *def);
        if (inventory && can_handle_items(player))
        {
            detail::reconcile_native_inventory(*inventory, native_weapons(edict), rounds_weight(edict));
            if (wants_to_store(player, *def) && store_world_item(player, weapon, *def, *inventory))
            {
                pickup_notice_(edict, std::string("Stored ") + def->name);
                return;
            }
            if (weapon_over_limit(player, *def, *inventory))
            {
                pickup_notice_(edict, std::string("Too heavy to pick up ") + def->name);
                return;
            }
        }
        auto* previous = weapon_in_slot_(player, def->game_slot);
        const auto before = read_reserve(edict);
        const int rounds = firearm(*def) ? field<int>(weapon, linux_sdk::extra_primary_ammo) : 0;
        auto* outer = std::exchange(using_weapon_, weapon);
        gun_use_(weapon, player, caller, use_type, value);
        using_weapon_ = outer;
        if (inventory != nullptr || fresh)
        {
            const bool equipped = weapon_in_slot_(player, def->game_slot) == weapon;
            finish_weapon_pickup(edict, player, equipped ? weapon : nullptr, previous, *def, before, rounds, fresh);
        }
    }

    void LinuxGame::use_weapon_spawner(CBaseEntity* spawner, CBaseEntity* player, CBaseEntity* caller, int use_type, float value)
    {
        if (marked_for_deletion_(network_property_(spawner)))
            return;
        auto* edict = survivor_edict(player);
        const auto* def = spawner_item(spawner);
        if (edict == nullptr || def == nullptr)
        {
            weapon_spawn_use_(spawner, player, caller, use_type, value);
            return;
        }
        inv::Inventory* inventory = inventory_lookup_(edict);
        const auto& rules = inv::map_gun_rules();
        const bool once = rules.enabled && rules.spawners_once && def->weapon();
        int& remaining = field<int>(spawner, linux_sdk::spawn_count);
        if (inventory && can_handle_items(player))
        {
            detail::reconcile_native_inventory(*inventory, native_weapons(edict), rounds_weight(edict));
            if (wants_to_store(player, *def) && inventory->room_for(*def) >= 1)
            {
                inv::WeaponState state;
                if (melee_weapon(*def) && melee_spawn_name_ && std::strcmp(classname_(spawner), "weapon_melee_spawn") == 0)
                    state.melee = melee_spawn_name_(spawner);
                Vector at = origin_(spawner);
                at.z += 16.f;
                if (auto* copy = spawn_item(*def, state, at, true))
                {
                    cut_map_weapon(copy, *def);
                    if (store_world_item(player, copy, *def, *inventory))
                    {
                        if (once || remaining == 1)
                            remove_(spawner);
                        else if (remaining > 1)
                            --remaining;
                        pickup_notice_(edict, std::string("Stored ") + def->name);
                        return;
                    }
                    remove_(copy);
                }
            }
            if (weapon_over_limit(player, *def, *inventory))
            {
                pickup_notice_(edict, std::string("Too heavy to pick up ") + def->name);
                return;
            }
        }
        auto* previous = weapon_in_slot_(player, def->game_slot);
        const auto before = read_reserve(edict);
        auto* outer = std::exchange(giving_to_, player);
        weapon_spawn_use_(spawner, player, caller, use_type, value);
        giving_to_ = outer;

        auto* given = weapon_in_slot_(player, def->game_slot);
        if (given == previous)
            given = nullptr;
        const bool fresh = given != nullptr && cut_map_weapon(given, *def);
        if (def->weapon() && (inventory || rules.enabled))
        {
            int type = -1;
            if (rules.enabled && firearm(*def))
                if (const char* kind = inv::ammo_kind(def->classname))
                    type = inv::ammo_index(kind);
            if (given == nullptr && type >= 0)
                set_ammo_count_(player, before[type], type);
            finish_weapon_pickup(edict, player, given, previous, *def, before, 0, fresh);
        }
        if (once && given != nullptr && !marked_for_deletion_(network_property_(spawner)))
            remove_(spawner);
    }

    bool LinuxGame::store_item(CBaseEntity* player, CBaseEntity* weapon, const inv::ItemDef& def,
                               inv::Inventory& inventory) const
    {
        inv::Inventory next = inventory;
        if (def.weapon())
        {
            next.set_weapons_weight(std::max(0.f, next.weapons_weight() - def.weight));
            if (next.add_weapon(def, weapon_state(weapon, def, true)) < 0)
                return false;
        }
        else
        {
            const int home = next.held_slot(def);
            if (home >= 0)
                next.release_held(home, can_spent(weapon));
            else if (next.add(def, 1, can_spent(weapon)) != 1)
                return false;
        }

        if (!release_carried(player, weapon, def, quiet_drop_))
            return false;
        set_can_spent(weapon, 0);
        remove_(weapon);
        inventory = std::move(next);
        return true;
    }

    std::string LinuxGame::store(edict_t* player, inv::Inventory& inventory) const
    {
        CBaseEntity* entity = entities_->EdictToBaseEntity(player);
        if (!can_handle_items(entity))
            return "The game does not let you put that away now";
        CBaseEntity* weapon = active_weapon_(entity);
        const inv::ItemDef* def = weapon == nullptr ? nullptr : inv::find_item(classname_(weapon));
        if (def == nullptr)
            return "Nothing to store";
        if (!store_item(entity, weapon, *def, inventory))
            return std::string("Could not store ") + def->name;
        return std::string("Stored ") + def->name;
    }

    std::string LinuxGame::use_weapon(CBaseEntity* player, inv::Inventory& inventory, int slot,
                                     const inv::Stack& stack) const
    {
        const inv::ItemDef& def = *stack.def;
        const inv::WeaponState state = stack.weapon;
        CBaseEntity* carried = weapon_in_slot_(player, def.game_slot);
        const inv::ItemDef* old_def = carried == nullptr ? nullptr : inv::find_item(classname_(carried));
        inv::Inventory next = inventory;
        next.remove_from_slot(slot);
        if (carried != nullptr)
        {
            if (old_def == nullptr || !old_def->weapon())
                return "The game does not let you swap this weapon";
            next.set_weapons_weight(std::max(0.f, next.weapons_weight() - old_def->weight));
            if (next.add_weapon(*old_def, weapon_state(carried, *old_def, true), slot) < 0)
                return std::string("No room for your ") + old_def->name;
        }

        int pool[32];
        std::memcpy(pool, field<int[32]>(player, linux_sdk::player_ammo), sizeof(pool));
        if (carried != nullptr && !release_carried(player, carried, *old_def, quiet_drop_))
            return "The game does not let you put that away now";

        const char* give_name = melee_weapon(def) && !state.melee.empty() ? state.melee.c_str() : def.classname;
        give_to_player(player, give_name);
        restore_rounds(player, pool);
        CBaseEntity* given = weapon_in_slot_(player, def.game_slot);
        if (given == nullptr || std::strcmp(classname_(given), def.classname) != 0)
        {
            if (carried != nullptr && !marked_for_deletion_(network_property_(carried)))
                equip_(player, carried);
            restore_rounds(player, pool);
            return std::string("The game refused the ") + def.name;
        }
        if (carried != nullptr && carried != given)
            remove_(carried);
        apply_weapon_state(given, def, state, false);
        remember_weapon(given);
        add_rounds(player, given, def, state.ammo);
        inventory = std::move(next);
        select_item_(player, def.classname, 0);
        return {};
    }

    std::string LinuxGame::use(edict_t* player, inv::Inventory& inventory, int slot)
    {
        CBaseEntity* entity = entities_->EdictToBaseEntity(player);
        const inv::ItemDef* selected = inventory.slot(slot).def;
        if (entity != nullptr && selected != nullptr)
            if (auto revived = start_revive(player, entity, *selected, slot, inventory))
                return *revived;
        if (!can_handle_items(entity))
            return "The game does not let you take out items now";
        if (selected == nullptr)
            return {};
        put_away_carried(entity, selected->classname, inventory);
        const inv::Stack& stack = inventory.slot(slot);
        if (stack.def != selected)
            return {};
        const inv::ItemDef& def = *stack.def;
        if (def.weapon())
            return use_weapon(entity, inventory, slot, stack);

        if (CBaseEntity* carried = weapon_in_slot_(entity, def.game_slot))
        {
            if (std::strcmp(classname_(carried), def.classname) == 0)
            {
                select_item_(entity, def.classname, 0);
                return {};
            }
            const inv::ItemDef* carried_def = inv::find_item(classname_(carried));
            if (carried_def == nullptr || carried_def->weapon() || !store_item(entity, carried, *carried_def, inventory))
                return std::string("Your hands are full: no room for ") + def.name;
        }
        if (inventory.slot(slot).count <= 0)
            return {};

        const int spent = inventory.next_spent(slot);
        give_to_player(entity, def.classname);
        CBaseEntity* given = weapon_in_slot_(entity, def.game_slot);
        if (given == nullptr || std::strcmp(classname_(given), def.classname) != 0)
            return std::string("The game refused the ") + def.name;
        set_can_spent(given, spent);
        inventory.remove_from_slot(slot);
        inventory.set_held(slot, def);
        select_item_(entity, def.classname, 0);
        return {};
    }

    CBaseEntity* LinuxGame::spawn_item(const inv::ItemDef& def, const inv::WeaponState& state, const Vector& position,
                                      bool fresh_map_weapon) const
    {
        auto* item = static_cast<CBaseEntity*>(tools_->CreateEntityByName(def.classname));
        if (item == nullptr)
        {
            std::fprintf(stderr, "inventory_mod: could not create %s\n", def.classname);
            return nullptr;
        }
        set_origin_(item, position);
        if (melee_weapon(def) && !state.melee.empty())
            tools_->SetKeyValue(item, "melee_script_name", state.melee.c_str());
        if (dispatch_spawn_(item, true) < 0)
        {
            std::fprintf(stderr, "inventory_mod: %s did not survive DispatchSpawn\n", def.classname);
            return nullptr;
        }
        if (def.weapon())
        {
            apply_weapon_state(item, def, state, true);
            if (!fresh_map_weapon)
                remember_weapon(item);
        }
        return item;
    }

    std::string LinuxGame::spawn(edict_t* player, std::string_view classname) const
    {
        auto* entity = entities_->EdictToBaseEntity(player);
        const auto* def = inv::find_item(classname);
        if (!can_handle_items(entity) || def == nullptr)
            return {};
        Vector eye, forward;
        eye_vectors_(entity, &eye, &forward, nullptr, nullptr);
        const Vector position(eye.x + 56.f * forward.x, eye.y + 56.f * forward.y, eye.z + 56.f * forward.z - 14.f);
        return std::string(spawn_item(*def, {}, position) != nullptr ? "Spawned " : "Could not spawn ") + def->name;
    }

    std::string LinuxGame::drop(edict_t* player, inv::Inventory& inventory, int slot, int count) const
    {
        CBaseEntity* entity = entities_->EdictToBaseEntity(player);
        if (!can_handle_items(entity))
            return "The game does not let you drop items now";
        const inv::Stack& stack = inventory.slot(slot);
        if (stack.empty())
            return {};
        const inv::ItemDef& def = *stack.def;
        if (stack.count <= 0)
            return drop_weapon(player, def.classname);
        const inv::WeaponState state = stack.weapon;

        Vector eye, forward;
        eye_vectors_(entity, &eye, &forward, nullptr, nullptr);
        int dropped = 0;
        const int requested = std::min(stack.count, std::max(1, count));
        while (dropped < requested)
        {
            const float distance = 48.f + 6.f * static_cast<float>(dropped);
            const Vector position(eye.x + forward.x * distance, eye.y + forward.y * distance,
                                  eye.z + forward.z * distance - 14.f);
            auto* spawned = spawn_item(def, state, position);
            if (spawned == nullptr)
                break;
            set_can_spent(spawned, inventory.next_spent(slot));
            inventory.remove_from_slot(slot);
            ++dropped;
        }
        if (dropped == 0)
            return std::string("Could not drop ") + def.name;
        return std::string("Dropped ") + def.name + " x" + std::to_string(dropped);
    }

    std::optional<LinuxGame::Position> LinuxGame::player_position(edict_t* player) const
    {
        CBaseEntity* entity = entities_->EdictToBaseEntity(player);
        if (entity == nullptr)
            return std::nullopt;
        const Vector& origin = origin_(entity);
        return Position{origin.x, origin.y, origin.z};
    }

    void LinuxGame::tint_pile(CBaseEntity* pile, int type) const
    {
        const unsigned color = inv::ammo_color(inv::ammo_kind_at(type));
        char tint[16];
        std::snprintf(tint, sizeof(tint), "%u %u %u 255", (color >> 16) & 255u, (color >> 8) & 255u, color & 255u);
        tools_->SetKeyValue(pile, "rendercolor", tint);
    }

    bool LinuxGame::spawn_pile(int type, int count, const Vector& position, bool placed_on_map)
    {
        const char* kind = inv::ammo_kind_at(type);
        if (ammo_game_ != this || kind == nullptr || count <= 0)
            return false;
        Vector at = position;
        std::vector<Vector> nearby;
        for (void* current = tools_->FirstEntity(); current != nullptr; current = tools_->NextEntity(current))
        {
            auto* entity = static_cast<CBaseEntity*>(current);
            if (std::strcmp(classname_(entity), "weapon_ammo_spawn") != 0 ||
                marked_for_deletion_(network_property_(entity)))
                continue;
            const auto& other = origin_(entity);
            const float dx = other.x - position.x, dy = other.y - position.y;
            if (std::abs(other.z - position.z) > 72.f || dx * dx + dy * dy > 256.f * 256.f)
                continue;
            const auto existing = piles_.find(field<std::uint32_t>(entity, linux_sdk::entity_handle));
            if (!placed_on_map && existing != piles_.end() && !existing->second.map &&
                existing->second.type == type && dx * dx + dy * dy < 40.f * 40.f &&
                existing->second.count <= std::numeric_limits<int>::max() - count)
            {
                existing->second.count += count;
                return true;
            }
            nearby.push_back(other);
        }

        // Ammo piles have static physics. Choose a clear spot before creating one.
        const auto overlaps_other_pile = [&] {
            return std::any_of(nearby.begin(), nearby.end(), [&](const Vector& other) {
                const float dx = other.x - at.x, dy = other.y - at.y;
                return dx * dx + dy * dy < 24.f * 24.f;
            });
        };
        for (int step = 1; step <= 8 && overlaps_other_pile(); ++step)
            at.x = position.x + 26.f * static_cast<float>((step + 1) / 2) * (step % 2 == 0 ? -1.f : 1.f);
        if (overlaps_other_pile())
            return false;

        auto* probe = static_cast<CBaseEntity*>(tools_->CreateEntityByName("info_target"));
        if (probe != nullptr)
        {
            set_origin_(probe, at);
            if (dispatch_spawn_(probe, false) >= 0)
            {
                if (drop_to_floor_(probe, 0x200400Bu, nullptr) == 1)
                    at = origin_(probe);
                remove_(probe);
            }
        }
        const char* model = "models/w_models/weapons/w_eq_incendiary_ammopack.mdl";
        if (engine_->PrecacheModel(model) <= 0)
            return false;
        auto* pile = static_cast<CBaseEntity*>(tools_->CreateEntityByName("weapon_ammo_spawn"));
        if (pile == nullptr)
            return false;
        tools_->SetKeyValue(pile, "model", model);
        tint_pile(pile, type);
        tools_->SetKeyValue(pile, "solid", "6");
        tools_->SetKeyValue(pile, "spawnflags", "2");
        tools_->SetKeyValue(pile, "disableshadows", "1");
        at.z += 0.3f;
        set_origin_(pile, at);
        if (dispatch_spawn_(pile, true) < 0)
            return false;
        piles_.emplace(field<std::uint32_t>(pile, linux_sdk::entity_handle), AmmoPile{type, count, placed_on_map, true});
        return true;
    }

    void LinuxGame::place_map_piles()
    {
        const auto& rules = inv::ammo_pile_rules();
        if (ammo_game_ != this || !rules.enabled || rules.kinds.empty() || (!rules.replace_map_piles && rules.extra == 0))
            return;

        std::vector<Vector> survivor_positions, pile_positions, item_positions;
        int registered = 0;
        for (void* current = tools_->FirstEntity(); current != nullptr; current = tools_->NextEntity(current))
        {
            auto* entity = static_cast<CBaseEntity*>(current);
            const std::string_view classname(classname_(entity));
            if (classname == "weapon_ammo_spawn")
            {
                pile_positions.push_back(origin_(entity));
                if (!rules.replace_map_piles)
                    continue;
                const auto handle = field<std::uint32_t>(entity, linux_sdk::entity_handle);
                if (piles_.contains(handle))
                    continue;
                const auto [type, count] = roll_pile(rules);
                piles_.emplace(handle, AmmoPile{type, count, true, false});
                tint_pile(entity, type);
                ++registered;
            }
            else if (classname.starts_with("weapon_") && classname.ends_with("_spawn"))
                item_positions.push_back(origin_(entity));
            else if (classname == "player")
            {
                edict_t* edict = entities_->BaseEntityToEdict(entity);
                IPlayerInfo* info = edict == nullptr ? nullptr : players_->GetPlayerInfo(edict);
                if (info != nullptr && info->GetTeamIndex() == 2 && !info->IsDead())
                    survivor_positions.push_back(origin_(entity));
            }
        }

        auto& random = pile_random();
        std::shuffle(item_positions.begin(), item_positions.end(), random);
        std::vector<Vector> extra_positions;
        for (const Vector& item : item_positions)
        {
            if (extra_positions.size() >= static_cast<std::size_t>(rules.extra))
                break;
            const float angle = std::uniform_real_distribution<float>(0.f, 6.2831853f)(random);
            const Vector at(item.x + 20.f * std::cos(angle), item.y + 20.f * std::sin(angle), item.z + 8.f);
            const auto near = [&](const std::vector<Vector>& points, float distance) {
                return std::any_of(points.begin(), points.end(), [&](const Vector& point) {
                    const float dx = point.x - at.x, dy = point.y - at.y, dz = point.z - at.z;
                    return dx * dx + dy * dy + dz * dz < distance * distance;
                });
            };
            if (near(survivor_positions, 700.f) || near(extra_positions, 300.f) || near(pile_positions, 150.f))
                continue;
            const auto [type, count] = roll_pile(rules);
            if (spawn_pile(type, count, at, true))
                extra_positions.push_back(at);
        }
        std::fprintf(stderr, "inventory_mod: %d map ammo piles made finite, %zu extra packs placed\n",
                     registered, extra_positions.size());
    }

    void LinuxGame::pile_use_hook(CBaseEntity* pile, CBaseEntity* activator, CBaseEntity* caller, int use_type, float value)
    {
        ammo_game_->use_pile(pile, activator, caller, use_type, value);
    }

    void LinuxGame::use_pile(CBaseEntity* pile, CBaseEntity* activator, CBaseEntity* caller, int use_type, float value)
    {
        if (marked_for_deletion_(network_property_(pile)))
            return;
        const auto handle = field<std::uint32_t>(pile, linux_sdk::entity_handle);
        auto found = piles_.find(handle);
        const auto& rules = inv::ammo_pile_rules();
        if (found == piles_.end() && (!rules.enabled || rules.kinds.empty()))
        {
            original_pile_use_(pile, activator, caller, use_type, value);
            return;
        }
        if (activator == nullptr)
            return;
        edict_t* edict = entities_->BaseEntityToEdict(activator);
        IPlayerInfo* info = edict == nullptr ? nullptr : players_->GetPlayerInfo(edict);
        if (info == nullptr || info->GetTeamIndex() != 2 || info->IsDead())
            return;
        if (found == piles_.end())
        {
            const auto [type, count] = roll_pile(rules);
            found = piles_.emplace(handle, AmmoPile{type, count, true, false}).first;
            tint_pile(pile, type);
        }
        normalize_ammo(edict);
        AmmoPile& ammo = found->second;
        if (info->IsFakeClient())
        {
            if (!ammo.map)
                return;
            bool uses_rounds = false;
            for (int slot = 0; slot < 2; ++slot)
                if (auto* weapon = weapon_in_slot_(activator, slot))
                    uses_rounds |= field<int>(weapon, linux_sdk::primary_ammo_type) == ammo.type;
            if (!uses_rounds)
                return;
        }
        const char* kind = inv::ammo_kind_at(ammo.type);
        const int before = field<int[32]>(activator, linux_sdk::player_ammo)[ammo.type];
        float weight_room = -1.f;
        if (inv::Inventory* inventory = inventory_lookup_(edict))
        {
            detail::reconcile_native_inventory(*inventory, native_weapons(edict), rounds_weight(edict));
            if (inventory->limits().max_weight > 0.f)
                weight_room = std::max(0.f, inventory->limits().max_weight - inventory->weight());
        }
        const int taken = inv::split_unload(kind, ammo.count, before, weight_room).to_reserve;
        if (taken <= 0)
            return;
        set_ammo_count_(activator, before + taken, ammo.type);
        ammo.count -= taken;
        if (ammo.count == 0)
        {
            remove_(pile);
            piles_.erase(found);
        }
    }

    std::string LinuxGame::drop_ammo(edict_t* player, std::string_view kind, int count)
    {
        CBaseEntity* entity = entities_->EdictToBaseEntity(player);
        const int type = inv::ammo_index(kind);
        if (!can_handle_items(entity) || type < 0)
            return {};
        const int before = field<int[32]>(entity, linux_sdk::player_ammo)[type];
        const int dropped = std::min(before, count > 0 ? count : inv::ammo_magazine(kind));
        if (dropped <= 0)
            return std::string("No ") + inv::ammo_title(kind) + " to drop";
        Vector eye, forward;
        eye_vectors_(entity, &eye, &forward, nullptr, nullptr);
        const Vector at(eye.x + forward.x * 48.f, eye.y + forward.y * 48.f, eye.z + forward.z * 48.f - 14.f);
        if (!spawn_pile(type, dropped, at))
            return std::string("Could not drop ") + inv::ammo_title(kind);
        set_ammo_count_(entity, before - dropped, type);
        return "Dropped " + std::to_string(dropped) + " " + inv::ammo_title(kind);
    }

    bool LinuxGame::install_reload(CBaseEntity* gun)
    {
        if (ammo_game_ != this)
            return false;
        void* table = field<void*>(gun, 0);
        if (reload_tables_.contains(table))
            return true;
        auto* reload = static_cast<linux_sdk::Reload*>(table) + linux_sdk::reload_index;
        auto* fire = static_cast<linux_sdk::FireOnEmpty*>(table) + linux_sdk::fire_empty_index;
        if (!server_vtable_slot(server_base_, reload) || !server_vtable_slot(server_base_, fire))
            return false;
        if ((*reload != gun_reload_ && *reload != shotgun_reload_) || *fire != fire_empty_)
            return false;
        const auto saved = reload_tables_.emplace(table, ReloadTable{*reload, *fire}).first;
        if (!write_vtable_entry(reload, reload_hook))
        {
            reload_tables_.erase(saved);
            return false;
        }
        if (!write_vtable_entry(fire, fire_empty_hook))
        {
            write_vtable_entry(reload, saved->second.reload);
            reload_tables_.erase(saved);
            return false;
        }
        return true;
    }

    bool LinuxGame::reload_hook(CBaseEntity* gun)
    {
        auto& game = *ammo_game_;
        const auto hold = game.unloaded_.find(field<std::uint32_t>(gun, linux_sdk::entity_handle));
        if (hold != game.unloaded_.end())
        {
            CBaseEntity* owner = game.owner_(gun);
            constexpr int attack_or_reload = 1 | (1 << 13);
            if (owner && field<std::uint32_t>(owner, linux_sdk::entity_handle) == hold->second &&
                field<int>(gun, linux_sdk::clip) == 0 &&
                !(field<int>(owner, linux_sdk::buttons) & attack_or_reload))
                return false;
            game.unloaded_.erase(hold);
        }
        return game.reload_tables_.at(field<void*>(gun, 0)).reload(gun);
    }

    void LinuxGame::fire_empty_hook(CBaseEntity* gun)
    {
        auto& game = *ammo_game_;
        const auto hold = game.unloaded_.find(field<std::uint32_t>(gun, linux_sdk::entity_handle));
        if (hold != game.unloaded_.end())
        {
            CBaseEntity* owner = game.owner_(gun);
            const int type = field<int>(gun, linux_sdk::primary_ammo_type);
            if (owner && field<std::uint32_t>(owner, linux_sdk::entity_handle) == hold->second &&
                type >= 0 && type < 32 && field<int[32]>(owner, linux_sdk::player_ammo)[type] > 0)
                field<unsigned char>(gun, linux_sdk::fire_on_empty) = 1;
        }
        game.reload_tables_.at(field<void*>(gun, 0)).fire(gun);
    }

    std::string LinuxGame::unload(edict_t* player, const inv::Inventory& inventory)
    {
        CBaseEntity* entity = entities_->EdictToBaseEntity(player);
        if (!can_handle_items(entity))
            return "The game does not let you unload now";
        Vector eye, forward;
        eye_vectors_(entity, &eye, &forward, nullptr, nullptr);
        auto* gun = item_in_front(eye, forward, 0.9f, [](const inv::ItemDef& def) {
            return inv::unloadable(def.classname);
        }, true);
        const bool on_ground = gun != nullptr;
        CBaseEntity* spawner = on_ground && weapon_spawner(gun) ? gun : nullptr;
        if (!on_ground)
            gun = active_weapon_(entity);
        const auto* def = spawner ? spawner_item(spawner) : gun ? inv::find_item(classname_(gun)) : nullptr;
        if (def == nullptr || !inv::unloadable(def->classname))
            return "No gun to unload";
        if (!on_ground && field<unsigned char>(gun, linux_sdk::in_reload))
            return "Wait for the reload to finish";
        int remaining = 0;
        bool remove_spawner = false;
        if (spawner != nullptr)
        {
            remaining = field<int>(spawner, linux_sdk::spawn_count);
            const auto& rules = inv::map_gun_rules();
            remove_spawner = remaining == 1 || (rules.enabled && rules.spawners_once);
            Vector spot = origin_(spawner);
            if (!remove_spawner)
            {
                spot.x -= 24.f * forward.y;
                spot.y += 24.f * forward.x;
            }
            gun = spawn_item(*def, {}, spot, true);
            if (gun == nullptr)
                return std::string("Cannot unload ") + def->name;
            cut_map_weapon(gun, *def);
        }
        const int clip = field<int>(gun, linux_sdk::clip);
        if (clip <= 0)
        {
            if (spawner)
                remove_(gun);
            return std::string(def->name) + " is empty";
        }
        if (!on_ground && !install_reload(gun))
            return std::string("Cannot unload ") + def->name;
        const char* kind = inv::ammo_kind(def->classname);
        const int type = inv::ammo_index(kind);
        const int before = field<int[32]>(entity, linux_sdk::player_ammo)[type];
        const float limit = inventory.limits().max_weight;
        const auto split = inv::split_unload(kind, clip, before, limit > 0.f ? std::max(0.f, limit - inventory.weight()) : -1.f);
        const Vector at(eye.x + forward.x * 48.f, eye.y + forward.y * 48.f, eye.z + forward.z * 48.f - 14.f);
        if (split.to_ground > 0 && !spawn_pile(type, split.to_ground, at))
        {
            if (spawner)
                remove_(gun);
            return "The overflow could not be dropped";
        }
        if (remove_spawner)
            remove_(spawner);
        else if (remaining > 1)
            field<int>(spawner, linux_sdk::spawn_count) = remaining - 1;
        if (!on_ground)
            unloaded_[field<std::uint32_t>(gun, linux_sdk::entity_handle)] =
                field<std::uint32_t>(entity, linux_sdk::entity_handle);
        field<int>(gun, linux_sdk::clip) = 0;
        field<int>(gun, linux_sdk::upgrades) &= ~3;
        field<unsigned char>(gun, linux_sdk::special_rounds) = 0;
        network_changed_(gun);
        remember_weapon(gun);
        set_ammo_count_(entity, before + split.to_reserve, type);
        std::string result = "Unloaded " + std::to_string(clip) + " " + inv::ammo_title(kind);
        if (split.to_ground > 0)
            result += " (" + std::to_string(split.to_ground) + " dropped: no room)";
        return result;
    }

    void LinuxGame::drop_on_death(edict_t* player, const Position& origin, inv::Inventory& inventory)
    {
        int scattered = 0;
        for (int slot = 0; slot < inventory.total_slots(); ++slot)
        {
            const inv::Stack& stack = inventory.slot(slot);
            for (int count = 0; count < stack.count; ++count)
            {
                const float angle = static_cast<float>(scattered) * 2.39996f;
                const float radius = std::min(4.f + 3.f * static_cast<float>(scattered), 18.f);
                const Vector position(origin[0] + std::cos(angle) * radius, origin[1] + std::sin(angle) * radius,
                                      origin[2] + 16.f);
                if (auto* dropped = spawn_item(*stack.def, stack.weapon, position))
                    set_can_spent(dropped, count == stack.count - 1 ? stack.spent : 0);
                ++scattered;
            }
        }
        inventory.clear();
        CBaseEntity* entity = entities_->EdictToBaseEntity(player);
        if (entity == nullptr)
            return;
        for (const char* kind : inv::ammo_kinds())
        {
            const int type = inv::ammo_index(kind);
            const int count = field<int[32]>(entity, linux_sdk::player_ammo)[type];
            const float angle = static_cast<float>(scattered++) * 2.39996f;
            const Vector spot(origin[0] + std::cos(angle) * 38.f, origin[1] + std::sin(angle) * 38.f, origin[2] + 16.f);
            if (count > 0 && spawn_pile(type, count, spot))
                set_ammo_count_(entity, 0, type);
        }
    }
}
