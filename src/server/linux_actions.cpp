#include "server/linux_game.hpp"
#include "server/linux_memory.hpp"
#include "inv/rules.hpp"

#include <algorithm>
#include <cstring>

#include "eiface.h"
#include "game/server/iplayerinfo.h"
#include "icvar.h"
#include "tier1/convar.h"

namespace game
{
    using linux_memory::field;

    namespace
    {
        bool gas_can(const inv::Stack& stack)
        {
            return !stack.empty() && std::strcmp(stack.def->classname, "weapon_gascan") == 0;
        }

        int remaining_fuel(const inv::Inventory& inventory)
        {
            int total = 0;
            for (int slot = 0; slot < inventory.slot_count(); ++slot)
                if (gas_can(inventory.slot(slot)))
                    total += inventory.slot(slot).fuel();
            return total;
        }
    }

    bool LinuxGame::install_actions(ICvar* cvars)
    {
        cvars_ = cvars;
        if (inv::chainsaw_rules().enabled && chainsaw_frame_slot_ && ammo_game_ == this)
            fuel_hook_installed_ = linux_memory::write_vtable_entry(chainsaw_frame_slot_, chainsaw_post_frame);
        return (!inv::chainsaw_rules().enabled || fuel_hook_installed_) &&
               (!inv::self_revive_rules().enabled || on_revived_);
    }

    void LinuxGame::chainsaw_post_frame(CBaseEntity* saw)
    {
        if (!inv::chainsaw_rules().enabled || field<int>(saw, linux_sdk::clip) > 0)
            ammo_game_->original_chainsaw_frame_(saw);
    }

    int LinuxGame::can_spent(CBaseEntity* can) const
    {
        const auto entry = spent_cans_.find(field<std::uint32_t>(can, linux_sdk::entity_handle));
        return entry == spent_cans_.end() ? 0 : entry->second;
    }

    void LinuxGame::set_can_spent(CBaseEntity* can, int spent) const
    {
        if (spent_cans_.empty() && spent == 0)
            return;
        const auto handle = field<std::uint32_t>(can, linux_sdk::entity_handle);
        if (handle == 0xffffffffu)
            return;
        if (spent > 0)
            spent_cans_.insert_or_assign(handle, spent);
        else
            spent_cans_.erase(handle);
    }

    int LinuxGame::max_fuel(CBaseEntity* saw) const
    {
        const auto* table = field<linux_sdk::ClipSize*>(saw, 0);
        const int size = table[linux_sdk::max_clip_index](saw);
        return size > 0 && size <= 1000 ? size : 30;
    }

    std::string LinuxGame::pour_fuel(CBaseEntity* saw, inv::Inventory& inventory) const
    {
        auto& clip = field<int>(saw, linux_sdk::clip);
        const int capacity = max_fuel(saw);
        const int before = std::max(0, clip);
        if (before >= capacity)
            return "The chainsaw is full";

        int taken = 0;
        // Drain opened cans first. The inventory settles fully drained cans itself.
        for (const bool opened : {true, false})
            for (int slot = 0; slot < inventory.slot_count() && before + taken < capacity; ++slot)
            {
                const auto& stack = inventory.slot(slot);
                if (gas_can(stack) && (stack.spent > 0) == opened)
                    taken += inventory.take_fuel(slot, capacity - before - taken);
            }
        if (taken <= 0)
            return "No fuel: carry a gas can in your inventory";
        clip = before + taken;
        network_changed_(saw);
        return "Chainsaw refuelled: " + std::to_string(clip) + " / " + std::to_string(capacity) +
               " (" + std::to_string(remaining_fuel(inventory)) + " left in your gas cans)";
    }

    LinuxGame::Progress LinuxGame::start_progress(edict_t* edict, CBaseEntity* player, float now, float duration) const
    {
        if (duration > 0.f)
        {
            field<float>(player, linux_sdk::progress_start) = now;
            field<float>(player, linux_sdk::progress_duration) = duration;
            network_changed_(player);
        }
        return {edict, field<std::uint32_t>(player, linux_sdk::entity_handle), now, duration};
    }

    void LinuxGame::clear_progress(CBaseEntity* player, const Progress& timer) const
    {
        auto& current_duration = field<float>(player, linux_sdk::progress_duration);
        if (timer.duration > 0.f && current_duration == timer.duration &&
            field<float>(player, linux_sdk::progress_start) == timer.start)
        {
            current_duration = 0.f;
            network_changed_(player);
        }
    }

