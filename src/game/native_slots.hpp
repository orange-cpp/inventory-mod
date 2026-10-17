#pragma once
// The game's own weapon slots: what the player carries there (for the HUD, the boxes' "in hand" marks and the weight),
// and switching to or dropping one of them.  Main thread.

#include "game/backend.hpp"

#include <string>

namespace game::detail
{

    // "weapon_rifle_ak47" -> name "Rifle Ak47", tag "RA"
    NativeWeapon describe_weapon(const char* classname);
    int native_slot_of(const char* classname);
    // What the local player carries in m_hMyWeapons, into the active session.
    void refresh_native();
    void do_drop_weapon(const std::string& classname);
    void do_select_weapon(const std::string& classname);

} // namespace game::detail
