#include "ui/audio.hpp"

#include "core/config.hpp"
#include "core/hook.hpp"
#include "core/log.hpp"
#include "game/mem.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>
#include <mmreg.h>
#include <dsound.h>

namespace audio
{

    namespace
    {
        namespace mem = game::mem;

        // engine.dll 2.2.4.3, CAudioDirectSound (vtable 0x103518EC):
        //   TransferSamples 0x1000A8E0: Lock (vtable + 0x2C) of the buffer in 0x1043F3A4, the paint buffer copied in,
        //     then   8B 75 FC  8B 0D [0x1043F3A4]  8B 11  56 50 8B 45 F8 50 8B 45 08 50 51  8B 4A 4C  FF D1
        //     Unlock (+0x4C) from 0x1000AA56;
        //   the surround transfer 0x100091E0 ends with
        //     8B 55 E8  A1 [0x1043F3A4]  8B 08  6A 00 6A 00 52 8B 55 F8 52 50  8B 41 4C  FF D0
        //     Unlock from 0x10009409.
        //   (ClearBuffer, slot 14, Unlocks the same buffer from elsewhere: left alone.)
        constexpr const char* k_stereo_unlock = "8B 75 FC 8B 0D ?? ?? ?? ?? 8B 11 56 50 8B 45 F8 50 8B 45 08 50 51 8B 4A 4C FF D1";
        constexpr std::size_t k_stereo_return = 27, k_stereo_global = 5;
        constexpr const char* k_surround_unlock = "8B 55 E8 A1 ?? ?? ?? ?? 8B 08 6A 00 6A 00 52 8B 55 F8 52 50 8B 41 4C FF D0";
        constexpr std::size_t k_surround_return = 25;
        constexpr int k_vt_unlock = 19; // IDirectSoundBuffer: ... 11 Lock ... 19 Unlock
        // The game's master volume (the "volume" ConVar, "Sound volume", 0x1044A560), registered at 0x10338200:
        //   B9 [ConVar]  6A 01  68 ["Sound volume"]  68 80 00 00 01  68 ["1.0"]  68 ["volume"]  E8 [ConVar::ConVar]
        // The engine applies it while it mixes; what the mod adds (the scream, the heartbeat) is scaled by it here.
        // ConVar: m_pParent +0x1C (the value lives there), m_fValue +0x2C (m_nValue +0x30).
        constexpr std::size_t k_convar_parent = 0x1C, k_convar_float = 0x2C;
        std::uintptr_t g_volume_convar = 0;

        using UnlockFn = HRESULT(WINAPI*)(IDirectSoundBuffer*, LPVOID, DWORD, LPVOID, DWORD);
        core::Hook<UnlockFn> g_unlock;
        std::uintptr_t g_ret_stereo = 0, g_ret_surround = 0;
        IDirectSoundBuffer** g_engine_buffer = nullptr;
        std::mutex g_install_mutex;
        std::atomic<bool> g_installed{false};
        ULONGLONG g_last_try = 0;

        // what the HUD says (render thread -> the mixing thread)
        std::atomic<float> g_severity{0.f};
        std::atomic<double> g_beats{0.0};
        std::atomic<ULONGLONG> g_updated{0}; // GetTickCount64 of the last set_infection
        std::atomic<bool> g_scream_start{false};

        // the scream: mono, -1 .. 1
        std::vector<float> g_scream;
        int g_scream_rate = 44100;
        std::atomic<bool> g_scream_loaded{false};
        std::string g_scream_path; // what g_scream was read from

        // ---- the mixing thread's state ---------------------------------------------------------------------------
        constexpr int k_max_channels = 8;
        constexpr int k_delay = 2048; // frames, a power of two
        struct State
        {
            IDirectSoundBuffer* buffer = nullptr;
            int channels = 0, rate = 0;
            bool usable = false;
            DWORD buffer_bytes = 0;
            float mix = 0.f; // 0 .. 1, faded in and out
            float severity = 0.f; // smoothed
            float lp1[k_max_channels] = {}, lp2[k_max_channels] = {};
            float delay[k_max_channels][k_delay] = {};
            int write = 0;
            double lfo = 0.0, lfo2 = 0.0, ring = 0.0, thump_phase = 0.0;
            double beats = 0.0; // own count, kept close to the HUD's
            float thump = 0.f; // the envelope of the last beat
            double scream_at = -1.0; // position in g_scream, -1: not screaming
            std::uint32_t noise = 22222;
        };
        State g_state;
        bool g_logged = false;

