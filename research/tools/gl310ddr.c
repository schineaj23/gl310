/*
 * gl310ddr.c - step 3 of the bring-up: DDR init, and a memory test to prove it.
 *
 * This is CQLCodec_InitializeMemory (0x4b930) for this board, which the four config
 * fields pin down as type=1, size=512Mb, field 0x3c8 == 8, field 0x3cc == 0x10020.
 * That combination selects the geometry-autodetect path:
 *
 *      cols = 7; rows = 2
 *      RegisterWrite(0xf14, (rows<<16) | cols)
 *      MemoryWrite(0, (rows<<16) | cols)
 *      while (cols > 3) { MemoryWrite(1 << (cols+6),    cols-1); cols--; }
 *      cols = MemoryRead(0) & 0xf
 *      RegisterWrite(0xf14, (rows<<16) | cols)
 *      MemoryWrite(0, rows)
 *      while (rows > 1) { MemoryWrite(1 << (cols+0x15), rows-1); rows--; }
 *      rows = MemoryRead(0) & 0xf
 *      RegisterWrite(0xf14, (rows<<16) | cols)
 *      RegisterWrite(0xf1c, RegisterRead(0xf1c) & ~0x300)
 *      RegisterWrite(0xf04, 0x0d03110b)
 *      RegisterWrite(0xf08, 0x00000003)
 *      RegisterWrite(0xf40, 0x00000002)
 *      RegisterWrite(0xf10, 0x05140080)
 *      RegisterWrite(0xf18, 0x00000001)
 *      delay 100 ms
 *
 * It probes address aliasing to find the real row/column width: write a value at 0,
 * write a distinct marker at each candidate address line, then read address 0 back -
 * whichever marker landed there says how many address bits physically exist.
 *
 * On a cold card every register in the 0xf00 block already reads the value the vendor
 * writes, except 0xf18, which is 0. So --enable-only tries just the enable bit first;
 * --full runs the whole sequence. Either way the memory test is what decides.
 *
 * Build:
 *   clang -O2 -o gl310ddr gl310ddr.c -I/opt/homebrew/include \
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
#define TIMEOUT 1000

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
static int reg_read(unsigned int reg, unsigned int *out) {
    unsigned char c[8], r[4]; hdr(c, 0x01, 0x00, 1, reg);
    if (cmd(c, 8, r, 4) != 4) return -1;
    *out = get32(r); return 0;
}
static int reg_write(unsigned int reg, unsigned int val) {
    unsigned char c[12]; hdr(c, 0x01, 0x01, 1, reg); put32(c+8, val);
    return cmd(c, 12, NULL, 0);
}
/* MemoryRead/Write take a BYTE address and shift it right by 2 themselves. */
static int mem_read(unsigned int ba, unsigned int *out) {
    unsigned char c[12], r[4]; unsigned int w = ba >> 2;
    hdr(c, 0x02, 0x00, 4, w); put32(c+8, w);
    if (cmd(c, 12, r, 4) != 4) return -1;
    *out = get32(r); return 0;
}
static int mem_write(unsigned int ba, unsigned int val) {
    unsigned char c[16]; unsigned int w = ba >> 2;
    hdr(c, 0x02, 0x01, 4, w); put32(c+8, w); put32(c+12, val);
    return cmd(c, 16, NULL, 0);
}
static int reset_arm(int run) {
    unsigned char c[8]; hdr(c, 0x07, run ? 1 : 0, 0, 0); return cmd(c, 8, NULL, 0);
}

/* Does DRAM actually store what we put in it, at addresses far enough apart to
   catch aliasing? Restores nothing - this runs before any firmware is loaded. */
static int memtest(void) {
    static const unsigned int ADDR[] = {
        0x000000, 0x000004, 0x001000, 0x010000, 0x040000,
        0x100000, 0x400000, 0x1000000, 0x2000000, 0x3000000,
    };
    const int N = (int)(sizeof ADDR / sizeof ADDR[0]);
    unsigned int want[16];
    int bad = 0;

    for (int i = 0; i < N; i++) {
        want[i] = 0xa5000000u ^ (ADDR[i] * 2654435761u);
        if (mem_write(ADDR[i], want[i])) return -1;
    }
    /* read back only after every write, so aliasing shows up as a mismatch */
    for (int i = 0; i < N; i++) {
        unsigned int got = 0;
        if (mem_read(ADDR[i], &got)) return -1;
        int ok = (got == want[i]);
        printf("    0x%08x  wrote 0x%08x  read 0x%08x  %s\n",
               ADDR[i], want[i], got, ok ? "ok" : "MISMATCH");
        if (!ok) bad++;
    }
    return bad;
}

