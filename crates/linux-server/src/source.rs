//! Interfaces from the L4D2 SDK (ee978cde412bb8720453f1f89e5748ec1449a4b3).
//! The Linux x86 compiler passes the receiver as the first C argument.

use std::{
    ffi::{CStr, c_char, c_int, c_void},
    mem,
    path::PathBuf,
    ptr::{self, NonNull},
};

use crate::game::Vector;

pub type Factory = unsafe extern "C" fn(*const c_char, *mut c_int) -> *mut c_void;

#[repr(C)]
pub struct Entity {
    opaque: [u8; 0],
}

#[repr(C)]
pub struct Edict {
    state_flags: c_int,
    serial_number: c_int,
    networkable: *mut c_void,
    unknown: *mut c_void,
}

const _: () = assert!(mem::size_of::<Edict>() == 16);

pub struct Clients {
    first: NonNull<Edict>,
    count: usize,
}

impl Clients {
    /// Source owns the edict array from ServerActivate until LevelShutdown.
    pub unsafe fn new(edicts: *mut Edict, total: c_int, clients: c_int) -> Option<Self> {
        let first = NonNull::new(edicts)?;
        let count = usize::try_from(clients.min(total.checked_sub(1)?)).ok()?;
        Some(Self { first, count })
    }

    pub fn live(&self) -> impl Iterator<Item = NonNull<Edict>> + '_ {
        (1..=self.count).filter_map(|index| {
            // SAFETY: ServerActivate supplies the size, with the world edict at zero.
            let edict = unsafe { self.first.add(index) };
            let flags = unsafe { ptr::addr_of!((*edict.as_ptr()).state_flags).read() };
            (flags & (1 << 1) == 0).then_some(edict)
        })
    }
}

#[repr(C)]
pub struct Command {
    argc: c_int,
    command_size: c_int,
    full_command: [c_char; 512],
    argument_buffer: [c_char; 512],
    argv: [*const c_char; 64],
}

impl Command {
    pub fn arg(&self, index: usize) -> &str {
        if index >= self.argc.max(0) as usize {
            return "";
        }
        self.argv.get(index).map_or("", |&arg| {
            // The engine owns the argument storage for the duration of ClientCommand.
            unsafe { text(arg) }.unwrap_or("")
        })
    }
}

pub struct PlayerInfo {
    pub connected: bool,
    pub fake: bool,
    pub dead: bool,
    pub team: c_int,
}

pub struct Source {
    engine: NonNull<c_void>,
    player_manager: NonNull<c_void>,
    game_entities: NonNull<c_void>,
    tools: NonNull<c_void>,
    events: NonNull<c_void>,
    cvars: Option<NonNull<c_void>>,
    trace: Option<NonNull<c_void>>,
}

// All indices below refer to the ABI's vtable address point, after its header.
macro_rules! virtual_call {
    ($method:ident, $this:expr, $slot:literal, ($($arg_ty:ty => $arg:expr),*) -> $ret:ty) => {{
        let interface: NonNull<c_void> = $this;
        let $method: unsafe extern "C" fn(*mut c_void $(, $arg_ty)*) -> $ret =
            unsafe { virtual_method(interface, $slot) };
        unsafe { $method(interface.as_ptr() $(, $arg)*) }
    }};
}

impl Source {
    pub unsafe fn bind(engine_factory: Factory, game_factory: Factory) -> Option<Self> {
        let query = |factory: Factory, name: &CStr| {
            // SAFETY: The factories and their named interfaces come from Source's Load callback.
            NonNull::new(unsafe { factory(name.as_ptr(), ptr::null_mut()) })
        };
        let engine = query(engine_factory, c"VEngineServer022")?;
        if !virtual_call!(dedicated, engine, 2, () -> bool) {
            return None;
        }
        Some(Self {
            engine,
            player_manager: query(game_factory, c"PlayerInfoManager002")?,
            game_entities: query(game_factory, c"ServerGameEnts001")?,
            tools: query(game_factory, c"VSERVERTOOLS001")?,
            events: query(engine_factory, c"GAMEEVENTSMANAGER002")?,
            cvars: query(engine_factory, c"VEngineCvar007"),
            trace: query(engine_factory, c"EngineTraceServer003"),
        })
    }

