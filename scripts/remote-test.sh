#!/usr/bin/env bash
# Copies this checkout's tracked files, as they are in the working tree, to a machine over ssh
# and runs `just test` there in a login shell, so its nix or scoop tools are on PATH.
#
#   scripts/remote-test.sh <user@host> <remote-dir>
#
# <remote-dir> is relative to the remote home; use one named after your branch,
# since other agents share these machines. Windows hosts need git-bash as the
# ssh shell and the MSVC environment already set up for `just test`.

set -euo pipefail

if [ $# -ne 2 ]; then
    echo "usage: $0 <user@host> <remote-dir>" >&2
    exit 2
fi

host=$1
remote_dir=$2
repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

ssh "$host" "mkdir -p '$remote_dir'"
git ls-files -z --cached | tar --null -T - -cf - | ssh "$host" "tar -xf - -C '$remote_dir'"
ssh "$host" "bash -lc \"cd '$remote_dir' && just test\""
