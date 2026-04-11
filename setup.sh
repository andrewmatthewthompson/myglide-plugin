#!/bin/bash
set -euo pipefail

if ! command -v xcodegen &>/dev/null; then
    echo "XcodeGen not found. Install with: brew install xcodegen"
    exit 1
fi

if [ ! -f Local.xcconfig ]; then
    cp Local.xcconfig.example Local.xcconfig
    echo "Created Local.xcconfig — edit it and set DEVELOPMENT_TEAM to your"
    echo "Apple Developer team ID, then re-run ./setup.sh."
    exit 1
fi

if grep -q "REPLACE_WITH_YOUR_TEAM_ID" Local.xcconfig; then
    echo "Local.xcconfig still has the placeholder DEVELOPMENT_TEAM."
    echo "Edit it with your real team ID, then re-run ./setup.sh."
    exit 1
fi

xcodegen generate
echo "Generated MyGlide.xcodeproj — open it in Xcode."
