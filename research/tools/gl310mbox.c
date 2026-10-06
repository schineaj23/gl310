/*
 * gl310mbox.c - the host<->ARM mailbox, recovered from the driver.
 *
 * Host -> ARM (written in this order):
 *      0x6f8 0x6f4 0x6f0 0x6ec 0x6e8 0x6e4 0x6e0 0x6dc 0x6d8   params 1..9
 *      0x6cc   REG_TO_ARM_MESSAGE_STATUS = (taskId<<16) | (needAck<<8) | 1
 *      0x6fc   REG_TO_ARM_MESSAGE        = (taskId<<16) | cmd
 *
 * Bit 0 of 0x6cc is the doorbell. The host sets it; the ARM clears it when it has
 * taken the message. QPFWAPI_MailboxReady (0x5b020) is exactly a poll of that bit
 * with a 500 ms timeout, so the bit clearing is our proof that the firmware is
 * listening - and it is the only acknowledgement we get for a command with no reply.
 *
 * ARM -> host:
 *      0x6b0   message word
 *      0x6c8   status word
 *      0x6b4 0x6b8 0x6bc 0x6c0 0x6c4                            params 1..5
 *
 * Command codes (from the QPFW*API_ wrappers; cmd 0x10 is "set property", with the
 * property selector in param 1):
 *
 *   0x01 StartEncoder      0x02 StopEncoder       0x03 PauseEncoder
 *   0x04 ResumeEncoder     0x07 GetViosdTableAddr 0x08 GetCurVidBufInfo
 *   0x0a SetAudioInVolume  0x0c InsertUserData    0x10 SetProperty
 *   0x11 SetEncMode        0x12 GetAFrame
 *   0x81 StartDecoder      0x82 StopDecoder       0x86 Flush
 *   0x87 GetPlayInfo       0x88 SetAudioOutVolume 0xb0 GetVouOsdMem
 *   0xf1 SystemOpen        0xf2 SystemLink        0xf3 SystemClose
 *
 * Reading the mailbox is harmless. Sending needs --go. Recovery from anything that
 * upsets the firmware is a reset plus `gl310init --go`, which reboots QPSOS from the
 * resident image in about 8 ms; there is no flash, so nothing survives a reboot.
 *
 * Build:
 *   clang -O2 -o gl310mbox gl310mbox.c -I/opt/homebrew/include \
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

#define R_TO_ARM_STATUS  0x6cc
#define R_TO_ARM_MSG     0x6fc
#define R_FROM_ARM_MSG   0x6b0
#define R_FROM_ARM_STAT  0x6c8
#define TICKS            0x064dc4     /* firmware's Update Tick Thread counter */

static const unsigned int PARAM_REG[9] =
    { 0x6f8, 0x6f4, 0x6f0, 0x6ec, 0x6e8, 0x6e4, 0x6e0, 0x6dc, 0x6d8 };
static const unsigned int FROM_ARM_PARAM[5] = { 0x6b4, 0x6b8, 0x6bc, 0x6c0, 0x6c4 };

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
static int mem_read(unsigned int ba, unsigned int *out) {
    unsigned char c[12], r[4]; unsigned int w = ba >> 2;
    hdr(c, 0x02, 0x00, 4, w); put32(c+8, w);
    if (cmd(c, 12, r, 4) != 4) return -1;
    *out = get32(r); return 0;
}

static void show_state(const char *when) {
    unsigned int v, t;
    printf("  %s:\n", when);
    if (!reg_read(R_TO_ARM_STATUS, &v))
        printf("    0x6cc TO_ARM_STATUS   = 0x%08x   doorbell=%u\n", v, v & 1);
    if (!reg_read(R_TO_ARM_MSG, &v))
        printf("    0x6fc TO_ARM_MESSAGE  = 0x%08x   cmd=0x%02x task=%u\n",
               v, v & 0xff, v >> 16);
    if (!reg_read(R_FROM_ARM_STAT, &v))
        printf("    0x6c8 FROM_ARM_STATUS = 0x%08x\n", v);
    if (!reg_read(R_FROM_ARM_MSG, &v))
        printf("    0x6b0 FROM_ARM_MSG    = 0x%08x   cmd=0x%02x\n", v, v & 0xff);
    printf("    from-ARM params        =");
    for (int i = 0; i < 5; i++) {
        if (!reg_read(FROM_ARM_PARAM[i], &v)) printf(" 0x%08x", v);
    }
    printf("\n");
    if (!mem_read(TICKS, &t)) printf("    tick counter           = %u\n", t);
}

