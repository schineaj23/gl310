/*
 * gl310aperture.c - measure the HCI paging aperture's translation function.
 *
 * QPHCI_ReInit (0x4aca0) programs three descriptors with a 0xc stride:
 *
 *      base[i]  = i * page                 -> reg 0x824 + i*0xc
 *      start[i] = base[i] + 0x4000         -> reg 0x81c + i*0xc
 *      end[i]   = start[i] + page - 1      -> reg 0x820 + i*0xc
 *
 * with page = 0x100000, which on the live card reads back as
 *
 *      0x081c=0x00004000  0x0820=0x00103fff  0x0824=0x00000000
 *      0x0828=0x00104000  0x082c=0x00203fff  0x0830=0x00100000
 *      0x0834=0x00204000  0x0838=0x00303fff  0x083c=0x00200000
 *
 * If those windows translate host addresses as `phys = addr - start + base`,
 * then everything the host writes lands 0x4000 BELOW where it thinks it does,
 * and the ARM - which fetches its reset vector from physical 0 - never sees the
 * image. Host-side read-back cannot detect this, because reads go through the
 * very same translation and are therefore self-consistent.
 *
 * This measures the translation directly instead: move window 0's start from
 * 0x4000 to 0x8000 and see whether our view of DRAM slides by 0x4000.
 *
 *      translation applies  ->  after the change, our 0x8000 reads file[0]
 *      aperture not in path ->  our 0x8000 still reads file[0x8000]
 *
 * Only the three window registers are touched, never DRAM, and they are
 * restored unconditionally before exit. The resident firmware image is not
 * modified, so nothing here can cost us the warm state.
 *
 * Build:
 *   clang -O2 -o gl310aperture gl310aperture.c -I/opt/homebrew/include \
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

#define W0_START 0x81c
#define W0_END   0x820
#define W0_BASE  0x824

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
/* MemoryRead: byte address, shifted >>2 internally, waddr sent twice. */
static int mem_read(unsigned int ba, unsigned int *out) {
    unsigned char c[12], r[4]; unsigned int w = ba >> 2;
    hdr(c, 0x02, 0x00, 4, w); put32(c+8, w);
    if (cmd(c, 12, r, 4) != 4) return -1;
    *out = get32(r); return 0;
}

/* First words of qpvidfwusb.bin at a few offsets, so a shifted view is obvious. */
struct marker { unsigned int off, word; const char *what; };
static const struct marker MARKERS[] = {
    { 0x00000, 0xe59ff018, "vector table (file+0)" },
    { 0x04000, 0xe3a0082f, "file+0x4000" },
    { 0x08000, 0xda000000, "file+0x8000" },
    { 0x0c000, 0x00000000, "file+0xc000" },
};
#define NMARK (int)(sizeof MARKERS / sizeof MARKERS[0])

static void probe(const char *label) {
    printf("  %-22s", label);
    for (int i = 0; i < NMARK; i++) {
        unsigned int v = 0;
        if (mem_read(MARKERS[i].off, &v)) { printf("  <read failed>"); continue; }
        printf("  0x%08x", v);
    }
    printf("\n");
}

static void identify(unsigned int addr) {
    unsigned int v = 0;
    if (mem_read(addr, &v)) { printf("    our 0x%06x: read failed\n", addr); return; }
    const char *id = "unrecognised";
    for (int i = 0; i < NMARK; i++)
        if (v == MARKERS[i].word) { id = MARKERS[i].what; break; }
    printf("    our 0x%06x = 0x%08x  -> %s\n", addr, v, id);
}

int main(int argc, char **argv) {
    int go = 0;
    unsigned int newstart = 0x8000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--go")) go = 1;
        else if (!strcmp(argv[i], "--start") && i + 1 < argc) newstart = strtoul(argv[++i], 0, 0);
    }
    if (!go) {
        printf("Moves HCI window 0's start register (0x81c) to 0x%x, re-reads DRAM,\n"
               "then restores it. DRAM contents and the DDR controller are untouched,\n"
               "so the resident firmware image cannot be lost.\n\n"
               "Re-run with --go to proceed.\n", newstart);
        return 0;
    }

    if (libusb_init(NULL) < 0) return 1;
    libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);
    dev = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!dev) { fprintf(stderr, "cannot open %04x:%04x\n", VID, PID); return 1; }
    if (libusb_claim_interface(dev, 0)) { fprintf(stderr, "claim failed\n"); return 1; }

    unsigned int s0 = 0, e0 = 0, b0 = 0;
    if (reg_read(W0_START, &s0) || reg_read(W0_END, &e0) || reg_read(W0_BASE, &b0)) {
        fprintf(stderr, "cannot read window 0\n"); goto out;
    }
    printf("window 0 as found:  start=0x%08x end=0x%08x base=0x%08x\n\n", s0, e0, b0);

    printf("                          our 0x0     our 0x4000  our 0x8000  our 0xc000\n");
    probe("baseline");

    printf("\nmoving window 0 start 0x%x -> 0x%x (end follows, base unchanged)\n",
           s0, newstart);
    if (reg_write(W0_START, newstart) || reg_write(W0_END, newstart + (e0 - s0))) {
        fprintf(stderr, "window write failed\n"); goto restore;
    }
    unsigned int chk = 0;
    reg_read(W0_START, &chk);
    printf("  0x81c reads back 0x%08x %s\n", chk,
           chk == newstart ? "(took)" : "(DID NOT TAKE)");

    probe("after shift");
    printf("\n  interpreting:\n");
    identify(0x0);
    identify(newstart);

    printf("\n  verdict: ");
    unsigned int at_new = 0;
    mem_read(newstart, &at_new);
    if (at_new == MARKERS[0].word)
        printf("the aperture DOES translate. our 0x%x now reads file[0], so host\n"
               "           addresses are offset by -start, and the image we load at our 0x0\n"
               "           is NOT at physical 0 where the ARM fetches its reset vector.\n",
               newstart);
    else if (at_new == 0xda000000)
        printf("no shift: our 0x%x still reads file[0x8000]. The window registers are\n"
               "           not in this read path, so the aperture is not the blocker.\n", newstart);
    else
        printf("inconclusive: our 0x%x reads 0x%08x, neither the shifted nor the\n"
               "           unshifted value. Treat the window semantics as unknown.\n",
               newstart, at_new);

restore:
    printf("\nrestoring window 0 ... ");
    if (reg_write(W0_START, s0) || reg_write(W0_END, e0) || reg_write(W0_BASE, b0)) {
        printf("FAILED - rerun gl310init --go to reprogram the aperture\n");
    } else {
        unsigned int a = 0, b = 0, c = 0;
        reg_read(W0_START, &a); reg_read(W0_END, &b); reg_read(W0_BASE, &c);
        printf("start=0x%08x end=0x%08x base=0x%08x %s\n", a, b, c,
               (a == s0 && b == e0 && c == b0) ? "(restored)" : "(MISMATCH)");
        probe("after restore");
    }
out:
    libusb_release_interface(dev, 0); libusb_close(dev); libusb_exit(NULL);
    return 0;
}
