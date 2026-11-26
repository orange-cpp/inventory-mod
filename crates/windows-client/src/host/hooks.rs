use std::{
    cell::RefCell,
    ffi::{CStr, c_char, c_void},
    io, ptr,
    sync::atomic::{AtomicUsize, Ordering},
};

use windows_sys::Win32::System::LibraryLoader::GetProcAddress;

use crate::{
    client::with_client,
    native::{self, Detour, PeImage},
};

type Factory = unsafe extern "C" fn(*const c_char, *mut i32) -> *mut c_void;
type Fire = unsafe extern "thiscall" fn(*mut c_void, *mut c_void, bool) -> bool;
type Name = unsafe extern "thiscall" fn(*mut c_void) -> *const c_char;
type EventInt = unsafe extern "thiscall" fn(*mut c_void, *const c_char, i32) -> i32;
type EventBool = unsafe extern "thiscall" fn(*mut c_void, *const c_char, bool) -> bool;
type Command = unsafe extern "C" fn(*mut c_void, *const c_void);
type UserId = unsafe extern "thiscall" fn(*mut c_void, *mut c_void) -> i32;
type SteamId = unsafe extern "thiscall" fn(*mut c_void, *mut c_void) -> *const c_char;
type Print = unsafe extern "thiscall" fn(*mut c_void, *mut c_void, *const c_char);
type Globals = unsafe extern "thiscall" fn(*mut c_void) -> *mut c_void;
type Precache = unsafe extern "thiscall" fn(*mut c_void, *const c_char, bool) -> i32;
type EdictToEntity = unsafe extern "thiscall" fn(*mut c_void, *mut c_void) -> *mut c_void;
type ChangeAccessor = unsafe extern "thiscall" fn(*mut c_void, *mut c_void) -> *mut c_void;
type Frame = unsafe extern "thiscall" fn(*mut c_void, bool);

static FIRE_ORIGINAL: AtomicUsize = AtomicUsize::new(0);
static COMMAND_ORIGINAL: AtomicUsize = AtomicUsize::new(0);
static FRAME_ORIGINAL: AtomicUsize = AtomicUsize::new(0);

thread_local! {
    static EVENTS: RefCell<Option<Vec<Event>>> = const { RefCell::new(None) };
}

#[derive(Clone, Copy)]
pub enum Event {
    Transition,
    PlayerTransitioned(i32),
    PlayerActivated(i32),
    RoundStart,
    Death(i32),
    RoundEnd,
    LeftStart,
    Exposure(i32),
    Down {
        user: i32,
        attacker: i32,
        entity: i32,
    },
    Pills(i32),
}

pub fn take_events() -> Vec<Event> {
    EVENTS.with_borrow_mut(|queue| queue.as_mut().map_or_else(Vec::new, std::mem::take))
}

pub struct Hooks {
    engine: usize,
    user_id: UserId,
    steam_id: SteamId,
    client_print: Print,
    globals: Option<usize>,
    fire: Option<Detour>,
    commands: Option<Detour>,
    frame: Option<Detour>,
    entities: Option<(usize, EdictToEntity)>,
}

fn ptr(addr: usize) -> *mut c_void {
    ptr::with_exposed_provenance_mut(addr)
}

pub fn factory(module: &str) -> Option<Factory> {
    let module = native::module(module)?;
    let function = unsafe { GetProcAddress(module.as_ptr(), c"CreateInterface".as_ptr().cast()) }?;
    Some(unsafe { std::mem::transmute::<unsafe extern "system" fn() -> isize, Factory>(function) })
}

fn create(factory: Factory, name: &CStr) -> Option<usize> {
    let result = unsafe { factory(name.as_ptr(), ptr::null_mut()) };
    (!result.is_null()).then_some(result.addr())
}

fn bind_globals() -> Option<usize> {
    let interface = create(factory("server.dll")?, c"PlayerInfoManager002")?;
    let function = native::vslot(interface, 1)?;
    let globals = unsafe { std::mem::transmute::<usize, Globals>(function)(ptr(interface)) }.addr();
    (globals != 0).then_some(globals)
}