        float soft_clip(float x)
        {
            return x / (1.f + std::fabs(x));
        }

        // Untouched below 0.8, then bent smoothly toward 1 (no hard clipping when the scream lands on a loud mix).
        float limit(float x)
        {
            const float a = std::fabs(x);
            return a <= 0.8f ? x : std::copysign(0.8f + 0.2f * std::tanh((a - 0.8f) / 0.2f), x);
        }

        // The game's "volume" (0 .. 1; 1 when it cannot be read).  (mixing thread, inside process_guarded's __try)
        float game_volume()
        {
            if (g_volume_convar == 0)
                return 1.f;
            std::uintptr_t parent = *reinterpret_cast<const std::uintptr_t*>(g_volume_convar + k_convar_parent);
            if (parent == 0)
                parent = g_volume_convar;
            return std::clamp(*reinterpret_cast<const float*>(parent + k_convar_float), 0.f, 1.f);
        }

        void process(State& st, std::int16_t* data, DWORD bytes)
        {
            const int channels = st.channels;
            const DWORD frames = bytes / (2u * static_cast<DWORD>(channels));
            const float sr = static_cast<float>(st.rate);
            const ULONGLONG now = GetTickCount64();
            const bool infected = now - g_updated.load(std::memory_order_relaxed) < 300 && core::config().infection_sound;
            const float target_severity = std::clamp(g_severity.load(std::memory_order_relaxed), 0.f, 1.f);
            const float volume = game_volume(); // what the mod adds is as loud as the game's own sounds
            if (g_scream_start.exchange(false))
                st.scream_at = g_scream_loaded.load() ? 0.0 : -1.0;
            const bool screaming = st.scream_at >= 0.0;
            if (!infected && st.mix <= 0.f && !screaming)
                return;

            // the HUD's heartbeat: catch up when it is ahead, start again when it went back (a new infection)
            const double hud_beats = g_beats.load(std::memory_order_relaxed);
            if (hud_beats < st.beats - 2.0 || hud_beats > st.beats + 2.0)
                st.beats = hud_beats;
            else if (hud_beats > st.beats)
                st.beats += (hud_beats - st.beats) * 0.25;

            const float fade_step = 1.f / (0.6f * sr); // in or out over 0.6 s
            const float scream_step = static_cast<float>(g_scream_rate) / sr;
            const std::size_t scream_len = g_scream.size();
            constexpr float k_two_pi = 6.2831853f;

            for (DWORD f = 0; f < frames; ++f)
            {
                st.mix = infected ? std::min(1.f, st.mix + fade_step) : std::max(0.f, st.mix - fade_step);
                st.severity += (target_severity - st.severity) * 0.00005f;
                const float s = st.severity, m = st.mix;

                // the heartbeat: lub on the beat, dub a quarter later
                const float bpm = 50.f + 100.f * s;
                const double before = st.beats;
                st.beats += bpm / 60.f / sr;
                const double whole = std::floor(st.beats);
                const bool lub = std::floor(before) != whole;
                const bool dub = before - std::floor(before) < 0.25 && st.beats - whole >= 0.25 && !lub;
                if (lub || dub)
                {
                    st.thump = lub ? 1.f : 0.7f;
                    st.thump_phase = 0.0;
                }
                const float thump = st.thump * std::sin(static_cast<float>(st.thump_phase) * k_two_pi);
                st.thump_phase += (52.0 - 14.0 * st.thump) / sr; // the pitch falls as it decays
                st.thump *= 1.f - 1.f / (0.11f * sr);

                // the sway: a delay swept by two slow waves (the pitch bends up and down)
                const float depth = (0.0004f + 0.0065f * s) * sr * m;
                st.lfo += (0.35 + 1.6 * s) / sr;
                st.lfo2 += (0.13 + 0.4 * s) / sr;
                const float sweep = 0.5f + 0.35f * std::sin(static_cast<float>(st.lfo) * k_two_pi) +
                                    0.15f * std::sin(static_cast<float>(st.lfo2) * k_two_pi);
                const float read_delay = 2.f + depth * sweep;
                const float cutoff = 15000.f * std::pow(1.f - 0.95f * s, 1.6f) + 500.f; // muffled
                const float lp = 1.f - std::exp(-k_two_pi * std::min(cutoff, sr * 0.45f) / sr);
                const float duck = 1.f - (0.12f + 0.4f * s) * st.thump * m; // the world pulses with the heart
                st.ring += 7100.0 / sr;
                const float ring = s > 0.35f ? 0.012f * (s - 0.35f) / 0.65f * m *
                                                   (0.6f + 0.4f * std::sin(static_cast<float>(st.lfo2) * 9.f)) *
                                                   std::sin(static_cast<float>(st.ring) * k_two_pi)
                                             : 0.f;
                // near the end the sound saturates (a soft clip at the same level for quiet sounds: no hiss)
                const float drive = 1.f + (s > 0.75f ? 1.5f * (s - 0.75f) / 0.25f * m : 0.f);

                float scream = 0.f;
                if (st.scream_at >= 0.0)
                {
                    const std::size_t i = static_cast<std::size_t>(st.scream_at);
                    if (i + 1 < scream_len)
                    {
                        const float t = static_cast<float>(st.scream_at - static_cast<double>(i));
                        scream = (g_scream[i] * (1.f - t) + g_scream[i + 1] * t) * volume;
                        st.scream_at += scream_step;
                    }
                    else
                        st.scream_at = -1.0;
                }

                for (int c = 0; c < channels; ++c)
                {
                    std::int16_t& sample = data[f * static_cast<DWORD>(channels) + static_cast<DWORD>(c)];
                    const float dry = static_cast<float>(sample) / 32768.f;
                    float* line = st.delay[c];
                    line[st.write] = dry;
                    const float pos = static_cast<float>(st.write) - read_delay + k_delay;
                    const int i0 = static_cast<int>(pos);
                    const float frac = pos - static_cast<float>(i0);
                    float y = line[i0 & (k_delay - 1)] * (1.f - frac) + line[(i0 + 1) & (k_delay - 1)] * frac;
                    st.lp1[c] += lp * (y - st.lp1[c]);
                    st.lp2[c] += lp * (st.lp1[c] - st.lp2[c]);
                    y = st.lp2[c];
                    if (drive > 1.f)
                        y = soft_clip(y * drive) / drive * (1.f + 0.25f * (drive - 1.f));
                    float out = (dry + (y * duck - dry) * m) +
                                (thump * (0.10f + 0.32f * s) + ring) * volume * m * (c < 2 ? 1.f : 0.5f);
                    if (scream != 0.f)
                        out = limit(out * 0.6f + scream); // the game a little under it, the scream at the game's level
                    sample = static_cast<std::int16_t>(std::clamp(out, -1.f, 1.f) * 32767.f);
                }
                st.write = (st.write + 1) & (k_delay - 1);
            }
        }

