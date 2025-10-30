# Compio Documentation

This directory contains two versions of the project documentation:

## Public Documentation (`docs/public/html/`)

**Target audience**: End users and library consumers

**Contains**:
- Public API reference (C and C++ interfaces)
- Usage examples
- Architecture overview

**To build**:
```bash
doxygen Doxyfile
```

**To view**: Open `docs/public/html/index.html` in a web browser

---

## Developer Documentation (`docs/dev/html/`)

**Target audience**: Project developers and contributors

**Contains**:
- Complete internal API documentation
- Implementation details
- Private classes and methods
- Source code with full annotations

**To build**:
```bash
doxygen Doxyfile_Dev
```

**To view**: Open `docs/dev/html/index.html` in a web browser

---

## Building Both Versions

### Quick Build (Recommended)

Use the provided script to build both versions at once:

```bash
./docs/build_docs.sh
```

This script automatically:
- Builds public documentation
- Builds developer documentation  
- Shows any warnings from both builds
- Can be run from project root or docs directory

---

## Requirements

- Doxygen 1.12.0 or higher

## Notes

- The public documentation focuses on the API that library users need
- The developer documentation includes all internal implementation details
- Both versions use the same source files but with different extraction settings

