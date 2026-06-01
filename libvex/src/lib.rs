//! libvex - VibeOS Userspace Library
//!
//! Bietet Syscall-Wrapper und Standard-Funktionen für User-Programme

#![no_std]

pub mod syscall;
pub mod io;
pub mod string;

// ============================================================================
// SYSCALLS (Raw)
// ============================================================================

pub mod raw {
    #[inline(always)]
    pub unsafe fn syscall0(n: usize) -> usize {
        let ret: usize;
        core::arch::asm!(
            "syscall",
            in("rax") n,
            lateout("rax") ret,
            out("rcx") _, out("r11") _,
            options(nostack, preserves_flags)
        );
        ret
    }
    
    #[inline(always)]
    pub unsafe fn syscall1(n: usize, a1: usize) -> usize {
        let ret: usize;
        core::arch::asm!(
            "syscall",
            in("rax") n,
            in("rdi") a1,
            lateout("rax") ret,
            out("rcx") _, out("r11") _,
            options(nostack, preserves_flags)
        );
        ret
    }
    
    #[inline(always)]
    pub unsafe fn syscall2(n: usize, a1: usize, a2: usize) -> usize {
        let ret: usize;
        core::arch::asm!(
            "syscall",
            in("rax") n,
            in("rdi") a1,
            in("rsi") a2,
            lateout("rax") ret,
            out("rcx") _, out("r11") _,
            options(nostack, preserves_flags)
        );
        ret
    }
    
    #[inline(always)]
    pub unsafe fn syscall3(n: usize, a1: usize, a2: usize, a3: usize) -> usize {
        let ret: usize;
        core::arch::asm!(
            "syscall",
            in("rax") n,
            in("rdi") a1,
            in("rsi") a2,
            in("rdx") a3,
            lateout("rax") ret,
            out("rcx") _, out("r11") _,
            options(nostack, preserves_flags)
        );
        ret
    }
}

// ============================================================================
// HIGH-LEVEL SYSCALLS
// ============================================================================

/// Beendet den aktuellen Prozess
pub fn exit(status: i32) -> ! {
    unsafe {
        raw::syscall1(0, status as usize);
    }
    loop {} // Sollte nie erreicht werden
}

/// Führt ein Programm aus (ersetzt aktuellen Prozess)
pub fn exec(path: &str, argv: &[&str]) -> Result<(), ()> {
    unsafe {
        // In echtem System: argv zusammenbauen
        let ret = raw::syscall2(1, path.as_ptr() as usize, argv.as_ptr() as usize);
        if ret == 0 {
            Ok(())
        } else {
            Err(())
        }
    }
}

/// Öffnet eine Datei
pub fn open(path: &str, flags: OpenFlags) -> Result<u32, ()> {
    unsafe {
        let ret = raw::syscall2(2, path.as_ptr() as usize, flags.bits());
        if ret < 0xFFFF_FFFF {
            Ok(ret as u32)
        } else {
            Err(())
        }
    }
}

/// Liest aus Datei/FD
pub fn read(fd: u32, buf: &mut [u8]) -> Result<usize, ()> {
    unsafe {
        let ret = raw::syscall3(3, fd as usize, buf.as_mut_ptr() as usize, buf.len());
        if ret != usize::MAX {
            Ok(ret)
        } else {
            Err(())
        }
    }
}

/// Schreibt in Datei/FD
pub fn write(fd: u32, buf: &[u8]) -> Result<usize, ()> {
    unsafe {
        let ret = raw::syscall3(4, fd as usize, buf.as_ptr() as usize, buf.len());
        if ret != usize::MAX {
            Ok(ret)
        } else {
            Err(())
        }
    }
}

/// Schließt eine Datei
pub fn close(_fd: u32) -> Result<(), ()> {
    // Simplified
    Ok(())
}

/// Gibt PID zurück
pub fn getpid() -> u32 {
    unsafe { raw::syscall0(6) as u32 }
}

/// Erstellt neuen Prozess
pub fn fork() -> Result<u32, ()> {
    unsafe {
        let ret = raw::syscall0(7);
        if ret != usize::MAX {
            Ok(ret as u32)
        } else {
            Err(())
        }
    }
}

/// Wartet auf Kind-Prozess
pub fn wait(pid: u32) -> Result<i32, ()> {
    unsafe {
        let ret = raw::syscall1(8, pid as usize);
        if ret != usize::MAX {
            Ok(ret as i32)
        } else {
            Err(())
        }
    }
}

// ============================================================================
// FLAGS
// ============================================================================

#[derive(Clone, Copy)]
pub struct OpenFlags {
    pub bits: usize,
}

impl OpenFlags {
    pub const READ: Self = Self { bits: 0 };
    pub const WRITE: Self = Self { bits: 1 };
    pub const CREATE: Self = Self { bits: 2 };
    pub const TRUNC: Self = Self { bits: 4 };
    
    pub const fn bits(self) -> usize {
        self.bits
    }
}

impl core::ops::BitOr for OpenFlags {
    type Output = Self;
    fn bitor(self, rhs: Self) -> Self::Output {
        Self { bits: self.bits | rhs.bits }
    }
}

// ============================================================================
// STANDARD I/O
// ============================================================================

pub const STDIN: u32 = 0;
pub const STDOUT: u32 = 1;
pub const STDERR: u32 = 2;

/// Print zu stdout
#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => {
        $crate::io::print(format_args!($($arg)*))
    };
}

/// Println zu stdout
#[macro_export]
macro_rules! println {
    () => ($crate::print!("\n"));
    ($($arg:tt)*) => {{
        $crate::print!($($arg)*);
        $crate::print!("\n");
    }};
}

/// Epanic (kein std)
#[macro_export]
macro_rules! panic {
    ($($arg:tt)*) => {{
        $crate::io::eprint(format_args!($($arg)*));
        $crate::exit(1);
    }};
}
