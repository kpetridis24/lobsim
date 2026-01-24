set -e

if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
else
    GENERATOR="Unix Makefiles"
fi

echo "Using generator: $GENERATOR"

cmake -S . -B build -G "$GENERATOR" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLOBSIM_BUILD_PYTHON=OFF -DLOBSIM_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
