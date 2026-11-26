use std::{
    cell::RefCell,
    ffi::{CString, c_void},
    fs::{File, OpenOptions},
    io::{self, Write},
    path::{Path, PathBuf},
    ptr,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicUsize, Ordering},
    },
    thread,
    time::{Duration, Instant},
};

use inventory_mod::{
    inventory::Inventory,
    items::Catalog,
    protocol::{ActiveMutator, Ping, PingKind, ServerMessage},
    remote_state::RemoteState,
    settings::Settings,
};
use inventory_source_assets::icons::Icons;
use windows_sys::Win32::{
    Foundation::{CloseHandle, HINSTANCE, HWND, LPARAM, LRESULT, POINT, RECT, WPARAM},
    Graphics::Gdi::ClientToScreen,
    System::{
        LibraryLoader::{DisableThreadLibraryCalls, FreeLibraryAndExitThread},
        SystemServices::DLL_PROCESS_ATTACH,
        Threading::{CreateThread, GetCurrentProcessId},
    },
    UI::{
        Input::KeyboardAndMouse::{
            GetCapture, GetKeyState, MAPVK_VK_TO_CHAR, MapVirtualKeyW, ReleaseCapture, SetCapture,
            VK_CONTROL, VK_END, VK_ESCAPE,
        },
        WindowsAndMessaging::{
            CallWindowProcW, ClipCursor, DefWindowProcW, EnumWindows, GWL_WNDPROC, GetClassNameW,
            GetClientRect, GetClipCursor, GetForegroundWindow, GetWindowLongW,
            GetWindowThreadProcessId, KillTimer, SMTO_ABORTIFHUNG, SMTO_BLOCK, SendMessageTimeoutW,
            SetCursorPos, SetTimer, SetWindowLongW, ShowCursor, WM_APP, WM_CHAR, WM_INPUT,
            WM_KEYDOWN, WM_KEYUP, WM_KILLFOCUS, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MBUTTONDOWN,
            WM_MBUTTONUP, WM_MOUSEMOVE, WM_MOUSEWHEEL, WM_NCDESTROY, WM_RBUTTONDOWN, WM_RBUTTONUP,
            WM_SYSKEYDOWN, WM_SYSKEYUP, WM_TIMER, WNDPROC,
        },
    },
};

use crate::{
    addons,
    audio::Audio,
    cursor::{self, Cursor},
    engine::{Engine, Link},
    fog::Fog,
    host::{Host, Session},
    inventory_ui::{Action, InventoryUi},
    melee::{Features, Melee, View, WeaponView},
    menu::Menu,
    native,
    overlay::Visuals,
    postfx::{self, Renderer},
    reload::{Panel, Reload},
    vgui::Vgui,
    wheel::{OrderKey, Wheel},
};

const START: u32 = WM_APP + 0x4d3;
const STOP: u32 = WM_APP + 0x4d4;
const REQUEST_UNLOAD: u32 = WM_APP + 0x4a;
const TIMER: usize = 0x6016_2733;

static WINDOW_PROC: AtomicUsize = AtomicUsize::new(0);
static UNLOAD: AtomicBool = AtomicBool::new(false);
static STARTUP: Mutex<Option<Startup>> = Mutex::new(None);
static LOG: Mutex<Option<File>> = Mutex::new(None);

struct Startup {
    settings: Settings,
    directory: PathBuf,
}

thread_local! {
    static CLIENT: RefCell<Option<Client>> = const { RefCell::new(None) };
}

pub struct Client {
    engine: Engine,
    pub host: Option<Host>,
    offline: Option<Session>,
    pub settings: Settings,
    pub catalog: Catalog,
    pub snapshot: Option<RemoteState>,
    pub selection: usize,
    pub native_selection: Option<usize>,
    pub open: bool,
    pub stamina: Option<u16>,
    pub exhausted: bool,
    pub infection: Option<(Instant, u32)>,
    pub screamer: Option<Surprise>,
    audio: Audio,
    melee_settings: Features,
    pub melee: Option<Melee>,
    view: Option<View>,
    reload: Reload,
    panel: Option<Panel>,
    pub ammo: Option<WeaponView>,
    pub reserve: Option<[i32; 32]>,
    pub mutators: Vec<ActiveMutator>,
    pub mutator_caption: Option<String>,
    fog: Fog,
    mutator_generation: Option<u32>,
    pub pings: Vec<Mark>,
    pub vgui: Option<Vgui>,
    renderer: Option<Renderer>,
    pub visuals: Visuals,
    pub icons: Arc<Icons>,
    icons_loader: Option<thread::JoinHandle<Icons>>,
    pub ui: InventoryUi,
    pub panel_notice: Option<(String, Instant)>,
    menu: Option<Menu>,
    reload_menu: bool,
    directory: PathBuf,
    pub wheel: Option<Wheel>,
    wheel_order: Vec<OrderKey>,
    cursor: Option<CursorCapture>,
    cursor_hooks: Option<Cursor>,
    last_sync: Option<Instant>,
    retries: u8,
    next_host_probe: Option<Instant>,
    hotkeys: [bool; 256],
    swallowed_char: Option<(u8, Instant)>,
    was_in_game: bool,
}

pub struct Mark {
    pub ping: Ping,
    pub born: Instant,
    pub until: Instant,
}

pub struct Surprise {
    pub started: Instant,
    pub path: PathBuf,
}

impl Client {
    pub fn link(&self) -> Link {
        self.engine.link
    }

