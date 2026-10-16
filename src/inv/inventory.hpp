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

    struct Stack
    {
        const ItemDef* def = nullptr;
        int count = 0; // how many are stored here
        // One more of `def` is in the player's hands and this is its box: it comes back here, and the box keeps showing
        // it while the player holds it.  Not counted in `count`, the weight or the save file.
        bool held = false;

        bool empty() const
        {
            return def == nullptr || (count <= 0 && !held);
        }
    };

    struct Limits
    {
        int slots = 8;
        float max_weight = 0.f; // 0: no weight limit
    };

    class Inventory
    {
    public:
        explicit Inventory(Limits limits = {});

        const Limits& limits() const
        {
            return limits_;
        }
        int slot_count() const
        {
            return static_cast<int>(slots_.size());
        }
        // An empty Stack for an index out of range.
        const Stack& slot(int index) const;

        // Fills the existing stacks of the item first, then empty slots.  Returns how many were stored (less than
        // asked when the slots or the weight limit run out).
        int add(const ItemDef& def, int count = 1);
        // Into one slot only (empty, or holding the same item with room).  Returns how many were stored.
        int add_to_slot(int index, const ItemDef& def, int count = 1);
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
        // empty slot; -1 when there is none.
        int home_for(const ItemDef& def) const;
        // Marks `index` as the box of the held `def` (the slot must be empty or hold `def`).  False when it cannot.
        bool set_held(int index, const ItemDef& def);
        // The held item is gone from the hands (used up, thrown, dropped): the box keeps only what is stored.
        void clear_held(int index);
        // The held item comes back into its box from the hands (stored again, whatever the limits).
        void release_held(int index);

        int count_of(const ItemDef& def) const;
        int used_slots() const;
        float weight() const;

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

        // "inventory 1" then one "<slot> <classname> <count>" line per occupied slot.
        std::string to_text() const;
        // Replaces the contents.  Unknown items and bad slot numbers are skipped; false when the header is wrong.
        bool from_text(std::string_view text);

    private:
        int room_for(const ItemDef& def) const;

        Limits limits_;
        std::vector<Stack> slots_;
        int selected_ = 0;
        std::uint32_t revision_ = 0;
    };

} // namespace inv
