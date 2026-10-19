/* SPDX-License-Identifier: GPL-2.0-only
 * Part of maxivmac (GPL v2, see LICENSE).
 *
 * LD_PRELOAD shim for headless SDL2 programs that only read SDL_GetKeyboardState
 * (BomberTalk's SDL build): with SDL_VIDEODRIVER=dummy nothing can press a key,
 * so this reports one scancode as held for a window of wall-clock time.
 *
 *   SDLKEYS_SCANCODE   SDL scancode to hold         (default 40 = Return)
 *   SDLKEYS_AFTER_MS   start of the press, ms after the first call (default 5000)
 *   SDLKEYS_HOLD_MS    how long it is held          (default 300)
 *
 * No SDL headers are needed: only the one function's signature is replaced.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <time.h>

typedef unsigned char Uint8;
static Uint8 keys[512];

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static long envl(const char *n, long d)
{
    const char *v = getenv(n);
    return v ? atol(v) : d;
}

const Uint8 *SDL_GetKeyboardState(int *numkeys)
{
    static long t0 = -1;
    long t, after = envl("SDLKEYS_AFTER_MS", 5000), hold = envl("SDLKEYS_HOLD_MS", 300);
    int sc = (int)envl("SDLKEYS_SCANCODE", 40);

    if (t0 < 0) t0 = now_ms();
    t = now_ms() - t0;
    if (sc >= 0 && sc < 512) keys[sc] = (t >= after && t < after + hold) ? 1 : 0;
    if (numkeys) *numkeys = 512;
    return keys;
}