    pub fn directory(&self) -> Option<PathBuf> {
        use std::{ffi::OsStr, os::unix::ffi::OsStrExt};

        let mut buffer = [0u8; 4096];
        virtual_call!(game_dir, self.engine, 52, (
            *mut c_char => buffer.as_mut_ptr().cast(), c_int => buffer.len() as c_int
        ) -> ());
        let path = CStr::from_bytes_until_nul(&buffer).ok()?.to_bytes();
        (!path.is_empty()).then(|| PathBuf::from(OsStr::from_bytes(path)))
    }

    pub fn user_id(&self, edict: NonNull<Edict>) -> Option<c_int> {
        let id = virtual_call!(user_id, self.engine, 16, (*mut Edict => edict.as_ptr()) -> c_int);
        (id > 0).then_some(id)
    }

    pub fn network_id(&self, edict: NonNull<Edict>) -> &str {
        let ptr = virtual_call!(network_id, self.engine, 17, (
            *mut Edict => edict.as_ptr()
        ) -> *const c_char);
        // SAFETY: Source owns this string until the player disconnects.
        unsafe { text(ptr) }.unwrap_or("")
    }

    pub fn client_print(&self, edict: NonNull<Edict>, message: &CStr) {
        virtual_call!(client_print, self.engine, 46, (
            *mut Edict => edict.as_ptr(), *const c_char => message.as_ptr()
        ) -> ());
    }

    pub fn player_info(&self, edict: NonNull<Edict>) -> Option<PlayerInfo> {
        let info = virtual_call!(player_info, self.player_manager, 0, (
            *mut Edict => edict.as_ptr()
        ) -> *mut c_void);
        let info = NonNull::new(info)?;
        Some(PlayerInfo {
            connected: virtual_call!(connected, info, 7, () -> bool),
            fake: virtual_call!(fake, info, 11, () -> bool),
            dead: virtual_call!(dead, info, 12, () -> bool),
            team: virtual_call!(team, info, 3, () -> c_int),
        })
    }

    pub fn model_name(&self, edict: NonNull<Edict>) -> Option<&str> {
        let info = virtual_call!(player_info, self.player_manager, 0, (
            *mut Edict => edict.as_ptr()
        ) -> *mut c_void);
        let model = virtual_call!(model, NonNull::new(info)?, 20, () -> *const c_char);
        unsafe { text(model) }
    }

    pub fn entity(&self, edict: NonNull<Edict>) -> Option<NonNull<Entity>> {
        NonNull::new(virtual_call!(entity, self.game_entities, 5, (
            *mut Edict => edict.as_ptr()
        ) -> *mut Entity))
    }

    pub fn create_entity(&self, classname: &CStr) -> Option<NonNull<Entity>> {
        NonNull::new(virtual_call!(create_entity, self.tools, 15, (
            *const c_char => classname.as_ptr()
        ) -> *mut Entity))
    }

    pub fn precache_model(&self, name: &CStr) -> bool {
        virtual_call!(precache, self.engine, 5, (
            *const c_char => name.as_ptr(), bool => false
        ) -> c_int)
            > 0
    }

    pub fn as_edict(&self, entity: NonNull<Entity>) -> Option<NonNull<Edict>> {
        NonNull::new(virtual_call!(as_edict, self.game_entities, 4, (
            *mut Entity => entity.as_ptr()
        ) -> *mut Edict))
    }

    pub fn current_time(&self) -> Option<f32> {
        let globals = virtual_call!(globals, self.player_manager, 1, () -> *mut u8);
        // SAFETY: This is the CGlobalVarsBase::curtime field in the pinned SDK.
        NonNull::new(globals).map(|p| unsafe { p.as_ptr().add(12).cast::<f32>().read() })
    }