    CBaseEntity* LinuxGame::progress_player(int user_id, const Progress& timer) const
    {
        if (timer.edict == nullptr || timer.edict->IsFree() || engine_->GetPlayerUserId(timer.edict) != user_id)
            return nullptr;
        auto* player = entities_->EdictToBaseEntity(timer.edict);
        return player != nullptr && field<std::uint32_t>(player, linux_sdk::entity_handle) == timer.player
                   ? player : nullptr;
    }

    void LinuxGame::fuel_input(edict_t* edict, CBaseEntity* player, int user_id, float now)
    {
        const bool pressed = (field<int>(player, linux_sdk::buttons) & (1 << 13)) != 0;
        bool& was_pressed = reload_down_[user_id];
        const bool started = pressed && !was_pressed;
        was_pressed = pressed;
        if (!started || down(player) || dominated_(player) || refuels_.contains(user_id))
            return;

        auto* saw = active_weapon_(player);
        auto* inventory = inventory_lookup_(edict);
        if (saw == nullptr || inventory == nullptr || std::strcmp(classname_(saw), "weapon_chainsaw") != 0)
            return;
        if (field<int>(saw, linux_sdk::clip) >= max_fuel(saw))
        {
            pickup_notice_(edict, "The chainsaw is full");
            return;
        }
        if (remaining_fuel(*inventory) <= 0)
        {
            pickup_notice_(edict, "No fuel: carry a gas can in your inventory");
            return;
        }
        const float seconds = inv::chainsaw_rules().seconds;
        if (seconds <= 0.f)
        {
            pickup_notice_(edict, pour_fuel(saw, *inventory));
            return;
        }
        refuels_.emplace(user_id, Refuel{
            start_progress(edict, player, now, seconds), field<std::uint32_t>(saw, linux_sdk::entity_handle)});
        pickup_notice_(edict, "Refuelling the chainsaw");
    }

    bool LinuxGame::down(CBaseEntity* player) const
    {
        return incapacitated_(player) || (hanging_from_ledge_ && hanging_from_ledge_(player));
    }

    bool LinuxGame::use_revive_item(CBaseEntity* player, const Revive& revive,
                                   inv::Inventory& inventory, bool consume) const
    {
        const auto use_held = [&] {
            const auto& def = *revive.item;
            auto* held = weapon_in_slot_(player, def.game_slot);
            if (held == nullptr || std::strcmp(classname_(held), def.classname) != 0 ||
                marked_for_deletion_(network_property_(held)))
                return false;
            if (consume)
            {
                quiet_drop_(player, held, nullptr, nullptr);
                remove_(held);
                inventory.clear_held(inventory.held_slot(def));
            }
            return true;
        };
        if (revive.slot < 0 && use_held())
            return true;

        int slot = revive.slot;
        if (inventory.slot(slot).def != revive.item || inventory.slot(slot).count <= 0)
        {
            slot = -1;
            for (int index = 0; index < inventory.slot_count(); ++index)
                if (inventory.slot(index).def == revive.item && inventory.slot(index).count > 0)
                {
                    slot = index;
                    break;
                }
        }
        if (slot >= 0)
            return !consume || inventory.remove_from_slot(slot) == 1;
        return revive.slot >= 0 && use_held();
    }

    std::string LinuxGame::finish_revive(CBaseEntity* player, const Revive& revive, inv::Inventory& inventory) const
    {
        if (!use_revive_item(player, revive, inventory, false))
            return std::string("You have no ") + revive.item->name + " left to get up with";
        on_revived_(player);
        if (down(player))
            return "You could not get up";

        use_revive_item(player, revive, inventory, true);
        if (on_adrenaline_used_ && inv::self_revive_rules().rush &&
            std::strcmp(revive.item->classname, "weapon_adrenaline") == 0)
        {
            const ConVar* duration = cvars_ == nullptr ? nullptr : cvars_->FindVar("adrenaline_duration");
            const float seconds = duration == nullptr ? 15.f : duration->GetFloat();
            on_adrenaline_used_(player, seconds > 0.f && seconds < 600.f ? seconds : 15.f);
        }
        network_changed_(player);
        return std::string("Got up with your ") + revive.item->name;
    }

