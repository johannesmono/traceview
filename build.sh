#!/bin/sh

CFLAGS="-std=c99
        -O3 -march=native
        -Wall
        -Wconversion
        -Wextra
        -Wpedantic
        -Wshadow
        -Wundef
        -Wunused
        -Wvla"

build() {
	echo "\033[36m./build.sh: build\033[0m"
	mkdir -p build || exit 1
	cc ${CFLAGS} -o build/tv tv.c snk.m \
		-fobjc-arc -framework AppKit -framework QuartzCore -framework Metal || exit 1
}

clean() {
	echo "\033[36m./build.sh: clean\033[0m"
	rm -rf build || exit 1
}

run() {
	echo "\033[36m./build.sh: run\033[0m"
	build/tv "${@}"
}

usage() {
	echo "build.sh [command ...]" 2>&1
	echo "\ncommands:"            2>&1
	echo "  clean"                2>&1
	echo "  build"                2>&1
	echo "  run [args]"           2>&1
}

test ${#} -eq 0 && { usage; exit 1; }
while test ${#} -gt 0; do
	case "${1}" in
	'build') shift; build || exit 1;;
	'clean') shift; clean || exit 1;;
	'run')   shift; run "$@"; exit $?;;
	*)       usage; exit 1;;
	esac
done