    fn new(engine: Engine, startup: Startup, game: HWND) -> Self {
        let renderer = Renderer::install(game)
            .map_err(|error| log(&error.to_string()))
            .ok();
        let files = addons::files(&startup.directory);
        let menu = if files.iter().any(|path| addons::is_menu(path)) {
            Menu::install()
                .map_err(|error| log(&error.to_string()))
                .ok()
        } else {
            None
        };
        let mut reload_menu = false;
        for path in files {
            let is_menu = addons::is_menu(&path);
            if (!is_menu || menu.is_some()) && addons::mount(&path) {
                reload_menu |= is_menu;
                log(&format!("mounted {}", path.display()));
            }
        }
        if addons::exists(c"models/inv_mod/rounds_smg.mdl") {
            log("the game can load the inventory ammo packs");
        }
        let view = engine
            .link
            .bound()
            .then(|| View::bind(engine.link))
            .flatten();
        let host = engine
            .link
            .bound()
            .then(|| Host::bind(&startup.directory, &startup.settings, false))
            .flatten();
        let offline = host
            .is_none()
            .then(|| Session::offline(&startup.directory, &startup.settings));
        if host.is_some() {
            log("connected the Rust inventory to the listen server");
        }
        let catalog = host
            .as_ref()
            .map(Host::catalog)
            .or_else(|| offline.as_ref().map(Session::catalog))
            .cloned()
            .unwrap_or_default();
        let vgui = Vgui::install();
        if vgui.is_some() {
            log("connected the Rust HUD to the in-game VGUI surface");
        }
        let melee = view.and_then(Melee::install);
        if melee.is_some() {
            log("connected the Rust melee input to the Source client");
        }
        let icons_loader = std::env::current_exe()
            .ok()
            .and_then(|exe| exe.parent().map(Path::to_path_buf))
            .and_then(|game| {
                thread::Builder::new()
                    .name("l4d2-inventory-icons".into())
                    .spawn(move || Icons::load(&game))
                    .ok()
            });
        Self {
            engine,
            host,
            offline,
            settings: startup.settings,
            catalog,
            snapshot: None,
            selection: 0,
            native_selection: None,
            open: false,
            stamina: None,
            exhausted: false,
            infection: None,
            screamer: None,
            audio: Audio::new(),
            melee_settings: Features::default(),
            melee,
            view,
            reload: Reload::client(),
            panel: Panel::install(),
            ammo: None,
            reserve: None,
            mutators: Vec::new(),
            mutator_caption: None,
            fog: Fog::default(),
            mutator_generation: None,
            pings: Vec::new(),
            vgui,
            renderer,
            visuals: Visuals::default(),
            icons: Arc::default(),
            icons_loader,
            ui: InventoryUi::default(),
            panel_notice: None,
            menu,
            reload_menu,
            directory: startup.directory,
            wheel: None,
            wheel_order: Vec::new(),
            cursor: None,
            cursor_hooks: Cursor::install(),
            last_sync: None,
            retries: 0,
            next_host_probe: None,
            hotkeys: [false; 256],
            swallowed_char: None,
            was_in_game: false,
        }
    }

    fn clear(&mut self) {
        self.snapshot = None;
        self.selection = 0;
        self.native_selection = None;
        self.stamina = None;
        self.exhausted = false;
        self.infection = None;
        self.visuals.clear();
        postfx::publish(postfx::Frame {
            infection: None,
            stamina: None,
        });
        self.screamer = None;
        self.audio.update(false, 0.0, 0.0);
        self.ammo = None;
        self.reload.reset();
        self.reserve = None;
        if self.wheel.take().is_some() || !self.open {
            self.cursor = None;
        }
        self.panel_notice = None;
        self.melee_settings = Features::default();
        self.configure_melee();
        self.mutators.clear();
        self.mutator_caption = None;
        self.fog.clear();
        self.mutator_generation = None;
        self.pings.clear();
        self.last_sync = None;
        self.retries = 0;
    }

    fn timer(&mut self) -> Option<CString> {
        if self
            .icons_loader
            .as_ref()
            .is_some_and(thread::JoinHandle::is_finished)
            && let Some(Ok(icons)) = self.icons_loader.take().map(thread::JoinHandle::join)
        {
            log(&format!(
                "loaded {} original game icons from VPKs",
                icons.len()
            ));
            self.icons = Arc::new(icons);
        }
        if let Some(panel) = &self.panel {
            let hud = &self.settings.hud;
            panel
                .set_item_boxes(!hud.replace_native_panel && (!hud.view.shows_slots() || hud.show));
        }
        let link = self.engine.link;
        if self.host.is_none()
            && link.in_game()
            && self
                .next_host_probe
                .is_none_or(|probe| Instant::now() >= probe)
        {
            self.next_host_probe = Some(Instant::now() + Duration::from_secs(2));
            self.host = Host::bind(&self.directory, &self.settings, false);
            if let Some(host) = &self.host {
                self.catalog = host.catalog().clone();
                self.offline = None;
                log("connected the Rust inventory to the listen server");
            }
        }
        if self.reload_menu && !link.connected() {
            self.reload_menu = false;
            return CString::new("ui_reloadscheme").ok();
        }
        if let Some(host) = &mut self.host {
            host.window_tick();
            for line in host.take_hud() {
                let _ = self.receive(&line);
            }
        }
        let in_game = link.in_game();
        if self.was_in_game != in_game {
            self.clear();
            self.was_in_game = in_game;
        }
        if !in_game {
            if self.snapshot.is_none() {
                self.snapshot = self
                    .host
                    .as_ref()
                    .and_then(Host::local_preview)
                    .or_else(|| self.offline.as_ref().and_then(Session::preview));
            }
            return None;
        }
        let now = Instant::now();
        let severity = self
            .infection
            .filter(|(until, _)| *until > now)
            .map(|(until, total)| {
                (1.0 - until.saturating_duration_since(now).as_secs_f32() * 1000.0 / total as f32)
                    .clamp(0.0, 1.0)
            });
        self.audio.update(
            self.settings.infection.sound
                && (self.ui.infection_preview.get().is_some() || severity.is_some()),
            self.ui.infection_preview.get().or(severity).unwrap_or(0.0),
            self.visuals.beats(),
        );
        if self
            .screamer
            .as_ref()
            .is_some_and(|effect| now - effect.started >= Duration::from_millis(1700))
        {
            self.screamer = None;
        }
        self.fog.tick();
        self.pings.retain(|mark| now < mark.until);
        self.ammo = self.view.and_then(View::ammo);
        self.reserve = self.view.and_then(View::reserves);
        if let Some(mut wheel) = self.wheel.take() {
            let mut order = std::mem::take(&mut self.wheel_order);
            wheel.refresh(self, &mut order);
            self.wheel_order = order;
            self.wheel = Some(wheel);
        }
        if self
            .last_sync
            .is_some_and(|last| now - last < Duration::from_millis(400))
            || (self.snapshot.is_none() && self.retries >= 4)
        {
            return None;
        }
        self.last_sync = Some(now);
        self.retries = self.retries.saturating_add(1);
        CString::new("inv_sync").ok()
    }

