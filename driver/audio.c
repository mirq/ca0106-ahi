/*
 * ca0106.audio - AHI sub-driver implementation.
 *
 * Playback: the CA0106 loops over one DMA buffer (two halves) in Radeon
 * VRAM reached through the Prometheus DMA arena.  Each half-buffer
 * interrupt causes a software interrupt that runs the AHI player and
 * mixer hooks into fast RAM, converts to little endian and copies one half
 * to the card.  The player hook is called exactly once per mixed block, so
 * AHI's PlayerFreq timing is preserved independently of the half size.
 *
 * Recording: capture runs into a second DMA buffer; each full half is
 * byte-swapped into fast RAM and handed to ahiac_SamplerFunc.
 *
 * Monitoring: the WM8775 input is mixed into the analog output inside the
 * CA0106 (CAPTURE_CONTROL channel 1), controlled with AHIC_MonitorVolume.
 * It keeps running after FreeAudio so Paula can be heard through Aux.
 *
 * GPL-2.0-or-later.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/utility.h>

#include "driver.h"
#include "ca0106_regs.h"

extern const char LibName[];

static struct CADrv *Active;

#define PLAY_HALF_48    480     /* 10 ms */
#define REC_HALF_48     1920    /* 40 ms */

static const char *const InputNames[CA_INPUTS] =
{
    "Line in",
    "Mic",
    "Aux (Amiga audio)",
    "CD / TAD"
};

/* ---------------------------------------------------------------------- */

static inline ULONG call_timer(APTR func, struct AHIAudioCtrlDrv *ac)
{
    register ULONG d0 __asm("d0");
    register APTR a0 __asm("a0") = func;
    register struct AHIAudioCtrlDrv *a2 __asm("a2") = ac;
    __asm__ volatile ("jsr (%1)"
                      : "=r"(d0), "+a"(a0)
                      : "a"(a2)
                      : "d1", "a1", "cc", "memory");
    return d0;
}

static inline ULONG swap16x2(ULONG v)
{
    return ((v & 0x00ff00ffUL) << 8) | ((v >> 8) & 0x00ff00ffUL);
}

/* AHI mixing buffer -> card format (S16 LE stereo or S32 LE stereo) */
static void convert(struct CADrv *d, const void *src, UBYTE *dst, ULONG n)
{
    switch (d->buftype)
    {
        case AHIST_S16S:
        {
            const ULONG *s = src;
            ULONG *o = (ULONG *)dst;
            while (n--)
                *o++ = swap16x2(*s++);
            break;
        }
        case AHIST_M16S:
        {
            const UWORD *s = src;
            ULONG *o = (ULONG *)dst;
            while (n--)
            {
                ULONG w = ca_swap16(*s++);
                *o++ = (w << 16) | w;
            }
            break;
        }
        case AHIST_S32S:
        {
            const ULONG *s = src;
            ULONG *o = (ULONG *)dst;
            n *= 2;
            while (n--)
                *o++ = ca_swap32(*s++);
            break;
        }
        case AHIST_M32S:
        {
            const ULONG *s = src;
            ULONG *o = (ULONG *)dst;
            while (n--)
            {
                ULONG w = ca_swap32(*s++);
                *o++ = w;
                *o++ = w;
            }
            break;
        }
    }
}

static ULONG ahi_frame_size(ULONG buftype)
{
    switch (buftype)
    {
        case AHIST_M16S: return 2;
        case AHIST_S16S: return 4;
        case AHIST_M32S: return 4;
        case AHIST_S32S: return 8;
        default:         return 0;
    }
}

/* ---------------------------------------------------------------------- */
/* Interrupt side                                                          */
/* ---------------------------------------------------------------------- */

