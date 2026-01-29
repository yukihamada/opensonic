#!/bin/bash
# Auto-commit hook for Claude Code
# Runs after Write/Edit tools to automatically commit changes

set -e

# Read hook input from stdin
INPUT=$(cat)

# Parse tool info from JSON input
TOOL_NAME=$(echo "$INPUT" | jq -r '.tool_name // "unknown"' 2>/dev/null || echo "unknown")
FILE_PATH=$(echo "$INPUT" | jq -r '.tool_input.file_path // ""' 2>/dev/null || echo "")
CWD=$(echo "$INPUT" | jq -r '.cwd // "."' 2>/dev/null || echo ".")

# Change to project directory
cd "$CWD" || exit 0

# Ensure we're in a git repository
git rev-parse --git-dir > /dev/null 2>&1 || exit 0

# Check if there are any changes
if git diff --quiet HEAD 2>/dev/null && git diff --cached --quiet 2>/dev/null; then
    # No changes
    exit 0
fi

# Stage all changes
git add -A

# Check if there's anything to commit after staging
if git diff --cached --quiet 2>/dev/null; then
    exit 0
fi

# Create commit message based on the file
if [ -n "$FILE_PATH" ]; then
    FILENAME=$(basename "$FILE_PATH")
    COMMIT_MSG="auto: Update $FILENAME"
else
    COMMIT_MSG="auto: Changes via Claude Code"
fi

# Commit (--no-verify to skip pre-commit hooks that might fail)
git commit -m "$COMMIT_MSG

Co-Authored-By: Claude <noreply@anthropic.com>" --no-verify 2>/dev/null

echo "Auto-committed: $COMMIT_MSG"
