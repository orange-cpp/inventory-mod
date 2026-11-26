use std::{
    ffi::{CStr, c_char, c_int, c_void},
    mem,
    ptr::NonNull,
    slice,
};

use super::{Vector, source::Entity};

const SERVER_BUILD_ID: [u8; 20] = [
    0xc9, 0xb0, 0xcd, 0x3d, 0x18, 0x00, 0xcb, 0x19, 0x08, 0x30, 0x26, 0xf3, 0x49, 0xa9, 0xde, 0x15,
    0x93, 0x3c, 0x05, 0xac,
];

#[repr(C)]
struct ProgramHeader {
    kind: u32,
    offset: u32,
    virtual_address: u32,
    physical_address: u32,
    file_size: u32,
    memory_size: u32,
    flags: u32,
    alignment: u32,
}

/// The prefix of glibc's dl_phdr_info shared by supported glibc versions.
#[repr(C)]
struct Module {
    base: usize,
    name: *const c_char,
    headers: *const ProgramHeader,
    header_count: u16,
}

#[link(name = "dl")]
unsafe extern "C" {
    fn dl_iterate_phdr(
        callback: unsafe extern "C" fn(*const Module, usize, *mut c_void) -> c_int,
        context: *mut c_void,
    ) -> c_int;
}

macro_rules! server_functions {
    ($($name:ident: fn($($args:ty),*) $( -> $ret:ty)? = $offset:literal, [$($b:literal),+];)+) => {
        #[derive(Clone, Copy)]
        pub struct ServerFunctions {
            pub base: usize,
            $(pub $name: unsafe extern "C" fn($($args),*) $( -> $ret)?,)+
        }

        impl ServerFunctions {
            fn resolve(module: &Module) -> Option<Self> {
                Some(Self {
                    base: module.base,
                    $($name: module.resolve($offset, &[$($b),+]).or_else(|| {
                        eprintln!("inventory_mod: signature mismatch for {}", stringify!($name));
                        None
                    })?,)+
                })
            }
        }
    };
}

