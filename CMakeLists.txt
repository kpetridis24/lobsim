cmake_minimum_required(VERSION 3.23)

project(simex VERSION 0.1.0 LANGUAGES CXX)

# ---- Options ----
option(SIMEX_BUILD_TESTS  "Build C++ unit tests" ON)
option(SIMEX_BUILD_PYTHON "Build Python extension module" ON)
option(SIMEX_SHARED_CORE  "Build simex_core as a shared library" OFF)

# ---- C++ standard / global settings ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Good default for libraries used by Python modules too
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

# ---- Dependencies ----
find_package(ZLIB REQUIRED)
find_package(Arrow CONFIG REQUIRED)
find_package(Parquet CONFIG REQUIRED)

# ---- Core library: simex_core ----
# List sources explicitly (recommended). Add more files as you implement.
set(SIMEX_CORE_SOURCES
  cpp/src/paper_trading_simulator_core.cpp
  cpp/src/in_memory_sink.cpp
  cpp/src/coinapi_coinbase_btcusdt_parquet_source.cpp
)

add_library(simex_core ${SIMEX_CORE_SOURCES})

if(SIMEX_SHARED_CORE)
  set_target_properties(simex_core PROPERTIES
    TYPE SHARED
  )
endif()

add_library(simex::core ALIAS simex_core)

target_include_directories(simex_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cpp/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Warnings (keep reasonable; adjust later)
if(MSVC)
  target_compile_options(simex_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(simex_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# (Optional) define for your codebase if you want
target_compile_definitions(simex_core PUBLIC SIMEX_VERSION="${PROJECT_VERSION}")

target_link_libraries(simex_core PRIVATE Arrow::arrow_shared Parquet::parquet_shared)

add_executable(simex_temp
  cpp/src/temp.cpp
)
target_link_libraries(simex_temp PRIVATE simex::core ZLIB::ZLIB Arrow::arrow_shared Parquet::parquet_shared)


# ---- Install rules for C++ consumers ----
install(TARGETS simex_core
  EXPORT simexTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY cpp/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT simexTargets
  NAMESPACE simex::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/simex
)

# ---- Tests (Catch2) ----
if(SIMEX_BUILD_TESTS)
  include(CTest)

  include(FetchContent)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
  )
  FetchContent_MakeAvailable(Catch2)

  add_executable(simex_core_tests
    cpp/tests/test_paper_trading_simulator_core.cpp
  )
  target_link_libraries(simex_core_tests PRIVATE simex::core Catch2::Catch2WithMain)

  # Auto-discover tests for ctest
  include(Catch)
  catch_discover_tests(simex_core_tests)
endif()

# ---- Python extension (pybind11) ----
# This builds a module that will be imported as: `import simex._core`
if(SIMEX_BUILD_PYTHON)
  include(FetchContent)
  FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG        v2.13.6
  )
  FetchContent_MakeAvailable(pybind11)

  # The pybind module target name can be anything; OUTPUT_NAME controls the filename.
  pybind11_add_module(simex_py MODULE
    cpp/src/python_bindings.cpp
  )

  target_link_libraries(simex_py PRIVATE simex::core)

  # Make the compiled module name "_core" so it imports as simex._core
  set_target_properties(simex_py PROPERTIES
    OUTPUT_NAME "_core"
  )

  # For local (non-wheel) builds, drop the module into python/simex/
  # For wheel builds, scikit-build-core will use CMAKE_INSTALL_PREFIX and install() rules.
  set_target_properties(simex_py PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/python/simex"
  )

  # Install into the Python package namespace "simex"
  install(TARGETS simex_py
    LIBRARY DESTINATION simex
  )
endif()
