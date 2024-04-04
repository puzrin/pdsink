#!/usr/bin/env python3

import sys
import os
import re

# Check if the correct number of arguments are provided
if len(sys.argv) != 2:
    print("Usage: ./delete_function.py <file_name>")
    sys.exit(1)

# File to modify
file_name = sys.argv[1]

# Get the directory of the script
script_dir = os.path.dirname(os.path.realpath(__file__))

# Function names file
functions_file = os.path.join(script_dir, "functions_to_remove.txt")

# Read the function names, ignoring comments
with open(functions_file, 'r') as file:
    function_names = [line.strip() for line in file if line.strip() and not line.strip().startswith('#')]

# Read the source file
with open(file_name, 'r') as file:
    lines = file.readlines()

# Check if the file is a header file
is_header_file = file_name.endswith('.h')

# For each function name
for function_name in function_names:
    if is_header_file:
        # If it's a header file, remove function declarations
        lines = [line for line in lines if not line.strip().startswith(function_name)]
    else:
        # If it's a source file, remove function definitions
        start_line = end_line = None
        braces_count = 0
        for i, line in enumerate(lines):
            # Use a regular expression to match the function name along with its parameters and possible whitespace before the opening brace
            if re.search(r'\b' + re.escape(function_name) + r'\b\s*\(', line):
                start_line = i
            if start_line is not None and i >= start_line:
                braces_count += line.count('{')
                braces_count -= line.count('}')
                if braces_count == 0 and start_line != i:  # Ensure we're not on the same line where we started
                    end_line = i
                    break
        # If the function is found, delete it
        if start_line is not None and end_line is not None:
            # Backtrack to remove comments
            while start_line > 0 and (lines[start_line - 1].strip().startswith('/*') or lines[start_line - 1].strip().startswith('*') or lines[start_line - 1].strip().startswith('*/')):
                start_line -= 1
            del lines[start_line:end_line+1]

# Write the file
with open(file_name, 'w') as file:
    file.writelines(lines)
