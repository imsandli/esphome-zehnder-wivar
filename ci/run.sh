#!/usr/bin/env bash
# Local CI: lint, protocol tests, privacy check, ESPHome config validation and firmware builds.
#
#   ci/run.sh            all stages
#   ci/run.sh --quick    skip the firmware builds (used by the pre-commit hook)
#
# Config and build stages run in .ci/work with the dummy secrets from ci/secrets.ci.yaml, so CI never needs
# (or touches) the real secrets.yaml. The work directory is kept between runs for incremental builds.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/.ci/work"
ESPHOME="$ROOT/.venv/bin/esphome"
DEVICES=(wivar-shelly.yaml wivar-lcd.yaml)
QUICK=0
[[ "${1:-}" == "--quick" ]] && QUICK=1

cd "$ROOT" || exit 1
RESULTS=()
FAILED=0

stage() {  # stage NAME COMMAND...
  local name="$1"; shift
  local log="$ROOT/.ci/$name.log" start=$SECONDS
  mkdir -p "$ROOT/.ci"
  printf '%-10s ' "$name"
  if "$@" >"$log" 2>&1; then
    printf 'PASS  %3ss\n' $((SECONDS - start))
    RESULTS+=("PASS $name")
  else
    printf 'FAIL  %3ss  (log: .ci/%s.log)\n' $((SECONDS - start)) "$name"
    tail -n 15 "$log" | sed 's/^/           | /'
    RESULTS+=("FAIL $name")
    FAILED=1
  fi
}

# Files that would be committed: tracked plus new, not ignored.
project_files() { git -C "$ROOT" ls-files -co --exclude-standard; }

lint() {
  local f rc=0
  for f in $(project_files | grep -E '\.sh$'); do
    bash -n "$f" || { echo "syntax error: $f"; rc=1; }
  done
  if command -v shellcheck >/dev/null; then
    project_files | grep -E '\.sh$' | xargs shellcheck -S warning || rc=1
  else
    echo "shellcheck not installed, skipped"
  fi
  for f in $(project_files | grep -E '\.py$'); do
    "$ROOT/.venv/bin/python" -m py_compile "$f" || { echo "syntax error: $f"; rc=1; }
  done
  return $rc
}

unit() { "$ROOT/tests/run.sh"; }

privacy() {
  local rc=0 files
  files=$(project_files | grep -v '^ci/secrets.ci.yaml$')
  # 1. No value from the local secrets.yaml may appear in a committed file.
  if [[ -f secrets.yaml ]]; then
    while IFS= read -r value; do
      [[ ${#value} -lt 6 ]] && continue
      if grep -lF -- "$value" $files 2>/dev/null; then
        echo "a value from secrets.yaml appears in the file(s) above"
        rc=1
      fi
    done < <(sed -nE 's/^[a-z0-9_]+:[[:space:]]*"?([^"#]*[^"#[:space:]])"?.*$/\1/p' secrets.yaml)
  else
    echo "no secrets.yaml, value check skipped"
  fi
  # 2. Generic patterns: real MAC addresses, private IPv4 addresses, 32-byte base64 keys.
  local hits
  hits=$(grep -nE '\b([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\b' $files | grep -vE '(00:){5}00')
  [[ -n "$hits" ]] && { echo "MAC address found:"; echo "$hits"; rc=1; }
  hits=$(grep -nE '\b(192\.168|10\.[0-9]{1,3}|172\.(1[6-9]|2[0-9]|3[01]))\.[0-9]{1,3}\.[0-9]{1,3}\b' $files)
  [[ -n "$hits" ]] && { echo "private IP address found:"; echo "$hits"; rc=1; }
  hits=$(grep -nE '[A-Za-z0-9+/]{43}=' $files | grep -vE 'A{43}=')
  [[ -n "$hits" ]] && { echo "possible API key found:"; echo "$hits"; rc=1; }
  return $rc
}

prepare_work() {
  mkdir -p "$WORK"
  # Mirror the project files (keeping .esphome build caches), then use the dummy secrets.
  project_files | rsync -a --delete --files-from=- --exclude='.esphome/' "$ROOT/" "$WORK/"
  cp "$ROOT/ci/secrets.ci.yaml" "$WORK/secrets.yaml"
}

config() {
  local rc=0 f
  # The CI secrets must cover exactly the keys of the example.
  diff <(grep -oE '^[a-z0-9_]+:' secrets.yaml.example | sort) <(grep -oE '^[a-z0-9_]+:' ci/secrets.ci.yaml | sort) \
    || { echo "ci/secrets.ci.yaml and secrets.yaml.example list different keys"; rc=1; }
  prepare_work
  for f in "${DEVICES[@]}"; do
    echo "== $f"
    # esphome prints validation errors to stdout, together with the full config: show only the error part.
    local out
    out=$(cd "$WORK" && "$ESPHOME" config "$f" 2>&1) || { echo "$out" | sed -n '/Failed config/,$p' | head -40; rc=1; }
  done
  return $rc
}

build() {
  local rc=0 f
  prepare_work
  for f in "${DEVICES[@]}"; do
    echo "== $f"
    (cd "$WORK" && "$ESPHOME" compile "$f") || rc=1
  done
  return $rc
}

echo "Local CI ($( [[ $QUICK == 1 ]] && echo quick || echo full ))"
stage lint lint
stage unit unit
stage privacy privacy
stage config config
if [[ $QUICK == 0 ]]; then
  stage build build
  grep -E 'RAM:|Flash:' "$ROOT/.ci/build.log" | sed 's/^/           /'
fi

echo
if [[ $FAILED == 0 ]]; then echo "CI passed"; else echo "CI failed"; fi
exit $FAILED
