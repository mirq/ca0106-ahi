/*
 * ca0106.audio - AHI driver for the Creative CA0106 (Sound Blaster Audigy SE
 * SB0570) on openpci.library, AmigaOS 68k.
 *
 * GPL-2.0-or-later.
 */
#ifndef CA0106_DRIVER_H
#define CA0106_DRIVER_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/interrupts.h>
#include <dos/dos.h>
#include <devices/ahi.h>
#include <libraries/ahi_sub.h>

#include "ca0106_hw.h"

#define LIB_NAME        "ca0106.audio"
#define LIB_VERSION     4
#define LIB_REVISION    2
#define LIB_DATE        "25.11.2026"
#define LIB_IDSTRING    "ca0106.audio 4.2 (" LIB_DATE ") Audigy SE (CA0106) via openpci"

struct CABase
{
    struct Library      lib;
    UWORD               pad;
    BPTR                seglist;
};

/* DMA block layout (one block per open, worst case 96 kHz HiFi) */
#define PLAY_OFF        0UL
#define PLAY_MAXBYTES   16384UL         /* 2 halves x 960 frames x 8 bytes */
#define CAP_OFF         16384UL
#define CAP_MAXBYTES    30720UL         /* 2 halves x 3840 frames x 4 bytes */
#define DMA_BYTES       (CAP_OFF + CAP_MAXBYTES)

/* pending bits set by the hardware interrupt */
#define PEND_P0         1UL     /* playback half 0 is free */
#define PEND_P1         2UL     /* playback half 1 is free */
#define PEND_R0         4UL     /* capture half 0 is full */
#define PEND_R1         8UL     /* capture half 1 is full */

struct CADrv
{
    struct CAHw             hw;         /* must be first */
    struct AHIAudioCtrlDrv *ac;
    struct Interrupt        play_int;
    struct Interrupt        rec_int;
    volatile ULONG          pending;

    ULONG                   rate;
    UBYTE                   rate_code;
    UBYTE                   playing;
    UBYTE                   recording;
    UBYTE                   hw_s32;     /* card plays S32 (HiFi) */

    /* playback */
    ULONG                   half_frames;
    ULONG                   hw_fsize;   /* 4 (S16 stereo) or 8 (S32 stereo) */
    ULONG                   buftype;    /* AHIST_xxx of the mixing buffer */
    APTR                    mixtmp;     /* ahiac_BuffSize, mixer output */
    UBYTE                  *stage;      /* card-format FIFO in fast RAM */
    ULONG                   stage_bytes;
    ULONG                   stage_frames;   /* frames waiting in stage */

    /* recording */
    ULONG                   rec_half_frames;
    ULONG                  *recbuf[2];
    UBYTE                   recsel;
    struct AHIRecordMessage recmsg;

    /* mixer state (AHI Fixed values) */
    LONG                    monitor;
    LONG                    gain;
    LONG                    outvol;
    ULONG                   input;

    /* statistics */
    ULONG                   underruns;
    ULONG                   overloads;
};

/* lib.c */
void *memcpy(void *d, const void *s, unsigned long n);
void *memmove(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);

/* audio.c */
ULONG drv_AllocAudio(struct TagItem *tags, struct AHIAudioCtrlDrv *ac);
void  drv_FreeAudio(struct AHIAudioCtrlDrv *ac);
ULONG drv_Start(ULONG flags, struct AHIAudioCtrlDrv *ac);
void  drv_Update(ULONG flags, struct AHIAudioCtrlDrv *ac);
void  drv_Stop(ULONG flags, struct AHIAudioCtrlDrv *ac);
LONG  drv_GetAttr(ULONG attr, LONG arg, LONG def, struct TagItem *tags,
                  struct AHIAudioCtrlDrv *ac);
ULONG drv_HardwareControl(ULONG attr, LONG arg, struct AHIAudioCtrlDrv *ac);
void  drv_Cleanup(void);

extern struct ExecBase *SysBase;
extern struct Library  *UtilityBase;
extern const char       LibIdString[];

#endif