    fn select_step(&mut self, step: isize) -> Option<String> {
        let snapshot = self.snapshot.as_ref()?;
        let (slots, weapons) = snapshot.slots.split_at(snapshot.limits.slots);
        if !self.settings.hud.view.shows_slots() {
            return None;
        }
        #[derive(Clone, Copy)]
        enum Entry {
            Native(usize),
            Slot(usize),
        }
        let mut entries = Vec::new();
        let primary = snapshot.native[..2]
            .iter()
            .enumerate()
            .filter(|(_, weapon)| !weapon.classname.is_empty())
            .map(|(index, _)| Entry::Native(index));
        entries.extend(primary);
        if !self.settings.hud.replace_native_panel {
            entries.extend(
                weapons
                    .iter()
                    .enumerate()
                    .filter(|(_, slot)| slot.is_some())
                    .map(|(i, _)| Entry::Slot(snapshot.limits.slots + i)),
            );
            for (index, weapon) in snapshot.native.iter().enumerate().skip(2) {
                if !weapon.classname.is_empty()
                    && !slots.iter().flatten().any(|stack| {
                        stack.held && self.catalog.item(stack.item).classname == weapon.classname
                    })
                {
                    entries.push(Entry::Native(index));
                }
            }
        }
        entries.extend(
            slots
                .iter()
                .enumerate()
                .filter(|(_, slot)| slot.is_some())
                .map(|(index, _)| Entry::Slot(index)),
        );
        let count = entries.len() as isize;
        if count == 0 {
            return None;
        }
        let current = entries.iter().rposition(|entry| {
            let replacing = self.settings.hud.replace_native_panel;
            match *entry {
                Entry::Native(index) if replacing => self.native_selection == Some(index),
                Entry::Native(index) => snapshot.native[index].active,
                Entry::Slot(index) if replacing => {
                    self.native_selection.is_none() && self.selection == index
                }
                Entry::Slot(index) => snapshot.slots[index].as_ref().is_some_and(|slot| {
                    slot.held
                        && snapshot.native.iter().any(|weapon| {
                            weapon.active
                                && weapon.classname == self.catalog.item(slot.item).classname
                        })
                }),
            }
        });
        let next = current.map_or_else(
            || if step > 0 { 0 } else { count - 1 },
            |i| (i as isize + step).rem_euclid(count),
        );
        match entries[next as usize] {
            Entry::Native(index) => {
                self.native_selection = Some(index);
                Some(format!("inv_select {}", snapshot.native[index].classname))
            }
            Entry::Slot(index) => {
                self.native_selection = None;
                self.selection = index;
                Some(format!("inv_use {index}"))
            }
        }
    }

    fn key(&mut self, key: i32) -> (bool, Option<CString>) {
        let keys = self.settings.keys;
        let inventory_key = [
            keys.toggle,
            keys.previous,
            keys.next,
            keys.take,
            keys.drop,
            keys.store,
            keys.pickup,
            keys.unload,
            keys.ping,
        ]
        .contains(&key);
        if key == i32::from(VK_END) && unsafe { GetKeyState(i32::from(VK_CONTROL)) } < 0 {
            UNLOAD.store(true, Ordering::Release);
            return (true, None);
        }
        if !self.open && self.engine.link.bound() && self.vgui.as_ref().is_some_and(Vgui::typing) {
            if inventory_key {
                log(&format!(
                    "hotkey {key} ignored while Source has keyboard focus"
                ));
            }
            return (false, None);
        }
        if self.open && self.ui.rebind(key, &mut self.settings) {
            self.save_settings();
            return (true, None);
        }
        if self.open
            && let Some(changed) = self.ui.number_key(key, &mut self.settings)
        {
            if changed {
                self.save_settings();
            }
            return (true, None);
        }
        if key == keys.toggle && keys.toggle != 0 {
            if self.open {
                self.close_inventory();
            } else {
                self.open = true;
            }
            self.retries = 0;
            return (true, None);
        }
        if key == i32::from(VK_ESCAPE) && (self.open || self.wheel.is_some()) {
            self.close_inventory();
            self.wheel = None;
            return (true, None);
        }
        if !self.playing() {
            if inventory_key && self.engine.link.in_game() {
                log(&format!(
                    "hotkey {key} ignored while the client is outside survivor play"
                ));
            }
            return (false, None);
        }
        let Some(snapshot) = self.snapshot.as_ref() else {
            return (false, None);
        };
        let command = if key == keys.previous {
            self.select_step(-1)
        } else if key == keys.next {
            self.select_step(1)
        } else if key == keys.store {
            Some("inv_store".to_owned())
        } else if key == keys.pickup {
            Some("inv_pickup".to_owned())
        } else if key == keys.ping {
            Some("inv_ping".to_owned())
        } else if key == keys.unload {
            self.unload()
        } else if key == keys.take {
            let native = self
                .native_selection
                .filter(|_| self.settings.hud.replace_native_panel)
                .and_then(|index| snapshot.native.get(index))
                .filter(|weapon| !weapon.classname.is_empty());
            Some(native.map_or_else(
                || format!("inv_use {}", self.selection),
                |weapon| format!("inv_select {}", weapon.classname),
            ))
        } else if key == keys.drop {
            if !self.settings.hud.replace_native_panel {
                snapshot
                    .native
                    .iter()
                    .find(|weapon| weapon.active && !weapon.classname.is_empty())
                    .map(|weapon| format!("inv_dropweapon {}", weapon.classname))
            } else {
                let native = self
                    .native_selection
                    .and_then(|index| snapshot.native.get(index))
                    .filter(|weapon| !weapon.classname.is_empty());
                Some(native.map_or_else(
                    || format!("inv_drop {} 1", self.selection),
                    |weapon| format!("inv_dropweapon {}", weapon.classname),
                ))
            }
        } else if self.settings.hud.view.shows_slots() && (0x30..=0x39).contains(&key) {
            let number = if key == 0x30 { 10 } else { key - 0x30 };
            if number <= 2 {
                if !self.settings.hud.replace_native_panel {
                    return (false, None);
                }
                let command = snapshot
                    .native
                    .get(number as usize - 1)
                    .filter(|weapon| !weapon.classname.is_empty())
                    .map(|weapon| format!("inv_select {}", weapon.classname));
                if command.is_some() {
                    self.native_selection = Some(number as usize - 1);
                }
                command
            } else {
                let index = (number - 3) as usize;
                if index >= snapshot.limits.slots {
                    return (true, None);
                }
                self.native_selection = None;
                self.selection = index;
                snapshot.slots[index]
                    .as_ref()
                    .map(|_| format!("inv_use {index}"))
            }
        } else {
            return (false, None);
        };
        (true, command.and_then(|text| CString::new(text).ok()))
    }

