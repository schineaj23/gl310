/*
 * gl310start.c - start the hardware H.264 encoder and capture what it produces.
 *
 * Every value here comes from captures/gl310-bringup-debugview.log, i.e. from a real
 * working 1920x1080 session on this exact card, so nothing is guessed. The log gives
 * the ordered sequence; the driver disassembly gives the encodings.
 *
 *   QPFWCODECAPI_SystemOpen   cmd 0xf1  p1 = 0x80000011
 *   QPFWCODECAPI_SystemLink   cmd 0xf2  p1 = 4 bits per field, see below
 *   ten property messages     cmd 0x10  p1 = selector, p2.. = values
 *   eleven direct register writes, 0x6d0..0x6f8, the encoder config block
 *   QPFWENCAPI_StartEncoder   cmd 0x01  no params
 *
 * Note the config block occupies the *same* registers as the mailbox parameters: they
 * are one shared scratch window. That is why order matters - each property message is
 * consumed by the ARM before the next write lands.
 *
 * Note also there is no UpdateConfig (cmd 0x06) in the log between the config writes
 * and StartEncoder, so the ARM reads those registers when it processes StartEncoder.
 *
 * Then the ARM posts frames: cmd 0x40 on 0x6b0 with p1 = stream type (0x83),
 * p2 = buffer address in WORDS, p4 = length in WORDS. The host DMA-reads that and
 * acks with message code 0x30. The driver reads with swap(1), so the bitstream may
 * need 32-bit byte swapping - we detect that by looking for H.264 start codes.
 *
 * HAZARD, hit twice. The command channel has no framing and cannot resynchronise, so
 * one short or abandoned bulk transfer stalls the firmware's whole USB gadget: every
 * later transfer times out, on the OUT pipe as well, and neither draining, clear_halt,
 * SET_INTERFACE nor a ResetArm toggle brings it back. Only a physical replug does.
 * Both times it happened at shutdown, with StopEncoder landing while the ARM still had
 * frames in flight and a payload left queued on EP 0x81.
 *
 * So this tool now drains EP 0x81 before starting, after any short read, and - most
 * importantly - quiesces before stopping: keep acking and draining until the ARM stops
 * posting, and only then send StopEncoder. That is a hypothesis about the cause, not
 * yet a proven fix.
 *
 * Build:
 *   clang -O2 -o gl310start gl310start.c -I/opt/homebrew/include \
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

#define R_TO_ARM_STATUS 0x6cc
#define R_TO_ARM_MSG    0x6fc
#define R_FROM_ARM_MSG  0x6b0
#define R_FROM_ARM_STAT 0x6c8

static const unsigned int PARAM_REG[9] =
    { 0x6f8, 0x6f4, 0x6f0, 0x6ec, 0x6e8, 0x6e4, 0x6e0, 0x6dc, 0x6d8 };
static const unsigned int FROM_ARM_PARAM[5] = { 0x6b4, 0x6b8, 0x6bc, 0x6c0, 0x6c4 };

/* The encoder config block, verbatim from the working session. */
struct cfg { unsigned int reg, val; const char *what; };
static const struct cfg CONFIG[] = {
    { 0x6f8, 0x2101c219, "SystemControl"      },
    { 0x6f4, 0x04380780, "PictureResolution 1920x1080" },
    { 0x6f0, 0x0f5e0608, "InputControl"       },
    { 0x6ec, 0x0078ea60, "RateControl"        },
    { 0x6e8, 0x1f4007d0, "VBRBitRate"         },
    { 0x6e4, 0x80002000, "FilterControl"      },
    { 0x6e0, 0xf199001e, "GOPLoopFilter"      },
    { 0x6d8, 0x00000010, "BlockSize"          },
    { 0x6dc, 0x04380780, "OutPicResolution 1920x1080" },
    { 0x6d4, 0x21161100, "AudioControl"       },
    { 0x6d0, 0x520840f4, "AudioControlEx"     },
};
#define NCONFIG (int)(sizeof CONFIG / sizeof CONFIG[0])

