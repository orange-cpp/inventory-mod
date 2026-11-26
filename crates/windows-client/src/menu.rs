use std::{
    cell::Cell,
    ffi::{CString, c_char, c_void},
    io, ptr,
    sync::atomic::{AtomicUsize, Ordering},
};

use inventory_mod::settings::{Settings, View};
use windows_sys::Win32::System::Memory::{PAGE_READWRITE, VirtualProtect};

use crate::{
    client::with_client,
    native::{self, PeImage, VtablePatch},
};

type MenuCommand = unsafe extern "thiscall" fn(*mut c_void, *const c_char);
type Singleton = unsafe extern "C" fn() -> *mut c_void;
type Open = unsafe extern "thiscall" fn(
    *mut c_void,
    *const i32,
    *mut c_void,
    bool,
    *mut c_void,
) -> *mut c_void;
type Find = unsafe extern "thiscall" fn(*mut c_void, *const c_char, bool) -> *mut c_void;
type SetChoice = unsafe extern "thiscall" fn(*mut c_void, *const c_char);
type Flyout = unsafe extern "thiscall" fn(*mut c_void) -> *mut c_void;
type Listener = unsafe extern "thiscall" fn(*mut c_void, *mut c_void);
type Title =
    unsafe extern "thiscall" fn(*mut c_void, *const c_char, i32, *const c_char, i32, i32, i32, i32);
type Page = unsafe extern "thiscall" fn(*mut c_void);
type User = unsafe extern "thiscall" fn(*mut c_void) -> i32;
type Button = unsafe extern "C" fn(i32, i32) -> i32;
type Back = unsafe extern "thiscall" fn(*mut c_void, i32);

const PAGE_TABLE: usize = 0x16908c;
const MENU_TABLES: [(usize, usize, &str); 2] = [
    (0x17af1c, 0x68220, "55 8B EC 81 EC CC 01 00 00"),
    (0x178154, 0x5ee40, "55 8B EC 81 EC C0 00 00 00"),
];

static GAME_UI: AtomicUsize = AtomicUsize::new(0);
thread_local! {
    static QUIET: Cell<bool> = const { Cell::new(false) };
    static REFRESH_DUE: Cell<bool> = const { Cell::new(false) };
}

pub struct Menu {
    patches: Vec<VtablePatch>,
    blur: Option<(usize, u8)>,
}

struct Setting {
    key: &'static str,
    get: fn(&Settings) -> f32,
    set: fn(&mut Settings, f32),
    choices: &'static [f32],
}

