#!/usr/bin/env python3
import re
import sys

def main():
    if len(sys.argv) < 3:
        print("Usage: ./eval_IS_ENABLED_macro.py <c_file_path> <h_file_path>")
        sys.exit(1)

    c_file_path = sys.argv[1]
    h_file_path = sys.argv[2]

    # Read the .h file and find all defined and undefined variables
    with open(h_file_path, 'r') as f:
        h_file = f.read()
    defined_variables = re.findall(r'#define (\w+)', h_file)
    undefined_variables = re.findall(r'#undef (\w+)', h_file)

    # Create a dictionary of variables and their values
    variables = {var: '1' for var in defined_variables}
    variables.update({var: '0' for var in undefined_variables})

    # Read the .c file and replace IS_ENABLED(VARIABLE) with value/*IS_ENABLED(VARIABLE)*/
    with open(c_file_path, 'r') as f:
        c_file = f.read()
    for variable, value in variables.items():
        c_file = re.sub(r'IS_ENABLED\(' + variable + r'\)', value + '/*IS_ENABLED(' + variable + ')*/', c_file)

    # Write the modified .c file back to disk
    with open(c_file_path, 'w') as f:
        f.write(c_file)

if __name__ == "__main__":
    main()