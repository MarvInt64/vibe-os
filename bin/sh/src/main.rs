//! sh - Die VibeOS Shell
//!
//! Kommandos:
//! - ls [pfad]
//! - editor [datei]
//! - exec /pfad/zu/programm
//! - exit
//! - help

#![no_std]
#![no_main]

use libvex::io::read_line;
use libvex::println;
use libvex::string::{to_str, trim};
use libvex::OpenFlags;
use libvex::{close, exec, exit, fork, getpid, open, wait};

const PROMPT: &str = "$ ";
const BUF_SIZE: usize = 256;

#[no_mangle]
pub extern "C" fn _start() -> ! {
    let mut buf = [0u8; BUF_SIZE];

    println!("VibeOS Shell (PID {})", getpid());
    println!("Type 'help' for commands");

    loop {
        // Prompt
        print_prompt();

        // Input lesen
        let n = match read_line(&mut buf) {
            Ok(n) => n,
            Err(_) => continue,
        };

        if n == 0 {
            continue;
        }

        // Parsen
        let input = trim(&buf[..n]);
        if input.is_empty() {
            continue;
        }

        // Kommando ausführen
        execute(input);
    }
}

fn print_prompt() {
    libvex::print!("{}", PROMPT);
}

fn execute(input: &[u8]) {
    // Erstes Wort = Kommando
    let parts = split_args(input);
    if parts.is_empty() {
        return;
    }

    let cmd = to_str(parts[0]);
    let args = &parts[1..];

    match cmd {
        "exit" => exit(0),
        "help" => print_help(),
        "ls" => {
            let path = if args.is_empty() {
                "."
            } else {
                to_str(args[0])
            };
            run_program("/bin/ls", &[path]);
        }
        "editor" => {
            let file = if args.is_empty() {
                "newfile.txt"
            } else {
                to_str(args[0])
            };
            run_program("/bin/editor", &[file]);
        }
        "exec" => {
            if args.is_empty() {
                println!("exec: missing operand");
                return;
            }
            let prog = to_str(args[0]);
            run_program(prog, &[]);
        }
        "./" | "." => {
            // Lokales Programm ausführen
            if cmd.len() > 2 {
                let prog = &cmd[2..];
                run_program(prog, &[]);
            } else {
                println!("sh: {}: not found", cmd);
            }
        }
        _ => {
            // Versuche als Programm in /bin
            let mut full_path = [0u8; 128];
            let bin_prefix = b"/bin/";
            full_path[..bin_prefix.len()].copy_from_slice(bin_prefix);
            let cmd_bytes = cmd.as_bytes();
            let len = cmd_bytes.len().min(122);
            full_path[bin_prefix.len()..bin_prefix.len() + len].copy_from_slice(&cmd_bytes[..len]);

            run_program(to_str(&full_path), &[]);
        }
    }
}

fn run_program(path: &str, args: &[&str]) {
    match fork() {
        Ok(0) => {
            // Kind: Programm laden
            let _ = exec(path, args);
            println!("sh: {}: not found", path);
            exit(127);
        }
        Ok(pid) => {
            // Eltern: Warten
            match wait(pid) {
                Ok(status) => {
                    if status != 0 {
                        println!("sh: {}: exited with {}", path, status);
                    }
                }
                Err(_) => {
                    println!("sh: wait failed");
                }
            }
        }
        Err(_) => {
            println!("sh: fork failed");
        }
    }
}

fn split_args(input: &[u8]) -> [&[u8]; 16] {
    let mut args: [&[u8]; 16] = [&[]; 16];
    let mut arg_count = 0;
    let mut i = 0;

    while i < input.len() && arg_count < 16 {
        // Skip spaces
        while i < input.len() && input[i] == b' ' {
            i += 1;
        }
        if i >= input.len() {
            break;
        }

        // Argument start
        let start = i;
        while i < input.len() && input[i] != b' ' {
            i += 1;
        }

        args[arg_count] = &input[start..i];
        arg_count += 1;
    }

    args
}

fn print_help() {
    println!("VibeOS Shell Commands:");
    println!("  ls [path]        - List directory contents");
    println!("  editor [file]    - Open text editor");
    println!("  exec /path/prog  - Execute program");
    println!("  help             - Show this help");
    println!("  exit             - Exit shell");
    println!("");
    println!("  <command>        - Execute /bin/<command>");
    println!("  ./program        - Execute local program");
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