    /// Source skips ServerActivate and LevelShutdown for paused plugins. Reacquire
    /// the live edicts from the SDK's CGlobalVars before using any on resume.
    pub fn current_level(&self) -> Option<(i32, String, Clients)> {
        let globals = virtual_call!(globals, self.player_manager, 1, () -> *mut u8);
        let globals = NonNull::new(globals)?.as_ptr();
        // CGlobalVarsBase is 60 bytes on L4D2 Linux x86. mapname is a string_t
        // (a string pointer here), followed by maxEntities, serverCount and pEdicts.
        unsafe {
            let clients = globals.add(20).cast::<i32>().read();
            let name = globals.add(60).cast::<*const c_char>().read();
            let total = globals.add(80).cast::<i32>().read();
            let generation = globals.add(84).cast::<i32>().read();
            let edicts = globals.add(88).cast::<*mut Edict>().read();
            if clients < 1 || clients >= total || clients > 255 {
                return None;
            }
            Some((
                generation,
                text(name)?.to_owned(),
                Clients::new(edicts, total, clients)?,
            ))
        }
    }

    pub fn find_cvar(&self, name: &CStr) -> Option<ConVar> {
        NonNull::new(virtual_call!(find_var, self.cvars?, 12, (
            *const c_char => name.as_ptr()
        ) -> *mut c_void))
        .map(ConVar)
    }

    pub fn trace(
        &self,
        start: Vector,
        delta: Vector,
        ignore: Option<NonNull<Edict>>,
        mask: u32,
    ) -> Option<Hit> {
        self.cast_ray(start, delta, ignore, mask, false)
    }

    pub fn trace_world(&self, start: Vector, delta: Vector, mask: u32) -> Option<Hit> {
        self.cast_ray(start, delta, None, mask, true)
    }

    fn cast_ray(
        &self,
        start: Vector,
        delta: Vector,
        ignore: Option<NonNull<Edict>>,
        mask: u32,
        only_world: bool,
    ) -> Option<Hit> {
        let trace = self.trace?;
        let ray = Ray {
            start: [start.x, start.y, start.z, 0.0],
            delta: [delta.x, delta.y, delta.z, 0.0],
            start_offset: [0.0; 4],
            extents: [0.0; 4],
            world_axis_transform: ptr::null(),
            ray: true,
            swept: delta.dot(delta) != 0.0,
        };
        let mut hit = RawHit([0; 84]);
        let mut filter = TraceFilter {
            methods: &TRACE_FILTER,
            only_world,
            ignore: ignore.map_or(ptr::null_mut(), |edict| {
                // SAFETY: Source supplies the live edict. GetIServerEntity checks FL_EDICT_FULL.
                unsafe {
                    let edict = edict.as_ptr();
                    if ptr::addr_of!((*edict).state_flags).read() & (1 << 2) != 0 {
                        ptr::addr_of!((*edict).unknown).read()
                    } else {
                        ptr::null_mut()
                    }
                }
            }),
        };
        virtual_call!(trace_ray, trace, 5, (
            *const Ray => &ray, u32 => mask, *mut TraceFilter => &mut filter, *mut RawHit => &mut hit
        ) -> ());
        let position = |base| -> Vector {
            let float = |offset| f32::from_ne_bytes(hit.0[offset..offset + 4].try_into().unwrap());
            Vector {
                x: float(base),
                y: float(base + 4),
                z: float(base + 8),
            }
        };
        Some(Hit {
            end: position(12),
            normal: position(24),
            fraction: f32::from_ne_bytes(hit.0[44..48].try_into().unwrap()),
            start_solid: hit.0[55] != 0,
            sky: u16::from_ne_bytes(hit.0[66..68].try_into().unwrap()) & 4 != 0,
            entity: NonNull::new(ptr::with_exposed_provenance_mut(usize::from_ne_bytes(
                hit.0[76..80].try_into().unwrap(),
            ))),
        })
    }

    pub fn set_key(&self, entity: NonNull<Entity>, key: &CStr, value: &CStr) -> bool {
        virtual_call!(set_key, self.tools, 12, (
            *mut Entity => entity.as_ptr(), *const c_char => key.as_ptr(), *const c_char => value.as_ptr()
        ) -> bool)
    }

    pub fn entities(&self) -> impl Iterator<Item = NonNull<Entity>> {
        std::iter::successors(
            NonNull::new(virtual_call!(first, self.tools, 8, () -> *mut Entity)),
            |entity| {
                NonNull::new(virtual_call!(next, self.tools, 9, (
                *mut Entity => entity.as_ptr()
            ) -> *mut Entity))
            },
        )
    }

