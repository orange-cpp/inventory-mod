use std::{
    arch::naked_asm,
    collections::HashMap,
    ffi::c_void,
    io,
    sync::atomic::{AtomicBool, AtomicUsize, Ordering},
    time::Instant,
};

use inventory_mod::melee::{BUTTON_ATTACK, BUTTON_RELOAD};

use crate::{
    client::with_client,
    dedicated,
    native::{self, Detour, PeImage},
};

type ReloadFn = unsafe extern "thiscall" fn(*mut c_void) -> bool;
type FireEmpty = unsafe extern "thiscall" fn(*mut c_void);
type Draw = unsafe extern "thiscall" fn(*mut c_void) -> bool;

static CLIENT_GUN: AtomicUsize = AtomicUsize::new(0);
static CLIENT_SHOTGUN: AtomicUsize = AtomicUsize::new(0);
static SERVER_GUN: AtomicUsize = AtomicUsize::new(0);
static SERVER_SHOTGUN: AtomicUsize = AtomicUsize::new(0);
static CLIENT_EMPTY: AtomicUsize = AtomicUsize::new(0);
static SERVER_EMPTY: AtomicUsize = AtomicUsize::new(0);
static PANEL: AtomicUsize = AtomicUsize::new(0);
static HIDE_ITEM_BOXES: AtomicBool = AtomicBool::new(false);
static ITEM_BOXES: AtomicUsize = AtomicUsize::new(0);
static ITEM_BOXES_EMPTY: AtomicUsize = AtomicUsize::new(0);
static ITEM_BOXES_END: AtomicUsize = AtomicUsize::new(0);

pub struct Reload {
    hooks: [Option<Detour>; 2],
    empty: Option<Detour>,
    client: bool,
    held: HashMap<usize, Hold>,
}

struct Hold {
    actor: usize,
    since: Instant,
    empty: bool,
}

impl Reload {
    pub fn client() -> Self {
        let image = native::module("client.dll").and_then(PeImage::loaded);
        Self::install(image.as_ref(), true)
    }

    pub fn server(image: &PeImage) -> Self {
        Self::install(Some(image), false)
    }

    fn install(image: Option<&PeImage>, client: bool) -> Self {
        let hooks = image.map_or([None, None], |image| {
            let (tables, slot) = if client {
                let updated = image.timestamp == 0x6afc_ccab;
                (
                    if updated {
                        [0x609044, 0x5ff70c]
                    } else {
                        [0x609034, 0x5ff6fc]
                    },
                    269,
                )
            } else {
                let updated = image.timestamp == 0x6afc_cca0;
                (
                    if updated {
                        [0x6d559c, 0x6cc9ec]
                    } else {
                        [0x6d5544, 0x6cc994]
                    },
                    281,
                )
            };
            let (funcs, originals, signatures) = if client {
                (
                    [client_gun as ReloadFn, client_shotgun],
                    [&CLIENT_GUN, &CLIENT_SHOTGUN],
                    ["56 57 8B F1 E8", "55 8B EC 51 56 57 8B F1 E8"],
                )
            } else {
                (
                    [server_gun as ReloadFn, server_shotgun],
                    [&SERVER_GUN, &SERVER_SHOTGUN],
                    [
                        "55 8B EC 83 EC 08 53 56 8B F1 E8",
                        "55 8B EC 83 EC 08 56 57 8B F1 E8",
                    ],
                )
            };
            std::array::from_fn(|index| {
                let slot = image.base + tables[index] + slot * 4;
                let target = native::word(slot)? as usize;
                if !image.includes(target, 10)
                    || !native::matches_pattern(target, signatures[index])
                {
                    return None;
                }
                attach(target, funcs[index] as usize, originals[index])
            })
        });
        Self {
            hooks,
            empty: image.and_then(|image| {
                let (offset, pattern, callback, original) = if client {
                    (
                        0x155d0,
                        "56 8B F1 80 BE BE 09 00 00 00",
                        client_empty as FireEmpty,
                        &CLIENT_EMPTY,
                    )
                } else {
                    (
                        0x4aad0,
                        "56 8B F1 80 BE 4E 14 00 00 00",
                        server_empty as FireEmpty,
                        &SERVER_EMPTY,
                    )
                };
                let start = image.base + offset;
                let target = native::find_pattern(start, 0x400, pattern)?;
                image
                    .includes(target, 10)
                    .then(|| attach(target, callback as usize, original))
                    .flatten()
            }),
            client,
            held: HashMap::new(),
        }
    }

    pub fn mark(&mut self, actor: usize, weapon: usize) {
        if self.held.len() >= 64 && !self.held.contains_key(&weapon) {
            self.held
                .retain(|_, hold| hold.empty && hold.since.elapsed().as_secs() < 300);
        }
        self.held.insert(
            weapon,
            Hold {
                actor,
                since: Instant::now(),
                empty: !self.client,
            },
        );
    }

