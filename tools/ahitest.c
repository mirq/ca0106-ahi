/*
 * ahitest - exercise ca0106.audio through the AHI low-level API.
 *
 * ahitest [MODE=$00CA0001] [FREQ=1000] [SECS=3] [VOL=30] [LOOP] [MIXFREQ=48000]
 *
 * Plays a sine with AHI_Play() and records with AHIA_RecordFunc.  With LOOP
 * the card's capture source is switched to its own I2S mixer output so the
 * recorded data must contain the played tone (automatic end-to-end check).
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
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include <libraries/openpci.h>
#include <inline/openpci.h>

#include "ca0106_regs.h"
#include "ca0106_hw.h"

struct Library *AHIBase;

static volatile ULONG rec_calls, rec_frames, rec_bad;
static WORD rec_copy[4096 * 2];
static volatile ULONG rec_copy_frames;
static volatile ULONG play_calls;

/* AHIA_RecordFunc: A0 hook, A2 audioctrl, A1 AHIRecordMessage */
static ULONG rec_hook(struct Hook *h __asm("a0"), struct AHIAudioCtrl *ac __asm("a2"),
                      struct AHIRecordMessage *m __asm("a1"))
{
    (void)h;
    (void)ac;
    rec_calls++;
    if (m->ahirm_Type != AHIST_S16S)
        rec_bad++;
    rec_frames += m->ahirm_Length;
    /* keep the most recent buffer, but only once enough has been played */
    if (rec_calls > 10 && rec_copy_frames == 0)
    {
        ULONG n = m->ahirm_Length > 4096 ? 4096 : m->ahirm_Length;
        CopyMem(m->ahirm_Buffer, rec_copy, n * 4);
        rec_copy_frames = n;
    }
    return 0;
}

static ULONG player_hook(struct Hook *h __asm("a0"), struct AHIAudioCtrl *ac __asm("a2"),
                         APTR msg __asm("a1"))
{
    (void)h; (void)ac; (void)msg;
    play_calls++;
    return 0;
}

static struct Hook RecHook = { { 0 }, (ULONG (*)())rec_hook, 0, 0 };
static struct Hook PlayHook = { { 0 }, (ULONG (*)())player_hook, 0, 0 };

/* direct register poke for LOOP (the driver owns the card) */
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

