use std::{
    ffi::c_void,
    fs, io,
    path::{Path, PathBuf},
    sync::{
        Mutex,
        atomic::{AtomicUsize, Ordering},
    },
    time::{Duration, Instant},
};

use crate::{
    cvars::ConVar,
    native::{
        self, PeImage,
        resident::{CallbackSlots, Method},
    },
};

const DELAY: usize = 2048;
const MAX_CHANNELS: usize = 8;

type Unlock = unsafe extern "system" fn(*mut c_void, *mut c_void, u32, *mut c_void, u32) -> i32;
type Caps = unsafe extern "system" fn(*mut c_void, *mut u32) -> i32;
type Format = unsafe extern "system" fn(*mut c_void, *mut u8, u32, *mut u32) -> i32;

static ORIGINAL: AtomicUsize = AtomicUsize::new(0);
static ENGINE_BUFFER: AtomicUsize = AtomicUsize::new(0);
static MIX: Mutex<Option<Mixer>> = Mutex::new(None);

pub struct Audio {
    patch: Option<CallbackSlots>,
    global: Option<usize>,
    next_try: Option<Instant>,
    checked: bool,
    volume: Option<ConVar>,
}

struct Mixer {
    input: Input,
    played: Sample,
    scream: Option<Scream>,
}

struct Input {
    updated: Instant,
    severity: f32,
    beats: f64,
    volume: f32,
    enabled: bool,
}

#[derive(Default)]
struct Sample {
    buffer: usize,
    format: Option<NativeFormat>,
    mix: f32,
    severity: f32,
    write: usize,
    beat: f64,
    beat_phase: f64,
    thump: f32,
    lfo: f64,
    lfo2: f64,
    ring: f64,
    delay: Vec<[f32; DELAY]>,
    filter1: [f32; MAX_CHANNELS],
    filter2: [f32; MAX_CHANNELS],
}

#[derive(Clone, Copy)]
struct NativeFormat {
    rate: f32,
    channels: usize,
    bytes: u32,
}

struct Scream {
    samples: Vec<f32>,
    rate: f32,
    position: f64,
}

impl Audio {
    pub fn new() -> Self {
        Self {
            patch: None,
            global: None,
            next_try: None,
            checked: false,
            volume: None,
        }
    }

    fn prepare(&mut self) -> bool {
        if self.patch.is_some() {
            return true;
        }
        let now = Instant::now();
        if self.next_try.is_some_and(|when| now < when) {
            return false;
        }
        self.next_try = Some(now + Duration::from_secs(2));
        if !self.checked {
            self.checked = true;
            self.global = native::module("engine.dll").and_then(PeImage::loaded).and_then(|image| {
                let code = &image.code;
                let stereo = native::find_pattern(code.start, code.len(),
                    "8B 75 FC 8B 0D ?? ?? ?? ?? 8B 11 56 50 8B 45 F8 50 8B 45 08 50 51 8B 4A 4C FF D1")?;
                let global = native::word(stereo + 5)? as usize;
                image.includes(global, 4).then_some(global)
            });
            self.volume = ConVar::find(c"volume");
        }
        let Some(global) = self.global else {
            return false;
        };
        let Some(buffer) = native::word(global)
            .map(|buffer| buffer as usize)
            .filter(|&buffer| buffer != 0)
        else {
            return false;
        };
        let Some(table) = native::word(buffer).map(|table| table as usize) else {
            return false;
        };
        let slot = table + 4 * 19;
        let Some(original) = native::word(slot)
            .map(|method| method as usize)
            .filter(|&method| method != 0)
        else {
            return false;
        };
        ORIGINAL.store(original, Ordering::Release);
        ENGINE_BUFFER.store(global, Ordering::Release);
        self.patch =
            CallbackSlots::install(Method::Unlock, &[slot], original, unlock as Unlock as usize)
                .ok();
        self.patch.is_some()
    }

