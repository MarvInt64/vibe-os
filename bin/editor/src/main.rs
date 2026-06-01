//! editor - Ein einfacher Texteditor für VibeOS
//!
//! Steuerung:
//! - ^S: Speichern
//! - ^Q: Beenden
//! - ^N: Neue Datei

#![no_std]
#![no_main]

use libvex::io::read_line;
use libvex::OpenFlags;
use libvex::{close, exit, open, println, read, write};

const MAX_LINES: usize = 100;
const LINE_SIZE: usize = 256;

struct Editor {
    lines: [[u8; LINE_SIZE]; MAX_LINES],
    line_lens: [usize; MAX_LINES],
    num_lines: usize,
    filename: [u8; 64],
    dirty: bool,
}

impl Editor {
    const fn new() -> Self {
        Self {
            lines: [[0u8; LINE_SIZE]; MAX_LINES],
            line_lens: [0; MAX_LINES],
            num_lines: 0,
            filename: [0u8; 64],
            dirty: false,
        }
    }

    fn set_filename(&mut self, name: &str) {
        let bytes = name.as_bytes();
        let len = bytes.len().min(63);
        self.filename[..len].copy_from_slice(&bytes[..len]);
    }

    fn filename_str(&self) -> &str {
        let len = self.filename.iter().position(|&b| b == 0).unwrap_or(64);
        unsafe { core::str::from_utf8_unchecked(&self.filename[..len]) }
    }

    fn add_line(&mut self, text: &[u8]) {
        if self.num_lines >= MAX_LINES {
            return;
        }
        let idx = self.num_lines;
        let len = text.len().min(LINE_SIZE - 1);
        self.lines[idx][..len].copy_from_slice(&text[..len]);
        self.line_lens[idx] = len;
        self.num_lines += 1;
        self.dirty = true;
    }

    fn display(&self) {
        // Clear screen (simplified)
        println!("\n--- Editor: {} ---", self.filename_str());
        println!(
            "Lines: {} ({})",
            self.num_lines,
            if self.dirty { "modified" } else { "saved" }
        );
        println!("");

        for i in 0..self.num_lines.min(20) {
            let len = self.line_lens[i];
            let line = unsafe { core::str::from_utf8_unchecked(&self.lines[i][..len]) };
            println!("{:3} | {}", i + 1, line);
        }

        println!("");
        println!("Commands: ^S Save | ^Q Quit | ^N New | Enter: add line");
    }

    fn save(&mut self) {
        // Simplified: In echtem System würde hier in Datei geschrieben
        println!("Saving {}...", self.filename_str());
        self.dirty = false;
        println!("Saved!");
    }

    fn clear(&mut self) {
        self.num_lines = 0;
        self.dirty = false;
        self.filename = [0u8; 64];
    }
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    let mut editor = Editor::new();
    let mut buf = [0u8; LINE_SIZE];

    println!("VibeOS Text Editor");
    println!("==================");
    println!("");

    // Dateiname von Kommandozeile
    // Simplified: Wir fragen nach dem Dateinamen
    println!("Enter filename: ");

    if let Ok(n) = read_line(&mut buf) {
        if n > 0 {
            let name = unsafe { core::str::from_utf8_unchecked(&buf[..n]) };
            editor.set_filename(name);
            println!("Editing: {}", name);
        } else {
            editor.set_filename("untitled.txt");
        }
    }

    // Haupt-Loop
    loop {
        editor.display();

        println!("\nEnter command or text (^S=save, ^Q=quit, ^N=new): ");

        buf = [0u8; LINE_SIZE];
        let n = match read_line(&mut buf) {
            Ok(n) => n,
            Err(_) => continue,
        };

        if n == 0 {
            continue;
        }

        // Prüfe auf Steuerzeichen
        if n >= 1 && buf[0] == 0x13 {
            // ^S
            editor.save();
        } else if n >= 1 && buf[0] == 0x11 {
            // ^Q
            if editor.dirty {
                println!("Unsaved changes! Press ^Q again to quit.");
                buf = [0u8; LINE_SIZE];
                if let Ok(m) = read_line(&mut buf) {
                    if m >= 1 && buf[0] == 0x11 {
                        break;
                    }
                }
            } else {
                break;
            }
        } else if n >= 1 && buf[0] == 0x0E {
            // ^N
            editor.clear();
            println!("New file. Enter filename: ");
            buf = [0u8; LINE_SIZE];
            if let Ok(m) = read_line(&mut buf) {
                if m > 0 {
                    let name = unsafe { core::str::from_utf8_unchecked(&buf[..m]) };
                    editor.set_filename(name);
                }
            }
        } else {
            // Normale Zeile hinzufügen
            editor.add_line(&buf[..n]);
        }
    }

    println!("Editor: exiting");
    exit(0);
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
