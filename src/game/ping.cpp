// Pings: what a player points at, told to their team, and drawn by their clients (see ping.hpp).

#include "game/ping.hpp"

#include "core/log.hpp"
#include "game/backend.hpp"
#include "game/client_remote.hpp"
#include "game/entities.hpp"
#include "game/players.hpp"
#include "game/weapons.hpp"
#include "inv/rules.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace game::detail
{
    std::vector<void*> carried_by_players(); // weapons.cpp
}

namespace game::detail
{

    using namespace sdk;

    namespace
    {
        constexpr int k_team_infected = 3;

        // ---- the server's half: what the player points at -------------------------------------------------------------
        //
        //   server.dll UTIL_TraceLine(start, end, mask, filter, trace_t*) 0x100101E0 with a CTraceFilterWorldOnly, the
        //   same call as the melee swing's wall test (see sdk.hpp).  The world alone is traced; the entities near the
        //   ray are found in the entity list, as the pick-up key finds an item, because the only filter the mod knows
        //   ignores them all.

        TraceLineFn g_trace_line = nullptr;
        SmartLookFn g_smart_look = nullptr; // what a `vocalize smartlook` says (see sdk.hpp)
        ConceptCtorFn g_concept_ctor = nullptr;
        std::map<std::uint32_t, float> g_last_ping; // by the pinger's entity handle: game_time of their last ping

        bool is_class(void* entity, const char* classname)
        {
            char name[32];
            return entity != nullptr && classname_of(entity, name, sizeof(name)) && std::strcmp(name, classname) == 0;
        }

        struct Target
        {
            PingKind kind = PingKind::Spot;
            Vec3 at{};
            std::string label;
            void* entity = nullptr; // what it is, for the voice
            std::string speech; // the speech concept the pinger's survivor says ("PlayerSpotPills"); empty: nothing
        };