        bool format_of(State& st, IDirectSoundBuffer* buffer)
        {
            if (st.buffer == buffer)
                return st.usable;
            st = State{};
            st.buffer = buffer;
            alignas(8) unsigned char raw[64] = {};
            DWORD written = 0;
            DSBCAPS caps{sizeof(DSBCAPS)};
            if (FAILED(buffer->GetFormat(reinterpret_cast<WAVEFORMATEX*>(raw), sizeof(raw), &written)) ||
                FAILED(buffer->GetCaps(&caps)))
                return false;
            const auto* wfx = reinterpret_cast<const WAVEFORMATEX*>(raw);
            const bool pcm = wfx->wFormatTag == WAVE_FORMAT_PCM || wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE;
            st.channels = wfx->nChannels;
            st.rate = static_cast<int>(wfx->nSamplesPerSec);
            st.buffer_bytes = caps.dwBufferBytes;
            st.usable = pcm && wfx->wBitsPerSample == 16 && st.channels >= 1 && st.channels <= k_max_channels && st.rate >= 8000;
            INV_LOG("audio: the engine's buffer %p: %d channels, %d Hz, %d bits, %lu bytes (%s)", static_cast<void*>(buffer),
                    st.channels, st.rate, wfx->wBitsPerSample, static_cast<unsigned long>(st.buffer_bytes),
                    st.usable ? "processed" : "left alone");
            return st.usable;
        }

        void process_guarded(IDirectSoundBuffer* buffer, LPVOID p1, DWORD b1, LPVOID p2, DWORD b2)
        {
            __try
            {
                if (!format_of(g_state, buffer))
                    return;
                if (b1 + b2 >= g_state.buffer_bytes) // the whole buffer: a clear, not a mix
                    return;
                if (p1 != nullptr && b1 != 0)
                    process(g_state, static_cast<std::int16_t*>(p1), b1);
                if (p2 != nullptr && b2 != 0)
                    process(g_state, static_cast<std::int16_t*>(p2), b2);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                if (!g_logged)
                    INV_LOG("audio: fault while processing the mix (left alone from now)");
                g_logged = true;
                g_state.usable = false;
            }
        }