    fn sync_cursor(&mut self, hwnd: HWND) {
        if !self.open && self.wheel.is_none() {
            self.cursor = None;
        } else if self.open
            && self.cursor.is_none()
            && (self.vgui.as_ref().is_some_and(Vgui::active) || postfx::drawable())
            && unsafe { GetForegroundWindow() } == hwnd
        {
            self.cursor = Some(CursorCapture::acquire(hwnd));
        }
    }

    fn key_pressed(&mut self, key: usize, repeat: bool) -> (bool, Option<CString>) {
        if repeat {
            return (
                self.open || self.hotkeys.get(key).copied().unwrap_or(false),
                None,
            );
        }
        let (handled, command) = self.key(key as i32);
        let handled = handled || self.open;
        if handled && let Some(held) = self.hotkeys.get_mut(key) {
            *held = true;
            let typed = unsafe { MapVirtualKeyW(key as u32, MAPVK_VK_TO_CHAR) } & 0x7fff;
            self.swallowed_char = u8::try_from(typed)
                .ok()
                .filter(|&value| value.is_ascii() && value != 0)
                .map(|value| {
                    (
                        value.to_ascii_lowercase(),
                        Instant::now() + Duration::from_millis(250),
                    )
                });
        }
        (handled, command)
    }

    pub fn unload(&mut self) -> Option<String> {
        if let Some(view) = self.view
            && let Some(player) = view.player()
            && let Some(weapon) = view.weapon(player)
        {
            self.reload.mark(player, weapon);
        }
        Some("inv_unload".to_owned())
    }

    pub fn close_inventory(&mut self) {
        self.open = false;
        self.cursor = None;
        self.ui.reset_input();
    }

    pub fn has_settings_page(&self) -> bool {
        self.menu.is_some()
    }

    pub fn playing(&self) -> bool {
        let link = self.link();
        link.in_game()
            && !link.menu_visible()
            && self.view.map_or(self.snapshot.is_some(), View::playing)
    }

    fn edit_inventory(&mut self, operation: impl FnOnce(&mut Inventory)) {
        let state = if let Some(host) = &mut self.host {
            if self.engine.link.in_game() && !host.active() {
                return;
            }
            host.debug_inventory(operation)
        } else if !self.engine.link.in_game() {
            self.offline
                .as_mut()
                .and_then(|session| session.edit(operation))
        } else {
            None
        };
        if let Some(state) = state {
            self.set_inventory(state);
        }
    }

    pub fn inventory_action(&mut self, action: Action) -> Option<CString> {
        match action {
            Action::Close => self.close_inventory(),
            Action::Send(command) => return CString::new(command).ok(),
            Action::Add(id, count) => self.edit_inventory(|inventory| {
                inventory.add(id, count, 0);
            }),
            Action::AddEach => self.edit_inventory(|inventory| {
                let ids: Vec<_> = inventory.catalog().items().map(|(id, _)| id).collect();
                for id in ids {
                    inventory.add(id, 1, 0);
                }
            }),
            Action::Clear => self.edit_inventory(Inventory::clear),

            Action::Screamer => self.start_screamer(true),
            Action::Fog(density) => {
                if let Some(density) = density {
                    self.fog.apply(&[ActiveMutator {
                        mutator: inventory_mod::mutators::Mutator::Fog,
                        fog_density: Some(density),
                        fog_color: None,
                        hunted_class: None,
                    }]);
                } else {
                    self.fog.apply(&self.mutators);
                }
            }
            Action::SettingsChanged => self.save_settings(),
        }
        None
    }

    fn receive(&mut self, text: &str) -> Option<CString> {
        match ServerMessage::decode(text, &self.catalog) {
            Some(ServerMessage::Inventory(inventory)) => {
                if self.snapshot.is_none() {
                    log("synchronized inventory with server");
                }
                self.set_inventory(*inventory);
            }
            Some(ServerMessage::Stamina { tenths, exhausted }) => {
                self.stamina = tenths;
                self.exhausted = exhausted;
                self.configure_melee();
            }
            Some(ServerMessage::Infection {
                remaining_ms,
                total_ms,
                turned,
            }) => {
                self.infection = (remaining_ms > 0 && total_ms > 0).then(|| {
                    (
                        Instant::now() + Duration::from_millis(u64::from(remaining_ms)),
                        total_ms,
                    )
                });
                if turned {
                    self.start_screamer(false);
                }
            }
            Some(ServerMessage::Melee {
                hold_ms,
                shove_hold_ms,
                overhead,
                throwing,
            }) => {
                self.melee_settings = Features {
                    hold: hold_ms.min(10_000) as f32 / 1000.0,
                    shove_hold: shove_hold_ms.min(10_000) as f32 / 1000.0,
                    overhead: hold_ms > 0 && overhead,
                    throwing,
                };
                self.configure_melee();
            }
            Some(ServerMessage::MeleeHit { heavy }) => {
                if let Some(melee) = &mut self.melee {
                    melee.hit(heavy);
                }
            }
            Some(ServerMessage::Mutators {
                active,
                announce,
                generation,
            }) => {
                if self.mutator_generation != Some(generation) {
                    self.mutator_caption = mutator_label(&active);
                    if announce && let Some(label) = &self.mutator_caption {
                        self.panel_notice =
                            Some((label.clone(), Instant::now() + Duration::from_secs(4)));
                    }
                    self.mutator_generation = Some(generation);
                    self.fog.apply(&active);
                }
                self.mutators = active;
            }
            Some(ServerMessage::Ping(ping)) => return self.ping(ping),
            _ => {}
        }
        None
    }

