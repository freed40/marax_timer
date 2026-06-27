#!/usr/bin/env bash
# Empfiehlt patch | minor | major anhand der Änderungen seit dem letzten Git-Tag.
# Für Menschen und KI vor ./scripts/bump-version.sh

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

LAST_TAG=""
if LAST_TAG="$(git describe --tags --abbrev=0 --match 'v*' 2>/dev/null)"; then
  RANGE="$LAST_TAG..HEAD"
  echo "Seit $LAST_TAG:"
else
  RANGE="HEAD"
  echo "Kein Tag v* — betrachte alle Commits:"
fi

FILES="$(git diff --name-only "$RANGE" 2>/dev/null || true)"
if [[ -z "$FILES" ]]; then
  echo "  (keine Dateiänderungen)"
  echo
  echo "Empfehlung: patch"
  echo "  ./scripts/bump-version.sh patch"
  exit 0
fi

echo "$FILES" | sed 's/^/  /'
echo

LEVEL="patch"
REASON="nur Doku/Config/Scripts oder kleine Änderungen"

if echo "$FILES" | grep -qE '(^timer_esp32/.*\.ino$|^timer/.*\.ino$)'; then
  DIFF="$(git diff "$RANGE" -- timer_esp32/ timer/ 2>/dev/null || true)"
  if echo "$DIFF" | grep -qE '^[+-]#define MARAX_(RX|TX) '; then
    LEVEL="major"
    REASON="Firmware: UART-Pinout geändert (MARAX_RX/TX)"
  elif echo "$DIFF" | grep -qiE 'BREAKING|PartitionScheme|partition.*change'; then
    LEVEL="major"
    REASON="Firmware: expliziter Breaking Change oder Partition"
  else
    LEVEL="minor"
    REASON="Firmware-Sketch geändert (Feature oder Verhaltensänderung)"
  fi
elif echo "$FILES" | grep -qE '^\.github/'; then
  LEVEL="patch"
  REASON="CI/Workflow"
fi

# Nur VERSION/scripts ohne Firmware → patch
if ! echo "$FILES" | grep -qE '\.ino$'; then
  if [[ "$LEVEL" != "major" ]]; then
    LEVEL="patch"
    REASON="keine .ino-Änderungen"
  fi
fi

CURRENT="$(tr -d '[:space:]' < VERSION)"
IFS=. read -r MA MI PA <<< "$CURRENT"
case "$LEVEL" in
  patch) PA=$((PA + 1)) ;;
  minor) MI=$((MI + 1)); PA=0 ;;
  major) MA=$((MA + 1)); MI=0; PA=0 ;;
esac
NEXT="$MA.$MI.$PA"

echo "Empfehlung: $LEVEL ($REASON)"
echo "  $CURRENT → $NEXT"
echo
echo "  ./scripts/bump-version.sh $LEVEL"