static void show_ddr(const char *when) {
    static const unsigned int R[] = {0xf00,0xf04,0xf08,0xf0c,0xf10,0xf14,0xf18,0xf1c,0xf20,0xf40};
    printf("  DDR block %s:\n   ", when);
    for (int i = 0; i < 10; i++) {
        unsigned int v = 0;
        if (reg_read(R[i], &v)) { printf("  0x%03x=??", R[i]); continue; }
        printf("  0x%03x=0x%08x", R[i], v);
        if (i == 4) printf("\n   ");
    }
    printf("\n");
}

static int ddr_full(void) {
    unsigned int cols = 7, rows = 2, v = 0;

    printf("  geometry autodetect:\n");
    if (reg_write(0xf14, (rows << 16) | cols)) return -1;
    if (mem_write(0, (rows << 16) | cols)) return -1;
    while (cols > 3) {
        if (mem_write(1u << (cols + 6), cols - 1)) return -1;
        cols--;
    }
    if (mem_read(0, &v)) return -1;
    cols = v & 0xf;
    printf("    column probe read 0x%08x -> cols = %u\n", v, cols);
    if (reg_write(0xf14, (rows << 16) | cols)) return -1;

    if (mem_write(0, rows)) return -1;
    while (rows > 1) {
        if (mem_write(1u << (cols + 0x15), rows - 1)) return -1;
        rows--;
    }
    if (mem_read(0, &v)) return -1;
    rows = v & 0xf;
    printf("    row probe    read 0x%08x -> rows = %u\n", v, rows);
    if (reg_write(0xf14, (rows << 16) | cols)) return -1;
    printf("    0xf14 = 0x%08x\n", (rows << 16) | cols);

    if (reg_read(0xf1c, &v)) return -1;
    if (reg_write(0xf1c, v & 0xfffffcffu)) return -1;
    printf("  timings: 0xf04 0xf08 0xf40 0xf10, then enable 0xf18\n");
    if (reg_write(0xf04, 0x0d03110b)) return -1;
    if (reg_write(0xf08, 0x00000003)) return -1;
    if (reg_write(0xf40, 0x00000002)) return -1;   /* type==1 && 0x3cc==0x10020 */
    if (reg_write(0xf10, 0x05140080)) return -1;   /* field 0x3c8 != 4          */
    if (reg_write(0xf18, 0x00000001)) return -1;
    usleep(100000);
    return 0;
}

int main(int argc, char **argv) {
    int go = 0, full = 0, enable_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--go")) go = 1;
        else if (!strcmp(argv[i], "--full")) full = 1;
        else if (!strcmp(argv[i], "--enable-only")) enable_only = 1;
        else { printf("usage: gl310ddr [--enable-only | --full] --go\n"); return 0; }
    }
    if (!go || (!full && !enable_only)) {
        printf("DDR bring-up, step 3. Pick one:\n"
               "  --enable-only   just set 0xf18 = 1 (the cold 0xf00 block already holds\n"
               "                  every other value the vendor writes)\n"
               "  --full          the whole CQLCodec_InitializeMemory sequence, including\n"
               "                  the geometry autodetect\n"
               "Then --go. Safe on a cold card: there is no resident firmware to lose.\n");
        return 0;
    }

    if (libusb_init(NULL) < 0) return 1;
    libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);
    dev = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!dev) { fprintf(stderr, "cannot open %04x:%04x\n", VID, PID); return 1; }
    if (libusb_claim_interface(dev, 0)) { fprintf(stderr, "claim failed\n"); return 1; }

    printf("Holding the ARM in reset first.\n");
    if (reset_arm(0)) goto out;
    usleep(20000);

    show_ddr("before");
    printf("\n  memory test before:\n");
    int before = memtest();
    printf("  -> %d of 10 addresses bad\n\n", before);

    if (enable_only) {
        printf("Setting 0xf18 = 1.\n");
        if (reg_write(0xf18, 1)) goto out;
        usleep(100000);
    } else {
        printf("Running the full InitializeMemory sequence.\n");
        if (ddr_full()) goto out;
    }

    printf("\n");
    show_ddr("after");
    printf("\n  memory test after:\n");
    int after = memtest();
    printf("  -> %d of 10 addresses bad\n", after);

    printf("\nVERDICT: ");
    if (after == 0)
        printf("DRAM works. Load the firmware with:  ./gl310init --go\n");
    else if (after < before)
        printf("improved (%d -> %d bad) but not clean. Try --full if you used\n"
               "         --enable-only.\n", before, after);
    else
        printf("still %d bad. DDR is not up.\n", after);
out:
    libusb_release_interface(dev, 0); libusb_close(dev); libusb_exit(NULL);
    return 0;
}
