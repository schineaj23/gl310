/*
 * gl310armtest.c - does the ARM core execute code we place in DRAM, and from
 *                  which base address?
 *
 * Rather than keep guessing why QPSOS will not boot, load a trivial ARM stub and
 * watch for it running. The stub is an increment loop:
 *
 *      0x00: E59F0018   ldr r0, [pc, #0x18]   ; r0 = counter address (literal @0x20)
 *      0x04: E3A01000   mov r1, #0
 *      0x08: E5801000   str r1, [r0]          ; counter = 0
 *      0x0c: E5901000   ldr r1, [r0]          ; LOOP
 *      0x10: E2811001   add r1, r1, #1
 *      0x14: E5801000   str r1, [r0]
 *      0x18: EAFFFFFB   b    LOOP
 *      0x1c: E1A00000   nop
 *      0x20: <counter address>
 *
 * An ARM core resets into ARM state with MMU and caches off and fetches from
 * 0x00000000, so a stub at address 0 runs immediately with no setup.
 *
 * Two copies are loaded, each with its own counter:
 *
 *      stub A at our byte 0x0000  -> counter at 0x080000
 *      stub B at our byte 0x4000  -> counter at 0x080010
 *
 * The HCI windows map start = base + 0x4000, so if DMA addresses are offset by
 * 0x4000 relative to what the core fetches, B runs and A does not. Whichever
 * counter moves identifies the core's real fetch base. If neither moves, the
 * core is not executing from DRAM at all and the problem is upstream of the
 * image entirely (clock, reset, or memory not visible to the core).
 *
 * DESTRUCTIVE: this overwrites the first 1 KiB at 0x0 and at 0x4000, so it
 * clobbers the resident firmware image. Restore it with `./gl310init --go`.
 *
 * SUPERSEDED, AND IT LIED TO US. Both verdicts below are untrustworthy:
 *
 *   1. The 1 KiB of zero padding covers offset 0x100, which holds the ASCII
 *      "QSOS" image header. The loader validates that header, so after this runs
 *      the image is not bootable and NOTHING starts - which looks exactly like
 *      "the core executes nothing". That false negative cost a day.
 *   2. The counters sit at 0x080000, which the running firmware never touches,
 *      so even a healthy boot would have moved neither of them.
 *
 * The ARM core does execute, and QPSOS boots in about 8 ms. Use `gl310log` to read
 * the firmware's own log and `gl310life` for a wide read-only DRAM diff; between
 * them there is no reason to run this at all. Kept only for the record.
 *
 * Build:
 *   clang -O2 -o gl310armtest gl310armtest.c -I/opt/homebrew/include \
 *         -L/opt/homebrew/lib -lusb-1.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define VID 0x07ca
#define PID 0xc835
#define EP_DMA_WR 0x02
#define EP_CMD_WR 0x04
#define EP_DMA_RD 0x81
#define EP_CMD_RD 0x83
#define TIMEOUT 1000
#define BULK_TIMEOUT 5000

#define STUB_BYTES 1024          /* zero-padded, comfortably above any DMA minimum */
#define CNT_A 0x080000
#define CNT_B 0x080010

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
static int reset_arm(int run) {
    unsigned char c[8]; hdr(c, 0x07, run?1:0, 0, 0); return cmd(c, 8, NULL, 0);
}
static int reg_read(unsigned int reg, unsigned int *out) {
    unsigned char c[8], r[4]; hdr(c, 0x01, 0x00, 1, reg);
    if (cmd(c, 8, r, 4) != 4) return -1;
    *out = r[0] | (r[1]<<8) | (r[2]<<16) | ((unsigned)r[3]<<24); return 0;
}
static int mem_read(unsigned int ba, unsigned int *out) {
    unsigned char c[12], r[4]; unsigned int w = ba >> 2;
    hdr(c, 0x02, 0x00, 4, w); put32(c+8, w);
    if (cmd(c, 12, r, 4) != 4) return -1;
    *out = r[0] | (r[1]<<8) | (r[2]<<16) | ((unsigned)r[3]<<24); return 0;
}
static int dma_write(unsigned int ba, const unsigned char *buf, int nbytes) {
    unsigned char c[16], st[1]; int n = 0, r;
    hdr(c, 0x09, 0x01, 8, 0); put32(c+8, ba>>2); put32(c+12, (unsigned)nbytes/4);
    if (cmd(c, 16, st, 1) != 1) return -1;
    r = libusb_bulk_transfer(dev, EP_DMA_WR, (unsigned char *)buf, nbytes, &n, BULK_TIMEOUT);
    if (r || n != nbytes) { fprintf(stderr, "dma OUT: %s\n", libusb_error_name(r)); return -1; }
    return 0;
}

