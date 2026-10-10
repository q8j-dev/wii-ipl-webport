// /dev/di: a disc drive with no disc in it.
//
// Results follow the drive's interrupt types (1 = transfer complete,
// 2 = device error); the error register reports "medium not present".

#include "web.h"

#include <private/ios.h>

#include <string.h>

#define DI_OK 1
#define DI_ERROR 2

#define DI_ERR_NO_DISC 0x01023A00

s32 __web_di_ioctl(s32 cmd, u8* in, u32 inLen, u8* out, u32 outLen) {
    if (out && outLen)
        memset(out, 0, outLen);
    switch (cmd) {
        case 0x7A:  // prepare cover register: bit 0 = cover open (no disc)
            if (out && outLen >= 4)
                *(u32*)out = 1;
            return DI_OK;
        case 0x88:  // cover status: 1 = no disc
            if (out && outLen >= 4)
                *(u32*)out = 1;
            return DI_OK;
        case 0xE0:  // request error
            if (out && outLen >= 4)
                *(u32*)out = DI_ERR_NO_DISC;
            return DI_OK;
        case 0x70:  // read disk ID
        case 0x71:  // read
        case 0x8D:  // unencrypted read
        case 0xD0:
        case 0xAB:  // seek
            return DI_ERROR;
        case 0x95:  // prepare status register
        case 0x86:  // clear cover interrupt
        case 0x8A:  // reset
        case 0x8C:  // close partition
        case 0xDD:  // set max rotation
        case 0xE3:  // stop motor
        case 0xE4:  // audio buffer config
        case 0x12:  // inquiry
        case 0x79:
        case 0x7E:
        case 0x8E:
            return DI_OK;
    }
    __web_report("[di] ioctl 0x%x", cmd);
    return DI_ERROR;
}

s32 __web_di_ioctlv(s32 cmd, u32 nIn, u32 nOut, IOSIoVector* v) {
    for (u32 i = nIn; i < nIn + nOut; i++)
        if (v[i].base)
            memset(v[i].base, 0, v[i].length);
    __web_report("[di] ioctlv 0x%x", cmd);
    return DI_ERROR;
}