        float dot(const Vec3& a, const Vec3& b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        // What a pick-up lying in the world is called: an item of the catalogue, or a map spawner of one
        // (weapon_pain_pills_spawn); empty for anything else.
        // The survivors' look-at line for an item (scripts/talker/terror_player.txt, "Triggered Smartlook Vocalizations":
        // "Pills here", "Ammo here" ...), by the item's classname without a spawner's suffix.
        const char* item_concept(const std::string& base)
        {
            // Only some of the spotting lines can be asked for by name: the survivors' rules for PlayerSpotAmmo,
            // PlayerSpotPistol, PlayerSpotShotgun and PlayerSpotOtherWeapon have a variant for a plain `vocalize`; the ones
            // for first aid kits, pills, grenades, SMGs, rifles and sniper rifles are only reached through the game's own look-at:
            // `vocalize smartlook #<n>` (the manual one: the auto one speaks only for what the player does not have yet, a
            // medkit when they carry none) says what the crosshair is on.
            if (base == "weapon_first_aid_kit" || base == "weapon_defibrillator" || base.rfind("weapon_upgradepack", 0) == 0 ||
                base == "weapon_pain_pills" || base == "weapon_adrenaline" || base == "weapon_pipe_bomb" || base == "weapon_molotov" ||
                base == "weapon_vomitjar" || base.rfind("weapon_smg", 0) == 0 || base == "weapon_rifle" || base == "weapon_rifle_ak47" ||
                base == "weapon_rifle_desert" || base == "weapon_rifle_sg552" || base == "weapon_hunting_rifle" ||
                base.rfind("weapon_sniper", 0) == 0)
                return "smartlook";
            if (base == "weapon_ammo")
                return "PlayerSpotAmmo";
            if (base == "weapon_pistol" || base == "weapon_pistol_magnum")
                return "PlayerSpotPistol";
            if (base == "weapon_pumpshotgun" || base == "weapon_shotgun_chrome" || base == "weapon_autoshotgun" ||
                base == "weapon_shotgun_spas")
                return "PlayerSpotShotgun";
            return base.rfind("weapon_", 0) == 0 ? "PlayerSpotOtherWeapon" : "";
        }

        std::string item_label(const char* classname)
        {
            if (const inv::ItemDef* def = inv::find_item(classname))
                return def->name;
            const std::string name = classname;
            constexpr char k_suffix[] = "_spawn";
            if (name.rfind("weapon_", 0) != 0 || name.size() <= sizeof(k_suffix) || name.compare(name.size() - (sizeof(k_suffix) - 1), std::string::npos, k_suffix) != 0)
                return {};
            const std::string base = name.substr(0, name.size() - (sizeof(k_suffix) - 1));
            if (const inv::ItemDef* def = inv::find_item(base))
                return def->name;
            if (name == "weapon_ammo_spawn")
                return "Ammo";
            return name == "weapon_spawn" || name == "weapon_item_spawn" || name == "weapon_melee_spawn" ? "Weapon" : std::string();
        }

        // The enemy or the item the ray passes closest to, before `reach` along it.  An enemy wins over an item, an item
        // over the world.  A pointed-at thing is within `slack` of the ray, which widens with the distance.
        bool find_target(void* pinger, const Vec3& eye, const Vec3& forward, float reach, Target& out)
        {
            const std::vector<void*> carried = carried_by_players();
            const bool survivor = team_of(pinger) == k_team_survivors;
            float best_enemy = 1e30f, best_item = 1e30f;
            Target enemy, item;
            for (int index = 1; index < 0x1000; ++index)
            {
                void* entity = entity_at_index(index);
                Vec3 origin{};
                if (entity == nullptr || entity == pinger || !mem::read(reinterpret_cast<std::uintptr_t>(entity) + k_off_origin, origin))
                    continue;
                // the middle of a standing character, not the feet
                const Vec3 chest{origin.x, origin.y, origin.z + 36.f};
                for (int pass = 0; pass < 2; ++pass)
                {
                    // an item is tall and its origin is anywhere on it (a melee weapon's is its middle, a box's its
                    // base): the ray passes close to its origin, or a hand's breadth above it
                    float along = 0.f, miss = 1e30f;
                    for (const float lift : {0.f, 14.f, 28.f})
                    {
                        if (pass == 0 && lift > 0.f)
                            break;
                        const Vec3& p = pass == 0 ? chest : origin;
                        const Vec3 d{p.x - eye.x, p.y - eye.y, p.z + lift - eye.z};
                        const float a = dot(d, forward);
                        const Vec3 off{d.x - forward.x * a, d.y - forward.y * a, d.z - forward.z * a};
                        const float m = std::sqrt(dot(off, off));
                        if (m < miss)
                        {
                            miss = m;
                            along = a;
                        }
                    }
                    if (along < 24.f || along > reach + 48.f)
                        continue;
                    if (pass == 0)
                    {
                        if (miss > 30.f + along * 0.035f || miss >= best_enemy)
                            continue;
                        char name[32];
                        if (!classname_of(entity, name, sizeof(name)))
                            break;
                        const bool common = std::strcmp(name, "infected") == 0, witch = std::strcmp(name, "witch") == 0;
                        const bool player = std::strcmp(name, "player") == 0;
                        if (!((survivor && (common || witch)) || (player && team_of(entity) != team_of(pinger) && team_of(entity) >= 2 &&
                                                                 alive(entity))))
                            continue;
                        if ((common || witch) && mem::read_or<int>(reinterpret_cast<std::uintptr_t>(entity) + k_off_health, 0) <= 0)
                            continue;
                        best_enemy = miss;
                        enemy.kind = PingKind::Enemy;
                        enemy.at = chest;
                        enemy.entity = entity;
                        enemy.speech = common ? "PlayerLook" : witch ? "PlayerWarnWitch" : "PlayerWarnSpecial";
                        enemy.label = common ? "Infected" : witch ? "Witch" : team_of(entity) == k_team_infected ? "Special infected" : "Enemy";
                        break;
                    }
                    if (miss > 36.f + along * 0.05f || miss >= best_item)
                        continue;
                    char name[64];
                    if (!classname_of(entity, name, sizeof(name)))
                        break;
                    std::string label = item_label(name);
                    if (label.empty() || std::find(carried.begin(), carried.end(), entity) != carried.end())
                        continue;
                    // gone from the world: hidden, or in somebody's hands (a bot's too: carried_by_players knows only the
                    // players with CTerrorPlayer's own vtable).  A spawner has no owner.
                    const auto address = reinterpret_cast<std::uintptr_t>(entity);
                    const bool spawner = std::strstr(name, "_spawn") != nullptr;
                    if ((mem::read_or<std::uint32_t>(address + k_off_effects) & k_ef_nodraw) != 0 ||
                        (!spawner && mem::read_or<std::uint32_t>(address + k_off_weapon_owner, k_invalid_handle) != k_invalid_handle))
                        continue;
                    best_item = miss;
                    item.kind = PingKind::Item;
                    item.at = {origin.x, origin.y, origin.z + 10.f};
                    item.label = std::move(label);
                    std::string base = name;
                    if (base.size() > 6 && base.compare(base.size() - 6, 6, "_spawn") == 0)
                        base.erase(base.size() - 6);
                    item.speech = item_concept(base);
                    item.entity = entity;
                }
            }
            if (enemy.kind == PingKind::Enemy)
                out = enemy;
            else if (item.kind == PingKind::Item)
                out = item;
            else
                return false;
            return true;
        }

        // The pinger's survivor says what they ping, as the game's own look-at does but for the pinged entity, not the one
        // its own use trace finds.  False when it cannot (not found, or the game knows nothing to say of the entity): the
        // pinger's client then asks for a line by name.
        bool speak_look(void* player, const Target& target)
        {
            if (g_smart_look == nullptr || g_concept_ctor == nullptr || target.entity == nullptr)
                return false;
            const auto speak = reinterpret_cast<SpeakFn>(mem::vslot(player, k_vt_speak));
            if (speak == nullptr || reinterpret_cast<std::uintptr_t>(speak) != g.server.base + k_rva_speak)
                return false;
            char modifiers[256] = "smartlooktype:manual";
            const char* name = nullptr;
            if (!mem::guarded([&] { name = g_smart_look(player, target.entity, modifiers, sizeof(modifiers)); }) || name == nullptr ||
                name[0] == '\0')
                return false;
            if (target.kind == PingKind::Item && std::strcmp(name, "PlayerLook") == 0)
                return false; // not a weapon the game knows: a line by name does better than "Look!"
            AiConcept concept_;
            INV_LOG("ping: %s says %s (%s)", "the pinger", name, modifiers);
            return mem::guarded([&] {
                g_concept_ctor(&concept_, name);
                speak(player, concept_, 2, modifiers, nullptr, 0, nullptr);
            });
        }

        // One recipient's line.
        void tell(void* recipient, bool mine, int owner, const Target& target, int life_ms, int per_player)
        {
            char line[256];
            std::snprintf(line, sizeof(line), "%s %d %d %d %d %d %.1f %.1f %.1f|%s|%s|%d\n", k_ping_marker, owner, mine ? 1 : 0,
                          static_cast<int>(target.kind), life_ms, per_player, target.at.x, target.at.y, target.at.z, target.label.c_str(), target.speech.c_str(), static_cast<int>(game_time() * 10.f));
            if (!g_dedicated && recipient == host_player())
            {
                mem::guarded([&] { apply_ping_line(line); });
                return;
            }
            ServerPlayer* sp = server_player(recipient);
            void* edict = edict_of(recipient);
            if (sp != nullptr && sp->has_mod && g_client_printf != nullptr && edict != nullptr)
                mem::guarded([&] { g_client_printf(g_engine_server, edict, line); });
        }

        // ---- the client's half: the marks and the projection -----------------------------------------------------------

        std::mutex g_marks_mutex;
        std::vector<PingMark> g_marks;
        unsigned g_serial = 0;

        WorldToScreenMatrixFn g_matrix_fn = nullptr; // the engine's WorldToScreenMatrix (VEngineClient013 slot 37)

        bool read_matrix(float (&m)[16])
        {
            const float* matrix = nullptr;
            if (g_matrix_fn == nullptr || !mem::guarded([&] { matrix = g_matrix_fn(g_engine_client); }) || matrix == nullptr ||
                !mem::safe_copy(m, matrix, sizeof(m)))
                return false;
            bool any = false;
            for (const float v : m)
            {
                if (!std::isfinite(v))
                    return false;
                any |= v != 0.f;
            }
            return any;
        }
    } // namespace

