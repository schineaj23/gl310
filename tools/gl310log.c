/*
 * gl310log.c - read QPSOS's own boot/diagnostic log out of the card's DRAM.
 *
 * The ARM firmware keeps a log of tagged records in its .bss, just above the
 * loaded image (qpvidfwusb.bin is 0x6edb0 bytes, the log starts at 0x06edc0).
 * Each record is a 16-byte header followed by NUL-terminated text:
 *
 *      +0x00  u32   (link/length, not needed here)
 *      +0x04  u32   0
 *      +0x08  u32   timestamp, in ticks of the same ~193 MHz counter the
 *                   firmware's "Update Tick Thread" maintains at 0x064dc4
 *      +0x0c  u32   source line number
 *      +0x10  char  text, tagged "(T)" trace, "(E)" error, "(W)" warn, "(I)" info
 *
 * This is the firmware talking, so it beats every inference we can make from
 * the outside: it says in its own words how far the boot got and what failed.
 * Reading it is a DMA read and nothing else - no DRAM write, no register write.
 *
 * Build:
 *   clang -O2 -o gl310log gl310log.c -I/opt/homebrew/include \
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
#define CHUNK 32768

/* Defaults cover the primary in-order buffer with room to grow. */
#define LOG_START 0x06e000
#define LOG_LEN   0x00c000

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

static int is_tag(const unsigned char *p) {
    return p[0] == '(' && p[2] == ')' &&
           (p[1] == 'T' || p[1] == 'E' || p[1] == 'W' || p[1] == 'I');
}

/* Print every record in the buffer; with `since` set, only newer timestamps.
   Returns the highest timestamp seen. */
static unsigned int dump(const unsigned char *buf, unsigned int start,
                         unsigned int len, unsigned int since, int show_addr) {
    unsigned int newest = since;
    for (unsigned int i = 0; i + 4 < len; i++) {
        if (!is_tag(buf + i)) continue;
        unsigned int j = i;
        while (j < len && buf[j] >= 0x20 && buf[j] <= 0x7e) j++;
        if (j - i < 4) { continue; }
        unsigned int ts = (i >= 8) ? get32(buf + i - 8) : 0;
        unsigned int ln = (i >= 4) ? get32(buf + i - 4) : 0;
        if (ts > newest) newest = ts;
        if (ts > since || since == 0) {
            if (show_addr) printf("0x%06x  ", start + i);
            printf("[%10u:%-5u] %.*s\n", ts, ln, (int)(j - i), (const char *)buf + i);
        }
        i = j;
    }
    return newest;
}

int main(int argc, char **argv) {
    unsigned int start = LOG_START, len = LOG_LEN;
    int follow = 0, show_addr = 0, interval_ms = 500;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--follow")) follow = 1;
        else if (!strcmp(argv[i], "--addr")) show_addr = 1;
        else if (!strcmp(argv[i], "--start") && i + 1 < argc) start = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--len") && i + 1 < argc) len = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval_ms = atoi(argv[++i]);
        else {
            printf("usage: gl310log [--follow] [--addr] [--start A] [--len N] [--interval MS]\n"
                   "  Reads the ARM firmware's log out of DRAM. Read-only.\n");
            return 0;
        }
    }
    len = (len + CHUNK - 1) & ~(unsigned)(CHUNK - 1);

    unsigned char *buf = malloc(len);
    if (!buf) { fprintf(stderr, "out of memory\n"); return 1; }

    if (libusb_init(NULL) < 0) return 1;
    libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);
    dev = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!dev) { fprintf(stderr, "cannot open %04x:%04x\n", VID, PID); return 1; }
    if (libusb_claim_interface(dev, 0)) { fprintf(stderr, "claim failed\n"); return 1; }

    if (snapshot(start, len, buf)) goto out;
    unsigned int newest = dump(buf, start, len, 0, show_addr);

    while (follow) {
        usleep((unsigned)interval_ms * 1000);
        if (snapshot(start, len, buf)) break;
        unsigned int n = dump(buf, start, len, newest, show_addr);
        if (n > newest) { newest = n; fflush(stdout); }
    }
out:
    libusb_release_interface(dev, 0); libusb_close(dev); libusb_exit(NULL);
    free(buf);
    return 0;
}
