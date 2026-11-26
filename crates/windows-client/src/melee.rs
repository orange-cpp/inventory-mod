use std::{
    ffi::{c_char, c_void},
    io, ptr,
    sync::atomic::{AtomicUsize, Ordering},
    time::Instant,
};

use inventory_mod::melee::{
    BUTTON_HEAVY, BUTTON_RELOAD, MeleeFrame, MeleeInput, Shove, Swing, melee_overhead_swing,
};
use windows_sys::Win32::System::LibraryLoader::GetProcAddress;

use crate::{
    client::with_client,
    engine::Link,
    native::{self, Detour, PeImage},
};

type LocalPlayer = unsafe extern "thiscall" fn(*mut c_void) -> i32;
type Entity = unsafe extern "thiscall" fn(*mut c_void, i32) -> *mut c_void;
type Factory = unsafe extern "C" fn(*const c_char, *mut i32) -> *mut c_void;
type Buttons = unsafe extern "thiscall" fn(*mut c_void, i32) -> i32;
type Choose = unsafe extern "thiscall" fn(*mut c_void, *mut c_void) -> i32;
type Pose = unsafe extern "thiscall" fn(*mut c_void, *mut c_void, *const Vec3, *const Vec3);

const GLOBALS_READ: usize = 0x17922;

static BUTTONS_ORIGINAL: AtomicUsize = AtomicUsize::new(0);
static CHOOSE_ORIGINAL: AtomicUsize = AtomicUsize::new(0);
static POSE_ORIGINAL: AtomicUsize = AtomicUsize::new(0);

#[derive(Clone, Copy)]
pub struct View {
    engine: usize,
    player_index: LocalPlayer,
    entities: usize,
    entity_at: Entity,
    entity_for: Entity,
}

#[derive(Clone, Copy, Default)]
pub struct WeaponView {
    pub clip: i32,
    pub reserve: Option<i32>,
}

fn matches(addr: usize, pattern: &str) -> bool {
    let bytes = pattern
        .split_ascii_whitespace()
        .map(|p| u8::from_str_radix(p, 16).ok())
        .collect::<Vec<_>>();
    native::matches(addr, &bytes)
}

impl View {
    pub fn bind(engine: Link) -> Option<Self> {
        let module = native::module("client.dll")?;
        let factory =
            unsafe { GetProcAddress(module.as_ptr(), c"CreateInterface".as_ptr().cast()) }?;
        let factory = unsafe {
            std::mem::transmute::<unsafe extern "system" fn() -> isize, Factory>(factory)
        };
        let list = unsafe { factory(c"VClientEntityList003".as_ptr(), ptr::null_mut()) }.addr();
        if list == 0 {
            return None;
        }
        let local = native::vslot(engine.interface(), 12)?;
        let at = native::vslot(list, 3)?;
        let from = native::vslot(list, 4)?;
        if !matches(
            local,
            "6A FF E8 ?? ?? ?? ?? 8B 80 D8 00 00 00 83 C4 04 40 C3",
        ) || !matches(at, "55 8B EC 8B 45 08 85 C0 78 ?? 3D 00 10 00 00")
            || !matches(from, "55 8B EC 8B 55 08 51 8B C4 89 10 8B 01 8B 50 08")
        {
            return None;
        }
        Some(Self {
            engine: engine.interface(),
            player_index: unsafe { std::mem::transmute::<usize, LocalPlayer>(local) },
            entities: list,
            entity_at: unsafe { std::mem::transmute::<usize, Entity>(at) },
            entity_for: unsafe { std::mem::transmute::<usize, Entity>(from) },
        })
    }

    pub(crate) fn player(self) -> Option<usize> {
        let index = unsafe { (self.player_index)(ptr::with_exposed_provenance_mut(self.engine)) };
        if index <= 0 {
            return None;
        }
        let ptr =
            unsafe { (self.entity_at)(ptr::with_exposed_provenance_mut(self.entities), index) };
        (!ptr.is_null()).then_some(ptr.addr())
    }

