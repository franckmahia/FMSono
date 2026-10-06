#!/bin/bash
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"
export CLANG_MODULE_CACHE_PATH="$PROJECT_ROOT/.build/module-cache"
export SWIFTPM_MODULECACHE_OVERRIDE="$PROJECT_ROOT/.build/module-cache"
mkdir -p .build/module-cache .build/cache .build/config .build/security
SWIFT_OPTIONS=(--disable-sandbox --cache-path .build/cache --config-path .build/config --security-path .build/security)
swift build -c release "${SWIFT_OPTIONS[@]}"
BINARY_DIRECTORY="$(swift build -c release "${SWIFT_OPTIONS[@]}" --show-bin-path)"
APP_DIRECTORY="$PROJECT_ROOT/dist/FMSono.app"
mkdir -p "$APP_DIRECTORY/Contents/MacOS" "$APP_DIRECTORY/Contents/Resources"
cp "$BINARY_DIRECTORY/FMSono" "$APP_DIRECTORY/Contents/MacOS/FMSono"
cp Packaging/Info.plist "$APP_DIRECTORY/Contents/Info.plist"
swift scripts/make-icon.swift "$PROJECT_ROOT/.build/AppIcon.iconset"
cp .build/AppIcon.icns "$APP_DIRECTORY/Contents/Resources/AppIcon.icns"
codesign --force --sign - "$APP_DIRECTORY"
codesign --verify --strict "$APP_DIRECTORY"
plutil -lint "$APP_DIRECTORY/Contents/Info.plist"
printf 'Application prête : %s\n' "$APP_DIRECTORY"
