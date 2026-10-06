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
 * HAZARD, hit three times: the firmware's USB gadget can be driven into a stall from
 * which nothing in user space recovers - every transfer times out, OUT pipe included,
 * and draining, clear_halt, SET_INTERFACE and a ResetArm toggle all fail. Only a
 * physical replug fixes it.
 *
 * The firmware says what is happening, in its own log:
 *
 *      (E)Drop
 *      (W)can't get sysmsg in dwc_otg_pcd_handle_in_ep_intr
 *
 * QPSOS enqueues 16 request buffers on ep4out (our command pipe) at boot and drains
 * them from its HCI thread. Flooding it with commands exhausts that pool, and once
 * exhausted the gadget stalls. The old loop spent ~30 round trips per fragment at
 * ~100 fragments/s - about 3000 commands/s against a 16-deep queue.
 *
 * So part of the fix is round-trip count:
 *   - one RegisterReadEx for the whole inbound block instead of eight reads
 *   - no per-fragment printf, one pre-allocated buffer
 *   - twelve transfers per fragment instead of about thirty
 *
 * The ack params stay as six single writes: op 0x03 RegisterWriteEx misplaces values
 * on this device (see RE.md), so only the read side is batched.
 *
 * But round trips are not the whole story. Polling flat out with no idle sleep made
 * the card produce NOTHING - 5381 polls/s and zero fragments - because the HCI thread
 * never got scheduled. --idle-us (default 1000) exists for that.
 *
 * And the captured volume scales with poll rate, which means cmd 0x40 reports ring
 * STATE rather than a queue of new data: at 366 polls/s the full-ring descriptor
 * (p2=0x102f00, p4=0x5d00) repeated 115 times in 3 s and the implied rate was 72 Mbps
 * against a configured 8 Mbps. Reassembly is therefore NOT solved; the host must tell
 * the ARM how much it consumed, which is what CTask_CompleteArm's parameters carry
 * from driver-side request bookkeeping (status, reqid), not a blind echo of p1..p5.
 *
 * The vendor driver throttles harder still: CTask_ProcessDataStreaming declines to
 * issue a DMA whenever rd_ready or wr_ready is clear, which in the captured session
 * was 1203 of 3653 calls - a third of the time it does nothing at all.
 *
 * Two operational rules fall out, both now enforced below:
 *   - quiesce before StopEncoder: keep acking and draining until the ARM stops posting
 *   - reboot the firmware when done, because a capture run leaves the gadget degraded
 *     and the next heavy operation wedges it. The third wedge was exactly that:
 *     gl310init's firmware download against an already poisoned gadget.
 *
 * Build:
 *   clang -O2 -o gl310start gl310start.c -I/opt/homebrew/include \
 *         -L/opt/homebrew/lib -lusb-1.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
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
/* RegisterReadEx: one command, n consecutive registers, 4*n bytes back. The whole
   inbound mailbox - message, p1..p5, status and the outbound doorbell - is eight
   consecutive registers from 0x6b0, so one transfer replaces eight. */
