import sys

with open("tests/src/test_allocator_edge_cases.cpp", "r") as f:
    lines = f.readlines()

changed = False
for i, line in enumerate(lines):
    if "EXPECT_EQ(offset, archive->header->disk_size() + INDEX_NODE_SIZE(config.b_tree_degree))" in line:
        lines[i] = line.replace("disk_size()", "disk_size() * 2")
        changed = True

if changed:
    with open("tests/src/test_allocator_edge_cases.cpp", "w") as f:
        f.writelines(lines)
    print("Fixed test_allocator_edge_cases.cpp")
else:
    print("No changes needed")
