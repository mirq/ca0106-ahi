/*
 * catone - play a sine tone on the CA0106 front output.
 *
 * catone [FREQ=1000] [RATE=48000] [SECS=3] [VOL=50] [LEFT] [RIGHT] [S32]
 *        [VERIFY] [QUIET]
 *
 * VERIFY records the chip's own I2S mixer output ("what you hear") while
 * the tone plays and checks frequency, channel separation and continuity,
 * so the data path can be validated without listening.
 *
 * GPL-2.0-or-later.
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <math.h>

#include <libraries/openpci.h>
#include <inline/openpci.h>

#include "ca0106_regs.h"
#include "ca0106_hw.h"

static const char vers[] = "$VER: catone 0.2 (18.11.2026)";

static struct CAHw hw;

#define PLAY_OFF    0
#define CAP_OFF     65536UL
#define CAP_BYTES   16384UL

static UBYTE rate_code(ULONG rate)
{
    switch (rate)
    {
        case 44100:  return CA_RATE_44100;
        case 96000:  return CA_RATE_96000;
        case 192000: return CA_RATE_192000;
        default:     return CA_RATE_48000;
    }
}

static void analyse(ULONG rate, ULONG freq, BOOL left, BOOL right, LONG amp, ULONG capptr)
{
    volatile ULONG *c = (volatile ULONG *)(hw.dma_cpu + CAP_OFF);
    ULONG frames = CAP_BYTES / 4, i, ch, start = (capptr / 4) % frames;
    LONG prev[2] = { 0, 0 }, maxabs[2] = { 0, 0 }, maxstep[2] = { 0, 0 };
    ULONG zc[2] = { 0, 0 }, glitch[2] = { 0, 0 };
    LONG expstep = (LONG)(2.0 * 3.14159265 * freq / rate * amp * 1.3) + 64;

    CacheClearE((APTR)c, CAP_BYTES, CACRF_ClearD);
    /* oldest data first; skip a few frames on both ends of the seam */
    for (i = 8; i < frames - 8; i++)
    {
        ULONG v = c[(start + i) % frames];
        /* LE frame in memory: bytes L.lo L.hi R.lo R.hi */
        WORD s[2];
        s[0] = (WORD)(((v >> 8) & 0xff00) | ((v >> 24) & 0xff));
        s[1] = (WORD)(((v << 8) & 0xff00) | ((v >> 8) & 0xff));
        for (ch = 0; ch < 2; ch++)
        {
            LONG a = s[ch] < 0 ? -s[ch] : s[ch];
            if (a > maxabs[ch]) maxabs[ch] = a;
            if (i > 8)
            {
                LONG d = s[ch] - prev[ch];
                if (d < 0) d = -d;
                if (d > maxstep[ch]) maxstep[ch] = d;
                if (d > expstep) glitch[ch]++;
                if ((prev[ch] < 0 && s[ch] >= 0))
                    zc[ch]++;
            }
            prev[ch] = s[ch];
        }
    }
    for (ch = 0; ch < 2; ch++)
    {
        BOOL want = ch == 0 ? left : right;
        ULONG estf = zc[ch] * rate / (frames - 16);
        printf("  %s: peak %5ld  est. freq %5lu Hz  max step %5ld (limit %ld) glitches %lu  %s\n",
               ch ? "right" : "left ", maxabs[ch], estf, maxstep[ch], expstep, glitch[ch],
               want ? ((maxabs[ch] > amp / 4 && glitch[ch] == 0 &&
                        estf > freq * 9 / 10 && estf < freq * 11 / 10) ? "OK" : "BAD")
                    : (maxabs[ch] < 256 ? "OK (silent)" : "BAD (should be silent)"));
    }
}

