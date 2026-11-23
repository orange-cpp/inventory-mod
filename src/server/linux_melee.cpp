#include "server/linux_game.hpp"
#include "inv/melee.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "eiface.h"
#include "engine/IEngineTrace.h"
#include "game/server/iplayerinfo.h"
#include "toolframework/itoolentity.h"

namespace game
{
    using linux_memory::field;

    namespace
    {
        std::uint32_t handle_of(CBaseEntity* entity)
        {
            return entity == nullptr ? 0xffffffffu : field<std::uint32_t>(entity, linux_sdk::entity_handle);
        }

        bool melee(CBaseEntity* entity, linux_sdk::Classname classname)
        {
            return entity != nullptr && std::strcmp(classname(entity), "weapon_melee") == 0;
        }

        void stretch_attack(CBaseEntity* weapon, std::uintptr_t offset, float now, float scale)
        {
            auto& timer = field<float>(weapon, offset);
            if (timer > now)
                timer = now + (timer - now) * scale;
        }

        struct Animations
        {
            const unsigned char* entries;
            int count;
            int read(int index, std::uintptr_t offset) const
            {
                return *reinterpret_cast<const int*>(entries + index * linux_sdk::melee_activity_size + offset);
            }
        };

        Animations animations(const void* info)
        {
            if (info == nullptr)
                return {};
            const auto* data = static_cast<const unsigned char*>(info);
            const auto* entries = *reinterpret_cast<const unsigned char* const*>(data + linux_sdk::melee_activities);
            const int count = *reinterpret_cast<const int*>(data + linux_sdk::melee_activity_count);
            return {entries, entries == nullptr ? 0 : std::clamp(count, 0, 8)};
        }

        float segment_distance_squared(const LinuxGame::Position& point, const LinuxGame::Position& start,
                                       const LinuxGame::Position& end)
        {
            LinuxGame::Position delta{}, offset{};
            float length = 0.f, dot = 0.f;
            for (std::size_t axis = 0; axis < point.size(); ++axis)
            {
                delta[axis] = end[axis] - start[axis];
                offset[axis] = point[axis] - start[axis];
                length += delta[axis] * delta[axis];
                dot += offset[axis] * delta[axis];
            }
            const float fraction = length > 0.f ? std::clamp(dot / length, 0.f, 1.f) : 0.f;
            float distance = 0.f;
            for (std::size_t axis = 0; axis < point.size(); ++axis)
            {
                const float component = offset[axis] - fraction * delta[axis];
                distance += component * component;
            }
            return distance;
        }
    }

    bool LinuxGame::install_melee(IEngineTrace* trace)
    {
        if (!inv::melee_rules().enabled)
            return true;
        if (ammo_game_ != this || !melee_primary_slot_ || !melee_secondary_slot_ ||
            !linux_memory::write_vtable_entry(melee_primary_slot_, melee_primary_hook))
            return false;
        if (!linux_memory::write_vtable_entry(melee_secondary_slot_, melee_secondary_hook))
        {
            linux_memory::write_vtable_entry(melee_primary_slot_, original_melee_primary_);
            return false;
        }
        melee_trace_ = trace;
        if (melee_sound_slot_)
            linux_memory::write_vtable_entry(melee_sound_slot_, melee_sound_hook);
        melee_installed_ = true;
        return true;
    }

    void LinuxGame::clear_melee(bool restore)
    {
        if (restore)
            for (const auto& [handle, state] : melee_)
                if (auto* player = find_entity(handle); player != nullptr && state.controls_speed &&
                    field<float>(player, linux_sdk::lagged_movement) == state.applied)
                {
                    field<float>(player, linux_sdk::lagged_movement) = state.base_speed;
                    network_changed_(player);
                }
        melee_.clear();
        melee_wear_.clear();
        breaking_melee_.clear();
        thrown_melee_.clear();
        last_melee_frame_.reset();
        prune_wear_at_ = 0.f;
    }

