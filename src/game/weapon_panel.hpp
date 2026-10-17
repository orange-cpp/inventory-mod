#pragma once
// The game's own weapon-selection panel (client.dll's CHudWeaponSelection): hidden while the mod replaces it, and its
// item boxes left out while the mod draws them itself.

#include "game/mem.hpp"

namespace game::detail
{

    // Hooks the panel's ShouldDraw and the item-box part of its Paint (optional).
    void install_weapon_panel(const mem::ModuleInfo& client);
    // Whether the panel leaves its item boxes out (set by the backend's pump from the settings).
    void hide_item_boxes(bool hide);

} // namespace game::detail
