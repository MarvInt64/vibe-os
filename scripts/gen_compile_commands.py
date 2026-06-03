#!/usr/bin/env python3
import json
import os
import subprocess
import re

def main():
    print("Generating compile_commands.json...")
    # Get current working directory
    cwd = os.getcwd()
    
    # Run make -n -B to dry run all build commands
    try:
        result = subprocess.run(["make", "-n", "-B"], capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as e:
        print(f"Error running make -n -B: {e}")
        return

    commands = []
    lines = result.stdout.splitlines()
    
    for line in lines:
        line = line.strip()
        # Find compile commands starting with clang or clang++ containing "-c"
        if ("clang" in line or "clang++" in line) and " -c " in line:
            # Match the source file name (.c, .cpp, .S, .s)
            match = re.search(r'\s+([^\s]+\.(?:c|cpp|S|s))\b', line)
            if match:
                file_path = match.group(1)
                commands.append({
                    "directory": cwd,
                    "command": line,
                    "file": file_path
                })
                
    # Write to compile_commands.json
    output_path = os.path.join(cwd, "compile_commands.json")
    with open(output_path, "w") as f:
        json.dump(commands, f, indent=2)
        
    print(f"Successfully generated {len(commands)} entries in compile_commands.json")

if __name__ == "__main__":
    main()
