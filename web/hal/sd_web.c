// SD card slot: present but empty (the SD card menu reports that no card is
// inserted).

#include "web.h"

#include <revolution/sdi.h>

#include <string.h>

static char sName[] = "sd0";

ISD_Error ISD_InitCard(void) { return SD_ERROR_SUCCESS; }
ISD_Error ISD_ProbeCard(u32 slot) { return SD_ERROR_FATAL; }

ISD_Error ISD_MountCard(u32 slot, SDDev** dev) {
    if (dev && *dev) {
        memset(*dev, 0, sizeof(SDDev));
        (*dev)->SDDevFd = -1;
        (*dev)->SDDevName = sName;
        (*dev)->SDDevSlot = slot;
    }
    return SD_ERROR_SUCCESS;
}

ISD_Error ISD_UnmountCard(SDDev* dev) { return SD_ERROR_SUCCESS; }

ISD_Error ISD_GetHCRegister(SDDev* dev, u32 reg, u32* outValue, u32 size) {
    if (outValue)
        *outValue = 0;
    return SD_ERROR_SUCCESS;
}

ISD_Error ISD_ReadBlock(SDDev* dev, u32 offset, u8* buf, u32 size) { return SD_ERROR_FATAL; }
ISD_Error ISD_WriteBlock(SDDev* dev, u32 offset, u8* buf, u32 size) { return SD_ERROR_FATAL; }
ISD_Error ISD_ReadCardRegister(SDDev* dev, u32 cmd, u32* resp, u32 size) { return SD_ERROR_FATAL; }
ISD_Error ISD_RegisterDeviceIntrHandler(SDDev* dev, SDDevIntrCallback cb, void* arg) { return SD_ERROR_SUCCESS; }
ISD_Error ISD_UnregisterDeviceIntrHandler(SDDev* dev) { return SD_ERROR_SUCCESS; }
ISD_Error ISD_ResetDevice(SDDev* dev) { return SD_ERROR_SUCCESS; }

// Bit 0: card inserted.
ISD_Error ISD_GetDeviceStatus(SDDev* dev, u32* status) {
    if (status)
        *status = 0;
    return SD_ERROR_SUCCESS;
}