static void drv_irq(struct CAHw *hw, ULONG ext, ULONG ipr)
{
    struct CADrv *d = (struct CADrv *)hw;
    ULONG p = 0;

    (void)ipr;
    if (ext & EXTINT_PLAY_HALF)       p |= PEND_P0;
    if (ext & EXTINT_PLAY_FULL)       p |= PEND_P1;
    if (ext & EXTINT_CAP_HALF_BUFFER) p |= PEND_R0;
    if (ext & EXTINT_CAP_FULL_BUFFER) p |= PEND_R1;
    if (!p)
        return;
    d->pending |= p;
    if ((p & (PEND_P0 | PEND_P1)) && d->playing)
        Cause(&d->play_int);
    if ((p & (PEND_R0 | PEND_R1)) && d->recording)
        Cause(&d->rec_int);
}

/*
 * Fill one hardware half.  All mixing passes needed for it run as one
 * burst inside a single PreTimer/PostTimer pair: AHI measures load as
 * (PostTimer - PreTimer) / (time since the previous PreTimer), so calling
 * PreTimer for back-to-back passes reads as ~100% CPU and makes AHI skip
 * the block (audible as "bubbling").
 */
static void play_fill(struct CADrv *d, ULONG half)
{
    struct AHIAudioCtrlDrv *ac = d->ac;
    ULONG need = d->half_frames;
    ULONG fs = d->hw_fsize;
    ULONG cap = d->stage_bytes / fs;
    ULONG *dst, *src, longs, rest;
    BOOL skip = FALSE, timed = FALSE;

    if (d->stage_frames < need && ac->ahiac_PreTimer)
    {
        timed = TRUE;
        if (call_timer((APTR)ac->ahiac_PreTimer, ac))
        {
            skip = TRUE;            /* CPU limit hit: play silence */
            d->overloads++;
        }
    }

    while (d->stage_frames < need)
    {
        ULONG n;
        UBYTE *out;

        CallHookPkt(ac->ahiac_PlayerFunc, ac, NULL);
        n = ac->ahiac_BuffSamples;
        out = d->stage + d->stage_frames * fs;
        if (n == 0 || d->stage_frames + n > cap)
        {
            /* should not happen; pad with silence instead of overflowing */
            n = need - d->stage_frames;
            memset(out, 0, n * fs);
            d->stage_frames += n;
            break;
        }
        if (skip)
            memset(out, 0, n * fs);
        else
        {
            CallHookPkt(ac->ahiac_MixerFunc, ac, d->mixtmp);
            convert(d, d->mixtmp, out, n);
        }
        d->stage_frames += n;
    }
    if (timed)
        call_timer((APTR)ac->ahiac_PostTimer, ac);

    dst = (ULONG *)(d->hw.dma_cpu + PLAY_OFF + half * need * fs);
    src = (ULONG *)d->stage;
    longs = need * fs / 4;
    while (longs--)
        *dst++ = *src++;
    CacheClearE(d->hw.dma_cpu + PLAY_OFF + half * need * fs, need * fs, CACRF_ClearD);

    rest = d->stage_frames - need;
    if (rest)
        memmove(d->stage, d->stage + need * fs, rest * fs);
    d->stage_frames = rest;
    ca_debug_counters(&d->hw, d->underruns, d->overloads);
}

static void play_soft(struct CADrv *d __asm("a1"))
{
    ULONG p;

    Disable();
    p = d->pending & (PEND_P0 | PEND_P1);
    d->pending &= ~p;
    Enable();
    if (!d->playing || !p)
        return;
    if (p == (PEND_P0 | PEND_P1))
    {
        /* we fell behind; refill the half that is not playing now */
        ULONG playing_half = ca_play_pointer(&d->hw) >= d->half_frames * d->hw_fsize;
        d->underruns++;
        play_fill(d, playing_half ^ 1);
        return;
    }
    play_fill(d, (p & PEND_P0) ? 0 : 1);
}

