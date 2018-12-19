#!/bin/bash
set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
SOURCE_FOLDERS="source"

if [ -z "$CLANG_FORMAT" ]; then
    CLANG_FORMAT=clang-format-7
fi

FULL_PATH_SOURCE_FOLDERS=""
for source_folder in $SOURCE_FOLDERS; do
    if [ -d "$SCRIPT_DIR/$source_folder" ]; then
        FULL_PATH_SOURCE_FOLDERS="$FULL_PATH_SOURCE_FOLDERS $SCRIPT_DIR/$source_folder"
    fi
done

find $FULL_PATH_SOURCE_FOLDERS -maxdepth 1 -iname *.hpp -o -iname *.cpp | xargs $CLANG_FORMAT -style=file -i