    pub fn add_listener(&self, listener: NonNull<c_void>, name: &CStr) -> bool {
        virtual_call!(add_listener, self.events, 4, (
            *mut c_void => listener.as_ptr(), *const c_char => name.as_ptr(), bool => true
        ) -> bool)
    }

    pub fn remove_listener(&self, listener: NonNull<c_void>) {
        virtual_call!(remove_listener, self.events, 6, (*mut c_void => listener.as_ptr()) -> ());
    }
}

#[repr(C, align(16))]
struct Ray {
    start: [f32; 4],
    delta: [f32; 4],
    start_offset: [f32; 4],
    extents: [f32; 4],
    world_axis_transform: *const c_void,
    ray: bool,
    swept: bool,
}

#[repr(C, align(4))]
struct RawHit([u8; 84]);

const _: () = {
    assert!(mem::size_of::<Ray>() == 80);
    assert!(mem::offset_of!(Ray, ray) == 68);
    assert!(mem::size_of::<RawHit>() == 84);
};

#[repr(C)]
struct TraceFilter {
    methods: &'static TraceMethods,
    ignore: *mut c_void,
    only_world: bool,
}

#[repr(C)]
struct TraceMethods {
    should_hit: unsafe extern "C" fn(*const TraceFilter, *mut c_void, c_int) -> bool,
    trace_type: unsafe extern "C" fn(*const TraceFilter) -> c_int,
}

static TRACE_FILTER: TraceMethods = TraceMethods {
    should_hit,
    trace_type,
};

unsafe extern "C" fn should_hit(filter: *const TraceFilter, entity: *mut c_void, _: c_int) -> bool {
    unsafe { !(*filter).only_world && (*filter).ignore != entity }
}

unsafe extern "C" fn trace_type(filter: *const TraceFilter) -> c_int {
    // TRACE_WORLD_ONLY = 1, TRACE_EVERYTHING = 0.
    i32::from(unsafe { (*filter).only_world })
}

pub struct Hit {
    pub end: Vector,
    pub normal: Vector,
    pub fraction: f32,
    pub start_solid: bool,
    pub sky: bool,
    pub entity: Option<NonNull<Entity>>,
}

pub struct ConVar(NonNull<c_void>);

impl ConVar {
    pub fn value(&self) -> f32 {
        // SAFETY: ConVar::GetFloat reads m_pParent->m_fValue in the pinned SDK.
        unsafe {
            let parent = self
                .0
                .cast::<u8>()
                .as_ptr()
                .add(28)
                .cast::<*const u8>()
                .read();
            parent.add(44).cast::<f32>().read()
        }
    }

    pub fn set(&self, value: f32, integer: bool) {
        if integer {
            virtual_call!(set_int, self.0, 17, (i32 => value.round().max(0.0) as i32) -> ());
        } else {
            virtual_call!(set_float, self.0, 16, (f32 => value) -> ());
        }
    }
}

/// The caller must hold a live IGameEvent for the duration of the callback.
pub unsafe fn event_name<'a>(event: NonNull<c_void>) -> Option<&'a str> {
    let name = virtual_call!(name, event, 2, () -> *const c_char);
    // SAFETY: GetName provides a valid NUL-terminated event name.
    unsafe { text(name) }
}

pub unsafe fn event_int(event: NonNull<c_void>, key: &CStr) -> c_int {
    virtual_call!(int, event, 7, (*const c_char => key.as_ptr(), c_int => 0) -> c_int)
}

pub unsafe fn event_bool(event: NonNull<c_void>, key: &CStr) -> bool {
    virtual_call!(get_bool, event, 6, (*const c_char => key.as_ptr(), bool => false) -> bool)
}

/// The caller verifies the object, slot and signature against the L4D2 SDK.
pub unsafe fn virtual_method<T: Copy>(object: NonNull<c_void>, slot: usize) -> T {
    assert_eq!(mem::size_of::<T>(), mem::size_of::<*const c_void>());
    // SAFETY: The validated SDK interface starts with a pointer to its methods.
    unsafe {
        let methods = object.cast::<*const *const c_void>().as_ptr().read();
        mem::transmute_copy(&methods.add(slot).read())
    }
}

/// The caller ensures the pointer is a live C string for the returned lifetime.
pub unsafe fn text<'a>(value: *const c_char) -> Option<&'a str> {
    if value.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(value) }.to_str().ok()
}