static void rec_deliver(struct CADrv *d, ULONG half)
{
    struct AHIAudioCtrlDrv *ac = d->ac;
    ULONG n = d->rec_half_frames;
    volatile ULONG *src = (volatile ULONG *)(d->hw.dma_cpu + CAP_OFF + half * n * 4);
    ULONG *dst = d->recbuf[d->recsel];
    ULONG i;

    CacheClearE((APTR)src, n * 4, CACRF_ClearD);
    for (i = 0; i < n; i++)
        dst[i] = swap16x2(src[i]);
    d->recmsg.ahirm_Type = AHIST_S16S;
    d->recmsg.ahirm_Buffer = dst;
    d->recmsg.ahirm_Length = n;
    d->recsel ^= 1;
    CallHookPkt(ac->ahiac_SamplerFunc, ac, &d->recmsg);
}

static void rec_soft(struct CADrv *d __asm("a1"))
{
    ULONG p;

    Disable();
    p = d->pending & (PEND_R0 | PEND_R1);
    d->pending &= ~p;
    Enable();
    if (!d->recording)
        return;
    if (p & PEND_R0)
        rec_deliver(d, 0);
    if (p & PEND_R1)
        rec_deliver(d, 1);
}

/* ---------------------------------------------------------------------- */
/* AHI entry points                                                        */
/* ---------------------------------------------------------------------- */

static void drv_release(struct CADrv *d)
{
    if (d->hw.owned)
        ca_chip_stop_dma(&d->hw);
    ca_close(&d->hw);       /* removes irq, frees DMA, releases card */
    FreeVec(d);
}

ULONG drv_AllocAudio(struct TagItem *tags, struct AHIAudioCtrlDrv *ac)
{
    struct CADrv *d;

    (void)tags;
    ac->ahiac_DriverData = NULL;

    Forbid();
    if (Active)
    {
        Permit();
        return AHISF_ERROR;
    }
    d = AllocVec(sizeof(*d), MEMF_PUBLIC | MEMF_CLEAR);
    Active = d;
    Permit();
    if (!d)
        return AHISF_ERROR;

    d->ac = ac;
    if (ca_open(&d->hw, FALSE) || ca_dma_alloc(&d->hw, DMA_BYTES, 0))
        goto fail;

    if (!ca_chip_is_running(&d->hw))
    {
        d->hw.input = CA_INPUT_AUX;
        if (ca_chip_init(&d->hw))
            goto fail;      /* codecs did not answer: report it to AHI */
    }
    else if (!ca_load_state(&d->hw))
    {
        d->hw.input = CA_INPUT_AUX;
        d->hw.gain_l = d->hw.gain_r = ADC_GAIN_0DB;
        ca_set_input(&d->hw, d->hw.input);
    }

    d->play_int.is_Node.ln_Type = NT_INTERRUPT;
    d->play_int.is_Node.ln_Pri = 32;
    d->play_int.is_Node.ln_Name = (char *)LibName;
    d->play_int.is_Data = d;
    d->play_int.is_Code = (void (*)(void))play_soft;
    d->rec_int = d->play_int;
    d->rec_int.is_Code = (void (*)(void))rec_soft;

    if (!ca_irq_add(&d->hw, drv_irq, d, LibName))
        goto fail;

    d->rate = ac->ahiac_MixFreq >= 72000 ? 96000 : 48000;
    d->rate_code = d->rate == 96000 ? CA_RATE_96000 : CA_RATE_48000;
    ac->ahiac_MixFreq = d->rate;

    d->monitor = 0;
    d->gain = 0x10000;
    d->outvol = 0x10000;
    d->input = d->hw.input;
    ca_debug_counters(&d->hw, 0, 0);

    ac->ahiac_DriverData = d;
    return AHISF_KNOWHIFI | AHISF_KNOWSTEREO | AHISF_MIXING | AHISF_TIMING |
           AHISF_CANRECORD;

fail:
    drv_release(d);
    Active = NULL;
    return AHISF_ERROR;
}

void drv_FreeAudio(struct AHIAudioCtrlDrv *ac)
{
    struct CADrv *d = ac->ahiac_DriverData;

    if (!d)
        return;
    drv_Stop(AHISF_PLAY | AHISF_RECORD, ac);
    /* the codecs and the monitor (Aux loopback) stay active */
    drv_release(d);
    ac->ahiac_DriverData = NULL;
    Active = NULL;
}

