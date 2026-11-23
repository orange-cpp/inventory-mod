#include "server/linux_game.hpp"
#include "server/linux_memory.hpp"
#include "inv/rules.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "eiface.h"
#include "game/server/iplayerinfo.h"
#include "toolframework/itoolentity.h"

namespace game
{
    using linux_memory::field;

    void LinuxGame::send_stamina(StaminaState& state, float now, bool gone)
    {
        const int value = gone ? -1 : static_cast<int>(std::lround(state.value * 10.f));
        const bool exhausted = !gone && state.exhausted;
        const bool transition = gone || state.sent < 0 || exhausted != state.sent_exhausted || value == 0 || value == 1000;
        if ((value == state.sent && exhausted == state.sent_exhausted) ||
            (!transition && now - state.sent_at < 0.1f) || !inventory_lookup_(state.edict))
            return;

        char line[48];
        std::snprintf(line, sizeof(line), "@sta@ %d %d\n", value, exhausted ? 1 : 0);
        engine_->ClientPrintf(state.edict, line);
        state.sent = value;
        state.sent_exhausted = exhausted;
        state.sent_at = now;
    }

    void LinuxGame::tick_stamina(CBaseEntity* player, StaminaState& state,
                                 const inv::StaminaRules& rules, float now, float dt)
    {
        const bool on_ground = (field<int>(player, linux_sdk::flags) & 1) != 0;
        const auto& velocity = field<float[3]>(player, linux_sdk::local_velocity);
        const bool running = on_ground && std::hypot(velocity[0], velocity[1]) > 130.f;
        const bool adrenaline = adrenaline_active_ && adrenaline_active_(player);
        if (adrenaline)
        {
            state.value = 100.f;
            state.exhausted = false;
        }
        const bool was_exhausted = state.exhausted;

        const auto spend = [&](float cost) {
            if (cost > 0.f && !adrenaline)
            {
                state.value = std::max(0.f, state.value - cost);
                state.used_at = now;
                state.exhausted |= state.value == 0.f;
            }
        };
        if (running && !state.exhausted)
            spend(rules.run * dt);
        if (state.on_ground && !on_ground)
        {
            if (velocity[2] > 150.f)
                spend(rules.jump);
            if (state.speed < 1.f)
            {
                for (const auto offset : {linux_sdk::local_velocity, linux_sdk::absolute_velocity})
                {
                    auto& changed = field<float[3]>(player, offset);
                    changed[0] *= state.speed;
                    changed[1] *= state.speed;
                }
                network_changed_(player);
            }
        }
        state.on_ground = on_ground;

        const int penalty = field<int>(player, linux_sdk::shove_penalty);
        if (penalty != 0)
            set_shove_penalty_(player, 0);
        auto* weapon = active_weapon_(player);
        const auto weapon_handle = weapon == nullptr ? 0xffffffffu : field<std::uint32_t>(weapon, linux_sdk::entity_handle);
        if (weapon == nullptr)
            state.melee = false;
        else
        {
            auto& primary = field<float>(weapon, linux_sdk::next_primary);
            auto& secondary = field<float>(weapon, linux_sdk::next_secondary);
            if (weapon_handle != state.weapon)
                state.melee = std::strcmp(classname_(weapon), "weapon_melee") == 0;
            else
            {
                const bool primary_moved = primary > state.next_primary + 0.05f && primary > now;
                const bool secondary_moved = secondary > state.next_secondary + 0.05f && secondary > now + 0.4f;
                constexpr int attack2 = 1 << 11;
                const bool shove = secondary_moved && (penalty != 0 ||
                    (field<int>(player, linux_sdk::buttons) & attack2) != 0 || !state.melee || !primary_moved);
                const bool swing = !shove && state.melee && primary_moved;
                if (shove || swing)
                {
                    if (was_exhausted && rules.swing_speed < 100.f)
                    {
                        const float stretch = 100.f / rules.swing_speed;
                        if (primary > now)
                            primary = now + (primary - now) * stretch;
                        if (shove && secondary > now)
                            secondary = now + (secondary - now) * stretch;
                        network_changed_(weapon);
                    }
                    spend(shove ? rules.shove : rules.swing);
                }
            }
            state.next_primary = primary;
            state.next_secondary = secondary;
        }
        state.weapon = weapon_handle;

        if (now - state.used_at >= rules.regen_delay)
            state.value = std::min(100.f, state.value + rules.regen * dt * (running ? 0.5f : 1.f));
        if (state.exhausted && state.value >= std::max(rules.recover, 0.1f))
            state.exhausted = false;

        const float desired = state.exhausted ? rules.slow / 100.f : 1.f;
        const float step = 1.5f * dt;
        state.speed += std::clamp(desired - state.speed, -step, step);
        auto& movement = field<float>(player, linux_sdk::lagged_movement);
        if (movement != state.applied)
            state.base_speed = movement;
        state.applied = state.base_speed * (on_ground ? state.speed * melee_move_scale(player) : 1.f);
        if (movement != state.applied)
        {
            movement = state.applied;
            network_changed_(player);
        }
        send_stamina(state, now, false);
    }