    pub fn set_inventory(&mut self, mut inventory: RemoteState) {
        self.selection = self.selection.min(inventory.slots.len().saturating_sub(1));
        if !inventory.message.is_empty() {
            self.panel_notice = Some((
                std::mem::take(&mut inventory.message),
                Instant::now() + Duration::from_secs(3),
            ));
        }
        self.snapshot = Some(inventory);
    }

    fn ping(&mut self, mut ping: Ping) -> Option<CString> {
        if !(500..=120_000).contains(&ping.lifetime_ms) {
            return None;
        }
        if ping.kind == PingKind::Follow {
            if let Some(target) = self
                .pings
                .iter_mut()
                .rev()
                .find(|mark| mark.ping.owner == ping.owner && mark.ping.kind == PingKind::Enemy)
            {
                target.ping.position = ping.position;
            }
            return None;
        }
        while self
            .pings
            .iter()
            .filter(|mark| mark.ping.owner == ping.owner)
            .count()
            >= ping.per_player as usize
        {
            if let Some(first) = self
                .pings
                .iter()
                .position(|mark| mark.ping.owner == ping.owner)
            {
                self.pings.remove(first);
            }
        }
        if let Some((byte, _)) = ping.name.char_indices().nth(48) {
            ping.name.truncate(byte);
        }
        let speak = if ping.mine
            && !ping.speech.is_empty()
            && ping.speech.len() < 40
            && ping.speech.bytes().all(|byte| byte.is_ascii_alphanumeric())
        {
            let tick = ping.server_tenths.unwrap_or_default();
            CString::new(format!("vocalize {} #{tick}", ping.speech)).ok()
        } else {
            None
        };
        let born = Instant::now();
        let until = born + Duration::from_millis(u64::from(ping.lifetime_ms));
        self.pings.push(Mark { ping, born, until });
        if let Some(vgui) = &self.vgui {
            vgui.tick_sound();
        }
        speak
    }

    fn configure_melee(&mut self) {
        if let Some(melee) = &mut self.melee {
            melee.configure(self.melee_settings, self.exhausted);
        }
    }

    fn start_screamer(&mut self, preview: bool) {
        let effect = &self.settings.infection;
        if !preview
            && (!effect.screamer || effect.screamer_chance <= 0.0 || self.screamer.is_some())
        {
            return;
        }
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|n| n.as_nanos() as u32)
            .unwrap_or_default();
        let roll = stamp.wrapping_mul(2654435761) as f64 / (f64::from(u32::MAX) + 1.0);
        if !preview && roll >= f64::from(effect.screamer_chance) {
            return;
        }
        let path = Path::new(&effect.screamer_image);
        let path = if path.is_absolute() {
            path.to_owned()
        } else {
            self.directory.join(path)
        };
        self.screamer = Some(Surprise {
            started: Instant::now(),
            path,
        });
        if !self.audio.scream(&self.directory, &effect.screamer_sound) {
            if let Some(vgui) = &self.vgui {
                vgui.play_sound(&effect.screamer_sound);
            } else if self.link().bound()
                && effect
                    .screamer_sound
                    .bytes()
                    .all(|byte| byte.is_ascii_alphanumeric() || b"/-._".contains(&byte))
                && let Ok(command) = CString::new(format!("play {}", effect.screamer_sound))
            {
                self.link().send(&command);
            }
        }
    }

    fn stop(&mut self) -> io::Result<()> {
        if let Some(loader) = self.icons_loader.take() {
            let _ = loader.join();
        }
        if let Some(renderer) = &mut self.renderer {
            renderer.stop()?;
        }
        self.reload.stop()?;
        if let Some(panel) = &mut self.panel {
            panel.stop()?;
        }
        self.audio.stop()?;
        self.fog.clear();
        self.wheel = None;
        self.cursor = None;
        if let Some(cursor) = &mut self.cursor_hooks {
            cursor.stop()?;
        }
        if let Some(host) = &mut self.host {
            host.stop()?;
        }
        if let Some(menu) = &mut self.menu {
            menu.stop()?;
        }
        if let Some(vgui) = &mut self.vgui {
            vgui.stop()?;
        }
        if let Some(melee) = &mut self.melee {
            melee.stop()?;
        }
        self.engine.stop()
    }

    pub fn save_settings(&mut self) {
        if let Some(host) = &mut self.host {
            host.set_auto_store(self.settings.auto_store);
        }
        if let Err(error) = self.settings.save(&self.directory) {
            log(&format!("could not save inventory settings ({error})"));
        }
    }

    pub fn block_reload(&mut self, weapon: usize) -> bool {
        let Some(actor) = self.view.and_then(View::player) else {
            return false;
        };
        if self.view.and_then(|view| view.weapon(actor)) != Some(weapon) {
            return false;
        }
        self.reload.block(actor, weapon)
    }

    pub fn fire_empty(&self, weapon: usize) {
        let ammo = self.view.and_then(|view| {
            let player = view.player()?;
            (view.weapon(player)? == weapon)
                .then(|| view.ammo())
                .flatten()
        });
        if ammo.is_some_and(|ammo| ammo.clip == 0 && ammo.reserve.is_some_and(|rounds| rounds > 0))
        {
            native::store_bytes(weapon + 0x9be, &[1]);
        }
    }
}

