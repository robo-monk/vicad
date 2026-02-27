#!/usr/bin/env bash
# tools/check-docs.sh
#
# Doc link checker.
# Extracts relative markdown links from AGENTS.md and docs/ files and
# verifies every target path exists in the repo. Exits non-zero on failure.

set -euo pipefail
cd "$(dirname "$0")/.."

PASS=0
FAIL=0

check_links_in_file() {
  local source_file="$1"
  local source_dir
  source_dir="$(dirname "$source_file")"

  # Extract path from [text](path) markdown links — relative only (no http/https/mailto)
  local links
  links=$(grep -o '\]([^)]*)' "$source_file" 2>/dev/null \
    | sed 's/^](\(.*\))/\1/' \
    | grep -v '^https\?://' \
    | grep -v '^mailto:' \
    || true)

  if [ -z "$links" ]; then
    return
  fi

  while IFS= read -r link; do
    # Strip any fragment (#section)
    local path="${link%%#*}"
    [ -z "$path" ] && continue

    # Resolve relative to the file's directory
    local resolved
    if [[ "$path" == /* ]]; then
      resolved=".$path"
    else
      resolved="$source_dir/$path"
    fi

    if [ -e "$resolved" ]; then
      PASS=$((PASS + 1))
    else
      echo "FAIL: broken link in $source_file → $link (resolved: $resolved)"
      FAIL=$((FAIL + 1))
    fi
  done <<< "$links"
}

# Check AGENTS.md and all docs/ markdown files
check_links_in_file "AGENTS.md"
for f in docs/*.md docs/**/*.md; do
  [ -f "$f" ] && check_links_in_file "$f"
done

echo ""
echo "Doc check: $PASS links OK, $FAIL broken."
[ "$FAIL" -eq 0 ]
