/*
 * gl310init.c - bring up the AVerMedia GL310 (07ca:c835) over libusb.
 *
 * Implements the bring-up sequence recovered in ../RE.md, which the Windows
 * driver logs as:
 *
 *     CQLCodec_FWDownloadAll() checkState(0) verify(1)
 *       1. CUsbCntl_ResetArm() run(0)                        hold ARM in reset
 *       2. QPHCI_ReInit() mode(1) regBase(0x100000) page(0x100000)
 *       3. CQLCodec_InitializeMemory() type(1) size(512Mb)    DDR training
 *       4. CQLCodec_AOSwitch() (1)                            reg 0x50 &= ~0x02
 *       5. CQLCodec_VOSwitch() (0)                            reg 0x50 |=  0x04
 *       6. CQLCodec_SetGPIODefaults() dir(0) val(0)           reg 0x610/0x614 = 0
 *       7. CQLCodec_FWDownload() start(0x100000) size(363832) audio image
 *       8. CQLCodec_FWDownload() start(0x0)      size(454064) video image
 *       9. CUsbCntl_ResetArm() run(1)                         release -> QPSOS
 *
 * Steps 2-6 must run while the core is HELD IN RESET.
 *
 * Step 3 (DDR training) is deliberately NOT implemented. On a card that has had
 * power maintained, the DDR controller config at 0xf00.. and the HCI windows at
 * 0x800.. both survive an ARM reset -- verified by reading them back after a
 * reset cycle. DDR training is only needed after a true power cycle, and its
 * command script is not yet fully recovered (see ../RE.md).
 *
 * Build:
 *   clang -O2 -o gl310init gl310init.c -I/opt/homebrew/include \
 *         -L/opt/homebrew/lib -lusb-1.0
 *
 * Run:
 *   ./gl310init                 dry run: print the plan and the current state
 *   ./gl310init --go            actually perform the bring-up
 *   ./gl310init --check         liveness check only, touches nothing
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

#define TIMEOUT      1000
#define BULK_TIMEOUT 5000
#define CHUNK        32768            /* bytes per DMA chunk, as the driver uses */

#define VID_FW "../vendor/qpvidfwusb.bin"
#define AUD_FW "../vendor/qpaudfwusb.bin"
#define VID_ADDR 0x000000              /* byte address in ARM space */
#define AUD_ADDR 0x100000

static libusb_device_handle *dev;
static int trace = 0;

/* ------------------------------------------------------------------ transport */

static int generic_cmd(const unsigned char *cmd, int len,
                       unsigned char *reply, int reply_len) {
    int n = 0, r;
    if (trace) {
        printf("      -> 0x04:");
        for (int i = 0; i < len && i < 16; i++) printf(" %02x", cmd[i]);
        printf("\n");
    }
    r = libusb_bulk_transfer(dev, EP_CMD_WR, (unsigned char *)cmd, len, &n, TIMEOUT);
    if (r || n != len) {
        fprintf(stderr, "      cmd OUT failed: %s (%d/%d)\n", libusb_error_name(r), n, len);
        return -1;
    }
    if (reply_len <= 0) return 0;
    r = libusb_bulk_transfer(dev, EP_CMD_RD, reply, reply_len, &n, TIMEOUT);
    if (r) {
        fprintf(stderr, "      cmd IN failed: %s\n", libusb_error_name(r));
        return -1;
    }
    return n;
}

static void hdr(unsigned char *b, unsigned char op, unsigned char sub,
                unsigned short count, unsigned int arg) {
    b[0] = op; b[1] = sub;
    b[2] = count & 0xff;        b[3] = (count >> 8) & 0xff;
    b[4] = arg & 0xff;          b[5] = (arg >> 8) & 0xff;
    b[6] = (arg >> 16) & 0xff;  b[7] = (arg >> 24) & 0xff;
}

static void put32(unsigned char *b, unsigned int v) {
    b[0] = v & 0xff; b[1] = (v >> 8) & 0xff;
    b[2] = (v >> 16) & 0xff; b[3] = (v >> 24) & 0xff;
}