fn mutator_label(modes: &[ActiveMutator]) -> Option<String> {
    if modes.is_empty() {
        return None;
    }
    let title = if modes.len() == 1 {
        "Mutator"
    } else {
        "Mutators"
    };
    let names = modes
        .iter()
        .map(|mode| mode.mutator.title())
        .collect::<Vec<_>>()
        .join(", ");
    Some(format!("{title}: {names}"))
}

struct CursorCapture {
    window: HWND,
    previous: HWND,
    clipping: Option<RECT>,
    hidden: u8,
    center: POINT,
}

impl CursorCapture {
    fn acquire(window: HWND) -> Self {
        let mut clipping = RECT::default();
        let mut rect = RECT::default();
        let clipping = (unsafe { GetClipCursor(&mut clipping) } != 0).then_some(clipping);
        let mut center = POINT::default();
        let mut hidden = 0;
        let previous = unsafe {
            GetClientRect(window, &mut rect);
            center.x = rect.right / 2;
            center.y = rect.bottom / 2;
            ClientToScreen(window, &mut center);
            ClipCursor(ptr::null());
            let previous = SetCapture(window);
            for _ in 0..16 {
                hidden += 1;
                if ShowCursor(0) < 0 {
                    break;
                }
            }
            SetCursorPos(center.x, center.y);
            cursor::capture(center);
            previous
        };
        Self {
            window,
            previous,
            clipping,
            hidden,
            center,
        }
    }
}

impl Drop for CursorCapture {
    fn drop(&mut self) {
        cursor::release();
        unsafe {
            SetCursorPos(self.center.x, self.center.y);
            for _ in 0..self.hidden {
                ShowCursor(1);
            }
            if GetCapture() == self.window {
                if self.previous.is_null() {
                    ReleaseCapture();
                } else {
                    SetCapture(self.previous);
                }
            }
            if let Some(clip) = &self.clipping {
                ClipCursor(clip);
            }
        }
    }
}

fn wheel_input(hwnd: HWND, message: u32, lparam: LPARAM) -> bool {
    let result = with_client(|client| {
        let mut command = None;
        let vgui = client.vgui.as_ref();
        let size = if let Some(vgui) = vgui.filter(|vgui| vgui.active()) {
            vgui.size()
        } else if postfx::drawable() {
            let mut bounds = RECT::default();
            if unsafe { GetClientRect(hwnd, &mut bounds) } == 0 {
                return (false, None);
            }
            (bounds.right, bounds.bottom)
        } else {
            return (false, None);
        };
        let (x, y) = (i32::from(lparam as i16), i32::from((lparam >> 16) as i16));
        if message == WM_MBUTTONDOWN && client.wheel.is_none() {
            if client.open
                || !client.settings.hud.view.shows_wheel()
                || !client.playing()
                || vgui.is_some_and(Vgui::typing)
            {
                return (false, None);
            }
            let mut order = std::mem::take(&mut client.wheel_order);
            let wheel = Wheel::new(client, &mut order, size.1);
            client.wheel_order = order;
            let Some(wheel) = wheel else {
                return (false, None);
            };
            client.wheel = Some(wheel);
            client.cursor = Some(CursorCapture::acquire(hwnd));
        } else if let Some(wheel) = &mut client.wheel {
            match message {
                WM_MOUSEMOVE => {
                    if wheel.point(x, y, size.0, size.1) && client.settings.hud.wheel_sounds {
                        if let Some(vgui) = vgui {
                            vgui.tick_sound();
                        } else if let Ok(sound) = CString::new("play ui/menu_focus.wav") {
                            command = Some(sound);
                        }
                    }
                }
                WM_RBUTTONDOWN => command = wheel.press_drop(),
                WM_RBUTTONUP => command = wheel.release_drop(),
                WM_MBUTTONUP | WM_LBUTTONDOWN => {
                    wheel.point(x, y, size.0, size.1);
                    if let Some((picked, slot)) = wheel.take() {
                        command = Some(picked);
                        if let Some(slot) = slot {
                            client.selection = slot;
                            client.native_selection = None;
                        }
                    }
                    client.wheel = None;
                    client.cursor = None;
                }
                _ => {}
            }
        } else {
            return (false, None);
        }
        (true, command.map(|command| (command, client.link())))
    });
    if let Some((true, command)) = result {
        if let Some((command, link)) = command {
            send(command, link);
        }
        true
    } else {
        false
    }
}

pub fn receive(text: &str) {
    if let Some((link, Some(command))) = with_client(|client| (client.link(), client.receive(text)))
    {
        send(command, link);
    }
}

pub fn with_client<T>(f: impl FnOnce(&mut Client) -> T) -> Option<T> {
    CLIENT.with(|slot| {
        let mut slot = slot.try_borrow_mut().ok()?;
        slot.as_mut().map(f)
    })
}

pub fn send(command: CString, link: Link) {
    if command.as_c_str() == c"ui_reloadscheme" && !link.connected() {
        link.send(&command);
        return;
    }
    if !link.in_game() {
        if let Ok(text) = command.to_str()
            && let Some(coords) = text.strip_prefix("inv_move ")
            && let Some((from, to)) = coords.split_once(' ')
            && let (Ok(from), Ok(to)) = (from.parse::<usize>(), to.parse::<usize>())
        {
            with_client(|client| {
                client.edit_inventory(|inventory| {
                    inventory.move_slot(from, to);
                })
            });
        }
    } else {
        let local = with_client(|client| {
            let Some(host) = &mut client.host else {
                return false;
            };
            if !host.active() {
                return false;
            }
            let Some(state) = host.command(&command) else {
                return false;
            };
            if client.snapshot.is_none() {
                log("read the Rust inventory from the listen server");
            }
            if !state.message.is_empty() && command.as_c_str() != c"inv_sync" {
                log(&state.message);
            }
            client.set_inventory(state);
            true
        })
        .unwrap_or(false);
        if local {
            return;
        }
        link.send(&command);
    }
}

