#pragma once
// The infection's sound: the game's own mix, processed where the engine hands it to DirectSound.  engine.dll mixes into
// one looping IDirectSoundBuffer and Unlocks what it wrote (CAudioDirectSound's TransferSamples); the mod hooks that
// Unlock and, for those two call sites only, works on the 16-bit PCM in place: the world muffled and warped more and
// more, a heartbeat thumping under it, a ringing in the ears, crushed near the end; and the screamer's scream.

namespace audio
{

    // Finds the engine's buffer and hooks its Unlock (again later if the game's sound is not up yet).  Any thread.
    bool install();
    // While infected, every frame (render thread): how bad it is (0 .. 1) and the heartbeat, in beats since it started
    // (the HUD's count: the thumps fall with the screen's throbs).  Without a call for a moment the effect fades out.
    void set_infection(float severity, double beats);
    // The screamer's scream (settings: screamer_sound), loud, from now.
    void scream();
    void uninstall();

} // namespace audio
