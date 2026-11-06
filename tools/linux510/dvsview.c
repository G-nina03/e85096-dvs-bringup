// SPDX-License-Identifier: GPL-2.0
/*
 * dvsview: S5KRC1S compressed AER packets -> event frames on /dev/fb0 at a fixed frame rate (11.27).
 *
 * ov5647-cap.py --live --raw8 hands every VC0 buffer to dvs_ingest() through ctypes (the GIL is released
 * for the call): the buffer is copied out of the uncached dma-buf, decoded into a ring of (t, x, y,
 * polarity) events and zeroed. A render thread draws, every 1/fps s, the events of one frame period by
 * *sensor timestamp*, a playout delay behind real time. A VC0 buffer holds ~70 ms of events in a sparse
 * scene and buffers come ~14/s (11.25), so drawing per buffer can not reach 30 fps; drawing by
 * timestamp shows a new 33 ms slice every frame, ~one buffer period late.
 *
 * Packets (32-bit big-endian, manual Fig. 49-52):
 *   [31] = 1        EVENT GROUP, [30:26] offset (0 = SGROUP), [24:18] group address (8 rows each):
 *                   SGROUP [7:0] ON, [15:8] OFF; MGROUP [7:0] group1, [15:8] group1 + offset,
 *                   polarity bit 16 (group1) / 17 (group2), 1 = OFF
 *   [31:26] 000001  COLUMN, [10:0] x
 *   [31:26] 000010  TIMESTAMP, [23] = 0: reference [21:0] ms, [23] = 1: sub [9:0] us
 *   others (frame end, IMU, padding, the undefined 000000/000101 of 11.23) are skipped.
 *
 * Build (Debian 12 arm64, same glibc as the board):
 *   docker run --rm -v "$PWD/tools/linux510:/s" debian:12 sh -c 'apt-get -qq update && apt-get -qq install -y gcc >/dev/null &&
 *     gcc -O2 -Wall -shared -fPIC -o /s/libdvsview.so /s/dvsview.c -lpthread &&
 *     gcc -O2 -Wall -DDVSVIEW_TEST -o /tmp/t /s/dvsview.c -lpthread && /tmp/t [capture.raw]'
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define XS 960
#define YS 720
#define RING (1u << 22)			/* 4M events x 8 B: > 200 ms of a buffer full of events at 40 buffers/s */
#define MARGIN 15000			/* us of slack on top of the measured playout delay */
#define STALL 300000			/* data this much later than the delay: restart the playout clock */
#define BLACK 0xff000000u		/* XRGB8888, alpha set as in ov5647-cap.py */
#define ON_RGB 0xff00ff00u
#define OFF_RGB 0xffff0000u
#define BEFORE(a, b) ((int32_t)((a) - (b)) < 0)	/* sensor-time compare on the 32-bit (71 min) clock */

struct ev {
	uint32_t t;			/* display clock, us */
	uint16_t x;
	uint16_t y;			/* bit 15: OFF */
};

static struct {
	struct ev *ring;
	_Atomic uint64_t wr;		/* events written; ring slot = index % RING */
	int col;			/* current column, -1 before the first COLUMN packet */
	uint32_t ref;			/* last reference timestamp, ms */
	int have_ts, bad;
	int64_t raw;			/* last accepted ref * 1000 + sub, us */
	int64_t t;			/* display clock: sum of the accepted forward steps */
	uint8_t *copy;
	size_t copy_len;
	/* playout: show sensor time s at real time s + delay */
	_Atomic int64_t delay;
	_Atomic int64_t anchor;		/* >= 0: restart the render clock at this sensor time */
	_Atomic int64_t newest;		/* display clock after the last buffer */
	_Atomic int started;
	/* stats, written by one thread each, read loosely by the render thread */
	_Atomic uint64_t buffers, ingest_us, ingest_max, bytes;
	/* frame buffer */
	uint8_t *dst;			/* top-left of the centred XS x YS window */
	size_t stride;
	uint32_t *back;
	int64_t period, window;
} S = { .col = -1, .anchor = -1 };