ULONG drv_Start(ULONG flags, struct AHIAudioCtrlDrv *ac)
{
    struct CADrv *d = ac->ahiac_DriverData;

    if (!d)
        return AHIE_UNKNOWN;
    drv_Stop(flags, ac);

    if (flags & AHISF_PLAY)
    {
        ULONG afs, maxn, i;

        d->buftype = ac->ahiac_BuffType;
        afs = ahi_frame_size(d->buftype);
        if (!afs)
            return AHIE_BADSAMPLETYPE;
        d->hw_s32 = (d->buftype == AHIST_S32S || d->buftype == AHIST_M32S);
        d->hw_fsize = d->hw_s32 ? 8 : 4;
        d->half_frames = PLAY_HALF_48 * (d->rate / 48000);

        maxn = ac->ahiac_MaxBuffSamples;
        if (ac->ahiac_BuffSize / afs > maxn)
            maxn = ac->ahiac_BuffSize / afs;
        d->stage_bytes = (d->half_frames + maxn) * d->hw_fsize;
        d->mixtmp = AllocVec(ac->ahiac_BuffSize + 16, MEMF_PUBLIC | MEMF_CLEAR);
        d->stage = AllocVec(d->stage_bytes, MEMF_PUBLIC | MEMF_CLEAR);
        if (!d->mixtmp || !d->stage)
            return AHIE_NOMEM;
        d->stage_frames = 0;

        for (i = 0; i < 2 * d->half_frames * d->hw_fsize; i += 4)
            *(volatile ULONG *)(d->hw.dma_cpu + PLAY_OFF + i) = 0;
        CacheClearE(d->hw.dma_cpu + PLAY_OFF, 2 * d->half_frames * d->hw_fsize, CACRF_ClearD);

        ca_play_setup(&d->hw, PLAY_OFF, 2 * d->half_frames * d->hw_fsize,
                      d->rate_code, d->hw_s32);
        Disable();
        d->pending &= ~(PEND_P0 | PEND_P1);
        d->playing = 1;
        Enable();
        ca_play_start(&d->hw);
    }

    if (flags & AHISF_RECORD)
    {
        ULONG bytes;

        d->rec_half_frames = REC_HALF_48 * (d->rate / 48000);
        bytes = d->rec_half_frames * 4;
        d->recbuf[0] = AllocVec(bytes, MEMF_PUBLIC | MEMF_CLEAR);
        d->recbuf[1] = AllocVec(bytes, MEMF_PUBLIC | MEMF_CLEAR);
        if (!d->recbuf[0] || !d->recbuf[1])
            return AHIE_NOMEM;
        d->recsel = 0;
        ca_cap_setup(&d->hw, CAP_OFF, 2 * bytes, d->rate_code);
        Disable();
        d->pending &= ~(PEND_R0 | PEND_R1);
        d->recording = 1;
        Enable();
        ca_cap_start(&d->hw);
    }
    return AHIE_OK;
}

void drv_Update(ULONG flags, struct AHIAudioCtrlDrv *ac)
{
    /* PlayerFreq/BuffSamples changes are picked up per mixing pass */
    (void)flags;
    (void)ac;
}

void drv_Stop(ULONG flags, struct AHIAudioCtrlDrv *ac)
{
    struct CADrv *d = ac->ahiac_DriverData;

    if (!d)
        return;
    if (flags & AHISF_PLAY)
    {
        if (d->playing)
            ca_play_stop(&d->hw);
        Disable();
        d->playing = 0;
        d->pending &= ~(PEND_P0 | PEND_P1);
        Enable();
        if (d->mixtmp)
            FreeVec(d->mixtmp);
        if (d->stage)
            FreeVec(d->stage);
        d->mixtmp = NULL;
        d->stage = NULL;
    }
    if (flags & AHISF_RECORD)
    {
        if (d->recording)
            ca_cap_stop(&d->hw);
        Disable();
        d->recording = 0;
        d->pending &= ~(PEND_R0 | PEND_R1);
        Enable();
        if (d->recbuf[0])
            FreeVec(d->recbuf[0]);
        if (d->recbuf[1])
            FreeVec(d->recbuf[1]);
        d->recbuf[0] = d->recbuf[1] = NULL;
    }
}

