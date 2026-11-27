/*
 * cactl - control the CA0106 (Audigy SE) analog mixer from the shell.
 *
 * Typical use in S:User-Startup, to hear Paula through the card's Aux input
 * without any AHI program running:
 *
 *     cactl INPUT=AUX MONITOR=0
 *
 * cactl [INIT] [INPUT=LINE|MIC|AUX|CD] [GAIN=dB] [MONITOR=dB|OFF]
 *       [OUTPUT=dB] [MONROUTE=hex] [TESTAUX] [STATUS]
 *
 * dB values accept one decimal (e.g. -6, +3.5).  The card is released on
 * exit; the codecs keep running with the chosen settings.  When
 * ca0106.audio is in use, the card cannot be claimed and cactl fails.
 *
 * GPL-2.0-or-later.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/audio.h>
#include <hardware/custom.h>
#include <hardware/dmabits.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include <libraries/openpci.h>
#include <inline/openpci.h>

#include "ca0106_regs.h"
#include "ca0106_hw.h"

static const char vers[] __attribute__((used)) = "$VER: cactl 0.2 (18.11.2026)";

static struct CAHw hw;

static const char *input_names[CA_INPUTS] = { "LINE", "MIC", "AUX", "CD" };

/* "-6", "+3.5", "0" -> tenths of dB; returns FALSE on syntax error */
static BOOL parse_db(const char *s, LONG *tenths)
{
    char *end;
    LONG v = strtol(s, &end, 10) * 10;
    if (end == s && *s != '.' && *s != '-' && *s != '+')
        return FALSE;
    if (*end == '.')
    {
        LONG frac = end[1] >= '0' && end[1] <= '9' ? end[1] - '0' : 0;
        v += (*s == '-') ? -frac : frac;
    }
    *tenths = v;
    return TRUE;
}

static UBYTE db_to_att(LONG tenths)
{
    /* mixer attenuation code: 0x30 = 0 dB, 0.25 dB per step, 0 = +12 dB */
    LONG att = 0x30 - (tenths * 4) / 10;
    if (att < 0) att = 0;
    if (att > 0xfe) att = 0xfe;
    return (UBYTE)att;
}

static LONG att_to_tenths(UBYTE att)
{
    return ((LONG)0x30 - att) * 10 / 4;
}

static void print_db(const char *what, LONG tenths)
{
    LONG a = tenths < 0 ? -tenths : tenths;
    printf("  %-10s %s%ld.%ld dB\n", what, tenths < 0 ? "-" : "+", a / 10, a % 10);
}

/* ---------------------------------------------------------------------- */
/* TESTAUX: play a tone on Paula and look for it at the ADC and the mixer  */
/* ---------------------------------------------------------------------- */

#define CAP_OFF   0
#define CAP_BYTES 16384UL

static void measure(const char *label, ULONG source_sel, ULONG tone)
{
    volatile ULONG *c = (volatile ULONG *)(hw.dma_cpu + CAP_OFF);
    ULONG frames = CAP_BYTES / 4, i, start, zc[2] = { 0, 0 };
    LONG peak[2] = { 0, 0 }, prev[2] = { 0, 0 };
    LONG sum[2] = { 0, 0 };

    for (i = 0; i < frames; i++)
        c[i] = 0;
    CacheClearE((APTR)c, CAP_BYTES, CACRF_ClearD);
    ca_ptr_write(&hw, CAPTURE_SOURCE, 0, source_sel);
    ca_cap_setup(&hw, CAP_OFF, CAP_BYTES, CA_RATE_48000);
    ca_cap_start(&hw);
    Delay(12);
    ca_cap_stop(&hw);
    start = (ca_cap_pointer(&hw) / 4) % frames;
    CacheClearE((APTR)c, CAP_BYTES, CACRF_ClearD);

    /* first pass: DC offset */
    for (i = 0; i < frames; i++)
    {
        ULONG v = c[i];
        sum[0] += (WORD)(((v >> 8) & 0xff00) | ((v >> 24) & 0xff));
        sum[1] += (WORD)(((v << 8) & 0xff00) | ((v >> 8) & 0xff));
    }
    sum[0] /= (LONG)frames;
    sum[1] /= (LONG)frames;
    for (i = 8; i < frames - 8; i++)
    {
        ULONG v = c[(start + i) % frames];
        LONG s[2], ch;
        s[0] = (WORD)(((v >> 8) & 0xff00) | ((v >> 24) & 0xff)) - sum[0];
        s[1] = (WORD)(((v << 8) & 0xff00) | ((v >> 8) & 0xff)) - sum[1];
        for (ch = 0; ch < 2; ch++)
        {
            LONG a = s[ch] < 0 ? -s[ch] : s[ch];
            if (a > peak[ch]) peak[ch] = a;
            /* hysteresis against noise */
            if (prev[ch] < -64 && s[ch] >= 64)
                zc[ch]++;
            if (s[ch] >= 64 || s[ch] <= -64)
                prev[ch] = s[ch];
        }
    }
    printf("  %-28s L: peak %5ld ~%5lu Hz   R: peak %5ld ~%5lu Hz  %s\n", label,
           peak[0], zc[0] * 48000 / (frames - 16), peak[1], zc[1] * 48000 / (frames - 16),
           (peak[0] > 300 || peak[1] > 300) &&
           (zc[0] * 48000 / (frames - 16) > tone * 8 / 10 ||
            zc[1] * 48000 / (frames - 16) > tone * 8 / 10) ? "<- tone found" : "");
}