pub(crate) fn log(message: &str) {
    if let Ok(mut log) = LOG.lock()
        && let Some(file) = log.as_mut()
    {
        let _ = writeln!(file, "{message}");
    }
}

fn open_log(directory: &Path) {
    if let Ok(log) = OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .open(directory.join("inventory_mod.log"))
        && let Ok(mut file) = LOG.lock()
    {
        *file = Some(log);
    }
}

fn initialize(hwnd: HWND) -> LRESULT {
    if CLIENT.with(|state| state.borrow().is_some()) {
        return 1;
    }
    let startup = STARTUP.lock().ok().and_then(|mut state| state.take());
    let Some(startup) = startup else {
        return 0;
    };
    let engine = Engine::bind().unwrap_or_else(|error| {
        log(&format!("{error}; using the local inventory editor"));
        Engine::offline()
    });
    let client = Client::new(engine, startup, hwnd);
    CLIENT.with(|state| state.replace(Some(client)));
    if unsafe { SetTimer(hwnd, TIMER, 16, None) } == 0 {
        log("could not start the inventory timer");
        return 0;
    }
    log("connected the Rust client to the Source game thread");
    1
}

fn stop(hwnd: HWND) -> LRESULT {
    // A later subclass may still call us in its chain. Keep the DLL loaded
    // until our procedure can be removed safely.
    let replacement = window_proc as *const () as i32;
    if unsafe { GetWindowLongW(hwnd, GWL_WNDPROC) } != replacement {
        log("waiting for the game's window procedure chain before unloading");
        return 0;
    }
    if let Some(Err(error)) = with_client(Client::stop) {
        if error.kind() != io::ErrorKind::WouldBlock {
            log(&format!("inventory hooks are still active ({error})"));
        }
        return 0;
    }
    CLIENT.with(|state| drop(state.take()));
    unsafe {
        KillTimer(hwnd, TIMER);
        SetWindowLongW(
            hwnd,
            GWL_WNDPROC,
            WINDOW_PROC.load(Ordering::Acquire) as i32,
        );
    }
    1
}

fn dispatch(hwnd: HWND, message: u32, wparam: WPARAM, lparam: LPARAM) -> Option<LRESULT> {
    match message {
        START => Some(initialize(hwnd)),
        STOP => Some(stop(hwnd)),
        REQUEST_UNLOAD => {
            UNLOAD.store(true, Ordering::Release);
            Some(0)
        }
        WM_TIMER if wparam == TIMER => {
            let timer = with_client(|client| {
                let sync = client.timer();
                let drop = client.wheel.as_mut().and_then(Wheel::tick);
                client.sync_cursor(hwnd);
                crate::overlay::update(client, hwnd);
                (client.engine.link, [drop, sync])
            });
            if let Some((link, commands)) = timer {
                for command in commands.into_iter().flatten() {
                    send(command, link);
                }
            }
            Some(0)
        }
        WM_KEYDOWN | WM_SYSKEYDOWN => {
            if let Some((true, link, command)) = with_client(|client| {
                let (used, command) = client.key_pressed(wparam, lparam & (1 << 30) != 0);
                client.sync_cursor(hwnd);
                (used, client.engine.link, command)
            }) {
                if let Some(command) = command {
                    send(command, link);
                }
                return Some(0);
            }
            None
        }
        WM_KEYUP | WM_SYSKEYUP
            if with_client(|client| {
                client.hotkeys.get_mut(wparam).is_some_and(std::mem::take) || client.open
            })
            .unwrap_or(false) =>
        {
            Some(0)
        }
        WM_CHAR
            if with_client(|client| {
                if client.open {
                    client.ui.number_character(wparam);
                    return true;
                }
                if let Some((expected, until)) = client.swallowed_char
                    && Instant::now() < until
                    && u8::try_from(wparam)
                        .ok()
                        .is_some_and(|actual| actual.to_ascii_lowercase() == expected)
                {
                    client.swallowed_char = None;
                    true
                } else {
                    false
                }
            })
            .unwrap_or(false) =>
        {
            Some(0)
        }
        WM_INPUT if with_client(|client| client.open).unwrap_or(false) => {
            Some(unsafe { DefWindowProcW(hwnd, message, wparam, lparam) })
        }
        WM_MBUTTONDOWN | WM_MBUTTONUP | WM_MOUSEMOVE | WM_RBUTTONDOWN | WM_LBUTTONDOWN
        | WM_LBUTTONUP | WM_RBUTTONUP
            if wheel_input(hwnd, message, lparam) =>
        {
            Some(0)
        }
        WM_MOUSEMOVE | WM_LBUTTONDOWN | WM_LBUTTONUP | WM_RBUTTONDOWN | WM_RBUTTONUP
            if crate::overlay::mouse(message, lparam, hwnd) =>
        {
            Some(0)
        }
        WM_MBUTTONDOWN | WM_MBUTTONUP if with_client(|client| client.open).unwrap_or(false) => {
            Some(0)
        }
        WM_MOUSEWHEEL => {
            let selected = with_client(|client| {
                let delta = (wparam >> 16) as i16;
                if client.wheel.is_some() {
                    return (true, None);
                }
                if client.open {
                    client.ui.scroll_by(-i32::from(delta.signum()) * 58);
                    return (true, None);
                }
                let hud = &client.settings.hud;
                if !hud.view.shows_slots()
                    || !(hud.show || hud.replace_native_panel)
                    || !client.playing()
                    || client.vgui.as_ref().is_some_and(Vgui::typing)
                {
                    return (false, None);
                }
                let command = client
                    .select_step(if delta > 0 { -1 } else { 1 })
                    .and_then(|command| CString::new(command).ok())
                    .map(|command| (command, client.link()));
                (true, command)
            });
            if let Some((true, command)) = selected {
                if let Some((command, link)) = command {
                    send(command, link);
                }
                Some(0)
            } else {
                None
            }
        }
        WM_KILLFOCUS => {
            with_client(|client| {
                client.wheel = None;
                client.cursor = None;
                client.ui.reset_input();
                client.hotkeys.fill(false);
                client.swallowed_char = None;
            });
            None
        }
        WM_NCDESTROY => {
            UNLOAD.store(true, Ordering::Release);
            None
        }
        _ => None,
    }
}