    pub fn update(&mut self, want: bool, severity: f32, beats: f64) {
        // Install only when an infection needs processing.
        if (!want && self.patch.is_none()) || !self.prepare() {
            return;
        }
        let volume = self
            .volume
            .as_ref()
            .and_then(ConVar::scalar)
            .unwrap_or(1.0)
            .clamp(0.0, 1.0);
        let input = Input {
            updated: Instant::now(),
            severity: severity.clamp(0.0, 1.0),
            beats,
            volume,
            enabled: want,
        };
        if let Ok(mut mixer) = MIX.try_lock() {
            if let Some(mixer) = mixer.as_mut() {
                mixer.input = input;
            } else {
                *mixer = Some(Mixer {
                    input,
                    played: Sample::default(),
                    scream: None,
                });
            }
        }
    }

    pub fn scream(&mut self, directory: &Path, filename: &str) -> bool {
        let Some(scream) = load_wav(directory, filename) else {
            return false;
        };
        if !self.prepare() {
            return false;
        }
        self.update(false, 0.0, 0.0);
        if let Ok(mut mix) = MIX.lock()
            && let Some(mix) = mix.as_mut()
        {
            mix.scream = Some(scream);
            return true;
        }
        false
    }

    pub fn stop(&mut self) -> io::Result<()> {
        if let Some(patch) = &mut self.patch {
            patch.stop()?;
        }
        ENGINE_BUFFER.store(0, Ordering::Release);
        if let Ok(mut mix) = MIX.lock() {
            *mix = None;
        }
        self.patch = None;
        Ok(())
    }
}

fn format(buffer: usize) -> Option<NativeFormat> {
    let caps = native::vslot(buffer, 3)?;
    let format = native::vslot(buffer, 5)?;
    let caps = unsafe { std::mem::transmute::<usize, Caps>(caps) };
    let format = unsafe { std::mem::transmute::<usize, Format>(format) };
    let object = std::ptr::with_exposed_provenance_mut(buffer);
    let mut description = [0_u32; 5];
    description[0] = 20;
    let mut wave = [0_u8; 64];
    let mut limit = 0;
    if unsafe { caps(object, description.as_mut_ptr()) } < 0
        || unsafe { format(object, wave.as_mut_ptr(), wave.len() as u32, &mut limit) } < 0
        || limit < 16
    {
        return None;
    }
    let short = |at| u16::from_le_bytes(wave[at..at + 2].try_into().unwrap());
    let rate = u32::from_le_bytes(wave[4..8].try_into().unwrap()) as f32;
    let channels = usize::from(short(2));
    if !matches!(short(0), 1 | 0xfffe)
        || short(14) != 16
        || !(1..=MAX_CHANNELS).contains(&channels)
        || !(8000.0..=192000.0).contains(&rate)
    {
        return None;
    }
    Some(NativeFormat {
        rate,
        channels,
        bytes: description[2],
    })
}

impl Mixer {
    fn buffer(&mut self, identity: usize, first: (usize, u32), second: (usize, u32)) {
        if self.played.buffer != identity {
            let format = format(identity);
            self.played = Sample {
                buffer: identity,
                format,
                ..Sample::default()
            };
            if let Some(format) = format {
                self.played.delay.resize(format.channels, [0.0; DELAY]);
            }
        }
        let Some(format) = self.played.format else {
            return;
        };
        if first.1.saturating_add(second.1) >= format.bytes {
            return; // a clear, not a slice of mixed sound
        }
        let infected =
            self.input.enabled && self.input.updated.elapsed() < Duration::from_millis(300);
        if !infected && self.played.mix == 0.0 && self.scream.is_none() {
            return;
        }
        let difference = self.input.beats - self.played.beat;
        if difference.abs() > 2.0 {
            self.played.beat = self.input.beats;
        } else if difference > 0.0 {
            self.played.beat += difference * 0.25;
        }
        for (address, size) in [first, second] {
            if address == 0 || size == 0 {
                continue;
            }
            let mut bytes = vec![0; size as usize];
            if !native::read(address, &mut bytes) {
                continue;
            }
            for frame in bytes.chunks_exact_mut(2 * format.channels) {
                self.process(format, infected, frame);
            }
            native::store_bytes(address, &bytes);
        }
    }