static int64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

/* Timestamps carry CRC-error bit flips (11.24: sub bit 8 dropped, lost ref packets): accept a step of
 * 0..100 ms forward, skip others; four rejects in a row mean a real jump (counter reset at K, a gap). */
static void timestamp(int64_t raw)
{
	int64_t dt = raw - S.raw;

	if (!S.have_ts || (dt >= 0 && dt < 100000)) {
		S.t += S.have_ts ? dt : 0;
		S.raw = raw;
		S.have_ts = 1;
		S.bad = 0;
	} else if (++S.bad >= 4) {
		S.raw = raw;
		S.bad = 0;
	}
}

static uint64_t put(uint64_t wr, uint32_t t, uint32_t addr, uint32_t bits, uint32_t off)
{
	while (bits) {
		uint32_t y = addr * 8 + __builtin_ctz(bits);
		struct ev *e;

		if (y >= YS)
			break;			/* bits climb: the rest of the group is off the array too */
		e = &S.ring[wr++ % RING];
		e->t = t;
		e->x = S.col;
		e->y = y | off << 15;
		bits &= bits - 1;
	}
	return wr;
}

static void decode(const uint32_t *p, size_t n)
{
	uint64_t wr = atomic_load_explicit(&S.wr, memory_order_relaxed);
	uint32_t t = S.t;

	for (size_t i = 0; i < n; i++) {
		uint32_t w = __builtin_bswap32(p[i]);

		if (w >> 31) {
			uint32_t off = w >> 26 & 0x1f, a1 = w >> 18 & 0x7f;

			if (S.col < 0 || S.col >= XS)
				continue;
			if (!off) {
				wr = put(wr, t, a1, w & 0xff, 0);
				wr = put(wr, t, a1, w >> 8 & 0xff, 1);
			} else {
				wr = put(wr, t, a1, w & 0xff, w >> 16 & 1);
				wr = put(wr, t, a1 + off, w >> 8 & 0xff, w >> 17 & 1);
			}
			continue;
		}
		switch (w >> 26) {
		case 1:
			S.col = w & 0x7ff;
			break;
		case 2:
			if (w >> 23 & 1) {
				timestamp((int64_t)S.ref * 1000 + (w & 0x3ff));
				t = S.t;
			} else {
				S.ref = w & 0x3fffff;
			}
			break;
		}
	}
	atomic_store_explicit(&S.wr, wr, memory_order_release);
}

int dvs_ingest(uint8_t *buf, size_t len)
{
	int64_t t0 = now_us(), s_first, d, delay;
	size_t n = len & ~(size_t)3;

	if (len > S.copy_len) {
		free(S.copy);
		S.copy = malloc(len);
		S.copy_len = S.copy ? len : 0;
		if (!S.copy)
			return -ENOMEM;
	}
	memcpy(S.copy, buf, n);			/* one sequential read of the uncached dma-buf */
	while (n && !*(uint32_t *)(S.copy + n - 4))
		n -= 4;				/* past the last packet the buffer is still zero */
	memset(buf, 0, n);			/* bytesused is 0: clear so these packets do not come back */

	s_first = S.t;
	decode((const uint32_t *)S.copy, n / 4);
	atomic_store(&S.newest, S.t);

	/* the oldest event of this buffer became visible now: the delay has to cover it */
	d = now_us() - s_first;
	delay = atomic_load(&S.delay);
	if (!atomic_load(&S.started) || d > delay + STALL) {
		delay = d;
		atomic_store(&S.anchor, s_first);
		atomic_store(&S.started, 1);
	} else if (d > delay) {
		delay = d;
	} else {
		delay -= (delay - d) >> 8;	/* creep down, ~20 s time constant at 14 buffers/s */
	}
	atomic_store(&S.delay, delay);

	d = now_us() - t0;
	atomic_fetch_add(&S.buffers, 1);
	atomic_fetch_add(&S.bytes, n);
	atomic_fetch_add(&S.ingest_us, d);
	if ((uint64_t)d > atomic_load(&S.ingest_max))
		atomic_store(&S.ingest_max, d);
	return 0;
}