static libusb_device_handle *dev;
static int verbose = 0;

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
/* Pull and discard anything left sitting in the DMA-in pipe. The command channel
   has no framing, so one short or abandoned bulk transfer desynchronises every
   later transfer on the device - which is how this tool wedged the card twice. */
static int drain_dma(void) {
    static unsigned char junk[131072];
    int n = 0, total = 0, rounds = 0;
    while (rounds++ < 32) {
        if (libusb_bulk_transfer(dev, EP_DMA_RD, junk, sizeof junk, &n, 50) || n == 0) break;
        total += n;
    }
    if (total) printf("    drained %d stale byte(s) from the DMA-in pipe\n", total);
    return total;
}

static int dma_read(unsigned int byteaddr, unsigned char *buf, int nbytes) {
    unsigned char c[16], st[1]; int n = 0, r;
    hdr(c, 0x09, 0x00, 8, 0);
    put32(c + 8, byteaddr >> 2);
    put32(c + 12, (unsigned)nbytes / 4);
    if (cmd(c, 16, st, 1) != 1) return -1;
    r = libusb_bulk_transfer(dev, EP_DMA_RD, buf, nbytes, &n, BULK_TIMEOUT);
    if (r || n != nbytes) {
        fprintf(stderr, "    dma IN @0x%x: %s (%d/%d)\n",
                byteaddr, libusb_error_name(r), n, nbytes);
        /* a short read leaves the rest of the payload queued - take it out */
        drain_dma();
        return -1;
    }
    return 0;
}

/* Poll bit 0 of 0x6cc until the ARM clears it. */
static int wait_free(int timeout_ms) {
    for (int ms = 0; ms <= timeout_ms; ms += 2) {
        unsigned int v;
        if (reg_read(R_TO_ARM_STATUS, &v)) return -1;
        if (!(v & 1)) return ms;
        usleep(2000);
    }
    return -1;
}

static int send_msg(const char *label, unsigned int code, int task,
                    const unsigned int *params, int nparam) {
    if (wait_free(500) < 0) {
        fprintf(stderr, "  %-28s mailbox busy, aborting\n", label);
        return -1;
    }
    for (int i = 0; i < nparam; i++)
        if (reg_write(PARAM_REG[i], params[i])) return -1;
    if (reg_write(R_TO_ARM_STATUS, ((unsigned)task << 16) | 1u)) return -1;
    if (reg_write(R_TO_ARM_MSG, ((unsigned)task << 16) | (code & 0xffff))) return -1;
    int w = wait_free(500);
    printf("  %-28s cmd 0x%02x", label, code);
    if (verbose) for (int i = 0; i < nparam; i++) printf(" p%d=0x%x", i + 1, params[i]);
    printf("  -> %s\n", w >= 0 ? "taken" : "NOT TAKEN");
    return w >= 0 ? 0 : -1;
}

static int prop(const char *label, unsigned int sel,
                unsigned int a, unsigned int b, unsigned int c, int n) {
    unsigned int p[4] = { sel, a, b, c };
    return send_msg(label, 0x10, 0, p, n + 1);
}

/* How many 4-byte-swapped vs straight H.264 start codes does this buffer hold? */
static void scan_h264(const unsigned char *buf, int len, int *plain, int *swapped) {
    *plain = *swapped = 0;
    for (int i = 0; i + 4 <= len; i++) {
        if (buf[i]==0 && buf[i+1]==0 && buf[i+2]==0 && buf[i+3]==1) (*plain)++;
        if (buf[i]==1 && buf[i+1]==0 && buf[i+2]==0 && buf[i+3]==0) (*swapped)++;
    }
}

