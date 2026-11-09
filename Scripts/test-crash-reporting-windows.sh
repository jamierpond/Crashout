#!/usr/bin/env bash
# Usage: Scripts/test-crash-reporting-windows.sh <ssh host> <x64|arm64> [branch]
# Checks out the pushed branch on a Windows box and runs the crash reporting tests.
set -euo pipefail
host=$1
arch=$2
branch=${3:-$(git rev-parse --abbrev-ref HEAD)}
repository=https://github.com/jamierpond/Crashout.git
checkout='$HOME\projects\Crashout'

ssh "$host" "powershell -NoProfile -Command \"
  if (!(Test-Path $checkout)) { git clone $repository $checkout }
  git -C $checkout fetch origin $branch
  git -C $checkout checkout -B $branch FETCH_HEAD
  cmd /c $checkout\\Scripts\\test-crash-reporting-windows.bat $arch
  exit \\\$LASTEXITCODE\""
