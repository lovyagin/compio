#!/bin/bash
# Build both public and developer documentation

set -e

# Go to project root if script is called from docs/
if [ -f "Doxyfile" ]; then
    PROJECT_ROOT="."
else
    PROJECT_ROOT=".."
fi

cd "$PROJECT_ROOT"

echo "======================================"
echo "Building Compio Documentation"
echo "======================================"
echo ""

# Build public documentation
echo "Building public documentation..."
doxygen Doxyfile 2>&1 | grep -i "warning" || echo "✓ Public documentation built successfully (no warnings)"
echo ""

# Build developer documentation
echo "Building developer documentation..."
doxygen Doxyfile_Dev 2>&1 | grep -i "warning" || echo "✓ Developer documentation built successfully"
echo ""

echo "======================================"
echo "Documentation Build Complete"
echo "======================================"
echo ""
echo "Public documentation: docs/public/html/index.html"
echo "Developer documentation: docs/dev/html/index.html"
echo ""

