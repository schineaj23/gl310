/*
 * gl310probe.c - validate the recovered GL310 command protocol on real hardware.
 *
 * Implements CUsbCntl_GenericCmd as documented in ../notes/RE.md:
 *     bulk OUT EP 0x04 <- command bytes
 *     bulk IN  EP 0x83 -> reply bytes (raw, no framing)
 *
 * Command header (8 bytes, little-endian):
 *     u8 op; u8 sub; u16 count; u32 arg;   [+ op-specific payload]
 *
 * SAFETY: the default battery is READ-ONLY. Every command it sends has sub=0
 * and is a register/speed read. Anything that writes, resets the ARM, or starts
 * a DMA requires --allow-write, and --raw refuses write-shaped opcodes without
 * it. Nothing here uploads firmware or touches the ARM reset line.
 *
 * Build:
 *   clang -O2 -o gl310probe gl310probe.c -I/opt/homebrew/include \
 *         -L/opt/homebrew/lib -lusb-1.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb-1.0/libusb.h>

#define VID 0x07ca
#define PID 0xc835

#define EP_DMA_WR 0x02 /* pipe 0 */
#define EP_CMD_WR 0x04 /* pipe 1 */
#define EP_DMA_RD 0x81 /* pipe 2 */
#define EP_CMD_RD 0x83 /* pipe 3 */

#define TIMEOUT 1000

static libusb_device_handle *dev;
static int allow_write = 0;
static int verbose = 1;

static void hexdump(const char *label, const unsigned char *b, int n) {
    printf("%s", label);
    for (int i = 0; i < n; i++) printf(" %02x", b[i]);
    printf("\n");
}

/* CUsbCntl_GenericCmd: write cmd on EP 0x04, optionally read reply on EP 0x83. */
static int generic_cmd(const unsigned char *cmd, int cmd_len,
                       unsigned char *reply, int reply_len) {
    int xfered = 0, r;

    if (verbose) hexdump("    -> 0x04:", cmd, cmd_len);

    r = libusb_bulk_transfer(dev, EP_CMD_WR, (unsigned char *)cmd, cmd_len,
                             &xfered, TIMEOUT);
    if (r != 0) {
        printf("    !! OUT failed: %s\n", libusb_error_name(r));
        return r;
    }
    if (xfered != cmd_len) {
        printf("    !! OUT short: %d of %d\n", xfered, cmd_len);
        return LIBUSB_ERROR_IO;
    }
    if (reply_len <= 0) return 0;

    memset(reply, 0, reply_len);
    r = libusb_bulk_transfer(dev, EP_CMD_RD, reply, reply_len, &xfered, TIMEOUT);
    if (r != 0) {
        printf("    !! IN failed: %s\n", libusb_error_name(r));
        return r;
    }
    if (verbose) {
        char lab[32];
        snprintf(lab, sizeof lab, "    <- 0x83 (%d):", xfered);
        hexdump(lab, reply, xfered);
    }
    return xfered;
}

static void put_hdr(unsigned char *b, unsigned char op, unsigned char sub,
                    unsigned short count, unsigned int arg) {
    b[0] = op;
    b[1] = sub;
    b[2] = count & 0xff;
    b[3] = (count >> 8) & 0xff;
    b[4] = arg & 0xff;
    b[5] = (arg >> 8) & 0xff;
    b[6] = (arg >> 16) & 0xff;
    b[7] = (arg >> 24) & 0xff;
}

/* --- read-only primitives ------------------------------------------------ */

static int get_usb_speed(unsigned char *out) {
    unsigned char c[8], r[1];
    put_hdr(c, 0x14, 0x00, 1, 0);
    int n = generic_cmd(c, 8, r, 1);
    if (n == 1) *out = r[0];
    return n;
}

static int read_hci_register(unsigned int reg, unsigned char *out) {
    unsigned char c[8], r[1];
    put_hdr(c, 0x00, 0x00, 1, reg);
    int n = generic_cmd(c, 8, r, 1);
    if (n == 1) *out = r[0];
    return n;
}

static int register_read(unsigned int reg, unsigned int *out) {
    unsigned char c[8], r[4];
    put_hdr(c, 0x01, 0x00, 1, reg);
    int n = generic_cmd(c, 8, r, 4);
    if (n == 4)
        *out = (unsigned)r[0] | ((unsigned)r[1] << 8) | ((unsigned)r[2] << 16) |
               ((unsigned)r[3] << 24);
    return n;
}

static int register_read_ex(unsigned int reg, int count, unsigned char *out) {
    unsigned char c[8];
    put_hdr(c, 0x01, 0x00, (unsigned short)count, reg);
    return generic_cmd(c, 8, out, 4 * count);
}

/* --- battery -------------------------------------------------------------- */

static void drain_stale_in(void) {
    /* If a previous run left a reply queued, clear it so replies stay aligned. */
    unsigned char junk[512];
    int xfered = 0;
    verbose = 0;
    while (libusb_bulk_transfer(dev, EP_CMD_RD, junk, sizeof junk, &xfered, 50) == 0
           && xfered > 0) {
        printf("  (drained %d stale byte(s) from EP 0x83)\n", xfered);
    }
    verbose = 1;
}

