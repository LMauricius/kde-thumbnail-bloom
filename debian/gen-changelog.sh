#!/bin/sh

_version=$(sed -n 's/.*project(.*VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
_date=$(git log -1 --format="%cd" --date=format:"%Y%m%d")
_build=$(date -R)

cp debian/changelog.in debian/changelog
sed -e "s#@VERSION@#${_version}#" \
    -e "s#@DATE@#${_date}#" \
    -e "s#@BUILDDATE@#${_build}#" \
    -i debian/changelog
