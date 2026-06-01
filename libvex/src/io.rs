//! I/O Funktionen für libvex

use core::fmt::Arguments;

/// Print zu stdout
pub fn print(args: Arguments) {
    struct Writer;

    impl core::fmt::Write for Writer {
        fn write_str(&mut self, s: &str) -> core::fmt::Result {
            unsafe {
                super::raw::syscall3(4, 1, s.as_ptr() as usize, s.len());
            }
            Ok(())
        }
    }

    let mut w = Writer;
    let _ = core::fmt::write(&mut w, args);
}

/// Print zu stderr
pub fn eprint(args: Arguments) {
    struct Writer;

    impl core::fmt::Write for Writer {
        fn write_str(&mut self, s: &str) -> core::fmt::Result {
            unsafe {
                super::raw::syscall3(4, 2, s.as_ptr() as usize, s.len());
            }
            Ok(())
        }
    }

    let mut w = Writer;
    let _ = core::fmt::write(&mut w, args);
}

/// Liest eine Zeile von stdin
pub fn read_line(buf: &mut [u8]) -> Result<usize, ()> {
    let mut i = 0;
    while i < buf.len() {
        let mut c = [0u8; 1];
        let ret = unsafe { super::raw::syscall3(3, 0, c.as_mut_ptr() as usize, 1) };
        if ret == 0 {
            break;
        }
        if c[0] == b'\n' {
            break;
        }
        // Überspringe \r (Carriage Return) - wichtig für macOS
        if c[0] == b'\r' {
            continue;
        }
        buf[i] = c[0];
        i += 1;
    }
    Ok(i)
}
