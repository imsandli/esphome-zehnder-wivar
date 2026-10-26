#!/usr/bin/env bash
# Install git hooks: quick CI before every commit, full CI (with firmware builds) before every push.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOOKS="$(git -C "$ROOT" rev-parse --git-path hooks)"
[[ "$HOOKS" = /* ]] || HOOKS="$ROOT/$HOOKS"
mkdir -p "$HOOKS"

cat > "$HOOKS/pre-commit" <<'EOF'
#!/usr/bin/env bash
# Installed by ci/install-hooks.sh. Skip once with: git commit --no-verify
exec "$(git rev-parse --show-toplevel)/ci/run.sh" --quick
EOF
cat > "$HOOKS/pre-push" <<'EOF'
#!/usr/bin/env bash
# Installed by ci/install-hooks.sh. Skip once with: git push --no-verify
exec "$(git rev-parse --show-toplevel)/ci/run.sh"
EOF
chmod +x "$HOOKS/pre-commit" "$HOOKS/pre-push"
echo "Installed pre-commit (quick CI) and pre-push (full CI) hooks in $HOOKS"
