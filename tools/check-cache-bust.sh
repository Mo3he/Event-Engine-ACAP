#!/bin/sh
# Fail if a script or stylesheet in the web UI is not cache-busted with the
# app version. index.html is served no-store, but the .js/.css files are not,
# so browsers keep using a cached copy after an upgrade unless the ?v= changes
# (1.9.21 shipped a new settings.js under the old ?v= and the MQTT form kept
# running the previous version's code).
#
#   tools/check-cache-bust.sh

set -e

repo_root=$(
	CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd
)
version=$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' "$repo_root/app/manifest.json" | head -1)
[ -n "$version" ] || {
	echo "check-cache-bust: no version in app/manifest.json" >&2
	exit 1
}

status=0
for page in "$repo_root"/app/html/*.html; do
	refs=$(grep -oE '(src|href)="[^"/:]+\.(js|css)(\?[^"]*)?"' "$page" || true)
	[ -n "$refs" ] || continue
	for ref in $refs; do
		case "$ref" in
		*"?v=$version\"") ;;
		*)
			echo "check-cache-bust: $(basename "$page"): $ref should end in ?v=$version" >&2
			status=1
			;;
		esac
	done
done
[ "$status" -eq 0 ] && echo "check-cache-bust: all UI assets use ?v=$version"
exit "$status"
