/* Video output, composite (CVBS) flavour:
 *
 * Using PIO[1], output the Mac 1BPP framebuffer as an interlaced, monochrome
 * PAL (625i/50) or NTSC (525i/60) composite signal, on two GPIOs + resistors.
 * This is done directly from the Mac framebuffer (no intermediate buffer).
 *
 * Even framebuffer lines are sent in the top field, odd lines in the bottom
 * field, so the full 342 (or 480) lines are visible with true interlace.
 *
 * Time is divided into "segments", each one a pair of DMA transfers:
 *   - a visible/blank video line: [4 config words] + [pixel words]
 *   - a vertical-sync start stream (tail half-line + equalising/broad/
 *     equalising pulses + optional filler half-line), sent as one blob.
 * The DMA IRQ fires once per segment to point DMA at the next one.
 *
 * [1]: see pio_video.pio
 *
 * Copyright 2024 Matt Evans
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/structs/padsbank0.h"
#include "pio_video.pio.h"

#include "hw.h"

////////////////////////////////////////////////////////////////////////////////
// Timing.  All horizontal figures are in pixels of VIDEO_PCLK_HZ.

#define VIDEO_PCLK_HZ           15000000        /* square-ish pixels on Mac */
#define VIDEO_PIO_HZ            (VIDEO_PCLK_HZ * 2)     /* 2 PIO cycles/pixel */

#if ((CVBS_SYSCLK_KHZ * 1000) % VIDEO_PIO_HZ) != 0
#error "CVBS_SYSCLK_KHZ must be an integer multiple of 2*pixel clock (no jitter)"
#endif

#ifdef CVBS_NTSC
/* NTSC-M, 525 lines, 59.94Hz fields (here ~59.96).  Line = 63.53us. */
#define VIDEO_LINE_PX           953
#define VIDEO_SLOT_PX           476     /* half line */
#define VIDEO_HSYNC_PX          71      /* 4.7us */
#define VIDEO_EQ_PX             35      /* 2.3us */
#define VIDEO_BROAD_PX          407     /* 27.1us */
#define VIDEO_NPULSE            6       /* 6 eq, 6 broad, 6 eq */
#define VIDEO_LINES_A           253     /* normal lines in top field */
#define VIDEO_LINES_B           253
#define VIDEO_TAIL_BEFORE_A     1       /* half-line slot ahead of vsync block */
#define VIDEO_TAIL_BEFORE_B     0
#define VIDEO_FILLER_A          0       /* half-line slot after vsync block */
#define VIDEO_FILLER_B          1
#define VIDEO_FIELD_HALFLINES   525
#define VIDEO_ACTIVE_START      12      /* first visible normal line */
#define VIDEO_ACTIVE_LINES      240     /* visible lines per field */
#define VIDEO_H_CENTRE_PX       536     /* centre of picture, from sync fall */
#else
/* PAL-B/G/I, 625 lines, 50Hz fields.  Line = 64.00us exactly. */
#define VIDEO_LINE_PX           960
#define VIDEO_SLOT_PX           480
#define VIDEO_HSYNC_PX          71      /* 4.7us */
#define VIDEO_EQ_PX             35      /* 2.35us */
#define VIDEO_BROAD_PX          410     /* 27.3us */
#define VIDEO_NPULSE            5       /* 5 eq, 5 broad, 5 eq */
#define VIDEO_LINES_A           305
#define VIDEO_LINES_B           304
#define VIDEO_TAIL_BEFORE_A     0
#define VIDEO_TAIL_BEFORE_B     1
#define VIDEO_FILLER_A          0
#define VIDEO_FILLER_B          1
#define VIDEO_FIELD_HALFLINES   625
#define VIDEO_ACTIVE_START      17
#define VIDEO_ACTIVE_LINES      288
#define VIDEO_H_CENTRE_PX       547
#endif

#if (2 * VIDEO_LINES_A + VIDEO_TAIL_BEFORE_A + 3 * VIDEO_NPULSE + VIDEO_FILLER_A) != VIDEO_FIELD_HALFLINES
#error "Field A must span exactly half a video frame"
#endif
#if (2 * VIDEO_LINES_B + VIDEO_TAIL_BEFORE_B + 3 * VIDEO_NPULSE + VIDEO_FILLER_B) != VIDEO_FIELD_HALFLINES
#error "Field B must span exactly half a video frame"
#endif

