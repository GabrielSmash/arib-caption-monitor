#!/usr/bin/env bash
# Builds dist/arib-caption-monitor-<version>.flatpak, a single-file bundle installable on any Linux
# distribution with Flatpak. Needs (per user, no root): flatpak install flathub org.flatpak.Builder
set -euo pipefail

APP_ID=io.github.gabrielsmash.AribCaptionMonitor
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MANIFEST="$ROOT/packaging/flatpak/$APP_ID.yml"
VERSION=$(sed -nE 's/^project\(arib-caption-monitor VERSION ([0-9.]+).*/\1/p' "$ROOT/CMakeLists.txt")
BUNDLE="$ROOT/dist/arib-caption-monitor-$VERSION.flatpak"

cd "$ROOT"
mkdir -p dist
flatpak run --user org.flatpak.Builder \
    --user --install-deps-from=flathub --force-clean --ccache --disable-rofiles-fuse \
    --state-dir="$ROOT/.flatpak-builder" --repo="$ROOT/flatpak-repo" \
    "$ROOT/flatpak-build" "$MANIFEST"

# --runtime-repo makes `flatpak install` fetch the KDE runtime from Flathub automatically.
flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
    "$ROOT/flatpak-repo" "$BUNDLE" "$APP_ID"

echo "Bundle created: $BUNDLE"