        HRESULT WINAPI hk_unlock(IDirectSoundBuffer* buffer, LPVOID p1, DWORD b1, LPVOID p2, DWORD b2)
        {
            const auto from = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
            if ((from == g_ret_stereo || from == g_ret_surround) && !g_logged)
                process_guarded(buffer, p1, b1, p2, b2);
            return g_unlock.original()(buffer, p1, b1, p2, b2);
        }

        // ---- the scream ------------------------------------------------------------------------------------------

        std::wstring widen(const std::string& text)
        {
            std::wstring out(text.size(), L'\0');
            const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                                              static_cast<int>(out.size()));
            out.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
            std::replace(out.begin(), out.end(), L'/', L'\\');
            return out;
        }

        bool read_all(const std::wstring& path, std::vector<unsigned char>& out)
        {
            FILE* f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr)
                return false;
            std::fseek(f, 0, SEEK_END);
            const long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            out.resize(size > 0 ? static_cast<std::size_t>(size) : 0);
            const bool ok = size > 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
            std::fclose(f);
            return ok;
        }

        // A 16-bit PCM .wav, mixed down to mono.
        bool parse_wav(const std::vector<unsigned char>& d, std::vector<float>& out, int& rate)
        {
            if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0)
                return false;
            int channels = 0, bits = 0, tag = 0;
            for (std::size_t i = 12; i + 8 <= d.size();)
            {
                std::uint32_t n = 0;
                std::memcpy(&n, d.data() + i + 4, 4);
                const unsigned char* body = d.data() + i + 8;
                if (i + 8 + n > d.size())
                    n = static_cast<std::uint32_t>(d.size() - i - 8);
                if (std::memcmp(d.data() + i, "fmt ", 4) == 0 && n >= 16)
                {
                    std::uint16_t v16 = 0;
                    std::uint32_t v32 = 0;
                    std::memcpy(&v16, body, 2), tag = v16;
                    std::memcpy(&v16, body + 2, 2), channels = v16;
                    std::memcpy(&v32, body + 4, 4), rate = static_cast<int>(v32);
                    std::memcpy(&v16, body + 14, 2), bits = v16;
                }
                else if (std::memcmp(d.data() + i, "data", 4) == 0)
                {
                    if ((tag != WAVE_FORMAT_PCM && tag != WAVE_FORMAT_EXTENSIBLE) || bits != 16 || channels < 1 || rate < 8000)
                        return false;
                    const std::size_t frames = n / (2u * static_cast<std::size_t>(channels));
                    out.assign(frames, 0.f);
                    const auto* samples = reinterpret_cast<const std::int16_t*>(body);
                    for (std::size_t f = 0; f < frames; ++f)
                    {
                        float sum = 0.f;
                        for (int c = 0; c < channels; ++c)
                            sum += static_cast<float>(samples[f * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)]);
                        out[f] = sum / (32768.f * static_cast<float>(channels));
                    }
                    return frames > 0;
                }
                i += 8 + n + (n & 1u);
            }
            return false;
        }

        std::wstring game_dir()
        {
            wchar_t path[MAX_PATH];
            const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring dir(path, n);
            return dir.substr(0, dir.find_last_of(L'\\') + 1);
        }

        // (render thread, while no scream plays)
        void load_scream()
        {
            const std::string& name = core::config().screamer_sound;
            if (name == g_scream_path)
                return;
            g_scream_path = name;
            g_scream_loaded = false;
            const std::wstring wide = widen(name);
            std::vector<std::wstring> candidates;
            if (wide.find(L':') != std::wstring::npos || wide.rfind(L"\\\\", 0) == 0)
                candidates.push_back(wide);
            else
            {
                const std::wstring dir = game_dir();
                for (const wchar_t* sub : {L"update", L"left4dead2_dlc3", L"left4dead2_dlc2", L"left4dead2_dlc1", L"left4dead2"})
                    candidates.push_back(dir + sub + L"\\sound\\" + wide);
                candidates.push_back(std::wstring(core::module_dir()) + wide);
            }
            std::vector<unsigned char> bytes;
            for (const std::wstring& path : candidates)
            {
                if (!read_all(path, bytes))
                    continue;
                std::vector<float> samples;
                int rate = 44100;
                if (!parse_wav(bytes, samples, rate))
                {
                    INV_LOG("audio: %s is not a 16-bit PCM .wav", name.c_str());
                    return;
                }
                g_scream = std::move(samples);
                g_scream_rate = rate;
                g_scream_loaded = true;
                INV_LOG("audio: the scream is %s (%.1f s)", name.c_str(), static_cast<double>(g_scream.size()) / rate);
                return;
            }
            INV_LOG("audio: no scream: %s not found", name.c_str());
        }
    } // namespace

    bool install()
    {
        if (g_installed.load())
            return true;
        std::lock_guard lock(g_install_mutex);
        if (g_installed.load())
            return true;
        const ULONGLONG now = GetTickCount64();
        if (g_last_try != 0 && now - g_last_try < 2000)
            return false;
        const bool first = g_last_try == 0;
        g_last_try = now;

        mem::ModuleInfo engine;
        if (!mem::module_info(L"engine.dll", engine))
            return false;
        const std::uintptr_t stereo = mem::find_pattern(engine.base, engine.size, k_stereo_unlock);
        const std::uintptr_t surround = mem::find_pattern(engine.base, engine.size, k_surround_unlock);
        if (stereo == 0)
        {
            if (first)
                INV_LOG("audio: the engine's Unlock of its mix was not found (another build?): no infection sound");
            g_last_try = ~0ULL >> 1; // never again
            return false;
        }
        g_ret_stereo = stereo + k_stereo_return;
        g_ret_surround = surround != 0 ? surround + k_surround_return : 0;
        if (const std::uintptr_t text = mem::find_pattern(engine.base, engine.size, "00 76 6F 6C 75 6D 65 00")) // "\0volume\0"
        {
            char code[96];
            const std::uintptr_t name = text + 1;
            std::snprintf(code, sizeof(code), "B9 ?? ?? ?? ?? 6A 01 68 ?? ?? ?? ?? 68 80 00 00 01 68 ?? ?? ?? ?? 68 %02X %02X %02X %02X E8",
                          static_cast<unsigned>(name & 0xFF), static_cast<unsigned>((name >> 8) & 0xFF),
                          static_cast<unsigned>((name >> 16) & 0xFF), static_cast<unsigned>((name >> 24) & 0xFF));
            if (const std::uintptr_t at = mem::find_pattern(engine.base, engine.size, code))
                g_volume_convar = mem::read_or<std::uintptr_t>(at + 1);
        }
        {
            const std::uintptr_t parent = g_volume_convar != 0 ? mem::read_or<std::uintptr_t>(g_volume_convar + k_convar_parent) : 0;
            INV_LOG("audio: the game's volume ConVar %p: %.2f", reinterpret_cast<void*>(g_volume_convar),
                    g_volume_convar != 0 ? static_cast<double>(mem::read_or<float>((parent != 0 ? parent : g_volume_convar) + k_convar_float, -1.f))
                                         : -1.0);
        }
        g_engine_buffer = *reinterpret_cast<IDirectSoundBuffer***>(stereo + k_stereo_global);
        IDirectSoundBuffer* buffer = nullptr;
        if (!mem::read(reinterpret_cast<std::uintptr_t>(g_engine_buffer), buffer) || buffer == nullptr)
        {
            if (first)
                INV_LOG("audio: the game's sound is not up yet (no DirectSound buffer): trying again later");
            return false;
        }
        const std::uintptr_t unlock = mem::vslot(buffer, k_vt_unlock);
        if (unlock == 0 || !g_unlock.create("IDirectSoundBuffer::Unlock", reinterpret_cast<void*>(unlock), hk_unlock) ||
            !g_unlock.enable())
        {
            g_unlock.remove();
            g_last_try = ~0ULL >> 1;
            return false;
        }
        g_installed = true;
        INV_LOG("audio: engine buffer %p, Unlock %p hooked (from %p and %p)", static_cast<void*>(buffer),
                reinterpret_cast<void*>(unlock), reinterpret_cast<void*>(g_ret_stereo), reinterpret_cast<void*>(g_ret_surround));
        return true;
    }

    void set_infection(float severity, double beats)
    {
        install();
        g_severity.store(severity, std::memory_order_relaxed);
        g_beats.store(beats, std::memory_order_relaxed);
        g_updated.store(GetTickCount64(), std::memory_order_relaxed);
    }

    void scream()
    {
        install();
        if (g_state.scream_at < 0.0)
            load_scream();
        if (g_scream_loaded.load())
            g_scream_start = true;
    }

    void uninstall()
    {
        g_unlock.remove();
        g_installed = false;
        Sleep(50); // a mix inside the detour leaves
    }

} // namespace audio