#define VIDEO_FB_HRES           DISP_WIDTH
#define VIDEO_FB_VRES           DISP_HEIGHT     /* whole frame; per field: /2 */
#define VIDEO_FB_FIELD_LINES    (VIDEO_FB_VRES / 2)

#if (VIDEO_FB_HRES & 31)
#error "DISP_WIDTH must be a multiple of 32 pixels"
#endif
#if (VIDEO_FB_VRES & 1)
#error "DISP_HEIGHT must be even (interlace)"
#endif
#if (VIDEO_FB_FIELD_LINES > VIDEO_ACTIVE_LINES)
#error "Framebuffer too tall for this TV standard"
#endif

/* First normal line (within the field) that carries framebuffer data: */
#define VIDEO_FB_V_START        (VIDEO_ACTIVE_START + \
                                 (VIDEO_ACTIVE_LINES - VIDEO_FB_FIELD_LINES)/2)

/* Words of 1BPP pixel data per line: */
#define VIDEO_VISIBLE_WPL       (VIDEO_FB_HRES / 32)

/* Pixel offset of first framebuffer pixel, from the falling edge of sync: */
#define VIDEO_PIX_START_PX      (VIDEO_H_CENTRE_PX - VIDEO_FB_HRES/2)

////////////////////////////////////////////////////////////////////////////////
// Line/slot descriptions -> PIO config words

enum { SLOT_EQ, SLOT_BROAD, SLOT_HALF, LINE_VIDEO, NUM_LINE_TYPES };

#define CFG_WORDS               4       /* words of config per line */
#define SLOT_WORDS              (CFG_WORDS + 1) /* config + 1 word of blank */
#define STREAM_TAIL_SLOTS       2       /* slots sent in the 2nd DMA of a stream */
#define STREAM_MAX_SLOTS        (1 + 3*VIDEO_NPULSE + 1)

static uint32_t video_cfg[NUM_LINE_TYPES][CFG_WORDS];
static uint32_t video_null[VIDEO_VISIBLE_WPL];  /* all 1s: black/blank */

/* Cycle accounting is described in pio_video.pio:
 * line_px = S+B+P+F + 7 ; first pixel at sync_px + B + 2 from sync fall.
 */
static void video_make_cfg(uint32_t *c, int line_px, int sync_px,
                           int pix_start_px, int pix_px)
{
        int s = sync_px - 1;
        int b = pix_start_px - sync_px - 2;
        int p = pix_px - 1;
        int f = line_px - sync_px - pix_px - b - 5;
        c[0] = s;
        c[1] = b;
        c[2] = p;
        c[3] = f;
        if (b < 0 || f < 0)
                printf("Video: bad timing (type %p b=%d f=%d)!\n", c, b, f);
}

/* A vertical-sync "start stream": the sequence of half-line slots that runs
 * between the last visible line of one field and the first normal line of the
 * next.  Built out of 5-word slots (4 config + 1 word of blank data).  The
 * stream is sent as two DMA transfers; the second is the last few slots and is
 * stored pre-byte-swapped because that DMA channel byte-swaps.
 */
typedef struct {
        uint32_t        words[STREAM_MAX_SLOTS * SLOT_WORDS];
        uint32_t        tail_swapped[STREAM_TAIL_SLOTS * SLOT_WORDS];
        unsigned int    head_words;
} vstream_t;

static vstream_t video_stream[2];       /* [0] = top field (A), [1] = B */

static unsigned int video_stream_add(vstream_t *s, unsigned int n, int type)
{
        memcpy(&s->words[n * SLOT_WORDS], video_cfg[type], CFG_WORDS * 4);
        s->words[n * SLOT_WORDS + CFG_WORDS] = 0xffffffff;
        return n + 1;
}

static void video_build_stream(vstream_t *s, int tail, int filler)
{
        unsigned int n = 0;
        if (tail)
                n = video_stream_add(s, n, SLOT_HALF);
        for (int i = 0; i < VIDEO_NPULSE; i++)
                n = video_stream_add(s, n, SLOT_EQ);
        for (int i = 0; i < VIDEO_NPULSE; i++)
                n = video_stream_add(s, n, SLOT_BROAD);
        for (int i = 0; i < VIDEO_NPULSE; i++)
                n = video_stream_add(s, n, SLOT_EQ);
        if (filler)
                n = video_stream_add(s, n, SLOT_HALF);

        s->head_words = (n - STREAM_TAIL_SLOTS) * SLOT_WORDS;
        for (int i = 0; i < STREAM_TAIL_SLOTS * SLOT_WORDS; i++)
                s->tail_swapped[i] = __builtin_bswap32(s->words[s->head_words + i]);
}

