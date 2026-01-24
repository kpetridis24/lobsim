cmake_minimum_required(VERSION 3.23)

project(lobsim VERSION 0.1.0 LANGUAGES CXX)

# ---- Options ----
option(LOBSIM_BUILD_TESTS  "Build C++ unit tests" ON)
option(LOBSIM_BUILD_PYTHON "Build Python extension module" ON)
option(LOBSIM_SHARED_CORE  "Build lobsim_core as a shared library" OFF)
option(LOBSIM_BUILD_PARQUET "Build Parquet/Arrow support" OFF)

# ---- C++ standard / global settings ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Good default for libraries used by Python modules too
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

# ---- Dependencies ----
include(FetchContent)

# Try to find ZLIB, or fetch it if missing (common on Windows)
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
  message(STATUS "ZLIB not found, fetching via FetchContent...")
  FetchContent_Declare(
    ZLIB
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3
  )
  FetchContent_MakeAvailable(ZLIB)
  
  # Alias for compatibility if needed
  if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
  endif()
endif()

if(LOBSIM_BUILD_PARQUET)
  find_package(Arrow CONFIG REQUIRED)
  find_package(Parquet CONFIG REQUIRED)
endif()

# ---- Core library: lobsim_core ----
# List sources explicitly (recommended). Add more files as you implement.
set(LOBSIM_CORE_SOURCES
  cpp/src/paper_trading_simulator.cpp
  cpp/src/in_memory_sink.cpp
  cpp/src/metrics_sink.cpp
)

add_library(lobsim_core ${LOBSIM_CORE_SOURCES})

if(LOBSIM_SHARED_CORE)
  set_target_properties(lobsim_core PROPERTIES
    TYPE SHARED
  )
endif()

add_library(lobsim::core ALIAS lobsim_core)

target_include_directories(lobsim_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cpp/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Warnings (keep reasonable; adjust later)
if(MSVC)
  target_compile_options(lobsim_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(lobsim_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# (Optional) define for your codebase if you want
target_compile_definitions(lobsim_core PUBLIC LOBSIM_VERSION="${PROJECT_VERSION}")

# (Keep lobsim_core free of heavy IO deps like Arrow/Parquet.)

# ---- Arrow/Parquet IO library (optional for Python users) ----
if(LOBSIM_BUILD_PARQUET)
  add_library(lobsim_parquet
    cpp/src/coinbase_btcusdt_parquet_source.cpp
  )
  add_library(lobsim::parquet ALIAS lobsim_parquet)

  target_include_directories(lobsim_parquet
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cpp/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )

  if(MSVC)
    target_compile_options(lobsim_parquet PRIVATE /W4 /permissive-)
  else()
    target_compile_options(lobsim_parquet PRIVATE -Wall -Wextra -Wpedantic)
  endif()

  target_compile_definitions(lobsim_parquet PUBLIC LOBSIM_VERSION="${PROJECT_VERSION}")
  target_link_libraries(lobsim_parquet PRIVATE Arrow::arrow_shared Parquet::parquet_shared)
  
  install(TARGETS lobsim_parquet
    EXPORT lobsimTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
endif()

# ---- Install rules for C++ consumers ----
install(TARGETS lobsim_core
  EXPORT lobsimTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY cpp/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT lobsimTargets
  NAMESPACE lobsim::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lobsim
)

# ---- Tests (Catch2) ----
if(LOBSIM_BUILD_TESTS)
  message(STATUS "LOBSIM_BUILD_TESTS is ON - Configuring tests")
  include(CTest)

  include(FetchContent)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
  )
  FetchContent_MakeAvailable(Catch2)

  add_executable(lobsim_core_tests
    cpp/tests/test_paper_trading_simulator.cpp
    cpp/tests/test_in_memory_sink.cpp
    cpp/tests/test_multibook_simulator.cpp
    cpp/tests/test_public_api.cpp
  )
  target_link_libraries(lobsim_core_tests PRIVATE lobsim::core Catch2::Catch2WithMain)

  # Auto-discover tests for ctest
  include(Catch)
  catch_discover_tests(lobsim_core_tests)
endif()

# ---- Python extension (pybind11) ----
# This builds a module that will be imported as: `import lobsim._core`
if(LOBSIM_BUILD_PYTHON)
  # Force pybind11 to use modern FindPython
  set(PYBIND11_FINDPYTHON ON)

  # Robust fallback: Try finding installed package first, otherwise FetchContent
  find_package(pybind11 CONFIG QUIET)
  if(NOT pybind11_FOUND)
    message(STATUS "pybind11 not found, fetching via FetchContent...")
    include(FetchContent)
    FetchContent_Declare(
      pybind11
      GIT_REPOSITORY https://github.com/pybind/pybind11.git
      GIT_TAG        v2.11.1
    )
    FetchContent_MakeAvailable(pybind11)
  endif()

  # The pybind module target name can be anything; OUTPUT_NAME controls the filename.
  pybind11_add_module(lobsim_py MODULE
    cpp/src/python_bindings.cpp
  )

  target_link_libraries(lobsim_py PRIVATE lobsim::core)

  # Make the compiled module name "_core" so it imports as lobsim._core
  set_target_properties(lobsim_py PROPERTIES
    OUTPUT_NAME "_core"
  )

  # For local (non-wheel) builds, drop the module into python/lobsim/
  # For wheel builds, scikit-build-core will use CMAKE_INSTALL_PREFIX and install() rules.
  set_target_properties(lobsim_py PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/python/lobsim"
  )

  # Install into the Python package namespace "lobsim"
  install(TARGETS lobsim_py
    LIBRARY DESTINATION lobsim
  )
endif()

# ---- Examples ----
if(LOBSIM_BUILD_PARQUET)
  add_executable(lobsim_cpp_example examples/lobsim_cpp.cpp)
  target_link_libraries(lobsim_cpp_example PRIVATE lobsim::core lobsim::parquet)

  add_executable(lobsim_multibook_example examples/lobsim_multibook_cpp.cpp)
  target_link_libraries(lobsim_multibook_example PRIVATE lobsim::core lobsim::parquet)
  
  add_executable(lobsim_bench_cpp benchmark/lobsim_bench_cpp.cpp)
  target_link_libraries(lobsim_bench_cpp PRIVATE lobsim::core lobsim::parquet)
endif()

# add_executable(lobsim_lobster_example examples/lobsim_lobster_cpp.cpp)
# target_link_libraries(lobsim_lobster_example PRIVATE lobsim::core)

add_executable(lobsim_bench_synthetic benchmark/lobsim_bench_synthetic.cpp)
target_link_libraries(lobsim_bench_synthetic PRIVATE lobsim::core)
