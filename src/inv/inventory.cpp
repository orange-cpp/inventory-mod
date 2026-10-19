#include "inv/inventory.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace inv
{

    namespace
    {
        const Stack k_empty{};

        std::string_view next_token(std::string_view& line)
        {
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                line.remove_prefix(1);
            std::size_t n = 0;
            while (n < line.size() && line[n] != ' ' && line[n] != '\t' && line[n] != '\r')
                ++n;
            std::string_view token = line.substr(0, n);
            line.remove_prefix(n);
            return token;
        }

        bool parse_int(std::string_view s, int& out)
        {
            auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
            return ec == std::errc{} && ptr == s.data() + s.size();
        }
    } // namespace

    Inventory::Inventory(Limits limits) : limits_(limits)
    {
        limits_.slots = std::max(limits_.slots, 1);
        limits_.weapon_slots = std::clamp(limits_.weapon_slots, 0, 16);
        slots_.assign(static_cast<std::size_t>(limits_.slots + limits_.weapon_slots), Stack{});
    }

    const Stack& Inventory::slot(int index) const
    {
        if (index < 0 || index >= total_slots())
            return k_empty;
        return slots_[static_cast<std::size_t>(index)];
    }

    // How many more of `def` the weight limit lets in (a large number when there is none).
    int Inventory::room_for(const ItemDef& def) const
    {
        if (limits_.max_weight <= 0.f || def.weight <= 0.f)
            return 1 << 20;
        const float left = limits_.max_weight - weight();
        if (left <= 0.f)
            return 0;
        return static_cast<int>(std::floor(left / def.weight + 1e-4f));
    }

    int Inventory::add(const ItemDef& def, int count)
    {
        if (def.weapon())
        {
            int added = 0;
            while (added < count && add_weapon(def, {}) >= 0)
                ++added;
            return added;
        }
        count = std::min(count, room_for(def));
        int added = 0;
        const auto items = slots_.begin() + slot_count();
        for (auto it = slots_.begin(); it != items; ++it)
        {
            Stack& s = *it;
            if (added >= count)
                break;
            if (s.empty() || s.def != &def || s.count >= def.max_stack)
                continue;
            const int take = std::min(def.max_stack - s.count, count - added);
            s.count += take;
            added += take;
        }
        for (auto it = slots_.begin(); it != items; ++it)
        {
            Stack& s = *it;
            if (added >= count)
                break;
            if (!s.empty())
                continue;
            const int take = std::min(def.max_stack, count - added);
            s.def = &def;
            s.count = take;
            added += take;
        }
        if (added > 0)
            ++revision_;
        return added;
    }

    int Inventory::add_to_slot(int index, const ItemDef& def, int count)
    {
        if (def.weapon())
            return count > 0 && add_weapon(def, {}, index) >= 0 ? 1 : 0;
        if (index < 0 || index >= slot_count() || count <= 0)
            return 0;
        Stack& s = slots_[static_cast<std::size_t>(index)];
        if (!s.empty() && s.def != &def)
            return 0;
        const int take = std::min({count, room_for(def), def.max_stack - (s.empty() ? 0 : s.count)});
        if (take <= 0)
            return 0;
        if (s.empty())
            s = Stack{&def, 0};
        s.count += take;
        ++revision_;
        return take;
    }

    int Inventory::add_weapon(const ItemDef& def, const WeaponState& state, int index)
    {
        if (!def.weapon() || room_for(def) < 1)
            return -1;
        if (index < 0)
            for (int i = first_weapon_slot(); i < total_slots() && index < 0; ++i)
                if (slot(i).empty())
                    index = i;
        if (!is_weapon_slot(index) || !slot(index).empty())
            return -1;
        slots_[static_cast<std::size_t>(index)] = Stack{&def, 1, false, state};
        ++revision_;
        return index;
    }

    bool Inventory::has_box_for(const ItemDef& def) const
    {
        const int from = def.weapon() ? first_weapon_slot() : 0;
        const int to = def.weapon() ? total_slots() : slot_count();
        for (int i = from; i < to; ++i)
            if (slot(i).empty() || (!def.weapon() && slot(i).def == &def && slot(i).count < def.max_stack))
                return true;
        return false;
    }

    int Inventory::remove_from_slot(int index, int count)
    {
        if (index < 0 || index >= total_slots() || count <= 0)
            return 0;
        Stack& s = slots_[static_cast<std::size_t>(index)];
        if (s.empty())
            return 0;
        const int removed = std::min(count, s.count);
        if (removed <= 0)
            return 0;
        s.count -= removed;
        if (s.count <= 0 && !s.held)
            s = Stack{};
        ++revision_;
        return removed;
    }

    int Inventory::remove_item(const ItemDef& def, int count)
    {
        int removed = 0;
        for (int i = total_slots() - 1; i >= 0 && removed < count; --i)
            if (slot(i).def == &def)
                removed += remove_from_slot(i, count - removed);
        return removed;
    }

    bool Inventory::move(int from, int to)
    {
        if (from == to || from < 0 || to < 0 || from >= total_slots() || to >= total_slots() ||
            is_weapon_slot(from) != is_weapon_slot(to)) // a weapon stays in the weapon boxes, an item in the item boxes
            return false;
        Stack& a = slots_[static_cast<std::size_t>(from)];
        Stack& b = slots_[static_cast<std::size_t>(to)];
        if (a.empty())
            return false;
        if (b.empty())
        {
            b = a;
            a = Stack{};
        }
        else if (a.def == b.def)
        {
            const int moved = std::min(a.def->max_stack - b.count, a.count);
            if (moved <= 0)
                std::swap(a, b); // both stacks full: swapping changes nothing visible, but keeps the drag meaningful
            else
            {
                b.count += moved;
                a.count -= moved;
                if (a.count <= 0 && !a.held)
                    a = Stack{};
            }
        }
        else
            std::swap(a, b);
        ++revision_;
        return true;
    }

    int Inventory::held_slot(const ItemDef& def) const
    {
        for (int i = 0; i < slot_count(); ++i)
            if (slot(i).held && slot(i).def == &def)
                return i;
        return -1;
    }

    int Inventory::home_for(const ItemDef& def) const
    {
        for (int i = 0; i < slot_count(); ++i)
            if (!slot(i).empty() && slot(i).def == &def && !slot(i).held)
                return i;
        for (int i = 0; i < slot_count(); ++i)
            if (slot(i).empty())
                return i;
        return -1;
    }

    bool Inventory::set_held(int index, const ItemDef& def)
    {
        if (index < 0 || index >= slot_count())
            return false;
        Stack& s = slots_[static_cast<std::size_t>(index)];
        if (!s.empty() && (s.def != &def || s.held))
            return false;
        if (s.empty())
            s = Stack{&def, 0, false};
        s.held = true;
        ++revision_;
        return true;
    }

    void Inventory::clear_held(int index)
    {
        if (index < 0 || index >= slot_count() || !slots_[static_cast<std::size_t>(index)].held)
            return;
        Stack& s = slots_[static_cast<std::size_t>(index)];
        s.held = false;
        if (s.count <= 0)
            s = Stack{};
        ++revision_;
    }

    void Inventory::release_held(int index)
    {
        if (index < 0 || index >= slot_count() || !slots_[static_cast<std::size_t>(index)].held)
            return;
        Stack& s = slots_[static_cast<std::size_t>(index)];
        s.held = false;
        ++s.count;
        ++revision_;
    }

    int Inventory::count_of(const ItemDef& def) const
    {
        int n = 0;
        for (const Stack& s : slots_)
            if (!s.empty() && s.def == &def)
                n += s.count;
        return n;
    }

    int Inventory::used_slots() const
    {
        int n = 0;
        for (const Stack& s : slots_)
            n += s.empty() ? 0 : 1;
        return n;
    }

    float Inventory::weight() const
    {
        float w = weapons_weight_ + ammo_weight_;
        for (const Stack& s : slots_)
            if (!s.empty())
                w += s.def->weight * static_cast<float>(s.count + (s.held ? 1 : 0)); // the one in the hands is carried too
        return w;
    }

    void Inventory::select(int index)
    {
        if (index >= 0 && index < slot_count())
            selected_ = index;
    }

    void Inventory::select_step(int direction)
    {
        const int n = slot_count();
        const int step = direction < 0 ? -1 : 1;
        for (int i = 1; i <= n; ++i)
        {
            const int candidate = ((selected_ + step * i) % n + n) % n;
            if (!slot(candidate).empty())
            {
                selected_ = candidate;
                return;
            }
        }
        selected_ = ((selected_ + step) % n + n) % n;
    }

    void Inventory::clear()
    {
        for (Stack& s : slots_)
            s = Stack{};
        ++revision_;
    }

    std::string Inventory::to_text() const
    {
        std::string out = "inventory 1\n";
        for (int i = 0; i < total_slots(); ++i)
        {
            const Stack& s = slot(i);
            if (s.def == nullptr || s.count <= 0)
                continue;
            out += std::to_string(i) + ' ' + s.def->classname + ' ' + std::to_string(s.count);
            if (s.def->weapon())
                out += ' ' + std::to_string(s.weapon.clip) + ' ' + std::to_string(s.weapon.ammo) + ' ' +
                       (s.weapon.melee.empty() ? std::string("-") : s.weapon.melee) + ' ' + std::to_string(s.weapon.upgrades) +
                       ' ' + std::to_string(s.weapon.special) + ' ' + std::to_string(s.weapon.wear);
            out += '\n';
        }
        return out;
    }

    bool Inventory::from_text(std::string_view text)
    {
        std::vector<Stack> loaded(slots_.size());
        std::vector<Stack> misplaced;
        bool header = false;
        while (!text.empty())
        {
            const std::size_t eol = text.find('\n');
            std::string_view line = text.substr(0, eol);
            text.remove_prefix(eol == std::string_view::npos ? text.size() : eol + 1);

            std::string_view first = next_token(line);
            if (first.empty())
                continue;
            if (!header)
            {
                if (first != "inventory" || next_token(line) != "1")
                    return false;
                header = true;
                continue;
            }
            int index = 0, count = 0;
            const std::string_view name = next_token(line);
            if (!parse_int(first, index) || !parse_int(next_token(line), count))
                continue;
            const ItemDef* def = find_item(name);
            if (def == nullptr || count <= 0)
                continue;
            Stack stack{def, std::min(count, def->max_stack)};
            if (def->weapon())
            {
                int clip = -1, ammo = -1;
                if (parse_int(next_token(line), clip))
                    stack.weapon.clip = clip;
                if (parse_int(next_token(line), ammo))
                    stack.weapon.ammo = ammo;
                const std::string_view melee = next_token(line);
                if (!melee.empty() && melee != "-")
                    stack.weapon.melee = std::string(melee);
                int upgrades = 0, special = 0;
                if (parse_int(next_token(line), upgrades) && upgrades >= 0)
                    stack.weapon.upgrades = upgrades;
                if (parse_int(next_token(line), special) && special >= 0)
                    stack.weapon.special = special;
                int wear = 0; // (a file from before the durability has none)
                if (parse_int(next_token(line), wear) && wear >= 0)
                    stack.weapon.wear = wear;
            }
            // the right kind of box at that number, else (a weapon in an item box, a box number past the end) later
            const bool fits = index >= 0 && index < total_slots() && (index >= slot_count()) == def->weapon() &&
                              loaded[static_cast<std::size_t>(index)].empty();
            if (fits)
                loaded[static_cast<std::size_t>(index)] = std::move(stack);
            else
                misplaced.push_back(std::move(stack));
        }
        if (!header)
            return false;
        for (Stack& stack : misplaced)
        {
            const int from = stack.def->weapon() ? slot_count() : 0;
            const int to = stack.def->weapon() ? total_slots() : slot_count();
            for (int i = from; i < to; ++i)
                if (loaded[static_cast<std::size_t>(i)].empty())
                {
                    loaded[static_cast<std::size_t>(i)] = std::move(stack);
                    break;
                }
        }
        slots_ = std::move(loaded);
        ++revision_;
        return true;
    }

} // namespace inv
