#!/usr/bin/env bash
# Publish wiki/*.md to the GitHub wiki.
#
# GitHub does not create the wiki's git repository until the FIRST PAGE HAS
# BEEN SAVED IN THE WEB UI. Until then `PetDoor.wiki.git` does not exist and
# clone fails with "Repository not found" — which is why this cannot be fully
# automated from a cold start.
#
#   ONE-TIME:  https://github.com/jchirayath/PetDoor/wiki
#              -> "Create the first page" -> Save (any content; it is replaced)
#
# Then run this from anywhere:
#
#   ./wiki/publish.sh
#
set -euo pipefail

REPO="${PETDOOR_WIKI_REPO:-https://github.com/jchirayath/PetDoor.wiki.git}"
here="$(cd "$(dirname "$0")" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "Cloning $REPO"
if ! git clone --quiet "$REPO" "$tmp/wiki" 2>/dev/null; then
  cat >&2 <<'MSG'

Could not clone the wiki.

Almost always this means the wiki has never been initialised. GitHub only
creates the wiki repository after the first page is saved through the web UI:

    https://github.com/jchirayath/PetDoor/wiki

Click "Create the first page", put anything in it, Save. Then re-run this.
MSG
  exit 1
fi

# README.md documents THIS DIRECTORY — how to edit and publish the wiki. It is
# not a wiki page, and copying it up would put "run ./wiki/publish.sh" in front
# of readers who have no checkout. Everything else here is a page.
for f in "$here"/*.md; do
  [ "$(basename "$f")" = "README.md" ] && continue
  cp "$f" "$tmp/wiki/"
done

cd "$tmp/wiki"
if git diff --quiet && git diff --cached --quiet && [ -z "$(git status --porcelain)" ]; then
  echo "No changes — the wiki already matches wiki/."
  exit 0
fi

git add -A
git -c user.name="${GIT_AUTHOR_NAME:-$(git config user.name)}" \
    -c user.email="${GIT_AUTHOR_EMAIL:-$(git config user.email)}" \
    commit --quiet -m "Sync wiki from the repository's wiki/ directory"
git push --quiet origin HEAD

echo "Published:"
ls -1 "$here"/*.md | grep -v '/README\.md$' | sed 's|.*/|  |'
echo
echo "  https://github.com/jchirayath/PetDoor/wiki"