impl Hooks {
    pub fn install(game: &PeImage, dedicated: bool) -> Option<Self> {
        let factory = factory("engine.dll")?;
        let engine = create(factory, c"VEngineServer022")?;
        let user_id = native::vslot(engine, 16)?;
        let network_id = native::vslot(engine, 17)?;
        let printf = native::vslot(engine, 46)?;
        let engine_image = PeImage::loaded(native::module("engine.dll")?)?;
        if ![user_id, network_id, printf]
            .iter()
            .all(|&method| engine_image.includes(method, 1))
        {
            return None;
        }
        let user_id = unsafe { std::mem::transmute::<usize, UserId>(user_id) };
        let steam_id = unsafe { std::mem::transmute::<usize, SteamId>(network_id) };
        let client_print = unsafe { std::mem::transmute::<usize, Print>(printf) };
        let fire = create(factory, c"GAMEEVENTSMANAGER002")
            .and_then(|events| native::vslot(events, 7))
            .filter(|&target| {
                native::matches_pattern(target, "55 8B EC A1 ?? ?? ?? ?? 83 78 30 00")
            })
            .and_then(|target| attach(target, fire_event as Fire as usize, &FIRE_ORIGINAL));

        let rva = if game.timestamp == 0x6afc_cca0 {
            0x90d10
        } else {
            0x90c80
        };
        let command = game.base + rva;
        let commands = (!dedicated
            && game.includes(command, 10)
            && native::matches_pattern(command, "55 8B EC 81 EC 18 01 00 00 A1"))
        .then(|| {
            attach(
                command,
                client_command as Command as usize,
                &COMMAND_ORIGINAL,
            )
        })
        .flatten();
        let frame = if dedicated {
            None
        } else {
            create(self::factory("server.dll")?, c"ServerGameDLL005")
                .and_then(|dll| native::vslot(dll, 4))
                .filter(|&function| {
                    game.includes(function, 6)
                        && native::matches_pattern(function, "55 8B EC 83 EC 14")
                })
                .and_then(|function| {
                    attach(function, game_frame as Frame as usize, &FRAME_ORIGINAL)
                })
        };
        let entities =
            create(self::factory("server.dll")?, c"ServerGameEnts001").and_then(|entities| {
                let method = native::vslot(entities, 4)?;
                game.includes(method, 1).then(|| {
                    (entities, unsafe {
                        std::mem::transmute::<usize, EdictToEntity>(method)
                    })
                })
            });
        if dedicated && entities.is_none() {
            return None;
        }
        if fire.is_some() {
            EVENTS.with_borrow_mut(|queue| *queue = Some(Vec::new()));
        }
        Some(Self {
            engine,
            user_id,
            steam_id,
            client_print,
            globals: bind_globals(),
            fire,
            commands,
            frame,
            entities,
        })
    }

    pub fn player(&self, edict: usize) -> Option<usize> {
        let (entities, resolve) = self.entities?;
        if edict == 0 {
            return None;
        }
        let player = unsafe { resolve(ptr(entities), ptr(edict)) }.addr();
        (player != 0).then_some(player)
    }

    pub fn stop(&mut self) -> io::Result<()> {
        // A later hook can keep calling our entry after the host goes away.
        // Stop collecting events even when we cannot restore the game code.
        EVENTS.with_borrow_mut(|queue| *queue = None);
        let stop = |detour: &mut Option<Detour>, original: &AtomicUsize| {
            if let Some(detour) = detour {
                detour.disable()?;
                original.store(0, Ordering::Release);
            }
            Ok(())
        };
        stop(&mut self.frame, &FRAME_ORIGINAL)
            .and(stop(&mut self.commands, &COMMAND_ORIGINAL))
            .and(stop(&mut self.fire, &FIRE_ORIGINAL))
    }

    pub fn user(&self, edict: usize) -> Option<i32> {
        if edict == 0 {
            return None;
        }
        let id = unsafe { (self.user_id)(ptr(self.engine), ptr(edict)) };
        (id > 0).then_some(id)
    }

    pub fn steam_id(&self, edict: usize) -> Option<String> {
        if edict == 0 {
            return None;
        }
        let value = unsafe { (self.steam_id)(ptr(self.engine), ptr(edict)) };
        native::string(value.addr(), 64)
    }