    pub fn reset(&mut self) {
        self.held.clear();
    }

    pub fn stop(&mut self) -> io::Result<()> {
        if let Some(detour) = &mut self.empty {
            detour.disable()?;
        }
        let originals = if self.client {
            [&CLIENT_GUN, &CLIENT_SHOTGUN]
        } else {
            [&SERVER_GUN, &SERVER_SHOTGUN]
        };
        for (hook, original) in self.hooks.iter_mut().zip(originals) {
            if let Some(hook) = hook {
                hook.disable()?;
                original.store(0, Ordering::Release);
            }
        }
        self.reset();
        Ok(())
    }

    pub fn block(&mut self, actor: usize, weapon: usize) -> bool {
        let Some(hold) = self.held.get_mut(&weapon) else {
            return false;
        };
        let offset = if self.client { 0x984 } else { 0x1414 };
        let clip = native::word(weapon + offset).unwrap_or(1) as i32;
        let control = if self.client { 0x13a4 } else { 0x1cb4 };
        let buttons = native::word(actor + control).unwrap_or_default() as i32;
        if actor != hold.actor || buttons & (BUTTON_ATTACK | BUTTON_RELOAD) != 0 {
            if clip == 0 && actor == hold.actor && buttons & BUTTON_ATTACK != 0 {
                let field = weapon + if self.client { 0x9be } else { 0x144e };
                if let Some(value) = native::word(field) {
                    native::store_word(field, value | 1);
                }
            }
            self.held.remove(&weapon);
            return false;
        }
        if clip == 0 {
            hold.empty = true;
            return true;
        }
        if hold.empty || hold.since.elapsed().as_secs_f32() > 2.0 {
            self.held.remove(&weapon);
        }
        false
    }
}

unsafe extern "thiscall" fn client_empty(gun: *mut c_void) {
    let _ = std::panic::catch_unwind(|| {
        with_client(|client| client.fire_empty(gun.addr()));
    });
    let callback = CLIENT_EMPTY.load(Ordering::Acquire);
    if callback != 0 {
        let original = unsafe { std::mem::transmute::<usize, FireEmpty>(callback) };
        unsafe { original(gun) };
    }
}

unsafe extern "thiscall" fn server_empty(gun: *mut c_void) {
    let _ = std::panic::catch_unwind(|| {
        if !with_client(|client| {
            client
                .host
                .as_ref()
                .is_some_and(|host| host.fire_empty(gun.addr()))
        })
        .unwrap_or(false)
        {
            dedicated::with_host(|host| host.fire_empty(gun.addr()));
        }
    });
    let callback = SERVER_EMPTY.load(Ordering::Acquire);
    if callback != 0 {
        let original = unsafe { std::mem::transmute::<usize, FireEmpty>(callback) };
        unsafe { original(gun) };
    }
}

fn attach(target: usize, callback: usize, original: &AtomicUsize) -> Option<Detour> {
    let mut detour = Detour::prepare(target, callback).ok()?;
    original.store(detour.trampoline(), Ordering::Release);
    if detour.enable().is_err() {
        original.store(0, Ordering::Release);
        return None;
    }
    Some(detour)
}

unsafe fn call(weapon: *mut c_void, original: &AtomicUsize, client: bool) -> bool {
    let block = std::panic::catch_unwind(|| {
        if client {
            with_client(|client| client.block_reload(weapon.addr())).unwrap_or(false)
        } else {
            with_client(|client| {
                client
                    .host
                    .as_mut()
                    .is_some_and(|host| host.block_reload(weapon.addr()))
            })
            .unwrap_or(false)
                || dedicated::with_host(|host| host.block_reload(weapon.addr())).unwrap_or(false)
        }
    })
    .unwrap_or(false);
    if block {
        return false;
    }
    let target = original.load(Ordering::Acquire);
    if target == 0 {
        return false;
    }
    let original = unsafe { std::mem::transmute::<usize, ReloadFn>(target) };
    unsafe { original(weapon) }
}

unsafe extern "thiscall" fn client_gun(weapon: *mut c_void) -> bool {
    unsafe { call(weapon, &CLIENT_GUN, true) }
}
unsafe extern "thiscall" fn client_shotgun(weapon: *mut c_void) -> bool {
    unsafe { call(weapon, &CLIENT_SHOTGUN, true) }
}
unsafe extern "thiscall" fn server_gun(weapon: *mut c_void) -> bool {
    unsafe { call(weapon, &SERVER_GUN, false) }
}
unsafe extern "thiscall" fn server_shotgun(weapon: *mut c_void) -> bool {
    unsafe { call(weapon, &SERVER_SHOTGUN, false) }
}

