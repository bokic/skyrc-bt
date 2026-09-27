#!/bin/env bash

set -e

rm -rf build

cmake -G Ninja -S . -B build
cmake --build build

cp build/compile_commands.json .

rm -rf build
