#!/usr/bin/env bash
# Usage: Scripts/test-crash-reporting-windows.sh <ssh host> <x64|arm64> [branch]
# Checks out the pushed branch on a Windows box and runs the crash reporting tests.
set -euo pipefail
host=$1
arch=$2
branch=${3:-$(git rev-parse --abbrev-ref HEAD)}
repository=https://github.com/jamierpond/Crashout.git
checkout='$env:USERPROFILE\projects\Crashout'

steps=(
    "if (!(Test-Path $checkout)) { git clone -q $repository $checkout }"
    "git -C $checkout fetch -q origin $branch"
    "git -C $checkout checkout -q -B $branch FETCH_HEAD"
    "cmd /c $checkout\\Scripts\\test-crash-reporting-windows.bat $arch"
    "exit \$LASTEXITCODE"
)
script=$(IFS=';'; echo "${steps[*]}")
encoded=$(printf '%s' "$script" | iconv -t UTF-16LE | base64 | tr -d '\n')
ssh "$host" "powershell -NoProfile -EncodedCommand $encoded"