/* Poll the doorbell until the ARM clears it. Returns ms waited, or -1 on timeout. */
static int wait_doorbell(int timeout_ms) {
    for (int ms = 0; ms <= timeout_ms; ms += 5) {
        unsigned int v;
        if (reg_read(R_TO_ARM_STATUS, &v)) return -1;
        if (!(v & 1)) return ms;
        usleep(5000);
    }
    return -1;
}

int main(int argc, char **argv) {
    int go = 0, send = -1, task = 0, need_ack = 0, nparam = 0;
    unsigned int params[9] = {0};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--go")) go = 1;
        else if (!strcmp(argv[i], "--ack")) need_ack = 1;
        else if (!strcmp(argv[i], "--send") && i + 1 < argc) send = (int)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--task") && i + 1 < argc) task = (int)strtoul(argv[++i], 0, 0);
        else if (!strncmp(argv[i], "--p", 3) && argv[i][3] >= '1' && argv[i][3] <= '9'
                 && i + 1 < argc) {
            int k = argv[i][3] - '1';
            params[k] = (unsigned)strtoul(argv[++i], 0, 0);
            if (k + 1 > nparam) nparam = k + 1;
        } else {
            printf("usage: gl310mbox                      dump mailbox state (read-only)\n"
                   "       gl310mbox --send CMD [--task N] [--p1 V ... --p9 V] [--ack] --go\n");
            return 0;
        }
    }

    if (libusb_init(NULL) < 0) return 1;
    libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);
    dev = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!dev) { fprintf(stderr, "cannot open %04x:%04x\n", VID, PID); return 1; }
    if (libusb_claim_interface(dev, 0)) { fprintf(stderr, "claim failed\n"); return 1; }

    if (send < 0) { show_state("mailbox"); goto out; }

    if (!go) {
        printf("Would send cmd 0x%02x task %d", send, task);
        for (int i = 0; i < nparam; i++) printf(" p%d=0x%x", i + 1, params[i]);
        printf("%s\n\nRe-run with --go to actually send.\n", need_ack ? " (ack requested)" : "");
        goto out;
    }

    show_state("before");

    printf("\n  waiting for the mailbox to be free ...\n");
    unsigned int st = 0;
    if (reg_read(R_TO_ARM_STATUS, &st)) goto out;
    if (st & 1) {
        int w = wait_doorbell(500);
        if (w < 0) { fprintf(stderr, "    doorbell still set after 500 ms - aborting\n"); goto out; }
        printf("    cleared after %d ms\n", w);
    } else {
        printf("    already free\n");
    }

    for (int i = 0; i < nparam; i++) {
        printf("  param %d -> 0x%03x = 0x%08x\n", i + 1, PARAM_REG[i], params[i]);
        if (reg_write(PARAM_REG[i], params[i])) goto out;
    }

    unsigned int status = ((unsigned)task << 16) | ((need_ack ? 1u : 0u) << 8) | 1u;
    unsigned int msg    = ((unsigned)task << 16) | (unsigned)(send & 0xffff);
    unsigned int chk = 0;

    printf("  0x6cc <- 0x%08x   (status, doorbell set)\n", status);
    if (reg_write(R_TO_ARM_STATUS, status)) goto out;
    /* The status write alone does not trigger the ARM - writing the message
       register does. Showing the bit still set here is what proves that. */
    if (!reg_read(R_TO_ARM_STATUS, &chk))
        printf("    0x6cc reads 0x%08x, doorbell=%u  (status alone does not trigger)\n",
               chk, chk & 1);
    printf("  0x6fc <- 0x%08x   (cmd 0x%02x, task %d)  <- this is the trigger\n",
           msg, send, task);
    if (reg_write(R_TO_ARM_MSG, msg)) goto out;

    printf("\n  polling the doorbell ...\n");
    int waited = wait_doorbell(1000);
    if (waited >= 0)
        printf("    *** the ARM cleared the doorbell after %d ms - it consumed the message\n",
               waited);
    else
        printf("    doorbell still set after 1000 ms - the firmware did not take it\n");

    printf("\n");
    show_state("after");
out:
    libusb_release_interface(dev, 0); libusb_close(dev); libusb_exit(NULL);
    return 0;
}