static void test_aux(ULONG monitor_att)
{
    struct MsgPort *port;
    struct IOAudio *io;
    BYTE *sample;
    UBYTE chans = 0x0f;
    const char *err;
    volatile struct Custom *custom = (struct Custom *)0xdff000;
    ULONG tone = 1000, n;
    UBYTE oldmon = ca_get_monitor(&hw);

    err = ca_dma_alloc(&hw, CAP_BYTES, 0);
    if (err)
    {
        printf("cactl: %s\n", err);
        return;
    }
    port = CreateMsgPort();
    io = (struct IOAudio *)CreateIORequest(port, sizeof(struct IOAudio));
    sample = AllocMem(32, MEMF_CHIP | MEMF_CLEAR);
    if (!port || !io || !sample)
        goto out;
    io->ioa_Request.io_Message.mn_Node.ln_Pri = 127;
    io->ioa_Data = &chans;
    io->ioa_Length = 1;
    if (OpenDevice(AUDIONAME, 0, (struct IORequest *)io, 0))
    {
        printf("cactl: cannot allocate Paula channels (audio.device busy)\n");
        io->ioa_Request.io_Device = NULL;
        goto out;
    }
    for (n = 0; n < 32; n++)
        sample[n] = (BYTE)(sin(2.0 * 3.14159265 * n / 32.0) * 100.0);

    printf("TESTAUX: card input %s, gain codes %02x/%02x\n",
           input_names[hw.input], hw.gain_l, hw.gain_r);
    measure("Paula silent, ADC input", 0x333300e4UL, tone);

    /* 1 kHz on all four Paula channels (PAL clock 3546895 Hz) */
    for (n = 0; n < 4; n++)
    {
        custom->aud[n].ac_ptr = (UWORD *)sample;
        custom->aud[n].ac_len = 16;
        custom->aud[n].ac_per = 3546895UL / (32 * tone);
        custom->aud[n].ac_vol = 64;
    }
    custom->dmacon = DMAF_SETCLR | DMAF_AUD0 | DMAF_AUD1 | DMAF_AUD2 | DMAF_AUD3;
    Delay(5);

    measure("Paula 1 kHz, ADC input", 0x333300e4UL, tone);
    ca_set_monitor(&hw, 0xff);
    measure("Paula 1 kHz, mixer, mon off", 0x111100e4UL, tone);
    ca_set_monitor(&hw, (UBYTE)monitor_att);
    measure("Paula 1 kHz, mixer, mon on", 0x111100e4UL, tone);
    ca_ptr_write(&hw, CAPTURE_ROUTING2, 0, 0x10101010UL);
    measure("  same, 0x68=10101010", 0x111100e4UL, tone);
    ca_ptr_write(&hw, CAPTURE_ROUTING2, 0, 0x76767676UL);

    custom->dmacon = DMAF_AUD0 | DMAF_AUD1 | DMAF_AUD2 | DMAF_AUD3;
    for (n = 0; n < 4; n++)
        custom->aud[n].ac_vol = 0;
    ca_set_monitor(&hw, oldmon);
    ca_ptr_write(&hw, CAPTURE_SOURCE, 0, 0x333300e4UL);

out:
    if (io && io->ioa_Request.io_Device)
        CloseDevice((struct IORequest *)io);
    if (sample)
        FreeMem(sample, 32);
    if (io)
        DeleteIORequest((struct IORequest *)io);
    if (port)
        DeleteMsgPort(port);
    ca_chip_stop_dma(&hw);
    ca_dma_free(&hw);
}