server_functions! {
    weapon_in_slot: fn(*mut Entity, i32) -> *mut Entity = 0x3ee360, [0x55, 0x89, 0xe5, 0x57, 0x31, 0xff, 0x56, 0x53];
    active_weapon: fn(*mut Entity) -> *mut Entity = 0x3ee410, [0x55, 0x8b, 0x0d];
    classname: fn(*mut Entity) -> *const c_char = 0x607a90, [0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08, 0x5d, 0x8b, 0x50, 0x7c];
    select_item: fn(*mut Entity, *const c_char, i32) = 0x406ee0, [0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x10];
    eye_vectors: fn(*mut Entity, *mut Vector, *mut Vector, *mut Vector, *mut Vector) = 0x406560, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x3c];
    origin: fn(*mut Entity) -> *const Vector = 0x6093a0, [0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x14];
    owner: fn(*mut Entity) -> *mut Entity = 0x3f5f30, [0x55, 0x8b, 0x0d];
    network_property: fn(*mut Entity) -> *mut c_void = 0x3d0500, [0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08, 0x5d, 0x83, 0xc0, 0x24];
    marked_for_deletion: fn(*mut c_void) -> bool = 0x8114e0, [0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08, 0x5d, 0x8b, 0x40, 0x08];
    remove: fn(*mut Entity) = 0xb199d0, [0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08, 0x85, 0xc0];
    quiet_drop: fn(*mut Entity, *mut Entity, *const Vector, *const Vector) = 0x7afb80, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53];
    weapon_drop: fn(*mut Entity, *mut Entity, *const Vector, *const Vector) = 0x9c6480, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x2c];
    give_item: fn(*mut Entity, *const c_char, i32, bool, *mut Entity) -> *mut Entity = 0x98b820, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x81, 0xec, 0x5c, 0x01];
    incapacitated: fn(*mut Entity) -> bool = 0x9349f0, [0x55, 0x31, 0xc0, 0x89, 0xe5, 0x8b, 0x55, 0x08];
    dominated: fn(*mut Entity) -> bool = 0x4e63b0, [0x55, 0x89, 0xe5, 0x56, 0x8b, 0x55, 0x08, 0x53];
    set_ammo_count: fn(*mut Entity, i32, i32) = 0x3ee5e0, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x8d, 0x45, 0x0c];
    equip: fn(*mut Entity, *mut Entity) = 0x5e68a0, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x31, 0xf6];
    network_changed: fn(*mut Entity) = 0x7bd6a0, [0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x14];
    melee_script_name: fn(*mut Entity) -> *const c_char = 0x5580a0, [0x55, 0x89, 0xe5, 0x83, 0xec, 0x18];
    set_origin: fn(*mut Entity, *const Vector) = 0x5fc720, [0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x50];
    dispatch_spawn: fn(*mut Entity, bool) -> i32 = 0xb1dce0, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x2c];
    drop_to_floor: fn(*mut Entity, u32, *mut Entity) -> i32 = 0xb19230, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x81, 0xec, 0x8c];
    pile_use: fn(*mut Entity, *mut Entity, *mut Entity, i32, f32) = 0x8ccf10, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x2c];
    weapon_spawn_use: fn(*mut Entity, *mut Entity, *mut Entity, i32, f32) = 0x8c6700, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x5c];
    spawned_weapon_name: fn(*mut Entity) -> *const c_char = 0x8c6c00, [0x55, 0x89, 0xe5, 0x83, 0xec, 0x18];
    gun_reload: fn(*mut Entity) -> bool = 0x555380, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x2c];
    shotgun_reload: fn(*mut Entity) -> bool = 0x517780, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x2c];
    fire_empty: fn(*mut Entity) = 0x3f4a90, [0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x14];
    panic_event: fn(*mut c_void, *mut Entity, i32) = 0x865130, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x1c];
    bump_weapon: fn(*mut Entity, *mut Entity) -> bool = 0x98c240, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x2c];
    gun_use: fn(*mut Entity, *mut Entity, *mut Entity, i32, f32) = 0x554da0, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x2c];
    melee_spawn_name: fn(*mut Entity) -> *const c_char = 0x8c6c60, [0x55, 0x89, 0xe5, 0x83, 0xec, 0x18];
    set_shove_penalty: fn(*mut Entity, i32) = 0x4e3880, [0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x10];
    adrenaline_active: fn(*mut Entity) -> bool = 0x4e6620, [0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08, 0x5d, 0x0f, 0xb6];
    chainsaw_frame: fn(*mut Entity) = 0x5233a0, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x81, 0xec, 0xac, 0x01];
    on_revived: fn(*mut Entity) = 0x9b36b0, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x6c];
    hanging_from_ledge: fn(*mut Entity) -> bool = 0x9d3fd0, [0x55, 0x89, 0xe5, 0x8b, 0x45, 0x08, 0x5d];
    on_adrenaline_used: fn(*mut Entity, f32) = 0x989450, [0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x10];
    rescue_call: fn(*mut Entity, *mut Entity) = 0x959fc0, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x5c];
    commit_suicide: fn(*mut Entity, bool, bool) = 0x799a10, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x81, 0xec, 0x8c];
    spawn_special: fn(*mut c_void, i32, *const Vector, *const Vector) -> *mut Entity = 0xa48f80, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x1c];
    precache_sound: fn(*mut Entity, *const c_char) = 0x4a3150, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x4c];
    emit_sound: fn(*mut Entity, *const c_char, f32, *mut f32) = 0x4a46f0, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x81, 0xec, 0xdc];
    zombie_class: fn(*mut Entity) -> i32 = 0x9d4220, [0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x14];
    set_glow_state: fn(*mut c_void, i32) = 0x455e40, [0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x10];
    set_glow_range: fn(*mut c_void, i32) = 0x455e80, [0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x10];
    set_glow_minimum: fn(*mut c_void, i32) = 0x455eb0, [0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x10];
    set_glow_color: fn(*mut c_void, u32) = 0x455ef0, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x1c];
    melee_primary: fn(*mut Entity) = 0x55b6b0, [0x55, 0x89, 0xe5, 0x56, 0x53, 0x83, 0xec, 0x10];
    melee_secondary: fn(*mut Entity) = 0x558fd0, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x1c];
    view_punch: fn(*mut Entity, *const Vector) = 0x408750, [0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x24];
    stagger_player: fn(*mut Entity, *mut Entity, *const Vector) = 0x99e650, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x5c];
    shove_responder: fn(*mut c_void, *mut Entity) = 0x3d26e0, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x1c];
    melee_info: fn(*mut Entity) -> *const u8 = 0x558cf0, [0x55, 0x89, 0xe5, 0x83, 0xec, 0x18, 0x8b, 0x45, 0x08];
    animation_event: fn(*mut Entity, i32, i32) = 0x986a80, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x1c];
    weapon_sound: fn(*mut Entity, i32, f32) = 0x3f6620, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x81, 0xec, 0x1c, 0x01];
    teleport: fn(*mut Entity, *const Vector, *const Vector, *const Vector) = 0x605310, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x81, 0xec, 0x9c];
    damage_info: fn(*mut u8, *mut Entity, *mut Entity, *mut Entity, f32, i32, i32) = 0x4b7a60, [0x55, 0x89, 0xe5, 0x83, 0xec, 0x38, 0x8b, 0x45, 0x08];
    take_damage: fn(*mut Entity, *const u8) -> i32 = 0x5f5410, [0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x7c];
}

