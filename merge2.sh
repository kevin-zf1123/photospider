#!/bin/bash
# merge_photospider.sh
#
# Usage: ./merge_photospider.sh
# This script consolidates the entire Photospider project into a single text file
# for easy sharing and context. It includes the directory structure, documentation,
# build files, source code, and examples.

OUTPUT="tmp2.txt"

# --- Start with a clean slate ---
> "$OUTPUT"
echo "Merging Photospider project files into '$OUTPUT'..."
echo "" >> "$OUTPUT"

# --- Section 1: Directory Structure ---
echo "--- DIRECTORY STRUCTURE ---" >> "$OUTPUT"
echo "" >> "$OUTPUT"
# Use 'tree' if available, otherwise 'find' as a fallback.
# Exclude .vscode, build, cache, and out directories for a clean view.
if command -v tree &> /dev/null
then
    tree -I ".vscode|build|cache|out" >> "$OUTPUT"
else
    find . -not -path '*/.vscode/*' -not -path '*/build/*' -not -path '*/cache/*' -not -path '*/out/*' | sort | sed 's/[^/]*\//--/g;s/--/|-- /' >> "$OUTPUT"
fi
echo "" >> "$OUTPUT"


# --- Section 2: Core Project Files (Documentation & Build) ---
echo "--- CORE PROJECT FILES ---" >> "$OUTPUT"
for file in README.md manual.md Makefile; do
    if [ -f "$file" ]; then
        echo "" >> "$OUTPUT"
        echo "// FILE: $file" >> "$OUTPUT"
        cat "$file" >> "$OUTPUT"
        echo "" >> "$OUTPUT"
    fi
done

# --- Section 3: C++ Source Code ---
echo "--- C++ SOURCE CODE ---" >> "$OUTPUT"
for dir in include src cli; do
    if [ -d "$dir" ]; then
        find "$dir" -type f \( -name "*.hpp" -o -name "*.cpp" \) | sort | while read -r file; do
            echo "" >> "$OUTPUT"
            echo "// FILE: $file" >> "$OUTPUT"
            cat "$file" >> "$OUTPUT"
            echo "" >> "$OUTPUT"
        done
    fi
done

# --- Section 4: Example YAML Files ---
echo "--- EXAMPLE YAML FILES ---" >> "$OUTPUT"
if [ -d "examples" ]; then
    find "examples" -type f -name "*.yaml" | sort | while read -r file; do
        echo "" >> "$OUTPUT"
        echo "# FILE: $file" >> "$OUTPUT"
        cat "$file" >> "$OUTPUT"
        echo "" >> "$OUTPUT"
    done
fi

echo "✅ All files have been successfully merged into: $OUTPUT"