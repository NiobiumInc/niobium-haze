#!/usr/bin/env bash
# local-pr-review.sh — run the repo's GitHub "PR - Claude Code Review" locally,
# before pushing, so its findings can be addressed early.
#
# It mirrors .github/workflows/pr-claude-code-review.yml: same model, same
# Read-only posture, same prompt pointing at .github/instructions/
# CODE_REVIEW_GUIDE.md. The only differences are that it runs against the local
# branch (committed AND uncommitted changes vs the base) and prints the review
# to stdout instead of posting a PR comment.
#
# Usage:
#   scripts/local-pr-review.sh                 # review changes vs `main`
#   BASE=origin/main scripts/local-pr-review.sh # review vs a different base
#   MODEL=claude-opus-4-8 scripts/local-pr-review.sh
#   scripts/local-pr-review.sh --dry-run       # print files + plan, don't call claude
#
# Resolves the repo root via `git rev-parse`. Requires the `claude` CLI on PATH.
set -euo pipefail

BASE="${BASE:-main}"
MODEL="${MODEL:-claude-sonnet-4-6}" # match the CI workflow's model
GUIDE=".github/instructions/CODE_REVIEW_GUIDE.md"

dry_run=0
case "${1:-}" in
    --dry-run) dry_run=1 ;;
    "") ;;
    *) printf '%s: unknown argument: %s\n' "$(basename "$0")" "$1" >&2; exit 2 ;;
esac

cd "$(git rev-parse --show-toplevel)"

[ -f "$GUIDE" ] || { printf 'error: %s not found (run from the niobium-haze repo)\n' "$GUIDE" >&2; exit 1; }
command -v claude >/dev/null || { printf "error: the 'claude' CLI is not on PATH\n" >&2; exit 1; }

merge_base="$(git merge-base "$BASE" HEAD)" ||
    { printf "error: no merge-base with '%s' (set BASE=<branch>)\n" "$BASE" >&2; exit 1; }

# Files this branch changes vs the base, including uncommitted edits (diff
# against the working tree). Exclude deletions — a deleted file can't be Read.
mapfile -t files < <(git diff --name-only --diff-filter=ACMR "$merge_base")
if [ "${#files[@]}" -eq 0 ]; then
    printf 'No changed files vs %s (%s). Nothing to review.\n' "$BASE" "$merge_base"
    exit 0
fi

prompt="You are a code reviewer for this local branch — a pre-push dry run of the
repository's GitHub PR review.

Start by reading the review guide at:
  $GUIDE

Then review the changed files below, following the guide. For each file:
1. Read the FULL file content with the Read tool.
2. Apply the universal checklist from the guide (section 3).

Changed files (vs $BASE):
$(printf '%s\n' "${files[@]}")

Post a single concise review with only:
1. Summary (2-4 lines)
2. Blockers (must-fix)
3. Risks / Watch-outs
4. Non-blocking suggestions
5. Questions for the author

Rules:
- Each issue must be one line: [Severity] file:line — problem — fix
- No prose, no elaboration, no closing summary
- If a section has no findings, omit it entirely
- Label each issue: Blocker / High / Medium / Low"

printf '== local PR review ==\nbase:  %s (%s)\nmodel: %s\nfiles: %d\n' \
    "$BASE" "$merge_base" "$MODEL" "${#files[@]}"
printf '  %s\n' "${files[@]}"

if [ "$dry_run" -eq 1 ]; then
    printf '\n[dry-run] would run: claude -p <prompt> --model %s --max-turns 45 --allowedTools Read\n' "$MODEL"
    exit 0
fi

printf '\n'
exec claude -p "$prompt" --model "$MODEL" --max-turns 45 --allowedTools "Read"