static void video_prep_buffer()
{
        memset(video_null, 0xff, sizeof(video_null));

        /* Half-line slots, 32 blank pixels (1 word) after the sync pulse: */
        video_make_cfg(video_cfg[SLOT_EQ], VIDEO_SLOT_PX, VIDEO_EQ_PX,
                       VIDEO_EQ_PX + 2, 32);
        video_make_cfg(video_cfg[SLOT_BROAD], VIDEO_SLOT_PX, VIDEO_BROAD_PX,
                       VIDEO_BROAD_PX + 2, 32);
        video_make_cfg(video_cfg[SLOT_HALF], VIDEO_SLOT_PX, VIDEO_HSYNC_PX,
                       VIDEO_HSYNC_PX + 2, 32);
        /* Normal line (framebuffer or blank), picture centred in the line: */
        video_make_cfg(video_cfg[LINE_VIDEO], VIDEO_LINE_PX, VIDEO_HSYNC_PX,
                       VIDEO_PIX_START_PX, VIDEO_FB_HRES);

        video_build_stream(&video_stream[0], VIDEO_TAIL_BEFORE_A, VIDEO_FILLER_A);
        video_build_stream(&video_stream[1], VIDEO_TAIL_BEFORE_B, VIDEO_FILLER_B);
}

////////////////////////////////////////////////////////////////////////////////
// Video DMA, framebuffer pointers

static uint32_t *video_framebuffer;

/* 3 DMA channels are used.  The first to transfer data to PIO, and
 * the other two to transfer descriptors to the first channel.
 */
static uint8_t video_dmach_tx;
static uint8_t video_dmach_descr_cfg;
static uint8_t video_dmach_descr_data;

typedef struct {
        const void *raddr;
        void *waddr;
        uint32_t count;
        uint32_t ctrl;
} dma_descr_t;

static dma_descr_t video_dmadescr_cfg;
static dma_descr_t video_dmadescr_data;

#define VIDEO_SEG_START         0xffffffffu

static volatile unsigned int video_field = 0;           /* 0 = top (A), 1 = B */
static volatile unsigned int video_line = VIDEO_SEG_START;

static const uint32_t   *__not_in_flash_func(video_line_addr)(unsigned int k, unsigned int field)
{
        if ((k >= VIDEO_FB_V_START) && (k < VIDEO_FB_V_START + VIDEO_FB_FIELD_LINES)) {
                /* Interlace: top field = even FB lines, bottom = odd */
                unsigned int y = ((k - VIDEO_FB_V_START) << 1) | field;
                return (const uint32_t *)&video_framebuffer[y * VIDEO_VISIBLE_WPL];
        }
        return (const uint32_t *)video_null;
}

static void    __not_in_flash_func(video_dma_prep_new)()
{
        /* The descriptor DMA read pointers have moved on; reset them.
         * The write pointers wrap so should be pointing to the
         * correct DMA regs.
         */
        dma_hw->ch[video_dmach_descr_cfg].read_addr = (uintptr_t)&video_dmadescr_cfg;
        dma_hw->ch[video_dmach_descr_cfg].transfer_count = 4;
        dma_hw->ch[video_dmach_descr_data].read_addr = (uintptr_t)&video_dmadescr_data;
        dma_hw->ch[video_dmach_descr_data].transfer_count = 4;

        /* Set up the next segment's two transfers.  This has until the 2nd
         * transfer of the *current* segment completes (i.e. until its data has
         * fully entered the PIO FIFO); that's at least ~2us even for the vsync
         * streams, and most of a line otherwise.
         */
        unsigned int field = video_field;
        unsigned int k = video_line;

        if (k == VIDEO_SEG_START) {
                vstream_t *s = &video_stream[field];
                video_dmadescr_cfg.raddr = s->words;
                video_dmadescr_cfg.count = s->head_words;
                video_dmadescr_data.raddr = s->tail_swapped;
                video_dmadescr_data.count = STREAM_TAIL_SLOTS * SLOT_WORDS;
                video_line = 0;
        } else {
                video_dmadescr_cfg.raddr = video_cfg[LINE_VIDEO];
                video_dmadescr_cfg.count = CFG_WORDS;
                video_dmadescr_data.raddr = video_line_addr(k, field);
                video_dmadescr_data.count = VIDEO_VISIBLE_WPL;
                if (++k >= (field ? VIDEO_LINES_B : VIDEO_LINES_A)) {
                        video_field = field ^ 1;
                        video_line = VIDEO_SEG_START;
                } else {
                        video_line = k;
                }
        }
}

