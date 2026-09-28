#!/bin/sh
# check-picks.sh - say which of the outstanding openwrt commits will apply
# cleanly, and what is blocking the rest.
#
# show-picks.sh tells you what has not been taken yet. This tells you which
# of those you can take right now with no work at all.
#
# Run it from your morrownr/mt76 clone, same as show-picks.sh. It works in a
# scratch copy, so your tree is left alone and there is nothing to abort.

set -u

REMOTE=openwrt
BRANCH=master

if ! git rev-parse --git-dir >/dev/null 2>&1; then
    echo "Not inside a git repository. cd into your morrownr/mt76 clone first."
    exit 1
fi

if ! git remote | grep -qx "$REMOTE"; then
    echo "No '$REMOTE' remote found. Add it once with:"
    echo
    echo "    git remote add $REMOTE https://github.com/openwrt/mt76.git"
    echo
    exit 1
fi

skipfile=$(dirname "$0")/skip-picks.txt
skiplist=""
if [ -f "$skipfile" ]; then
    skiplist=$(sed 's/#.*//' "$skipfile" | tr -d ' \t' | grep -v '^$')
fi

is_skipped() {
    _full=$1
    _oldifs=$IFS
    IFS='
'
    for _id in $skiplist; do
        case "$_full" in
        "$_id"*) IFS=$_oldifs; return 0 ;;
        esac
    done
    IFS=$_oldifs
    return 1
}

echo "Fetching $REMOTE ..."
if ! git fetch -q "$REMOTE"; then
    echo "Could not fetch $REMOTE. Check your connection and the remote URL."
    exit 1
fi

applied=$(git log --grep='cherry picked from commit' \
    | sed -n 's/.*cherry picked from commit \([0-9a-f]\{7,\}\).*/\1/p')

todo=$(git log --reverse --no-merges --format='%H %h %s' "HEAD..$REMOTE/$BRANCH" \
    | while read -r full short subject; do
        case "$applied" in
        *"$full"*) continue ;;
        esac
        is_skipped "$full" && continue
        printf '%s %s %s\n' "$full" "$short" "$subject"
    done)

total=$(printf '%s\n' "$todo" | grep -c .)
if [ "$total" -eq 0 ]; then
    echo
    echo "Nothing outstanding. You are caught up."
    exit 0
fi

echo "Testing $total commits ..."

scratch=$(mktemp -d)
git worktree add -q --detach "$scratch" HEAD || exit 1

clean=$(dirname "$0")/picks-clean.txt
blocked=$(dirname "$0")/picks-blocked.txt
: > "$clean"
: > "$blocked"

printf '%s\n' "$todo" | while read -r full short subject; do
    [ -z "$full" ] && continue
    if git -C "$scratch" cherry-pick --no-commit "$full" >/dev/null 2>&1; then
        printf '%s\n' "$short" >> "$clean"
    else
        stops=$(git -C "$scratch" diff --name-only --diff-filter=U | tr '\n' ' ')
        printf '%-10s %-34s %s\n' "$short" "${stops:-unknown}" "$subject" >> "$blocked"
    fi
    git -C "$scratch" cherry-pick --abort >/dev/null 2>&1
    git -C "$scratch" reset -q --hard HEAD
done

git worktree remove --force "$scratch" >/dev/null 2>&1
rmdir "$scratch" 2>/dev/null

nclean=$(grep -c . "$clean")
nblocked=$(grep -c . "$blocked")

echo
printf '  %s apply cleanly\n' "$nclean"
printf '  %s will not apply\n' "$nblocked"
echo

if [ "$nclean" -gt 0 ]; then
    echo "To take all of them, oldest first, copy and paste this line:"
    echo
    printf '    git cherry-pick -x %s\n' "$(tr '\n' ' ' < "$clean" | sed 's/ *$//')"
    echo
    echo "Build before you push. The NAN commit applied cleanly and still"
    echo "broke the build on kernels below 7.1."
    echo
fi

if [ "$nblocked" -gt 0 ]; then
    echo "The rest are in picks-blocked.txt with the file that stops each one."
    top=$(awk '{print $2}' "$blocked" | sort | uniq -c | sort -rn | head -1)
    topn=$(printf '%s\n' "$top" | awk '{print $1}')
    topf=$(printf '%s\n' "$top" | awk '{print $2}')
    if [ "${topn:-0}" -gt 2 ]; then
        printf '%s of them are stopped by the same file, %s.\n' "$topn" "$topf"
    fi
fi