int main(void)
{
    LONG *args[9] = { 0 };
    struct RDArgs *rda;
    const char *err;
    ULONG freq = 1000, rate = 48000, secs = 3, vol = 50;
    BOOL left, right, s32, verify, quiet;
    ULONG frames, bytes, cycles, i, fsize;
    LONG amp;

    rda = ReadArgs("FREQ/N,RATE/N,SECS/N,VOL/N,LEFT/S,RIGHT/S,S32/S,VERIFY/S,QUIET/S",
                   (LONG *)args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "catone");
        return RETURN_FAIL;
    }
    if (args[0]) freq = *args[0];
    if (args[1]) rate = *args[1];
    if (args[2]) secs = *args[2];
    if (args[3]) vol = *args[3];
    left = args[4] != 0;
    right = args[5] != 0;
    if (!left && !right)
        left = right = TRUE;
    s32 = args[6] != 0;
    verify = args[7] != 0;
    quiet = args[8] != 0;
    if (vol > 100) vol = 100;
    if (rate != 44100 && rate != 48000 && rate != 96000 && rate != 192000)
        rate = 48000;

    fsize = s32 ? 8 : 4;
    frames = rate / 10;                     /* 100 ms of audio */
    while (frames * fsize > 65532)
        frames /= 2;
    bytes = frames * fsize;
    cycles = (freq * frames + rate / 2) / rate;
    if (cycles < 1) cycles = 1;
    freq = cycles * rate / frames;
    amp = (LONG)(32767 * vol / 100);

    err = ca_open(&hw, FALSE);
    if (!err)
        err = ca_dma_alloc(&hw, CAP_OFF + CAP_BYTES, 0);
    if (!err)
        err = ca_chip_init(&hw);
    if (err)
    {
        printf("catone: %s\n", err);
        if (hw.owned)
            ca_chip_stop_dma(&hw);
        ca_close(&hw);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    for (i = 0; i < frames; i++)
    {
        double ph = 2.0 * 3.14159265358979 * (double)cycles * (double)i / (double)frames;
        LONG s = (LONG)(sin(ph) * amp);
        LONG l = left ? s : 0, r = right ? s : 0;
        if (s32)
        {
            volatile ULONG *p = (volatile ULONG *)(hw.dma_cpu + PLAY_OFF + i * 8);
            p[0] = ca_swap32((ULONG)(l << 16));
            p[1] = ca_swap32((ULONG)(r << 16));
        }
        else
        {
            volatile ULONG *p = (volatile ULONG *)(hw.dma_cpu + PLAY_OFF + i * 4);
            *p = ((ULONG)ca_swap16((UWORD)l) << 16) | ca_swap16((UWORD)r);
        }
    }
    CacheClearE(hw.dma_cpu + PLAY_OFF, bytes, CACRF_ClearD);

    if (!quiet)
        printf("catone: %lu Hz, %lu Hz %s, %s%s, vol %lu%%, %lu s (buffer %lu frames)\n",
               freq, rate, s32 ? "S32" : "S16",
               left && right ? "both" : left ? "left" : "right",
               "", vol, secs, frames);

    ca_play_setup(&hw, PLAY_OFF, bytes, rate_code(rate), s32);
    ca_play_start(&hw);

    if (verify)
    {
        Delay(10);
        /* record the I2S mixer output (source 1) */
        ca_ptr_write(&hw, CAPTURE_SOURCE, 0, 0x111100e4UL);
        for (i = 0; i < CAP_BYTES; i += 4)
            *(volatile ULONG *)(hw.dma_cpu + CAP_OFF + i) = 0;
        /* capture cannot run at 44.1 kHz; record the mixer at 48 kHz then */
        ca_cap_setup(&hw, CAP_OFF, CAP_BYTES, rate_code(rate == 44100 ? 48000 : rate));
        ca_cap_start(&hw);
        Delay(12);                              /* > 85 ms at 48 kHz */
        ca_cap_stop(&hw);
        i = ca_cap_pointer(&hw);
        ca_ptr_write(&hw, CAPTURE_SOURCE, 0, 0x333300e4UL);
        printf("verify (I2S mixer loopback, playback pointer $%04lx):\n",
               ca_play_pointer(&hw));
        analyse(rate == 44100 ? 48000 : rate, freq, left, right, amp, i);
    }

    for (i = 0; i < secs * 10; i++)
    {
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
            break;
        Delay(5);
    }
    ca_play_stop(&hw);
    ca_chip_stop_dma(&hw);
    ca_close(&hw);
    FreeArgs(rda);
    (void)vers;
    return RETURN_OK;
}