pub struct Panel {
    visibility: Detour,
    item_boxes: [Option<Detour>; 2],
}

impl Panel {
    pub fn install() -> Option<Self> {
        let image = PeImage::loaded(native::module("client.dll")?)?;
        let (table, paint_table) = if image.timestamp == 0x6afc_ccab {
            (0x5d5a94, 0x5d5744)
        } else {
            (0x5d5a84, 0x5d5734)
        };
        let routine = native::word(image.base + table + 4 * 12)? as usize;
        if !image.includes(routine, 7) || !native::matches_pattern(routine, "56 57 6A FF 8B F9 E8")
        {
            return None;
        }
        let visibility = attach(routine, should_draw as Draw as usize, &PANEL)?;
        Some(Self {
            visibility,
            item_boxes: Self::install_item_boxes(image, paint_table),
        })
    }

    fn install_item_boxes(image: PeImage, table: usize) -> [Option<Detour>; 2] {
        let locate = || {
            let paint = native::word(image.base + table + 4 * 128)? as usize;
            if !image.includes(paint, 0xd80) {
                return None;
            }
            let start = native::find_pattern(
                paint,
                0xd80,
                "6A 01 8B CB E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 89 45 AC DB 45",
            )?;
            let end = native::find_pattern(
                start,
                0x500,
                "6A 04 8B CB E8 ?? ?? ?? ?? 83 BD 78 FF FF FF 00 7C",
            )?;
            let empty = native::find_pattern(
                paint,
                0xd80,
                "6A 01 8B CB 89 7D C4 E8 ?? ?? ?? ?? 89 45 AC 85 C0 0F 84",
            )
            .and_then(|check| {
                let branch = native::word(check + 0x13)? as i32;
                let target = (check + 0x17).wrapping_add_signed(branch as isize);
                (target > check
                    && target < start + 0x109
                    && native::matches_pattern(target, "8B 45 9C 50 8B CB E8"))
                .then_some(target)
            });
            Some((start + 9, end + 9, empty))
        };
        let Some((start, end, empty)) = locate() else {
            return [None, None];
        };
        ITEM_BOXES_END.store(end, Ordering::Release);
        let regular = attach(start, item_boxes as *const () as usize, &ITEM_BOXES);
        let empty = empty.and_then(|address| {
            attach(
                address,
                item_boxes_empty as *const () as usize,
                &ITEM_BOXES_EMPTY,
            )
        });
        if regular.is_some() && empty.is_some() {
            crate::client::log("connected the native weapon HUD's item box branches");
        }
        [regular, empty]
    }

    pub fn set_item_boxes(&self, hide: bool) {
        HIDE_ITEM_BOXES.store(hide, Ordering::Release);
    }

    pub fn stop(&mut self) -> io::Result<()> {
        for detour in self.item_boxes.iter_mut().flatten() {
            detour.disable()?;
        }
        HIDE_ITEM_BOXES.store(false, Ordering::Release);
        self.visibility.disable()?;
        PANEL.store(0, Ordering::Release);
        Ok(())
    }
}

// These branches run within CHudWeaponSelection::Paint. The jump targets share
// the same live registers and frame layout. Both targets begin at statement boundaries.
#[unsafe(naked)]
unsafe extern "C" fn item_boxes() {
    naked_asm!(
        "cmp byte ptr [{hide}], 0",
        "je 2f",
        "jmp dword ptr [{end}]",
        "2: jmp dword ptr [{original}]",
        hide = sym HIDE_ITEM_BOXES,
        end = sym ITEM_BOXES_END,
        original = sym ITEM_BOXES,
    );
}

#[unsafe(naked)]
unsafe extern "C" fn item_boxes_empty() {
    naked_asm!(
        "cmp byte ptr [{hide}], 0",
        "je 2f",
        "jmp dword ptr [{end}]",
        "2: jmp dword ptr [{original}]",
        hide = sym HIDE_ITEM_BOXES,
        end = sym ITEM_BOXES_END,
        original = sym ITEM_BOXES_EMPTY,
    );
}

unsafe extern "thiscall" fn should_draw(panel: *mut c_void) -> bool {
    let hidden = std::panic::catch_unwind(|| {
        with_client(|client| {
            client.snapshot.is_some()
                && client.playing()
                && client.settings.hud.replace_native_panel
        })
        .unwrap_or(false)
    })
    .unwrap_or(false);
    if hidden {
        return false;
    }
    let original = PANEL.load(Ordering::Acquire);
    if original == 0 {
        return false;
    }
    let original = unsafe { std::mem::transmute::<usize, Draw>(original) };
    unsafe { original(panel) }
}
