//! VibeOS Kernel - Minimaler Kernel mit exec-Syscall
//!
//! Architektur:
//! - VFS: Virtuelles Dateisystem (in-memory)
//! - Process Manager: Verwaltet Prozesse
//! - Syscall Handler: Systemaufrufe (exec, exit, read, write, open)

#![no_std]
#![no_main]

use core::mem::size_of;
use core::panic::PanicInfo;

// ============================================================================
// TYPEN UND KONSTANTEN
// ============================================================================

const MAX_PROCESSES: usize = 32;
const MAX_FILES: usize = 256;
const MAX_FDS: usize = 16;
const PAGE_SIZE: usize = 4096;

pub type SyscallResult = Result<usize, SyscallError>;

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum SyscallError {
    NotFound,
    NoMemory,
    InvalidArgument,
    PermissionDenied,
    WouldBlock,
    IoError,
}

// ============================================================================
// DATEISYSTEM (VFS)
// ============================================================================

pub struct Vfs {
    files: [Option<VfsFile>; MAX_FILES],
    next_inode: u64,
}

#[derive(Clone)]
pub struct VfsFile {
    pub inode: u64,
    pub name: [u8; 64],
    pub data: [u8; 8192], // 8KB max pro Datei
    pub size: usize,
    pub executable: bool,
}

impl VfsFile {
    pub fn new(inode: u64, name: &str, executable: bool) -> Self {
        let mut n = [0u8; 64];
        let bytes = name.as_bytes();
        let len = bytes.len().min(63);
        n[..len].copy_from_slice(&bytes[..len]);

        Self {
            inode,
            name: n,
            data: [0u8; 8192],
            size: 0,
            executable,
        }
    }

    pub fn name_str(&self) -> &str {
        let len = self.name.iter().position(|&b| b == 0).unwrap_or(64);
        core::str::from_utf8(&self.name[..len]).unwrap_or("???")
    }

    pub fn write(&mut self, data: &[u8]) {
        let len = data.len().min(self.data.len());
        self.data[..len].copy_from_slice(&data[..len]);
        self.size = len;
    }
}

impl Vfs {
    pub const fn new() -> Self {
        Self {
            files: [const { None }; MAX_FILES],
            next_inode: 1,
        }
    }

    pub fn create(&mut self, name: &str, executable: bool) -> Option<&mut VfsFile> {
        for i in 0..MAX_FILES {
            if self.files[i].is_none() {
                let inode = self.next_inode;
                self.next_inode += 1;
                self.files[i] = Some(VfsFile::new(inode, name, executable));
                return self.files[i].as_mut();
            }
        }
        None
    }

    pub fn lookup(&self, path: &str) -> Option<&VfsFile> {
        let filename = path.strip_prefix('/').unwrap_or(path);
        for i in 0..MAX_FILES {
            if let Some(ref f) = self.files[i] {
                if f.name_str() == filename {
                    return Some(f);
                }
            }
        }
        None
    }

    pub fn lookup_mut(&mut self, path: &str) -> Option<&mut VfsFile> {
        let filename = path.strip_prefix('/').unwrap_or(path);
        for i in 0..MAX_FILES {
            if let Some(ref mut f) = self.files[i] {
                if f.name_str() == filename {
                    return Some(f);
                }
            }
        }
        None
    }

    pub fn ls(&self) -> impl Iterator<Item = &VfsFile> {
        self.files.iter().filter_map(|f| f.as_ref())
    }
}