static void *render(void *arg)
{
	uint64_t a = 0, b = 0;			/* events of the window: ring indices [a, b) */
	int64_t prev = INT64_MIN, frame_max = 0, frame_sum = 0, shown = 0, lag = 0, t_log = now_us();
	unsigned frames = 0;
	struct timespec next;

	clock_gettime(CLOCK_MONOTONIC, &next);
	for (;;) {
		int64_t t0, end, anchor;
		uint64_t wr;

		next.tv_nsec += S.period * 1000;
		while (next.tv_nsec >= 1000000000) {
			next.tv_nsec -= 1000000000;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
		if (!atomic_load(&S.started))
			continue;

		t0 = now_us();
		end = t0 - atomic_load(&S.delay) - MARGIN;
		anchor = atomic_exchange(&S.anchor, -1);
		if (anchor >= 0)
			prev = anchor - S.window;	/* first buffer or after a stall: start from its data */
		if (end < prev)
			end = prev;			/* the delay grew: hold, never step back in sensor time */
		wr = atomic_load_explicit(&S.wr, memory_order_acquire);
		if (wr - a > RING)
			a = wr - RING;			/* the decoder lapped us: those events are gone */
		if (b < a)
			b = a;
		while (b < wr && BEFORE(S.ring[b % RING].t, (uint32_t)end))
			b++;
		while (a < b && BEFORE(S.ring[a % RING].t, (uint32_t)(end - S.window)))
			a++;

		for (int i = 0; i < XS * YS; i++)
			S.back[i] = BLACK;
		for (uint64_t i = a; i < b; i++) {
			const struct ev *e = &S.ring[i % RING];

			S.back[(e->y & 0x7fff) * XS + e->x] = e->y >> 15 ? OFF_RGB : ON_RGB;
		}
		for (int y = 0; y < YS; y++)
			memcpy(S.dst + y * S.stride, S.back + y * XS, XS * 4);
		prev = end;

		t0 = now_us() - t0;
		frame_sum += t0;
		if (t0 > frame_max)
			frame_max = t0;
		shown += b - a;
		lag += atomic_load(&S.newest) - end;
		if (++frames == 2 * 1000000 / S.period) {
			int64_t now = now_us(), dt = now - t_log;
			uint64_t nb = atomic_exchange(&S.buffers, 0);

			fprintf(stderr, "[dvsview] %.1f fps, draw %.1f ms (max %.1f), %.0f events/frame | "
				"%.1f buffers/s, ingest %.1f ms (max %.1f), %llu kB/buffer | %.0f ms behind the newest event\n",
				frames * 1e6 / dt, frame_sum / 1e3 / frames, frame_max / 1e3, (double)shown / frames,
				nb * 1e6 / dt, nb ? atomic_exchange(&S.ingest_us, 0) / 1e3 / nb : 0.0,
				atomic_exchange(&S.ingest_max, 0) / 1e3,
				nb ? (unsigned long long)(atomic_exchange(&S.bytes, 0) / nb) >> 10 : 0ULL,
				lag / 1e3 / frames);
			frames = 0;
			frame_sum = frame_max = shown = lag = 0;
			t_log = now;
		}
	}
	return arg;
}

static int alloc_ring(void)
{
	if (!S.ring)
		S.ring = malloc(sizeof(*S.ring) * RING);
	return S.ring ? 0 : -ENOMEM;
}

/* Map fbdev, centre the XS x YS window on the visible page, start drawing fps frames/s. */
int dvs_start(const char *fbdev, int fps)
{
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	pthread_t th;
	uint8_t *fb;
	int fd;

	if (fps < 1 || fps > 240 || alloc_ring())
		return -EINVAL;
	fd = open(fbdev, O_RDWR);
	if (fd < 0)
		return -errno;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f) ||
	    v.bits_per_pixel != 32 || v.xres < XS || v.yres < YS)
		return -EINVAL;
	fb = mmap(NULL, (size_t)f.line_length * v.yres_virtual, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED)
		return -errno;
	S.stride = f.line_length;
	S.dst = fb + (size_t)(v.yoffset + (v.yres - YS) / 2) * S.stride + (v.xres - XS) / 2 * 4;
	S.back = malloc(XS * YS * 4);
	if (!S.back)
		return -ENOMEM;
	S.period = 1000000 / fps;
	S.window = S.period;			/* each event is drawn in exactly one frame */
	return -pthread_create(&th, NULL, render, NULL);
}

