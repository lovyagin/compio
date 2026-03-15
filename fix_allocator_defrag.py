import sys

with open("src/allocator.cpp", "r") as f:
    lines = f.readlines()

# Fix perform_defragmentation to start after reserved area
# Look for: uint64_t write_pos = readonly(archive_->header, header)->disk_size();
# Replace with: uint64_t write_pos = readonly(archive_->header, header)->disk_size() * 2;

changed = False
for i, line in enumerate(lines):
    if "uint64_t write_pos = readonly(archive_->header, header)->disk_size();" in line:
        lines[i] = line.replace("disk_size();", "disk_size() * 2;")
        changed = True

if changed:
    with open("src/allocator.cpp", "w") as f:
        f.writelines(lines)
    print("Fixed src/allocator.cpp defragmentation start pos")
else:
    print("No changes needed or line not found")
