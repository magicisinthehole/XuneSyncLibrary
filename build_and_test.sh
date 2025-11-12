#!/bin/bash

# Zune Device Library JSON Export - Build and Test Script

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
OUTPUT_FILE="${SCRIPT_DIR}/zune_library.json"

echo "================================"
echo "  Zune Library Export Builder"
echo "================================"
echo ""

# Check if build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo "Creating build directory..."
    mkdir -p "$BUILD_DIR"
else
    echo "Build directory already exists"
fi

echo ""
echo "Configuring project..."
cd "$BUILD_DIR"

# Check if we need to reconfigure
if [ ! -f "CMakeCache.txt" ]; then
    cmake ..
else
    echo "CMakeCache.txt found, skipping reconfiguration"
fi

echo ""
echo "Building test_library_json..."
cmake --build . --target test_library_json

echo ""
echo "================================"
echo "  Build Complete!"
echo "================================"
echo ""
echo "Executable: $BUILD_DIR/test_library_json"
echo ""
echo "Ready to run the test. To export library as JSON:"
echo ""
echo "  $BUILD_DIR/test_library_json"
echo ""
echo "Or with custom output file:"
echo ""
echo "  $BUILD_DIR/test_library_json /path/to/output.json"
echo ""

# Ask if user wants to run now
read -p "Run test now? (y/n) " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo ""
    echo "Starting test program..."
    echo "================================"
    "$BUILD_DIR/test_library_json" "$OUTPUT_FILE"

    echo ""
    if [ -f "$OUTPUT_FILE" ]; then
        echo "================================"
        echo "Test succeeded!"
        echo "Output saved to: $OUTPUT_FILE"
        echo "File size: $(du -h "$OUTPUT_FILE" | cut -f1)"
        echo ""
        echo "To view the JSON:"
        echo "  cat $OUTPUT_FILE | jq"
        echo ""
        echo "To count artists:"
        echo "  jq '.library | length' $OUTPUT_FILE"
        echo ""
        echo "To count total albums:"
        echo "  jq '[.library[].albums | length] | add' $OUTPUT_FILE"
        echo ""
        echo "To count total tracks:"
        echo "  jq '[.library[].albums[].tracks | length] | add' $OUTPUT_FILE"
    else
        echo "Test completed but output file was not created"
    fi
fi