int main(int argc, char **argv) {
    int go = 0, stop = 0, watch_s = 5;
    const char *outpath = "gl310-capture.bin";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--go")) go = 1;
        else if (!strcmp(argv[i], "--stop")) stop = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) watch_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outpath = argv[++i];
        else { printf("usage: gl310start [--go] [--stop] [--watch SECS] [--out FILE] [-v]\n"); return 0; }
    }
    if (!go && !stop) {
        printf("Starts the hardware encoder with the exact configuration from the\n"
               "working Windows session (1920x1080 H.264), then watches for frames.\n\n"
               "Recovery: --stop sends StopEncoder; gl310init --go reboots QPSOS.\n\n"
               "Re-run with --go to proceed.\n");
        return 0;
    }

    if (libusb_init(NULL) < 0) return 1;
    libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);
    dev = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!dev) { fprintf(stderr, "cannot open %04x:%04x\n", VID, PID); return 1; }
    if (libusb_claim_interface(dev, 0)) { fprintf(stderr, "claim failed\n"); return 1; }

    if (stop) {
        unsigned int p[2] = { 0, 0 };
        send_msg("StopEncoder", 0x02, 0, p, 2);
        goto out;
    }

    printf("Bringing up the encoder.\n\n");
    drain_dma();

    unsigned int one[1];
    one[0] = 0x80000011;
    if (send_msg("SystemOpen", 0xf1, 0, one, 1)) goto out;

    /* 4 bits per field: vi=0 vic=0 vo=1 voc=0 ai=0 aic=0 ao=1 aoc=0 */
    one[0] = (0u) | (0u<<4) | (1u<<8) | (0u<<12) | (0u<<16) | (0u<<20) | (1u<<24) | (0u<<28);
    if (send_msg("SystemLink", 0xf2, 0, one, 1)) goto out;

    printf("\n  properties:\n");
    prop("ExternalTriggerToSync", 0x0f, 0, 0, 0, 2);
    prop("PTSResetByTrigger",     0x10, 0, 0, 0, 3);
    prop("DeinterlaceMode",       0x12, 1, 0, 0, 1);
    prop("RateControlEx",         0x13, 120, 0, 8, 3);
    prop("LargeCompressBuffer",   0x14, 0x80004a38, 0, 0, 1);
    prop("AVDiscardControl",      0x16, 2, 0, 0, 1);
    prop("UseSWPTS",              0x17, 1, 0, 0, 1);
    prop("ViuSyncCode",           0x02, 0xf1f1f1da, 0xb6f1f1b6, 0, 2);

    printf("\n  encoder config block:\n");
    for (int i = 0; i < NCONFIG; i++) {
        if (reg_write(CONFIG[i].reg, CONFIG[i].val)) goto out;
        printf("    0x%03x = 0x%08x   %s\n", CONFIG[i].reg, CONFIG[i].val, CONFIG[i].what);
    }

    printf("\n");
    if (send_msg("StartEncoder", 0x01, 0, NULL, 0)) goto out;

    printf("\nWatching for frames for %d s (ARM posts cmd 0x40 on 0x6b0) ...\n", watch_s);
    FILE *f = NULL, *fidx = NULL;
    long total = 0;
    int frames = 0;
    int max_frames = 90;
    for (int ms = 0; ms < watch_s * 1000; ms += 10) {
        unsigned int st = 0, msg = 0;
        if (reg_read(R_FROM_ARM_STAT, &st)) break;
        if (!(st & 1)) { usleep(10000); continue; }
        if (reg_read(R_FROM_ARM_MSG, &msg)) break;
        unsigned int p[5] = {0};
        for (int i = 0; i < 5; i++) reg_read(FROM_ARM_PARAM[i], &p[i]);
        printf("  [%5d ms] status=0x%08x msg=0x%08x cmd=0x%02x  "
               "p1=0x%x p2=0x%x p3=0x%x p4=0x%x p5=0x%x\n",
               ms, st, msg, msg & 0xff, p[0], p[1], p[2], p[3], p[4]);

        if ((msg & 0xff) == 0x40 && frames < max_frames) {
            unsigned int addr = p[1] << 2;          /* p2 is a word address  */
            unsigned int nbytes = p[3] * 4;         /* p4 is a word count    */
            if (nbytes && nbytes < (32u << 20)) {
                unsigned char *buf = malloc(nbytes);
                if (buf) {
                    unsigned int done = 0; int ok = 1;
                    while (done < nbytes) {
                        unsigned int n = nbytes - done;
                        if (n > 131072) n = 131072;
                        n &= ~3u; if (!n) break;
                        if (dma_read(addr + done, buf + done, (int)n)) { ok = 0; break; }
                        done += n;
                    }
                    if (ok && done) {
                        int pl, sw; scan_h264(buf, (int)done, &pl, &sw);
                        printf("           read %u bytes from 0x%06x   "
                               "H.264 start codes: %d plain, %d byte-swapped\n",
                               done, addr, pl, sw);
                        if (!f) f = fopen(outpath, "wb");
                        if (f) {
                            /* Record what each byte range in the capture came from, so
                               reassembly can be worked out offline instead of guessed
                               at while holding the card open. */
                            if (!fidx) {
                                char ip[1024];
                                snprintf(ip, sizeof ip, "%s.idx", outpath);
                                fidx = fopen(ip, "w");
                                if (fidx) fprintf(fidx, "# ms offset len p1 p2 p3 p4 p5\n");
                            }
                            if (fidx)
                                fprintf(fidx, "%d %ld %u 0x%x 0x%x 0x%x 0x%x 0x%x\n",
                                        ms, total, done, p[0], p[1], p[2], p[3], p[4]);
                            fwrite(buf, 1, done, f); total += done; frames++;
                        }
                    }
                    free(buf);
                }
            }
            /* Return the buffer: CTask_CompleteArm sends message code 0x30
               echoing the incoming parameters. */
            unsigned int ap[6] = { p[0], p[1], p[2], p[3], p[4], 1 };
            send_msg("  complete", 0x30, 0, ap, 6);
        }
        /* QPFWAPI_AckARMMessage (0x5afca) clears bit 0 of the inbound status word
           and writes it back to 0x6c8. Bit 0 is the inbound busy flag, exactly
           mirroring bit 0 of 0x6cc in the other direction. Writing the word back
           unmodified does nothing, which is why the ARM re-posted one descriptor
           forever. */
        reg_write(R_FROM_ARM_STAT, st & ~1u);
        usleep(8000);   /* gentler than the driver needs, but far gentler than before */
    }
    if (f) fclose(f);
    if (fidx) fclose(fidx);

    printf("\n");
    if (frames)
        printf("*** captured %d buffer(s), %ld bytes -> %s\n", frames, total, outpath);
    else
        printf("No frame messages. The encoder did not post data - most likely there is\n"
               "no valid signal on the HDMI input, since the ADV7441 receiver is\n"
               "configured by the host over I2C and we have not done that yet.\n");

    /* Shutting down is where this wedged the card twice: StopEncoder lands while
       the ARM still has frames in flight, so a payload stays queued on EP 0x81 and
       the whole gadget stalls. Quiesce first - keep acking and draining until the
       ARM stops posting - and only then stop the encoder. */
    printf("\nQuiescing: draining in-flight frames before StopEncoder.\n");
    for (int i = 0; i < 100; i++) {
        unsigned int st = 0;
        if (reg_read(R_FROM_ARM_STAT, &st)) break;
        drain_dma();
        if (!(st & 1)) { usleep(20000); continue; }
        reg_write(R_FROM_ARM_STAT, st & ~1u);
        usleep(10000);
    }
    drain_dma();

    printf("Sending StopEncoder.\n");
    unsigned int sp[2] = { 0, 0 };
    send_msg("StopEncoder", 0x02, 0, sp, 2);
    for (int i = 0; i < 40; i++) {
        unsigned int st = 0;
        if (reg_read(R_FROM_ARM_STAT, &st)) break;
        if (st & 1) { drain_dma(); reg_write(R_FROM_ARM_STAT, st & ~1u); }
        usleep(25000);
    }
    drain_dma();
    printf("Card left idle.\n");
out:
    libusb_release_interface(dev, 0); libusb_close(dev); libusb_exit(NULL);
    return 0;
}
