#!/bin/bash
VERSION_FILE="version.txt"
HEADER_FILE="kernel/include/version.h"

if [ ! -f "$VERSION_FILE" ]; then
    echo "0.0.0" > "$VERSION_FILE"
fi

VERSION=$(cat "$VERSION_FILE")

# Bump version first
IFS='.' read -r major minor patch <<< "$VERSION"
new_patch=$((patch + 1))
NEW_VERSION="$major.$minor.$new_patch"
echo "$NEW_VERSION" > "$VERSION_FILE"

# Generate the header file with the NEW version
echo "#pragma once" > "$HEADER_FILE"
echo "#define VERSION_STRING \"$NEW_VERSION\"" >> "$HEADER_FILE"

echo "Version bumped to $NEW_VERSION and header updated."
