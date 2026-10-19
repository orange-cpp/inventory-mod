#pragma once
// A player's inventory: a fixed number of slots, each holding a stack of one item type, with an optional weight limit.
// No game, window or UI code in here, so it is tested on its own.

#include "inv/items.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace inv
{

    // A stored gun or melee weapon is one of a kind: its rounds and, for a melee weapon, which one it is.
    struct WeaponState
    {
        int clip = -1; // rounds in the magazine; -1: as the game gives it
        int ammo = -1; // rounds in reserve; -1: as the game gives it
        std::string melee; // weapon_melee's script name ("fireaxe", "katana" ...); empty for a gun
        // A gun's upgrades (the game's m_upgradeBitVec: 1 incendiary ammo, 2 explosive ammo, 4 laser sight) and how many
        // of the incendiary / explosive rounds are still loaded.
        int upgrades = 0;
        int special = 0;
        // A melee weapon's wear ([melee] durability): 0 new .. inv::k_wear_broken (10000) broken.
        int wear = 0;
    };

    struct Stack
    {
        const ItemDef* def = nullptr;
        int count = 0; // how many are stored here
        // One more of `def` is in the player's hands and this is its box: it comes back here, and the box keeps showing
        // it while the player holds it.  Not counted in `count` or the save file; counted in the weight (it is carried).
        // Never for a weapon (ItemDef::weapon()): the game's own panel shows the gun and melee in the hands.
        bool held = false;
        WeaponState weapon; // a weapon's (count is then 1)

        bool empty() const
        {
            return def == nullptr || (count <= 0 && !held);
        }
    };

    struct Limits
    {
        int slots = 8; // item boxes
        float max_weight = 0.f; // 0: no weight limit
        int weapon_slots = 2; // boxes for guns and melee weapons, apart from the item boxes
    };

    class Inventory
    {
    public:
        explicit Inventory(Limits limits = {});

        const Limits& limits() const
        {
            return limits_;
        }
        // The boxes are numbered 0 .. total_slots() - 1: first the item boxes (0 .. slot_count() - 1), then the weapon
        // boxes (first_weapon_slot() ..).  An item only goes into an item box, a weapon (ItemDef::weapon()) only into a
        // weapon box.
        int slot_count() const
        {
            return limits_.slots;
        }
        int weapon_slot_count() const
        {
            return limits_.weapon_slots;
        }
        int first_weapon_slot() const
        {
            return limits_.slots;
        }
        int total_slots() const
        {
            return static_cast<int>(slots_.size());
        }
        bool is_weapon_slot(int index) const
        {
            return index >= first_weapon_slot() && index < total_slots();
        }
        // An empty Stack for an index out of range.
        const Stack& slot(int index) const;

        // Fills the existing stacks of the item first, then empty slots.  Returns how many were stored (less than
        // asked when the slots or the weight limit run out).  A weapon: each into a free weapon box, as it comes.
        int add(const ItemDef& def, int count = 1);
        // Into one slot only (empty, or holding the same item with room).  Returns how many were stored.
        int add_to_slot(int index, const ItemDef& def, int count = 1);
        // A weapon, with its state, into weapon box `index` when it is empty (-1: the first empty weapon box).  Returns
        // the box, or -1 when there is none or it is too heavy.
        int add_weapon(const ItemDef& def, const WeaponState& state, int index = -1);
        // Whether `def` would find a box (not counting the weight): a free one, or for an item a stack with room.
        bool has_box_for(const ItemDef& def) const;
        // Removes up to `count` from a slot.  Returns how many were removed.
        int remove_from_slot(int index, int count = 1);
        // Removes up to `count` of an item from any slots, last slots first.  Returns how many were removed.
        int remove_item(const ItemDef& def, int count = 1);
        // Drag and drop: onto an empty slot it moves, onto the same item it merges (what does not fit stays), onto
        // another item it swaps.  False when nothing changed.
        bool move(int from, int to);

        // The player's hands.  The box of the held `def`, or -1.
        int held_slot(const ItemDef& def) const;
        // Where a newly held `def` gets its box: a slot already holding that item (and no held one), else the first
        // empty item box; -1 when there is none.
        int home_for(const ItemDef& def) const;
        // Marks `index` as the box of the held `def` (the slot must be empty or hold `def`).  False when it cannot.
        bool set_held(int index, const ItemDef& def);
        // The held item is gone from the hands (used up, thrown, dropped): the box keeps only what is stored.
        void clear_held(int index);
        // The held item comes back into its box from the hands (stored again; the weight stays the same).
        void release_held(int index);

        int count_of(const ItemDef& def) const;
        int used_slots() const;
        // What the stored items, the held ones (Stack::held), the weapons (set_weapons_weight) and the reserve rounds
        // (set_ammo_weight) weigh: everything the player carries.
        float weight() const;
        // What the player's gun and melee / pistol weigh (weapon_weight); the game code keeps it up to date.
        void set_weapons_weight(float weight)
        {
            weapons_weight_ = weight;
        }
        float weapons_weight() const
        {
            return weapons_weight_;
        }
        // What the player's reserve rounds weigh (ammo_weight of m_iAmmo); the game code keeps it up to date.
        void set_ammo_weight(float weight)
        {
            ammo_weight_ = weight;
        }
        float ammo_weight() const
        {
            return ammo_weight_;
        }

        // The slot the hotbar has selected.
        int selected() const
        {
            return selected_;
        }
        void select(int index);
        // Moves the selection to the next occupied slot in that direction (wraps); with no occupied slot it steps
        // through the empty ones.
        void select_step(int direction);

        void clear();
        // Bumped on every change, for a UI or a network layer that sends only what changed.
        std::uint32_t revision() const
        {
            return revision_;
        }

        // "inventory 1" then one "<slot> <classname> <count>" line per occupied slot; a weapon's line goes on with
        // "<clip> <ammo> <melee script name or -> <upgrades> <special rounds>" (files from before the upgrades end at the
        // melee name).  Reading puts what is in the wrong kind of box (a weapon in an
        // item box: files from before the weapon boxes) into a free box of its kind.
        std::string to_text() const;
        // Replaces the contents.  Unknown items are skipped; a box number past the end goes into a free box of its kind
        // (a smaller slot count loses only what does not fit); false when the header is wrong.
        bool from_text(std::string_view text);

        // How many more of `def` the weight limit lets in (a large number when there is none).
        int room_for(const ItemDef& def) const;

    private:

        Limits limits_;
        std::vector<Stack> slots_;
        int selected_ = 0;
        std::uint32_t revision_ = 0;
        float weapons_weight_ = 0.f;
        float ammo_weight_ = 0.f;
    };

} // namespace inv