const SETTINGS: &[Setting] = &[
    Setting {
        key: "show_hotbar",
        get: |s| f32::from(s.hud.show),
        set: |s, n| s.hud.show = n != 0.0,
        choices: &[],
    },
    Setting {
        key: "inventory_view",
        get: |s| s.hud.view as u8 as f32,
        set: |s, n| s.hud.view = [View::Both, View::Slots, View::Wheel][(n as usize).min(2)],
        choices: &[],
    },
    Setting {
        key: "auto_store",
        get: |s| f32::from(s.auto_store),
        set: |s, n| s.auto_store = n != 0.0,
        choices: &[],
    },
    Setting {
        key: "replace_native",
        get: |s| f32::from(s.hud.replace_native_panel),
        set: |s, n| s.hud.replace_native_panel = n != 0.0,
        choices: &[],
    },
    Setting {
        key: "hud_scale",
        get: |s| s.hud.scale,
        set: |s, n| s.hud.scale = n,
        choices: &[0.75, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0],
    },
    Setting {
        key: "infection_effect",
        get: |s| f32::from(s.infection.effect),
        set: |s, n| s.infection.effect = n != 0.0,
        choices: &[],
    },
    Setting {
        key: "infection_cover_game_hud",
        get: |s| f32::from(s.infection.cover_game_hud),
        set: |s, n| s.infection.cover_game_hud = n != 0.0,
        choices: &[],
    },
    Setting {
        key: "infection_cover_mod_hud",
        get: |s| f32::from(s.infection.cover_mod_hud),
        set: |s, n| s.infection.cover_mod_hud = n != 0.0,
        choices: &[],
    },
    Setting {
        key: "infection_sound",
        get: |s| f32::from(s.infection.sound),
        set: |s, n| s.infection.sound = n != 0.0,
        choices: &[],
    },
    Setting {
        key: "infection_screamer",
        get: |s| f32::from(s.infection.screamer),
        set: |s, n| s.infection.screamer = n != 0.0,
        choices: &[],
    },
    Setting {
        key: "screamer_chance",
        get: |s| s.infection.screamer_chance,
        set: |s, n| s.infection.screamer_chance = n,
        choices: &[0.0, 0.1, 0.25, 0.5, 0.75, 1.0],
    },
    Setting {
        key: "stamina_bar",
        get: |s| f32::from(s.stamina.bar),
        set: |s, n| s.stamina.bar = n != 0.0,
        choices: &[],
    },
    Setting {
        key: "stamina_vignette",
        get: |s| f32::from(s.stamina.vignette),
        set: |s, n| s.stamina.vignette = n != 0.0,
        choices: &[],
    },
    Setting {
        key: "stamina_cover_game_hud",
        get: |s| f32::from(s.stamina.cover_game_hud),
        set: |s, n| s.stamina.cover_game_hud = n != 0.0,
        choices: &[],
    },
    Setting {
        key: "stamina_cover_mod_hud",
        get: |s| f32::from(s.stamina.cover_mod_hud),
        set: |s, n| s.stamina.cover_mod_hud = n != 0.0,
        choices: &[],
    },
    Setting {
        key: "melee_ring",
        get: |s| f32::from(s.melee_ring),
        set: |s, n| s.melee_ring = n != 0.0,
        choices: &[],
    },
    Setting {
        key: "key_toggle",
        get: |s| s.keys.toggle as f32,
        set: |s, n| s.keys.toggle = n.clamp(0.0, 255.0) as i32,
        choices: &[],
    },
    Setting {
        key: "key_prev",
        get: |s| s.keys.previous as f32,
        set: |s, n| s.keys.previous = n.clamp(0.0, 255.0) as i32,
        choices: &[],
    },
    Setting {
        key: "key_next",
        get: |s| s.keys.next as f32,
        set: |s, n| s.keys.next = n.clamp(0.0, 255.0) as i32,
        choices: &[],
    },
    Setting {
        key: "key_use",
        get: |s| s.keys.take as f32,
        set: |s, n| s.keys.take = n.clamp(0.0, 255.0) as i32,
        choices: &[],
    },
    Setting {
        key: "key_drop",
        get: |s| s.keys.drop as f32,
        set: |s, n| s.keys.drop = n.clamp(0.0, 255.0) as i32,
        choices: &[],
    },
    Setting {
        key: "key_store",
        get: |s| s.keys.store as f32,
        set: |s, n| s.keys.store = n.clamp(0.0, 255.0) as i32,
        choices: &[],
    },
    Setting {
        key: "key_pickup",
        get: |s| s.keys.pickup as f32,
        set: |s, n| s.keys.pickup = n.clamp(0.0, 255.0) as i32,
        choices: &[],
    },
    Setting {
        key: "key_unload",
        get: |s| s.keys.unload as f32,
        set: |s, n| s.keys.unload = n.clamp(0.0, 255.0) as i32,
        choices: &[],
    },
    Setting {
        key: "slots",
        get: |s| s.limits.slots as f32,
        set: |s, n| s.limits.slots = n as usize,
        choices: &[4.0, 6.0, 8.0, 10.0, 12.0, 16.0, 20.0, 24.0],
    },
    Setting {
        key: "weapon_slots",
        get: |s| s.limits.weapon_slots as f32,
        set: |s, n| s.limits.weapon_slots = n as usize,
        choices: &[0.0, 1.0, 2.0, 3.0, 4.0, 6.0, 8.0],
    },
    Setting {
        key: "max_weight",
        get: |s| s.limits.max_weight,
        set: |s, n| s.limits.max_weight = n,
        choices: &[0.0, 10.0, 15.0, 20.0, 25.0, 30.0, 40.0, 50.0],
    },
];

impl Setting {
    fn choice(&self, value: f32) -> f32 {
        self.choices
            .iter()
            .copied()
            .min_by(|a, b| (a - value).abs().total_cmp(&(b - value).abs()))
            .unwrap_or(value)
    }
}