    void LinuxGame::uninstall_melee()
    {
        clear_melee(true);
        if (melee_installed_)
        {
            if (*melee_primary_slot_ == melee_primary_hook)
                linux_memory::write_vtable_entry(melee_primary_slot_, original_melee_primary_);
            if (*melee_secondary_slot_ == melee_secondary_hook)
                linux_memory::write_vtable_entry(melee_secondary_slot_, original_melee_secondary_);
            if (melee_sound_slot_ && *melee_sound_slot_ == melee_sound_hook)
                linux_memory::write_vtable_entry(melee_sound_slot_, original_melee_sound_);
            melee_installed_ = false;
        }
    }

    int LinuxGame::weapon_wear(CBaseEntity* weapon) const
    {
        const auto found = melee_wear_.find(handle_of(weapon));
        return found == melee_wear_.end() ? 0 : found->second;
    }

    void LinuxGame::set_weapon_wear(CBaseEntity* weapon, int wear) const
    {
        const auto handle = handle_of(weapon);
        if (handle == 0xffffffffu)
            return;
        if (wear > 0)
            melee_wear_.insert_or_assign(handle, std::min(wear, inv::k_wear_broken));
        else
            melee_wear_.erase(handle);
    }

    void LinuxGame::wear_out(CBaseEntity* weapon, float hits)
    {
        const auto& rules = inv::melee_rules();
        if (!rules.breaking)
            return;
        const auto profile = inv::melee_profile(melee_script_name_(weapon), rules);
        if (profile.durability <= 0)
            return;
        const int before = weapon_wear(weapon);
        const int after = inv::melee_wear_after(before, hits, profile.durability);
        set_weapon_wear(weapon, after);
        if (after >= inv::k_wear_broken)
            breaking_melee_.insert(handle_of(weapon));
        else if (before < 8000 && after >= 8000)
            if (auto* edict = survivor_edict(owner_(weapon)))
                pickup_notice_(edict, "Your weapon is about to break");
    }

    float LinuxGame::melee_move_scale(CBaseEntity* player) const
    {
        const auto found = melee_.find(handle_of(player));
        return found == melee_.end() ? 1.f : found->second.move_scale;
    }

    bool LinuxGame::in_front(CBaseEntity* player, CBaseEntity* target) const
    {
        Vector eye, forward;
        eye_vectors_(player, &eye, &forward, nullptr, nullptr);
        const auto& at = origin_(target);
        const float x = at.x - eye.x, y = at.y - eye.y;
        const float distance = std::hypot(x, y);
        return distance < 1.f || (x * forward.x + y * forward.y) > 0.5f * distance * std::hypot(forward.x, forward.y);
    }

