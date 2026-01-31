FROM condaforge/mambaforge:23.11.0-0

SHELL ["/bin/bash", "-lc"]

RUN mamba install -y -c conda-forge \
    cmake \
    ninja \
    cxx-compiler \
    boost-cpp \
    zlib \
    arrow-cpp \
  && mamba clean -a -y

ENV CMAKE_PREFIX_PATH=/opt/conda

WORKDIR /app
COPY . /app

RUN cmake -S . -B build -G Ninja \
    -DLOBSIM_BUILD_TESTS=OFF \
    -DLOBSIM_BUILD_PYTHON=OFF

RUN cmake --build build --target lobsim_udp_engine --target lobsim_event_generator

EXPOSE 1234/udp

ENTRYPOINT ["/app/build/lobsim_udp_engine"]
