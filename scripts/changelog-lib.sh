# Shared helpers for CHANGELOG.md — sourced by bump-version.sh

_changelog_finalize() {
  local new_ver="$1"
  local changelog="$2"
  local dt
  dt="$(date +%Y-%m-%d)"
  local tmp
  tmp="$(mktemp)"

  awk -v ver="$new_ver" -v dt="$dt" '
    BEGIN { unreleased = ""; in_unrel = 0 }
    /^## \[Unreleased\]/ { in_unrel = 1; unreleased = ""; next }
    in_unrel && /^## \[/ {
      print "## [Unreleased]"
      print ""
      print "## [" ver "] - " dt
      if (unreleased ~ /[^[:space:]]/) {
        printf "%s", unreleased
        if (unreleased !~ /\n$/) print ""
      } else {
        print ""
        print "*(keine Einträge)*"
        print ""
      }
      in_unrel = 0
      print
      next
    }
    in_unrel { unreleased = unreleased $0 "\n"; next }
    { print }
  ' "$changelog" > "$tmp" && mv "$tmp" "$changelog"
}

_changelog_extract_version() {
  local ver="$1"
  local changelog="$2"
  awk -v ver="$ver" '
    $0 ~ "^## \\[" ver "\\]" { found = 1; next }
    found && /^## \[/ { exit }
    found { print }
  ' "$changelog"
}
