import sys

with open("src/compio.cpp", "r") as f:
    lines = f.readlines()

# Find the lines to fix
# 1. clear_temporary_index -> invalidate_temporary_index
# 2. compio::smart_infile_object -> smart_infile_object
# 3. compio::header -> header (optional, but cleaner if already using namespace)

changed = False
for i, line in enumerate(lines):
    if "archive->block_reader->clear_temporary_index();" in line:
        lines[i] = line.replace("clear_temporary_index", "invalidate_temporary_index")
        changed = True
    
    if "compio::smart_infile_object<compio::header>" in line:
        lines[i] = line.replace("compio::smart_infile_object<compio::header>", "smart_infile_object<header>")
        changed = True
    
    if "new compio::header" in line:
        lines[i] = line.replace("new compio::header", "new header")
        changed = True

if changed:
    with open("src/compio.cpp", "w") as f:
        f.writelines(lines)
    print("Fixed src/compio.cpp")
else:
    print("No changes needed")
