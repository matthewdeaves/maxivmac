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
 *   SDLKEYS_CMDFILE    optional: a file the test driver writes while the program runs, holding
 *                      the SDL scancodes that are down right now, as decimal numbers separated
 *                      by spaces (empty or missing file: none).  Re-read about every 40 ms.
 *                      This is how rung R5 drives the POSIX BomberTalk through a game (arrows
 *                      79 right, 80 left, 81 down, 82 up; 44 space) without changing BomberTalk.
 *
 * No SDL headers are needed: only the one function's signature is replaced.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* scancodes the driver says are down (SDLKEYS_CMDFILE), refreshed every 40 ms */
static unsigned char cmd_down[512];

static void poll_cmdfile(long t)
{
    static long last = -1000;
    const char *path = getenv("SDLKEYS_CMDFILE");
    char buf[256], *p, *e;
    FILE *f;
    size_t n;

    if (!path || t - last < 40) return;
    last = t;
    memset(cmd_down, 0, sizeof cmd_down);
    f = fopen(path, "r");
    if (!f) return;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    for (p = buf; *p; p = e) {
        long v = strtol(p, &e, 10);
        if (e == p) { e = p + 1; continue; }
        if (v >= 0 && v < 512) cmd_down[v] = 1;
    }
}

const Uint8 *SDL_GetKeyboardState(int *numkeys)
{
    static long t0 = -1;
    long t, after = envl("SDLKEYS_AFTER_MS", 5000), hold = envl("SDLKEYS_HOLD_MS", 300);
    int sc = (int)envl("SDLKEYS_SCANCODE", 40);

    if (t0 < 0) t0 = now_ms();
    t = now_ms() - t0;
    poll_cmdfile(t);
    memcpy(keys, cmd_down, sizeof keys);
    if (sc >= 0 && sc < 512 && t >= after && t < after + hold) keys[sc] = 1;
    if (numkeys) *numkeys = 512;
    return keys;
}
