#!/bin/sh
set -eu

if [ "$#" -gt 1 ]; then
  echo "Usage: $0 [INDEX_DIR]" >&2
  exit 2
fi
case ${1:-} in
  -h|--help) echo "Usage: $0 [INDEX_DIR]"; exit 0 ;;
esac

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
searcher="$root/build/bin/searcher"
url=${SEARCHER_URL:-http://127.0.0.1:9411}
router_config=${ROUTER_CONFIG:-"$root/examples/searcher/router.conf"}
if [ ! -x "$searcher" ]; then
  echo "Missing $searcher. Build the project with CMake first." >&2
  exit 1
fi

if [ "$#" -eq 1 ]; then
  index_dir=$1
  mkdir -p -- "$index_dir"
else
  index_dir=$(mktemp -d "${TMPDIR:-/tmp}/web3-searcher.XXXXXXXX")
fi
if [ ! -f "$index_dir/schema.json" ]; then
  for name in transactions.json blocks.json schema.json; do
    if [ ! -e "$index_dir/$name" ]; then
      cp "$root/examples/searcher/$name" "$index_dir/$name"
    fi
  done
fi

printf 'Index directory: %s\nSearcher URL: %s\n' "$index_dir" "$url" >&2
exec "$searcher" serve "$index_dir" "$url" "$router_config"
