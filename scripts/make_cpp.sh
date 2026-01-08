cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSIMEX_BUILD_PYTHON=OFF -DSIMEX_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