unsafe extern "system" fn window_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    if let Some(result) = std::panic::catch_unwind(|| dispatch(hwnd, msg, wparam, lparam))
        .ok()
        .flatten()
    {
        return result;
    }
    let previous = WINDOW_PROC.load(Ordering::Acquire);
    if previous == 0 {
        return unsafe { DefWindowProcW(hwnd, msg, wparam, lparam) };
    }
    let previous = unsafe { std::mem::transmute::<usize, WNDPROC>(previous) };
    unsafe { CallWindowProcW(previous, hwnd, msg, wparam, lparam) }
}

unsafe extern "system" fn find_window(hwnd: HWND, output: LPARAM) -> i32 {
    let mut pid = 0;
    unsafe { GetWindowThreadProcessId(hwnd, &mut pid) };
    if pid != unsafe { GetCurrentProcessId() } {
        return 1;
    }
    let mut class = [0; 64];
    let length = unsafe { GetClassNameW(hwnd, class.as_mut_ptr(), class.len() as i32) };
    if length > 0
        && (&class[..length as usize] == native::wide("Valve001").strip_suffix(&[0]).unwrap())
    {
        unsafe { (output as *mut HWND).write(hwnd) };
        0
    } else {
        1
    }
}

fn wait_for_game() -> Option<HWND> {
    let until = Instant::now() + Duration::from_secs(60);
    while Instant::now() < until {
        let mut hwnd: HWND = ptr::null_mut();
        if native::module("engine.dll").is_some() && native::module("client.dll").is_some() {
            unsafe { EnumWindows(Some(find_window), (&mut hwnd as *mut HWND).addr() as LPARAM) };
        }
        if !hwnd.is_null() {
            return Some(hwnd);
        }
        thread::sleep(Duration::from_millis(100));
    }
    None
}

fn event(hwnd: HWND, message: u32) -> Option<usize> {
    let mut result = 0;
    // A synchronous return guarantees that the game thread has left the DLL.
    unsafe {
        (SendMessageTimeoutW(
            hwnd,
            message,
            0,
            0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK,
            3000,
            &mut result,
        ) != 0)
            .then_some(result)
    }
}

fn run(module: HINSTANCE) -> io::Result<()> {
    let executable = std::env::current_exe()?;
    let supported = executable.file_name().is_some_and(|name| {
        name.to_string_lossy()
            .eq_ignore_ascii_case("left4dead2.exe")
    });
    let insecure =
        std::env::args_os().any(|arg| arg.to_string_lossy().eq_ignore_ascii_case("-insecure"));
    if !supported || !insecure {
        return Err(io::Error::other(
            "the Rust client needs Left 4 Dead 2 started with -insecure",
        ));
    }
    let module_path = native::module_path(module).ok_or_else(io::Error::last_os_error)?;
    let directory = module_path
        .parent()
        .map(PathBuf::from)
        .ok_or_else(io::Error::last_os_error)?;
    open_log(&directory);
    let settings = Settings::load(&directory).unwrap_or_else(|error| {
        log(&format!("using default settings ({error})"));
        Settings::default()
    });
    let hwnd =
        wait_for_game().ok_or_else(|| io::Error::other("Left 4 Dead 2 window is unavailable"))?;
    let previous = unsafe { GetWindowLongW(hwnd, GWL_WNDPROC) };
    if previous == 0 {
        return Err(io::Error::last_os_error());
    }
    WINDOW_PROC.store(previous as usize, Ordering::Release);
    let startup = Startup {
        settings,
        directory,
    };
    *STARTUP
        .lock()
        .map_err(|_| io::Error::other("startup state is unavailable"))? = Some(startup);
    if unsafe { SetWindowLongW(hwnd, GWL_WNDPROC, window_proc as *const () as i32) } == 0 {
        return Err(io::Error::last_os_error());
    }
    let deadline = Instant::now() + Duration::from_secs(30);
    let started = loop {
        match event(hwnd, START) {
            Some(1) => break true,
            Some(_) => break false,
            None if Instant::now() >= deadline || UNLOAD.load(Ordering::Acquire) => break false,
            None => thread::sleep(Duration::from_millis(150)),
        }
    };
    if started {
        log("running; Ctrl+End unloads the Rust client");
        while !UNLOAD.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(100));
        }
    }
    while event(hwnd, STOP) != Some(1) {
        thread::sleep(Duration::from_millis(300));
    }
    log("unloaded the Rust client");
    Ok(())
}

unsafe extern "system" fn worker(parameter: *mut c_void) -> u32 {
    let outcome = std::panic::catch_unwind(|| run(parameter));
    if let Ok(Err(error)) = &outcome {
        log(&error.to_string());
    }
    let exit_code = u32::from(!matches!(outcome, Ok(Ok(()))));
    if let Ok(mut log) = LOG.lock() {
        *log = None;
    }
    if let Ok(mut startup) = STARTUP.lock() {
        *startup = None;
    }
    unsafe { FreeLibraryAndExitThread(parameter, exit_code) }
}

#[unsafe(no_mangle)]
pub unsafe extern "system" fn DllMain(module: HINSTANCE, reason: u32, _: *mut c_void) -> i32 {
    if reason == DLL_PROCESS_ATTACH {
        if crate::dedicated::on_attach(module) {
            return 1;
        }
        unsafe {
            DisableThreadLibraryCalls(module);
            let handle = CreateThread(ptr::null(), 0, Some(worker), module, 0, ptr::null_mut());
            if handle.is_null() {
                return 0;
            }
            CloseHandle(handle);
        }
    }
    1
}