    pub fn playing(self) -> bool {
        let Some(player) = self.player() else {
            return false;
        };
        let mut life = [u8::MAX];
        native::word(player + 0xe4) == Some(2)
            && native::read(player + 0x147, &mut life)
            && life[0] == 0
            && native::word(player + 0x1450) == Some(0)
    }

    pub(crate) fn weapon(self, player: usize) -> Option<usize> {
        let handle = native::word(player + 0x1084)? as i32;
        if handle == -1 {
            return None;
        }
        let ptr =
            unsafe { (self.entity_for)(ptr::with_exposed_provenance_mut(self.entities), handle) };
        (!ptr.is_null()).then_some(ptr.addr())
    }

    pub fn ammo(self) -> Option<WeaponView> {
        let player = self.player()?;
        let weapon = self.weapon(player)?;
        let clip = native::word(weapon + 0x984)? as i32;
        let ammo = native::word(weapon + 0x97c)? as usize;
        let reserve = (ammo < 32)
            .then(|| native::word(player + 0xf24 + ammo * 4))
            .flatten()
            .map(|n| n as i32);
        Some(WeaponView { clip, reserve })
    }

    pub fn reserves(self) -> Option<[i32; 32]> {
        let player = self.player()?;
        let mut data = [0; 128];
        if !native::read(player + 0xf24, &mut data) {
            return None;
        }
        let rounds = data.as_chunks::<4>().0;
        Some(std::array::from_fn(|index| {
            i32::from_le_bytes(rounds[index])
        }))
    }
}

pub struct Melee {
    view: View,
    table: usize,
    globals: usize,
    buttons: Detour,
    choose: Option<Detour>,
    pose: Option<Detour>,
    input: MeleeInput,
    epoch: Instant,
    settings: Features,
    exhausted: bool,
    animation: Animation,
}

#[derive(Clone, Copy, Default)]
pub struct Features {
    pub hold: f32,
    pub shove_hold: f32,
    pub overhead: bool,
    pub throwing: bool,
}

#[derive(Default)]
struct Animation {
    pose: f32,
    block: f32,
    brace: f32,
    blocking: bool,
    released: Option<(f64, bool)>,
    shoved: Option<(f64, bool)>,
    hit: Option<(f64, bool)>,
    last: Option<f64>,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Vec3 {
    x: f32,
    y: f32,
    z: f32,
}

impl Vec3 {
    fn read(addr: usize) -> Option<Self> {
        Some(Self {
            x: f32::from_bits(native::word(addr)?),
            y: f32::from_bits(native::word(addr + 4)?),
            z: f32::from_bits(native::word(addr + 8)?),
        })
    }
}

impl Melee {
    pub fn install(view: View) -> Option<Self> {
        let client = PeImage::loaded(native::module("client.dll")?)?;
        let (input_table, bits, melee_table, viewmodel_table, viewmodel_pose, attack_choice) =
            if client.timestamp == 0x6afc_ccab {
                (0x575764, 0x1315e0, 0x609c0c, 0x5c105c, 0x289000, 0x30dac0)
            } else {
                (0x57575c, 0x131570, 0x609bfc, 0x5c1054, 0x288f40, 0x30da10)
            };
        let bits = client.base + bits;
        let table = client.base + melee_table;
        let clock = client.base + GLOBALS_READ;
        let locator = native::word(table - 4)? as usize;
        let descriptor = native::word(locator + 12)? as usize;
        if native::word(locator + 4) != Some(0)
            || native::string(descriptor + 8, 64).as_deref() != Some(".?AVC_TerrorMeleeWeapon@@")
            || native::word(client.base + input_table + 2 * 4).map(|n| n as usize) != Some(bits)
            || !matches(clock, "A1 ?? ?? ?? ?? F3 0F 10 40 0C 0F 2F 86 60 09 00 00")
            || !matches(bits, "55 8B EC 83 EC 08 56 8B F1 8B 0D")
        {
            return None;
        }
        let globals = native::word(clock + 1)? as usize;
        let buttons = install(bits, get_buttons as Buttons as usize, &BUTTONS_ORIGINAL)?;
        let choose = client.base + attack_choice;
        let choose = matches(choose, "55 8B EC 56 8B 75 08 8B 86 2C 0C 00 00 85 C0 7F 0A")
            .then(|| install(choose, choose_attack as Choose as usize, &CHOOSE_ORIGINAL))
            .flatten();
        let pose = client.base + viewmodel_pose;
        let pose = (native::word(client.base + viewmodel_table + 222 * 4).map(|n| n as usize)
            == Some(pose)
            && matches(
                pose,
                "55 8B EC 83 EC 48 A1 ?? ?? ?? ?? 33 C5 89 45 FC 8B 45 10",
            ))
        .then(|| install(pose, calculate_pose as Pose as usize, &POSE_ORIGINAL))
        .flatten();
        Some(Self {
            view,
            table,
            globals,
            buttons,
            choose,
            pose,
            input: MeleeInput::default(),
            epoch: Instant::now(),
            settings: Features::default(),
            exhausted: false,
            animation: Animation::default(),
        })
    }