    pub fn precache(&self, path: &CStr) -> bool {
        let Some(method) = native::vslot(self.engine, 5) else {
            return false;
        };
        let precache = unsafe { std::mem::transmute::<usize, Precache>(method) };
        unsafe { precache(ptr(self.engine), path.as_ptr(), false) > 0 }
    }

    pub fn print(&self, edict: usize, text: &CStr) {
        unsafe { (self.client_print)(ptr(self.engine), ptr(edict), text.as_ptr()) };
    }

    pub fn map(&self) -> Option<String> {
        let name = native::word(self.globals? + 0x3c)?;
        native::string(name as usize, 96)
    }

    pub fn time(&self) -> Option<f32> {
        native::word(self.globals? + 12)
            .map(f32::from_bits)
            .filter(|n| n.is_finite())
    }

    pub fn panic_event(&self) {
        if let Some(module) = native::module("server.dll").and_then(PeImage::loaded) {
            super::infection::panic_event(&module);
        }
    }

    pub fn changed(&self, entity: usize) {
        let Some(edict) = native::word(entity + 0x28)
            .map(|addr| addr as usize)
            .filter(|&addr| addr != 0)
        else {
            return;
        };
        if let Some(flags) = native::word(edict) {
            native::store_word(edict, flags | 0x101);
        }
        if let Some(slot) = native::vslot(self.engine, 97) {
            let accessor = unsafe {
                std::mem::transmute::<usize, ChangeAccessor>(slot)(ptr(self.engine), ptr(edict))
            }
            .addr();
            if accessor != 0
                && let Some(state) = native::word(accessor)
            {
                native::store_word(accessor, state & 0xffff);
            }
        }
    }
}