impl Menu {
    pub fn install() -> io::Result<Self> {
        let unsupported =
            || io::Error::other("the game's menu does not match the Inventory Mod settings layout");
        let module = native::module("GameUI.dll").ok_or_else(unsupported)?;
        let image = PeImage::loaded(module).ok_or_else(unsupported)?;
        let base = image.base;
        for (offset, pattern) in [
            (0xac00, "55 8B EC 83 EC 0C 53 8B 5D 08"),
            (0xceb70, "55 8B EC 83 EC 0C 53 56 89 4D"),
            (0x2d180, "55 8B EC 81 EC 08 01 00 00"),
            (0x2cd50, "56 8B F1 83 BE 88"),
            (0x2da00, "55 8B EC 8B 45 08 89 81 8C"),
            (0xf8c0, "55 8B EC 81 EC 44 04 00 00"),
            (0x9a90, "A1"),
            (0x9b30, "8B 81 84 02 00 00"),
            (0xe950, "55 8B EC 8B 45 08"),
            (0xfa2e0, "53 56 8B F1"),
        ] {
            if !image.includes(base + offset, pattern.len() / 3 + 1)
                || !native::matches_pattern(base + offset, pattern)
            {
                return Err(unsupported());
            }
        }
        let original = |table: usize, slot: usize, offset: usize| {
            let at = base + table + 4 * slot;
            (image.includes(at, 4) && native::word(at) == Some((base + offset) as u32))
                .then_some(at)
        };
        for (table, offset, signature) in MENU_TABLES {
            if original(table, 96, offset).is_none()
                || !native::matches_pattern(base + offset, signature)
            {
                return Err(unsupported());
            }
        }
        let page = [
            (96, 0x25430, page_command as MenuCommand as usize),
            (101, 0x25220, page_layout as Page as usize),
            (121, 0x25ee0, base + 0xeb80),
            (127, 0x259e0, page_title as Page as usize),
            (238, 0x25ca0, page_activate as Page as usize),
            (282, 0x25850, base + 0xf580),
        ];
        if page
            .iter()
            .any(|&(slot, old, _)| original(PAGE_TABLE, slot, old).is_none())
        {
            return Err(unsupported());
        }
        GAME_UI.store(base, Ordering::Release);
        let mut patches = Vec::new();
        for (slot, old, new) in page {
            patches.push(VtablePatch::install(
                base + PAGE_TABLE + 4 * slot,
                base + old,
                new,
            )?);
        }
        for ((table, offset, _), replacement) in MENU_TABLES
            .into_iter()
            .zip([main_menu as MenuCommand, in_game_menu])
        {
            patches.push(VtablePatch::install(
                base + table + 4 * 96,
                base + offset,
                replacement as usize,
            )?);
        }
        let blur = menu_blur(&image);
        Ok(Self { patches, blur })
    }

    pub fn stop(&mut self) -> io::Result<()> {
        if let Some((address, previous)) = self.blur {
            write_blur(address, previous)?;
            self.blur = None;
        }
        for patch in self.patches.iter_mut().rev() {
            patch.restore()?;
        }
        GAME_UI.store(0, Ordering::Release);
        REFRESH_DUE.set(false);
        Ok(())
    }
}

fn menu_blur(image: &PeImage) -> Option<(usize, u8)> {
    let switch = image.base + 0xd35d;
    let table = image.base + 0xd4cc;
    let page = table + 3;
    if !image.includes(switch, 15)
        || !image.includes(page, 1)
        || !native::matches_pattern(switch, "83 F8 15 77 ?? 0F B6 88 ?? ?? ?? ?? FF 24 8D")
        || native::word(switch + 8) != Some(table as u32)
    {
        return None;
    }
    let mut current = [0xff];
    if !native::read(page, &mut current) || current[0] != 0 {
        return None;
    }
    write_blur(page, 1).ok()?;
    Some((page, current[0]))
}