static void battery(void) {
    unsigned char b;
    unsigned int v;
    int n;

    printf("\n== T1: GetUSBSpeed  op 0x14 ==\n");
    printf("   RE.md predicts: 8-byte cmd `14 00 01 00 00 00 00 00`, 1-byte reply\n");
    n = get_usb_speed(&b);
    if (n == 1) {
        printf("   RESULT speed code = 0x%02x  %s\n", b,
               b == 2 ? "(2 = high speed, matches 480Mbps enumeration)"
                      : "(interpretation unknown)");
    } else {
        printf("   RESULT no reply (n=%d)\n", n);
    }

    printf("\n== T2: ReadHciRegister  op 0x00  (1-byte HCI reads) ==\n");
    unsigned int hci[] = {0x0, 0x4, 0x8, 0xc, 0x10, 0x100000};
    for (unsigned i = 0; i < sizeof hci / sizeof hci[0]; i++) {
        printf("  reg 0x%06x:\n", hci[i]);
        if (read_hci_register(hci[i], &b) == 1)
            printf("    RESULT 0x%02x\n", b);
    }

    printf("\n== T3: RegisterRead  op 0x01  (4-byte reads) ==\n");
    unsigned int regs[] = {0x0, 0x4, 0x8, 0x100000, 0x100004};
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++) {
        printf("  reg 0x%06x:\n", regs[i]);
        if (register_read(regs[i], &v) == 4)
            printf("    RESULT 0x%08x\n", v);
    }

    printf("\n== T4: RegisterReadEx  op 0x01 count=4  (expect 16 bytes) ==\n");
    unsigned char big[64];
    n = register_read_ex(0x0, 4, big);
    printf("   RESULT %d bytes%s\n", n,
           n == 16 ? "  <- count field confirmed: reply scales 4*n" : "");

    printf("\n== T5: repeatability of T1 (same cmd twice) ==\n");
    unsigned char b1 = 0, b2 = 0;
    get_usb_speed(&b1);
    get_usb_speed(&b2);
    printf("   RESULT 0x%02x then 0x%02x  %s\n", b1, b2,
           b1 == b2 ? "<- stable, replies are aligned with commands"
                    : "<- DIFFERENT: replies may be off by one");
}

/* --- ARM memory reads (op 0x02, read-only) -------------------------------- */

/* MemoryRead: `02 00 04 00` waddr:u32 waddr:u32 -> 4 bytes.
   waddr is a 32-bit WORD address, i.e. byte_addr >> 2. */
static int mem_read(unsigned int byte_addr, unsigned char *out4) {
    unsigned char c[12];
    unsigned int w = byte_addr >> 2;
    put_hdr(c, 0x02, 0x00, 4, w);
    c[8]  = w & 0xff;
    c[9]  = (w >> 8) & 0xff;
    c[10] = (w >> 16) & 0xff;
    c[11] = (w >> 24) & 0xff;
    return generic_cmd(c, 12, out4, 4);
}

static void mem_dump(unsigned int start, unsigned int nwords) {
    unsigned char w[4];
    verbose = 0;
    for (unsigned int i = 0; i < nwords; i++) {
        unsigned int a = start + i * 4;
        if (i % 8 == 0) printf("%s%08x:", i ? "\n" : "", a);
        if (mem_read(a, w) == 4) printf(" %02x%02x%02x%02x", w[0], w[1], w[2], w[3]);
        else printf(" --------");
    }
    printf("\n");
}

/* Sample one word every `stride` bytes over [start,end), twice, and report any
   word that changed. A single change proves the ARM core is executing. */
static void mem_watch(unsigned int start, unsigned int end, unsigned int stride) {
    unsigned int n = (end - start + stride - 1) / stride;
    unsigned char *a = malloc(n * 4), *b = malloc(n * 4);
    unsigned char w[4];
    int reads = 0, nonzero = 0, changed = 0;

    verbose = 0;
    printf("pass 1: %u samples, 0x%06x..0x%06x stride 0x%x\n", n, start, end, stride);
    for (unsigned int i = 0; i < n; i++) {
        memset(a + i * 4, 0, 4);
        if (mem_read(start + i * stride, w) == 4) { memcpy(a + i * 4, w, 4); reads++; }
    }
    printf("pass 2 ...\n");
    for (unsigned int i = 0; i < n; i++) {
        memset(b + i * 4, 0, 4);
        mem_read(start + i * stride, w);
        memcpy(b + i * 4, w, 4);
    }
    for (unsigned int i = 0; i < n; i++) {
        if (a[i*4] || a[i*4+1] || a[i*4+2] || a[i*4+3]) nonzero++;
        if (memcmp(a + i * 4, b + i * 4, 4)) {
            if (changed < 24)
                printf("  CHANGED 0x%06x: %02x%02x%02x%02x -> %02x%02x%02x%02x\n",
                       start + i * stride,
                       a[i*4], a[i*4+1], a[i*4+2], a[i*4+3],
                       b[i*4], b[i*4+1], b[i*4+2], b[i*4+3]);
            changed++;
        }
    }
    printf("\n%d/%u reads ok, %d non-zero, %d CHANGED\n", reads, n, nonzero, changed);
    printf(changed ? "=> words are moving: the ARM core IS executing\n"
                   : "=> nothing moved: no sign of a running ARM\n");
    free(a); free(b);
}

