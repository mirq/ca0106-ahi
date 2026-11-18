/*
 * ahistream - stress tests for ca0106.audio through ahi.device.
 *
 * ahistream [MODE=$00CA0002] [RATE=44100] [MIXFREQ=48000] [PLAYERFREQ=0]
 *           [SECS=4] [VOL=40] [HALF=4096] [NOLOOP] [SCAN=n] [ALLOC=n]
 *
 * Default: streams a phase-continuous sine at RATE through a double
 * buffered dynamic sound (refilled by a task on SoundFunc, as MP3 players
 * do), records the card's own mixer output and reports every
 * discontinuity found in the whole recording.
 *
 * SCAN=n   n passes over all audio modes querying every attribute the AHI
 *          mode requester uses (reproduces requester crashes).
 * ALLOC=n  n AllocAudio/FreeAudio cycles on MODE, including a second
 *          concurrent AllocAudio that must fail cleanly.
 *
 * GPL-2.0-or-later.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <devices/ahi.h>
#include <utility/hooks.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/ahi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <libraries/openpci.h>
#include <inline/openpci.h>

#include "ca0106_regs.h"
#include "ca0106_hw.h"

struct Library *AHIBase;

/* ---------------- recording ---------------- */
static WORD *recbuf;
static volatile ULONG rec_frames, rec_max, rec_calls;

static ULONG rec_hook(struct Hook *h __asm("a0"), struct AHIAudioCtrl *ac __asm("a2"),
                      struct AHIRecordMessage *m __asm("a1"))
{
    ULONG n = m->ahirm_Length;
    (void)h; (void)ac;
    rec_calls++;
    if (rec_frames + n > rec_max)
        n = rec_max - rec_frames;
    if (n)
    {
        CopyMem(m->ahirm_Buffer, recbuf + rec_frames * 2, n * 4);
        rec_frames += n;
    }
    return 0;
}
static struct Hook RecHook = { { 0 }, (ULONG (*)())rec_hook, 0, 0 };

/* ---------------- streaming ---------------- */
static struct Task *main_task;
static BYTE sound_sig = -1;
static volatile ULONG sound_events;

static ULONG sound_hook(struct Hook *h __asm("a0"), struct AHIAudioCtrl *ac __asm("a2"),
                        struct AHISoundMessage *m __asm("a1"))
{
    (void)h; (void)ac; (void)m;
    sound_events++;
    Signal(main_task, 1UL << sound_sig);
    return 0;
}
static struct Hook SoundHook = { { 0 }, (ULONG (*)())sound_hook, 0, 0 };

static volatile ULONG player_calls;
static ULONG player_hook(struct Hook *h __asm("a0"), struct AHIAudioCtrl *ac __asm("a2"),
                         APTR m __asm("a1"))
{
    (void)h; (void)ac; (void)m;
    player_calls++;
    return 0;
}
static struct Hook PlayerHook = { { 0 }, (ULONG (*)())player_hook, 0, 0 };

static double phase, dphase;
static LONG amp;

static void gen(WORD *dst, ULONG frames)
{
    ULONG i;
    for (i = 0; i < frames; i++)
    {
        WORD s = (WORD)(sin(phase) * amp);
        dst[i * 2] = s;
        dst[i * 2 + 1] = s;
        phase += dphase;
        if (phase > 2.0 * 3.14159265358979)
            phase -= 2.0 * 3.14159265358979;
    }
}

static void set_capture_source(ULONG v)
{
    struct pci_dev *dev;
    volatile UBYTE *io;

    OpenPciBase = OpenLibrary("openpci.library", 2);
    if (!OpenPciBase)
        return;
    dev = pci_find_device(CA0106_VENDOR, CA0106_DEVICE, NULL);
    if (dev)
    {
        io = (volatile UBYTE *)(dev->base_address[0] & ~3UL);
        Disable();
        *(volatile ULONG *)(io + 0) = ca_swap32((ULONG)CAPTURE_SOURCE << 16);
        *(volatile ULONG *)(io + 4) = ca_swap32(v);
        Enable();
    }
    CloseLibrary(OpenPciBase);
    OpenPciBase = NULL;
}