    fn process(&mut self, format: NativeFormat, infected: bool, frame: &mut [u8]) {
        use std::f32::consts::TAU;
        let state = &mut self.played;
        let sr = format.rate;
        let direction = if infected { 1.0 } else { -1.0 };
        state.mix = (state.mix + direction / (0.6 * sr)).clamp(0.0, 1.0);
        state.severity += (self.input.severity - state.severity) * 0.00005;
        let s = state.severity;
        let m = state.mix;

        let before = state.beat;
        state.beat += f64::from((50.0 + 100.0 * s) / (60.0 * sr));
        let beat = state.beat.floor();
        let lub = before.floor() != beat;
        let dub = before.fract() < 0.25 && state.beat.fract() >= 0.25 && !lub;
        if lub || dub {
            state.thump = if lub { 1.0 } else { 0.7 };
            state.beat_phase = 0.0;
        }
        let thump = state.thump * (state.beat_phase as f32 * TAU).sin();
        state.beat_phase += f64::from((52.0 - 14.0 * state.thump) / sr);
        state.thump *= 1.0 - 1.0 / (0.11 * sr);

        state.lfo += f64::from((0.35 + 1.6 * s) / sr);
        state.lfo2 += f64::from((0.13 + 0.4 * s) / sr);
        let sweep =
            0.5 + 0.35 * (state.lfo as f32 * TAU).sin() + 0.15 * (state.lfo2 as f32 * TAU).sin();
        let delay = 2.0 + (0.0004 + 0.0065 * s) * sr * sweep * m;
        let cutoff = (15000.0 * (1.0 - 0.95 * s).powf(1.6) + 500.0).min(sr * 0.45);
        let filter = 1.0 - (-TAU * cutoff / sr).exp();
        let duck = 1.0 - (0.12 + 0.4 * s) * state.thump * m;
        state.ring += 7100.0 / f64::from(sr);
        let ring = 0.012 * (s - 0.35).max(0.0) / 0.65
            * m
            * (0.6 + 0.4 * (state.lfo2 as f32 * 9.0).sin())
            * (state.ring as f32 * TAU).sin();
        let drive = 1.0 + 1.5 * (s - 0.75).max(0.0) / 0.25 * m;
        let scream = self.scream.as_mut().and_then(|scream| {
            let index = scream.position as usize;
            let next = *scream.samples.get(index + 1)?;
            let frac = scream.position.fract() as f32;
            let tone = scream.samples[index] * (1.0 - frac) + next * frac;
            scream.position += f64::from(scream.rate / sr);
            Some(tone * self.input.volume)
        });
        if scream.is_none() {
            self.scream = None;
        }

        for (channel, bytes) in frame.as_chunks_mut::<2>().0.iter_mut().enumerate() {
            let dry = f32::from(i16::from_le_bytes(*bytes)) / 32768.0;
            let line = &mut state.delay[channel];
            line[state.write] = dry;
            let position = state.write as f32 + DELAY as f32 - delay;
            let first = position as usize;
            let amount = position.fract();
            let y = line[first & (DELAY - 1)] * (1.0 - amount)
                + line[(first + 1) & (DELAY - 1)] * amount;
            state.filter1[channel] += filter * (y - state.filter1[channel]);
            state.filter2[channel] += filter * (state.filter1[channel] - state.filter2[channel]);
            let mut wet = state.filter2[channel];
            if drive > 1.0 {
                wet = wet / (1.0 + (wet * drive).abs()) * (1.0 + 0.25 * (drive - 1.0));
            }
            let speaker = if channel < 2 { 1.0 } else { 0.5 };
            let mut out = dry
                + (wet * duck - dry) * m
                + (thump * (0.10 + 0.32 * s) + ring) * m * self.input.volume * speaker;
            if let Some(scream) = scream.filter(|&tone| tone != 0.0) {
                out = smooth_limit(out * 0.6 + scream);
            }
            *bytes = ((out.clamp(-1.0, 1.0) * 32767.0) as i16).to_le_bytes();
        }
        state.write = (state.write + 1) & (DELAY - 1);
    }
}

