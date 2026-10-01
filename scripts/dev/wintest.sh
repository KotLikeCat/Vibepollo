#!/usr/bin/env bash
# Builds the current working tree (including uncommitted and untracked files) on the
# build-win machine and runs the matching ctest tests there.
# Usage: scripts/dev/wintest.sh <ctest-regex> <cmake-target> [cmake-target...]
# Regex and targets must not contain spaces or quotes.
set -euo pipefail
HOST="${WINTEST_HOST:-Administrator@192.168.88.21}"
cd "$(git rev-parse --show-toplevel)"
REF="wip/$(git rev-parse --abbrev-ref HEAD)"
TMP_INDEX="$(mktemp)"
trap 'rm -f "$TMP_INDEX"' EXIT
cp "$(git rev-parse --git-dir)/index" "$TMP_INDEX"
GIT_INDEX_FILE="$TMP_INDEX" git add -A
TREE="$(GIT_INDEX_FILE="$TMP_INDEX" git write-tree)"
COMMIT="$(git commit-tree "$TREE" -p HEAD -m "wip: remote test snapshot")"
git push -q -f origin "$COMMIT:refs/heads/$REF"
scp -q scripts/dev/wintest-remote.sh "$HOST:C:/build/wintest-remote.sh"
ssh "$HOST" "C:\\msys64\\usr\\bin\\env.exe MSYSTEM=UCRT64 CHERE_INVOKING=1 C:\\msys64\\usr\\bin\\bash.exe -lc 'bash /c/build/wintest-remote.sh $REF $*'"
