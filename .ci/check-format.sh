#!/usr/bin/env bash

set -u -o pipefail

ret=0

# Check C/C++ files with clang-format
while IFS= read -r -d '' file; do
    clang-format-21 -n --Werror "${file}" || ret=1
done < <(git ls-files -z '*.c' '*.cpp' '*.h' ':!:*/submodule/*')

# Check shell scripts with shfmt
while IFS= read -r file; do
    diff <(cat "${file}") <(shfmt -i 4 -bn -ci -sr "${file}")
    if [ $? -ne 0 ]; then
        echo "${file}" "mismatch"
        ret=1
    fi
done < <(git ls-files '*.sh')

exit ${ret}
