#!/usr/bin/env bash
# Version hochzählen: VERSION + timer_esp32.ino, Commit + Git-Tag vX.Y.Z
#
# Aufruf (im Repo-Root oder von überall):
#   ./scripts/bump-version.sh patch
#   ./scripts/bump-version.sh minor
#   ./scripts/bump-version.sh major
#   ./scripts/bump-version.sh patch --push   # danach main + Tag pushen
#
# SemVer: patch = Bugfix/Doku, minor = Feature, major = Breaking Change

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TYPE="${1:-}"
PUSH=false
[[ "${2:-}" == "--push" ]] && PUSH=true

if [[ ! "$TYPE" =~ ^(patch|minor|major)$ ]]; then
  echo "Usage: $0 patch|minor|major [--push]"
  exit 1
fi

OLD="$(tr -d '[:space:]' < VERSION)"
if [[ ! "$OLD" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Fehler: ungültige VERSION-Datei: '$OLD' (erwartet X.Y.Z)"
  exit 1
fi

IFS=. read -r MA MI PA <<< "$OLD"
case "$TYPE" in
  major) MA=$((MA + 1)); MI=0; PA=0 ;;
  minor) MI=$((MI + 1)); PA=0 ;;
  patch) PA=$((PA + 1)) ;;
esac
NEW="$MA.$MI.$PA"

if git rev-parse "v$NEW" >/dev/null 2>&1; then
  echo "Fehler: Tag v$NEW existiert bereits"
  exit 1
fi

echo "$NEW" > VERSION

SKETCH="$ROOT/timer_esp32/timer_esp32.ino"
if [[ "$(uname -s)" == "Darwin" ]]; then
  sed -i '' "s/#define FIRMWARE_VERSION \".*\"/#define FIRMWARE_VERSION \"$NEW\"/" "$SKETCH"
else
  sed -i "s/#define FIRMWARE_VERSION \".*\"/#define FIRMWARE_VERSION \"$NEW\"/" "$SKETCH"
fi

CHANGELOG="$ROOT/CHANGELOG.md"
if [[ -f "$CHANGELOG" ]]; then
  # shellcheck source=scripts/changelog-lib.sh
  source "$ROOT/scripts/changelog-lib.sh"
  _changelog_finalize "$NEW" "$CHANGELOG"
  echo "CHANGELOG.md: [Unreleased] → [$NEW]"
else
  echo "Hinweis: CHANGELOG.md fehlt — Release-Notes manuell pflegen."
fi

git add VERSION "$SKETCH"
[[ -f "$CHANGELOG" ]] && git add "$CHANGELOG"
git commit -m "Release v$NEW"
git tag "v$NEW"

echo "Version $OLD → $NEW"
echo "  VERSION, timer_esp32.ino, Commit und Tag v$NEW erstellt."

if $PUSH; then
  git push origin main
  git push origin "v$NEW"
  echo "Gepusht — GitHub Actions baut Release + .bin"
else
  echo "Push:"
  echo "  git push origin main && git push origin v$NEW"
fi
