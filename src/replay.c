/* replay.c - replay the captured Windows HPD start sequence on Linux via libft4222 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

typedef void *FT_HANDLE;
typedef unsigned char u8;
typedef unsigned int u32;

extern u32 FT_OpenEx(void *arg1, u32 flags, FT_HANDLE *h);
extern u32 FT_Close(FT_HANDLE h);
extern u32 FT4222_SetClock(FT_HANDLE h, int clk);
extern u32 FT_SetLatencyTimer(FT_HANDLE h, u8 ms);
extern u32 FT_SetUSBParameters(FT_HANDLE h, u32 inSize, u32 outSize);
extern u32 FT_SetTimeouts(FT_HANDLE h, u32 rd, u32 wr);
extern u32 FT4222_SPIMaster_Init(FT_HANDLE h, int ioLine, int clkDiv, int cpol, int cpha, u8 ssoMap);
extern u32 FT4222_SPIMaster_SingleReadWrite(FT_HANDLE h, u8 *rd, u8 *wr, uint16_t size, uint16_t *sizeTransferred, int isEnd);
extern u32 FT4222_GPIO_Init(FT_HANDLE h, u8 *dirs);
extern u32 FT4222_GPIO_Read(FT_HANDLE h, int port, u8 *val);
extern u32 FT4222_GPIO_Write(FT_HANDLE h, int port, u8 val);
extern u32 FT4222_SetSuspendOut(FT_HANDLE h, int enable);
extern u32 FT4222_SetWakeUpInterrupt(FT_HANDLE h, int enable);
extern u32 FT4222_SetInterruptTrigger(FT_HANDLE h, int trigger);

#define FT_OPEN_BY_DESCRIPTION 2
#define DEFAULT_CYCLE_US 50000

typedef struct { double t; int len; u8 b[600]; } Step;
static Step steps[4096];
static int nsteps;

static long now_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

static int load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror("open seq"); return -1; }
    char line[4096];
    while (fgets(line, sizeof line, f) && nsteps < 4096) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        Step *s = &steps[nsteps];
        s->t = atof(line);
        char *hx = tab + 1;
        int n = 0;
        while (*hx && *hx != '\n' && n < 600) {
            while (*hx == ' ' || *hx == '\r' || *hx == '\n') hx++;
            if (!*hx) break;
            unsigned v; if (sscanf(hx, "%2x", &v) != 1) break;
            s->b[n++] = (u8)v; hx += 2;
        }
        s->len = n;
        if (n) nsteps++;
    }
    fclose(f);
    return nsteps;
}

static FT_HANDLE dev;
static u8 rd[1024], wr[1024];
static int steady = 0;

static long t_base = 0;
static long xfer_us_total = 0;
static void wait_until(long us_off) {
    long t = t_base + us_off;
    long d;
    while ((d = t - now_us()) > 0) {
        if (d > 2000) usleep((useconds_t)(d - 1000));
        else { struct timespec ts = {0, 200000}; nanosleep(&ts, 0); }
    }
}

static int retryable(u8 op) {
    return op == 0x11 || op == 0x15 || op == 0x10 || op == 0x14 || op == 0x05;
}

static int all_zero(const u8 *b, int n) {
    for (int i = 0; i < n; i++) if (b[i]) return 0;
    return 1;
}

static int xfer(u8 *w, int len, u8 *r) {
    long _xs = now_us();
    memset(rd, 0, sizeof rd);
    memcpy(wr, w, len);
    uint16_t got = 0;
    u32 st = 0;
    const char *e_tries = getenv("RD_TRIES"), *e_del = getenv("RD_DELAY_US");
    const char *st_tries = getenv("ST_TRIES"), *st_retry = getenv("ST_RETRY");
    int tries = (retryable(w[0]) && !steady) ? (e_tries ? atoi(e_tries) : 40) : 1;
    long delay = e_del ? atol(e_del) : 2000;
    if (steady) {
        const char *sd = getenv("ST_DELAY_US");
        if (sd) delay = atol(sd);
        if (steady && w[0] == 0x05 && st_retry && st_retry[0] == '1') tries = st_tries ? atoi(st_tries) : 10;
        else tries = 1;
    }
    for (int t = 0; t < tries; t++) {
        if (delay > 0) usleep((useconds_t)delay);
        memset(rd, 0, sizeof rd);
        got = 0;
        st = FT4222_SPIMaster_SingleReadWrite(dev, rd, wr, (uint16_t)len, &got, 1);
        if (st) break;
        if (!retryable(w[0])) break;
        if (!all_zero(rd, len)) break;
    }
    if (r) memcpy(r, rd, len);
    xfer_us_total += now_us() - _xs;
    return (int)st;
}

static void hexdump(const char *tag, u8 *b, int n) {
    printf("%s", tag);
    for (int i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}

int main(int argc, char **argv) {
    const char *seq = argc > 1 ? argv[1] : "seq.txt";
    int cycles = argc > 2 ? atoi(argv[2]) : 60;
    if (load(seq) < 0) return 1;
    printf("steps=%d\n", nsteps);

    int idx_lposc = -1, idx_cfg_end = -1, idx_cycle_start = -1, idx_cycle_end = -1;
    for (int i = 0; i < nsteps; i++) {
        if (steps[i].len == 3 && steps[i].b[0] == 0x8f && steps[i].b[1] == 0x0f) idx_lposc = i;
        if (steps[i].len == 4 && steps[i].b[0] == 0x8f && steps[i].b[1] == 0x0e) idx_cfg_end = i;
    }
    /* cycle = first 0x8f10 after cfg end, through the 0x05 read */
    for (int i = idx_cfg_end + 1; i < nsteps; i++)
        if (steps[i].len == 2 && steps[i].b[0] == 0x8f && steps[i].b[1] == 0x10) { idx_cycle_start = i; break; }
    for (int i = idx_cycle_start; i < nsteps; i++)
        if (steps[i].b[0] == 0x05) { idx_cycle_end = i; break; }
    printf("lposc=%d cfg_end=%d cycle=%d..%d\n", idx_lposc, idx_cfg_end, idx_cycle_start, idx_cycle_end);

    u32 st = FT_OpenEx((void *)"FT4222 A", FT_OPEN_BY_DESCRIPTION, &dev);
    if (st) { fprintf(stderr, "FT_OpenEx failed %u\n", st); return 2; }
    printf("opened\n");

    printf("SetLatency=%u\n", FT_SetLatencyTimer(dev, 1));
    printf("SetUSBParams=%u\n", FT_SetUSBParameters(dev, 65536, 65536));
    printf("SetTimeouts=%u\n", FT_SetTimeouts(dev, 1000, 1000));
    st = FT4222_SetClock(dev, 3);            printf("SetClock=%u\n", st);
    st = FT4222_SPIMaster_Init(dev, 1, 4, 0, 0, 1); printf("SPI_Init=%u\n", st);
    u8 dirs[4] = {1, 1, 0, 1};
    st = FT4222_GPIO_Init(dev, dirs);        printf("GPIO_Init=%u\n", st);
    FT4222_SetSuspendOut(dev, 0);
    FT4222_SetWakeUpInterrupt(dev, 1);
    FT4222_SetInterruptTrigger(dev, 8);

    u8 v; u8 tmp[64];
    FT4222_GPIO_Write(dev, 2, 1); usleep(2000);
    FT4222_GPIO_Write(dev, 2, 0); usleep(10000);
    tmp[0] = 0; FT4222_GPIO_Read(dev, 3, &v);
    FT4222_GPIO_Write(dev, 2, 1); usleep(2000);
    FT4222_GPIO_Read(dev, 3, &v);
    printf("port3=%d\n", v);

    /* replay up to and including the run-mode write */
    t_base = now_us();
    double t0s = steps[0].t;
    for (int i = 0; i <= idx_cfg_end; i++) {
        if (getenv("REPLAY_TIMING")) wait_until((long)((steps[i].t - t0s) * 1e6));
        u8 r[600]; memset(r, 0, sizeof r);
        int rc = xfer(steps[i].b, steps[i].len, r);
        if (rc) { fprintf(stderr, "xfer %d failed rc=%d\n", i, rc); break; }
        if (steps[i].b[0] == 0x11) hexdump("VERIFY-> ", r, steps[i].len);
        if (steps[i].b[0] == 0x8f && steps[i].b[1] == 0x0f)
            { hexdump("LPOSC resp ", r, steps[i].len); usleep(145000); }
        if (i == idx_cfg_end) hexdump("RUNMODE resp ", r, steps[i].len);
    }

    usleep(33000);   /* Windows idle between 8f0e0201 and first cycle */
    /* steady-state cycles */
    steady = 1;
    long t0 = now_us(), prev = t0;
    long minf = 1 << 30, maxf = 0;
    int ok = 0; long low = 0, high = 0;
    FILE *raw = fopen("/tmp/frames.bin", "wb");
    for (int c = 0; c < cycles; c++) {
        long cs = now_us();
        int got = 0;
        double ct0 = steps[idx_cycle_start].t;
        for (int i = idx_cycle_start; i <= idx_cycle_end; i++) {
            if (getenv("REPLAY_TIMING")) {
                double span = (steps[idx_cycle_end].t - ct0);
                double scale = span > 0 ? (DEFAULT_CYCLE_US * 0.98) / (span * 1e6) : 1.0;
                long off = (long)((steps[i].t - ct0) * 1e6 * scale);
                while (now_us() - cs < off) { struct timespec ts = {0, 200000}; nanosleep(&ts, 0); }
            }
            u8 r[600]; memset(r, 0, sizeof r);
            int rc = xfer(steps[i].b, steps[i].len, r);
            if (rc) { fprintf(stderr, "cycle xfer failed rc=%d\n", rc); goto done; }
            if (steps[i].b[0] == 0x05 && steps[i].len > 20) {
                got = 1;
                if (raw) fwrite(r, 1, steps[i].len, raw);
                if (c < 3) hexdump("FRAME ", r, steps[i].len < 40 ? steps[i].len : 40);
            }
        }
        { u8 pv = 0; FT4222_GPIO_Read(dev, 3, &pv); if (pv == 0) low++; else high++; }
        long now = now_us();
        long dt = now - prev; prev = now;
        if (dt < minf) minf = dt;
        if (dt > maxf) maxf = dt;
        if (got) ok++;
        if (c == 0) { long e = now - cs; printf("first cycle %ld us\n", e); }
        { const char *cy = getenv("CYCLE_US"); long target = cy ? atol(cy) : DEFAULT_CYCLE_US;
          long left = target - (now_us() - cs);
          while (left > 0) { usleep((useconds_t)(left > 1000 ? left - 500 : left)); left = target - (now_us() - cs); } }
    }
done:
    if (raw) fclose(raw);
    long tot = now_us() - t0;
    printf("cycles_ok=%d total=%ld us rate=%.2f Hz min=%ld max=%ld avg=%ld us\n",
           ok, tot, ok * 1e6 / (double)tot, minf, maxf, ok ? tot / ok : 0);
    printf("port3 low=%ld high=%ld xfer_time_per_cycle=%.1f us\n", low, high, ok ? (double)xfer_us_total / ok : 0);
    FT4222_UnInitialize(dev);
    FT_Close(dev);
    return 0;
}