    void LinuxGame::stagger_enemy(CBaseEntity* enemy, CBaseEntity* source) const
    {
        if (std::strcmp(classname_(enemy), "infected") == 0 && shove_responder_)
        {
            auto& push_only = field<unsigned char>(enemy, linux_sdk::infected_push_only);
            const auto before = push_only;
            push_only = 1;
            shove_responder_(reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(enemy) + linux_sdk::infected_nextbot), source);
            push_only = before;
        }
        else if (stagger_player_ && std::strcmp(classname_(enemy), "player") == 0)
        {
            const Vector point = origin_(source);
            stagger_player_(enemy, source, &point);
        }
    }

    void LinuxGame::tell_melee_hit(CBaseEntity* player, bool heavy) const
    {
        const auto& rules = inv::melee_rules();
        if (view_punch_ && rules.view_kick && rules.view_punch > 0.f)
        {
            const float amount = rules.view_punch * (heavy ? 1.f : 0.33f);
            const QAngle punch(amount, 0.35f * amount, 0.f);
            view_punch_(player, punch);
        }
        auto* edict = entities_->BaseEntityToEdict(player);
        if (edict != nullptr && inventory_lookup_(edict))
            engine_->ClientPrintf(edict, heavy ? "@mhi@ 1\n" : "@mhi@ 0\n");
    }

    void LinuxGame::melee_primary_hook(CBaseEntity* weapon)
    {
        ammo_game_->melee_attack(weapon, false);
    }

    void LinuxGame::melee_secondary_hook(CBaseEntity* weapon)
    {
        ammo_game_->melee_attack(weapon, true);
    }

    int LinuxGame::overhead_index(CBaseEntity* weapon) const
    {
        const auto choices = animations(melee_info_ ? melee_info_(weapon) : nullptr);
        int directions[8]{};
        for (int index = 0; index < choices.count; ++index)
            directions[index] = choices.read(index, linux_sdk::melee_activity_direction);
        return inv::melee_overhead_swing(directions, choices.count);
    }

    void LinuxGame::raise_melee(CBaseEntity* player, CBaseEntity* weapon) const
    {
        if (!animation_event_ || !melee_info_)
            return;
        const auto choices = animations(melee_info_(weapon));
        if (choices.count == 0)
            return;
        const int next = field<int>(weapon, linux_sdk::melee_next_sequence);
        const int activity = choices.read(next >= 0 && next < choices.count ? next : 0, linux_sdk::melee_activity_idle);
        if (activity > 0)
            animation_event_(player, linux_sdk::animation_gesture, activity);
    }

    bool LinuxGame::wall_ahead(CBaseEntity* player) const
    {
        if (melee_trace_ == nullptr)
            return false;
        class WorldRay final : public ITraceFilter
        {
        public:
            bool ShouldHitEntity(IHandleEntity*, int) override { return false; }
            TraceType_t GetTraceType() const override { return TRACE_WORLD_ONLY; }
        } filter;
        Vector eye, forward;
        eye_vectors_(player, &eye, &forward, nullptr, nullptr);
        const float pitch = std::clamp(forward.z, -0.34202014f, 0.34202014f);
        const float horizontal = std::hypot(forward.x, forward.y);
        if (horizontal < 1e-3f)
            return false;
        const float length = 72.f * std::sqrt(1.f - pitch * pitch) / horizontal;
        const Vector end(eye.x + forward.x * length, eye.y + forward.y * length, eye.z + pitch * 72.f);
        Ray_t ray;
        ray.Init(eye, end);
        trace_t hit{};
        melee_trace_->TraceRay(ray, MASK_SOLID, &filter, &hit);
        return hit.fraction < 1.f && !hit.startsolid && std::abs(hit.plane.normal.z) < 0.6f;
    }

    void LinuxGame::melee_sound_hook(CBaseEntity* weapon, int type, float soundtime)
    {
        auto& game = *ammo_game_;
        game.original_melee_sound_(weapon, type, soundtime);
        if (type != linux_sdk::melee_world_hit_sound || !inv::melee_rules().enabled || !inv::melee_rules().wall_bounce)
            return;
        auto* owner = game.owner_(weapon);
        const auto* globals = game.players_->GetGlobalVars();
        const auto found = game.melee_.find(handle_of(owner));
        if (owner == nullptr || globals == nullptr || found == game.melee_.end())
            return;
        auto& state = found->second;
        if (state.swung_weapon != handle_of(weapon) || globals->curtime - state.swing_at > 1.5f ||
            state.bounced || !state.victims.empty() || !game.wall_ahead(owner))
            return;
        state.bounced = true;
        game.wear_out(weapon, 3.f);
        game.tell_melee_hit(owner, true);
    }

    bool LinuxGame::throw_melee(CBaseEntity* player, CBaseEntity* weapon, float now)
    {
        if (!teleport_ || !damage_info_ || !take_damage_ || marked_for_deletion_(network_property_(weapon)))
            return false;
        Vector eye, forward;
        eye_vectors_(player, &eye, &forward, nullptr, nullptr);
        const float speed = inv::melee_rules().throw_speed;
        const Vector velocity(forward.x * speed, forward.y * speed, forward.z * speed + 120.f);
        const Vector target(eye.x + 200.f * forward.x, eye.y + 200.f * forward.y, eye.z + 200.f * forward.z);
        quiet_drop_(player, weapon, &target, &velocity);
        if (owner_(weapon) != nullptr || marked_for_deletion_(network_property_(weapon)))
            return false;
        constexpr float to_degrees = 57.2957795f;
        const QAngle axes(-std::asin(std::clamp(forward.z, -1.f, 1.f)) * to_degrees,
                          std::atan2(forward.y, forward.x) * to_degrees, 0.f);
        const Vector start(eye.x + 24.f * forward.x, eye.y + 24.f * forward.y, eye.z + 24.f * forward.z - 4.f);
        teleport_(weapon, &start, &axes, &velocity);
        const auto& at = origin_(weapon);
        ThrownMelee thrown{handle_of(weapon), handle_of(player), now, {at.x, at.y, at.z}, {}};
        for (void* current = tools_->FirstEntity(); current != nullptr; current = tools_->NextEntity(current))
        {
            auto* target_entity = static_cast<CBaseEntity*>(current);
            const std::string_view name(classname_(target_entity));
            if (name != "infected" && name != "witch" &&
                (name != "player" || field<int>(target_entity, linux_sdk::team) != 3))
                continue;
            const auto& pos = origin_(target_entity);
            if (std::hypot(pos.x - eye.x, pos.y - eye.y) < 1500.f)
                thrown.targets.push_back(handle_of(target_entity));
        }
        thrown_melee_.push_back(std::move(thrown));
        return true;
    }

    void LinuxGame::fly_thrown_melee(float now)
    {
        std::erase_if(thrown_melee_, [&](ThrownMelee& thrown) {
            auto* weapon = find_entity(thrown.weapon);
            if (weapon == nullptr || now - thrown.started > 1.6f || owner_(weapon) != nullptr)
                return true;
            const auto& point = origin_(weapon);
            const Position next{point.x, point.y, point.z};
            const Position last = thrown.last;
            thrown.last = next;
            for (const auto handle : thrown.targets)
            {
                auto* victim = find_entity(handle);
                if (victim == nullptr || field<unsigned char>(victim, linux_sdk::life_state) != 0)
                    continue;
                const auto& pos = origin_(victim);
                if (segment_distance_squared({pos.x, pos.y, pos.z + 40.f}, last, next) > 36.f * 36.f)
                    continue;
                auto* player = find_entity(thrown.player);
                const float full = std::strcmp(classname_(victim), "infected") == 0 ?
                    static_cast<float>(std::max(1, field<int>(victim, linux_sdk::max_health))) : 100.f;
                const float damage = full * inv::melee_rules().throw_damage / 100.f;
                alignas(16) std::array<unsigned char, linux_sdk::damage_info_size> info{};
                damage_info_(info.data(), weapon, player ? player : weapon, weapon, damage, 0x80, 0);
                const float force[3] = {(next[0] - last[0]) * 300.f, (next[1] - last[1]) * 300.f, 0.f};
                const float hit[3] = {pos.x, pos.y, pos.z + 40.f};
                std::memcpy(info.data(), force, sizeof(force));
                std::memcpy(info.data() + 0xc, hit, sizeof(hit));
                take_damage_(victim, info.data());
                if (auto* remaining = find_entity(handle); remaining && field<unsigned char>(remaining, linux_sdk::life_state) == 0)
                    stagger_enemy(remaining, player ? player : weapon);
                wear_out(weapon, 2.f);
                if (player)
                    tell_melee_hit(player, true);
                return true;
            }
            return false;
        });
    }

    void LinuxGame::melee_attack(CBaseEntity* weapon, bool shove)
    {
        const auto offset = shove ? linux_sdk::next_secondary : linux_sdk::next_primary;
        const float before = field<float>(weapon, offset);
        const auto original = shove ? original_melee_secondary_ : original_melee_primary_;
        const auto* globals = players_->GetGlobalVars();
        auto* player = owner_(weapon);
        if (globals == nullptr || player == nullptr || !inv::melee_rules().enabled || !survivor_edict(player))
        {
            original(weapon);
            return;
        }
        const float now = globals->curtime;
        const auto& rules = inv::melee_rules();
        const auto profile = inv::melee_profile(melee_script_name_(weapon), rules);
        const int buttons = field<int>(player, linux_sdk::buttons);
        const auto charging = melee_.find(handle_of(player));
        float charge = 0.f;
        if (charging != melee_.end())
        {
            const auto& held = charging->second;
            charge = held.winding >= 0.f ? held.winding_last - held.winding :
                     now - held.wound_at < 1.f ? held.wound : 0.f;
        }
        const bool heavy = !shove && rules.heavy && (buttons & inv::k_button_heavy) &&
                           !exhausted(player) && charge + 0.15f >= profile.heavy_hold;
        const int overhead = heavy && rules.overhead ? overhead_index(weapon) : -1;
        int saved_index = 0;
        float saved_time = 0.f;
        if (overhead >= 0)
        {
            saved_index = field<int>(weapon, linux_sdk::melee_next_sequence);
            saved_time = field<float>(weapon, linux_sdk::melee_sequence_time);
            field<int>(weapon, linux_sdk::melee_next_sequence) = overhead;
            field<float>(weapon, linux_sdk::melee_sequence_time) = now;
        }
        original(weapon);
        const float after = field<float>(weapon, offset);
        if (!(after > before + 0.05f && after > now))
        {
            if (overhead >= 0)
            {
                field<int>(weapon, linux_sdk::melee_next_sequence) = saved_index;
                field<float>(weapon, linux_sdk::melee_sequence_time) = saved_time;
            }
            return;
        }

        auto& state = melee_[handle_of(player)];
        const auto& stamina = inv::stamina_rules();
        if (shove)
        {
            if (!rules.heavy_shove || !(buttons & inv::k_button_heavy_shove) || exhausted(player))
                return;
            stretch_attack(weapon, offset, now, 1.5f);
            network_changed_(weapon);
            std::fprintf(stderr, "inventory_mod: heavy melee shove, %.2f-second recovery\n",
                         field<float>(weapon, offset) - now);
            spend_stamina(player, rules.heavy_shove_stamina * profile.stamina / 100.f - stamina.shove, now);
            const auto& start = origin_(player);
            for (void* current = tools_->FirstEntity(); current != nullptr; current = tools_->NextEntity(current))
            {
                auto* enemy = static_cast<CBaseEntity*>(current);
                if (field<int>(enemy, linux_sdk::team) != 3 || field<unsigned char>(enemy, linux_sdk::life_state) != 0)
                    continue;
                const auto& at = origin_(enemy);
                if (std::hypot(at.x - start.x, at.y - start.y) <= 120.f && in_front(player, enemy))
                    stagger_enemy(enemy, player);
            }
            return;
        }

        state.heavy = heavy;
        state.bounced = false;
        state.swing_at = now;
        state.swung_weapon = handle_of(weapon);
        state.victims.clear();
        if (state.heavy)
        {
            stretch_attack(weapon, linux_sdk::next_primary, now, rules.heavy_recovery / 100.f);
            stretch_attack(weapon, linux_sdk::next_secondary, now, rules.heavy_recovery / 100.f);
            network_changed_(weapon);
        }
        const float cost = state.heavy ? rules.heavy_stamina : stamina.swing;
        spend_stamina(player, cost * profile.stamina / 100.f - stamina.swing, now);
    }

    float LinuxGame::melee_damage(CBaseEntity* victim, const void* info, float damage)
    {
        const auto* raw = static_cast<const std::byte*>(info);
        const auto handle = [&](std::uintptr_t offset) {
            std::uint32_t value;
            std::memcpy(&value, raw + offset, sizeof(value));
            return value;
        };
        const auto attacker_handle = handle(linux_sdk::damage_info_attacker);
        auto* attacker = find_entity(attacker_handle);
        if (!attacker || attacker == victim || damage <= 0.f)
            return damage;
        const auto& rules = inv::melee_rules();
        const auto* globals = players_->GetGlobalVars();
        const float now = globals ? globals->curtime : 0.f;
        const auto attack_team = field<int>(attacker, linux_sdk::team);
        const auto victim_team = field<int>(victim, linux_sdk::team);
        if (melee_installed_ && rules.enabled && rules.blocking && attack_team == 3 && victim_team == 2 &&
            std::strcmp(classname_(attacker), "infected") == 0 && !exhausted(victim))
        {
            const auto found = melee_.find(handle_of(victim));
            if (found != melee_.end() && found->second.block_since >= 0.f && in_front(victim, attacker))
            {
                auto* weapon = active_weapon_(victim);
                if (melee(weapon, classname_))
                {
                    const auto profile = inv::melee_profile(melee_script_name_(weapon), rules);
                    const bool deflect = rules.deflecting && now - found->second.block_since <= rules.deflect;
                    spend_stamina(victim, rules.block_stamina * profile.stamina * (deflect ? 0.005f : 0.01f), now);
                    if (deflect)
                        stagger_enemy(attacker, victim);
                    return deflect ? 0.f : damage * rules.block / 100.f;
                }
            }
        }
        if (attack_team != 2 || std::strcmp(classname_(attacker), "player") != 0)
            return damage;
        auto* weapon = active_weapon_(attacker);
        const auto weapon_handle = handle_of(weapon);
        if (!melee(weapon, classname_) || (handle(linux_sdk::damage_info_weapon) != weapon_handle &&
                                          handle(linux_sdk::damage_info_inflictor) != weapon_handle))
            return damage;
        const float penalty = exhausted(attacker) ? inv::stamina_rules().damage / 100.f : 1.f;
        const auto found = melee_.find(attacker_handle);
        if (!melee_installed_ || !rules.enabled || victim_team == 2 || found == melee_.end() ||
            found->second.swung_weapon != weapon_handle || now - found->second.swing_at > 1.5f)
            return damage * penalty;
        auto& state = found->second;
        const auto profile = inv::melee_profile(melee_script_name_(weapon), rules);
        const auto handle_victim = handle_of(victim);
        const auto prior = std::find(state.victims.begin(), state.victims.end(), handle_victim);
        const int index = static_cast<int>(prior - state.victims.begin());
        const bool common = std::strcmp(classname_(victim), "infected") == 0;
        const std::string_view victim_class(classname_(victim));
        const bool head = victim_team == 3 && (common || victim_class == "player" || victim_class == "witch") &&
                          field<int>(victim, linux_sdk::hit_group) == 1;
        const int health = field<int>(victim, linux_sdk::health);
        const float full = common ? static_cast<float>(std::max(1, field<int>(victim, linux_sdk::max_health))) : damage;
        float result = full * (state.heavy ? profile.heavy_damage : profile.light_damage) *
                       inv::cleave_scale(index, profile.cleave, profile.cleave_min) * penalty / 100.f;
        if (head)
            result *= rules.head_damage / 100.f;
        if (common && rules.head_only && !head && health > 0)
            result = std::min(result, static_cast<float>(health - 1));
        if (prior == state.victims.end())
        {
            state.victims.push_back(handle_victim);
            wear_out(weapon, state.heavy ? 1.5f : 1.f);
            spend_stamina(attacker, rules.hit_stamina * profile.stamina / 100.f, now);
            if (index == 0)
                tell_melee_hit(attacker, state.heavy);
            if (state.heavy && rules.hit_stagger && victim_team == 3 && result < health)
                stagger_enemy(victim, attacker);
        }
        return result;
    }

    void LinuxGame::melee_frame(edict_t* edicts, int clients)
    {
        const auto& rules = inv::melee_rules();
        const auto* globals = players_ ? players_->GetGlobalVars() : nullptr;
        if (!melee_installed_ || !rules.enabled || globals == nullptr || edicts == nullptr)
            return;
        const float now = globals->curtime;
        const float dt = std::clamp(now - last_melee_frame_.value_or(now), 0.f, 0.25f);
        last_melee_frame_ = now;
        if (now >= scan_damage_at_)
        {
            find_damage_targets();
            scan_damage_at_ = now + 0.25f;
        }
        for (auto& [handle, state] : melee_)
            state.seen = false;
        for (int index = 1; index <= clients; ++index)
        {
            auto* edict = edicts + index;
            auto* info = players_->GetPlayerInfo(edict);
            if (info == nullptr || !info->IsConnected() || info->GetTeamIndex() != 2 || info->IsDead())
                continue;
            auto* player = entities_->EdictToBaseEntity(edict);
            if (player == nullptr)
                continue;
            auto& state = melee_[handle_of(player)];
            state.seen = true;
            auto* weapon = active_weapon_(player);
            const bool held = melee(weapon, classname_);
            const auto profile = inv::melee_profile(held ? melee_script_name_(weapon) : "", rules);
            const int buttons = field<int>(player, linux_sdk::buttons);
            const bool winding = held && (buttons & inv::k_button_winding);
            if (winding)
            {
                if (state.winding < 0.f)
                {
                    state.winding = now;
                    if (rules.gesture)
                        raise_melee(player, weapon);
                }
                state.winding_last = now;
                spend_stamina(player, rules.wind_stamina * profile.stamina * dt / 100.f, now);
            }
            else if (state.winding >= 0.f)
            {
                state.wound = state.winding_last - state.winding;
                state.wound_at = now;
                state.winding = -1.f;
            }
            const bool blocking = held && rules.blocking && !winding && (buttons & inv::k_button_reload) && !exhausted(player);
            if (blocking && state.block_since < 0.f)
                state.block_since = now;
            else if (!blocking)
                state.block_since = -1.f;
            if (held && rules.throwing && (buttons & inv::k_button_throw) && now - state.thrown_at > 0.5f &&
                throw_melee(player, weapon, now))
                state.thrown_at = now;

            state.move_scale = 1.f;
            if (winding && rules.wind_slowdown)
            {
                const float fraction = std::clamp((now - state.winding) / std::max(0.1f, profile.heavy_hold), 0.f, 1.f);
                state.move_scale = std::lerp(1.f, rules.wind_speed / 100.f, fraction);
            }
            if (blocking)
                state.move_scale = std::min(state.move_scale, rules.block_speed / 100.f);
            if (!inv::stamina_rules().enabled)
            {
                const float scale = (field<int>(player, linux_sdk::flags) & 1) != 0 ? state.move_scale : 1.f;
                auto& movement = field<float>(player, linux_sdk::lagged_movement);
                if (!state.controls_speed || movement != state.applied)
                    state.base_speed = movement;
                state.applied = state.base_speed * scale;
                state.controls_speed = scale != 1.f;
                if (movement != state.applied)
                {
                    movement = state.applied;
                    network_changed_(player);
                }
            }
            const int hold = rules.heavy ? static_cast<int>(std::lround(profile.heavy_hold * 1000.f)) : 0;
            const int shove = rules.heavy_shove ? static_cast<int>(std::lround(rules.heavy_shove_hold * 1000.f)) : 0;
            if (!info->IsFakeClient() && inventory_lookup_(edict) &&
                (hold != state.sent_hold || shove != state.sent_shove || now - state.sent_at >= 5.f))
            {
                char line[64];
                const int features = (rules.overhead && melee_info_ ? 1 : 0) |
                                     (rules.throwing && teleport_ && damage_info_ && take_damage_ ? 2 : 0);
                std::snprintf(line, sizeof(line), "@mel@ %d %d %d\n", hold, shove, features);
                engine_->ClientPrintf(edict, line);
                state.sent_hold = hold;
                state.sent_shove = shove;
                state.sent_at = now;
            }
        }
        std::erase_if(melee_, [&](const auto& entry) {
            const auto& [handle, state] = entry;
            if (state.seen)
                return false;
            if (auto* player = find_entity(handle); player != nullptr && state.controls_speed &&
                field<float>(player, linux_sdk::lagged_movement) == state.applied)
            {
                field<float>(player, linux_sdk::lagged_movement) = state.base_speed;
                network_changed_(player);
            }
            return true;
        });

        fly_thrown_melee(now);
        for (const auto handle : breaking_melee_)
            if (auto* weapon = find_entity(handle))
            {
                if (auto* player = owner_(weapon))
                {
                    if (auto* edict = survivor_edict(player))
                        pickup_notice_(edict, std::string("Your ") + melee_script_name_(weapon) + " broke");
                    quiet_drop_(player, weapon, nullptr, nullptr);
                }
                remove_(weapon);
            }
        breaking_melee_.clear();
        if (now >= prune_wear_at_)
        {
            prune_wear_at_ = now + 10.f;
            std::erase_if(melee_wear_, [&](const auto& entry) { return find_entity(entry.first) == nullptr; });
        }
    }
}
