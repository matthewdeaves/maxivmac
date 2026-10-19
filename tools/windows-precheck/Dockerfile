# Cross-compile toolchain for the Windows build precheck (see ../windows-precheck.sh).
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl git cmake ninja-build pkg-config make xz-utils \
      gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix mingw-w64-tools \
      clang lld \
    && rm -rf /var/lib/apt/lists/*
ARG SDL_VERSION=3.2.14
RUN curl -fsSL https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL3-${SDL_VERSION}.tar.gz | tar xz -C /tmp
COPY *.cmake /tmp/
RUN cmake -S /tmp/SDL3-${SDL_VERSION} -B /tmp/sdl-bld -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=/tmp/mingw-toolchain.cmake -DCMAKE_INSTALL_PREFIX=/opt/sdl3-mingw \
      -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_SHARED=ON -DSDL_STATIC=OFF \
 && cmake --build /tmp/sdl-bld && cmake --install /tmp/sdl-bld && rm -rf /tmp/sdl-bld /tmp/SDL3-*