static void     __not_in_flash_func(video_dma_irq)()
{
        /* The DMA IRQ occurs once the 2nd transfer of a segment has been
         * triggered (not when the transfer completes, but when the
         * descriptor transfer (that leads to the data transfer!) completes.
         * All we need to do is reconfigure the descriptors; the video DMA will
         * re-trigger the descriptors later.
         */
        if (dma_channel_get_irq0_status(video_dmach_descr_data)) {
                dma_channel_acknowledge_irq0(video_dmach_descr_data);
                video_dma_prep_new();
        }
}

static void     video_init_dma()
{
        /* pio_video expects each line to be composed of 4 words of config
         * followed by pixel data.  To avoid having to embed config metadata in
         * the display framebuffer, two DMA transfers to PIO are used for each
         * segment: the first sends config (or most of a vsync stream), then
         * triggers the second which sends the framebuffer line (or the last
         * slots of a vsync stream).  This lets us use a flat, regular FB.
         *
         * The PIO side emits 1BPP MSB-first.  The second DMA also byteswaps
         * to match the Mac framebuffer layout.
         *
         *  "Another caveat is that multiple channels should not be connected
         *   to the same DREQ.":
         * Only one DMA channel can do the transfers to PIO, because of how the
         * credit-based flow control works.  So, _only_ channel 0 transfers
         * from $SOME_BUFFER into the PIO FIFO, and channel 1+2 are used to
         * reprogram/trigger channel 0 from a DMA descriptor list.
         *
         * ch1 programs ch0 with a descriptor that causes ch0 to transfer
         * config, then trigger ch2 when complete.  ch2 then programs ch0 with
         * a descriptor to transfer data, then trigger ch1 when ch0 completes;
         * when ch2 finishes doing that, it produces an IRQ.
         *
         * The IRQ handler sets up descriptors for the next segment; the
         * deadline is by the end of ch0's data transfer.
         */
        video_dmach_tx = dma_claim_unused_channel(true);
        video_dmach_descr_cfg = dma_claim_unused_channel(true);
        video_dmach_descr_data = dma_claim_unused_channel(true);

        /* Transmit DMA: config+video data */
        dma_channel_config dc_tx_c = dma_channel_get_default_config(video_dmach_tx);
        channel_config_set_dreq(&dc_tx_c, DREQ_PIO0_TX0);
        channel_config_set_transfer_data_size(&dc_tx_c, DMA_SIZE_32);
        channel_config_set_read_increment(&dc_tx_c, true);
        channel_config_set_write_increment(&dc_tx_c, false);
        channel_config_set_bswap(&dc_tx_c, false);
        /* Completion of the config TX triggers the video_dmach_descr_data channel */
        channel_config_set_chain_to(&dc_tx_c, video_dmach_descr_data);
        video_dmadescr_cfg.raddr = NULL;                /* Reprogrammed each segment */
        video_dmadescr_cfg.waddr = (void *)&pio0_hw->txf[0];
        video_dmadescr_cfg.count = CFG_WORDS;
        video_dmadescr_cfg.ctrl = dc_tx_c.ctrl;

        dma_channel_config dc_tx_d = dma_channel_get_default_config(video_dmach_tx);
        channel_config_set_dreq(&dc_tx_d, DREQ_PIO0_TX0);
        channel_config_set_transfer_data_size(&dc_tx_d, DMA_SIZE_32);
        channel_config_set_read_increment(&dc_tx_d, true);
        channel_config_set_write_increment(&dc_tx_d, false);
        channel_config_set_bswap(&dc_tx_d, true);      /* This channel bswaps */
        /* Completion of the data TX triggers the video_dmach_descr_cfg channel */
        channel_config_set_chain_to(&dc_tx_d, video_dmach_descr_cfg);
        video_dmadescr_data.raddr = NULL;               /* Reprogrammed each segment */
        video_dmadescr_data.waddr = (void *)&pio0_hw->txf[0];
        video_dmadescr_data.count = VIDEO_VISIBLE_WPL;
        video_dmadescr_data.ctrl = dc_tx_d.ctrl;

        /* Now, the descr_cfg and descr_data channels transfer _those_
         * descriptors to program the video_dmach_tx channel:
         */
        dma_channel_config dcfg = dma_channel_get_default_config(video_dmach_descr_cfg);
        channel_config_set_transfer_data_size(&dcfg, DMA_SIZE_32);
        channel_config_set_read_increment(&dcfg, true);
        channel_config_set_write_increment(&dcfg, true);
        /* This channel loops on 16-byte/4-word boundary (i.e. writes all config): */
        channel_config_set_ring(&dcfg, true, 4);
        dma_channel_configure(video_dmach_descr_cfg, &dcfg,
                              &dma_hw->ch[video_dmach_tx].read_addr,
                              &video_dmadescr_cfg,
                              4 /* 4 words of config */,
                              false /* Not yet */);

        dma_channel_config ddata = dma_channel_get_default_config(video_dmach_descr_data);
        channel_config_set_transfer_data_size(&ddata, DMA_SIZE_32);
        channel_config_set_read_increment(&ddata, true);
        channel_config_set_write_increment(&ddata, true);
        channel_config_set_ring(&ddata, true, 4);
        /* This transfer has a completion IRQ.  Receipt of that means that both
         * descriptors have been transferred, and should be reprogrammed for the
         * next segment.
         */
        dma_channel_set_irq0_enabled(video_dmach_descr_data, true);
        dma_channel_configure(video_dmach_descr_data, &ddata,
                              &dma_hw->ch[video_dmach_tx].read_addr,
                              &video_dmadescr_data,
                              4 /* 4 words of config */,
                              false /* Not yet */);
}

