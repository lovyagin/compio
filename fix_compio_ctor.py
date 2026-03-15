import sys

with open("src/compio.cpp", "r") as f:
    lines = f.readlines()

# Fix lines 103-106 in compio_archive constructor
# Original:
#                  ? smart_infile_object<compio::header>(file, 0,
#                                                        new compio::header(static_cast<uint32_t>(config->max_files)),
#                                                        &io_mutex)

# New:
#                  ? smart_infile_object<header>(file, 0,
#                                                        new header(static_cast<uint32_t>(config->max_files)),
#                                                        &io_mutex)

changed = False
for i, line in enumerate(lines):
    if "smart_infile_object<compio::header>" in line:
        lines[i] = line.replace("compio::header", "header")
        changed = True
        
    if "new compio::header" in line:
        lines[i] = line.replace("compio::header", "header")
        changed = True

if changed:
    with open("src/compio.cpp", "w") as f:
        f.writelines(lines)
    print("Fixed src/compio.cpp constructor")
else:
    print("No changes needed")