static void build_stub(unsigned char *out, unsigned int counter_addr) {
    static const unsigned int code[8] = {
        0xE59F0018u, 0xE3A01000u, 0xE5801000u, 0xE5901000u,
        0xE2811001u, 0xE5801000u, 0xEAFFFFFBu, 0xE1A00000u,
    };
    memset(out, 0, STUB_BYTES);
    for (int i = 0; i < 8; i++) put32(out + i * 4, code[i]);
    put32(out + 0x20, counter_addr);
}

int main(int argc, char **argv) {
    int go = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--go")) go = 1;

    if (!go) {
        printf("DESTRUCTIVE test: overwrites the firmware image at 0x0 and 0x4000.\n"
               "It loads an ARM increment loop and watches whether the core runs it.\n"
               "Restore the firmware afterwards with:  ./gl310init --go\n\n"
               "Re-run with --go to proceed.\n");
        return 0;
    }

    if (libusb_init(NULL) < 0) return 1;
    libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);
    dev = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!dev) { fprintf(stderr, "cannot open %04x:%04x\n", VID, PID); return 1; }
    if (libusb_claim_interface(dev, 0)) { fprintf(stderr, "claim failed\n"); return 1; }
    printf("GL310 claimed.\n\n");

    unsigned char a[STUB_BYTES], b[STUB_BYTES];
    build_stub(a, CNT_A);
    build_stub(b, CNT_B);

    printf("1. hold core in reset\n");
    if (reset_arm(0)) goto fail;
    usleep(20000);

    printf("2. zero both counters\n");
    unsigned char zero[16]; memset(zero, 0, sizeof zero);
    if (dma_write(0x080000, zero, 16)) goto fail;

    printf("3. load stub A at 0x000000 (counter 0x%06x)\n", CNT_A);
    if (dma_write(0x000000, a, STUB_BYTES)) goto fail;
    printf("4. load stub B at 0x004000 (counter 0x%06x)\n", CNT_B);
    if (dma_write(0x004000, b, STUB_BYTES)) goto fail;

    unsigned int v;
    printf("5. read back stub A word 0: ");
    if (!mem_read(0, &v)) printf("0x%08x %s\n", v, v == 0xE59F0018u ? "(correct)" : "(MISMATCH)");

    printf("6. release core\n");
    if (reset_arm(1)) goto fail;

    printf("7. polling both counters for 2 s ...\n\n");
    unsigned int ca0 = 0, cb0 = 0;
    mem_read(CNT_A, &ca0); mem_read(CNT_B, &cb0);
    for (int i = 0; i < 8; i++) {
        usleep(250000);
        unsigned int ca = 0, cb = 0, r4 = 0;
        mem_read(CNT_A, &ca); mem_read(CNT_B, &cb); reg_read(0x04, &r4);
        printf("   t=%4dms  A@0x%06x=0x%08x  B@0x%06x=0x%08x  reg0x04=0x%08x\n",
               (i + 1) * 250, CNT_A, ca, CNT_B, cb, r4);
    }

    unsigned int ca = 0, cb = 0;
    mem_read(CNT_A, &ca); mem_read(CNT_B, &cb);
    printf("\nVERDICT: ");
    if (ca != ca0 && cb == cb0)
        printf("stub A ran. The core fetches from our address 0x0 -- no 0x4000 offset.\n");
    else if (cb != cb0 && ca == ca0)
        printf("stub B ran. The core fetches from our 0x4000 -- the +0x4000 offset IS real,\n"
               "         so the firmware must be loaded 0x4000 higher than we have been.\n");
    else if (ca != ca0 && cb != cb0)
        printf("both counters moved -- unexpected; aliasing or a stale read.\n");
    else
        printf("neither counter moved. The core is not executing from DRAM at all;\n"
               "         the blocker is upstream of the image (clock, reset, or the core\n"
               "         cannot see this memory).\n");

    printf("\nFirmware image is now clobbered. Restore with: ./gl310init --go\n");
    libusb_release_interface(dev, 0); libusb_close(dev); libusb_exit(NULL);
    return 0;
fail:
    fprintf(stderr, "aborted\n");
    libusb_release_interface(dev, 0); libusb_close(dev); libusb_exit(NULL);
    return 1;
}