    pub fn stop(&mut self) -> io::Result<()> {
        self.buttons.disable()?;
        BUTTONS_ORIGINAL.store(0, Ordering::Release);
        if let Some(detour) = &mut self.choose {
            detour.disable()?;
            CHOOSE_ORIGINAL.store(0, Ordering::Release);
        }
        if let Some(detour) = &mut self.pose {
            detour.disable()?;
            POSE_ORIGINAL.store(0, Ordering::Release);
        }
        Ok(())
    }

    pub fn configure(&mut self, features: Features, exhausted: bool) {
        self.settings = features;
        self.exhausted = exhausted;
    }

    pub fn hit(&mut self, heavy: bool) {
        self.animation.hit = Some((self.seconds(), heavy));
    }

    pub fn has_ring(&self) -> bool {
        self.settings.hold > 0.0
    }

    pub fn charge(&self) -> Option<(f32, f32)> {
        let Swing::Winding { since, .. } = self.input.swing() else {
            return None;
        };
        let held = (self.seconds() - since) as f32;
        self.has_ring()
            .then(|| (held, (held / self.settings.hold).clamp(0.0, 1.0)))
    }

    pub fn heavy_flash(&self) -> Option<f32> {
        let (when, heavy) = self.animation.released?;
        let elapsed = (self.seconds() - when) as f32;
        (heavy && (0.0..0.3).contains(&elapsed)).then_some(elapsed / 0.3)
    }

    fn seconds(&self) -> f64 {
        self.epoch.elapsed().as_secs_f64()
    }

    fn game_time(&self) -> f32 {
        native::word(self.globals)
            .and_then(|globals| native::word(globals as usize + 12))
            .map(f32::from_bits)
            .unwrap_or_default()
    }

    fn command(&mut self, bits: i32, reset: bool) -> i32 {
        if !reset {
            return self.input.peek(bits);
        }
        let now = self.seconds();
        let weapon = if self.settings.hold > 0.0 {
            self.view
                .player()
                .and_then(|player| self.view.weapon(player))
                .filter(|&weapon| native::word(weapon).map(|n| n as usize) == Some(self.table))
        } else {
            None
        };
        let timer = |offset| {
            weapon
                .and_then(|weapon| native::word(weapon + offset))
                .map(f32::from_bits)
                .unwrap_or_default()
        };
        let before = self.input.swing();
        let shove = self.input.shove();
        let thrown = self.input.thrown_at();
        let flags = self.input.command(MeleeFrame {
            buttons: bits,
            now,
            melee: weapon.is_some(),
            can_heavy: !self.exhausted,
            next_attack: timer(0x960),
            next_shove: timer(0x964),
            game_time: self.game_time(),
            hold: self.settings.hold,
            shove_hold: self.settings.shove_hold,
            throwing: self.settings.throwing,
        });
        let after = self.input.swing();
        self.animation.blocking = weapon.is_some()
            && bits & BUTTON_RELOAD != 0
            && after == Swing::Idle
            && self.input.shove() == Shove::Idle;
        if matches!(before, Swing::Winding { .. })
            && let Swing::Releasing(release) = after
        {
            self.animation.released = Some((now, release.heavy));
        }
        if matches!(shove, Shove::Holding { .. })
            && let Shove::Releasing(release) = self.input.shove()
        {
            self.animation.shoved = Some((now, release.heavy));
        }
        if thrown != self.input.thrown_at() {
            self.animation.released = Some((now, true));
        }
        flags
    }