unsafe extern "thiscall" fn game_frame(dll: *mut c_void, simulating: bool) {
    let address = FRAME_ORIGINAL.load(Ordering::Acquire);
    if address != 0 {
        let original = unsafe { std::mem::transmute::<usize, Frame>(address) };
        unsafe { original(dll, simulating) };
    }
    if simulating {
        let _ = std::panic::catch_unwind(|| {
            with_client(|client| {
                if let Some(host) = &mut client.host {
                    host.native_tick();
                }
            });
        });
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

fn event(source: usize) -> Option<Event> {
    let method = native::vslot(source, 1)?;
    let get_name = unsafe { std::mem::transmute::<usize, Name>(method) };
    let name = unsafe { get_name(ptr(source)) };
    let get_int = |key: &CStr| {
        let slot = native::vslot(source, 6)?;
        let number = unsafe { std::mem::transmute::<usize, EventInt>(slot) };
        Some(unsafe { number(ptr(source), key.as_ptr(), 0) })
    };
    let get_bool = |key: &CStr| {
        let slot = native::vslot(source, 5)?;
        let boolean = unsafe { std::mem::transmute::<usize, EventBool>(slot) };
        Some(unsafe { boolean(ptr(source), key.as_ptr(), false) })
    };
    match native::string(name.addr(), 48)?.as_str() {
        "round_start" => Some(Event::RoundStart),
        "map_transition" => Some(Event::Transition),
        "player_transitioned" => Some(Event::PlayerTransitioned(get_int(c"userid")?)),
        "player_activate" => Some(Event::PlayerActivated(get_int(c"userid")?)),
        "player_death" => Some(Event::Death(get_int(c"userid")?)),
        "round_end" | "mission_lost" | "finale_vehicle_leaving" | "finale_win" => {
            Some(Event::RoundEnd)
        }
        "player_left_start_area" => Some(Event::LeftStart),
        "tongue_grab"
        | "lunge_pounce"
        | "jockey_ride"
        | "charger_carry_start"
        | "charger_pummel_start" => Some(Event::Exposure(get_int(c"victim")?)),
        "player_now_it" if get_bool(c"by_boomer")? || get_bool(c"exploded")? => {
            Some(Event::Exposure(get_int(c"userid")?))
        }
        "player_incapacitated" => Some(Event::Down {
            user: get_int(c"userid")?,
            attacker: get_int(c"attacker")?,
            entity: get_int(c"attackerentid")?,
        }),
        "pills_used" => {
            let subject = get_int(c"subject")?;
            Some(Event::Pills(if subject > 0 {
                subject
            } else {
                get_int(c"userid")?
            }))
        }
        _ => None,
    }
}

unsafe extern "thiscall" fn fire_event(
    manager: *mut c_void,
    source: *mut c_void,
    dont_broadcast: bool,
) -> bool {
    if !source.is_null() {
        let _ = std::panic::catch_unwind(|| {
            EVENTS.with_borrow_mut(|queue| {
                if let Some(events) = queue
                    && let Some(event) = event(source.addr())
                {
                    events.push(event);
                }
            });
        });
    }
    let addr = FIRE_ORIGINAL.load(Ordering::Acquire);
    if addr == 0 {
        return false;
    }
    let original = unsafe { std::mem::transmute::<usize, Fire>(addr) };
    unsafe { original(manager, source, dont_broadcast) }
}

pub(super) fn read_command(arguments: usize) -> Option<String> {
    let count = native::word(arguments)?.min(4) as usize;
    let mut values = Vec::new();
    for index in 0..count {
        let word = native::word(arguments + 1032 + 4 * index)?;
        let value = native::string(word as usize, 128)?;
        if value.is_empty() || value.chars().any(char::is_whitespace) {
            return None;
        }
        if index == 0 && !inventory_mod::protocol::is_inventory_command(&value) {
            return None;
        }
        values.push(value);
    }
    (!values.is_empty()).then(|| values.join(" "))
}

unsafe extern "C" fn client_command(player: *mut c_void, arguments: *const c_void) {
    let used = std::panic::catch_unwind(|| {
        let Some(command) = read_command(arguments.addr()) else {
            return false;
        };
        let listen = with_client(|client| {
            let Some(host) = &mut client.host else {
                return false;
            };
            if host.is_local_player(player.addr())
                && let Ok(request) = std::ffi::CString::new(command.as_str())
                && let Some(state) = host.command(&request)
            {
                client.set_inventory(state);
                true
            } else {
                host.guest(player.addr(), &command)
            }
        })
        .unwrap_or(false);
        listen
            || crate::dedicated::with_host(|host| host.guest(player.addr(), &command))
                .unwrap_or(false)
    })
    .unwrap_or(false);
    if used {
        return;
    }
    let addr = COMMAND_ORIGINAL.load(Ordering::Acquire);
    if addr != 0 {
        let original = unsafe { std::mem::transmute::<usize, Command>(addr) };
        unsafe { original(player, arguments) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    unsafe extern "thiscall" fn round_start(_: *mut c_void) -> *const c_char {
        c"round_start".as_ptr()
    }

    unsafe extern "thiscall" fn original_fire(_: *mut c_void, _: *mut c_void, _: bool) -> bool {
        true
    }

    unsafe extern "thiscall" fn unused_id(_: *mut c_void, _: *mut c_void) -> i32 {
        0
    }

    unsafe extern "thiscall" fn unused_steam_id(_: *mut c_void, _: *mut c_void) -> *const c_char {
        ptr::null()
    }

    unsafe extern "thiscall" fn unused_print(_: *mut c_void, _: *mut c_void, _: *const c_char) {}

    #[test]
    fn retired_event_callback_still_forwards_without_queueing() {
        let mut hooks = Hooks {
            engine: 0,
            user_id: unused_id,
            steam_id: unused_steam_id,
            client_print: unused_print,
            globals: None,
            fire: None,
            commands: None,
            frame: None,
            entities: None,
        };
        let table = [0usize, round_start as Name as usize];
        let mut object = table.as_ptr();
        let source = (&raw mut object).cast::<c_void>();
        FIRE_ORIGINAL.store(original_fire as Fire as usize, Ordering::Release);
        EVENTS.with_borrow_mut(|queue| *queue = Some(Vec::new()));
        assert!(unsafe { fire_event(ptr::null_mut(), source, false) });
        assert!(matches!(take_events().as_slice(), [Event::RoundStart]));

        hooks.stop().unwrap();
        assert!(unsafe { fire_event(ptr::null_mut(), source, false) });
        assert!(take_events().is_empty());
        FIRE_ORIGINAL.store(0, Ordering::Release);
    }
}
