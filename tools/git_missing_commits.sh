#!/bin/bash
set -eu
tgt_tmp_fn=""
src_tmp_fn=""

# examples:
#
# 1. Find commits in branch 3.3.2 that are missing in 3.4.0 and clolor the members of the block team
# ./git_missing_commits.sh 3.3.2 3.4.0 | grep --color=always -E 'Yaron|Roman|Gan|Gerli|$'
#
git_top=$(git rev-parse --show-toplevel || echo .) || exit 1

clean_tmp() {
        for f in "$src_tmp_fn" "$tgt_tmp_fn"; do
                [ -n "$f" ] && [ -f "$f" ] && rm "$f" 2> /dev/null
        done
}
trap clean_tmp EXIT

escape_branch_name() {
        local branch_name="$1"
        echo "${branch_name//\//-}"
}

usage_str="Usage: $0 <source branch> <target branch> | grep <say... Author name>"

# Parse the arguments
if (( $# < 2 )); then
        echo "$usage_str"
        exit 1
fi
src_branch="$1"
tgt_branch="$2"


tgt_tmp_fn="/tmp/$(escape_branch_name "change_ids_$tgt_branch.$$")"
src_tmp_fn="/tmp/$(escape_branch_name "change_ids_$src_branch.$$")"

git --no-pager log "$tgt_branch" --grep='Change-Id:' --all-match --pretty='%b' \
        | sed -n 's/^[[:space:]]*Change-Id:[[:space:]]*//p' \
        | sort -u > "$tgt_tmp_fn"

ignore_commits=$(sed -E 's/[[:space:]]*#.*$//; /^[[:space:]]*$/d' "$git_top/.git_ignore_changeids") # removes comments from .git_ignore_changeids
ignore_commits=$(echo "$ignore_commits" | tr '\n' '|' | sed 's/|$//')

git --no-pager log "$src_branch" --grep='Change-Id:' --all-match --pretty='%b' \
        | sed -n 's/^[[:space:]]*Change-Id:[[:space:]]*//p' \
        | grep -vE "$ignore_commits" | sort -u > "$src_tmp_fn"

change_ids_unique_to_src=$(comm -23 "$src_tmp_fn" "$tgt_tmp_fn")

echo "In $src_branch not in $tgt_branch"
echo "----------------------------------------------"
for change_id in $change_ids_unique_to_src; do
        git -P log -1 --pretty=format:'%C(yellow)%h %Cgreen%an %Cblue%ad %Creset%<(50,trunc)%s' --date=short --color=always "$src_branch" --grep="$change_id" --all-match "$src_branch"
        echo
done