    void LinuxGame::release_stamina(int user_id, StaminaState& state)
    {
        if (state.edict == nullptr || state.edict->IsFree() || engine_->GetPlayerUserId(state.edict) != user_id)
            return;
        auto* player = entities_->EdictToBaseEntity(state.edict);
        if (player == nullptr || field<std::uint32_t>(player, linux_sdk::entity_handle) != state.handle)
            return;
        auto& movement = field<float>(player, linux_sdk::lagged_movement);
        if (movement == state.applied && movement != state.base_speed)
        {
            movement = state.base_speed;
            network_changed_(player);
        }
        send_stamina(state, last_stamina_frame_.value_or(0.f), true);
    }

    void LinuxGame::clear_stamina(bool restore)
    {
        if (restore)
            for (auto& [user_id, state] : stamina_)
                release_stamina(user_id, state);
        stamina_.clear();
        any_exhausted_ = false;
        last_stamina_frame_.reset();
        scan_damage_at_ = -1.f;
    }

    void LinuxGame::stamina_frame(edict_t* edicts, int clients)
    {
        const auto& rules = inv::stamina_rules();
        const auto* globals = players_ == nullptr ? nullptr : players_->GetGlobalVars();
        if (!rules.enabled || set_shove_penalty_ == nullptr || globals == nullptr || edicts == nullptr ||
            ammo_game_ != this)
        {
            if (!stamina_.empty())
                clear_stamina(true);
            return;
        }

        const float now = globals->curtime;
        float dt = now - last_stamina_frame_.value_or(now);
        if (last_stamina_frame_ && dt == 0.f)
            return;
        if (dt < 0.f)
            clear_stamina(true);
        last_stamina_frame_ = now;
        if (dt < 0.f || dt > 0.25f)
            dt = 0.f;
        if (rules.damage < 100.f && now >= scan_damage_at_)
        {
            find_damage_targets();
            scan_damage_at_ = now + 0.25f;
        }

        for (auto& [user_id, state] : stamina_)
            state.seen = false;
        any_exhausted_ = false;
        for (int index = 1; index <= clients; ++index)
        {
            auto* edict = edicts + index;
            auto* info = players_->GetPlayerInfo(edict);
            if (info == nullptr || !info->IsConnected() || info->GetTeamIndex() != 2 ||
                info->IsDead() || (!rules.bots && info->IsFakeClient()))
                continue;
            const int user_id = engine_->GetPlayerUserId(edict);
            auto* player = entities_->EdictToBaseEntity(edict);
            if (user_id < 1 || player == nullptr)
                continue;
            auto& state = stamina_[user_id];
            const auto handle = field<std::uint32_t>(player, linux_sdk::entity_handle);
            if (state.edict != edict || state.handle != handle)
            {
                state = StaminaState{};
                state.edict = edict;
                state.handle = handle;
                state.on_ground = (field<int>(player, linux_sdk::flags) & 1) != 0;
                state.applied = state.base_speed = field<float>(player, linux_sdk::lagged_movement);
            }
            state.seen = true;
            tick_stamina(player, state, rules, now, dt);
            any_exhausted_ |= state.exhausted;
        }
        for (auto it = stamina_.begin(); it != stamina_.end();)
        {
            if (it->second.seen)
                ++it;
            else
            {
                release_stamina(it->first, it->second);
                it = stamina_.erase(it);
            }
        }
    }

