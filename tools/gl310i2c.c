/*
 * gl310i2c.c - I2C access, and the ADV7441 HDMI receiver init.
 *
 * Ordering matters, and it is not obvious: in the captured Windows session the
 * receiver is configured BEFORE the ARM firmware is downloaded.
 *
 *      4.934  CADI7441_InitDevice()
 *      5.937  CADI7441_SelectVideoSource source(20)
 *      5.981  CQLCodec_FWDownloadAll()
 *      6.348  CQLCodecLib_InitDevice() QPSOS2
 *
 * So host-side I2C runs against the boot loader with the ARM still in reset, not
 * against QPSOS. Probing it with the firmware up - which is what I did first - gets
 * nothing, because QPSOS owns the bus by then (it has its own i2c.c, and its SW-I2C
 * path is not even implemented: "Software (GPIO) I2C not supported").
 *
 * Reply format, from CUsbCntl_I2CWriteThenRead (0x84f50): the reply is rlen+1 bytes
 * and the LAST byte is a status, where **0x08 means success**. Anything else is a
 * failure, so a NAK and a zero data byte are distinguishable - which is what makes a
 * bus scan possible at all.
 *
 * Commands used (see RE.md):
 *      op 0x08  I2CRead           08 00 rlen:u16 slave:u32                -> rlen+1
 *      op 0x08  I2CWriteThenRead  08 wlen:u8 rlen:u16 slave:u32 wdata[]   -> rlen+1
 *      op 0x05  I2CWrite          05 01 len:u16 slave:u32 data[len]       -> 1 byte
 *
 * The ADV7441 init sequence, extracted from CADI7441_InitDevice (0x9ba10) - a 1 s
 * delay, three writes to one address then five to another:
 *
 *      slave A: f0 10    f1 0f    f4 20
 *      slave B: 14 1f    15 ec    1c 49    1d 04    5a 01
 *
 * The driver passes those addresses as 0x31 and 0x35. Both are odd, so they cannot be
 * 8-bit write addresses; they are presumably 7-bit, making the 8-bit forms 0x62 and
 * 0x6a. --shift picks which interpretation to send, because the chain from
 * CADI7441_InitDevice through the CI2C wrapper to the wire has not been traced.
 *
 * Build:
 *   clang -O2 -o gl310i2c gl310i2c.c -I/opt/homebrew/include \
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

#define I2C_OK 0x08

static libusb_device_handle *dev;

static int cmd(const unsigned char *c, int len, unsigned char *rep, int rlen) {
    int n = 0, r = libusb_bulk_transfer(dev, EP_CMD_WR, (unsigned char *)c, len, &n, TIMEOUT);
    if (r || n != len) { fprintf(stderr, "cmd OUT: %s\n", libusb_error_name(r)); return -1; }
    if (rlen <= 0) return 0;
    r = libusb_bulk_transfer(dev, EP_CMD_RD, rep, rlen, &n, TIMEOUT);
    if (r) return -1;
    return n;
}
static int sw = 0;   /* --sw selects the op 0x0b/0x0c SW-I2C family */

static int reset_arm(int run) {
    unsigned char c[8] = {0}; c[0] = 0x07; c[1] = run ? 1 : 0;
    return cmd(c, 8, NULL, 0);
}

/* op 0x08 with wlen=0: plain read of rlen bytes. Returns status, or -1. */
static int i2c_read(unsigned int slave, unsigned char *out, int rlen) {
    unsigned char c[8], r[64];
    if (rlen < 1 || rlen > 32) return -1;
    c[0] = sw ? 0x0c : 0x08; c[1] = 0x00; c[2] = rlen & 0xff; c[3] = (rlen >> 8) & 0xff;
    c[4] = slave & 0xff; c[5] = 0; c[6] = 0; c[7] = 0;
    if (cmd(c, 8, r, rlen + 1) != rlen + 1) return -1;
    if (out) memcpy(out, r, rlen);
    return r[rlen];
}
/* op 0x08 with wlen>0: write sub-address(es), then read. Returns status. */
static int i2c_wr_rd(unsigned int slave, const unsigned char *w, int wlen,
                     unsigned char *out, int rlen) {
    unsigned char c[8 + 32], r[64];
    if (wlen < 1 || wlen > 32 || rlen < 1 || rlen > 32) return -1;
    c[0] = sw ? 0x0c : 0x08; c[1] = (unsigned char)wlen;
    c[2] = rlen & 0xff; c[3] = (rlen >> 8) & 0xff;
    c[4] = slave & 0xff; c[5] = 0; c[6] = 0; c[7] = 0;
    memcpy(c + 8, w, wlen);
    if (cmd(c, 8 + wlen, r, rlen + 1) != rlen + 1) return -1;
    if (out) memcpy(out, r, rlen);
    return r[rlen];
}
/* op 0x05: write len bytes. Reply is a single status byte. */
static int i2c_write(unsigned int slave, const unsigned char *d, int len) {
    unsigned char c[8 + 32], r[1];
    if (len < 1 || len > 32) return -1;
    c[0] = sw ? 0x0b : 0x05; c[1] = 0x01; c[2] = len & 0xff; c[3] = (len >> 8) & 0xff;
    c[4] = slave & 0xff; c[5] = 0; c[6] = 0; c[7] = 0;
    memcpy(c + 8, d, len);
    if (cmd(c, 8 + len, r, 1) != 1) return -1;
    return r[0];
}