fn smooth_limit(value: f32) -> f32 {
    let magnitude = value.abs();
    if magnitude <= 0.8 {
        value
    } else {
        (0.8 + 0.2 * ((magnitude - 0.8) / 0.2).tanh()).copysign(value)
    }
}

fn load_wav(directory: &Path, name: &str) -> Option<Scream> {
    let file = Path::new(name);
    let locations = if file.is_absolute() {
        vec![file.to_path_buf()]
    } else {
        let mut locations = vec![directory.join(file)];
        if let Some(game) = std::env::current_exe()
            .ok()
            .and_then(|exe| exe.parent().map(PathBuf::from))
        {
            for folder in [
                "update",
                "left4dead2_dlc3",
                "left4dead2_dlc2",
                "left4dead2_dlc1",
                "left4dead2",
            ] {
                locations.push(game.join(folder).join("sound").join(file));
            }
        }
        locations
    };
    locations
        .into_iter()
        .find_map(|path| fs::read(path).ok().and_then(|data| parse_wav(&data)))
}

fn parse_wav(data: &[u8]) -> Option<Scream> {
    if data.get(..4)? != b"RIFF" || data.get(8..12)? != b"WAVE" {
        return None;
    }
    let mut next = 12;
    let mut encoding = None;
    while let Some(header) = data.get(next..next + 8) {
        let n = u32::from_le_bytes(header[4..8].try_into().ok()?) as usize;
        let start = next + 8;
        let end = start.checked_add(n)?;
        let body = data.get(start..end)?;
        if &header[..4] == b"fmt " && body.len() >= 16 {
            let u16 = |offset| u16::from_le_bytes(body[offset..offset + 2].try_into().unwrap());
            let channels = usize::from(u16(2));
            let rate = u32::from_le_bytes(body[4..8].try_into().unwrap()) as f32;
            if !matches!(u16(0), 1 | 0xfffe)
                || u16(14) != 16
                || !(1..=8).contains(&channels)
                || rate < 8000.0
            {
                return None;
            }
            encoding = Some((channels, rate));
        } else if &header[..4] == b"data" {
            let (channels, rate) = encoding?;
            let samples = body
                .chunks_exact(channels * 2)
                .map(|frame| {
                    frame
                        .as_chunks::<2>()
                        .0
                        .iter()
                        .map(|&pcm| f32::from(i16::from_le_bytes(pcm)) / 32768.0)
                        .sum::<f32>()
                        / channels as f32
                })
                .collect::<Vec<_>>();
            return (samples.len() >= 2).then_some(Scream {
                samples,
                rate,
                position: 0.0,
            });
        }
        next = end + (n & 1);
    }
    None
}

unsafe extern "system" fn unlock(
    buffer: *mut c_void,
    p1: *mut c_void,
    b1: u32,
    p2: *mut c_void,
    b2: u32,
) -> i32 {
    let global = ENGINE_BUFFER.load(Ordering::Acquire);
    let _ = std::panic::catch_unwind(|| {
        if global != 0
            && native::word(global).map(|addr| addr as usize) == Some(buffer.addr())
            && let Ok(mut mixer) = MIX.try_lock()
            && let Some(mixer) = mixer.as_mut()
        {
            mixer.buffer(buffer.addr(), (p1.addr(), b1), (p2.addr(), b2));
        }
    });
    let original = ORIGINAL.load(Ordering::Acquire);
    if original == 0 {
        return -1;
    }
    let original = unsafe { std::mem::transmute::<usize, Unlock>(original) };
    unsafe { original(buffer, p1, b1, p2, b2) }
}
