#!/usr/bin/env python3

import os
import re
import sys

# Parse command-line arguments
if len(sys.argv) != 2:
    print("Usage: python cleanup_script.py <file_name>")
    sys.exit(1)

file_name = sys.argv[1]

# Get the directory of the script
script_dir = os.path.dirname(os.path.realpath(__file__))

# Read the states to remove from the file in the same directory as the script
with open(os.path.join(script_dir, 'states_to_remove.txt'), 'r') as f:
    states_to_remove = {line.strip() for line in f if line.strip() and not line.startswith('#')}

# Load the lines from the file
with open(file_name, 'r') as f:
    lines = f.readlines()

# Process each state to remove
for state_to_remove in states_to_remove:
    # Process the lines
    for current_line in range(len(lines)):
        line = lines[current_line]
        # Check if the line contains a state definition
        match = re.search(r'\[(\w+)\]\s*=\s*{', line)
        if match:
            state = match.group(1)
            if state == state_to_remove:
                # Find the end of the state definition
                end_line = current_line
                while '},' not in lines[end_line]:
                    end_line += 1
                # Check if the previous line is a comment
                start_line = current_line - 1
                if start_line >= 0 and re.search(r'/\*.*\*/', lines[start_line]):
                    start_line -= 1
                # Remove the state definition and any preceding comment
                lines = lines[:start_line+1] + lines[end_line+1:]
                break

# Overwrite the original file with the cleaned lines
with open(file_name, 'w') as file:
    file.writelines(lines)