/* op 0x01 sub 0: RegisterRead -> u32 */
static int reg_read(unsigned int reg, unsigned int *out) {
    unsigned char c[8], r[4];
    hdr(c, 0x01, 0x00, 1, reg);
    if (generic_cmd(c, 8, r, 4) != 4) return -1;
    *out = (unsigned)r[0] | ((unsigned)r[1] << 8) |
           ((unsigned)r[2] << 16) | ((unsigned)r[3] << 24);
    return 0;
}

/* op 0x01 sub 1: RegisterWrite, 12 bytes, no reply */
static int reg_write(unsigned int reg, unsigned int val) {
    unsigned char c[12];
    hdr(c, 0x01, 0x01, 1, reg);
    put32(c + 8, val);
    return generic_cmd(c, 12, NULL, 0);
}

/* op 0x07: ResetArm. run=0 holds the core, run=1 releases it. */
static int reset_arm(int run) {
    unsigned char c[8];
    hdr(c, 0x07, run ? 1 : 0, 0, 0);
    return generic_cmd(c, 8, NULL, 0);
}

/* op 0x02 sub 0: MemoryRead of one 32-bit word (waddr = byte >> 2) */
static int mem_read(unsigned int byte_addr, unsigned char *out4) {
    unsigned char c[12];
    unsigned int w = byte_addr >> 2;
    hdr(c, 0x02, 0x00, 4, w);
    put32(c + 8, w);
    return generic_cmd(c, 12, out4, 4) == 4 ? 0 : -1;
}

/* op 0x09 sub 1: StartDMAWrite, then the payload on bulk OUT 0x02.
   armaddr and nwords are in 32-bit WORDS. */
static int dma_write(unsigned int armaddr_w, const unsigned char *buf, int nbytes) {
    unsigned char c[16], st[1];
    int n = 0, r;
    if (nbytes & 3) { fprintf(stderr, "dma_write: %d not word-aligned\n", nbytes); return -1; }
    hdr(c, 0x09, 0x01, 8, 0);
    put32(c + 8, armaddr_w);
    put32(c + 12, (unsigned)nbytes / 4);
    if (generic_cmd(c, 16, st, 1) != 1) return -1;
    r = libusb_bulk_transfer(dev, EP_DMA_WR, (unsigned char *)buf, nbytes, &n, BULK_TIMEOUT);
    if (r || n != nbytes) {
        fprintf(stderr, "      dma OUT failed: %s (%d/%d)\n", libusb_error_name(r), n, nbytes);
        return -1;
    }
    return 0;
}

