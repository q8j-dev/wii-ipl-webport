#define BS2_MACH_VOLATILE_DVD_STATE
#define BS2_MACH_FIVE_ARG_READ
#include "BS2/BS2.h"
#include "BS2/BS2BringUp.h"
#include "BS2/BS2Update.h"
#include "config.h"

#include <private/dvd.h>
#include <private/nand.h>
#include <private/os.h>
#include <revolution/os/OSBootInfo.h>
#include <private/vi.h>
#include <private/hollywood/flipper.h>

#include <private/es.h>
#include <private/fs.h>
#include <private/ios.h>
#include <private/ipc.h>
#include <revolution/pad.h>
#include <revolution/sc.h>

#include <string.h>

BOOL BS2IsValidDisc(void);

typedef struct AppLoaderHeader {
    char date[16];   // 0x00
    u32 entryPoint;  // 0x10
    u32 length;      // 0x14
    u32 trailerSize; // 0x18
    u8 pad_0x1C[4];
} AppLoaderHeader;

static u8 TicketViewsBuf[OSRoundUp32B(sizeof(ESTicketView) * 64)] ALIGN32;
ESTicketView *TicketViews = (ESTicketView *)TicketViewsBuf;
DVDDiskID UpdateDiskID ALIGN32;
OSBootInfo2 bi2 ALIGN32;
AppLoaderHeader AppLoaderHdr ALIGN32;
OSBootInfo3 bi3 ALIGN32;
u8 GameTOCBuf[OSRoundUp32B(sizeof(DVDGameTOC))] ALIGN32;
DVDDriveInfo DriveInfo ALIGN32;
static DVDDiskID DiskID ALIGN32;
static DVDPartitionParams PartitionParams ALIGN32;
NANDCommandBlock BS2NandBlock;
NANDFileInfo BS2CacheFileInfo;
DVDCommandBlock CoverBlock;
u8 PartitionInfoBuf[OSRoundUp32B(sizeof(DVDPartitionInfo) * 256)] ALIGN32;
DVDCommandBlock Block;

static BS2State State = BS2_STT_BEGIN;
// MWCC must preserve the reads and writes of interrupt-shared flags below.
static vu32 DvdReadPending = 0;
static u32 BannerAllocation = 0;
static u32 BannerBuffer = 0;
static u32 BannerAvailable = 0;
static u32 Allocator = 0;
volatile BOOL StartingGame = FALSE;
static vu32 RestartRequested = 0;
static vu32 PartitionOpen = 0;
static u32 CacheSeekComplete = 0;
static vu32 LoadingTitle = 0;
volatile BOOL FatalErrorFlag = FALSE;
BOOL RetryErrorFlag = FALSE;
BOOL UpdateErrorFlag = FALSE;
volatile BOOL AbortFlag = FALSE;
volatile int CacheFailed = 0;
volatile int RegionValid = 0;
static volatile int NandPending = 0;
static vu32 CancelNand = 0;
static vu32 LowReadResult = 0;
static u32 LoaderInit;
static u32 LoaderMain;
static u32 LoaderClose;
static u32 *DvdProgress;
static vu32 DvdTransferred;
static vu32 DvdTransferLength;
static vu32 BannerLength;
vu32 CacheLength;
u32 LoaderAddress;
vu32 LoaderLength;
u32 LoaderOffset;
volatile NANDCallback NandCompletion;
vu32 NandOperation;
NANDFileInfo *NandFile;
u8 *NandBuffer;
vu32 NandLength;
vu32 NandTransferred;
OSTime CoverOpenTime;
u64 GameToc;
u32 DataToc;
u32 PartitionCursor;
u32 UpdatePartition;
u32 GamePartition;
u64 RequiredIos;
u32 TitleCode;
static u32 CurrentTmd;
static u32 TitleTicketView;
u32 DriveWasReset;
u64 CoverPollTime;
static u64 SpinupDeadline;
static u64 ResetTime;


void BS2Report(const char *msg, ...) {
#ifdef ENABLE_BS2_REPORT
    va_list marker;
    va_start(marker, msg);
    OSVReport(msg, marker);
    va_end(marker);
#endif
}

void BS2NANDCallback(s32 result, NANDCommandBlock *block) {
    if (NandPending) {
        NandPending = 0;
    }

    if (result < NAND_RESULT_OK) {
        OSReport("Failed to access boot cache file\n");
        BS2BootFromCache = FALSE;
        BS2BootCaching = FALSE;
        CacheFailed = 1;
        State = BS2_STT_2;
    }
}

void BS2DVDCallback(s32 result, DVDCommandBlock *block) {
    if (DvdReadPending) {
        DvdReadPending = 0;
        DvdTransferred += DvdTransferLength;
        if (result < DVD_RESULT_OK) {
            *DvdProgress = 0;
        }
    }

    if (State == BS2_STT_3 && result == 1) {
        State = BS2_STT_NO_DISK;
    }

    if ((State == BS2_STT_RVL_GAME || State == BS2_STT_GC_GAME || State == BS2_STT_UPDATE_DISK || State == BS2_STT_DATA_DISK) &&
        result == 1) {
        BS2CancelUpdate();
        State = BS2_STT_NO_DISK;
    }

    if (State == BS2_STT_DIRTY_DISK || State == BS2_STT_63) {
        if (result == 1) {
            BS2CancelUpdate();
            State = BS2_STT_NO_DISK;
        } else {
            if (DVDLowGetCoverRegister() >> 2 & 1) {
                BS2CancelUpdate();
                if (AbortFlag == FALSE) {
                    State = BS2_STT_COVER_CLOSED;
                }
            }
        }
    }

    if ((State == BS2_STT_NO_DISK || State == BS2_STT_COVER_OPEN) && result == 2) {
        DVDLowMaskCoverInterrupt();
        State = BS2_STT_COVER_CLOSED;
    }
    if (State == BS2_STT_UPDATE_DISK && BS2UpdateState() == 2) {
        State = BS2_STT_RUNNING_UPDATE;
    }
}

void BS2RestartStateMachine() {
    BOOL old;
    u32 rtcFlags;

    __OSGetRTCFlags(&rtcFlags);
    __OSClearRTCFlags();

    old = OSDisableInterrupts();

    BS2Report("[BS2RestartStateMachine]\n");

    if ((rtcFlags & 1) || (rtcFlags & 2)) {
        BS2BootFromCache = FALSE;
        BS2BootCaching = TRUE;
    }

    RestartRequested = 1;

    OSRestoreInterrupts(old);
}

void BS2AbortStateMachine() {
    BOOL enabled = OSDisableInterrupts();

    BS2Report("[BS2AbortStateMachine]\n");

    RestartRequested = 0;

    if (State == BS2_STT_64) {
        OSRestoreInterrupts(enabled);
    } else if (State == BS2_STT_BEGIN || State == BS2_STT_1 || State == BS2_STT_2 || State == BS2_STT_4 || State == BS2_STT_6 ||
               State == BS2_STT_8 || State == BS2_STT_10) {
        State = BS2_STT_64;
        OSRestoreInterrupts(enabled);
    } else if (State == BS2_STT_NO_DISK || State == BS2_STT_COVER_OPEN || State == BS2_STT_55 || State == BS2_STT_WRONG_DISK ||
               State == BS2_STT_66 || State == BS2_STT_67 || State == BS2_STT_68 || State == BS2_STT_FATAL_ERROR ||
               State == BS2_STT_UPDATE_FAILED || State == BS2_STT_DIRTY_DISK) {
        OSRestoreInterrupts(enabled);
    } else {
        *DvdProgress = 0;
        DvdTransferred = 0;
        DvdTransferLength = 0;
        if (State == BS2_STT_3 || State == BS2_STT_5 || State == BS2_STT_7 || State == BS2_STT_9) {
            State = BS2_STT_62;
        } else {
            if (NandPending != 0) {
                CancelNand = 1;
            }
            State = BS2_STT_60;
            DVDCancelAsync(&Block, NULL);
        }
        AbortFlag = 1;
        OSRestoreInterrupts(enabled);
    }
}

void BS2SetBannerBuffer(void *pBanner, u32 bannerSize) {
    BannerBuffer = (u32)pBanner;
    BannerLength = bannerSize;
}

void BS2SetMemAllocator(MEMAllocator *allocator) { Allocator = (u32)allocator; }

BOOL BS2IsBannerAvailable() { return BannerAvailable; }

void *BS2GetBannerBufferAddr() { return (void *)BannerBuffer; }

u32 BS2GetBannerBufferLength() { return BannerLength; }

BOOL BS2IsDiagDisc() { return (u8)(*(u8 *)OSPhysicalToCached(OS_ADDR_BOOT_INFO) - 0x30U) <= 1; }

extern vu32 BS2VideoMode;
extern vu32 __DVDLayoutFormat;

static asm void Run(u32 entryPoint, void *start, u32 blockCount, u32 argument) {
    // clang-format off
#ifdef __MWERKS__
    nofralloc

    mtctr   r5
    mtlr    r3

    li      r0, 0
    li      r2, 0
    li      r3, 0
    li      r5, 0
    li      r7, 0
    li      r8, 0
    li      r9, 0
    li      r10, 0
    li      r11, 0
    li      r12, 0
    li      r13, 0
    li      r14, 0
    li      r15, 0
    li      r16, 0
    li      r17, 0
    li      r18, 0
    li      r19, 0
    li      r20, 0
    li      r21, 0
    li      r22, 0
    li      r23, 0
    li      r24, 0
    li      r25, 0
    li      r26, 0
    li      r27, 0
    li      r28, 0
    li      r29, 0
    li      r30, 0
    li      r31, 0

    // The application stack is fixed; it is unrelated to the formatter.
    lis     r1, 0x8160
    ori     r1, r1, 0
    li      r6, 0

    b       Run_loop_entry
Run_loop:
    dcbz    r4, r0
    dcbf    r4, r0
    addi    r4, r4, 0x20
    bdnz    Run_loop
    b       Run_done
Run_done:
    li      r4, 0
    blr
Run_loop_entry:
    b       Run_loop
#endif
    // clang-format on
}