#ifdef DVSVIEW_TEST
static int nev(void)
{
	return atomic_load(&S.wr);
}

static void reset(void)
{
	S.col = -1;
	S.have_ts = S.bad = 0;
	S.raw = S.t = 0;
	S.ref = 0;
	atomic_store(&S.wr, 0);
}

static void feed(const uint32_t *w, size_t n)
{
	uint32_t be[16];

	for (size_t i = 0; i < n; i++)
		be[i] = __builtin_bswap32(w[i]);
	decode(be, n);
}

int main(int argc, char **argv)
{
	/* column 5; SGROUP addr 2: ON bit 0, OFF bit 7; MGROUP offset 1 addr 3: group1 ON bit 1, group2 OFF bit 2 */
	static const uint32_t groups[] = { 0x04000005, 0x80080001 | 0x8000, 0x84000000 | 3 << 18 | 1 << 17 | 0x0402 };
	static const struct { uint16_t x, y; } want[] = { { 5, 16 }, { 5, 23 | 1 << 15 }, { 5, 25 }, { 5, 34 | 1 << 15 } };
	/* ref 3 ms + sub 717 us, then ref 4 + sub 17 (+300 us), a sub that goes back (rejected), sub 511 (+494) */
	static const uint32_t ts[] = { 0x08000003, 0x088002cd, 0x08000004, 0x08800011, 0x08800005, 0x088001ff };

	if (alloc_ring())
		return 1;
	feed(groups, 3);
	if (nev() != 4)
		return fprintf(stderr, "events %d, want 4\n", nev()), 1;
	for (int i = 0; i < 4; i++)
		if (S.ring[i].x != want[i].x || S.ring[i].y != want[i].y)
			return fprintf(stderr, "event %d: %u,%#x\n", i, S.ring[i].x, S.ring[i].y), 1;
	reset();
	feed(ts, 4);
	if (S.t != 300)
		return fprintf(stderr, "t %lld after +300 us\n", (long long)S.t), 1;
	feed(ts + 4, 2);
	if (S.t != 794)
		return fprintf(stderr, "t %lld after a bad and a +494 us timestamp\n", (long long)S.t), 1;
	puts("selftest ok");

	if (argc > 1) {				/* decode speed and event count on a capture file */
		FILE *fp = fopen(argv[1], "rb");
		static uint8_t buf[8192 * 127];
		size_t len = fp ? fread(buf, 1, sizeof(buf), fp) : 0;
		int64_t t0;
		const int rounds = 20;

		if (!len)
			return perror(argv[1]), 1;
		reset();
		decode((const uint32_t *)buf, len / 4);
		printf("%s: %zu B, %d events\n", argv[1], len, nev());
		t0 = now_us();
		for (int i = 0; i < rounds; i++) {
			reset();
			decode((const uint32_t *)buf, len / 4);
		}
		printf("decode %.2f ms per buffer\n", (now_us() - t0) / 1e3 / rounds);
	}
	return 0;
}
#endif
