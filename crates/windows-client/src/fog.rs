use std::{
    ffi::CStr,
    time::{Duration, Instant},
};

use inventory_mod::{mutators::Mutator, protocol::ActiveMutator};

use crate::cvars::{ConVar, Restore};

#[derive(Default)]
pub struct Fog {
    configured: Option<Description>,
    pending: Option<Instant>,
    original: Vec<Restore>,
}

#[derive(Clone, Copy, PartialEq)]
struct Description {
    density: f32,
    color: Option<[u8; 3]>,
}

impl Fog {
    pub fn apply(&mut self, list: &[ActiveMutator]) {
        let desc = list
            .iter()
            .find(|mode| mode.mutator == Mutator::Fog)
            .map(|mode| Description {
                density: mode.fog_density.unwrap_or(0.6),
                color: mode.fog_color,
            });
        if desc == self.configured {
            return;
        }
        if let Some((before, next)) = self.configured.zip(desc)
            && before.color == next.color
            && !self.original.is_empty()
        {
            self.configured = Some(next);
            if self.pending.is_none() {
                for (name, value) in distances(next.density) {
                    if let Some(var) = ConVar::find(name) {
                        var.set(&value.to_string());
                    }
                }
            }
            return;
        }
        self.clear();
        self.configured = desc;
        if desc.is_some()
            && let Some(fog) = ConVar::find(c"fog_override")
            && let Some(value) = fog.change("-1")
        {
            self.original.push(value);
            self.pending = Some(Instant::now() + Duration::from_millis(300));
        }
    }

    pub fn tick(&mut self) {
        if self.pending.is_none_or(|at| Instant::now() < at) {
            return;
        }
        self.pending = None;
        let Some(description) = self.configured else {
            return;
        };
        let color = description
            .color
            .map(|[r, g, b]| format!("{r} {g} {b}"))
            .or_else(|| ConVar::find(c"fog_color")?.value());
        if let Some(fog) = ConVar::find(c"fog_override") {
            fog.set("1");
        }
        let mut set = |name: &CStr, value: &str| {
            if let Some(saved) = ConVar::find(name).and_then(|var| var.change(value)) {
                self.original.push(saved);
            }
        };
        set(c"fog_enable", "1");
        set(c"fog_enableskybox", "1");
        set(c"fog_maxdensity", "1");
        set(c"fog_maxdensityskybox", "1");
        for (name, value) in distances(description.density) {
            set(name, &value.to_string());
        }
        if let Some(color) = color {
            set(c"fog_color", &color);
            set(c"fog_colorskybox", &color);
        }
    }

    pub fn clear(&mut self) {
        for value in self.original.drain(..).rev() {
            value.restore();
        }
        self.configured = None;
        self.pending = None;
    }
}

fn distances(density: f32) -> [(&'static CStr, i32); 4] {
    let density = density.clamp(0.0, 1.0);
    let start = (300.0 - 290.0 * density) as i32;
    let end = (1200.0 - 1000.0 * density) as i32;
    [
        (c"fog_start", start),
        (c"fog_end", end),
        (c"fog_startskybox", start / 16),
        (c"fog_endskybox", end / 16),
    ]
}
