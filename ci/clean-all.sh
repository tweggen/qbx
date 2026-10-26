#!/usr/bin/env bash

VCPKG_ROOT="$PWD/vcpkg"
export VCPKG_ROOT

export QBX_PARENT="$PWD/../"

for i in nassau-plugin-sdk qbx
do
	(
		cd "$QBX_PARENT/$i"
		./ci/clean.sh
	)
done

for i in nassau-analogue nassau-bloom nassau-eq nassau-mangrove2 nassau-zermatt
do
	(
		cd "$QBX_PARENT/$i"
		./ci/clean.sh
	)
done
