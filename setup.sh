#!/bin/bash
set -euo pipefail

if ! command -v xcodegen &>/dev/null; then
    echo "XcodeGen not found. Install with: brew install xcodegen"
    exit 1
fi

xcodegen generate
echo "Generated MyGlide.xcodeproj — open it in Xcode."
