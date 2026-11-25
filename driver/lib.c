/*
 * ca0106.audio - library skeleton (no startup code, no C library).
 *
 * GPL-2.0-or-later.
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <exec/initializers.h>
#include <dos/dos.h>
#include <proto/exec.h>

#include "driver.h"

/* Must be the first code in the file: running the library as a program
 * returns immediately. Built with -fno-toplevel-reorder. */
__asm__(
"	.text\n"
"	.globl	__start\n"
"__start:\n"
"	moveq	#-1,d0\n"
"	rts\n");

struct ExecBase *SysBase;
struct Library  *UtilityBase;

static struct CABase *CABase;

const char LibName[] = LIB_NAME;
const char LibIdString[] = LIB_IDSTRING;
static const char VersTag[] __attribute__((used)) =
    "$VER: " LIB_NAME " 4.2 (" LIB_DATE ") GPL-2.0";

/* ---------------------------------------------------------------------- */

static struct CABase *LibInit(struct CABase *base __asm("d0"),
                              BPTR seglist __asm("a0"),
                              struct ExecBase *sysbase __asm("a6"))
{
    SysBase = sysbase;
    CABase = base;
    base->seglist = seglist;
    base->lib.lib_Node.ln_Type = NT_LIBRARY;
    base->lib.lib_Node.ln_Name = (char *)LibName;
    base->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    base->lib.lib_Version = LIB_VERSION;
    base->lib.lib_Revision = LIB_REVISION;
    base->lib.lib_IdString = (APTR)LibIdString;

    UtilityBase = OpenLibrary("utility.library", 39);
    if (!UtilityBase)
    {
        FreeMem((UBYTE *)base - base->lib.lib_NegSize,
                base->lib.lib_NegSize + base->lib.lib_PosSize);
        return NULL;
    }
    return base;
}

static BPTR LibExpunge(struct CABase *base __asm("a6"))
{
    BPTR seglist;

    if (base->lib.lib_OpenCnt)
    {
        base->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    seglist = base->seglist;
    Remove(&base->lib.lib_Node);
    drv_Cleanup();
    if (UtilityBase)
    {
        CloseLibrary(UtilityBase);
        UtilityBase = NULL;
    }
    FreeMem((UBYTE *)base - base->lib.lib_NegSize,
            base->lib.lib_NegSize + base->lib.lib_PosSize);
    return seglist;
}

static struct CABase *LibOpen(struct CABase *base __asm("a6"))
{
    base->lib.lib_OpenCnt++;
    base->lib.lib_Flags &= ~LIBF_DELEXP;
    return base;
}

static BPTR LibClose(struct CABase *base __asm("a6"))
{
    if (--base->lib.lib_OpenCnt == 0 && (base->lib.lib_Flags & LIBF_DELEXP))
        return LibExpunge(base);
    return 0;
}

static ULONG LibNull(void)
{
    return 0;
}

/* ---------------------------------------------------------------------- */
/* AHI sub-driver entry points (register conventions from ahi_sub_lib.fd)  */
/* ---------------------------------------------------------------------- */

static ULONG sub_AllocAudio(struct TagItem *tags __asm("a1"),
                            struct AHIAudioCtrlDrv *ac __asm("a2"))
{
    return drv_AllocAudio(tags, ac);
}

static void sub_FreeAudio(struct AHIAudioCtrlDrv *ac __asm("a2"))
{
    drv_FreeAudio(ac);
}

static void sub_Disable(struct AHIAudioCtrlDrv *ac __asm("a2"))
{
    (void)ac;
    Disable();
}

static void sub_Enable(struct AHIAudioCtrlDrv *ac __asm("a2"))
{
    (void)ac;
    Enable();
}

static ULONG sub_Start(ULONG flags __asm("d0"), struct AHIAudioCtrlDrv *ac __asm("a2"))
{
    return drv_Start(flags, ac);
}

static void sub_Update(ULONG flags __asm("d0"), struct AHIAudioCtrlDrv *ac __asm("a2"))
{
    drv_Update(flags, ac);
}

static void sub_Stop(ULONG flags __asm("d0"), struct AHIAudioCtrlDrv *ac __asm("a2"))
{
    drv_Stop(flags, ac);
}

/* SetVol, SetFreq, SetSound, SetEffect, LoadSound, UnloadSound:
 * the AHI mixer does the work */
static ULONG sub_Unknown(void)
{
    return AHIS_UNKNOWN;
}

static LONG sub_GetAttr(ULONG attr __asm("d0"), LONG arg __asm("d1"), LONG def __asm("d2"),
                        struct TagItem *tags __asm("a1"),
                        struct AHIAudioCtrlDrv *ac __asm("a2"))
{
    return drv_GetAttr(attr, arg, def, tags, ac);
}

static ULONG sub_HardwareControl(ULONG attr __asm("d0"), LONG arg __asm("d1"),
                                 struct AHIAudioCtrlDrv *ac __asm("a2"))
{
    return drv_HardwareControl(attr, arg, ac);
}

/* ---------------------------------------------------------------------- */

static const APTR LibVectors[] =
{
    (APTR)LibOpen,
    (APTR)LibClose,
    (APTR)LibExpunge,
    (APTR)LibNull,
    (APTR)sub_AllocAudio,
    (APTR)sub_FreeAudio,
    (APTR)sub_Disable,
    (APTR)sub_Enable,
    (APTR)sub_Start,
    (APTR)sub_Update,
    (APTR)sub_Stop,
    (APTR)sub_Unknown,      /* SetVol */
    (APTR)sub_Unknown,      /* SetFreq */
    (APTR)sub_Unknown,      /* SetSound */
    (APTR)sub_Unknown,      /* SetEffect */
    (APTR)sub_Unknown,      /* LoadSound */
    (APTR)sub_Unknown,      /* UnloadSound */
    (APTR)sub_GetAttr,
    (APTR)sub_HardwareControl,
    (APTR)-1
};

static const ULONG LibInitTable[4] =
{
    sizeof(struct CABase),
    (ULONG)LibVectors,
    0,
    (ULONG)LibInit
};

const struct Resident RomTag __attribute__((used)) =
{
    RTC_MATCHWORD,
    (struct Resident *)&RomTag,
    (APTR)(&RomTag + 1),
    RTF_AUTOINIT,
    LIB_VERSION,
    NT_LIBRARY,
    0,
    (char *)LibName,
    (char *)LibIdString,
    (APTR)LibInitTable
};

/* ---------------------------------------------------------------------- */
/* Minimal libc replacements that GCC may emit calls to                    */
/* ---------------------------------------------------------------------- */

void *memcpy(void *d, const void *s, unsigned long n)
{
    CopyMem((APTR)s, d, n);
    return d;
}

void *memmove(void *d, const void *s, unsigned long n)
{
    UBYTE *dp = d;
    const UBYTE *sp = s;
    if (dp < sp)
    {
        /* CopyMem() does not define overlap behaviour */
        if (!(((ULONG)dp | (ULONG)sp | n) & 3))
        {
            ULONG *dl = (ULONG *)dp;
            const ULONG *sl = (const ULONG *)sp;
            n >>= 2;
            while (n--)
                *dl++ = *sl++;
        }
        else
            while (n--)
                *dp++ = *sp++;
    }
    else
        while (n--)
            dp[n] = sp[n];
    return d;
}

void *memset(void *d, int c, unsigned long n)
{
    UBYTE *p = d;
    while (n--)
        *p++ = (UBYTE)c;
    return d;
}