    void do_ping()
    {
        const inv::PingRules& rules = inv::ping_rules();
        void* player = local_player();
        if (player == nullptr)
        {
            notify("Not in a game");
            return;
        }
        if (!rules.enabled)
        {
            notify("Pings are off on this server");
            return;
        }
        if (!alive(player))
            return;
        const std::uint32_t handle = handle_of(player);
        const float now = game_time();
        if (const auto it = g_last_ping.find(handle); it != g_last_ping.end() && now >= it->second && now - it->second < rules.cooldown)
            return;
        Vec3 eye{}, forward{};
        if (!player_view(player, eye, forward))
            return;

        // where the view meets the world
        Target target;
        float reach = rules.range;
        bool hit_world = false;
        if (g_trace_line != nullptr)
        {
            const Vec3 end{eye.x + forward.x * rules.range, eye.y + forward.y * rules.range, eye.z + forward.z * rules.range};
            alignas(16) unsigned char trace[k_trace_size] = {};
            const std::uintptr_t filter = g.server.base + k_rva_trace_filter_world_only; // a bare object: just its vtable
            const std::uintptr_t* filter_object = &filter;
            if (mem::guarded([&] { g_trace_line(&eye, &end, k_mask_solid, filter_object, trace); }))
            {
                const float fraction = *reinterpret_cast<float*>(trace + k_off_trace_fraction);
                if (fraction < 1.f && trace[k_off_trace_start_solid] == 0)
                {
                    const Vec3 normal = *reinterpret_cast<Vec3*>(trace + k_off_trace_normal);
                    reach = rules.range * fraction;
                    // a little off the surface, so the marker does not sink into it
                    target.at = {eye.x + forward.x * reach + normal.x * 6.f, eye.y + forward.y * reach + normal.y * 6.f,
                                 eye.z + forward.z * reach + normal.z * 6.f};
                    hit_world = true;
                }
            }
        }
        Target found;
        if (find_target(player, eye, forward, reach, found))
            target = found;
        else if (!hit_world)
            return; // the sky, or nothing in range
        g_last_ping[handle] = now;
        if (target.kind != PingKind::Spot && speak_look(player, target))
            target.speech.clear(); // said already: the client has nothing to ask for

        const int owner = user_id_of(player);
        const int life_ms = static_cast<int>(rules.lifetime * 1000.f);
        const int team = team_of(player);
        INV_LOG("ping: userid %d marks %s%s%s at %.0f %.0f %.0f", owner, target.kind == PingKind::Enemy ? "an enemy" : target.kind == PingKind::Item ? "an item" : "a spot",
                target.label.empty() ? "" : " ", target.label.c_str(), target.at.x, target.at.y, target.at.z);
        for (int index = 1; index <= 64; ++index)
        {
            void* other = entity_at_index(index);
            if (other == nullptr || !is_class(other, "player") || team_of(other) != team || is_bot(other))
                continue;
            tell(other, other == player, owner, target, life_ms, rules.per_player);
        }
    }