/* MemoryWrite, op 0x02 sub 1: byte address, shifted >>2 internally. */
static int mem_write(unsigned int ba, unsigned int val) {
    unsigned char c[16]; unsigned int w = ba >> 2;
    c[0]=0x02; c[1]=0x01; c[2]=4; c[3]=0;
    c[4]=w&0xff; c[5]=(w>>8)&0xff; c[6]=(w>>16)&0xff; c[7]=(w>>24)&0xff;
    c[8]=w&0xff; c[9]=(w>>8)&0xff; c[10]=(w>>16)&0xff; c[11]=(w>>24)&0xff;
    c[12]=val&0xff; c[13]=(val>>8)&0xff; c[14]=(val>>16)&0xff; c[15]=(val>>24)&0xff;
    return cmd(c, 16, NULL, 0);
}
static int reg_write(unsigned int reg, unsigned int val) {
    unsigned char c[12];
    c[0]=0x01; c[1]=0x01; c[2]=1; c[3]=0;
    c[4]=reg&0xff; c[5]=(reg>>8)&0xff; c[6]=0; c[7]=0;
    c[8]=val&0xff; c[9]=(val>>8)&0xff; c[10]=(val>>16)&0xff; c[11]=(val>>24)&0xff;
    return cmd(c, 12, NULL, 0);
}

/* CQLCodec_FWSwitchMode (0x58eb0), QPSOS2 branch. The mode flags live at DRAM
   0x2f1090 and 0x2f2004 - note gl310init does NOT overwrite those, it only writes
   0x0 and 0x100000, so they survive a firmware reload and must be cleared
   explicitly. That is what --unswitch is for.

   DO NOT RUN THIS unless you can physically replug the card. Tried once: the device
   vanished from USB completely - not a stalled gadget, not enumerated at all, gone
   from ioreg - and only a replug brought it back. Switching mode evidently tears down
   the USB gadget, and in the mode it lands in nothing re-advertises it. A replug does
   clear DRAM, so the flags do not persist and the card comes back healthy; but
   --unswitch can only help if the device is still reachable, which after --switch it
   is not. Kept for the record and because the decoded sequence is correct. */
static void switch_mode(unsigned int flag) {
    printf("  DRAM 0x2f1090 <- %u\n", flag);
    mem_write(0x2f1090, flag);
    printf("  DRAM 0x2f2004 <- %u\n", flag);
    mem_write(0x2f2004, flag);
    reset_arm(0);
    usleep(1000);
    reg_write(0x6cc, 0);
    reset_arm(1);
    usleep(400000);
}

/* The two register blocks from CADI7441_InitDevice, in order. */
static const unsigned char BLOCK_A[][2] = { {0xf0,0x10}, {0xf1,0x0f}, {0xf4,0x20} };
static const unsigned char BLOCK_B[][2] = { {0x14,0x1f}, {0x15,0xec}, {0x1c,0x49},
                                            {0x1d,0x04}, {0x5a,0x01} };

