#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
benchmark="$root/build/bin/indexer_benchmark"
url=${SEARCHER_URL:-http://127.0.0.1:9411}
result_file=${RESULT_FILE:-"$root/benchmark-result.json"}
if [ ! -x "$benchmark" ]; then
  echo "Missing $benchmark. Build the project with CMake first." >&2
  exit 1
fi
case ${1:-} in
  -h|--help)
    echo "Usage: $0 [benchmark options] (defaults to 4 query threads)"
    exec "$benchmark" --help
    ;;
esac

# The benchmark streams progress to stderr and emits one JSON report on success.
report=$("$benchmark" "$url" --query-threads 4 "$@")
printf '%s\n' "$report" > "$result_file"
printf 'Result: %s\n' "$result_file" >&2
