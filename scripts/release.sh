#!/usr/bin/env bash
# Usage: ./scripts/release.sh 1.2.7
# Bumps inkmod_version in platformio.ini, commits, and creates the git tag.
set -euo pipefail

VERSION="${1:-}"
if [[ -z "$VERSION" ]]; then
  echo "Usage: $0 <version>  (e.g. 1.2.7)" >&2
  exit 1
fi

if sed --version >/dev/null 2>&1; then
  # GNU sed (Linux)
  sed -i "s/inkmod_version = .*/inkmod_version = $VERSION/" platformio.ini
else
  # BSD sed (macOS)
  sed -i '' "s/inkmod_version = .*/inkmod_version = $VERSION/" platformio.ini
fi
git add platformio.ini
git commit -m "Update inkmod_version to $VERSION"
git tag "v$VERSION"
echo "Tagged v$VERSION — push with: git push && git push origin v$VERSION"