fn write_blur(address: usize, value: u8) -> io::Result<()> {
    let entry = ptr::with_exposed_provenance_mut::<u8>(address);
    let mut previous = 0;
    if unsafe { VirtualProtect(entry.cast(), 1, PAGE_READWRITE, &mut previous) } == 0 {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: The verified GameUI switch indexes this byte to choose the backdrop.
    unsafe { entry.write_volatile(value) };
    unsafe { VirtualProtect(entry.cast(), 1, previous, &mut previous) };
    Ok(())
}

macro_rules! ui {
    ($rva:literal, $type:ty) => {{
        let address = GAME_UI.load(Ordering::Acquire) + $rva;
        // SAFETY: GameUI's entry points and all patched slots were checked on installation.
        std::mem::transmute::<usize, $type>(address)
    }};
}

fn show_page(caller: *mut c_void) {
    let window = unsafe { ui!(0x9a90, Singleton)() };
    if !window.is_null() {
        let page = 3; // WT_AUDIOVIDEO
        unsafe { ui!(0xac00, Open)(window, &page, caller, true, ptr::null_mut()) };
    }
}

fn back(page: *mut c_void) {
    let singleton = unsafe { ui!(0x9a90, Singleton)() };
    if !singleton.is_null() {
        let user = unsafe { ui!(0x9b30, User)(singleton) };
        let code = unsafe { ui!(0xe950, Button)(0x73, user) }; // KEY_XBUTTON_B
        unsafe { ui!(0xeb80, Back)(page, code) };
    }
}

fn show_choices(page: *mut c_void) -> bool {
    if QUIET.replace(true) {
        return false;
    }
    struct Reset;
    impl Drop for Reset {
        fn drop(&mut self) {
            QUIET.set(false);
        }
    }
    let _reset = Reset;
    let Some(settings) = with_client(|client| client.settings.clone()) else {
        return false;
    };
    let mut found = false;
    for setting in SETTINGS {
        let control = CString::new(format!("Drp_{}", setting.key)).unwrap();
        let drop_down = unsafe { ui!(0xceb70, Find)(page, control.as_ptr(), false) };
        if drop_down.is_null() {
            continue;
        }
        found = true;
        let value = setting.choice((setting.get)(&settings));
        let command = CString::new(format!("InvMod {} {value}", setting.key)).unwrap();
        unsafe { ui!(0x2d180, SetChoice)(drop_down, command.as_ptr()) };
        let flyout = unsafe { ui!(0x2cd50, Flyout)(drop_down) };
        if !flyout.is_null() {
            let listener = page.cast::<u8>().wrapping_add(0x2bc).cast();
            unsafe { ui!(0x2da00, Listener)(flyout, listener) };
        }
    }
    found
}

unsafe extern "thiscall" fn main_menu(menu: *mut c_void, command: *const c_char) {
    unsafe { menu_command(menu, command, 0) };
}

unsafe extern "thiscall" fn in_game_menu(menu: *mut c_void, command: *const c_char) {
    unsafe { menu_command(menu, command, 1) };
}

unsafe fn menu_command(menu: *mut c_void, command: *const c_char, index: usize) {
    let result = std::panic::catch_unwind(|| {
        if native::string(command.addr(), 128)
            .is_some_and(|name| name.eq_ignore_ascii_case("InventoryModSettings"))
        {
            show_page(menu);
            true
        } else {
            false
        }
    })
    .unwrap_or(false);
    if !result {
        let original = GAME_UI.load(Ordering::Acquire) + MENU_TABLES[index].1;
        let call = unsafe { std::mem::transmute::<usize, MenuCommand>(original) };
        unsafe { call(menu, command) };
    }
}

unsafe extern "thiscall" fn page_title(page: *mut c_void) {
    unsafe {
        ui!(0xf8c0, Title)(
            page,
            c"Inventory Mod".as_ptr(),
            0,
            c"".as_ptr(),
            0,
            0,
            0,
            i32::MAX,
        )
    };
}

unsafe extern "thiscall" fn page_activate(page: *mut c_void) {
    unsafe { ui!(0xfa2e0, Page)(page) };
    let found = std::panic::catch_unwind(|| show_choices(page)).unwrap_or(false);
    REFRESH_DUE.set(!found);
}

unsafe extern "thiscall" fn page_layout(page: *mut c_void) {
    unsafe { ui!(0xfc680, Page)(page) };
    if REFRESH_DUE.get() {
        let found = std::panic::catch_unwind(|| show_choices(page)).unwrap_or(false);
        REFRESH_DUE.set(!found);
    }
}

unsafe extern "thiscall" fn page_command(page: *mut c_void, command: *const c_char) {
    let handled = std::panic::catch_unwind(|| {
        let Some(text) = native::string(command.addr(), 128) else {
            return false;
        };
        if let Some(text) = text.strip_prefix("InvMod ") {
            if !QUIET.get()
                && let Some((key, value)) = text.split_once(' ')
                && let Some(setting) = SETTINGS.iter().find(|setting| setting.key == key)
                && let Ok(value) = value.parse::<f32>()
                && value.is_finite()
            {
                with_client(|client| {
                    (setting.set)(&mut client.settings, setting.choice(value));
                    client.save_settings();
                });
            }
            return true;
        }
        if text.eq_ignore_ascii_case("InventoryModWindow") {
            with_client(|client| client.open = true);
            back(page);
            return true;
        }
        if text.eq_ignore_ascii_case("Back") || text.eq_ignore_ascii_case("Done") {
            back(page);
            return true;
        }
        false
    })
    .unwrap_or(false);
    if !handled {
        unsafe { ui!(0xfb8b0, MenuCommand)(page, command) };
    }
}
