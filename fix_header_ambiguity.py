import sys

with open("src/compio.cpp", "r") as f:
    lines = f.readlines()

# Fix header ambiguity
# smart_infile_object<header> -> smart_infile_object<compio::header>
# new header -> new compio::header

changed = False
for i, line in enumerate(lines):
    # Fix constructor usages
    if "smart_infile_object<header>" in line:
        lines[i] = line.replace("smart_infile_object<header>", "smart_infile_object<compio::header>")
        changed = True
        
    if "new header" in line:
        lines[i] = line.replace("new header", "new compio::header")
        changed = True

if changed:
    with open("src/compio.cpp", "w") as f:
        f.writelines(lines)
    print("Fixed header ambiguity in src/compio.cpp")
else:
    print("No changes needed")
