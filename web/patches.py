"""Compiler-compatibility edits applied to copies of the sources in the web
build (web/build/conv). They only work around constructs that MWCC accepts and
clang rejects; the original files, and so the matching build, are untouched.
Each entry maps a source path to (old, new) replacements that must apply."""

PATCHES = {
    # the web entry point (web/hal/web_main.c) calls the boot program
    "src/BS2/BS2.c": [
        ("int main(int argc, char** argv) {", "int BS2Main(int argc, char** argv) {"),
    ],
    # the same explicit specialization is also defined in iplChanAppEdit.cpp
    "src/scene/board/iplFocusObject.cpp": [
        ("""        template <>
        VEC3 LinearIntp<VEC3>::get() const {""", """        template <>
        inline VEC3 LinearIntp<VEC3>::get() const {"""),
    ],
    # Emscripten's libc has a function of this name
    "libs/MSL/src/MSL_Common/misc_io.c": [
        ("extern void (*__stdio_exit)(void);", "extern void (*__msl_stdio_exit)(void);"),
        ("    __stdio_exit = __close_all;", "    __msl_stdio_exit = __close_all;"),
    ],
    # static definitions after a non-static declaration
    "libs/RevoEX/src/vf/develop/d_vf_sys.c": [
        ("static u32 VFSysGetSyncMode(", "u32 VFSysGetSyncMode("),
        ("static VFSys_handle* VFSysVol2HandleP(", "VFSys_handle* VFSysVol2HandleP("),
        ("static void VFSysSetDevErrInfo(", "void VFSysSetDevErrInfo("),
    ],
    "libs/RVL_SDK/src/wad/wad.c": [
        ("static u32 _WADIsTerminated(", "u32 _WADIsTerminated("),
    ],
    "libs/RVLFaceLib/src/RFL_System.c": [
        ("static inline RFLErrcode RFLiBootLoadAsync()", "RFLErrcode RFLiBootLoadAsync()"),
        ("static inline u32 RFLiGetUsedWorkSize()", "u32 RFLiGetUsedWorkSize()"),
        ("static inline BOOL RFLiCriticalError()", "BOOL RFLiCriticalError()"),
        ("static inline RFLErrcode RFLiBootLoad()", "RFLErrcode RFLiBootLoad()"),
    ],
    # assignment to a cast lvalue
    "libs/RVL_SDK/src/card/CARDRdwr.c": [
        ("((u8*)card->buffer) += 0x200;", "card->buffer = (u8*)card->buffer + 0x200;"),
        ("((u8*)card->buffer) += card->pageSize;", "card->buffer = (u8*)card->buffer + card->pageSize;"),
    ],
    # jump into the scope of an initialized local
    "src/scene/address/iplAddressEdit.cpp": [
        ("""        button->reserveAnm(ipl::scene::Button::IDANIM_APPEAR_LEFT_AND_RIGHT_BUTTON);
        ipl::layout::Animator* animator = mpCodeLayout->getAnim(0x1e);
        animator->initFrame();
        animator->restart();
        mState = 0x30;
        break;""", """        button->reserveAnm(ipl::scene::Button::IDANIM_APPEAR_LEFT_AND_RIGHT_BUTTON);
        {
        ipl::layout::Animator* animator = mpCodeLayout->getAnim(0x1e);
        animator->initFrame();
        animator->restart();
        }
        mState = 0x30;
        break;"""),
    ],
    "src/keyboard/tiInputForm.cpp": [
        ('extern "C" int wcsnicmp(const wchar_t* left, const wchar_t* right, u32 length);', ''),
        ("if (!wcsnicmp(current, prefix, length)) {",
         "if (!wcsnicmp((const char*)current, (const char*)prefix, length)) {"),
    ],
    # attributes after an initializer
    "libs/RVL_SDK/src/nand/NANDCore.c": [
        ('static char s_currentDir[NAND_MAX_PATH] = "/" ALIGN32;', 'static char s_currentDir[NAND_MAX_PATH] ALIGN32 = "/";'),
    ],
    "libs/RVL_SDK/src/nand/NANDOpenClose.c": [
        ('char canYouAlignMe[] = "pretty pls?" ALIGN32;', 'char canYouAlignMe[] ALIGN32 = "pretty pls?";'),
    ],
    "libs/RVLMiddleware/TMC_JPEG/src/jpegdec/jdec_main.c": [
        ("extern void* memset(void* dest, s32 val, u32 count);", ""),
    ],
    "src/scene/board/iplBoardObject.cpp": [
        ("""        template <> inline LinearIntp<VEC2>::~LinearIntp() {}
        template <> inline Interporation<VEC2>::~Interporation() {}""", ""),
    ],
    "src/scene/setting/iplSetting.cpp": [
        ("""            case ::gui::EventHandler::ON_TRIG:
                controller::Interface* input = static_cast<controller::Interface*>(data);
                if (input->downTrg(controller::BTN_INTERACT)) {
                    mpSetting->start_trig_event(paneName);
                }
                break;""", """            case ::gui::EventHandler::ON_TRIG: {
                controller::Interface* input = static_cast<controller::Interface*>(data);
                if (input->downTrg(controller::BTN_INTERACT)) {
                    mpSetting->start_trig_event(paneName);
                }
                break;
            }"""),
    ],
    "libs/MSL/src/MSL_Common/alloc.c": [
        ("static void Block_link(", "void Block_link("),
    ],
    "libs/RVLFaceLib/src/RFL_MakeRandomFace.c": [
        (f"static {t} RFLi_Get{n}(", f"{t} RFLi_Get{n}(") for t, n in [
            ("u8", "FacelineType"), ("u8", "EyeType"), ("u8", "EyebrowType"), ("u8", "NoseType"),
            ("u8", "MouthType"), ("u8", "FaceTexType"), ("u8", "FaceColor"), ("u8", "HairColor"),
            ("u8", "EyeColor"), ("u32", "RandU32")]
    ],
    "src/keyboard/MyTiManager.cpp": [
        ("mpDefaultInputForm->~InputForm();", "mpDefaultInputForm->~LayoutByNW4R();"),
        ("mpBigTextInputForm->~InputForm();", "mpBigTextInputForm->~LayoutByNW4R();"),
        ("""void bg::LayoutByNW4R::draw() {
    nw4rmanager::Layout::draw();
}""", """}  // namespace memo
void bg::LayoutByNW4R::draw() {
    nw4rmanager::Layout::draw();
}
namespace memo {"""),
        ("""void bg::Base::init() {}

void bg::Base::create(MEMAllocator*) {}""", """}  // namespace memo
void bg::Base::init() {}

void bg::Base::create(MEMAllocator*) {}
namespace memo {"""),
    ],
}
