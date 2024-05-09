#!/bin/sh -x

mkdir -p build/src/python  # for the couple of .o files

# DOC on the options: https://github.com/wjakob/nanobind/blob/master/src/nb_combined.cpp#L30

# 17 is from nanobind's doc, does it work welll with others?
# TODO tapkee submmodule or external dependency.
#g++ src/python/nanobind_extension.cpp -std=c++17 -fvisibility=hidden -DNDEBUG -DNB_COMPACT_ASSERTIONS -I/usr/include/python3.12 -fPIC -I./include -I/usr/include/eigen3 -I/home/garcia/coding-challenges/ext/nanobind/include -I~/coding-challenges/ext/nanobind/ext/robin_map/include -Os -c -o build/src/python/nanobind_extension.o
g++ src/python/nanobind_extension.cpp -std=c++17 -fvisibility=hidden -I/usr/include/python3.12 -fPIC -I./include -I/usr/include/eigen3 -I/home/garcia/coding-challenges/ext/nanobind/include -I~/coding-challenges/ext/nanobind/ext/robin_map/include -Os -c -o build/src/python/nanobind_extension.o -ggdb  # debug

#g++ ~/coding-challenges/ext/nanobind/src/nb_combined.cpp -std=c++17 -fvisibility=hidden -DNDEBUG -DNB_COMPACT_ASSERTIONS -I/usr/include/python3.12 -fPIC -I/home/garcia/coding-challenges/ext/nanobind/include -I/home/garcia/coding-challenges/ext/nanobind/ext/robin_map/include -O3 -fno-strict-aliasing -ffunction-sections -fdata-sections -c -o build/src/python/libnanobind.o
g++ ~/coding-challenges/ext/nanobind/src/nb_combined.cpp -std=c++17 -fvisibility=hidden -I/usr/include/python3.12 -fPIC -I/home/garcia/coding-challenges/ext/nanobind/include -I/home/garcia/coding-challenges/ext/nanobind/ext/robin_map/include -O3 -fno-strict-aliasing -ffunction-sections -fdata-sections -c -o build/src/python/libnanobind.o -ggdb  # debug

#g++ -shared -Wl,-s -Wl,--gc-sections build/src/python/nanobind_extension.o build/src/python/libnanobind.o -o lib/tapkee.cpython-312-x86_64-linux-gnu.so
g++ -shared -Wl,--gc-sections build/src/python/nanobind_extension.o build/src/python/libnanobind.o -lfmt -o lib/tapkee.cpython-312-x86_64-linux-gnu.so -ggdb # debug, not sure if ggdb in this one makes sense

python -c "import lib.tapkee as tapkee; import numpy as np; parameters = tapkee.ParametersSet(); tapkee.initialize().withParameters(parameters).embedUsing(np.array([[0,2],[3,1]]))"