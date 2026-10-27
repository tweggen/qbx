#!/usr/bin/env bash
#
# This is for building the entire suite during development.
# It builds everything in the respective repos as thesy are living side by
# side on hard drive.
# For deployment, look into the nassau-suite.
#

VCPKG_ROOT="$PWD/vcpkg"
export VCPKG_ROOT

export QBX_PARENT="$PWD/../"

for i in nassau-plugin-sdk nassau-dsp qbx
do
	(
		cd "$QBX_PARENT/$i"
		git pull && ./ci/build.sh
	)
done

for i in nassau-analogue nassau-bloom nassau-eq nassau-mangrove2 nassau-zermatt
do
	(
		cd "$QBX_PARENT/$i"
		git pull && ./ci/build.sh && ./ci/install.sh
	)
done