static ULONG read_driver_counters(void)
{
    struct pci_dev *dev;
    volatile UBYTE *io;
    ULONG v = 0;

    OpenPciBase = OpenLibrary("openpci.library", 2);
    if (!OpenPciBase)
        return 0;
    dev = pci_find_device(CA0106_VENDOR, CA0106_DEVICE, NULL);
    if (dev)
    {
        io = (volatile UBYTE *)(dev->base_address[0] & ~3UL);
        Disable();
        *(volatile ULONG *)(io + 0) = ca_swap32((0x0eUL << 16) | 3);
        v = ca_swap32(*(volatile ULONG *)(io + 4));
        Enable();
    }
    CloseLibrary(OpenPciBase);
    OpenPciBase = NULL;
    return v;
}

static void analyse(ULONG mixfreq, ULONG rate, ULONG freq)
{
    ULONG n = rec_frames, i, ch, start, glitches = 0, gaps = 0, run0 = 0;
    LONG peak = 0, prev = 0;
    /* AHI non-HiFi mixing is sample-and-hold resampling: allow the largest
     * step of the source sine, plus margin */
    LONG lim = (LONG)(2.0 * 3.14159265 * freq / (rate < mixfreq ? rate : mixfreq) * amp * 1.35) + 96;
    ULONG first_glitch[8], ng = 0;

    /* skip the first 200 ms (start-up, loopback switch) */
    start = mixfreq / 5;
    if (n <= start + 100)
    {
        printf("analysis: only %lu frames recorded\n", n);
        return;
    }
    for (ch = 0; ch < 2; ch++)
    {
        prev = recbuf[start * 2 + ch];
        run0 = 0;
        for (i = start + 1; i < n; i++)
        {
            LONG s = recbuf[i * 2 + ch], d = s - prev, a = s < 0 ? -s : s;
            if (d < 0) d = -d;
            if (a > peak) peak = a;
            if (d > lim)
            {
                glitches++;
                if (ng < 8 && ch == 0)
                    first_glitch[ng++] = i;
            }
            if (s > -48 && s < 48)
            {
                if (++run0 == mixfreq / 1000 * 2)   /* 2 ms of ~silence */
                    gaps++;
            }
            else
                run0 = 0;
            prev = s;
        }
    }
    printf("analysed %lu frames (%lu ms): peak %ld, step limit %ld\n",
           n - start, (n - start) * 1000 / mixfreq, peak, lim);
    printf("  discontinuities: %lu   silent gaps (>2 ms): %lu\n", glitches, gaps);
    if (ng)
    {
        ULONG g = first_glitch[0], k;
        printf("  around first (L):");
        for (k = g - 6; k <= g + 4; k++)
            printf(" %d", recbuf[k * 2]);
        printf("\n");
        printf("  first glitches at ms:");
        for (i = 0; i < ng; i++)
            printf(" %lu.%lu", first_glitch[i] * 1000 / mixfreq,
                   (first_glitch[i] * 10000 / mixfreq) % 10);
        printf("\n");
    }
    printf("STREAM check: %s\n", (glitches == 0 && gaps == 0 && peak > amp / 3) ? "PASS" : "FAIL");
}