    std::optional<std::string> LinuxGame::start_revive(edict_t* edict, CBaseEntity* player,
                                                       const inv::ItemDef& item, int slot, inv::Inventory& inventory)
    {
        if (!on_revived_ || engine_ == nullptr || players_ == nullptr || !inv::revives(item.classname) || !down(player))
            return std::nullopt;
        const auto& rules = inv::self_revive_rules();
        if (!rules.ledge && hanging_from_ledge_ && hanging_from_ledge_(player))
            return "You cannot pull yourself up off a ledge";
        if (dominated_(player))
            return "Not while you are pinned";
        const int user_id = engine_->GetPlayerUserId(edict);
        if (revives_.contains(user_id))
            return std::string{};

        const auto* globals = players_->GetGlobalVars();
        if (globals == nullptr)
            return std::nullopt;
        const float now = globals->curtime;
        Revive revive{{edict, field<std::uint32_t>(player, linux_sdk::entity_handle), now, rules.seconds}, &item, slot};
        if (!use_revive_item(player, revive, inventory, false))
            return std::string("You have no ") + item.name + " left to get up with";
        if (rules.seconds <= 0.f)
            return finish_revive(player, revive, inventory);
        revive.timer = start_progress(edict, player, now, rules.seconds);
        revives_.emplace(user_id, revive);
        return std::string("Getting up with your ") + item.name;
    }

    void LinuxGame::actions_frame(edict_t* edicts, int clients)
    {
        const auto* globals = players_ == nullptr ? nullptr : players_->GetGlobalVars();
        if (globals == nullptr)
            return;
        const float now = globals->curtime;
        if (now >= prune_cans_at_)
        {
            prune_cans_at_ = now + 5.f;
            std::erase_if(spent_cans_, [&](const auto& entry) { return find_entity(entry.first) == nullptr; });
        }
        if (fuel_hook_installed_ && inv::chainsaw_rules().enabled && edicts != nullptr)
            for (int index = 1; index <= clients; ++index)
            {
                auto* edict = edicts + index;
                auto* info = players_->GetPlayerInfo(edict);
                if (info == nullptr || !info->IsConnected() || info->GetTeamIndex() != 2 || info->IsDead() ||
                    info->IsFakeClient())
                    continue;
                auto* player = entities_->EdictToBaseEntity(edict);
                const int user_id = engine_->GetPlayerUserId(edict);
                if (player != nullptr && user_id > 0)
                    fuel_input(edict, player, user_id, now);
            }

        std::erase_if(refuels_, [&](const auto& entry) {
            const auto& [user_id, refuel] = entry;
            auto* player = progress_player(user_id, refuel.timer);
            auto* saw = player == nullptr ? nullptr : active_weapon_(player);
            const bool valid = player != nullptr && !down(player) && !dominated_(player) &&
                field<unsigned char>(player, linux_sdk::life_state) == 0 &&
                saw != nullptr && field<std::uint32_t>(saw, linux_sdk::entity_handle) == refuel.saw;
            if (valid && now < refuel.timer.start + refuel.timer.duration)
                return false;
            if (player != nullptr)
                clear_progress(player, refuel.timer);
            if (valid)
                if (auto* inventory = inventory_lookup_(refuel.timer.edict))
                    pickup_notice_(refuel.timer.edict, pour_fuel(saw, *inventory));
            return true;
        });

        std::erase_if(revives_, [&](const auto& entry) {
            const auto& [user_id, revive] = entry;
            auto* player = progress_player(user_id, revive.timer);
            const bool valid = player != nullptr && field<unsigned char>(player, linux_sdk::life_state) == 0 &&
                               down(player) && !dominated_(player);
            if (valid && now < revive.timer.start + revive.timer.duration)
                return false;
            if (player != nullptr)
                clear_progress(player, revive.timer);
            if (valid)
                if (auto* inventory = inventory_lookup_(revive.timer.edict))
                    pickup_notice_(revive.timer.edict, finish_revive(player, revive, *inventory));
            return true;
        });
    }

    void LinuxGame::clear_actions(bool restore)
    {
        if (restore)
        {
            for (const auto& [user_id, refuel] : refuels_)
                if (auto* player = progress_player(user_id, refuel.timer))
                    clear_progress(player, refuel.timer);
            for (const auto& [user_id, revive] : revives_)
                if (auto* player = progress_player(user_id, revive.timer))
                    clear_progress(player, revive.timer);
        }
        refuels_.clear();
        revives_.clear();
        reload_down_.clear();
        spent_cans_.clear();
        prune_cans_at_ = 0.f;
    }

    void LinuxGame::uninstall_actions()
    {
        clear_actions(true);
        if (fuel_hook_installed_ && *chainsaw_frame_slot_ == chainsaw_post_frame)
            linux_memory::write_vtable_entry(chainsaw_frame_slot_, original_chainsaw_frame_);
        fuel_hook_installed_ = false;
    }
}