    void apply_ping_line(const char* text)
    {
        const char* body = text + sizeof(k_ping_marker) - 1;
        int owner = 0, mine = 0, kind = 0, life_ms = 0, keep = 1;
        float x = 0.f, y = 0.f, z = 0.f;
        if (std::sscanf(body, "%d %d %d %d %d %f %f %f", &owner, &mine, &kind, &life_ms, &keep, &x, &y, &z) != 8 || kind < 0 || kind > 2 ||
            life_ms < 500 || life_ms > 120000)
            return;
        PingMark mark;
        std::string speech;
        int server_tenths = 0;
        mark.owner = owner;
        mark.mine = mine != 0;
        mark.kind = static_cast<PingKind>(kind);
        mark.x = x;
        mark.y = y;
        mark.z = z;
        mark.born = GetTickCount64();
        mark.life_ms = life_ms;
        if (const char* bar = std::strchr(body, '|'))
        {
            mark.label = bar + 1;
            if (const std::size_t second = mark.label.find('|'); second != std::string::npos)
            {
                speech = mark.label.substr(second + 1);
                mark.label.erase(second);
                if (const std::size_t third = speech.find('|'); third != std::string::npos)
                {
                    server_tenths = std::atoi(speech.c_str() + third + 1);
                    speech.erase(third);
                }
            }
            while (!speech.empty() && (speech.back() == '\n' || speech.back() == '\r'))
                speech.pop_back();
            while (!mark.label.empty() && (mark.label.back() == '\n' || mark.label.back() == '\r'))
                mark.label.pop_back();
            if (mark.label.size() > 48)
                mark.label.resize(48);
        }
        // the pinger's character says what it is ("Pills here", "Ammo here"): the console command vocalize with the speech
        // concept the server chose for it (see item_concept).  The server's handler (CTerrorPlayer's ClientCommand,
        // 0x1033B2AA) speaks any concept but smartlook only when argument 2 is "#<n>" with n / 10 within 2 seconds of the
        // server's game time, so the line carries the server's clock.
        if (mark.mine && !speech.empty() && speech.size() < 40 &&
            std::all_of(speech.begin(), speech.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }))
            send_to_server(speech == "smartlook" ? std::string("vocalize smartlook #") + std::to_string(server_tenths)
                                                 : "vocalize " + speech + " #" + std::to_string(server_tenths));
        std::lock_guard lock(g_marks_mutex);
        // a player keeps `keep` pings: this one replaces their oldest
        keep = std::clamp(keep, 1, 8);
        for (int count = static_cast<int>(std::count_if(g_marks.begin(), g_marks.end(), [&](const PingMark& m) { return m.owner == owner; }));
             count >= keep; --count)
            g_marks.erase(std::find_if(g_marks.begin(), g_marks.end(), [&](const PingMark& m) { return m.owner == owner; }));
        mark.serial = ++g_serial;
        g_marks.push_back(std::move(mark));
    }

