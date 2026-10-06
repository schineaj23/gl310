/*
 * gl310life.c - does the ARM core modify DRAM when we release it from reset?
 *
 * The earlier stub test watched a single 32-bit counter, which is only
 * conclusive if we already know where the core's writes land. This does the
 * opposite: it snapshots a wide span of DRAM with the core held, releases the
 * core, snapshots the same span again, and diffs. The real firmware is used, so
 * any execution at all mutates its .data/.bss/stack somewhere in the span.
 *
 *   - no DRAM is written, so the resident firmware image cannot be lost
 *   - no DDR controller or aperture register is touched
 *   - the only state change is the ARM reset line, which is reversible and
 *     has already been shown to toggle 65 status registers cleanly
 *
 * A diff means the core executes and tells us where it writes. No diff over the
 * whole span means the core truly fetches nothing, and the blocker is a clock,
 * a second reset, or a fetch path that never reaches this memory.
 *
 * Build:
 *   clang -O2 -o gl310life gl310life.c -I/opt/homebrew/include \
 *         -L/opt/homebrew/lib -lusb-1.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define VID 0x07ca
#define PID 0xc835
#define EP_CMD_WR 0x04
#define EP_CMD_RD 0x83
#define EP_DMA_RD 0x81
#define TIMEOUT 1000
#define BULK_TIMEOUT 5000

#define CHUNK 32768              /* same size the verify path already uses */

static libusb_device_handle *dev;

static int cmd(const unsigned char *c, int len, unsigned char *rep, int rlen) {
    int n = 0, r = libusb_bulk_transfer(dev, EP_CMD_WR, (unsigned char *)c, len, &n, TIMEOUT);
    if (r || n != len) { fprintf(stderr, "cmd OUT: %s\n", libusb_error_name(r)); return -1; }
    if (rlen <= 0) return 0;
    r = libusb_bulk_transfer(dev, EP_CMD_RD, rep, rlen, &n, TIMEOUT);
    if (r) { fprintf(stderr, "cmd IN: %s\n", libusb_error_name(r)); return -1; }
    return n;
}
static void hdr(unsigned char *b, unsigned char op, unsigned char sub,
                unsigned short cnt, unsigned int arg) {
    b[0]=op; b[1]=sub; b[2]=cnt&0xff; b[3]=(cnt>>8)&0xff;
    b[4]=arg&0xff; b[5]=(arg>>8)&0xff; b[6]=(arg>>16)&0xff; b[7]=(arg>>24)&0xff;
}
static void put32(unsigned char *b, unsigned int v) {
    b[0]=v&0xff; b[1]=(v>>8)&0xff; b[2]=(v>>16)&0xff; b[3]=(v>>24)&0xff;
}
static unsigned int get32(const unsigned char *b) {
    return b[0] | (b[1]<<8) | (b[2]<<16) | ((unsigned)b[3]<<24);
}
static int reset_arm(int run) {
    unsigned char c[8]; hdr(c, 0x07, run ? 1 : 0, 0, 0); return cmd(c, 8, NULL, 0);
}
static int reg_read(unsigned int reg, unsigned int *out) {
    unsigned char c[8], r[4]; hdr(c, 0x01, 0x00, 1, reg);
    if (cmd(c, 8, r, 4) != 4) return -1;
    *out = get32(r); return 0;
}
/* StartDMARead: armaddr and length are in WORDS, payload on bulk IN 0x81. */
static int dma_read(unsigned int byteaddr, unsigned char *buf, int nbytes) {
    unsigned char c[16], st[1]; int n = 0, r;
    hdr(c, 0x09, 0x00, 8, 0);
    put32(c + 8, byteaddr >> 2);
    put32(c + 12, (unsigned)nbytes / 4);
    if (cmd(c, 16, st, 1) != 1) return -1;
    r = libusb_bulk_transfer(dev, EP_DMA_RD, buf, nbytes, &n, BULK_TIMEOUT);
    if (r || n != nbytes) {
        fprintf(stderr, "dma IN @0x%x: %s (%d/%d)\n", byteaddr, libusb_error_name(r), n, nbytes);
        return -1;
    }
    return 0;
}

