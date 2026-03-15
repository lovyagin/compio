import sys

with open("src/compio.cpp", "r") as f:
    lines = f.readlines()

# Indentation: 4 spaces
new_block = """    if (archive->block_reader) archive->block_reader->clear_cache();
    if (archive->block_reader) archive->block_reader->clear_temporary_index();
    if (archive->index) archive->index->clear_cache();

    // Save allocator state (updates header fields)
    if (archive->allocator && !(archive->mode_b & mode_bit::r)) {
         archive->allocator->save_state(archive);
    }
    
    // Double-buffered Header Write
    if (!(archive->mode_b & mode_bit::r)) {
        int target_slot = 1 - archive->current_header_slot;
        uint64_t target_addr = (target_slot == 0) ? 0 : archive->header->disk_size();
        
        compio::header* new_h_data = new compio::header(*archive->header);
        new_h_data->sequence_id++;
        
        compio::smart_infile_object<compio::header> new_header_obj(archive->file, target_addr, new_h_data, &archive->io_mutex, true);
        new_header_obj.write();
        
        archive->current_header_slot = target_slot;
        archive->header.unmodify();
        archive->header = std::move(new_header_obj);
    }

    if (fflush(archive->file)) {
        WARNING_PRINT("warning: fflush failed\\n");
    }
"""

start_idx = -1
for i, line in enumerate(lines):
    if "void compio_flush(compio_archive *archive) {" in line:
        start_idx = i
        break

if start_idx != -1:
    # 1095: void compio_flush
    # 1096: if (!archive) return;
    # 1097: lock...
    # 1098: block_reader clear_cache
    # 1099: index clear_cache
    # 1100: }
    
    # We want to replace lines 1098 and 1099.
    # Note: lines list is 0-indexed.
    # start_idx is 1095.
    # 1096 is start_idx + 1
    # 1097 is start_idx + 2
    # 1098 is start_idx + 3
    
    idx_block_reader = start_idx + 3
    idx_index = start_idx + 4
    
    if "block_reader->clear_cache" in lines[idx_block_reader] and "index->clear_cache" in lines[idx_index]:
        # Delete index line first (higher index)
        del lines[idx_index]
        # Delete block_reader line
        del lines[idx_block_reader]
        
        # Insert new block
        lines.insert(idx_block_reader, new_block)
        
        with open("src/compio.cpp", "w") as f:
            f.writelines(lines)
        print("Updated src/compio.cpp successfully")
    else:
        print("Context mismatch")
        print(f"Line {idx_block_reader}: {lines[idx_block_reader]}")
        print(f"Line {idx_index}: {lines[idx_index]}")
else:
    print("Function not found")
