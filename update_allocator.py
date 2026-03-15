import sys

with open("src/allocator.cpp", "r") as f:
    lines = f.readlines()

new_block = """    // Ensure we reserve space for double-buffered header
    uint64_t reserved_size = readonly(archive_->header, header)->disk_size() * 2;
    if (readonly(archive_->header, header)->file_size < reserved_size) {
        archive_->header->file_size = reserved_size;
    }
"""

start_idx = -1
for i, line in enumerate(lines):
    if "block_allocator::block_allocator(compio_archive *archive)" in line:
        start_idx = i
        break

if start_idx != -1:
    # Look for the if block
    # It was around line 662 in original file.
    # relative to constructor start (line 646)
    
    # scan for "Ensure we have valid initial file size"
    found = False
    for i in range(start_idx, len(lines)):
        if "Ensure we have valid initial file size" in lines[i]:
            # Found comment. Replace next 3 lines?
            # 662: if ...
            # 663:     ...
            # 664: }
            
            # Check context
            if "if (readonly" in lines[i+1] and "disk_size()) {" in lines[i+1]:
                # Remove 4 lines (comment + if + body + brace)
                del lines[i+3]
                del lines[i+2]
                del lines[i+1]
                del lines[i]
                
                # Insert new block
                lines.insert(i, new_block)
                
                with open("src/allocator.cpp", "w") as f:
                    f.writelines(lines)
                print("Updated src/allocator.cpp successfully")
                found = True
                break
    if not found:
        print("Block not found")
else:
    print("Constructor not found")