static int snapshot(unsigned int start, unsigned int len, unsigned char *out) {
    for (unsigned int o = 0; o < len; o += CHUNK) {
        unsigned int n = (len - o < CHUNK) ? (len - o) : CHUNK;
        if (dma_read(start + o, out + o, (int)n)) return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int go = 0, settle_ms = 500, no_reset = 0;
    const char *save_a = NULL, *save_b = NULL;
    unsigned int start = 0x0, len = 0x400000;      /* 4 MB covers both images + slack */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--go")) go = 1;
        else if (!strcmp(argv[i], "--no-reset")) no_reset = 1;
        else if (!strcmp(argv[i], "--save-a") && i + 1 < argc) save_a = argv[++i];
        else if (!strcmp(argv[i], "--save-b") && i + 1 < argc) save_b = argv[++i];
        else if (!strcmp(argv[i], "--start") && i + 1 < argc) start = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--len") && i + 1 < argc) len = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--settle") && i + 1 < argc) settle_ms = atoi(argv[++i]);
    }
    len &= ~(unsigned)(CHUNK - 1);
    if (!len) { fprintf(stderr, "--len too small\n"); return 1; }

    if (!go) {
        printf("Read-only liveness test. Snapshots DRAM 0x%x..0x%x with the ARM held,\n"
               "releases it, waits %d ms, snapshots again, and diffs.\n"
               "No DRAM write, no DDR or aperture register touched - the resident\n"
               "firmware image is safe.\n\nRe-run with --go to proceed.\n",
               start, start + len, settle_ms);
        return 0;
    }

    unsigned char *a = malloc(len), *b = malloc(len);
    if (!a || !b) { fprintf(stderr, "out of memory\n"); return 1; }

    if (libusb_init(NULL) < 0) return 1;
    libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);
    dev = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!dev) { fprintf(stderr, "cannot open %04x:%04x\n", VID, PID); return 1; }
    if (libusb_claim_interface(dev, 0)) { fprintf(stderr, "claim failed\n"); return 1; }
    printf("GL310 claimed. span 0x%06x..0x%06x (%u KiB), settle %d ms\n\n",
           start, start + len, len >> 10, settle_ms);

    unsigned int r400 = 0, r04 = 0;
    if (no_reset) {
        printf("1. (--no-reset) leaving the ARM exactly as it is\n");
        reg_read(0x400, &r400); reg_read(0x004, &r04);
        printf("   reg 0x400=0x%08x  reg 0x004=0x%08x\n", r400, r04);
    } else {
        printf("1. hold ARM in reset\n");
        if (reset_arm(0)) goto out;
        usleep(50000);
        reg_read(0x400, &r400); reg_read(0x004, &r04);
        printf("   reg 0x400=0x%08x  reg 0x004=0x%08x\n", r400, r04);
    }

    printf("2. snapshot A ...\n");
    if (snapshot(start, len, a)) goto out;

    if (no_reset) {
        printf("3. waiting %d ms, touching nothing\n", settle_ms);
    } else {
        printf("3. release ARM\n");
        if (reset_arm(1)) goto out;
        reg_read(0x400, &r400); reg_read(0x004, &r04);
        printf("   reg 0x400=0x%08x  reg 0x004=0x%08x\n", r400, r04);
    }
    usleep((unsigned)settle_ms * 1000);

    printf("4. snapshot B ...\n");
    if (snapshot(start, len, b)) goto out;

    printf("\n5. diff\n");
    unsigned long nwords = len / 4, ndiff = 0;
    unsigned int firstdiff = 0, lastdiff = 0;
    int shown = 0;
    for (unsigned long w = 0; w < nwords; w++) {
        unsigned int va = get32(a + w * 4), vb = get32(b + w * 4);
        if (va == vb) continue;
        unsigned int addr = start + (unsigned)(w * 4);
        if (!ndiff) firstdiff = addr;
        lastdiff = addr;
        ndiff++;
        if (shown < 24) {
            printf("   0x%06x  0x%08x -> 0x%08x\n", addr, va, vb);
            shown++;
        }
    }
    if (ndiff > (unsigned long)shown)
        printf("   ... %lu more\n", ndiff - shown);

    printf("\n   %lu of %lu words changed", ndiff, nwords);
    if (ndiff) printf("  (0x%06x .. 0x%06x)", firstdiff, lastdiff);
    printf("\n\nVERDICT: ");
    if (ndiff)
        printf("the ARM core IS executing. It mutated %lu words, first at 0x%06x.\n"
               "         Follow that region to find where the firmware gets to.\n",
               ndiff, firstdiff);
    else
        printf("nothing in %u KiB of DRAM changed. The core fetches nothing from\n"
               "         this memory, so the blocker is upstream: an ARM clock gate, a\n"
               "         second reset, or a fetch path that does not reach this DRAM.\n",
               len >> 10);

    for (int pass = 0; pass < 2; pass++) {
        const char *path = pass ? save_b : save_a;
        if (!path) continue;
        FILE *f = fopen(path, "wb");
        if (!f) { fprintf(stderr, "cannot write %s\n", path); continue; }
        fwrite(pass ? b : a, 1, len, f);
        fclose(f);
        printf("   snapshot %c -> %s (0x%x bytes at 0x%x)\n", pass ? 'B' : 'A', path, len, start);
    }

    printf("\nARM left released. DRAM was never written.\n");
out:
    libusb_release_interface(dev, 0); libusb_close(dev); libusb_exit(NULL);
    free(a); free(b);
    return 0;
}