// ============================================================================
// ELF LOADER
// ============================================================================

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ElfHeader {
    pub magic: [u8; 4], // 0x7F, 'E', 'L', 'F'
    pub class: u8,      // 1 = 32-bit, 2 = 64-bit
    pub data: u8,       // 1 = little endian
    pub version: u8,
    pub osabi: u8,
    pub abiversion: u8,
    pub pad: [u8; 7],
    pub type_: u16,   // 1 = relocatable, 2 = executable, 3 = shared
    pub machine: u16, // 0x3E = x86_64
    pub version2: u32,
    pub entry: u64, // Entry point
    pub phoff: u64, // Program header offset
    pub shoff: u64, // Section header offset
    pub flags: u32,
    pub ehsize: u16,    // ELF header size
    pub phentsize: u16, // Program header entry size
    pub phnum: u16,     // Number of program headers
    pub shentsize: u16, // Section header entry size
    pub shnum: u16,     // Number of section headers
    pub shstrndx: u16,  // Section name string table index
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ProgramHeader {
    pub type_: u32, // 1 = LOAD
    pub flags: u32,
    pub offset: u64,
    pub vaddr: u64,
    pub paddr: u64,
    pub filesz: u64,
    pub memsz: u64,
    pub align: u64,
}

pub struct ElfLoader;

impl ElfLoader {
    pub fn is_valid_elf(data: &[u8]) -> bool {
        data.len() >= 4 && data[0] == 0x7F && data[1] == b'E' && data[2] == b'L' && data[3] == b'F'
    }

    pub fn load(data: &[u8]) -> Option<(u64, usize)> {
        if !Self::is_valid_elf(data) {
            return None;
        }

        let header = unsafe { &*(data.as_ptr() as *const ElfHeader) };

        if header.type_ != 2 {
            // 2 = executable
            return None;
        }

        // Entry point zurückgeben
        Some((header.entry, data.len()))
    }
}

// ============================================================================
// PROCESS MANAGER
// ============================================================================

pub const STACK_SIZE: usize = 8192;

#[derive(Clone, Copy, PartialEq)]
pub enum ProcState {
    Empty,
    Running,
    Zombie,
}

pub struct Process {
    pub pid: u32,
    pub state: ProcState,
    pub entry_point: u64,
    pub stack: [u8; STACK_SIZE],
    pub stack_ptr: usize,
    pub name: [u8; 64],
    pub working_dir: [u8; 256],
    pub fds: [Option<u32>; MAX_FDS],
}

impl Process {
    pub const fn empty() -> Self {
        Self {
            pid: 0,
            state: ProcState::Empty,
            entry_point: 0,
            stack: [0u8; STACK_SIZE],
            stack_ptr: 0,
            name: [0u8; 64],
            working_dir: [0u8; 256],
            fds: [None; MAX_FDS],
        }
    }

    pub fn name_str(&self) -> &str {
        let len = self.name.iter().position(|&b| b == 0).unwrap_or(64);
        core::str::from_utf8(&self.name[..len]).unwrap_or("???")
    }
}

pub struct ProcessManager {
    processes: [Process; MAX_PROCESSES],
    current_pid: u32,
}

impl ProcessManager {
    pub const fn new() -> Self {
        Self {
            processes: [const { Process::empty() }; MAX_PROCESSES],
            current_pid: 1,
        }
    }

    pub fn spawn(&mut self, entry: u64, name: &str) -> Option<u32> {
        for i in 0..MAX_PROCESSES {
            if self.processes[i].state == ProcState::Empty {
                let pid = self.current_pid;
                self.current_pid += 1;

                self.processes[i].pid = pid;
                self.processes[i].state = ProcState::Running;
                self.processes[i].entry_point = entry;
                self.processes[i].stack_ptr = STACK_SIZE;

                // Name setzen
                let bytes = name.as_bytes();
                let len = bytes.len().min(63);
                self.processes[i].name[..len].copy_from_slice(&bytes[..len]);

                // Standard working dir = /home
                let wd = b"/home";
                self.processes[i].working_dir[..wd.len()].copy_from_slice(wd);

                return Some(pid);
            }
        }
        None
    }

    pub fn current(&self) -> &Process {
        for i in 0..MAX_PROCESSES {
            if self.processes[i].state == ProcState::Running {
                return &self.processes[i];
            }
        }
        &self.processes[0]
    }

    pub fn current_mut(&mut self) -> &mut Process {
        for i in 0..MAX_PROCESSES {
            if self.processes[i].state == ProcState::Running {
                return &mut self.processes[i];
            }
        }
        &mut self.processes[0]
    }

    pub fn find(&self, pid: u32) -> Option<&Process> {
        self.processes
            .iter()
            .find(|p| p.pid == pid && p.state != ProcState::Empty)
    }

    pub fn exit(&mut self, pid: u32) {
        for i in 0..MAX_PROCESSES {
            if self.processes[i].pid == pid {
                self.processes[i].state = ProcState::Zombie;
                break;
            }
        }
    }
}

// ============================================================================
// SYSCALLS
// ============================================================================

pub enum Syscall {
    Exit = 0,
    Exec = 1,
    Open = 2,
    Read = 3,
    Write = 4,
    Close = 5,
    Getpid = 6,
    Fork = 7,
    Wait = 8,
}

pub struct Kernel {
    pub vfs: Vfs,
    pub proc: ProcessManager,
}

impl Kernel {
    pub const fn new() -> Self {
        Self {
            vfs: Vfs::new(),
            proc: ProcessManager::new(),
        }
    }

    /// exec(path: *const u8) -> Result<pid, Error>
    /// Lädt ein Programm vom Dateisystem und führt es aus
    pub fn sys_exec(&mut self, path_ptr: usize, _argv_ptr: usize) -> SyscallResult {
        // Path aus Userspace lesen (simplified)
        let path = self.read_user_string(path_ptr)?;

        // Datei im VFS suchen
        let file = self.vfs.lookup(path).ok_or(SyscallError::NotFound)?;

        if !file.executable {
            return Err(SyscallError::PermissionDenied);
        }

        // ELF laden
        let data = &file.data[..file.size];
        let (entry, _size) = ElfLoader::load(data).ok_or(SyscallError::InvalidArgument)?;

        // Neuen Prozess spawnen
        let pid = self
            .proc
            .spawn(entry, file.name_str())
            .ok_or(SyscallError::NoMemory)?;

        Ok(pid as usize)
    }

    pub fn sys_exit(&mut self, status: usize) -> SyscallResult {
        let current = self.proc.current().pid;
        self.proc.exit(current);
        Ok(status)
    }

    pub fn sys_getpid(&self) -> SyscallResult {
        Ok(self.proc.current().pid as usize)
    }

    pub fn sys_write(&mut self, fd: usize, buf: usize, count: usize) -> SyscallResult {
        if fd == 1 || fd == 2 {
            // stdout / stderr
            let data = unsafe { core::slice::from_raw_parts(buf as *const u8, count) };
            // In echtem Kernel: an Console ausgeben
            // Hier: einfach nur OK zurückgeben
            return Ok(count);
        }
        Err(SyscallError::InvalidArgument)
    }

    /// read(fd, buf, count) -> bytes_read
    pub fn sys_read(&mut self, fd: usize, buf: usize, count: usize) -> SyscallResult {
        if fd == 0 {
            // stdin
            // In echtem Kernel: vom Tastaturpuffer lesen
            return Ok(0); // EOF für jetzt
        }
        Err(SyscallError::InvalidArgument)
    }

    pub fn sys_open(&mut self, path_ptr: usize, _flags: usize) -> SyscallResult {
        let path = self.read_user_string(path_ptr)?;

        // Prüfen ob Datei existiert
        let _file = self.vfs.lookup(path).ok_or(SyscallError::NotFound)?;

        // FD zuweisen (simplified)
        Ok(3) // Erster nicht-Standard-FD
    }

    fn read_user_string(&self, ptr: usize) -> Result<&str, SyscallError> {
        if ptr == 0 {
            return Err(SyscallError::InvalidArgument);
        }

        // Simplified: wir gehen davon aus, dass der String null-terminiert ist
        // und im "Userspace" liegt
        let cstr = unsafe {
            let mut len = 0;
            while *((ptr + len) as *const u8) != 0 {
                len += 1;
            }
            core::slice::from_raw_parts(ptr as *const u8, len)
        };

        core::str::from_utf8(cstr).map_err(|_| SyscallError::InvalidArgument)
    }
}

// ============================================================================
// INITIALISIERUNG UND MAIN
// ============================================================================

static mut KERNEL: Kernel = Kernel::new();

/// Initialisiert das Dateisystem mit Standard-Programmen
fn init_fs(kernel: &mut Kernel) {
    // /bin/init - der erste Prozess
    // In echtem System würde das vom echten FS geladen
    // Hier: Platzhalter für init
    if let Some(init) = kernel.vfs.create("/bin/init", true) {
        // ELF Header für init
        let elf = create_minimal_elf(0x1000);
        init.write(&elf);
    }

    // /bin/sh - die Shell
    if let Some(sh) = kernel.vfs.create("/bin/sh", true) {
        let elf = create_minimal_elf(0x2000);
        sh.write(&elf);
    }

    // /bin/editor - der Editor
    if let Some(ed) = kernel.vfs.create("/bin/editor", true) {
        let elf = create_minimal_elf(0x3000);
        ed.write(&elf);
    }

    // /bin/ls
    if let Some(ls) = kernel.vfs.create("/bin/ls", true) {
        let elf = create_minimal_elf(0x4000);
        ls.write(&elf);
    }
}

/// Erstellt einen minimalen ELF-Header für User-Programme
fn create_minimal_elf(entry_point: u64) -> Vec<u8> {
    // Simplified: In echtem System wäre das echte ELF-Binary
    let mut data = vec![0u8; 128];

    // ELF Magic
    data[0] = 0x7F;
    data[1] = b'E';
    data[2] = b'L';
    data[3] = b'F';

    // 64-bit, little endian
    data[4] = 2;
    data[5] = 1;
    data[6] = 1; // ELF version

    // Type: executable
    data[16] = 0x02;
    data[17] = 0x00;

    // Machine: x86_64
    data[18] = 0x3E;
    data[19] = 0x00;

    // Entry point (u64 at offset 24)
    let entry_bytes = entry_point.to_le_bytes();
    data[24..32].copy_from_slice(&entry_bytes);

    // Program header offset
    data[32..40].copy_from_slice(&64u64.to_le_bytes());

    // ELF header size
    data[40..42].copy_from_slice(&64u16.to_le_bytes());

    // Program header entry size
    data[42..44].copy_from_slice(&56u16.to_le_bytes());

    // Number of program headers
    data[44..46].copy_from_slice(&1u16.to_le_bytes());

    data
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    unsafe {
        init_fs(&mut KERNEL);

        // Starte init als ersten Prozess
        let init_pid = KERNEL
            .sys_exec("/bin/init" as *const u8 as usize, 0)
            .expect("Failed to start init");

        // Kernel-Loop
        loop {
            // In echtem System: Scheduler, IRQs, etc.
            // Hier: einfacher Loop

            let current = KERNEL.proc.current();
            if current.state == ProcState::Running {
                // Simuliere Programmausführung
                // In echtem Kernel würde hier der Context-Switch passieren
            }
        }
    }
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}
