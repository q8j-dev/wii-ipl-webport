// Program entry for the web build: prepares the emulated machine, then runs
// the System Menu's boot program (BS2Main, the decompiled BS2 main()) on the
// default OS thread.

#include "web.h"

#include <revolution/os.h>
#include <revolution/sc.h>

#include <string.h>

void __web_os_early_init(void);
void __web_threads_init(void);
void __web_gx_boot(void);
void __web_input_init(void);
int BS2Main(int argc, char** argv);

// First boot: SYSCONF starts empty; fill it the way the factory and the
// initial setup would (English, USA, 16:9, setup completed).
void __web_sc_defaults(void) {
    if (SCGetConfigDoneFlag())
        return;
    __web_report("[web] creating SYSCONF defaults");
    SCSetAspectRatio(SC_ASPECT_RATIO_16x9);
    SCSetLanguage(SC_LANG_ENGLISH);
    SCSetSoundMode(SC_SOUND_MODE_STEREO);
    SCSetProgressiveMode(0);
    SCSetEuRgb60Mode(1);
    SCSetScreenSaverMode(1);
    SCSetCounterBias(0);
    SCSetDisplayOffsetH(0);
    SCSetWpadMotorMode(1);
    SCSetWpadSensorBarPosition(0);
    SCSetWpadSpeakerVolume(0x58);
    SCSetEULA(TRUE);
    SCSetConfigDoneFlag(TRUE);
    SCSetConfigDoneFlag2(TRUE);
    {
        static SCSimpleAddress addr;
        memset(&addr, 0, sizeof(addr));
        if (SCGetSimpleAddressData(&addr) == FALSE || addr.id == 0xFFFFFFFF || addr.id == 0) {
            memset(&addr, 0, sizeof(addr));
            addr.id = 0x31000000;  // country 49 (United States)
            SCSetSimpleAddressData(&addr);
        }
    }
    SCFlushAsync(NULL);
}

int main(int argc, char** argv) {
    __web_os_early_init();
    __web_threads_init();
    __web_gx_boot();
    __web_input_init();
    static char* args[] = {"wiimenu", NULL};
    BS2Main(1, args);
    __web_fatal("BS2 returned");
    return 0;
}