////////////////////////////////////////////////////////////////////////////////

/* Initialise PIO, DMA, start sending pixels.  Passed a pointer to a
 * DISP_WIDTH x DISP_HEIGHT x 1 Mac-order framebuffer.
 *
 * Pins: GPIO_VID_DATA (video) and GPIO_VID_SYNC (= video + 1).
 */
void    video_init(uint32_t *framebuffer)
{
        printf("Video init (CVBS %s, %dx%d)\n",
#ifdef CVBS_NTSC
               "NTSC",
#else
               "PAL",
#endif
               VIDEO_FB_HRES, VIDEO_FB_VRES);

        float clkdiv = (float)clock_get_hz(clk_sys) / (float)VIDEO_PIO_HZ;

        pio_video_program_init(pio0, 0,
                               pio_add_program(pio0, &pio_video_program),
                               GPIO_VID_DATA, /* Followed by SYNC */
                               clkdiv);

        /* Framebuffer bit 1 = black: invert the video pin so that 1 = low
         * (blanking level), 0 = high (white).  SYNC isn't inverted.
         */
        gpio_set_outover(GPIO_VID_DATA, GPIO_OVERRIDE_INVERT);
        /* Highest drive strength, keeps the resistor-DAC levels stiff: */
        hw_write_masked(&padsbank0_hw->io[GPIO_VID_DATA],
                        PADS_BANK0_GPIO0_DRIVE_VALUE_12MA << PADS_BANK0_GPIO0_DRIVE_LSB,
                        PADS_BANK0_GPIO0_DRIVE_BITS);
        hw_write_masked(&padsbank0_hw->io[GPIO_VID_SYNC],
                        PADS_BANK0_GPIO0_DRIVE_VALUE_12MA << PADS_BANK0_GPIO0_DRIVE_LSB,
                        PADS_BANK0_GPIO0_DRIVE_BITS);

        /* IRQ handlers for DMA_IRQ_0: */
        irq_set_exclusive_handler(DMA_IRQ_0, video_dma_irq);
        irq_set_enabled(DMA_IRQ_0, true);

        video_init_dma();

        /* Init config word buffers */
        video_field = 0;
        video_line = VIDEO_SEG_START;
        video_framebuffer = framebuffer;
        video_prep_buffer();

        /* Set up pointers to the first segment, and start DMA */
        video_dma_prep_new();
        dma_channel_start(video_dmach_descr_cfg);
}
