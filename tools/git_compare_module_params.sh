#!/bin/bash

# Check if two branches are provided
if [ $# -ne 2 ]; then
    echo "Usage: $0 <old_branch> <new_branch>"
    exit 1
fi

OLD_BRANCH=$1
NEW_BRANCH=$2

# Define color codes for the output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m' # No color

# Extract module parameters from both branches using git grep, remove characters after ";" and sort them
echo "Extracting module parameters from branch '$OLD_BRANCH'..."
OLD_PARAMS=$(git grep '^module_param.*(' "$OLD_BRANCH" | grep -v "drivers/infiniband" | sed 's/;.*//' | sed 's/;.*//' | cut -d ':' -f2-200 | sed -E 's/([^:]+):module_param.*\(([^,]+),.*/\1:\2/' | sort)

echo "Extracting module parameters from branch '$NEW_BRANCH'..."
NEW_PARAMS=$(git grep '^module_param.*(' "$NEW_BRANCH" | grep -v "drivers/infiniband" | sed 's/;.*//' | sed 's/;.*//' | cut -d ':' -f2-200 | sed -E 's/([^:]+):module_param.*\(([^,]+),.*/\1:\2/' | sort)

# Compare the two sets of module parameters and identify changes
echo "Comparing module parameters between '$OLD_BRANCH' and '$NEW_BRANCH'..."

# Find removed module parameters (in old but not in new)
echo -e "\n${RED}Module parameters in '$OLD_BRANCH' removed in '$NEW_BRANCH':${NC}"
removed_params=$(comm -23 <(echo "$OLD_PARAMS") <(echo "$NEW_PARAMS"))

if [ -n "$removed_params" ]; then
    while read -r line; do
        # Get the author who last modified this line in the old branch
	param_name=$(echo "$line" | cut -d ':' -f2)
        author=$(git log -1 --pretty=format:"%an" -S"$param_name" "$OLD_BRANCH") # To optimize
	echo "Removed: $line by:$author"
    done <<< "$removed_params"
else
    echo -e "${YELLOW}No module parameters were removed.${NC}"
fi

# Add spacing between the removed and added sections
echo -e "\n${GREEN}Module parameters in '$NEW_BRANCH' added since '$OLD_BRANCH':${NC}"

# Find added module parameters (in new but not in old)
added_params=$(comm -13 <(echo "$OLD_PARAMS") <(echo "$NEW_PARAMS"))

if [ -n "$added_params" ]; then
    while read -r line; do
        # Get the author who added this line in the new branch
        param_name=$(echo "$line" | cut -d ':' -f2)
        author=$(git log -1 --pretty=format:"%an" -S"$param_name" "$NEW_BRANCH") # To optimize
	desc=$(git grep -w $param_name | grep MODULE_PARM_DESC | cut -d ',' -f2 | cut -d ')' -f1)
	echo "Added: $line by:$author:$desc"
    done <<< "$added_params"
else
    echo -e "${YELLOW}No module parameters were added.${NC}"
fi

# Output a message indicating the comparison is complete
echo -e "\n${YELLOW}Comparison complete.${NC}"

