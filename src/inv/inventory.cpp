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
        slots_.assign(static_cast<std::size_t>(limits_.slots), Stack{});
    }

    const Stack& Inventory::slot(int index) const
    {
        if (index < 0 || index >= slot_count())
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
        count = std::min(count, room_for(def));
        int added = 0;
        for (Stack& s : slots_)
        {
            if (added >= count)
                break;
            if (s.empty() || s.def != &def || s.count >= def.max_stack)
                continue;
            const int take = std::min(def.max_stack - s.count, count - added);
            s.count += take;
            added += take;
        }
        for (Stack& s : slots_)
        {
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

    int Inventory::remove_from_slot(int index, int count)
    {
        if (index < 0 || index >= slot_count() || count <= 0)
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
        for (int i = slot_count() - 1; i >= 0 && removed < count; --i)
            if (slot(i).def == &def)
                removed += remove_from_slot(i, count - removed);
        return removed;
    }

    bool Inventory::move(int from, int to)
    {
        if (from == to || from < 0 || to < 0 || from >= slot_count() || to >= slot_count())
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
        float w = 0.f;
        for (const Stack& s : slots_)
            if (!s.empty())
                w += s.def->weight * static_cast<float>(s.count);
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
        for (int i = 0; i < slot_count(); ++i)
        {
            const Stack& s = slot(i);
            if (s.def == nullptr || s.count <= 0)
                continue;
            out += std::to_string(i) + ' ' + s.def->classname + ' ' + std::to_string(s.count) + '\n';
        }
        return out;
    }

    bool Inventory::from_text(std::string_view text)
    {
        std::vector<Stack> loaded(slots_.size());
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
            if (def == nullptr || index < 0 || index >= slot_count() || count <= 0)
                continue;
            loaded[static_cast<std::size_t>(index)] = Stack{def, std::min(count, def->max_stack)};
        }
        if (!header)
            return false;
        slots_ = std::move(loaded);
        ++revision_;
        return true;
    }

} // namespace inv