    void install_ping_voice();

    void install_ping()
    {
        const std::uintptr_t trace = g.server.base + k_rva_trace_line;
        const std::uintptr_t filter_vtable = g.server.base + k_rva_trace_filter_world_only;
        if (mem::find_pattern(trace, 32, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 83 EC 5C 56 8B 43 08 8B 4B 0C") == trace &&
            mem::find_pattern(mem::read_or<std::uintptr_t>(filter_vtable), 5, "32 C0 C2 08 00") != 0 &&
            mem::find_pattern(mem::read_or<std::uintptr_t>(filter_vtable + 4), 6, "B8 01 00 00 00 C3") != 0)
        {
            g_trace_line = reinterpret_cast<TraceLineFn>(trace);
            INV_LOG("game: pings ready (on when inventory_mod.toml has [ping])");
        }
        else
            INV_LOG("game: UTIL_TraceLine does not look as expected: a ping marks only items and enemies");
        install_ping_voice(); // (apart from the trace)
    }

    void install_ping_voice()
    {
        const std::uintptr_t look = g.server.base + k_rva_smart_look;
        const std::uintptr_t ctor = g.server.base + k_rva_ai_concept_ctor;
        const std::uintptr_t speak = g.server.base + k_rva_speak;
        if (mem::find_pattern(look, 26, "55 8B EC 81 EC 30 01 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 8B 45 08 53 8B 5D 10") == look &&
            mem::find_pattern(ctor, 21, "55 8B EC 56 8B F1 B8 FF FF 00 00 66 89 06 C7 46 04 FF FF FF FF") == ctor &&
            mem::read_or<std::uintptr_t>(g.player_vtable + 4u * k_vt_speak) == speak &&
            mem::find_pattern(speak, 18, "55 8B EC 53 56 8B F1 8B 06 8B 90 28 01 00 00 57 FF D2") == speak)
        {
            g_smart_look = reinterpret_cast<SmartLookFn>(look);
            g_concept_ctor = reinterpret_cast<ConceptCtorFn>(ctor);
            INV_LOG("game: a ping makes the pinger's survivor say what it is");
        }
        else
            INV_LOG("game: the survivors' look-at speech does not look as expected: a ping asks for a line by name");
    }

    void install_ping_client()
    {
        mem::ModuleInfo engine;
        if (g_engine_client == nullptr || !mem::module_info(L"engine.dll", engine))
            return;
        const std::uintptr_t slot = mem::vslot(g_engine_client, k_vt_world_to_screen_matrix);
        // mov ecx, [the renderer]; mov eax, [ecx]; mov edx, [eax+0x34]; jmp edx
        if (slot == 0 || mem::find_pattern(slot, 13, "8B 0D ?? ?? ?? ?? 8B 01 8B 50 34 FF E2") != slot)
        {
            INV_LOG("game: the engine's WorldToScreenMatrix (%p) does not look as expected: pings are not drawn", reinterpret_cast<void*>(slot));
            return;
        }
        g_matrix_fn = reinterpret_cast<WorldToScreenMatrixFn>(slot);
        INV_LOG("game: pings are drawn with the engine's WorldToScreenMatrix (%p)", reinterpret_cast<void*>(slot));
    }

} // namespace game::detail

namespace game
{

