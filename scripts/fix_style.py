#!/usr/bin/env python3

import subprocess
import glob
import os

def format_files():
    files_to_format = []
    extensions = ["*.cpp", "*.c", "*.h", "*.glsl"]
    allowed_dirs = ["scripts", "plugins", "src", "tests", "games"]
    excluded_dirs = {"ForeverValidator"}
    for base_dir in allowed_dirs:
        for ext in extensions:
            pattern = os.path.join(base_dir, "**", ext)
            files_to_format.extend(glob.glob(pattern, recursive=True))
    for file in files_to_format:
        path_parts = set(os.path.normpath(file).split(os.sep))
        if path_parts & excluded_dirs:
            continue
            
        print(f"Formatting {file}...")
        subprocess.run(["clang-format", "-i", file])

if __name__ == "__main__":
    format_files()