impl ServerFunctions {
    pub fn bind() -> Option<Self> {
        let mut functions = None;
        // SAFETY: The context is stack-owned and dl_iterate_phdr completes synchronously.
        unsafe { dl_iterate_phdr(probe, (&mut functions as *mut Option<Self>).cast()) };
        functions
    }
}

unsafe extern "C" fn probe(module: *const Module, size: usize, context: *mut c_void) -> c_int {
    if size < mem::offset_of!(Module, header_count) + mem::size_of::<u16>() {
        return 0;
    }
    let (Some(module), Some(out)) = (unsafe { module.as_ref() }, unsafe {
        context.cast::<Option<ServerFunctions>>().as_mut()
    }) else {
        return 0;
    };
    let Some(name) = NonNull::new(module.name.cast_mut()) else {
        return 0;
    };
    let name = unsafe { CStr::from_ptr(name.as_ptr()) }.to_bytes();
    if name.rsplit(|&byte| byte == b'/').next() != Some(b"server_srv.so") {
        return 0;
    }
    if module.has_build_id() {
        *out = ServerFunctions::resolve(module);
    }
    1
}

impl Module {
    fn headers(&self) -> &[ProgramHeader] {
        if self.header_count == 0 || self.headers.is_null() {
            return &[];
        }
        // SAFETY: ld.so owns this table and keeps it live during the callback.
        unsafe { slice::from_raw_parts(self.headers, usize::from(self.header_count)) }
    }

    fn bytes(&self, address: usize, size: usize) -> Option<&[u8]> {
        let end = address.checked_add(size)?;
        let mapped = self.headers().iter().any(|segment| {
            const PT_LOAD: u32 = 1;
            const PF_READ: u32 = 4;
            segment.kind == PT_LOAD
                && segment.flags & PF_READ != 0
                && address >= segment.virtual_address as usize
                && end
                    <= (segment.virtual_address as usize)
                        .saturating_add(segment.memory_size as usize)
        });
        if !mapped || size > isize::MAX as usize {
            return None;
        }
        let start = self.base.checked_add(address)?;
        // SAFETY: The address range was checked against a readable mapped segment.
        Some(unsafe { slice::from_raw_parts(std::ptr::with_exposed_provenance(start), size) })
    }

    fn has_build_id(&self) -> bool {
        self.headers()
            .iter()
            .filter(|segment| segment.kind == 4)
            .any(|segment| {
                let Some(notes) =
                    self.bytes(segment.virtual_address as usize, segment.file_size as usize)
                else {
                    return false;
                };
                GnuNotes(notes).any(|(kind, name, descriptor)| {
                    kind == 3 && name == b"GNU\0" && descriptor == SERVER_BUILD_ID
                })
            })
    }