LONG drv_GetAttr(ULONG attr, LONG arg, LONG def, struct TagItem *tags,
                 struct AHIAudioCtrlDrv *ac)
{
    (void)ac;
    switch (attr)
    {
        case AHIDB_Bits:
            return GetTagData(AHIDB_HiFi, FALSE, tags) ? 24 : 16;
        case AHIDB_Frequencies:
            return 2;
        case AHIDB_Frequency:
            return arg == 1 ? 96000 : 48000;
        case AHIDB_Index:
            return arg >= 72000 ? 1 : 0;
        case AHIDB_Author:
            return (LONG)"ca0106.audio contributors";
        case AHIDB_Copyright:
            return (LONG)"GPL-2.0-or-later; chip setup from Linux ALSA ca0106 and 4Front OSS";
        case AHIDB_Version:
            return (LONG)LibIdString;
        case AHIDB_Annotation:
            return (LONG)"Sound Blaster Audigy SE (SB0570) front output, "
                         "Line/Mic/Aux recording and hardware monitor";
        case AHIDB_Record:
        case AHIDB_FullDuplex:
        case AHIDB_Realtime:
            return TRUE;
        case AHIDB_MaxPlaySamples:
            return def + 2 * PLAY_HALF_48 * 2;
        case AHIDB_MaxRecordSamples:
            return REC_HALF_48 * 2;
        case AHIDB_MinMonitorVolume:
            return 0;
        case AHIDB_MaxMonitorVolume:
            return 0x3fb28;             /* +12 dB */
        case AHIDB_MinInputGain:
            return 0x1027;              /* -24 dB */
        case AHIDB_MaxInputGain:
            return 0xfd960;             /* +24 dB */
        case AHIDB_MinOutputVolume:
            return 0;
        case AHIDB_MaxOutputVolume:
            return 0x10000;
        case AHIDB_Inputs:
            return CA_INPUTS;
        case AHIDB_Input:
            return (LONG)InputNames[(ULONG)arg < CA_INPUTS ? arg : 0];
        case AHIDB_Outputs:
            return 1;
        case AHIDB_Output:
            return (LONG)"Line out (front)";
        default:
            return def;
    }
}

ULONG drv_HardwareControl(ULONG attr, LONG arg, struct AHIAudioCtrlDrv *ac)
{
    struct CADrv *d = ac ? ac->ahiac_DriverData : NULL;

    if (!d)
        return FALSE;
    switch (attr)
    {
        case AHIC_MonitorVolume:
            d->monitor = arg;
            ca_set_monitor(&d->hw, ca_fixed_to_att(arg));
            return TRUE;
        case AHIC_MonitorVolume_Query:
            return d->monitor;
        case AHIC_InputGain:
        {
            UBYTE code = ca_fixed_to_adcgain(arg);
            d->gain = arg;
            ca_set_input_gain(&d->hw, code, code);
            return TRUE;
        }
        case AHIC_InputGain_Query:
            return d->gain;
        case AHIC_OutputVolume:
            d->outvol = arg;
            ca_set_output(&d->hw, ca_fixed_to_att(arg));
            return TRUE;
        case AHIC_OutputVolume_Query:
            return d->outvol;
        case AHIC_Input:
            if ((ULONG)arg >= CA_INPUTS)
                return FALSE;
            d->input = arg;
            ca_set_input(&d->hw, (UBYTE)arg);
            return TRUE;
        case AHIC_Input_Query:
            return d->input;
        case AHIC_Output:
            return arg == 0;
        case AHIC_Output_Query:
            return 0;
        default:
            return FALSE;
    }
}

void drv_Cleanup(void)
{
}