/* --- raw mode ------------------------------------------------------------- */

static int hexparse(const char *s, unsigned char *out, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == ',' || *s == ':') s++;
        if (!*s) break;
        char h[3] = {0, 0, 0};
        if (!s[0] || !s[1]) return -1;
        h[0] = s[0];
        h[1] = s[1];
        out[n++] = (unsigned char)strtol(h, NULL, 16);
        s += 2;
    }
    return n;
}

int main(int argc, char **argv) {
    const char *raw = NULL;
    int raw_reply = 0, do_battery = 1;

    unsigned int md_start = 0, md_words = 0, mw_start = 0, mw_end = 0, mw_stride = 0;
    int do_memdump = 0, do_memwatch = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--allow-write")) allow_write = 1;
        else if (!strcmp(argv[i], "--raw") && i + 2 < argc) {
            raw = argv[++i];
            raw_reply = atoi(argv[++i]);
            do_battery = 0;
        } else if (!strcmp(argv[i], "--memdump") && i + 2 < argc) {
            md_start = (unsigned)strtoul(argv[++i], NULL, 0);
            md_words = (unsigned)strtoul(argv[++i], NULL, 0);
            do_memdump = 1; do_battery = 0;
        } else if (!strcmp(argv[i], "--memwatch") && i + 3 < argc) {
            mw_start  = (unsigned)strtoul(argv[++i], NULL, 0);
            mw_end    = (unsigned)strtoul(argv[++i], NULL, 0);
            mw_stride = (unsigned)strtoul(argv[++i], NULL, 0);
            do_memwatch = 1; do_battery = 0;
        } else if (!strcmp(argv[i], "--quiet")) verbose = 0;
        else {
            fprintf(stderr,
                    "usage: %s [options]\n"
                    "  (no args)                        read-only validation battery\n"
                    "  --raw \"<hexbytes>\" <reply_len>   send one command\n"
                    "  --memdump <addr> <nwords>        ARM memory hex dump (op 0x02)\n"
                    "  --memwatch <start> <end> <stride>  sample twice, report changes\n"
                    "  --allow-write                    permit write/reset/DMA opcodes\n"
                    "  --quiet                          suppress per-transfer tracing\n",
                    argv[0]);
            return 2;
        }
    }

    if (libusb_init(NULL) < 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
    libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);

    dev = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!dev) {
        fprintf(stderr,
                "Could not open %04x:%04x.\n"
                "  - Is the card plugged in?  (ioreg -p IOUSB | grep -i aver)\n"
                "  - Another process may hold it; try quitting Chrome.\n",
                VID, PID);
        libusb_exit(NULL);
        return 1;
    }

    int r = libusb_claim_interface(dev, 0);
    if (r != 0) {
        fprintf(stderr, "claim_interface(0) failed: %s\n", libusb_error_name(r));
        fprintf(stderr, "  Something else has the interface open.\n");
        libusb_close(dev);
        libusb_exit(NULL);
        return 1;
    }
    printf("Opened %04x:%04x, interface 0 claimed.\n", VID, PID);
    printf("Pipes: cmd_wr=0x%02x cmd_rd=0x%02x dma_wr=0x%02x dma_rd=0x%02x\n",
           EP_CMD_WR, EP_CMD_RD, EP_DMA_WR, EP_DMA_RD);

    drain_stale_in();

    if (do_memdump) {
        mem_dump(md_start, md_words);
    } else if (do_memwatch) {
        mem_watch(mw_start, mw_end, mw_stride);
    } else if (do_battery) {
        battery();
    } else {
        unsigned char cmd[512];
        int n = hexparse(raw, cmd, sizeof cmd);
        if (n <= 0) { fprintf(stderr, "bad --raw hex\n"); goto out; }
        /* refuse write-shaped commands unless explicitly allowed */
        int writeish = (n >= 2 && cmd[1] == 0x01) ||
                       (n >= 1 && (cmd[0] == 0x07 || cmd[0] == 0x09));
        if (writeish && !allow_write) {
            fprintf(stderr,
                    "refusing: op 0x%02x sub 0x%02x looks like a write/reset/DMA.\n"
                    "pass --allow-write if you really mean it.\n",
                    cmd[0], n >= 2 ? cmd[1] : 0);
            goto out;
        }
        unsigned char rep[4096];
        if (raw_reply > (int)sizeof rep) raw_reply = sizeof rep;
        generic_cmd(cmd, n, rep, raw_reply);
    }

out:
    libusb_release_interface(dev, 0);
    libusb_close(dev);
    libusb_exit(NULL);
    return 0;
}
