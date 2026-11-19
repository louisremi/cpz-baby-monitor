#!/usr/bin/env bash
# Fetch pinned third-party sources into dependencies/ (shallow, exact revision).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
count=$(jq length repos.json)
for ((i = 0; i < count; i++)); do
    url=$(jq -r ".[$i].url" repos.json)
    path=$(jq -r ".[$i].path" repos.json)
    rev=$(jq -r ".[$i].revision" repos.json)
    if [[ -d "$path/.git" ]] && [[ "$(git -C "$path" rev-parse HEAD)" == "$rev" ]]; then
        continue
    fi
    echo "fetching $path @ $rev"
    rm -rf -- "${ROOT:?}/$path"
    mkdir -p "$path"
    git -C "$path" init -q
    git -C "$path" remote add origin "$url"
    git -C "$path" fetch -q --depth 1 origin "$rev"
    git -C "$path" checkout -q --detach FETCH_HEAD
done
echo "dependencies ready"