int main(void)
{
    LONG *args[6] = { 0 };
    struct RDArgs *rda;
    struct MsgPort *port = NULL;
    struct AHIRequest *req = NULL;
    struct AHIAudioCtrl *ac = NULL;
    struct AHISampleInfo si;
    WORD *sample = NULL;
    ULONG mode = 0x00CA0001, freq = 1000, secs = 3, vol = 30, mixfreq = 48000;
    ULONG len, i, got_mix = 0, playerfreq = 50, panning = TRUE;
    BOOL loop;
    char name[64];
    int rc = RETURN_FAIL;

    rda = ReadArgs("MODE/K,FREQ/N,SECS/N,VOL/N,LOOP/S,MIXFREQ/N", (LONG *)args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "ahitest");
        return RETURN_FAIL;
    }
    if (args[0])
    {
        char *s = (char *)args[0];
        if (*s == '$') s++;
        else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
        mode = strtoul(s, NULL, 16);
    }
    if (args[1]) freq = *args[1];
    if (args[2]) secs = *args[2];
    if (args[3]) vol = *args[3];
    loop = args[4] != 0;
    if (args[5]) mixfreq = *args[5];

    port = CreateMsgPort();
    req = (struct AHIRequest *)CreateIORequest(port, sizeof(*req));
    if (!req)
        goto out;
    req->ahir_Version = 4;
    if (OpenDevice(AHINAME, AHI_NO_UNIT, (struct IORequest *)req, 0))
    {
        printf("ahitest: cannot open ahi.device v4\n");
        req->ahir_Std.io_Device = NULL;
        goto out;
    }
    AHIBase = (struct Library *)req->ahir_Std.io_Device;

    name[0] = 0;
    AHI_GetAudioAttrs(mode, NULL, AHIDB_Name, (ULONG)name, AHIDB_BufferLen, sizeof(name),
                      AHIDB_Panning, (ULONG)&panning, TAG_DONE);
    printf("mode $%08lx \"%s\"%s\n", mode, name, panning ? "" : " (no panning: channel 0 = left)");

    ac = AHI_AllocAudio(AHIA_AudioID, mode,
                        AHIA_MixFreq, mixfreq,
                        AHIA_Channels, 1,
                        AHIA_Sounds, 1,
                        AHIA_PlayerFunc, (ULONG)&PlayHook,
                        AHIA_PlayerFreq, playerfreq << 16,
                        AHIA_MinPlayerFreq, playerfreq << 16,
                        AHIA_MaxPlayerFreq, playerfreq << 16,
                        AHIA_RecordFunc, (ULONG)&RecHook,
                        TAG_DONE);
    if (!ac)
    {
        printf("ahitest: AHI_AllocAudio failed\n");
        goto out;
    }
    AHI_ControlAudio(ac, AHIC_MixFreq_Query, (ULONG)&got_mix, TAG_DONE);
    printf("allocated, mixing at %lu Hz\n", got_mix);

    /* one second of a sine at the mixing rate, looped */
    len = got_mix;
    sample = AllocVec(len * 2, MEMF_PUBLIC);
    if (!sample)
        goto out;
    for (i = 0; i < len; i++)
        sample[i] = (WORD)(sin(2.0 * 3.14159265358979 * freq * i / len) * 32767.0);
    si.ahisi_Type = AHIST_M16S;
    si.ahisi_Address = sample;
    si.ahisi_Length = len;
    if (AHI_LoadSound(0, AHIST_SAMPLE, &si, ac) != AHIE_OK)
    {
        printf("ahitest: AHI_LoadSound failed\n");
        goto out;
    }

    if (loop)
        set_capture_source(0x111100e4UL);   /* I2S mixer output */

    if (AHI_ControlAudio(ac, AHIC_Play, TRUE, AHIC_Record, TRUE, TAG_DONE) != AHIE_OK)
    {
        printf("ahitest: AHIC_Play/Record failed\n");
        goto out;
    }
    AHI_Play(ac, AHIP_BeginChannel, 0,
             AHIP_Freq, got_mix,
             AHIP_Vol, (vol * 0x10000) / 100,
             AHIP_Pan, 0x8000,
             AHIP_Sound, 0,
             AHIP_EndChannel, 0,
             TAG_DONE);

    for (i = 0; i < secs * 10; i++)
    {
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
            break;
        Delay(5);
    }
    AHI_ControlAudio(ac, AHIC_Play, FALSE, AHIC_Record, FALSE, TAG_DONE);
    if (loop)
        set_capture_source(0x333300e4UL);

    printf("player hook: %lu calls in %lu s (expect ~%lu)\n", play_calls, secs, secs * playerfreq);
    printf("record hook: %lu calls, %lu frames (expect ~%lu), bad type %lu\n",
           rec_calls, rec_frames, secs * got_mix, rec_bad);

    if (rec_copy_frames)
    {
        LONG peak[2] = { 0, 0 }, prev[2] = { 0, 0 }, maxstep[2] = { 0, 0 };
        ULONG zc[2] = { 0, 0 }, n = rec_copy_frames, ch;
        /* panning modes: centre pan = -6 dB on both sides;
         * non-panning modes: channel 0 goes to the left at full level */
        LONG amp = panning ? (LONG)(32767 * vol / 100) / 2 : (LONG)(32767 * vol / 100);
        for (i = 0; i < n; i++)
        {
            for (ch = 0; ch < 2; ch++)
            {
                LONG s = rec_copy[i * 2 + ch], a = s < 0 ? -s : s;
                if (a > peak[ch]) peak[ch] = a;
                if (i)
                {
                    LONG dd = s - prev[ch];
                    if (dd < 0) dd = -dd;
                    if (dd > maxstep[ch]) maxstep[ch] = dd;
                    if (prev[ch] < 0 && s >= 0) zc[ch]++;
                }
                prev[ch] = s;
            }
        }
        for (ch = 0; ch < 2; ch++)
            printf("recorded %s: peak %5ld (played ~%ld)  ~%lu Hz  max step %ld\n",
                   ch ? "R" : "L", peak[ch], amp, zc[ch] * got_mix / n, maxstep[ch]);
        if (loop)
        {
            LONG lim = (LONG)(2.0 * 3.14159265 * freq / got_mix * amp * 1.3) + 64;
            BOOL ok = TRUE;
            for (ch = 0; ch < 2; ch++)
            {
                if (!panning && ch == 1)
                {
                    if (peak[1] > 256)
                        ok = FALSE;
                    continue;
                }
                if (peak[ch] < amp / 2 || maxstep[ch] > lim ||
                    zc[ch] * got_mix / n < freq * 8 / 10 || zc[ch] * got_mix / n > freq * 12 / 10)
                    ok = FALSE;
            }
            printf("LOOP check: %s\n", ok ? "PASS" : "FAIL");
            rc = ok ? RETURN_OK : RETURN_WARN;
        }
        else
            rc = RETURN_OK;
    }
    else
        printf("no record data received\n");

out:
    if (ac)
        AHI_FreeAudio(ac);
    if (sample)
        FreeVec(sample);
    if (req && req->ahir_Std.io_Device)
        CloseDevice((struct IORequest *)req);
    if (req)
        DeleteIORequest((struct IORequest *)req);
    if (port)
        DeleteMsgPort(port);
    FreeArgs(rda);
    return rc;
}