/* op 0x09 sub 0: StartDMARead, then the payload on bulk IN 0x81. */
static int dma_read(unsigned int armaddr_w, unsigned char *buf, int nbytes) {
    unsigned char c[16], st[1];
    int n = 0, r;
    if (nbytes & 3) { fprintf(stderr, "dma_read: %d not word-aligned\n", nbytes); return -1; }
    hdr(c, 0x09, 0x00, 8, 0);
    put32(c + 8, armaddr_w);
    put32(c + 12, (unsigned)nbytes / 4);
    if (generic_cmd(c, 16, st, 1) != 1) return -1;
    r = libusb_bulk_transfer(dev, EP_DMA_RD, buf, nbytes, &n, BULK_TIMEOUT);
    if (r || n != nbytes) {
        fprintf(stderr, "      dma IN failed: %s (%d/%d)\n", libusb_error_name(r), n, nbytes);
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------- bring-up steps */

/* Step 2. Values confirmed by reading the 0x800 block off a working card. */
static int hci_reinit(void) {
    const unsigned int page = 0x100000;
    for (int i = 0; i < 3; i++) {
        unsigned int base  = (unsigned)i * page;
        unsigned int start = base + 0x4000;
        unsigned int end   = start + page - 1;
        if (reg_write(0x81c + i * 0xc, start)) return -1;
        if (reg_write(0x820 + i * 0xc, end))   return -1;
        if (reg_write(0x824 + i * 0xc, base))  return -1;
        printf("      window %d: start=0x%06x end=0x%06x base=0x%06x\n", i, start, end, base);
    }
    if (reg_write(0x840, 0x90003124)) return -1;
    printf("      0x840 = 0x90003124\n");
    return 0;
}

/* Steps 4 and 5: read-modify-write of register 0x50. */
static int ao_switch(int on) {
    unsigned int v;
    if (reg_read(0x50, &v)) return -1;
    unsigned int nv = on ? (v & ~0x2u) : (v | 0x2u);
    printf("      AOSwitch(%d): 0x50 0x%08x -> 0x%08x\n", on, v, nv);
    return reg_write(0x50, nv);
}

static int vo_switch(int on) {
    unsigned int v;
    if (reg_read(0x50, &v)) return -1;
    unsigned int nv = on ? (v & ~0x4u) : (v | 0x4u);
    printf("      VOSwitch(%d): 0x50 0x%08x -> 0x%08x\n", on, v, nv);
    return reg_write(0x50, nv);
}

/* Step 6. */
static int gpio_defaults(void) {
    printf("      0x610 = 0, 0x614 = 0\n");
    return reg_write(0x610, 0) || reg_write(0x614, 0) ? -1 : 0;
}

/* Steps 7 and 8: push an image in CHUNK-sized pieces, reading each back
   (the driver passes verify(1), so every chunk is compared). */
static int fw_download(const char *path, unsigned int byte_addr, int verify) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "      cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *img = malloc((size_t)size);
    if (!img || fread(img, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "      read failed %s\n", path); fclose(f); free(img); return -1;
    }
    fclose(f);
    if (size & 3) { fprintf(stderr, "      %s size %ld not word-aligned\n", path, size);
                    free(img); return -1; }

    printf("      %s: %ld bytes -> ARM byte 0x%06x (word 0x%05x)\n",
           path, size, byte_addr, byte_addr >> 2);

    unsigned char *back = malloc(CHUNK);
    int bad = 0;
    for (long off = 0; off < size; off += CHUNK) {
        int n = (int)((size - off < CHUNK) ? (size - off) : CHUNK);
        unsigned int w = (byte_addr + (unsigned)off) >> 2;
        if (dma_write(w, img + off, n)) { bad = 1; break; }
        if (verify) {
            if (dma_read(w, back, n)) { bad = 1; break; }
            if (memcmp(back, img + off, (size_t)n)) {
                fprintf(stderr, "      VERIFY MISMATCH at ARM word 0x%05x (+0x%lx)\n", w, off);
                bad = 1; break;
            }
        }
        printf("\r        %ld / %ld bytes", off + n, size);
        fflush(stdout);
    }
    printf("\n");
    free(back); free(img);
    return bad ? -1 : 0;
}

/* ------------------------------------------------------------------- liveness */

static void dump_state(const char *tag) {
    unsigned int v;
    printf("  %s\n", tag);
    struct { unsigned int r; const char *n; } regs[] = {
        {0x0004, "ARM reset state (000b6c7d=released, 000a4040=held)"},
        {0x0050, "AO/VO switch"},
        {0x061c, "firmware-written latch"},
        {0x0f04, "DDR timing"},
        {0x081c, "HCI window 0 start"},
        {0x06fc, "TO_ARM_STATUS"},
    };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
        if (!reg_read(regs[i].r, &v))
            printf("    0x%04x = 0x%08x   %s\n", regs[i].r, v, regs[i].n);
}

/* Sample ARM memory twice; any change means the core is executing. */
static int check_alive(void) {
    enum { N = 96 };
    static unsigned char a[N][4], b[N][4];
    const unsigned int bases[] = {0x070000, 0x080000, 0x0f0000};
    int changed = 0;

    for (unsigned k = 0; k < sizeof bases / sizeof bases[0]; k++) {
        for (int i = 0; i < N; i++) mem_read(bases[k] + (unsigned)i * 4, a[i]);
        for (int i = 0; i < N; i++) mem_read(bases[k] + (unsigned)i * 4, b[i]);
        int c = 0;
        for (int i = 0; i < N; i++) if (memcmp(a[i], b[i], 4)) c++;
        printf("    0x%06x: %d/%d words changed\n", bases[k], c, N);
        changed += c;
    }
    /* SW-I2C only works once the ARM services it: status 0x08 = success. */
    unsigned char c9[9] = {0x0c, 0x01, 0x01, 0x00, 0x2a, 0, 0, 0, 0x1b}, rep[2] = {0, 0};
    if (generic_cmd(c9, 9, rep, 2) == 2)
        printf("    SW-I2C sync read -> %02x %02x  (status 0x%02x, %s)\n",
               rep[0], rep[1], rep[1],
               rep[1] == 0x08 ? "SUCCESS - firmware is servicing I2C" : "not serviced");
    return changed;
}

/* ----------------------------------------------------------------------- main */

int main(int argc, char **argv) {
    int go = 0, check_only = 0, verify = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--go")) go = 1;
        else if (!strcmp(argv[i], "--check")) check_only = 1;
        else if (!strcmp(argv[i], "--no-verify")) verify = 0;
        else if (!strcmp(argv[i], "--trace")) trace = 1;
        else {
            fprintf(stderr, "usage: %s [--go] [--check] [--no-verify] [--trace]\n", argv[0]);
            return 2;
        }
    }

    if (libusb_init(NULL) < 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
    libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);
    dev = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!dev) { fprintf(stderr, "cannot open %04x:%04x\n", VID, PID); return 1; }
    if (libusb_claim_interface(dev, 0)) {
        fprintf(stderr, "claim_interface(0) failed\n");
        libusb_close(dev); libusb_exit(NULL); return 1;
    }
    printf("GL310 %04x:%04x, interface 0 claimed.\n\n", VID, PID);

    if (check_only) {
        dump_state("current register state:");
        printf("\n  liveness:\n");
        check_alive();
        goto out;
    }

    dump_state("before:");

    if (!go) {
        printf("\n  DRY RUN. Plan (pass --go to execute):\n"
               "    1. ResetArm(0)                       hold core\n"
               "    2. HCI windows 0x81c/0x820/0x824 x3, then 0x840\n"
               "    3. DDR training                      SKIPPED (config survives reset)\n"
               "    4. AOSwitch(1)                       0x50 &= ~0x02\n"
               "    5. VOSwitch(0)                       0x50 |=  0x04\n"
               "    6. GPIO defaults                     0x610=0 0x614=0\n"
               "    7. download %s -> 0x%06x\n"
               "    8. download %s -> 0x%06x\n"
               "    9. ResetArm(1)                       release core\n"
               "   10. wait 300ms, then liveness check\n", AUD_FW, AUD_ADDR, VID_FW, VID_ADDR);
        goto out;
    }

    printf("\n== 1. ResetArm(0) - hold core ==\n");
    if (reset_arm(0)) goto fail;
    usleep(20000);

    printf("== 2. HCI windows ==\n");
    if (hci_reinit()) goto fail;

    printf("== 3. DDR training - skipped by design ==\n");

    printf("== 4/5. AO / VO switch ==\n");
    if (ao_switch(1)) goto fail;
    if (vo_switch(0)) goto fail;

    printf("== 6. GPIO defaults ==\n");
    if (gpio_defaults()) goto fail;

    printf("== 7. audio firmware ==\n");
    if (fw_download(AUD_FW, AUD_ADDR, verify)) goto fail;

    printf("== 8. video firmware ==\n");
    if (fw_download(VID_FW, VID_ADDR, verify)) goto fail;

    printf("== 9. ResetArm(1) - release core ==\n");
    if (reset_arm(1)) goto fail;

    printf("== 10. waiting 300ms (driver sees QPSOS ~165ms after release) ==\n");
    usleep(300000);

    dump_state("after:");
    printf("\n  liveness:\n");
    if (check_alive() > 0)
        printf("\n  *** ARM IS EXECUTING ***\n");
    else
        printf("\n  no execution detected.\n");

out:
    libusb_release_interface(dev, 0);
    libusb_close(dev);
    libusb_exit(NULL);
    return 0;
fail:
    fprintf(stderr, "\nbring-up aborted.\n");
    libusb_release_interface(dev, 0);
    libusb_close(dev);
    libusb_exit(NULL);
    return 1;
}
