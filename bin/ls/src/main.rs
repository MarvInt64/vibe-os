//! ls - Dateien auflisten

#![no_std]
#![no_main]

use libvex::{exit, getpid, println};

#[no_mangle]
pub extern "C" fn _start() -> ! {
    println!("ls: PID {}", getpid());

    // In echtem System: Dateisystem durchsuchen
    // Hier: Beispiel-Ausgabe
    println!("total 42");
    println!("drwxr-xr-x  2 root root  4096  Jan  1 00:00 .");
    println!("drwxr-xr-x  3 root root  4096  Jan  1 00:00 ..");
    println!("-rwxr-xr-x  1 root root  8192  Jan  1 00:00 init");
    println!("-rwxr-xr-x  1 root root  8192  Jan  1 00:00 sh");
    println!("-rwxr-xr-x  1 root root  8192  Jan  1 00:00 editor");
    println!("-rwxr-xr-x  1 root root  4096  Jan  1 00:00 ls");

    exit(0);
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
