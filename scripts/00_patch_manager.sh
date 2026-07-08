#!/bin/bash

# manage_patches.sh
# Usage: ./manage_patches.sh [apply|update]

ACTION=$1
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"
PATCH_DIR="$(realpath "$SCRIPT_DIR/../patches")"
EXT_DIR="$(realpath "$SCRIPT_DIR/../ext")"

if [ "$ACTION" == "update" ]; then
    echo "Updating patches from submodule local changes..."
    # Iterate over all directories in ext/
    for d in "$EXT_DIR"/*; do
        if [ -d "$d" ]; then
            MODULE_NAME=$(basename "$d")
            cd "$d" || continue
            
            # Ensure it is a git repository
            if [ -d .git ] || [ -f .git ]; then
                # Temporary file to store untracked files list
                TMP_UNTRACKED="../.tmp_untracked_${MODULE_NAME}"
                git ls-files -z --others --exclude-standard > "$TMP_UNTRACKED"
                
                # If there are untracked files, mark them as intent-to-add so they are captured by git diff HEAD
                if [ -s "$TMP_UNTRACKED" ]; then
                    xargs -0 git add -N < "$TMP_UNTRACKED"
                    git diff HEAD > "$PATCH_DIR/${MODULE_NAME}.patch"
                    xargs -0 git reset > /dev/null < "$TMP_UNTRACKED"
                else
                    git diff HEAD > "$PATCH_DIR/${MODULE_NAME}.patch"
                fi
                rm -f "$TMP_UNTRACKED"
            else
                echo "Warning: $MODULE_NAME is not a git repository. Skipping."
                continue
            fi
            
            # If the patch is empty, remove it
            if [ ! -s "$PATCH_DIR/${MODULE_NAME}.patch" ]; then
                rm -f "$PATCH_DIR/${MODULE_NAME}.patch"
                echo "No changes found in $MODULE_NAME."
            else
                echo "Updated $PATCH_DIR/${MODULE_NAME}.patch"
            fi
        fi
    done
    echo "Patch update complete."

elif [ "$ACTION" == "apply" ]; then
    echo "Applying patches to submodules..."
    for patch in "$PATCH_DIR"/*.patch; do
        if [ -f "$patch" ]; then
            MODULE_NAME=$(basename "$patch" .patch)
            if [ -d "$EXT_DIR/$MODULE_NAME" ]; then
                cd "$EXT_DIR/$MODULE_NAME" || continue
                
                # Check if it applies cleanly
                if git apply --check "$patch" 2>/dev/null; then
                    git apply "$patch"
                    echo "Successfully applied $MODULE_NAME.patch"
                # Check if it is already applied
                elif git apply --reverse --check "$patch" 2>/dev/null; then
                    echo "Patch $MODULE_NAME.patch is already applied. Skipping."
                else
                    echo "Error: Patch $MODULE_NAME.patch does not apply cleanly. Please resolve conflicts manually."
                fi
            else
                echo "Warning: Module $MODULE_NAME not found for $patch."
            fi
        fi
    done
    echo "Patch application complete."
else
    echo "Usage: $0 [apply|update]"
    echo "  apply:  Applies all .patch files in patches/ to their respective submodules in ext/"
    echo "  update: Overwrites .patch files in patches/ with current uncommitted changes in ext/"
fi