int main(int argc, char **argv) {
    int scan = 0, go = 0, init = 0, hold = 1, shift = 1, swmode = 0;
    unsigned int rd_slave = 0; int rd_sub = -1, rd_n = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scan")) scan = 1;
        else if (!strcmp(argv[i], "--sw")) sw = 1;
        else if (!strcmp(argv[i], "--switch")) swmode = 1;
        else if (!strcmp(argv[i], "--unswitch")) swmode = 2;
        else if (!strcmp(argv[i], "--go")) go = 1;
        else if (!strcmp(argv[i], "--init")) init = 1;
        else if (!strcmp(argv[i], "--no-hold")) hold = 0;
        else if (!strcmp(argv[i], "--shift") && i + 1 < argc) shift = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--read") && i + 2 < argc) {
            rd_slave = strtoul(argv[++i], 0, 0);
            rd_sub   = (int)strtoul(argv[++i], 0, 0);
            if (i + 1 < argc && argv[i+1][0] != '-') rd_n = atoi(argv[++i]);
        } else {
            printf("usage:\n"
                   "  gl310i2c --scan [--no-hold]            scan the bus for responders\n"
                   "  gl310i2c --read SLAVE SUB [N]          write-then-read\n"
                   "  gl310i2c --init [--shift 0|1] --go     replay the ADV7441 init\n"
                   "By default the ARM is held in reset first, because that is the state\n"
                   "the vendor driver does its I2C in. Re-run gl310init --go afterwards.\n");
            return 0;
        }
    }

    if (libusb_init(NULL) < 0) return 1;
    libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);
    dev = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!dev) { fprintf(stderr, "cannot open %04x:%04x\n", VID, PID); return 1; }
    if (libusb_claim_interface(dev, 0)) { fprintf(stderr, "claim failed\n"); return 1; }

    if (swmode) {
        if (!go) { printf("Would %s firmware mode (FWSwitchMode flags at 0x2f1090 / 0x2f2004).\n"
                          "Re-run with --go.\n", swmode==1?"set":"clear"); goto done; }
        printf("%s the FWSwitchMode flags and restarting the ARM.\n",
               swmode==1?"Setting":"Clearing");
        switch_mode(swmode==1 ? 1u : 0u);
        hold = 0;
    }

    if (hold) {
        printf("Holding the ARM in reset (the state the driver does I2C in).\n");
        reset_arm(0);
        usleep(50000);
    }

    if (scan) {
        printf("Scanning: op 0x%02x, 1-byte read, reporting status != 0 (0x08 = success)\n", sw ? 0x0c : 0x08);
        int found = 0;
        for (unsigned int a = 0; a < 0x100; a++) {
            unsigned char b = 0;
            int st = i2c_read(a, &b, 1);
            if (st < 0) { printf("  0x%02x: transport error\n", a); break; }
            if (st != 0) {
                printf("  slave 0x%02x -> status 0x%02x, data 0x%02x%s\n",
                       a, st, b, st == I2C_OK ? "   *** ACK" : "");
                found++;
            }
        }
        if (!found) printf("  nothing answered on any of 256 addresses.\n");
    }

    if (rd_sub >= 0) {
        unsigned char w = (unsigned char)rd_sub, out[32] = {0};
        int st = i2c_wr_rd(rd_slave, &w, 1, out, rd_n);
        printf("read slave 0x%02x sub 0x%02x x%d -> status 0x%02x%s\n  ",
               rd_slave, rd_sub, rd_n, st, st == I2C_OK ? " (success)" : " (FAILED)");
        for (int i = 0; i < rd_n; i++) printf("%02x ", out[i]);
        printf("\n");
    }

    if (init) {
        unsigned int a = shift ? (0x31u << 1) : 0x31u;
        unsigned int b = shift ? (0x35u << 1) : 0x35u;
        if (!go) {
            printf("Would replay CADI7441_InitDevice with slave A=0x%02x B=0x%02x\n"
                   "  (driver values 0x31/0x35, %s)\n"
                   "Re-run with --go.\n", a, b,
                   shift ? "shifted left 1 as 7-bit addresses" : "used as-is");
        } else {
            printf("Replaying CADI7441_InitDevice (A=0x%02x B=0x%02x)\n", a, b);
            printf("  1 s delay, as the driver does\n");
            usleep(1000000);
            int fails = 0;
            for (int i = 0; i < 3; i++) {
                int st = i2c_write(a, BLOCK_A[i], 2);
                printf("    0x%02x <- %02x %02x   status 0x%02x%s\n", a,
                       BLOCK_A[i][0], BLOCK_A[i][1], st, st == I2C_OK ? "" : "  FAILED");
                if (st != I2C_OK) fails++;
            }
            for (int i = 0; i < 5; i++) {
                int st = i2c_write(b, BLOCK_B[i], 2);
                printf("    0x%02x <- %02x %02x   status 0x%02x%s\n", b,
                       BLOCK_B[i][0], BLOCK_B[i][1], st, st == I2C_OK ? "" : "  FAILED");
                if (st != I2C_OK) fails++;
            }
            printf("  %d of 8 writes failed\n", fails);
        }
    }

done:
    if (hold) printf("\nARM still held in reset - run gl310init --go to boot it.\n");
    libusb_release_interface(dev, 0); libusb_close(dev); libusb_exit(NULL);
    return 0;
}
