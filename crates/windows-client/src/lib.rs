//! Windows x86 client for Left 4 Dead 2.

#![cfg(all(windows, target_arch = "x86"))]

mod addons;
mod audio;
mod client;
mod cursor;
mod cvars;
mod dedicated;
mod engine;
mod fog;
mod host;
mod hud;
mod inventory_ui;
mod melee;
mod menu;
mod native;
mod overlay;
mod postfx;
mod reload;
mod vgui;
mod wheel;