int main(void)
{
    LONG args[9] = { 0 };
    struct RDArgs *rda;
    const char *err;
    LONG tenths;
    BOOL initneeded;

    rda = ReadArgs("INIT/S,INPUT/K,GAIN/K,MONITOR/K,OUTPUT/K,MONROUTE/K,TESTAUX/S,STATUS/S,FORCE/S",
                   args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "cactl");
        return RETURN_FAIL;
    }

    err = ca_open(&hw, args[8] != 0);
    if (err)
    {
        printf("cactl: %s\n", err);
        ca_close(&hw);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    initneeded = args[0] || !ca_chip_is_running(&hw);
    if (initneeded)
    {
        /* defaults: AUX input (Paula loopback), monitor off until asked */
        hw.input = CA_INPUT_AUX;
        err = ca_chip_init(&hw);
        if (err)
            printf("cactl: warning: %s\n", err);
    }
    else if (!ca_load_state(&hw))
    {
        hw.gain_l = hw.gain_r = ADC_GAIN_0DB;
        hw.input = CA_INPUT_AUX;
    }

    if (args[1])
    {
        int i;
        for (i = 0; i < CA_INPUTS; i++)
            if (!strcasecmp((char *)args[1], input_names[i]))
                break;
        if (i == CA_INPUTS)
            printf("cactl: unknown INPUT %s (LINE, MIC, AUX, CD)\n", (char *)args[1]);
        else
            ca_set_input(&hw, (UBYTE)i);
    }

    if (args[2] && parse_db((char *)args[2], &tenths))
    {
        LONG code = ADC_GAIN_0DB + (tenths * 2) / 10;   /* 0.5 dB steps */
        if (code < 1) code = 1;
        if (code > 0xff) code = 0xff;
        ca_set_input_gain(&hw, (UBYTE)code, (UBYTE)code);
    }

    if (args[3])
    {
        if (!strcasecmp((char *)args[3], "OFF"))
            ca_set_monitor(&hw, 0xff);
        else if (parse_db((char *)args[3], &tenths))
            ca_set_monitor(&hw, db_to_att(tenths));
    }

    if (args[4] && parse_db((char *)args[4], &tenths))
        ca_set_output(&hw, db_to_att(tenths));

    if (args[5])
        ca_ptr_write(&hw, CAPTURE_ROUTING2, 0, strtoul((char *)args[5], NULL, 16));

    if (args[6])
    {
        UBYTE m = ca_get_monitor(&hw);
        test_aux(m == 0xff ? 0x30 : m);
    }

    if (args[7] || (!args[1] && !args[2] && !args[3] && !args[4] && !args[5] && !args[6]))
    {
        UBYTE mon = ca_get_monitor(&hw);
        ULONG v2 = ca_ptr_read(&hw, PLAYBACK_VOLUME2, 0);
        printf("CA0106 %s, %s\n", ca_model_name(hw.serial),
               ca_chip_is_running(&hw) ? "running" : "stopped");
        printf("  input      %s\n", input_names[hw.input]);
        print_db("gain", ((LONG)hw.gain_l - ADC_GAIN_0DB) * 5);
        if (mon == 0xff)
            printf("  monitor    off\n");
        else
            print_db("monitor", att_to_tenths(mon));
        print_db("output", att_to_tenths((UBYTE)(v2 & 0xff)));
        {
            ULONG dc = ca_read_debug_counters(&hw);
            printf("  driver     %lu underruns, %lu CPU-limit skips (since last AHI open)\n",
                   dc >> 16, dc & 0xffff);
        }
        printf("  regs: HCFG=%08lx GPIO=%08lx 0x68=%08lx 0x69=%08lx 0x6a=%08lx 0x71/1=%08lx\n",
               ca_inl(&hw, CA0106_HCFG), ca_inl(&hw, CA0106_GPIO),
               ca_ptr_read(&hw, CAPTURE_ROUTING2, 0), ca_ptr_read(&hw, CAPTURE_MUTE, 0),
               v2, ca_ptr_read(&hw, CAPTURE_CONTROL, 1));
    }

    ca_close(&hw);      /* codecs keep running */
    FreeArgs(rda);
    (void)vers;
    return RETURN_OK;
}