    void LinuxGame::find_damage_targets()
    {
        for (void* current = tools_->FirstEntity(); current != nullptr; current = tools_->NextEntity(current))
        {
            auto* entity = static_cast<CBaseEntity*>(current);
            auto* slot = field<linux_sdk::OnTakeDamage*>(entity, 0) + linux_sdk::damage_index;
            if (!linux_memory::server_vtable_slot(server_base_, slot) || damage_vtables_.contains(slot))
                continue;
            const auto offset = reinterpret_cast<std::uintptr_t>(*slot) - server_base_;
            if (offset < linux_sdk::text_start || offset >= linux_sdk::text_end)
                continue;
            const auto original = damage_vtables_.emplace(slot, *slot).first;
            if (!linux_memory::write_vtable_entry(slot, stamina_damage_hook))
                damage_vtables_.erase(original);
        }
    }

    int LinuxGame::stamina_damage_hook(CBaseEntity* victim, const void* info)
    {
        auto& game = *ammo_game_;
        auto* slot = field<linux_sdk::OnTakeDamage*>(victim, 0) + linux_sdk::damage_index;
        const auto original = game.damage_vtables_.at(slot);
        if ((!game.any_exhausted_ && !game.melee_installed_) || info == nullptr)
            return original(victim, info);
        const auto* raw = static_cast<const std::byte*>(info);
        float damage;
        std::memcpy(&damage, raw + linux_sdk::damage_info_value, sizeof(damage));
        const float adjusted_damage = game.melee_damage(victim, info, damage);
        if (adjusted_damage == damage)
            return original(victim, info);
        alignas(16) std::array<std::byte, linux_sdk::damage_info_size> adjusted;
        std::memcpy(adjusted.data(), raw, adjusted.size());
        std::memcpy(adjusted.data() + linux_sdk::damage_info_value, &adjusted_damage, sizeof(adjusted_damage));
        return original(victim, adjusted.data());
    }

    bool LinuxGame::exhausted(CBaseEntity* player) const
    {
        if (player == nullptr || !inv::stamina_rules().enabled)
            return false;
        const auto handle = field<std::uint32_t>(player, linux_sdk::entity_handle);
        for (const auto& [id, state] : stamina_)
            if (state.handle == handle)
                return state.exhausted;
        return false;
    }

    void LinuxGame::spend_stamina(CBaseEntity* player, float amount, float now)
    {
        if (!inv::stamina_rules().enabled || player == nullptr || (adrenaline_active_ && adrenaline_active_(player)))
            return;
        const auto handle = field<std::uint32_t>(player, linux_sdk::entity_handle);
        for (auto& [id, state] : stamina_)
            if (state.handle == handle)
            {
                state.value = std::clamp(state.value - amount, 0.f, 100.f);
                if (amount > 0.f)
                {
                    state.used_at = now;
                    state.exhausted |= state.value == 0.f;
                    any_exhausted_ |= state.exhausted;
                }
                return;
            }
    }

    void LinuxGame::uninstall_stamina()
    {
        clear_stamina(true);
        for (const auto& [slot, original] : damage_vtables_)
            if (*slot == stamina_damage_hook)
                linux_memory::write_vtable_entry(slot, original);
        damage_vtables_.clear();
    }
}