BOOL BS2GetLockedTitles(ESTitleId *pTitleIds, u32 *count) {
    u32 i;
    u32 titleMask;
    u32 titleCount;

    if (State != BS2_STT_DATA_DISK && State != BS2_STT_RVL_GAME) {
        return FALSE;
    }

    if (!pTitleIds) {
        *count = 0;
        PartitionCursor = (u32)PartitionInfoBuf;
        for (i = 0; i < **(u32 **)&GameToc; i++) {
            BS2Report("gamePartition ... 0x%08X\n", (u32)((DVDPartitionInfo *)PartitionCursor)->partition);
            BS2Report("type          ... 0x%08X\n", ((DVDPartitionInfo *)PartitionCursor)->partitionType);
            if (((DVDPartitionInfo *)PartitionCursor)->partitionType & 0xFF000000) {
                BS2Report(" count++\n");
                (*count)++;
            }
            PartitionCursor += 8;
        }

        PartitionCursor = (u32)PartitionInfoBuf + 0x20;
        for (i = 0; i < **(u32 **)&DataToc; i++) {
            BS2Report("gamePartition ... 0x%08X\n", (u32)((DVDPartitionInfo *)PartitionCursor)->partition);
            BS2Report("type          ... 0x%08X\n", ((DVDPartitionInfo *)PartitionCursor)->partitionType);
            if (((DVDPartitionInfo *)PartitionCursor)->partitionType & 0xFF000000) {
                BS2Report(" count++\n");
                (*count)++;
            }
            PartitionCursor += 8;
        }

        return TRUE;
    }

    titleCount = *count;
    if (titleCount != 0) {
        PartitionCursor = (u32)PartitionInfoBuf;
        titleMask = (u32)-1;
        for (i = 0; i < **(u32 **)&GameToc; i++) {
            if (titleCount == 0) {
                return TRUE;
            }
            if (((DVDPartitionInfo *)PartitionCursor)->partitionType & 0xFF000000) {
                *pTitleIds = ((ESTitleId)0x00010000 << 32) | (((DVDPartitionInfo *)PartitionCursor)->partitionType & titleMask);
                titleCount--;
                pTitleIds++;
            }
            PartitionCursor += 8;
        }

        PartitionCursor = (u32)PartitionInfoBuf + 0x20;
        titleMask = (u32)-1;
        for (i = 0; i < **(u32 **)&DataToc; i++) {
            if (titleCount == 0) {
                return TRUE;
            }
            if (((DVDPartitionInfo *)PartitionCursor)->partitionType & 0xFF000000) {
                *pTitleIds = ((ESTitleId)0x00010000 << 32) | (((DVDPartitionInfo *)PartitionCursor)->partitionType & titleMask);
                titleCount--;
                pTitleIds++;
            }
            PartitionCursor += 8;
        }
    }

    return TRUE;
}

BOOL BS2IsTitleAvailable(ESTitleId titleId) {
    u32 *count;
    u32 i;

    if (State != BS2_STT_DATA_DISK && State != BS2_STT_RVL_GAME) {
        return FALSE;
    }

    PartitionCursor = (u32)PartitionInfoBuf;
    count = *(u32 **)&GameToc;
    for (i = 0; i < *count; i++) {
        if (((DVDPartitionInfo *)PartitionCursor)->partitionType == (u32)titleId) {
            return TRUE;
        }
        PartitionCursor += 8;
    }

    PartitionCursor = (u32)PartitionInfoBuf + 0x20;
    count = *(u32 **)&DataToc;
    for (i = 0; i < *count; i++) {
        if (((DVDPartitionInfo *)PartitionCursor)->partitionType == (u32)titleId) {
            return TRUE;
        }
        PartitionCursor += 8;
    }

    return FALSE;
}

s32 BS2GetTicketFromNand(ESTitleId titleId, ESTicketView *pTicketView) {
    s32 ret;
    u32 ticketCount;
    s32 index;

    ret = ES_GetTicketViews(titleId, NULL, &ticketCount);
    if (ret != 0) {
        OSReport("ES_GetTicketViews%d failed: %d\n", 1, ret);
        return ret;
    }
    if (ticketCount == 0) {
        OSReport("No ticket for disc.  Please import a ticket.\n");
        return -1;
    }
    if (ticketCount > 0x40) {
        OSReport("Internal error: Too many tickets\n");
        return -1;
    }

    ret = ES_GetTicketViews(titleId, TicketViews, &ticketCount);
    if (ret != 0) {
        OSReport("ES_GetTicketViews%d failed: %d\n", 2, ret);
        return ret;
    }

    BS2Report("Found %d tickets in NAND\n", ticketCount);
    index = __OSGetValidTicketIndex(TicketViews, ticketCount);
    if (index < 0 || (u32)index > ticketCount - 1) {
        OSReport("Failed to get best ticket.\n");
        return -1;
    }

    memcpy(pTicketView, &TicketViews[index], sizeof(ESTicketView));
    DCStoreRange(pTicketView, sizeof(ESTicketView));
    return ticketCount;
}

BOOL BS2StartLoadingTitle(ESTitleId titleId, ESTicketView *pTicketView) {
    u32 *gamePartitionCount;
    u32 *dataPartitionCount;
    u32 i;

    if (State != BS2_STT_DATA_DISK && State != BS2_STT_RVL_GAME) {
        return FALSE;
    }

    LoadingTitle = 1;
    TitleTicketView = (u32)pTicketView;
    PartitionCursor = (u32)PartitionInfoBuf;
    gamePartitionCount = *(u32 **)&GameToc;
    for (i = 0; i < *gamePartitionCount; i++) {
        if (((DVDPartitionInfo *)PartitionCursor)->partitionType == (u32)titleId) {
            GamePartition = PartitionCursor;
            if (BS2BootFromCache) {
                State = BS2_STT_8;
            } else {
                State = BS2_STT_LOCKED_DISK;
            }
            BS2BootFromCache = FALSE;
            BS2BootCaching = FALSE;
            return TRUE;
        }
        PartitionCursor += 8;
    }

    PartitionCursor = (u32)PartitionInfoBuf + 0x20;
    dataPartitionCount = *(u32 **)&DataToc;
    for (i = 0; i < *dataPartitionCount; i++) {
        if (((DVDPartitionInfo *)PartitionCursor)->partitionType == (u32)titleId) {
            GamePartition = PartitionCursor;
            if (BS2BootFromCache) {
                State = BS2_STT_8;
            } else {
                State = BS2_STT_LOCKED_DISK;
            }
            BS2BootFromCache = FALSE;
            BS2BootCaching = FALSE;
            return TRUE;
        }
        PartitionCursor += 8;
    }

    return FALSE;
}

static void callback(u32 result) { LowReadResult = result; }

