//! A Source render or input thread can fetch a function pointer, be preempted,
//! and enter it after the client unloads. Keep only these typed forwarding
//! entries resident. They hold no game state, textures, or client resources.
mod api;

use api::{Api, Method};
use std::{
    ffi::c_void,
    sync::{
        RwLock,
        atomic::{AtomicUsize, Ordering, fence},
    },
};

struct Gate {
    original: AtomicUsize,
    client: RwLock<Option<usize>>,
}

impl Gate {
    const fn new() -> Self {
        Self {
            original: AtomicUsize::new(0),
            client: RwLock::new(None),
        }
    }

    fn call<T>(&self, run: impl FnOnce(usize) -> T) -> T {
        if let Ok(reader) = self.client.try_read() {
            if let Some(handler) = *reader {
                // Keep the mapping callable until the entire client handler returns.
                return run(handler);
            }
        }
        run(self.original.load(Ordering::Acquire))
    }
}

static GATES: [Gate; 6] = [const { Gate::new() }; 6];

macro_rules! entry {
    ($abi:literal, $name:ident, $kind:ident, ($($arg:ident: $ty:ty),*) -> $r:ty) => {
        unsafe extern $abi fn $name($($arg: $ty),*) -> $r {
            GATES[Method::$kind as usize].call(|address| {
                let method: unsafe extern $abi fn($($ty),*) -> $r =
                    unsafe { std::mem::transmute(address) };
                unsafe { method($($arg),*) }
            })
        }
    };
}

type Ptr = *mut c_void;
entry!("system", present, Present, (device: Ptr, src: Ptr, dst: Ptr, window: Ptr, dirty: Ptr) -> i32);
entry!("system", reset, Reset, (device: Ptr, params: Ptr) -> i32);
entry!("system", unlock, Unlock, (buffer: Ptr, p1: Ptr, n1: u32, p2: Ptr, n2: u32) -> i32);
entry!("system", get_cursor, GetCursor, (point: Ptr) -> i32);
entry!("system", set_cursor, SetCursor, (x: i32, y: i32) -> i32);
entry!("thiscall", before_hud, BeforeHud, (this: Ptr, view: Ptr) -> ());

extern "system" fn connect(kind: Method, original: usize, client: usize) -> usize {
    if original == 0 || client == 0 {
        return 0;
    }
    let gate = &GATES[kind as usize];
    let Ok(mut current) = gate.client.try_write() else {
        return 0;
    };
    if current.is_some() {
        return 0;
    }
    // Existing cached pointers always have the same underlying ABI and original.
    if gate
        .original
        .compare_exchange(0, original, Ordering::AcqRel, Ordering::Acquire)
        .is_err_and(|previous| previous != original)
    {
        return 0;
    }
    *current = Some(client);
    match kind {
        Method::Present => present as *const () as usize,
        Method::Reset => reset as *const () as usize,
        Method::Unlock => unlock as *const () as usize,
        Method::GetCursor => get_cursor as *const () as usize,
        Method::SetCursor => set_cursor as *const () as usize,
        Method::BeforeHud => before_hud as *const () as usize,
    }
}

extern "system" fn disconnect(kind: Method, client: usize) -> bool {
    let Ok(mut current) = GATES[kind as usize].client.try_write() else {
        return false;
    };
    if *current == Some(client) {
        *current = None;
    }
    true
}

#[repr(C)]
struct Task {
    methods: &'static TaskMethods,
    references: AtomicUsize,
    layer: u32,
    client: RwLock<Option<extern "system" fn(u32)>>,
}
#[repr(C)]
struct TaskMethods {
    add_ref: unsafe extern "thiscall" fn(*const Task) -> i32,
    release: unsafe extern "thiscall" fn(*const Task) -> i32,
    destructor: unsafe extern "thiscall" fn(*const Task, i32) -> *const Task,
    invoke: unsafe extern "thiscall" fn(*const Task),
}

unsafe extern "thiscall" fn retain(task: *const Task) -> i32 {
    (unsafe { &*task }.references.fetch_add(1, Ordering::Relaxed) + 1) as i32
}
unsafe extern "thiscall" fn release(task: *const Task) -> i32 {
    let left = unsafe { &*task }.references.fetch_sub(1, Ordering::Release) - 1;
    if left == 0 {
        fence(Ordering::Acquire);
        drop(unsafe { Box::from_raw(task.cast_mut()) });
    }
    left as i32
}
unsafe extern "thiscall" fn destructor(task: *const Task, _: i32) -> *const Task {
    task
}
unsafe extern "thiscall" fn invoke(task: *const Task) {
    let task = unsafe { &*task };
    if let Ok(reader) = task.client.try_read() {
        if let Some(client) = *reader {
            client(task.layer);
        }
    }
}
static METHODS: TaskMethods = TaskMethods {
    add_ref: retain,
    release,
    destructor,
    invoke,
};
extern "system" fn new_task(layer: u32, client: extern "system" fn(u32)) -> *const c_void {
    Box::into_raw(Box::new(Task {
        methods: &METHODS,
        references: AtomicUsize::new(1),
        layer,
        client: RwLock::new(Some(client)),
    }))
    .cast()
}
unsafe extern "system" fn retire_task(task: *const c_void) -> bool {
    let task = task.cast::<Task>();
    let Ok(mut current) = (unsafe { &*task }).client.try_write() else {
        return false;
    };
    *current = None;
    drop(current);
    // A canceled queue releases its reference without ever invoking the task.
    // That release can run after unload and is implemented entirely in this DLL.
    unsafe { release(task) };
    true
}

static API: Api = Api {
    connect,
    disconnect,
    new_task,
    retire_task,
};
#[unsafe(no_mangle)]
pub extern "system" fn inventory_callbacks_v1() -> &'static Api {
    &API
}