    fn resolve<F: Copy>(&self, offset: usize, signature: &[u8]) -> Option<F> {
        if mem::size_of::<F>() != mem::size_of::<*const u8>() {
            return None;
        }
        let executable = self.headers().iter().any(|segment| {
            segment.kind == 1
                && segment.flags & 1 != 0
                && offset >= segment.virtual_address as usize
                && offset.saturating_add(signature.len())
                    <= (segment.virtual_address as usize)
                        .saturating_add(segment.memory_size as usize)
        });
        let bytes = self.bytes(offset, signature.len())?;
        if !executable || bytes != signature {
            return None;
        }
        // SAFETY: Signatures are declared against the exact supported build and live address.
        Some(unsafe { mem::transmute_copy(&bytes.as_ptr()) })
    }
}

struct GnuNotes<'a>(&'a [u8]);

impl<'a> Iterator for GnuNotes<'a> {
    type Item = (u32, &'a [u8], &'a [u8]);

    fn next(&mut self) -> Option<Self::Item> {
        fn take_aligned(input: &mut &[u8], size: usize) -> Option<()> {
            *input = input.get(size.checked_add(3)? & !3..)?;
            Some(())
        }

        let input = &mut self.0;
        let (header, remainder) = input.split_at_checked(12)?;
        let length = |index| u32::from_le_bytes(header[index..index + 4].try_into().unwrap());
        let namesize = length(0) as usize;
        let descsize = length(4) as usize;
        let kind = length(8);
        let name = remainder.get(..namesize)?;
        *input = remainder;
        take_aligned(input, namesize)?;
        let descriptor = input.get(..descsize)?;
        take_aligned(input, descsize)?;
        Some((kind, name, descriptor))
    }
}

// CBaseEntity/CTerrorPlayer and derived weapon fields for the same server build.
pub const CLIP: usize = 0x1420;
pub const IN_RELOAD: usize = 0x1459;
pub const FIRE_ON_EMPTY: usize = 0x145a;
pub const BUTTONS: usize = 0x1cb0;
pub const SPAWN_COUNT: usize = 0x144c;
pub const DIRECTOR: usize = 0xfeff08;
pub const PLAYER_AMMO: usize = 0x1874;
pub const WEAPON_OWNER: usize = 0x13f4;
pub const PRIMARY_AMMO_TYPE: usize = 0x1418;
pub const EXTRA_PRIMARY_AMMO: usize = 0x14fc;
pub const UPGRADES: usize = 0x17e4;
pub const SPECIAL_ROUNDS: usize = 0x1790;
pub const FLAGS: usize = 0x150;
pub const LOCAL_VELOCITY: usize = 0x2ec;
pub const ABSOLUTE_VELOCITY: usize = 0x274;
pub const LAGGED_MOVEMENT: usize = 0x20cc;
pub const NEXT_PRIMARY: usize = 0x13fc;
pub const NEXT_SECONDARY: usize = 0x1400;
pub const SHOVE_PENALTY: usize = 0x2cdc;
pub const PROGRESS_START: usize = 0x2904;
pub const PROGRESS_DURATION: usize = 0x2908;
pub const LIFE_STATE: usize = 0x104;
pub const ZOMBIE_MANAGER: usize = 0x10170e0;
pub const SURVIVOR_CHARACTER: usize = 0x2ba0;
pub const CORPSE_CHARACTER: usize = 0x17d8;
pub const ENTITY_LIST: usize = 0xf201bc;
pub const TEAM: usize = 0x24c;
pub const HEALTH: usize = 0x100;
pub const MAX_HEALTH: usize = 0xfc;
pub const HIT_GROUP: usize = 0x1838;
pub const INFECTED_NEXTBOT: usize = 0x1a3c;
pub const INFECTED_PUSH_ONLY: usize = 0x1d90;
pub const MELEE_NEXT_SEQUENCE: usize = 0x17fc;
pub const MELEE_SEQUENCE_TIME: usize = 0x182c;
