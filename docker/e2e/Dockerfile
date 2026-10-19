# SPDX-License-Identifier: GPL-2.0-only
# Part of maxivmac (GPL v2, see LICENSE).
#
# End-to-end test image: a System 6.0.8 guest in headless maxivmac talking to a
# POSIX PeerTalk peer through ethhub, all in ONE container.  See
# docs/features/NETWORKING_E2E.md.  Build and run with test/e2e/run.sh, which
# assembles the build context (this Dockerfile is not built from the repo root):
#
#   ctx/repo/          this repository for the emulator build (minus bld/, docs, media,
#                      docker/, test/net, test/e2e: editing the test scripts must not
#                      invalidate the emulator build layer)
#   ctx/e2e/           docker/e2e, test/e2e, test/net from this repository
#   ctx/peertalk/      PeerTalk checkout       (PEERTALK_DIR, read-only input)
#   ctx/peertalk-old/  optional second PeerTalk (PEERTALK_DIR_OLD), for the
#                      pre-fix comparison; empty directory when not given
#   ctx/bombertalk/    optional BomberTalk checkout (BOMBERTALK_DIR), for R4; empty = no R4 peer
#   ctx/clog/          clog sources            (CLOG_DIR; nothing is fetched)
#
# libslirp is deliberately NOT installed: the emulator auto-detects it, and this
# test only uses --eth=hub (trixie's libslirp 4.8 is also older than the 4.9 API
# the emulator's slirp backend needs).  There is no hfsutils either (trixie dropped
# it): the host reads PT_Log back from the disk image the run leaves in /out.
#
# Base image: Debian 13 "trixie" slim, pinned by digest.  Why Debian 13: maxivmac
# needs SDL3 and this is the oldest Debian/Ubuntu release that packages it
# (libsdl3-dev 3.2.x), so SDL3 is not built from source.  glibc, not musl: the
# emulator uses the usual Linux GL/X11 libraries SDL3 pulls in.  The digest keeps
# the base reproducible; refresh it deliberately (docker pull debian:trixie-slim).
#
# Nothing proprietary is baked in: no ROM, no disk image, no Apple or
# third-party software.  Those are mounted read-only at run time (/in/...).
ARG BASE=debian:trixie-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f

# ---- stage 1: maxivmac (headless Linux build) + ethhub ----------------------
FROM ${BASE} AS emu-build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config ca-certificates \
        libsdl3-dev libgl-dev libx11-dev \
    && rm -rf /var/lib/apt/lists/*
COPY repo/ /src/
RUN cmake -S /src -B /bld -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /bld --target maxivmac ethhub \
    && strip /bld/maxivmac /bld/ethhub

# ---- stage 2: PeerTalk POSIX test peers (+ optionally the pre-fix ones) -----
FROM ${BASE} AS peer-build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY clog/ /src/clog/
COPY peertalk/ /src/peertalk/
COPY peertalk-old/ /src/peertalk-old/
# FETCHCONTENT_FULLY_DISCONNECTED: clog comes from /src/clog, never the network.
RUN set -eu; mkdir -p /peers; \
    for v in peertalk:new peertalk-old:old; do \
        dir=/src/${v%%:*}; tag=${v##*:}; \
        [ -f "$dir/CMakeLists.txt" ] || continue; \
        cmake -S "$dir" -B "/bld-$tag" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCLOG_DIR=/src/clog -DFETCHCONTENT_FULLY_DISCONNECTED=ON; \
        cmake --build "/bld-$tag" --target test_mesh test_chat test_lifecycle test_multi; \
        mkdir -p "/peers/$tag"; \
        cp "/bld-$tag/test_mesh" "/bld-$tag/test_chat" "/bld-$tag/test_lifecycle" "/bld-$tag/test_multi" "/peers/$tag/"; \
        strip "/peers/$tag"/*; \
    done

# ---- stage 2b: BomberTalk, POSIX/SDL2 build (rung R4; empty context dir = skipped) ----
FROM ${BASE} AS bt-build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential pkg-config libsdl2-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY clog/ /src/clog/
COPY peertalk/ /src/peertalk/
COPY bombertalk/ /src/bombertalk/
COPY e2e/test/e2e/in-container/sdlkeys.c /src/sdlkeys.c
RUN mkdir -p /opt/bombertalk \
    && if [ -f /src/bombertalk/tools/build-sdl.sh ]; then \
        cd /src/bombertalk && PEERTALK_DIR=/src/peertalk CLOG_DIR=/src/clog OUT=/bt-out tools/build-sdl.sh \
        && cp /bt-out/BomberTalk /opt/bombertalk/ && strip /opt/bombertalk/BomberTalk; \
    fi \
    && gcc -shared -fPIC -O2 -Wall -Wextra -Werror -o /opt/bombertalk/sdlkeys.so /src/sdlkeys.c

# ---- stage 3: runtime --------------------------------------------------------
FROM ${BASE}
# iproute2: tap0 set-up (entrypoint, the only privileged step); iputils-ping:
# the in-container `ping` of the guest; python3: the test driver (stdlib
# only); runtime libraries of the headless emulator (SDL3 is linked even for
# --headless; the video driver is "dummy").  util-linux (setpriv) is in the base.
RUN apt-get update && apt-get install -y --no-install-recommends \
        iproute2 iputils-ping python3 libsdl3-0 libsdl2-2.0-0 libgl1 libx11-6 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -g 2010 e2e && useradd -u 2010 -g 2010 -M -d /nonexistent -s /usr/sbin/nologin e2e
COPY --from=emu-build /bld/maxivmac /bld/ethhub /usr/local/bin/
# trap names for the script's `wait text` (project data, not third-party)
COPY --from=emu-build /src/data/debug/ /opt/maxivmac/data/debug/
COPY --from=peer-build /peers /opt/peers
COPY --from=bt-build /opt/bombertalk /opt/bombertalk
COPY e2e/docker/e2e/entrypoint.sh /usr/local/bin/e2e-entrypoint
COPY e2e/test/e2e/in-container/ /opt/e2e/
COPY e2e/test/net/scripts/ /opt/e2e/net-scripts/
COPY e2e/test/net/mactcp-config.py /opt/e2e/
ENV SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy HOME=/tmp MAXIVMAC_ROOT=/opt/maxivmac
ENTRYPOINT ["/usr/local/bin/e2e-entrypoint"]
CMD ["r1"]