/* ---------------- requester-style mode scan ---------------- */
static ULONG scan(ULONG passes)
{
    ULONG id, p, modes = 0, calls = 0;
    char name[80], author[80], copy[80], ver[80], ann[160], inp[80], outp[80];

    for (p = 0; p < passes; p++)
    {
        id = AHI_INVALID_ID;
        while ((id = AHI_NextAudioID(id)) != AHI_INVALID_ID)
        {
            ULONG bits = 0, maxch = 0, minmix = 0, maxmix = 0, nfreq = 0, rec = 0, fd = 0,
                  rt = 0, ninp = 0, nout = 0, mps = 0, mrs = 0, f0 = 0, idx = 0, i;
            LONG minmon = 0, maxmon = 0, ming = 0, maxg = 0, minv = 0, maxv = 0;
            AHI_GetAudioAttrs(id, NULL,
                AHIDB_BufferLen, 80,
                AHIDB_Name, (ULONG)name,
                AHIDB_Author, (ULONG)author,
                AHIDB_Copyright, (ULONG)copy,
                AHIDB_Version, (ULONG)ver,
                AHIDB_Bits, (ULONG)&bits,
                AHIDB_MaxChannels, (ULONG)&maxch,
                AHIDB_MinMixFreq, (ULONG)&minmix,
                AHIDB_MaxMixFreq, (ULONG)&maxmix,
                AHIDB_Frequencies, (ULONG)&nfreq,
                AHIDB_Record, (ULONG)&rec,
                AHIDB_FullDuplex, (ULONG)&fd,
                AHIDB_Realtime, (ULONG)&rt,
                AHIDB_Inputs, (ULONG)&ninp,
                AHIDB_Outputs, (ULONG)&nout,
                AHIDB_MaxPlaySamples, (ULONG)&mps,
                AHIDB_MaxRecordSamples, (ULONG)&mrs,
                AHIDB_MinMonitorVolume, (ULONG)&minmon,
                AHIDB_MaxMonitorVolume, (ULONG)&maxmon,
                AHIDB_MinInputGain, (ULONG)&ming,
                AHIDB_MaxInputGain, (ULONG)&maxg,
                AHIDB_MinOutputVolume, (ULONG)&minv,
                AHIDB_MaxOutputVolume, (ULONG)&maxv,
                TAG_DONE);
            AHI_GetAudioAttrs(id, NULL, AHIDB_BufferLen, 160, AHIDB_Annotation, (ULONG)ann, TAG_DONE);
            calls += 2;
            for (i = 0; i < nfreq && i < 32; i++)
            {
                AHI_GetAudioAttrs(id, NULL, AHIDB_FrequencyArg, i, AHIDB_Frequency, (ULONG)&f0, TAG_DONE);
                AHI_GetAudioAttrs(id, NULL, AHIDB_IndexArg, f0, AHIDB_Index, (ULONG)&idx, TAG_DONE);
                calls += 2;
            }
            for (i = 0; i < ninp && i < 16; i++)
            {
                AHI_GetAudioAttrs(id, NULL, AHIDB_BufferLen, 80, AHIDB_InputArg, i,
                                  AHIDB_Input, (ULONG)inp, TAG_DONE);
                calls++;
            }
            for (i = 0; i < nout && i < 16; i++)
            {
                AHI_GetAudioAttrs(id, NULL, AHIDB_BufferLen, 80, AHIDB_OutputArg, i,
                                  AHIDB_Output, (ULONG)outp, TAG_DONE);
                calls++;
            }
            if (p == 0)
                printf("  $%08lx %-32s bits %lu freqs %lu (%lu-%lu) in %lu out %lu maxplay %lu\n",
                       id, name, bits, nfreq, minmix, maxmix, ninp, nout, mps);
            modes++;
        }
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
            break;
    }
    printf("scan: %lu passes, %lu mode visits, %lu GetAudioAttrs calls\n", p, modes, calls);
    return 0;
}

static void alloc_cycles(ULONG mode, ULONG n)
{
    ULONG i, ok = 0, busyok = 0;
    for (i = 0; i < n; i++)
    {
        struct AHIAudioCtrl *a1, *a2;
        a1 = AHI_AllocAudio(AHIA_AudioID, mode, AHIA_MixFreq, 44100, AHIA_Channels, 2,
                            AHIA_Sounds, 1, TAG_DONE);
        if (a1)
        {
            ok++;
            if (i & 1)
                AHI_ControlAudio(a1, AHIC_Play, TRUE, TAG_DONE);
            a2 = AHI_AllocAudio(AHIA_AudioID, mode, AHIA_MixFreq, 48000, AHIA_Channels, 2,
                                AHIA_Sounds, 1, TAG_DONE);
            if (!a2)
                busyok++;
            else
                AHI_FreeAudio(a2);
            if (i & 1)
                AHI_ControlAudio(a1, AHIC_Play, FALSE, TAG_DONE);
            AHI_FreeAudio(a1);
        }
    }
    printf("alloc: %lu/%lu allocations ok, %lu second allocations refused cleanly\n", ok, n, busyok);
}

