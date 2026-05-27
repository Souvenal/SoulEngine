#!/bin/bash

STDIN=$(cat)
FILE_PATH=$(echo "$STDIN" | jq -r '.tool_input.file_path // empty')

if [[ "$FILE_PATH" =~ \.(h|hpp|c|cpp|cppm)$ ]]; then
    clang-format -i --style=file "$FILE_PATH" 2>/dev/null || true
fi
exit 0