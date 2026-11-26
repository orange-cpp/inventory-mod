//! Source server plugin for the 32-bit Linux build of Left 4 Dead 2.

#![cfg(all(target_os = "linux", target_arch = "x86"))]

mod game;
mod plugin;
mod source;