static ESError BS2ESGetTicketViews(IOSFd *descriptor, ESTitleId titleId, ESTicketView *views, u32 *count) {
    u8 work[256] ALIGN32;
    IOSIoVector vectors[4] ALIGN32;
    ESError result;
    IOSIoVector *request = vectors;
    ESTitleId *title = (ESTitleId *)work;
    u32 *ticketCount = (u32 *)(work + 0x20);

    if (*descriptor < 0 || !count) {
        return ES_ERR_INVALID;
    }
    if ((u32)views % 32 != 0) {
        return ES_ERR_INVALID;
    }
    *title = titleId;
    if (!views) {
        request[0].base = (u8 *)title;
        request[0].length = sizeof(*title);
        request[1].base = (u8 *)ticketCount;
        request[1].length = sizeof(*ticketCount);
        result = IOS_Ioctlv(*descriptor, 0x12, 1, 1, request);
        if (result == 0) {
            *count = *ticketCount;
        }
        return result;
    }
    if (*count == 0) {
        return ES_ERR_INVALID;
    }
    *ticketCount = *count;
    request[0].base = (u8 *)title;
    request[0].length = sizeof(*title);
    request[1].base = (u8 *)ticketCount;
    request[1].length = sizeof(*ticketCount);
    request[2].base = (u8 *)views;
    request[2].length = *count * sizeof(*views);
    return IOS_Ioctlv(*descriptor, 0x13, 2, 1, request);
}
static void BS2Reboot(void) {
    ESTicketView ticket ALIGN32;
    u8 work[256] ALIGN32;
    OSStateFlags flags ALIGN32;
    IOSIoVector vectors[4] ALIGN32;
    IOSFd descriptor;
    u32 ticketCount;
    s32 initErr;
    s32 err;

    descriptor = -1;
    initErr = ISFS_OpenLibEx();
    if (initErr != ISFS_ERROR_OK) {
        OSReport("ISFS_OpenLibEx failed: %d\n", initErr);
    } else {
        BS2Report("\nISFS_OpenLibEx successful\n");
        __OSReadStateFlags(&flags);
        flags.lastAppType = 0;
        flags.shutdownType = OS_STATE_FLAGS_SHUTDOWN_RETURN_MENU;
        flags.discState = OS_STATE_FLAGS_DISC_CHANGED;
        __OSWriteStateFlags(&flags);

        err = 0;
        descriptor = IOS_Open("/dev/es", 0);
        if (descriptor < 0) {
            err = descriptor;
        }
        if (err != 0) {
            OSReport("ES_InitLib failed: %d\n", err);
        } else {
            BS2Report("ES_InitLib: %d\n", err);
            {
                s32 result = BS2ESGetTicketViews(&descriptor, SYSMENU_TITLE_ID, NULL, &ticketCount);
                if (result != 0) {
                    OSReport("ES_GetTicketViews failed: %d\n", result);
                } else {
                    BS2Report("ES_GetTicketViews: %d\n", result);
                    if (ticketCount != 1) {
                        OSReport("Error: Should only have 1 ticket for System Menu\n");
                    }
                    {
                        s32 result = BS2ESGetTicketViews(&descriptor, SYSMENU_TITLE_ID, &ticket, &ticketCount);
                        if (result != 0) {
                            OSReport("ES_GetTicketViews failed: %d\n", result);
                        } else {
                            BS2Report("ES_GetTicketViews: %d\n", result);
                            {
                                s32 launchResult;
                                IOSIoVector *request = vectors;
                                ESTitleId *title = (ESTitleId *)work;
                                if (descriptor < 0) {
                                    launchResult = -0x3f9;
                                } else if ((u32)&ticket % 32 != 0) {
                                    launchResult = -0x3f9;
                                } else {
                                    *title = SYSMENU_TITLE_ID;
                                    request[0].base = (u8 *)title;
                                    request[0].length = sizeof(*title);
                                    request[1].base = (u8 *)&ticket;
                                    request[1].length = sizeof(ESTicketView);
                                    launchResult = IOS_IoctlvReboot(descriptor, 8, 2, 0, request);
                                }
                                if (launchResult != 0) {
                                    OSReport("ES_LaunchTitle failed: %d\n", launchResult);
                                } else {
                                    BS2Report("ES_LaunchTitle: %d\n", launchResult);
                                    while (TRUE) {
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

static inline s32 BS2ReadDVDState(const volatile DVDCommandBlock *block) {
    return block->state;
}

void BS2StartGame() {
    u32 runResult;
    u8 *base;
    s32 ret;
    u32 oldBytes;
    u32 driveError;
    s32 status;
    ESTicketView *ticketViews;
    u32 titleType;
    u32 titleCode;
    u32 ticketCount;
    BOOL oldInterrupts;
    OSStateFlags stateFlags;
    u32 (*entry)(void);

    StartingGame = TRUE;
    while (BS2ReadDVDState(&CoverBlock) != DVD_STATE_IDLE) {
#ifdef RVL_WEB
        __web_poll(0);
#endif
    }

    BS2Report("BS2StartGame(1)\n");
    BS2Report("DVD transferred bytes:%d\n", DvdTransferred);

    NandPending = 1;
    {
        s32 closeResult;

        closeResult = NANDCloseAsync(&BS2CacheFileInfo, BS2NANDCallback, &BS2NandBlock);
        if (closeResult == NAND_RESULT_OK) {
            while (NandPending != 0) {
#ifdef RVL_WEB
        __web_poll(0);
#endif
            }
            if (CacheFailed != 0) {
                NANDDelete("/title/00000001/00000002/data/cache.dat");
            }
        } else {
            OSReport("Error occurred when close cache.dat (%d)\n", closeResult);
        }
    }

    while (!PADSync()) {
#ifdef RVL_WEB
        __web_poll(0);
#endif
    }

    base = (u8 *)0x80000000;
    *(u32 *)0x80003180 = *(u32 *)base;
    *(u8 *)0x80003184 = 0x80;
    *(u8 *)0x80003187 = PartitionParams.numTmdBytes ? 0x80 : 0;
    *(u32 *)0x80003194 = ((DVDPartitionInfo *)GamePartition)->partitionType;
    *(u32 *)0x80003198 = (u32)((DVDPartitionInfo *)GamePartition)->partition;
    DCFlushRange((void *)0x80003100, 0x100);

    __OSUnRegisterStateEvent();
    __OSReadStateFlags(&stateFlags);
    stateFlags.lastAppType = 0x81;
    if (CacheFailed == 0) {
        stateFlags.lastAppType |= 0x40;
    }
    stateFlags.shutdownType = OS_STATE_FLAGS_SHUTDOWN_BAD;
    stateFlags.discState = OS_STATE_FLAGS_DISC_IN;
    __OSWriteStateFlags(&stateFlags);
    __OSClearRTCFlags();

    titleType = (u32)(RequiredIos >> 32);
    titleCode = (u32)RequiredIos;
    ticketCount = 1;
    ticketViews = TicketViews;
    if ((titleCode | titleType) == 0) {
        titleCode = 3;
        titleType = 1;
    }

    ret = ES_InitLib();
    if (ret != ES_ERR_OK) {
        OSReport("\nsecurity error(%d) has occurred", ret);
        OSPanic("BS2Mach.c", 0xAC3, "\nFailed to boot app");
    }
    BS2Report("  sysVersion = %016llx\n", ((ESTitleId)titleType << 32) | titleCode);
    ret = ES_GetTicketViews(((ESTitleId)titleType << 32) | titleCode, NULL, &ticketCount);
    if (ticketCount != 1 || ret != ES_ERR_OK) {
        OSReport("\nsecurity error(%d) has occurred", ret);
        OSPanic("BS2Mach.c", 0xACD, "\nFailed to boot app");
    }
    ret = ES_GetTicketViews(((ESTitleId)titleType << 32) | titleCode, ticketViews, &ticketCount);
    BS2Report("GetTicketViews: rc = %d\n", ret);
    if (ret != ES_ERR_OK) {
        OSReport("\nsecurity error(%d) has occurred", ret);
        OSPanic("BS2Mach.c", 0xAD7, "\nFailed to boot app");
    }

    DVDLowFinalize();
    oldBytes = *(u32 *)0x8000311C;
    DCStoreRange((void *)0x80003100, 0x100);
    ret = ES_LaunchTitle(((ESTitleId)titleType << 32) | titleCode, ticketViews);
    BS2Report("LaunchTitle: rc = %d\n", ret);
    if (ret != ES_ERR_OK) {
        OSReport("\nsecurity error(%d) has occurred", ret);
        OSPanic("BS2Mach.c", 0xAF3, "\nFailed to boot app");
    }

    DCInvalidateRange((void *)0x80003100, 0x100);
    __OSInitIPCBuffer();
    IPCReInit();
    IPCCltReInit();
    BS2Report("IPC driver is re-initialized\n");
    if (oldBytes < *(u32 *)0x8000311C) {
        *(u32 *)0x80003120 = oldBytes - (*(u32 *)0x8000311C - *(u32 *)0x80003120);
        *(u32 *)0x80003128 = oldBytes - (*(u32 *)0x8000311C - *(u32 *)0x80003128);
        *(u32 *)0x80003130 = oldBytes - (*(u32 *)0x8000311C - *(u32 *)0x80003130);
        *(u32 *)0x80003134 = oldBytes - (*(u32 *)0x8000311C - *(u32 *)0x80003134);
        *(u32 *)0x8000311C = oldBytes;
    }

    DVDLowInit();
    BS2Report("DVDLowInit done\n");

    LowReadResult = 0;
    DVDLowReadDiskID(&DiskID, callback);
    while (LowReadResult == 0) {
#ifdef RVL_WEB
        __web_poll(0);
#endif
    }

    status = LowReadResult;
    // MWCC needs the shared fatal and completion exits below.
    if (status != 2) {
        if ((status >= 2) || (status < 1)) {
            goto disk_fatal;
        } else {
            goto disk_done;
        }
    } else {
        OSReport("\nDisk error(%d) has occurred", LowReadResult);
        LowReadResult = 0;
        DVDLowRequestError(callback);
        while (LowReadResult == 0) {
#ifdef RVL_WEB
        __web_poll(0);
#endif
        }
        driveError = DVDLowGetImmBufferReg() & 0xFF000000;
        if (driveError == 0x01000000 || (driveError = DVDLowGetImmBufferReg() & 0xFF000000) == 0x03000000) {
            BS2Reboot();
        }
        if (DvdTransferred == 0 && (driveError = DVDLowGetImmBufferReg() & 0xFF000000) == 0x04000000) {
            BS2Reboot();
        }
        goto disk_fatal;
    }

disk_fatal:
    DVDSetAutoFatalMessaging(TRUE);
    __DVDPrintFatalMessage();

disk_done:
    BS2Report("DVDLowReadDiskID done\n");
    ret = strncmp((char *)base, (char *)&DiskID, 4);
    if (ret != 0) {
        OSReport("\nDisk has changed!");
        BS2Reboot();
    }

    BS2Report("PartitionParams.numTmdBytes = %d\n", PartitionParams.numTmdBytes);
    LowReadResult = 0;
    if (PartitionParams.numTmdBytes != 0) {
        DVDLowOpenPartitionWithTmdAndTicketView((u32)((DVDPartitionInfo *)GamePartition)->partition, &PartitionParams.ticketView,
                                                PartitionParams.numTmdBytes, &PartitionParams.tmd, PartitionParams.numCertBytes,
                                                PartitionParams.certificates, callback);
    } else {
        DVDLowOpenPartition((u32)((DVDPartitionInfo *)GamePartition)->partition, NULL, 0, NULL, &PartitionParams.tmd,
                            callback);
    }
    while (LowReadResult == 0) {
#ifdef RVL_WEB
        __web_poll(0);
#endif
    }

    status = LowReadResult;
    if (status != 2) {
        if (status < 2) {
            if (status >= 1) {
                goto partition_done;
            }
        } else if (status == 0x40) {
            goto partition_eticket;
        }
        goto partition_fatal;
    } else {
        OSReport("\nDisk error(%d) has occurred", LowReadResult);
        LowReadResult = 0;
        DVDLowRequestError(callback);
        while (LowReadResult == 0) {
#ifdef RVL_WEB
        __web_poll(0);
#endif
        }
        driveError = DVDLowGetImmBufferReg() & 0xFF000000;
        if (driveError == 0x01000000 || (driveError = DVDLowGetImmBufferReg() & 0xFF000000) == 0x03000000) {
            BS2Reboot();
        }
        goto partition_fatal;
    }

partition_eticket:
    OSReport("ETicket Error: %d\n", DVDLowGetLastEticketError());

partition_fatal:
    DVDSetAutoFatalMessaging(TRUE);
    __DVDPrintFatalMessage();

partition_done:
    BS2Report("DVDLowOpenPartition done\n");
    if (*(u8 *)0x8000315D != 0x80) {
        typedef union {
            u32 value;
            struct {
                u32 reserved : 29;
                u32 bit2 : 1;
                u32 bit1 : 1;
                u32 bit0 : 1;
            } bits;
        } DIControl;
        DIControl diBits;
        DIControl diValue;
        u32 *diAddr;

        diAddr = (u32 *)0xCC003024;
        diValue.value = *diAddr;
        diValue.bits.bit0 = 1;
        diBits.value = 2;
        diBits.bits.bit2 = 1;
        *diAddr = diValue.value | diBits.value;
        enableLegacyDI();
    }
    oldInterrupts = OSDisableInterrupts();
    __OSStopAudioSystem();
    entry = (u32 (*)(void))LoaderClose;
    runResult = entry();
    __OSFPRInit();
    ICFlashInvalidate();
    __sync();
    __isync();
    Run(runResult, (void *)0x81330000, 0x20000, 0);
    (void)oldInterrupts;
}

void BS2StartGCGame() {
    static u8 gameCubeLanguages[] = {0, 0, 1, 2, 3, 4, 5};
    s32 ret;
    u32 rtc;
    u32 soundMode;
    s8 productVideoMode;
    u8 progressiveMode;
    u8 language;
    u8 euRgb60Mode;
    u32 ticketCount;
    ESTicketView *ticketViews;
    OSSram *sram;
    OSTime time;

    StartingGame = TRUE;
    while (BS2ReadDVDState(&CoverBlock) != DVD_STATE_IDLE) {
#ifdef RVL_WEB
        __web_poll(0);
#endif
    }

    soundMode = SCGetSoundMode();
    if (soundMode == 2) {
        soundMode = 1;
    } else {
        soundMode = SCGetSoundMode();
    }
    OSSetSoundMode(soundMode);

    productVideoMode = SCGetProductVideoMode();
    if (productVideoMode != SC_PRODUCT_VIDEO_PAL) {
        progressiveMode = SCGetProgressiveMode();
        OSSetProgressiveMode(progressiveMode == 1);
        OSSetLanguage(0);
        OSSetEuRgb60Mode(0);
    } else {
        language = SCGetLanguage();
        if (language > 6) {
            OSSetLanguage(0);
        } else {
            language = SCGetLanguage();
            OSSetLanguage(gameCubeLanguages[language]);
        }
        euRgb60Mode = SCGetEuRgb60Mode();
        OSSetEuRgb60Mode(euRgb60Mode == 1);
    }

    __OSGetRTC(&rtc);
    time = OS_TIMER_CLOCK * (OSTime)(rtc + SCGetCounterBias());
    __OSSetTime(time);

    sram = __OSLockSram();
    sram->displayOffsetH = SCGetDisplayOffsetH();
    sram->counterBias = SCGetCounterBias();
    sram->flags |= 0x20;
    __OSUnlockSram(TRUE);
    while (!__OSSyncSram()) {
    }

    NandPending = 1;
    ret = NANDCloseAsync(&BS2CacheFileInfo, BS2NANDCallback, &BS2NandBlock);
    if (ret == NAND_RESULT_OK) {
        while (NandPending != 0) {
#ifdef RVL_WEB
        __web_poll(0);
#endif
        }
        if (CacheFailed != 0) {
            NANDDelete("/title/00000001/00000002/data/cache.dat");
        }
    } else {
        OSReport("Error occurred when close cache.dat (%d)\n", ret);
    }

    while (!PADSync()) {
#ifdef RVL_WEB
        __web_poll(0);
#endif
    }

    OSDisableInterrupts();
    __OSStopAudioSystem();
    OSEnableInterrupts();
    OSSetVideoMode(BS2VideoMode);
    while (!__OSSyncSram()) {
    }

    __OSUnRegisterStateEvent();
    {
        OSStateFlags stateFlags;
        __OSReadStateFlags(&stateFlags);
        stateFlags.lastAppType = 0x82;
        stateFlags.shutdownType = OS_STATE_FLAGS_SHUTDOWN_BAD;
        stateFlags.discState = OS_STATE_FLAGS_DISC_CHANGED;
        __OSWriteStateFlags(&stateFlags);
    }

    __OSClearRTCFlags();
    PI_SET_REG_F(0x24, 7);
    (void)PI_READ_REG(0x24);
    ticketViews = TicketViews;
    ticketCount = 1;
    BS2Report("  sysVersion = %016llx\n", 0x0000000100000100ULL);
    ret = ES_InitLib();
    if (ret != ES_ERR_OK) {
        OSReport("\nsecurity error(%d) has occurred", ret);
        OSPanic("BS2Mach.c", 0xC40, "\nFailed to boot app");
    }
    BS2Report("  sysVersion = %016llx\n", 0x0000000100000100ULL);
    ret = ES_GetTicketViews(0x0000000100000100ULL, NULL, &ticketCount);
    if (ticketCount != 1 || ret != ES_ERR_OK) {
        OSReport("\nsecurity error(%d) has occurred", ret);
        OSPanic("BS2Mach.c", 0xC4A, "\nFailed to boot app");
    }
    ret = ES_GetTicketViews(0x0000000100000100ULL, ticketViews, &ticketCount);
    BS2Report("GetTicketViews: rc = %d\n", ret);
    if (ret != ES_ERR_OK) {
        OSReport("\nsecurity error(%d) has occurred", ret);
        OSPanic("BS2Mach.c", 0xC54, "\nFailed to boot app");
    }
    ret = ES_LaunchTitle(0x0000000100000100ULL, ticketViews);
    BS2Report("LaunchTitle: rc = %d\n", ret);
    if (ret != ES_ERR_OK) {
        OSReport("\nsecurity error(%d) has occurred", ret);
        OSPanic("BS2Mach.c", 0xC5D, "\nFailed to boot app");
    }
}

static BOOL CheckDVDCommandStatus(DVDCommandBlock *block) {
    if (State == 0x3A) {
        return TRUE;
    }

    if (State == 0x43) {
        u32 coverRegister = DVDLowGetCoverRegister();
        u32 nextState = BS2_STT_68;

        if ((coverRegister & 1) != 0) {
            nextState = BS2_STT_NO_DISK;
        }
        State = nextState;
    }

    switch (block->state) {
    case 0:
        return TRUE;
    case 3:
        State = BS2_STT_COVER_CLOSED;
        break;
    case 4:
    case 5:
        if (State == 7 || State == 9 || State == 10) {
            CoverOpenTime = __OSGetSystemTime();
            State = BS2_STT_66;
        } else {
            State = BS2_STT_NO_DISK;
        }
        break;
    case -1:
        State = BS2_STT_FATAL_ERROR;
        FatalErrorFlag = TRUE;
        break;
    case 11:
        State = 0x3B;
        break;
    case 10:
        if (DVDResetRequired()) {
            State = BS2_STT_2;
        } else if (PartitionOpen != 0) {
            State = 0x3D;
        } else {
            State = 0x3F;
        }
        break;
    case 6:
    case 7:
        State = BS2_STT_WRONG_DISK;
        break;
    }

    if (AbortFlag != 0 && (State == 0x34 || State == 0x35 || State == 0x37 || State == 0x38 || State == 0x44 || (u32)(State - 0x39) <= 2)) {
        BS2BootFromCache = FALSE;
        BS2BootCaching = TRUE;
        State = BS2_STT_64;
    }

    return FALSE;
}

static void BS2NANDDivideCallback(s32 result, NANDCommandBlock *block);
static void BS2NANDDivideReadAsync(NANDFileInfo *info, void *buffer, u32 length, NANDCallback callback, NANDCommandBlock *block);
static void BS2NANDDivideWriteAsync(NANDFileInfo *info, const void *buffer, u32 length, NANDCallback callback, NANDCommandBlock *block);

static void BS2NANDDivideCallback(s32 result, NANDCommandBlock *block) {
    s32 ret;

    if (CancelNand != 0) {
        CancelNand = 0;
        NandCompletion(0, block);
    } else if (result < 0) {
        NandCompletion(result, block);
    } else {
        NandBuffer += result;
        NandTransferred += result;
        if (NandLength - NandTransferred > 0x40000) {
            if (NandOperation == 1) {
                BS2Report("NANDWriteAsync buf:0x%08X, length:0x%08X\n", NandBuffer, 0x40000);
                ret = NANDWriteAsync(NandFile, (void *)NandBuffer, 0x40000, BS2NANDDivideCallback, block);
            } else {
                BS2Report("NANDReadAsync buf:0x%08X, length:0x%08X\n", NandBuffer, 0x40000);
                ret = NANDReadAsync(NandFile, (void *)NandBuffer, 0x40000, BS2NANDDivideCallback, block);
            }
            if (ret < 0) {
                NandCompletion(ret, block);
            }
        } else {
            if (NandLength - NandTransferred != 0) {
                if (NandOperation == 1) {
                    BS2Report("NANDWriteAsync buf:0x%08X, length:0x%08X\n", NandBuffer, NandLength - NandTransferred);
                    ret = NANDWriteAsync(NandFile, (void *)NandBuffer, NandLength - NandTransferred, BS2NANDDivideCallback, block);
                } else {
                    BS2Report("NANDReadAsync buf:0x%08X, length:0x%08X\n", NandBuffer, NandLength - NandTransferred);
                    ret = NANDReadAsync(NandFile, (void *)NandBuffer, NandLength - NandTransferred, BS2NANDDivideCallback, block);
                }
                if (ret < 0) {
                    NandCompletion(ret, block);
                }
            } else {
                NandCompletion(NandLength, block);
            }
        }
    }
}

static void BS2NANDDivideReadAsync(NANDFileInfo *info, void *buffer, u32 length, NANDCallback callback, NANDCommandBlock *block) {
    NandCompletion = callback;
    NandOperation = 2;
    NandLength = length;
    NandTransferred = 0;
    NandFile = info;
    NandBuffer = (u8 *)buffer;
    if (NandLength > 0x40000) {
        BS2Report("NANDReadAsync buf:0x%08X, length:0x%08X\n", NandBuffer, 0x40000);
        NANDReadAsync(NandFile, (void *)NandBuffer, 0x40000, BS2NANDDivideCallback, block);
    } else {
        BS2Report("NANDReadAsync buf:0x%08X, length:0x%08X\n", NandBuffer, NandLength);
        NANDReadAsync(NandFile, (void *)NandBuffer, NandLength, BS2NANDDivideCallback, block);
    }
}

static void BS2NANDDivideWriteAsync(NANDFileInfo *info, const void *buffer, u32 length, NANDCallback callback, NANDCommandBlock *block) {
    NandCompletion = callback;
    NandOperation = 1;
    NandLength = length;
    NandTransferred = 0;
    NandFile = info;
    NandBuffer = (u8 *)buffer;
    if (NandLength > 0x40000) {
        BS2Report("NANDWriteAsync buf:0x%08X, length:0x%08X\n", NandBuffer, 0x40000);
        NANDWriteAsync(NandFile, (void *)NandBuffer, 0x40000, BS2NANDDivideCallback, block);
    } else {
        BS2Report("NANDWriteAsync buf:0x%08X, length:0x%08X\n", NandBuffer, NandLength);
        NANDWriteAsync(NandFile, (void *)NandBuffer, NandLength, BS2NANDDivideCallback, block);
    }
}

static BOOL CheckBS2CommandStatus() {
    static u32 CacheCommandComplete = 0;

    if (CheckDVDCommandStatus(&Block) == 0) {
        BS2Report("DVD command is issuing\n");
        return 0;
    }
    if (BS2BootFromCache == 0) {
        if (BS2BootCaching == 0) {
            BS2Report("Boot cache mode OFF\n");
            return 1;
        }
    }
    if (NandPending != 0) {
        BS2Report("NAND command is issuing\n");
        return 0;
    }
    if (CacheCommandComplete != 0) {
        if (State == 3) {
            CacheSeekComplete = 1;
        }
        CacheCommandComplete = 0;
        return TRUE;
    }
    switch (State) {
    case 3: {
        CacheLength = 0;
        BS2Report("Seek cache.dat\n");
        NandPending = 1;
        CacheCommandComplete = 1;
        NANDSeekAsync(&BS2CacheFileInfo, 0, 0, BS2NANDCallback, &BS2NandBlock);
        return 0;
    }
    case 5: {
        if (BS2BootCaching != 0) {
            BS2Report("Write drive info\n");
            NandPending = 1;
            CacheCommandComplete = 1;
            CacheLength = CacheLength + 0x20;
            if ((unsigned int)CacheLength > 0xb00000) {
                BS2NANDCallback(-1, NULL);
                return 1;
            } else {
                BS2NANDDivideWriteAsync(&BS2CacheFileInfo, &DriveInfo, 0x20, BS2NANDCallback, &BS2NandBlock);
                return 0;
            }
        }
        break;
    }
    case 9: {
        if (BS2BootCaching != 0) {
            BS2Report("Write disk id\n");
            NandPending = 1;
            CacheCommandComplete = 1;
            CacheLength = CacheLength + 0x20;
            if ((unsigned int)CacheLength > 0xb00000) {
                BS2NANDCallback(-1, NULL);
                return 1;
            } else {
                BS2NANDDivideWriteAsync(&BS2CacheFileInfo, &DiskID, 0x20, BS2NANDCallback, &BS2NandBlock);
                return 0;
            }
        }
        break;
    }
    case 0x10: {
        if (BS2BootCaching != 0) {
            BS2Report("Write game toc\n");
            NandPending = 1;
            CacheCommandComplete = 1;
            CacheLength = CacheLength + 0x20;
            if ((unsigned int)CacheLength > 0xb00000) {
                BS2NANDCallback(-1, NULL);
                return 1;
            } else {
                BS2NANDDivideWriteAsync(&BS2CacheFileInfo, &GameTOCBuf, 0x20, BS2NANDCallback, &BS2NandBlock);
                return 0;
            }
        }
        break;
    }
    case 0x12: {
        if (BS2BootCaching != 0) {
            BS2Report("Write partition ifno\n");
            NandPending = 1;
            CacheCommandComplete = 1;
            CacheLength += OSRoundUp32B(((DVDGameTOC *)DataToc)->partitionCount * sizeof(DVDPartitionInfo)) + 32;
            if ((unsigned int)CacheLength > 0xb00000) {
                BS2NANDCallback(-1, NULL);
                return 1;
            } else {
                BS2NANDDivideWriteAsync(&BS2CacheFileInfo, &PartitionInfoBuf, OSRoundUp32B(((DVDGameTOC *)DataToc)->partitionCount * sizeof(DVDPartitionInfo)) + 32, BS2NANDCallback, &BS2NandBlock);
                return 0;
            }
        }
        break;
    }
    case 0x14: {
        if (BS2BootCaching != 0) {
            BS2Report("Write boot info 3\n");
            NandPending = 1;
            CacheCommandComplete = 1;
            CacheLength = CacheLength + 0x2000;
            if ((unsigned int)CacheLength > 0xb00000) {
                BS2NANDCallback(-1, NULL);
                return 1;
            } else {
                BS2NANDDivideWriteAsync(&BS2CacheFileInfo, &bi3, 0x2000, BS2NANDCallback, &BS2NandBlock);
                return 0;
            }
        }
        break;
    }
    case 0x26: {
        if (BS2BootCaching != 0) {
            BS2Report("Write open partition\n");
            NandPending = 1;
            CacheCommandComplete = 1;
            CacheLength = CacheLength + 0x4a00;
            if ((unsigned int)CacheLength > 0xb00000) {
                BS2NANDCallback(-1, NULL);
                return 1;
            } else {
                BS2NANDDivideWriteAsync(&BS2CacheFileInfo, &PartitionParams.tmd, 0x4a00, BS2NANDCallback, &BS2NandBlock);
                return 0;
            }
        }
        break;
    }
    case 0x28: {
        if (BS2BootCaching != 0) {
            BS2Report("Write apploader header\n");
            NandPending = 1;
            CacheCommandComplete = 1;
            CacheLength = CacheLength + 0x20;
            if ((unsigned int)CacheLength > 0xb00000) {
                BS2NANDCallback(-1, NULL);
                return 1;
            } else {
                BS2NANDDivideWriteAsync(&BS2CacheFileInfo, &AppLoaderHdr, 0x20, BS2NANDCallback, &BS2NandBlock);
                return 0;
            }
        }
        break;
    }
    case 0x2a: {
        if (BS2BootCaching != 0) {
            BS2Report("Write apploader\n");
            NandPending = 1;
            CacheCommandComplete = 1;
            CacheLength = CacheLength + (AppLoaderHdr.length + 0x1fU & 0xffffffe0);
            if ((unsigned int)CacheLength > 0xb00000) {
                BS2NANDCallback(-1, NULL);
                return 1;
            } else {
                BS2NANDDivideWriteAsync(&BS2CacheFileInfo, (void *)0x81200000, (AppLoaderHdr.length + 0x1fU & 0xffffffe0), BS2NANDCallback,
                                        &BS2NandBlock);
                return 0;
            }
        }
        break;
    }
    case 0x2c: {
        if (BS2BootCaching != 0) {
            BS2Report("Write apploader load\n");
            NandPending = 1;
            CacheCommandComplete = 1;
            CacheLength = CacheLength + LoaderLength;
            if ((unsigned int)CacheLength > 0xb00000) {
                BS2NANDCallback(-1, NULL);
                return 1;
            } else {
                BS2NANDDivideWriteAsync(&BS2CacheFileInfo, (void *)LoaderAddress, (unsigned int)LoaderLength, BS2NANDCallback,
                                        &BS2NandBlock);
                return 0;
            }
        }
        break;
    }
    case 0x2e: {
        if (BS2BootCaching != 0) {
            BS2Report("Write banner\n");
            NandPending = 1;
            CacheCommandComplete = 1;
            CacheLength = CacheLength + (BannerLength + 0x1fU & 0xffffffe0);
            if ((unsigned int)CacheLength > 0xb00000) {
                BS2NANDCallback(-1, NULL);
                return 1;
            } else {
                BS2NANDDivideWriteAsync(&BS2CacheFileInfo, (void *)BannerBuffer, BannerLength + 0x1fU & 0xffffffe0, BS2NANDCallback,
                                        &BS2NandBlock);
                return 0;
            }
        }
        break;
    }
    case 0x30: {
        break;
    }
    default: {
        return 1;
    }
    }
    return TRUE;
}

static void BS2InquiryAsync(void *buffer, s32 length, u32 offset) {
    if (BS2BootFromCache) {
        BS2Report("Unencrypted read from cache.dat\n");
        NandPending = 1;
        BS2NANDDivideReadAsync(&BS2CacheFileInfo, buffer, length, (NANDCallback)BS2NANDCallback, &BS2NandBlock);
    } else {
        DVDUnencryptedReadAbsAsyncForBS(&Block, buffer, length, offset, BS2DVDCallback);
    }
}

static void BS2ReadDiskID(void *buffer, s32 length, u32 offset) {
    if (BS2BootFromCache) {
        BS2Report("Read from cache.dat\n");
        NandPending = 1;
        BS2NANDDivideReadAsync(&BS2CacheFileInfo, buffer, length, (NANDCallback)BS2NANDCallback, &BS2NandBlock);
    } else {
        ((BOOL (*)(DVDCommandBlock *, void *, s32, u32, DVDCommandCallback))DVDReadAbsAsyncForBS)(&Block, buffer, length, offset,
                                                                                                  BS2DVDCallback);
    }
}

BS2State BS2Tick() {
    static u32 AudioBufferUnconfigured;
    u32 interruptsEnabled = OSDisableInterrupts();
    u32 loaderAddress;
    u32 loaderLength;
    u32 loaderOffset;
    u32 titleCode;
    s32 titlePrefix;
    u8 titleCharacters[4];
    DVDFileInfo bannerFile;
    u32 bannerAddress;
    u32 discRegion;
    s32 status;
    u64 currentTime;
    BOOL regionMatches;
    char productRegion;
    char *ticketByte;
    u32 entryCount;
    DVDGameTOC *dataToc;
    u32 readAddress;
    const char *discTitle;
    const DVDDiskID *bootDisc;
    switch (State) {
    case 0:
        BS2Report("No Disk          : %d\n", BS2NoDisk);
        BS2Report("Drive Reset      : %d\n", BS2DriveReset);
        BS2Report("Wait Spinup      : %d\n", BS2WaitSpinup);
        BS2Report("Boot From Cache  : %d\n", BS2BootFromCache);
        DvdProgress = (u32 *)0x800030d4;
        PartitionParams.numTmdBytes = 0;
        PartitionParams.numCertBytes = 0;
        PartitionParams.dataWordOffset = 0;
        *DvdProgress = 0;
        DvdTransferred = 0;
        DvdTransferLength = 0;
        AudioBufferUnconfigured = 1;
        CoverBlock.state = 0;
        if (BS2NoDisk != 0)
            State = BS2_STT_NO_DISK;
        else if (BS2DriveReset != 0)
            State = BS2_STT_2;
        else {
            State = BS2_STT_3;
            currentTime = __OSGetSystemTime();
            ResetTime = currentTime;
            if (BS2WaitSpinup != 0 && BS2BootFromCache == 0) {
                SpinupDeadline = __OSGetSystemTime() + OSSecondsToTicks(5);
            } else {
                currentTime = __OSGetSystemTime();
                SpinupDeadline = currentTime;
            }
        }
        break;
    case 1:
        currentTime = __OSGetSystemTime();
        if ((OSTime)currentTime < (OSTime)(((OS_BUS_CLOCK >> 2) / 1000) * 80))
            break;
        State = BS2_STT_2;
    case 2:
        PartitionParams.numTmdBytes = 0;
        PartitionParams.numCertBytes = 0;
        PartitionParams.dataWordOffset = 0;
        *DvdProgress = 0;
        DvdTransferred = 0;
        DvdTransferLength = 0;
        AudioBufferUnconfigured = 1;
        currentTime = __OSGetSystemTime();
        ResetTime = currentTime;
        if (BS2BootFromCache != 0) {
            currentTime = __OSGetSystemTime();
            SpinupDeadline = currentTime;
        } else {
            SpinupDeadline = __OSGetSystemTime() + OSSecondsToTicks(7);
        }
        __OSClearRTCFlags();
        DVDResetAsync(&Block, BS2DVDCallback);
        CoverBlock.state = 0;
        DriveWasReset = 1;
        State = BS2_STT_3;
    case 3:
        currentTime = __OSGetSystemTime();
        if ((OSTime)currentTime < (OSTime)SpinupDeadline) {
            status = CheckDVDCommandStatus(&Block);
            if (status != 0) {
                currentTime = __OSGetSystemTime();
                if ((OSTime)(currentTime - ResetTime) >= (OSTime)(((OS_BUS_CLOCK >> 2) / 1000) * 200)) {
                    status = CheckDVDCommandStatus(&CoverBlock);
                    if (status != 0) {
                        currentTime = __OSGetSystemTime();
                        if ((OSTime)(currentTime - CoverPollTime) >= OSMillisecondsToTicks((OSTime)100)) {
                            currentTime = __OSGetSystemTime();
                            CoverPollTime = currentTime;
                            __DVDGetCoverStatusAsync(&CoverBlock, BS2DVDCallback);
                        }
                    }
                }
            }
        } else {
            status = CheckBS2CommandStatus();
            if (status != 0) {
                State = BS2_STT_4;
            case 4:
                if (BS2BootFromCache != 0) {
                    BS2Report("Read drive info from cache.dat\n");
                    NandPending = 1;
                    BS2NANDDivideReadAsync(&BS2CacheFileInfo, &DriveInfo, 0x20, BS2NANDCallback, &BS2NandBlock);
                } else {
                    DVDInquiryAsync(&Block, &DriveInfo, BS2DVDCallback);
                }
                State = BS2_STT_5;
            }
        }
        break;
    case 5:
        status = CheckBS2CommandStatus();
        if (status != 0) {
            OSBootInfo *bootInfo = (OSBootInfo *)OSPhysicalToCached(OS_ADDR_BOOT_INFO);

            if ((bootInfo->consoleType & 0xf0000000) == 0) {
                __OSDeviceCode = 0x8002;
            } else {
                __OSDeviceCode = DriveInfo.deviceCode | 0x8000;
            }
            State = BS2_STT_8;
        }
        break;
    case 6:
        DVDDownRotationAsync(&Block, BS2DVDCallback);
        State = BS2_STT_7;
        break;
    case 7:
        status = CheckDVDCommandStatus(&Block);
        if (status == 0)
            break;
        State = BS2_STT_8;
    case 8:
        if (BS2BootFromCache != 0) {
            BS2Report("Read disk id from cache.dat\n");
            NandPending = 1;
            BS2NANDDivideReadAsync(&BS2CacheFileInfo, &DiskID, 0x20, BS2NANDCallback, &BS2NandBlock);
        } else {
            DVDReadDiskID(&Block, &DiskID, BS2DVDCallback);
        }
        State = BS2_STT_9;
        break;
    case 9:
    case 10: {
        status = CheckBS2CommandStatus();
        if (status == 0)
            break;
        memcpy((void *)0x80000000, &DiskID, sizeof(DVDDiskID));
        bootDisc = (const DVDDiskID *)0x80000000;
        if (bootDisc->rvlMagic == 0x5d1c9ea3) {
            discTitle = (const char *)0x80000000;
            status = strncmp(discTitle, "RAAE", 4);
            // MWCC needs both 410 comparisons to preserve the instruction stream.
            if (status == 0 || strncmp(discTitle, "408", 3) == 0 ||
                strncmp(discTitle, "410", 3) == 0 ||
                strncmp(discTitle, "410", 3) == 0)
                regionMatches = FALSE;
            else
                regionMatches = TRUE;
            if (regionMatches) {
                BS2Report("REVOLUTION DISC\n");
                __DVDLayoutFormat = 0;
                State = BS2_STT_15;
                break;
            }
        }
        if (bootDisc->gcMagic == -0x3dcc60c3) {
            BS2Report("DOLPHIN LAYOUT FORMAT\n");
            __DVDLayoutFormat = 2;
        } else {
            BS2Report("UNKNOWN\n");
            State = BS2_STT_54;
            break;
        }
        State = BS2_STT_11;
    }
    case 0xb: {
        u32 audioBufferSize;
        bootDisc = (const DVDDiskID *)0x80000000;
        if (AudioBufferUnconfigured == 0) {
            State = BS2_STT_GC_GAME;
            break;
        }
        AudioBufferUnconfigured = 0;
        if (bootDisc->streaming != 0) {
            audioBufferSize = bootDisc->streamingBufSize;
            audioBufferSize = audioBufferSize != 0 ? audioBufferSize : 10;
            __DVDAudioBufferConfig(&Block, 1, audioBufferSize, BS2DVDCallback);
        } else {
            __DVDAudioBufferConfig(&Block, 0, 0, BS2DVDCallback);
        }
        State = BS2_STT_12;
    }
    case 0xc:
        status = CheckDVDCommandStatus(&Block);
        if (status == 0)
            break;
        State = BS2_STT_13;
    case 0xd:
        DVDUnencryptedReadAbsAsyncForBS(&Block, &bi2, 0x2000, 0x110, BS2DVDCallback);
        State = BS2_STT_14;
    case 0xe:
        status = CheckDVDCommandStatus(&Block);
        if (status == 0)
            break;
        discRegion = bi2.countryCode;
        productRegion = SCGetProductGameRegion();
        // MWCC needs the shared false-result paths in these region switches.
        switch (productRegion) {
        case 0:
            if (discRegion != 0)
                goto invalidGcRegion;
            regionMatches = TRUE;
            break;
        case 1:
            if (discRegion != 1)
                goto invalidGcRegion;
            regionMatches = TRUE;
            break;
        case 2:
            if (discRegion != 2)
                goto invalidGcRegion;
            regionMatches = TRUE;
            break;
        default:
invalidGcRegion:
            regionMatches = FALSE;
            break;
        }
        if (!regionMatches)
            State = BS2_STT_54;
        else
            State = BS2_STT_GC_GAME;
        break;
    case 0xf:
        if (LoadingTitle != 0) {
            State = BS2_STT_LOCKED_DISK;
            break;
        }
        BS2InquiryAsync(&GameTOCBuf, 0x20, 0x10000);
        State = BS2_STT_16;
    case 0x10:
        status = CheckBS2CommandStatus();
        if (status == 0)
            break;
        *(DVDGameTOC **)&GameToc = (DVDGameTOC *)GameTOCBuf;
        BS2Report("length            ... 0x%08X\n", 0x20);
        BS2Report("numGamePartitions ... 0x%08X\n", (*(DVDGameTOC **)&GameToc)->partitionCount);
        BS2Report("partitionInfos    ... 0x%08X\n", (u32)(*(DVDGameTOC **)&GameToc)->partitionInfo);
        dataToc = (DVDGameTOC *)GameTOCBuf + 1;
        DataToc = (u32)dataToc;
        BS2Report("numGamePartitions ... 0x%08X\n", dataToc->partitionCount);
        BS2Report("partitionInfos    ... 0x%08X\n", ((DVDGameTOC *)DataToc)->partitionInfo);
        State = BS2_STT_17;
    case 0x11:
        BS2InquiryAsync(&PartitionInfoBuf, ((((DVDGameTOC *)DataToc)->partitionCount * 8 + 0x1fU & 0xffffffe0) + 0x20),
                        (u32)(*(DVDGameTOC **)&GameToc)->partitionInfo);
        State = BS2_STT_18;
    case 0x12: {
        status = CheckBS2CommandStatus();
        if (status == 0)
            break;
        PartitionCursor = (u32)&PartitionInfoBuf;
        UpdatePartition = 0;
        GamePartition = 0;
        for (titleCode = 0; titleCode < (*(DVDGameTOC **)&GameToc)->partitionCount; titleCode = titleCode + 1) {
            BS2Report("gamePartition ... 0x%08X\n", (u32)((DVDPartitionInfo *)PartitionCursor)->partition);
            BS2Report("type          ... 0x%08X\n", ((DVDPartitionInfo *)PartitionCursor)->partitionType);
            if (((DVDPartitionInfo *)PartitionCursor)->partitionType == 0)
                GamePartition = PartitionCursor;
            if (((DVDPartitionInfo *)PartitionCursor)->partitionType == 1)
                UpdatePartition = PartitionCursor;
            PartitionCursor = (u32)((DVDPartitionInfo *)PartitionCursor + 1);
        }
        PartitionCursor = (u32)((DVDPartitionInfo *)PartitionInfoBuf + 4);
        for (titleCode = 0; titleCode < ((DVDGameTOC *)DataToc)->partitionCount; titleCode = titleCode + 1) {
            BS2Report("gamePartition ... 0x%08X\n", (u32)((DVDPartitionInfo *)PartitionCursor)->partition);
            BS2Report("type          ... 0x%08X\n", ((DVDPartitionInfo *)PartitionCursor)->partitionType);
            PartitionCursor = (u32)((DVDPartitionInfo *)PartitionCursor + 1);
        }
        State = BS2_STT_19;
    }
    case 0x13:
        BS2InquiryAsync(&bi3, 0x2000, 0x13800);
        State = BS2_STT_20;
    case 0x14:
        status = CheckBS2CommandStatus();
        if (status == 0)
            break;
        if (bi3.magic == -0x3c07e572) {
            discRegion = bi3.countryCode;
            productRegion = SCGetProductGameRegion();
            switch (productRegion) {
            case 0:
                if (discRegion != 0)
                    goto invalidRvlRegion;
                regionMatches = TRUE;
                break;
            case 1:
                if (discRegion != 1)
                    goto invalidRvlRegion;
                regionMatches = TRUE;
                break;
            case 2:
                if (discRegion != 2)
                    goto invalidRvlRegion;
                regionMatches = TRUE;
                break;
            case 4:
                if (discRegion != 4)
                    goto invalidRvlRegion;
                regionMatches = TRUE;
                break;
            case 5:
                if (discRegion != 5)
                    goto invalidRvlRegion;
                regionMatches = TRUE;
                break;
            default:
invalidRvlRegion:
                regionMatches = FALSE;
                break;
            }
            if (!regionMatches) {
                State = BS2_STT_54;
                break;
            }
            status = BS2IsValidDisc();
            if (status == 0) {
                State = BS2_STT_54;
                break;
            }
        } else {
            State = BS2_STT_54;
            break;
        }
        if (BS2BootFromCache != 0) {
            State = BS2_STT_37;
            break;
        }
        if (UpdatePartition != 0)
            State = BS2_STT_21;
        else {
            if (GamePartition != 0)
                State = BS2_STT_37;
            else
                State = BS2_STT_54;
            break;
        }
    case 0x15:
        DVDOpenPartitionAsync(&Block, &PartitionParams.tmd,
                              (u32)((DVDPartitionInfo *)UpdatePartition)->partition,
                              BS2DVDCallback);
        PartitionOpen = 1;
        State = BS2_STT_22;
        break;
    case 0x16:
        status = CheckDVDCommandStatus(&Block);
        if (status != 0) {
            CurrentTmd = (u32)&PartitionParams.tmd;
            BS2Report("TMD ver        ... 0x%02X\n", ((ESTitleMeta *)CurrentTmd)->head.version);
            BS2Report("CA CRL ver     ... 0x%02X\n", ((ESTitleMeta *)CurrentTmd)->head.caCrlVersion);
            BS2Report("Signer CRL ver ... 0x%02X\n", ((ESTitleMeta *)CurrentTmd)->head.signerCrlVersion);
            BS2Report("Req sys ver    ... 0x%08X%08X\n", (u32)(((ESTitleMeta *)CurrentTmd)->head.sysVersion >> 32),
                      (u32)((ESTitleMeta *)CurrentTmd)->head.sysVersion);
            BS2Report("Title ID       ... 0x%08X%08X\n", (u32)(((ESTitleMeta *)CurrentTmd)->head.titleId >> 32),
                      (u32)((ESTitleMeta *)CurrentTmd)->head.titleId);
            State = BS2_STT_23;
        }
        break;
    case 0x17:
        DVDReadAbsAsyncForBS(&Block, &UpdateDiskID, 0x20, 0, BS2DVDCallback);
        State = BS2_STT_24;
        break;
    case 0x18:
        status = CheckDVDCommandStatus(&Block);
        if (status == 0)
            break;
        if (UpdateDiskID.rvlMagic == 0x5d1c9ea3)
            State = BS2_STT_25;
        else {
            State = BS2_STT_35;
            break;
        }
    case 0x19:
        DVDReadAbsAsyncForBS(&Block, &AppLoaderHdr, 0x20, 0x910, BS2DVDCallback);
        State = BS2_STT_26;
        break;
    case 0x1a:
        status = CheckDVDCommandStatus(&Block);
        if (status == 0)
            break;
        BS2Report("  appLoaderLength ...... 0x%x\n", AppLoaderHdr.length);
        BS2Report("  appLoaderFunc1  ...... 0x%x\n", AppLoaderHdr.entryPoint);
        State = BS2_STT_27;
    case 0x1b:
        DVDReadAbsAsyncForBS(&Block, (void *)0x81200000, (AppLoaderHdr.length + 0x1fU & 0xffffffe0), 0x918, BS2DVDCallback);
        State = BS2_STT_28;
        break;
    case 0x1c:
        status = CheckDVDCommandStatus(&Block);
        if (status == 0)
            break;
        ICInvalidateRange((void *)0x81200000, (AppLoaderHdr.length + 0x1fU & 0xffffffe0));
        ((void (*)(void *, void *, void *))AppLoaderHdr.entryPoint)(((void *)&LoaderInit), ((void *)&LoaderMain), ((void *)&LoaderClose));
        ((void (*)(void *))((*(void **)&LoaderInit)))((void *)BS2Report);
        BS2Report("\nApploader Initialized\n");
        State = BS2_STT_29;
    case 0x1d: {
        u32 readInterruptsEnabled;
        status = ((int (*)(u32 *, u32 *, u32 *))((*(void **)&LoaderMain)))(&loaderAddress, &loaderLength, &loaderOffset);
        if (status != 0) {
            BS2Report("Addr [0x%x] length [0x%x] offset [0x%x]\n", loaderAddress, loaderLength, loaderOffset);
            readInterruptsEnabled = OSDisableInterrupts();
            DVDReadAbsAsyncForBS(&Block, (void *)loaderAddress, loaderLength, loaderOffset >> __DVDLayoutFormat, BS2DVDCallback);
            if (*(s32 *)DvdProgress != 0) {
                DvdReadPending = 1;
                DvdTransferLength = loaderLength;
            }
            OSRestoreInterrupts(readInterruptsEnabled);
            State = BS2_STT_30;
        } else
            State = BS2_STT_31;
        break;
    }
    case 0x1e:
        status = CheckDVDCommandStatus(&Block);
        if (status != 0)
            State = BS2_STT_29;
        break;
    case 0x1f:
        __DVDFSInit();
        BS2UpdateInit((MEMAllocator *)Allocator);
        State = BS2_STT_32;
        break;
    case 0x20:
        status = BS2UpdateState();
        if (status == 0) {
            status = __DVDGetDriveStatus();
            if (((status == 4) || (status == 5)) || ((status == 3 || (status == 0xb)))) {
                State = BS2_STT_60;
                DVDCancelAllAsync(BS2DVDCallback);
                if (status == 0xb)
                    RetryErrorFlag = 1;
            }
        } else {
            status = BS2GetUpdateEntryNum();
            if ((status != 0) && (BS2UpdateState() == 1)) {
                entryCount = BS2GetUpdateEntryNum();
                BS2Report("%d entries\n", entryCount);
                currentTime = __OSGetSystemTime();
                if ((OSTime)(currentTime - CoverPollTime) >= OSMillisecondsToTicks((OSTime)100)) {
                    currentTime = __OSGetSystemTime();
                    CoverPollTime = currentTime;
                    __DVDGetCoverStatusAsync(&CoverBlock, BS2DVDCallback);
                    State = BS2_STT_UPDATE_DISK;
                }
            } else {
                BS2Report("No entry\n");
                State = BS2_STT_35;
            }
        }
        break;
    case 0x21:
        status = CheckDVDCommandStatus(&CoverBlock);
        if ((status != 0) && (BS2UpdateState() == 1)) {
            currentTime = __OSGetSystemTime();
            if ((OSTime)(currentTime - CoverPollTime) >= OSMillisecondsToTicks((OSTime)100)) {
                currentTime = __OSGetSystemTime();
                CoverPollTime = currentTime;
                __DVDGetCoverStatusAsync(&CoverBlock, BS2DVDCallback);
            }
            break;
        }
        status = BS2UpdateState();
        if (status == 1)
            break;
        State = BS2_STT_RUNNING_UPDATE;
    case 0x22:
        status = BS2UpdateState();
        if (status == 2) {
            status = __DVDGetDriveStatus();
            if ((((status == 4) || (status == 5)) || (status == 3)) || (status == 0xb)) {
                State = BS2_STT_60;
                DVDCancelAllAsync(BS2DVDCallback);
                UpdateErrorFlag = 1;
                if (status == 0xb)
                    RetryErrorFlag = 1;
            }
        } else {
            status = BS2UpdateState();
            if (status == 3) {
                BS2Report("Update success\n");
            } else {
                status = BS2UpdateState();
                if (status == 4) {
                    BS2Report("Update success\nPlease reboot\n");
                } else {
                    BS2Report("Update failed\n");
                    UpdateErrorFlag = 1;
                    status = __DVDGetDriveStatus();
                    if (((status == 4) || (status == 5)) || ((status == 3 || (status == 0xb)))) {
                        State = BS2_STT_60;
                        DVDCancelAllAsync(BS2DVDCallback);
                        if (status == 0xb)
                            RetryErrorFlag = 1;
                    } else
                        State = BS2_STT_UPDATE_FAILED;
                    break;
                }
            }
            State = BS2_STT_35;
        }
        break;
    case 0x23:
        DVDClosePartitionAsync(&Block, BS2DVDCallback);
        State = BS2_STT_36;
        break;
    case 0x24:
        status = CheckDVDCommandStatus(&Block);
        if (status == 0)
            break;
        PartitionOpen = 0;
        status = BS2UpdateState();
        if (status == 4) {
            State = BS2_STT_RESET_SYSTEM;
            break;
        }
        if (GamePartition != 0) {
            State = BS2_STT_37;
        } else {
            State = BS2_STT_54;
            break;
        }
    case 0x25: {
        u32 partitionOffset = (u32)((DVDPartitionInfo *)GamePartition)->partition;
        if (BS2BootFromCache != 0) {
            BS2Report("Open partition from cache.dat\n");
            NandPending = 1;
            BS2NANDDivideReadAsync(&BS2CacheFileInfo, &PartitionParams.tmd, 0x4a00, BS2NANDCallback, &BS2NandBlock);
        } else
            DVDOpenPartitionAsync(&Block, &PartitionParams.tmd, partitionOffset, BS2DVDCallback);
        PartitionOpen = 1;
        State = BS2_STT_38;
        break;
    }
    case 0x26: {
        status = CheckBS2CommandStatus();
        if (status == 0)
            break;
        CurrentTmd = (u32)&PartitionParams.tmd;
        BS2Report("TMD ver        ... 0x%02X\n", ((ESTitleMeta *)CurrentTmd)->head.version);
        BS2Report("CA CRL ver     ... 0x%02X\n", ((ESTitleMeta *)CurrentTmd)->head.caCrlVersion);
        BS2Report("Signer CRL ver ... 0x%02X\n", ((ESTitleMeta *)CurrentTmd)->head.signerCrlVersion);
        BS2Report("Req sys ver    ... 0x%08X%08X\n", (u32)(((ESTitleMeta *)CurrentTmd)->head.sysVersion >> 32),
                  (u32)((ESTitleMeta *)CurrentTmd)->head.sysVersion);
        BS2Report("Title ID       ... 0x%08X%08X\n", (u32)(((ESTitleMeta *)CurrentTmd)->head.titleId >> 32),
                  (u32)((ESTitleMeta *)CurrentTmd)->head.titleId);
        RequiredIos = ((ESTitleMeta *)CurrentTmd)->head.sysVersion;
        titleCode = (u32)((ESTitleMeta *)CurrentTmd)->head.titleId;
        TitleCode = titleCode;
        if (((ESTitleMeta *)CurrentTmd)->head.sysVersion == 0x0000000100000010ULL) {
            BS2Report("This must be a backup disk\n");
            State = BS2_STT_FATAL_ERROR;
            break;
        }
        memset(titleCharacters, 0, sizeof(titleCharacters));
        titlePrefix = titleCode >> 0x18;
        titleCharacters[3] = (u8)titleCode;
        titleCharacters[0] = (char)(titleCode >> 0x18);
        switch (titlePrefix) {
        case 'D':
        case 'R':
        case 'S':
        case 'T':
            goto check_title_region;
        default:
            regionMatches = TRUE;
            goto title_region_checked;
        }
        {
        check_title_region:
            if (titleCharacters[3] == 0x41) {
                regionMatches = TRUE;
                goto title_region_checked;
            }
            productRegion = SCGetProductGameRegion();
            switch (productRegion) {
            case 0:
                switch ((s32)titleCharacters[3]) {
                case 'J':
                case 'W':
                    regionMatches = TRUE;
                    break;
                default:
                    goto title_region_mismatch;
                }
                break;
            case 1:
                switch ((s32)titleCharacters[3]) {
                case 'E':
                case 'X':
                case 'Y':
                case 'Z':
                    regionMatches = TRUE;
                    break;
                default:
                    goto title_region_mismatch;
                }
                break;
            case 2:
                switch (titleCharacters[3]) {
                case 'D':
                case 'F':
                case 'H':
                case 'I':
                case 'P':
                case 'R':
                case 'S':
                case 'U':
                case 'V':
                case 'X':
                case 'Y':
                case 'Z':
                    regionMatches = TRUE;
                    break;
                case 'W':
                    titleCharacters[1] = (u8)(titleCode >> 16);
                    titleCharacters[2] = (u8)(titleCode >> 8);
                    if (titleCharacters[0] != 'R' ||
                        (titleCode >> 16 & 0xff) != 'L' ||
                        (titleCode >> 8 & 0xff) != 'W')
                        goto title_region_mismatch;
                    regionMatches = TRUE;
                    break;
                default:
                    goto title_region_mismatch;
                }
                break;
            case 4:
                switch ((s32)titleCharacters[3]) {
                case 'K':
                    regionMatches = TRUE;
                    break;
                default:
                    goto title_region_mismatch;
                }
                break;
            case 5:
                switch ((s32)titleCharacters[3]) {
                case 'C':
                    regionMatches = TRUE;
                    break;
                default:
                    goto title_region_mismatch;
                }
                break;
            default:
            title_region_mismatch:
                regionMatches = FALSE;
                break;
            }
        }
    title_region_checked:
        if (!regionMatches) {
            RegionValid = 0;
            State = BS2_STT_47;
            break;
        }
        RegionValid = 1;
        State = BS2_STT_39;
    }
    case 0x27:
        BS2ReadDiskID(&AppLoaderHdr, 0x20, 0x910);
        State = BS2_STT_40;
        break;
    case 0x28:
        status = CheckBS2CommandStatus();
        if (status == 0)
            break;
        BS2Report("  appLoaderLength ...... 0x%x\n", AppLoaderHdr.length);
        BS2Report("  appLoaderFunc1  ...... 0x%x\n", AppLoaderHdr.entryPoint);
        State = BS2_STT_41;
    case 0x29:
        BS2ReadDiskID((void *)0x81200000, (AppLoaderHdr.length + 0x1fU & 0xffffffe0), 0x918);
        State = BS2_STT_42;
        break;
    case 0x2a:
        status = CheckBS2CommandStatus();
        if (status == 0)
            break;
        ICInvalidateRange((void *)0x81200000, (AppLoaderHdr.length + 0x1fU & 0xffffffe0));
        ((void (*)(void *, void *, void *))AppLoaderHdr.entryPoint)(((void *)&LoaderInit), ((void *)&LoaderMain), ((void *)&LoaderClose));
        ((void (*)(void *))((*(void **)&LoaderInit)))((void *)OSReport);
        BS2Report("\nApploader Initialized\n");
        State = BS2_STT_43;
    case 0x2b: {
        u32 readInterruptsEnabled;
        status = ((int (*)(u32 *, u32 *, u32 *))((*(void **)&LoaderMain)))(&loaderAddress, &loaderLength, &loaderOffset);
        if (status != 0) {
            BS2Report("Addr [0x%x] length [0x%x] offset [0x%x]\n", loaderAddress, loaderLength, loaderOffset);
            readInterruptsEnabled = OSDisableInterrupts();
            BS2ReadDiskID((void *)loaderAddress, loaderLength, loaderOffset >> __DVDLayoutFormat);
            readAddress = loaderAddress;
            LoaderLength = loaderLength;
            LoaderAddress = readAddress;
            LoaderOffset = loaderOffset;
            if ((*(s32 *)DvdProgress != 0) && (BS2BootFromCache == 0)) {
                DvdReadPending = 1;
                DvdTransferLength = loaderLength;
            }
            OSRestoreInterrupts(readInterruptsEnabled);
            State = BS2_STT_44;
        } else
            State = BS2_STT_45;
        break;
    }
    case 0x2c:
        status = CheckBS2CommandStatus();
        if (status != 0)
            State = BS2_STT_43;
        break;
    case 0x2d:
        if (PartitionParams.numTmdBytes != 0) {
            State = BS2_STT_47;
            break;
        }
        {
            __DVDFSInit();
            status = DVDConvertPathToEntrynum("/opening.bnr");
            if (status >= 0) {
                DVDFastOpen(status, &bannerFile);
                if (Allocator != 0) {
                    if (BannerAllocation != 0)
                        MEMFreeToAllocator((MEMAllocator *)Allocator, (void *)BannerAllocation);
                    status = (int)MEMAllocFromAllocator((MEMAllocator *)Allocator, ((bannerFile.length + 0x3fU) & 0xffffffe0));
                    BannerAllocation = status;
                    if (status == 0)
                        OSPanic("BS2Mach.c", 0x12c8, "BS2 ERROR >>> Cannnot alloc 0x%08x from MEMAllocator", bannerFile.length);
                    bannerAddress = BannerAllocation + 32 - BannerAllocation % 32;
                    BannerLength = bannerFile.length;
                    BannerBuffer = bannerAddress;
                    BS2Report("BannerBufferAddr : %08X\n", BannerBuffer);
                } else {
                    if ((BannerBuffer != 0) && (BannerLength != 0)) {
                        if (BannerLength < ((bannerFile.length + 0x1fU) & 0xffffffe0))
                            OSPanic("BS2Mach.c", 0x12d3, "BS2 ERROR >>> Banner buffer is not enough to load banner file (0x%08x)",
                                    bannerFile.length);
                        BS2Report("BannerBufferAddr : %08X\n", BannerBuffer);
                    } else
                        OSPanic("BS2Mach.c", 0x12da, "BS2 ERROR >>> MEMAllocator and banner buffer is not set");
                }
                if (bannerFile.length != 0) {
                    BS2ReadDiskID((void *)BannerBuffer, ((bannerFile.length + 0x1fU) & 0xffffffe0),
                                  bannerFile.startAddr >> __DVDLayoutFormat);
                    State = BS2_STT_46;
                    break;
                }
            }
            BannerAvailable = 0;
            if ((Allocator != 0) && (BannerAllocation != 0)) {
                MEMFreeToAllocator((MEMAllocator *)Allocator, (void *)BannerAllocation);
                BannerAllocation = 0;
                BannerBuffer = 0;
                BannerLength = 0;
            }
            State = BS2_STT_47;
        }
        break;
    case 0x2e:
        status = CheckBS2CommandStatus();
        if (status != 0) {
            BannerAvailable = 1;
            State = BS2_STT_47;
        }
        break;
    case 0x2f:
        DVDClosePartitionAsync(&Block, BS2DVDCallback);
        State = BS2_STT_48;
        break;
    case 0x30:
        status = CheckBS2CommandStatus();
        if (status != 0) {
            PartitionOpen = 0;
            if (RegionValid == 0)
                State = BS2_STT_54;
            else {
                if (BS2BootCaching != 0) {
                    BS2BootFromCache = 1;
                    BS2BootCaching = 0;
                }
                if (((u32)((ESTitleMeta *)CurrentTmd)->head.titleId == 0x4c4f43) && (LoadingTitle == 0))
                    State = BS2_STT_DATA_DISK;
                else if (PartitionParams.numTmdBytes != 0)
                    State = BS2_STT_START_LOCKED_DISK;
                else
                    State = BS2_STT_RVL_GAME;
                currentTime = __OSGetSystemTime();
                CoverPollTime = currentTime;
                __DVDGetCoverStatusAsync(&CoverBlock, BS2DVDCallback);
            }
        }
        break;
    case 0x31:
    case 0x45:
    case 0x48:
        status = CheckDVDCommandStatus(&CoverBlock);
        if ((status != 0) && (StartingGame == 0)) {
            currentTime = __OSGetSystemTime();
            if ((OSTime)(currentTime - CoverPollTime) >= OSMillisecondsToTicks((OSTime)100)) {
                currentTime = __OSGetSystemTime();
                CoverPollTime = currentTime;
                __DVDGetCoverStatusAsync(&CoverBlock, BS2DVDCallback);
            }
        }
        break;
    case 0x32:
        status = CheckDVDCommandStatus(&CoverBlock);
        if ((status != 0) && (StartingGame == 0)) {
            currentTime = __OSGetSystemTime();
            if ((OSTime)(currentTime - CoverPollTime) >= OSMillisecondsToTicks((OSTime)100)) {
                currentTime = __OSGetSystemTime();
                CoverPollTime = currentTime;
                __DVDGetCoverStatusAsync(&CoverBlock, BS2DVDCallback);
            }
        }
        break;
    case 0x33:
        PartitionOpen = 0;
        BannerAvailable = 0;
        if ((Allocator != 0) && (BannerAllocation != 0)) {
            MEMFreeToAllocator((MEMAllocator *)Allocator, (void *)BannerAllocation);
            BannerAllocation = 0;
            BannerBuffer = 0;
            BannerLength = 0;
        }
        LoadingTitle = 0;
        State = BS2_STT_2;
        break;
    case 0x34:
    case 0x35:
        BannerAvailable = 0;
        if ((Allocator != 0) && (BannerAllocation != 0)) {
            MEMFreeToAllocator((MEMAllocator *)Allocator, (void *)BannerAllocation);
            BannerAllocation = 0;
            BannerBuffer = 0;
            BannerLength = 0;
        }
        BS2BootFromCache = 0;
        BS2BootCaching = 1;
        LoadingTitle = 0;
        switch (BS2ReadDVDState(&Block)) {
        case 4:
        case 5:
            CheckDVDCommandStatus(&Block);
            break;
        default: {
            currentTime = __OSGetSystemTime();
            if ((OSTime)(currentTime - CoverPollTime) >= OSMillisecondsToTicks((OSTime)100)) {
                currentTime = __OSGetSystemTime();
                CoverPollTime = currentTime;
                __DVDGetCoverStatusAsync(&CoverBlock, BS2DVDCallback);
            }
        } break;
        }
        break;
    case 0x36:
        BannerAvailable = 0;
        if ((Allocator != 0) && (BannerAllocation != 0)) {
            MEMFreeToAllocator((MEMAllocator *)Allocator, (void *)BannerAllocation);
            BannerAllocation = 0;
            BannerBuffer = 0;
            BannerLength = 0;
        }
        LoadingTitle = 0;
        DVDChangeDiskAsyncForBS(&Block, BS2DVDCallback);
        State = BS2_STT_55;
    case 0x37:
    case 0x38:
        CheckDVDCommandStatus(&Block);
        break;
    case 0x39:
    case 0x3a:
        BannerAvailable = 0;
        if ((Allocator != 0) && (BannerAllocation != 0)) {
            MEMFreeToAllocator((MEMAllocator *)Allocator, (void *)BannerAllocation);
            BannerAllocation = 0;
            BannerBuffer = 0;
            BannerLength = 0;
        }
        LoadingTitle = 0;
        break;
    case 0x3c:
        status = CheckBS2CommandStatus();
        if (status != 0)
            State = BS2_STT_61;
        break;
    case 0x3d:
        DVDClosePartitionAsync(&Block, BS2DVDCallback);
        State = BS2_STT_62;
        break;
    case 0x3e:
        status = CheckBS2CommandStatus();
        if (status != 0) {
            PartitionOpen = 0;
            RestartRequested = 0;
            State = BS2_STT_63;
        }
        break;
    case 0x46:
        DVDGetPartitionParamsAsync(&Block, &PartitionParams, (u32)((DVDPartitionInfo *)GamePartition)->partition, BS2DVDCallback);
        State = BS2_STT_71;
        break;
    case 0x47: {
        status = CheckBS2CommandStatus();
        if (status != 0) {
            ticketByte = (char *)&PartitionParams.ticket;
            for (status = 0; status < sizeof(PartitionParams.ticket); ++status) {
                if ((u8)ticketByte[status] != 0) {
                    BS2Report("eTicket is none zero\n");
                    break;
                }
            }
            memcpy(&PartitionParams.ticketView, (void *)TitleTicketView, sizeof(ESTicketView));
            DCStoreRange(&PartitionParams.ticketView, sizeof(ESTicketView));
            DVDOpenPartitionWithParamsAsync(&Block, &PartitionParams, (u32)((DVDPartitionInfo *)GamePartition)->partition, BS2DVDCallback);
            PartitionOpen = 1;
            State = BS2_STT_38;
        }
    } break;
    case 0x3b:
    case 0x3f:
        if (UpdateErrorFlag != 0) {
            State = BS2_STT_UPDATE_FAILED;
            break;
        }
        if (RetryErrorFlag != 0) {
            RetryErrorFlag = 0;
            State = BS2_STT_DIRTY_DISK;
        }
        if (AbortFlag != 0) {
            AbortFlag = 0;
            State = BS2_STT_64;
            break;
        }
        switch (Block.state) {
        case 4:
        case 5:
            CheckDVDCommandStatus(&Block);
            break;
        default: {
            currentTime = __OSGetSystemTime();
            if ((OSTime)(currentTime - CoverPollTime) >= OSMillisecondsToTicks((OSTime)100)) {
                currentTime = __OSGetSystemTime();
                CoverPollTime = currentTime;
                __DVDGetCoverStatusAsync(&CoverBlock, BS2DVDCallback);
            }
        } break;
        }
        break;
    case 0x40:
        AbortFlag = 0;
        if (RestartRequested == 0)
            break;
        RestartRequested = 0;
        if (FatalErrorFlag != 0) {
            State = BS2_STT_FATAL_ERROR;
            break;
        }
        if (PartitionOpen != 0) {
            PartitionOpen = 0;
            State = BS2_STT_15;
            break;
        }
        if ((__DVDLayoutFormat == 2) || (BS2DriveReset != 0 && DriveWasReset == 0)) {
            State = BS2_STT_2;
            break;
        }
        if (Block.state == 10)
            Block.state = 0;
        currentTime = __OSGetSystemTime();
        CoverPollTime = currentTime;
        __DVDGetCoverStatusAsync(&CoverBlock, BS2DVDCallback);
        State = BS2_STT_3;
        break;
    case 0x41:
        BannerAvailable = 0;
        if ((Allocator != 0) && (BannerAllocation != 0)) {
            MEMFreeToAllocator((MEMAllocator *)Allocator, (void *)BannerAllocation);
            BannerAllocation = 0;
            BannerBuffer = 0;
            BannerLength = 0;
        }
        break;
    case 0x42:
        currentTime = __OSGetSystemTime();
        if ((OSTime)(currentTime - CoverOpenTime) < OSMillisecondsToTicks((OSTime)350))
            break;
        currentTime = __OSGetSystemTime();
        CoverPollTime = currentTime;
        __DVDGetCoverStatusAsync(&Block, BS2DVDCallback);
        State = BS2_STT_67;
    case 0x43:
    case 0x44:
        CheckDVDCommandStatus(&Block);
        break;
    default:
        OSPanic("BS2Mach.c", 0x14e3, "BS2 ERROR >>> UNKNOWN STATE");
        break;
    }
    OSRestoreInterrupts(interruptsEnabled);
    return State;
}
