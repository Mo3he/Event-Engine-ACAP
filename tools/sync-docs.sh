#!/bin/sh
# Regenerate the published manual (docs/help.html) from the in-app manual.
#
# The two files are identical apart from the header navigation: the in-app copy
# links back to the app, the published copy links to the site and the repo.
# Keeping this as a script means the site cannot silently rot when the in-app
# manual changes, which is exactly what happened during 1.9.14.
#
#   tools/sync-docs.sh           regenerate docs/help.html
#   tools/sync-docs.sh --check   fail if docs/help.html is out of date (CI)

set -e

repo_root=$(
	CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd
)
src="$repo_root/app/html/help.html"
dst="$repo_root/docs/help.html"

anchor='      <a class="back-link" href="index.html">← Back to App</a>'

if [ ! -f "$src" ]; then
	echo "sync-docs: missing $src" >&2
	exit 1
fi

if ! grep -qF "$anchor" "$src"; then
	echo "sync-docs: navigation anchor not found in $src" >&2
	echo "sync-docs: update the anchor in tools/sync-docs.sh" >&2
	exit 1
fi

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

awk -v anchor="$anchor" '
	$0 == anchor {
		print "      <a class=\"back-link\" href=\"use-cases.html\">Use Cases</a>"
		print "      <a class=\"back-link\" href=\"https://github.com/Mo3he/Event-Engine-ACAP\">← Back to Repo</a>"
		next
	}
	{ print }
' "$src" >"$tmp"

if [ "$1" = "--check" ]; then
	if ! diff -u "$dst" "$tmp"; then
		echo >&2
		echo "sync-docs: docs/help.html is out of date with app/html/help.html" >&2
		echo "sync-docs: run tools/sync-docs.sh and commit the result" >&2
		exit 1
	fi
	echo "sync-docs: docs/help.html is up to date"
	exit 0
fi

cp "$tmp" "$dst"
echo "sync-docs: regenerated docs/help.html from app/html/help.html"