    fn pick_overhead(&self, weapon: usize, info: usize) {
        if !self.settings.overhead
            || self
                .view
                .player()
                .and_then(|player| native::word(player + 0x13a4))
                .is_none_or(|buttons| buttons as i32 & BUTTON_HEAVY == 0)
        {
            return;
        }
        let attacks = native::word(info.wrapping_add(0xc20)).unwrap_or_default() as usize;
        let count = native::word(info.wrapping_add(0xc2c))
            .unwrap_or_default()
            .min(8) as usize;
        if attacks == 0 || count == 0 {
            return;
        }
        let directions = (0..count)
            .map(|index| {
                native::word(attacks + index * 0x30 + 0x0c)
                    .map(|n| n as i32)
                    .unwrap_or(-1)
            })
            .collect::<Vec<_>>();
        if let Some(index) = melee_overhead_swing(&directions) {
            native::store_word(weapon + 0xd08, index as u32);
            native::store_word(weapon + 0xd38, self.game_time().to_bits());
        }
    }

    fn animate(&mut self, pos: Vec3, angles: Vec3) -> (Vec3, Vec3) {
        let mut position = pos;
        let mut angles = angles;
        let now = self.seconds();
        let dt = self
            .animation
            .last
            .replace(now)
            .map_or(0.0, |last| (now - last).clamp(0.0, 0.1) as f32);
        let charge = self.charge().map_or(0.0, |(_, fraction)| fraction);
        let target = (1.0 - (1.0 - charge).powi(2)) * if self.exhausted { 0.45 } else { 1.0 };
        let state = self.input.swing();
        let rate = if target > self.animation.pose {
            14.0
        } else if state == Swing::Cancelled {
            6.0
        } else {
            28.0
        };
        let blocking = self.animation.blocking;
        let brace = if let Shove::Holding { since } = self.input.shove() {
            if self.settings.shove_hold > 0.0 {
                ((now - since) as f32 / self.settings.shove_hold).clamp(0.0, 1.0)
            } else {
                0.0
            }
        } else {
            0.0
        };
        let pose = ease(self.animation.pose, target, rate, dt);
        let block = ease(
            self.animation.block,
            f32::from(blocking),
            if blocking { 16.0 } else { 10.0 },
            dt,
        );
        let brace = ease(
            self.animation.brace,
            brace,
            if brace > self.animation.brace {
                10.0
            } else {
                25.0
            },
            dt,
        );
        self.animation.pose = pose;
        self.animation.block = block;
        self.animation.brace = brace;
        let through = self.animation.released.map_or(0.0, |(start, heavy)| {
            if heavy {
                pulse(now, start, 0.42)
            } else {
                0.35 * pulse(now, start, 0.22)
            }
        });
        let push = self
            .animation
            .shoved
            .filter(|(_, heavy)| *heavy)
            .map_or(0.0, |(start, _)| pulse(now, start, 0.3));
        let impact = self.animation.hit.map_or(0.0, |(start, heavy)| {
            if heavy {
                pulse(now, start, 0.16)
            } else {
                0.5 * pulse(now, start, 0.1)
            }
        });
        let tremble = if charge >= 1.0 && !self.exhausted {
            ((now as f32 * 47.0).sin() + (now as f32 * 31.0).sin()) * 0.5
        } else {
            0.0
        };
        let (sp, cp) = angles.x.to_radians().sin_cos();
        let (sy, cy) = angles.y.to_radians().sin_cos();
        let (sr, cr) = angles.z.to_radians().sin_cos();
        let forward = [cp * cy, cp * sy, -sp];
        let right = [-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp];
        let up = [cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp];
        let f = -2.8 * pose + 3.5 * through + block - 1.5 * brace + 3.0 * push - 1.2 * impact;
        let r = 2.6 * pose - 0.8 * through - 1.8 * block - 0.6 * brace;
        let u =
            1.6 * pose - 1.4 * through + 2.2 * block + 0.5 * brace - 0.6 * push + 0.08 * tremble;
        position.x += forward[0] * f + right[0] * r + up[0] * u;
        position.y += forward[1] * f + right[1] * r + up[1] * u;
        position.z += forward[2] * f + right[2] * r + up[2] * u;
        angles.x += -11.0 * pose + 9.0 * through - 6.0 * block + 4.0 * brace + 5.0 * push
            - 3.0 * impact
            + 0.35 * tremble;
        angles.y += -9.0 * pose + 4.0 * through + 18.0 * block + 0.25 * tremble;
        angles.z += 14.0 * pose - 7.0 * through - 38.0 * block + 6.0 * brace + 2.0 * impact;
        (position, angles)
    }
}

fn pulse(now: f64, start: f64, duration: f32) -> f32 {
    let time = (now - start) as f32;
    if (0.0..duration).contains(&time) {
        (std::f32::consts::PI * time / duration).sin()
    } else {
        0.0
    }
}

fn ease(current: f32, target: f32, rate: f32, dt: f32) -> f32 {
    current + (target - current) * (dt * rate).min(1.0)
}

fn install(address: usize, hook: usize, original: &AtomicUsize) -> Option<Detour> {
    let mut detour = Detour::prepare(address, hook).ok()?;
    original.store(detour.trampoline(), Ordering::Release);
    if detour.enable().is_err() {
        original.store(0, Ordering::Release);
        return None;
    }
    Some(detour)
}

unsafe extern "thiscall" fn get_buttons(state: *mut c_void, reset: i32) -> i32 {
    let original = BUTTONS_ORIGINAL.load(Ordering::Acquire);
    if original == 0 {
        return 0;
    }
    let original = unsafe { std::mem::transmute::<usize, Buttons>(original) };
    let buttons = unsafe { original(state, reset) };
    std::panic::catch_unwind(|| {
        with_client(|client| {
            client
                .melee
                .as_mut()
                .map_or(buttons, |melee| melee.command(buttons, reset != 0))
        })
        .unwrap_or(buttons)
    })
    .unwrap_or(buttons)
}

unsafe extern "thiscall" fn choose_attack(weapon: *mut c_void, info: *mut c_void) -> i32 {
    let addr = CHOOSE_ORIGINAL.load(Ordering::Acquire);
    if addr == 0 {
        return 0;
    }
    let _ = std::panic::catch_unwind(|| {
        with_client(|client| {
            if let Some(melee) = &client.melee {
                melee.pick_overhead(weapon.addr(), info.addr());
            }
        });
    });
    let original = unsafe { std::mem::transmute::<usize, Choose>(addr) };
    unsafe { original(weapon, info) }
}

unsafe extern "thiscall" fn calculate_pose(
    viewmodel: *mut c_void,
    owner: *mut c_void,
    position: *const Vec3,
    angles: *const Vec3,
) {
    let addr = POSE_ORIGINAL.load(Ordering::Acquire);
    if addr == 0 {
        return;
    }
    let original = unsafe { std::mem::transmute::<usize, Pose>(addr) };
    let moved = std::panic::catch_unwind(|| {
        let p = Vec3::read(position.addr())?;
        let a = Vec3::read(angles.addr())?;
        with_client(|client| client.melee.as_mut().map(|melee| melee.animate(p, a))).flatten()
    })
    .ok()
    .flatten();
    if let Some((position, angles)) = moved {
        unsafe { original(viewmodel, owner, &position, &angles) };
    } else {
        unsafe { original(viewmodel, owner, position, angles) };
    }
}
