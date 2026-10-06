/*
 * gl310recover.c - get the command channel back after it wedges.
 *
 * The command channel is a bare request/response pair on two bulk pipes with no
 * framing and no sequence numbers (see CUsbCntl_GenericCmd). That means it has no
 * way to resynchronise itself: if a reply is ever left unread, or a transfer is
 * abandoned mid-flight, every later read returns the *previous* command's reply, or
 * times out. Starting the encoder makes this easy to hit, because the firmware is
 * then also pushing data on the DMA-in pipe.
 *
 * Escalates gently, stopping as soon as a register read answers:
 *   1. drain any stale bytes sitting in the two IN pipes
 *   2. clear_halt on all four endpoints
 *   3. ResetArm toggle - an OUT-only command, so it can still land when the IN
 *      path is dead; this reboots QPSOS from the resident image in about 8 ms
 *   4. USB-level device reset, only with --hard   <-- SEE WARNING
 *
 * After step 3 the firmware is freshly booted, so check it with `gl310log`.
 *
 * WARNING about step 4, learned the hard way. On macOS libusb_reset_device() left
 * this device in a state where it still enumerates correctly - ioreg shows
 * Aver_C835_USB, 07ca:c835, registered/matched/active - but libusb_open() first
 * returned LIBUSB_ERROR_OTHER and then hung outright. Nothing in user space
 * recovered it; it needed a physical replug. That is a far worse outcome than the
 * wedged command channel it was meant to fix, and it also costs the warm state.
 *
 * So the device reset is last and opt-in. Steps 1-3 are safe, and step 3 alone is
 * very likely enough, because ResetArm is OUT-only and reboots the firmware.
 *
 * Build:
 *   clang -O2 -o gl310recover gl310recover.c -I/opt/homebrew/include \
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

static libusb_device_handle *dev;

static int try_read_reg(unsigned int reg, unsigned int *out, int timeout) {
    unsigned char c[8] = {0}, r[4] = {0};
    int n = 0;
    c[0] = 0x01; c[1] = 0x00; c[2] = 1; c[3] = 0;
    c[4] = reg & 0xff; c[5] = (reg >> 8) & 0xff;
    if (libusb_bulk_transfer(dev, EP_CMD_WR, c, 8, &n, timeout) || n != 8) return -1;
    if (libusb_bulk_transfer(dev, EP_CMD_RD, r, 4, &n, timeout) || n != 4) return -1;
    *out = r[0] | (r[1] << 8) | (r[2] << 16) | ((unsigned)r[3] << 24);
    return 0;
}

static int alive(void) {
    unsigned int v = 0;
    /* register 0x00 is a plain control register and always answers when healthy */
    if (try_read_reg(0x00, &v, 500)) return 0;
    printf("      register 0x00 = 0x%08x\n", v);
    return 1;
}

static void drain(unsigned char ep) {
    unsigned char buf[65536];
    int n = 0, rounds = 0;
    while (rounds++ < 64) {
        if (libusb_bulk_transfer(dev, ep, buf, sizeof buf, &n, 100) || n == 0) break;
        printf("      drained %d stale byte(s) from EP 0x%02x\n", n, ep);
    }
}

static int reset_arm(int run) {
    unsigned char c[8] = {0}; int n = 0;
    c[0] = 0x07; c[1] = run ? 1 : 0;
    return libusb_bulk_transfer(dev, EP_CMD_WR, c, 8, &n, 1000) || n != 8 ? -1 : 0;
}

static int open_claim(void) {
    dev = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!dev) { fprintf(stderr, "cannot open %04x:%04x\n", VID, PID); return -1; }
    if (libusb_claim_interface(dev, 0)) { fprintf(stderr, "claim failed\n"); return -1; }
    return 0;
}

int main(int argc, char **argv) {
    int hard = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--hard")) hard = 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (libusb_init(NULL) < 0) return 1;
    libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);
    if (open_claim()) return 1;

    printf("0. is the command channel answering?\n");
    if (alive()) { printf("   yes - nothing to recover.\n"); goto done; }
    printf("   no.\n");

    printf("1. draining stale bytes from the IN pipes\n");
    drain(EP_CMD_RD);
    drain(EP_DMA_RD);
    if (alive()) { printf("   recovered by draining.\n"); goto done; }

    printf("2. clear_halt on all four endpoints\n");
    libusb_clear_halt(dev, EP_CMD_WR); libusb_clear_halt(dev, EP_CMD_RD);
    libusb_clear_halt(dev, EP_DMA_WR); libusb_clear_halt(dev, EP_DMA_RD);
    if (alive()) { printf("   recovered by clear_halt.\n"); goto done; }

    printf("3. ResetArm toggle (reboots QPSOS from the resident image)\n");
    if (reset_arm(0)) printf("   warning: ResetArm(0) did not go out\n");
    usleep(100000);
    if (reset_arm(1)) printf("   warning: ResetArm(1) did not go out\n");
    usleep(400000);
    if (alive()) {
        printf("   recovered by rebooting the firmware.\n"
               "   check it with:  ./gl310log\n");
        goto done;
    }

    if (!hard) {
        printf("\nStill stuck. The only step left here is a USB device reset, which on\n"
               "macOS has wedged this card at the IOKit level before - enumerating fine\n"
               "but impossible to open, needing a physical replug. Not doing that unless\n"
               "you ask: re-run with --hard, or just replug the card.\n");
        goto done;
    }

    printf("4. USB device reset (--hard; this is the risky one)\n");
    libusb_release_interface(dev, 0);
    if (libusb_reset_device(dev) == LIBUSB_ERROR_NOT_FOUND) {
        printf("   device re-enumerated, reopening\n");
        libusb_close(dev);
        usleep(1500000);
        if (open_claim()) goto fail;
    } else {
        usleep(500000);
        if (libusb_claim_interface(dev, 0)) { printf("   re-claim failed\n"); goto fail; }
    }
    if (alive()) { printf("   recovered by device reset.\n"); goto done; }

fail:
    printf("\nStill unresponsive. The remaining step is a physical replug of the\n"
           "card, or power-cycling the dock.\n");
    if (dev) { libusb_release_interface(dev, 0); libusb_close(dev); }
    libusb_exit(NULL);
    return 1;
done:
    libusb_release_interface(dev, 0); libusb_close(dev); libusb_exit(NULL);
    return 0;
}