    std::vector<PingMark> ping_marks()
    {
        const ULONGLONG now = GetTickCount64();
        std::lock_guard lock(detail::g_marks_mutex);
        std::erase_if(detail::g_marks, [&](const PingMark& m) { return now - m.born >= static_cast<ULONGLONG>(m.life_ms); });
        return detail::g_marks;
    }

    unsigned ping_serial()
    {
        std::lock_guard lock(detail::g_marks_mutex);
        return detail::g_serial;
    }

    void ping_clear()
    {
        std::lock_guard lock(detail::g_marks_mutex);
        detail::g_marks.clear();
    }

    bool ping_project(float x, float y, float z, float& ndc_x, float& ndc_y, bool& in_front)
    {
        float m[16];
        if (!detail::read_matrix(m))
            return false;
        const float cx = m[0] * x + m[1] * y + m[2] * z + m[3];
        const float cy = m[4] * x + m[5] * y + m[6] * z + m[7];
        const float w = m[12] * x + m[13] * y + m[14] * z + m[15];
        in_front = w > 0.001f;
        // behind the camera x / w and y / w are mirrored: flipping them points the way the marker is off the screen
        const float inv = 1.f / (in_front ? w : -std::max(std::fabs(w), 0.001f));
        ndc_x = cx * inv;
        ndc_y = cy * inv;
        return true;
    }

    bool ping_camera(float& x, float& y, float& z)
    {
        float m[16];
        if (!detail::read_matrix(m))
            return false;
        // rows 0, 1 and 3 (x, y, w) are 0 at the eye: A * c = -d, solved by Cramer's rule
        const float a[3][3] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[12], m[13], m[14]}};
        const float d[3] = {-m[3], -m[7], -m[15]};
        const auto det3 = [](const float (&r)[3][3]) {
            return r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                   r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
        };
        const float det = det3(a);
        if (std::fabs(det) < 1e-12f)
            return false;
        float out[3];
        for (int column = 0; column < 3; ++column)
        {
            float t[3][3];
            std::memcpy(t, a, sizeof(t));
            for (int row = 0; row < 3; ++row)
                t[row][column] = d[row];
            out[column] = det3(t) / det;
        }
        x = out[0];
        y = out[1];
        z = out[2];
        return true;
    }

} // namespace game
