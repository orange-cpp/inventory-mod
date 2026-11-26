use std::ffi::{CStr, CString, c_char, c_void};

use crate::{host::factory, native};

type Find = unsafe extern "thiscall" fn(*mut c_void, *const c_char) -> *mut c_void;
type Set = unsafe extern "thiscall" fn(*mut c_void, *const c_char);

#[derive(Clone, Copy)]
pub struct ConVar {
    primary: usize,
    iface: usize,
    set: Set,
}

impl ConVar {
    pub fn find(name: &CStr) -> Option<Self> {
        let create = factory("vstdlib.dll").or_else(|| factory("engine.dll"))?;
        let interface = unsafe { create(c"VEngineCvar007".as_ptr(), std::ptr::null_mut()) }.addr();
        let at = native::vslot(interface, 13)?; // FindVar(const char*) in the MSVC vtable
        let lookup = unsafe { std::mem::transmute::<usize, Find>(at) };
        let primary = unsafe { lookup(address(interface), name.as_ptr()) }.addr();
        if primary == 0 {
            return None;
        }
        let actual = native::word(primary + 0xc).and_then(|text| native::string(text as usize, 96));
        if actual.as_deref()?.as_bytes() != name.to_bytes() {
            return None;
        }
        // ConVar implements IConVar through its secondary vtable.
        let iface = primary + 0x18;
        let target = native::vslot(iface, 3)?;
        let set = unsafe { std::mem::transmute::<usize, Set>(target) };
        Some(Self {
            primary,
            iface,
            set,
        })
    }

    fn parent(&self) -> usize {
        native::word(self.primary + 0x1c)
            .map(|p| p as usize)
            .filter(|&ptr| ptr != 0)
            .unwrap_or(self.primary)
    }

    pub fn value(&self) -> Option<String> {
        let text = native::word(self.parent() + 0x24)?;
        native::string(text as usize, 256)
    }

    pub fn scalar(&self) -> Option<f32> {
        native::word(self.parent() + 0x2c)
            .map(f32::from_bits)
            .filter(|n| n.is_finite())
    }

    pub fn set(&self, value: &str) {
        if let Ok(value) = CString::new(value) {
            unsafe { (self.set)(address(self.iface), value.as_ptr()) };
        }
    }

    pub fn change(&self, value: &str) -> Option<Restore> {
        let before = self.value()?;
        self.set(value);
        Some(Restore {
            variable: *self,
            before,
        })
    }
}

pub struct Restore {
    variable: ConVar,
    before: String,
}

impl Restore {
    pub fn restore(self) {
        self.variable.set(&self.before);
    }
}

fn address(addr: usize) -> *mut c_void {
    std::ptr::with_exposed_provenance_mut(addr)
}
