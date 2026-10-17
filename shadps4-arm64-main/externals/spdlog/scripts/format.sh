#!/bin/bash

cd "$(dirname "$0")"/..
pwd
find_sources="find include src tests example bench -type f -name *\.h -o -name *\.cpp"
echo -n "Running dos2unix     "
$find_sources | xargs -I {} sh -c "dos2unix '{}' 2>/dev/null; echo -n '.'"
echo
echo -n "Running clang-format "
$find_sources | xargs -I {} sh -c "clang-format -i {}; echo -n '.'"
echo