int main(void)
{
    LONG *args[11] = { 0 };
    struct RDArgs *rda;
    struct MsgPort *port = NULL;
    struct AHIRequest *req = NULL;
    struct AHIAudioCtrl *ac = NULL;
    struct AHISampleInfo si;
    WORD *sbuf = NULL;
    ULONG mode = 0x00CA0002, rate = 44100, mixfreq = 48000, pfreq = 0, secs = 4, vol = 40;
    ULONG half = 4096, got_mix = 0, freq = 997, next_half, filled = 0, dc0, dc1;
    BOOL loop, static_mode;
    int rc = RETURN_FAIL;

    rda = ReadArgs("MODE/K,RATE/N,MIXFREQ/N,PLAYERFREQ/N,SECS/N,VOL/N,HALF/N,NOLOOP/S,SCAN/N,ALLOC/N,STATIC/S",
                   (LONG *)args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "ahistream");
        return RETURN_FAIL;
    }
    if (args[0])
    {
        char *s = (char *)args[0];
        if (*s == '$') s++;
        else if (s[0] == '0' && (s[1] | 0x20) == 'x') s += 2;
        mode = strtoul(s, NULL, 16);
    }
    if (args[1]) rate = *args[1];
    if (args[2]) mixfreq = *args[2];
    if (args[3]) pfreq = *args[3];
    if (args[4]) secs = *args[4];
    if (args[5]) vol = *args[5];
    if (args[6]) half = *args[6];
    loop = !args[7];
    static_mode = args[10] != 0;

    port = CreateMsgPort();
    req = (struct AHIRequest *)CreateIORequest(port, sizeof(*req));
    if (!req)
        goto out;
    req->ahir_Version = 4;
    if (OpenDevice(AHINAME, AHI_NO_UNIT, (struct IORequest *)req, 0))
    {
        printf("cannot open ahi.device\n");
        req->ahir_Std.io_Device = NULL;
        goto out;
    }
    AHIBase = (struct Library *)req->ahir_Std.io_Device;

    if (args[8])
    {
        scan(*args[8]);
        rc = RETURN_OK;
        goto out;
    }
    if (args[9])
    {
        alloc_cycles(mode, *args[9]);
        rc = RETURN_OK;
        goto out;
    }

    main_task = FindTask(NULL);
    sound_sig = AllocSignal(-1);
    rec_max = (secs + 1) * 96000;
    recbuf = AllocVec(rec_max * 4, MEMF_PUBLIC | MEMF_CLEAR);
    sbuf = AllocVec(half * 2 * 4, MEMF_PUBLIC | MEMF_CLEAR);
    if (sound_sig < 0 || !recbuf || !sbuf)
        goto out;

    ac = AHI_AllocAudio(AHIA_AudioID, mode,
                        AHIA_MixFreq, mixfreq,
                        AHIA_Channels, 1,
                        AHIA_Sounds, 1,
                        AHIA_SoundFunc, (ULONG)&SoundHook,
                        AHIA_RecordFunc, (ULONG)&RecHook,
                        pfreq ? AHIA_PlayerFunc : TAG_IGNORE, (ULONG)&PlayerHook,
                        pfreq ? AHIA_PlayerFreq : TAG_IGNORE, pfreq << 16,
                        pfreq ? AHIA_MinPlayerFreq : TAG_IGNORE, pfreq << 16,
                        pfreq ? AHIA_MaxPlayerFreq : TAG_IGNORE, pfreq << 16,
                        TAG_DONE);
    if (!ac)
    {
        printf("AHI_AllocAudio failed\n");
        goto out;
    }
    AHI_ControlAudio(ac, AHIC_MixFreq_Query, (ULONG)&got_mix, TAG_DONE);

    amp = 32767 * vol / 100;
    if (static_mode)
    {
        ULONG cyc = (freq * 2 * half + rate / 2) / rate;
        freq = cyc * rate / (2 * half);
        dphase = 2.0 * 3.14159265358979 * cyc / (2.0 * half);
    }
    else
    dphase = 2.0 * 3.14159265358979 * freq / rate;
    gen(sbuf, 2 * half);
    filled = 2;
    si.ahisi_Type = AHIST_S16S;
    si.ahisi_Address = sbuf;
    si.ahisi_Length = 2 * half;
    if (AHI_LoadSound(0, AHIST_DYNAMICSAMPLE, &si, ac) != AHIE_OK)
    {
        printf("AHI_LoadSound failed\n");
        goto out;
    }
    printf("mode $%08lx, mixing %lu Hz, stream %lu Hz stereo, half %lu frames (%lu ms), player freq %lu\n",
           mode, got_mix, rate, half, half * 1000 / rate, pfreq ? pfreq : 50);

    if (loop)
        set_capture_source(0x111100e4UL);
    dc0 = read_driver_counters();

    if (static_mode)
        AHI_Play(ac, AHIP_BeginChannel, 0,
                 AHIP_Freq, rate, AHIP_Vol, 0x10000, AHIP_Pan, 0x8000,
                 AHIP_Sound, 0, AHIP_EndChannel, 0, TAG_DONE);
    else
        AHI_Play(ac, AHIP_BeginChannel, 0,
                 AHIP_Freq, rate, AHIP_Vol, 0x10000, AHIP_Pan, 0x8000,
                 AHIP_Sound, 0, AHIP_Offset, 0, AHIP_Length, half,
                 AHIP_LoopSound, 0, AHIP_LoopOffset, half, AHIP_LoopLength, half,
                 AHIP_EndChannel, 0, TAG_DONE);
    AHI_ControlAudio(ac, AHIC_Play, TRUE, loop ? AHIC_Record : TAG_IGNORE, TRUE, TAG_DONE);
    if (static_mode)
    {
        Delay(secs * 50 + 25);
        goto stopped;
    }

    /* SoundFunc fires when a part starts: the 1st event = 1st half started,
     * then every event means the other half just finished playing.
     * Refill it and queue it as the next part (AHISF_NONE = after current). */
    next_half = 0;
    {
        ULONG seen = 0, end = secs * rate / half + 1;
        while (filled < end + 2)
        {
            ULONG sig = Wait((1UL << sound_sig) | SIGBREAKF_CTRL_C);
            if (sig & SIGBREAKF_CTRL_C)
                break;
            while (seen < sound_events)
            {
                seen++;
                if (seen == 1)
                    continue;   /* first half just started, second already queued as loop */
                /* the half that just ended is the one not playing now */
                gen(sbuf + next_half * half * 2, half);
                AHI_SetSound(0, 0, next_half * half, half, ac, 0);
                next_half ^= 1;
                filled++;
            }
        }
    }
stopped:
    AHI_ControlAudio(ac, AHIC_Play, FALSE, AHIC_Record, FALSE, TAG_DONE);
    dc1 = read_driver_counters();
    if (loop)
        set_capture_source(0x333300e4UL);

    printf("sound events %lu, player calls %lu, record calls %lu\n",
           sound_events, player_calls, rec_calls);
    printf("driver: underruns +%lu, CPU-limit skips +%lu\n",
           (dc1 >> 16) - (dc0 >> 16), (dc1 & 0xffff) - (dc0 & 0xffff));
    if (loop)
        analyse(got_mix, rate, freq);
    rc = RETURN_OK;

out:
    if (ac)
        AHI_FreeAudio(ac);
    if (sbuf)
        FreeVec(sbuf);
    if (recbuf)
        FreeVec(recbuf);
    if (sound_sig >= 0)
        FreeSignal(sound_sig);
    if (req && req->ahir_Std.io_Device)
        CloseDevice((struct IORequest *)req);
    if (req)
        DeleteIORequest((struct IORequest *)req);
    if (port)
        DeleteMsgPort(port);
    FreeArgs(rda);
    return rc;
}