static int reg_read_block(unsigned int start, int n, unsigned int *out) {
    unsigned char c[8], r[128];
    if (n < 1 || n > 32) return -1;
    hdr(c, 0x01, 0x00, (unsigned short)n, start);
    if (cmd(c, 8, r, 4 * n) != 4 * n) return -1;
    for (int i = 0; i < n; i++) out[i] = get32(r + 4 * i);
    return 0;
}
/* RegisterWriteEx (op 0x03): n consecutive registers in one command, ascending. */
static int reg_write_block(unsigned int start, int n, const unsigned int *vals) {
    unsigned char c[8 + 32 * 4];
    if (n < 1 || n > 32) return -1;
    hdr(c, 0x03, 0x01, (unsigned short)n, start);
    for (int i = 0; i < n; i++) put32(c + 8 + 4 * i, vals[i]);
    return cmd(c, 8 + 4 * n, NULL, 0);
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

static int reset_arm(int run) {
    unsigned char c[8]; hdr(c, 0x07, run ? 1 : 0, 0, 0); return cmd(c, 8, NULL, 0);
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


int main(int argc, char **argv) {
    int go = 0, stop = 0, watch_s = 5, keep = 0, idle_us = 1000;
    int stream_type = -1;   /* bits 0-2 of SystemControl, 0x6f8 */
    int vout = -1;          /* SystemLink video_output nibble, bits 8-11 */
    int capmode = -1;       /* QPFWENCAPI_SetEncMode, cmd 0x11 */
    int rawfmt = -1;        /* SetRawVideoDecimation output_format, sel 0x11 */
    const char *outpath = "gl310-capture.bin";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--go")) go = 1;
        else if (!strcmp(argv[i], "--stop")) stop = 1;
        else if (!strcmp(argv[i], "--keep")) keep = 1;
        else if (!strcmp(argv[i], "--stream-type") && i + 1 < argc)
            stream_type = (int)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--video-output") && i + 1 < argc)
            vout = (int)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--cap-mode") && i + 1 < argc)
            capmode = (int)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--raw-format") && i + 1 < argc)
            rawfmt = (int)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--idle-us") && i + 1 < argc) idle_us = atoi(argv[++i]);
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
    {
        unsigned int vo = (vout >= 0) ? (unsigned)vout & 0xf : 1u;
        one[0] = (0u) | (0u<<4) | (vo<<8) | (0u<<12) | (0u<<16) | (0u<<20) | (1u<<24) | (0u<<28);
        if (vout >= 0) printf("  SystemLink video_output -> %u (0x%08x)\n", vo, one[0]);
    }
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
    if (rawfmt >= 0) {
        /* QPFWENCAPI_SetRawVideoDecimation(input_format, output_format,
           scale_factor) = property selector 0x11. */
        prop("RawVideoDecimation", 0x11, 0, (unsigned)rawfmt, 0, 3);
    }
    if (capmode >= 0) {
        /* QPFWENCAPI_SetEncMode(taskId, capMode, trigMode, gpio_pin):
           p1 = capMode (0x6f8), p2 = trigMode (0x6f4), p3 = gpio_pin (0x6f0). */
        unsigned int ep[3] = { (unsigned)capmode, 0, 0 };
        send_msg("SetEncMode", 0x11, 0, ep, 3);
    }

    printf("\n  encoder config block:\n");
    for (int i = 0; i < NCONFIG; i++) {
        unsigned int v = CONFIG[i].val;
        /* SystemControl packs: bits 0-2 stream type, 3-7 stream data, 8-11 profile,
           12-15 level. 0x2101c219 = type 1, data 3, profile 2, level 12, matching the
           log's "stream type(1) stream data(3) profile(2) level(12)". */
        if (CONFIG[i].reg == 0x6f8 && stream_type >= 0) {
            v = (v & ~7u) | ((unsigned)stream_type & 7u);
            printf("    stream type -> %d  (0x%08x)\n", stream_type, v);
        }
        if (reg_write(CONFIG[i].reg, v)) goto out;
        printf("    0x%03x = 0x%08x   %s\n", CONFIG[i].reg, CONFIG[i].val, CONFIG[i].what);
    }

    printf("\n");
    if (send_msg("StartEncoder", 0x01, 0, NULL, 0)) goto out;

    printf("\nWatching for frames for %d s (ARM posts cmd 0x40 on 0x6b0) ...\n", watch_s);
    FILE *f = NULL, *fidx = NULL;
    long total = 0;
    int frames = 0;
    int max_frames = 4000;
    /* The loop below is written for round-trip count, because that is what the
       firmware's (E)Drop / "can't get sysmsg" warnings are really complaining about.
       The old version spent ~30 USB round trips per fragment: eight register reads to
       collect the notification, eight writes to ack it, a terminal printf, and an
       8 ms sleep. At ~100 fragments/s against an 8 Mbps stream that cannot keep up,
       so the firmware's gadget message pool empties and frames are dropped.

       Batched, it is nine transfers: one RegisterReadEx for the whole inbound block,
       the DMA command and its payload, one RegisterWriteEx for the six ack params,
       two doorbell writes and one status clear. No sleeps, no per-fragment printing,
       one pre-allocated buffer. */
    unsigned char *buf = malloc(1u << 20);
    if (!buf) { fprintf(stderr, "out of memory\n"); goto out; }

    struct timeval t0, now;
    gettimeofday(&t0, NULL);
    long polls = 0, short_reads = 0, idle = 0;

    for (;;) {
        gettimeofday(&now, NULL);
        long ms = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_usec - t0.tv_usec) / 1000;
        if (ms >= (long)watch_s * 1000 || frames >= max_frames) break;

        /* 0x6b0..0x6cc: msg, p1..p5, inbound status, outbound doorbell - one transfer */
        unsigned int blk[8];
        if (reg_read_block(R_FROM_ARM_MSG, 8, blk)) break;
        polls++;
        unsigned int msg = blk[0], st = blk[6], outdoor = blk[7];
        const unsigned int *p = &blk[1];
        if (!(st & 1)) { idle++; usleep(idle_us); continue; }

        /* blk[7] is 0x6cc: if our previous message is still pending, give the
           ARM a moment rather than stacking another one on top of it. */
        if (outdoor & 1) { usleep(idle_us); continue; }

        if ((msg & 0xff) == 0x40) {
            unsigned int addr = p[1] << 2;          /* p2 is a word address */
            unsigned int nbytes = p[3] * 4;         /* p4 is a word count   */
            if (nbytes && nbytes <= (1u << 20)) {
                unsigned int done = 0; int ok = 1;
                while (done < nbytes) {
                    unsigned int n = nbytes - done;
                    if (n > 131072) n = 131072;
                    n &= ~3u; if (!n) break;
                    if (dma_read(addr + done, buf + done, (int)n)) { ok = 0; break; }
                    done += n;
                }
                if (!ok) short_reads++;
                if (ok && done) {
                    if (!f) f = fopen(outpath, "wb");
                    if (f) {
                        if (!fidx) {
                            char ip[1024];
                            snprintf(ip, sizeof ip, "%s.idx", outpath);
                            fidx = fopen(ip, "w");
                            if (fidx) fprintf(fidx, "# ms offset len p1 p2 p3 p4 p5\n");
                        }
                        if (fidx)
                            fprintf(fidx, "%ld %ld %u 0x%x 0x%x 0x%x 0x%x 0x%x\n",
                                    ms, total, done, p[0], p[1], p[2], p[3], p[4]);
                        fwrite(buf, 1, done, f); total += done; frames++;
                    }
                    if (frames == 1)
                        printf("    first buffer: p1(stream type) = 0x%02x  %s\n", p[0],
                               p[0]==0x80 ? "<- ARM_BUF_YUV!" : (p[0]==0x83 ? "(compressed)" : ""));
                    if (verbose)
                        printf("  [%5ld ms] cmd 0x%02x  addr 0x%06x  %u B  last=%u\n",
                               ms, msg & 0xff, addr, done, p[2]);
                }
            }
            /* Return the buffer. CTask_CompleteArm (0x725d0) dispatches on the
               incoming command: 0x40 and 0x41 both reply with message code 0x30.

               Its parameters are NOT an echo of the incoming message. They come from
               the per-request array at task + reqid*0x48, and the captured session
               shows what they actually hold:

                 CTask_CompleteArm() type(0x83) addr(0x668f00) size(160740)
                     offset(160740) PTS(715827882) valid(0) last(0) frameFlags(0x0)

               for an incoming message with p1=0x83, p2=0x668f00, p4=0x9cf9 words
               (=160740 bytes), p5=0xaaaaaaaa. So the ack writes:

                 0x6f8 = type         = incoming p1
                 0x6f4 = offset >> 2  = how much we CONSUMED, in words
                 0x6f0 = PTS          = incoming p5 >> 2 (715827882 = 0xaaaaaaaa >> 2)
                 0x6ec = valid        = 0
                 0x6e8   not written at all on this path
                 0x6e4 = field 0x1e4  = 0

               offset always equals size in the log: the host reports it took the whole
               fragment. Putting the *address* in 0x6f4, as a blind echo does, tells the
               ARM nothing about progress - which is why its read pointer never advanced
               and it kept re-posting ring state until it gave up.

               Single writes, not RegisterWriteEx: op 0x03 misplaces values on this
               device (see RE.md), so only the read side is batched. */
            struct { unsigned int reg, val; } ack[] = {
                { 0x6f8, p[0]      },   /* type                      */
                { 0x6f4, p[3]      },   /* consumed length, in words */
                { 0x6f0, p[4] >> 2 },   /* PTS                       */
                { 0x6ec, 0         },   /* valid                     */
                { 0x6e4, 0         },   /* 0x1e4, unnamed            */
            };
            int bad = 0;
            for (int i = 0; i < (int)(sizeof ack / sizeof ack[0]) && !bad; i++)
                bad = reg_write(ack[i].reg, ack[i].val);
            if (bad) break;
            if (reg_write(R_TO_ARM_STATUS, 1u)) break;
            if (reg_write(R_TO_ARM_MSG, 0x30u)) break;
        }
        /* AckARMMessage (0x5afca) clears bit 0 of the inbound status and writes it
           back. Bit 0 is the inbound busy flag, mirroring bit 0 of 0x6cc. */
        if (reg_write(R_FROM_ARM_STAT, st & ~1u)) break;
    }
    free(buf);
    gettimeofday(&now, NULL);
    long elapsed = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_usec - t0.tv_usec) / 1000;
    if (!elapsed) elapsed = 1;
    printf("\n  %ld polls in %ld ms (%ld idle), %d fragments, %ld short reads\n",
           polls, elapsed, idle, frames, short_reads);
    printf("  %ld bytes in %ld ms = %.2f Mbps\n",
           total, elapsed, (double)total * 8.0 / (double)elapsed / 1000.0);
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

    /* Reboot the firmware before letting go of the card. A capture run leaves the
       gadget degraded - gl310log fills with (E)Drop and "can't get sysmsg" - and in
       that state the next heavy operation wedges it for good. That is exactly how the
       third wedge happened: gl310init's firmware download ran against an already
       poisoned gadget and hung. ResetArm is two OUT-only commands and gives QPSOS a
       clean gadget in about 8 ms, so there is no reason not to. */
    if (keep) {
        printf("--keep: leaving the firmware running so its log can be read.\n"
               "  Reboot it yourself before the next heavy operation.\n");
        goto out;
    }
    printf("Rebooting the firmware to leave the gadget clean.\n");
    reset_arm(0);
    usleep(100000);
    reset_arm(1);
    usleep(300000);
    unsigned int chk = 0;
    if (!reg_read(0x00, &chk)) printf("  reg 0x00 = 0x%08x - channel still answering\n", chk);
    printf("Card left idle. Re-download the firmware with gl310init --go before\n"
           "the next capture; check it with gl310log.\n");
out:
    libusb_release_interface(dev, 0); libusb_close(dev); libusb_exit(NULL);
    return 0;
}
