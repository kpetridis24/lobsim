cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLOBSIM_BUILD_PYTHON=OFF -DLOBSIM_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
