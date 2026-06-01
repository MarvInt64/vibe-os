//! init - Der erste Prozess (PID 1)
//!
//! 1. Startet die Shell (/bin/sh)
//! 2. Wartet auf Kind-Prozesse und räumt auf

#![no_std]
#![no_main]

use libvex::{exec, exit, fork, getpid, println, wait};

#[no_mangle]
pub extern "C" fn _start() -> ! {
    println!("VibeOS starting...");
    println!("init: PID {}", getpid());

    // Starte die Shell
    match fork() {
        Ok(0) => {
            // Kind-Prozess: starte Shell
            let _ = exec("/bin/sh", &[]);
            println!("init: failed to exec shell");
            exit(1);
        }
        Ok(pid) => {
            // Eltern: warte auf Kind
            println!("init: started shell (PID {})", pid);
            loop {
                match wait(0) {
                    Ok(status) => {
                        println!("init: child exited with status {}", status);
                        // Neu starten
                        let _ = fork();
                    }
                    Err(_) => {
                        // Keine Kinder mehr
                        break;
                    }
                }
            }
        }
        Err(_) => {
            println!("init: fork failed");
        }
    }

    exit(0);
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
