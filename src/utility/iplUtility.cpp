// MWCC needs this local interpolation layout for the original Scroller methods.
#define IPL_MATH_INTERPORATION_H

#include <nw4r/math.h>

#include "utility/iplFrameController.h"

namespace ipl {
    namespace math {
        template <typename T>
        class Interporation : public utility::FrameController {
        public:
            void init(int playback, f32 maxFrame, f32 minFrame, const T& start, const T& end, f32 speed = 1.0f) {
                mStart = start;
                mEnd = end;
                utility::FrameController::init(playback, maxFrame, minFrame, speed);
            }

            const T& getStart() { return mStart; }
            const T& getEnd() { return mEnd; }

            void playBackwards() {
                mAnmType = ANIM_TYPE_BACKWARD;
                mState = ANIM_STATE_PLAY;
            }

        protected:
            T mStart;
            T mEnd;
        };

        template <typename T>
        class LinearIntp : public Interporation<T> {
        public:
            T get() const {
                T r = mEnd * getCurrentFrame();
                T b = mStart * (mMaxFrame - mFrame);
                b += r;
                return b * (f64)(1.0f / getMaxFrame());
            }

            T get2() const { return (((mStart * (mMaxFrame - mFrame)) + (mEnd * mFrame)) / mMaxFrame); }
        };

        template <typename T>
        class HermiteIntp : public utility::FrameController {
        public:
            HermiteIntp() {}

            void init(const T& start, const T& end, f32 maxFrame, f32 startTangent, f32 endTangent, int playback = ANIM_TYPE_FORWARD, f32 speed = 1.0f) {
                mStart = start;
                mEnd = end;
                utility::FrameController::init(playback, maxFrame, 0.0f, speed);
                mStartTangent = startTangent;
                mEndTangent = endTangent;
            }

            T get() const {
                f32 frame = mFrame;
                f32 inverseDuration = 1.0f / mMaxFrame;
                T r =
                    (mStart *
                     (1.0f + ((inverseDuration * (inverseDuration * (inverseDuration * (frame * (2.0f * frame * frame))))) -
                              (inverseDuration * (inverseDuration * (3.0f * frame * frame)))))) -
                    (mEnd * ((inverseDuration * (inverseDuration * (inverseDuration * (frame * (2.0f * frame * frame))))) -
                              (inverseDuration * (inverseDuration * (3.0f * frame * frame)))));
                r +=
                    (mStartTangent *
                     (frame +
                      ((inverseDuration * (inverseDuration * (frame * (frame * frame)))) -
                       (inverseDuration * (2.0f * frame * frame))))) +
                    (mEndTangent * ((inverseDuration * (inverseDuration * (frame * (frame * frame)))) -
                                (inverseDuration * (frame * frame))));

                return r;
            }

        protected:
            T mStart;
            T mEnd;
            f32 mStartTangent;
            f32 mEndTangent;
        };
    }
}

#include "iplUtility.h"

#include "iplSystem.h"

#include "sound/iplSound.h"

#include "utility/iplTPLValidity.h"

#include <private/nwc24.h>
#include <private/os.h>

#include <revolution/enc.h>

extern "C" ipl::math::VEC2* __ct__Q33ipl4math4VEC2Fff(ipl::math::VEC2*, f32, f32);

namespace ipl {
    namespace utility {
        const char* Language::mLangPath[10] = {"jpn", "eng", "ger", "fra", "spa", "ita", "ned", "chn", "eng", "kor"};

        void Calendar::setCalendarTime(OSCalendarTime* newCalendar) {
            u32 bias;
            u32 rtc;

            bias = SCGetCounterBias();
            __OSGetRTC(&rtc);

            __OSSetTime(OSSecondsToTicks((OSTime)(rtc + bias)));

            OSSram* sram = __OSLockSram();
            sram->counterBias = bias;
            sram->flags |= 0x20;
            __OSUnlockSram(TRUE);

            while (!__OSSyncSram()) {
            }

            rtc = 0;
            if (!__OSGetRTC(&rtc) || rtc & RTC_FLAGS_DISC_31) {
                __OSSetRTC(rtc);
            }

            s32 newBias = (s32)OSTicksToSeconds(OSCalendarTimeToTicks(newCalendar)) - (s32)rtc;
            SCSetCounterBias(newBias);

            __OSSetTime(OSCalendarTimeToTicks(newCalendar));
            NWC24iSynchronizeRtcCounter(FALSE);
        }

        const char* Language::getPath() {
            return mLangPath[System::getLanguage()];
        }

        BScroller::BScroller() {
            init();
        }

        void BScroller::init() {
            mState = -1;
            mPointerPosition.x = 0.0f;
            mPointerPosition.y = 0.0f;
            mScrollOrigin.x = 0.0f;
            mScrollOrigin.y = 0.0f;
            mSpeed = 0.0f;
            mSoundFreq = 0.0f;
        }

        void BScroller::reset() {
            if (mState >= 0) {
                System::getPointer()->setState(mState, Pointer::STATE_NORMAL);
                System::getPointer()->setScrollState(Pointer::NO_SCROLL);
            }
            init();
        }

        BOOL BScroller::calc() {
            BOOL result = FALSE;

            if (mState < 0) {
                for (int chan = 0; chan < WPAD_MAX_CONTROLLERS; chan++) {
                    controller::Interface* ctrl = System::getController(chan);
                    if (ctrl == NULL) {
                        continue;
                    }
                    if (!ctrl->down(controller::REVO_BTN_B)) {
                        continue;
                    }
                    if (!ctrl->isValidDpd()) {
                        continue;
                    }
                    if (!isYoungController(chan)) {
                        continue;
                    }

                    mState = chan;

                    mPointerPosition.x = math::abs_clamp<float>(ctrl->getDpdPos().x, 1.f);
                    mPointerPosition.y = math::abs_clamp<float>(ctrl->getDpdPos().y, 1.f);

                    mScrollOrigin = mPointerPosition;

                    System::getPointer()->setState(mState, Pointer::STATE_SCROLL);
                    System::getPointer()->setScrollState(mState);
                    System::getPointer()->setOrigin(get_cursor_pos(ctrl->getDpdProjectionPos()));

                    mSpeed = _get();
                    set_arw_param();
                    result = TRUE;
                    break;
                }
            } else {
                controller::Interface* ctrl = System::getController(mState);
                if (ctrl == NULL || !ctrl->down(controller::REVO_BTN_B) || !isYoungController(mState)) {
                    System::getPointer()->setState(mState, Pointer::STATE_NORMAL);
                    System::getPointer()->setScrollState(Pointer::NO_SCROLL);
                    init();
                } else if (ctrl->isValidDpd()) {
                    mPointerPosition.x = math::abs_clamp<float>(ctrl->getDpdPos().x, 1.f);
                    mPointerPosition.y = math::abs_clamp<float>(ctrl->getDpdPos().y, 1.f);

                    mSpeed = _get();
                    set_arw_param();
                } else {
                    if (math::abs<float>(mPointerPosition.x) < math::abs<float>(mPointerPosition.y)) {
                        f32 newVal;
                        if (mPointerPosition.y < 0.0f) {
                            newVal = -1.0f;
                        } else {
                            newVal = 1.0f;
                        }
                        mPointerPosition.y = newVal;
                        mSpeed = _get();
                        set_arw_param();
                    }
                }
            }

            if (math::abs<float>(mSoundFreq) > 128) {
                snd::getSystem()->startSE("WIPL_SE_B_SCROLL");
                f32 newFreq;
                if (mSoundFreq > 0.0f) {
                    newFreq = mSoundFreq - 128;
                } else {
                    newFreq = mSoundFreq + 128;
                }
                mSoundFreq = newFreq;
            }

            return result;
        }

        f32 BScroller::_get() {
            f32 diff = mPointerPosition.y - mScrollOrigin.y;
            f32 result = 0.f;
            if (diff < -0.01f) {
                result = -10.0f * (diff * diff);
            } else if (diff > 0.01f) {
                result = 10.0f * (diff * diff);
            }
            return result;
        }

        // MWCC needs IRO disabled to keep the direction test as branches.
#pragma push
#pragma ppc_iro_level 0
        void BScroller::set_arw_param() {
            int direction;
            if (mSpeed < 0.0f) {
                direction = 0;
            } else {
                direction = 1;
            }
            System::getPointer()->setPointDirection(direction);

            nw4r::ut::Rect rect;
            System::getProjectionRect(&rect);

            f32 arrowLen = math::abs<float>(mScrollOrigin.y - mPointerPosition.y) * rect.GetHeight();
            System::getPointer()->setArrowLength(arrowLen);

            System::getPointer()->setCursorScrolling(math::abs<float>(mSpeed) > 0.0f);
        }
#pragma pop

        bool BScroller::isActive() const {
            return mState != -1;
        }

        bool BScroller::isUp() const {
            return mSpeed < 0.0f;
        }

        bool BScroller::isDown() const {
            return mSpeed > 0.0f;
        }

        BOOL YoungBScroller::isYoungController(int chan) {
            controller::Interface* pController = System::getYoungController();
            if (pController != NULL && pController->getChannel() == chan) {
                return TRUE;
            }
            return FALSE;
        }

        // MWCC needs inlining disabled for this constructor's original call sequence.
#pragma dont_inline on
        Scroller::Scroller() {
            mState = 0;
            mDownLimit = 0.0f;
            mUpLimit = 0.0f;
            mScrollVelocity = 0.0f;
            mScroll = 0.0f;
            mButtonScrollOrigin = 0.0f;
            mVelocityDecay = 0.6f;
            mScrollAcceleration = 1.5f;

            anim.init(0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0, 1.0f);
        }
#pragma dont_inline reset

        f32 Scroller::movable_pos(f32 speed) const {
            f32 newPos;
            f32 curScroll = mScroll;
            newPos = curScroll + speed;
            if (newPos > mDownLimit) {
                return mDownLimit - curScroll;
            }
            if (newPos < mUpLimit) {
                return mUpLimit - curScroll;
            }
            return speed;
        }

        void Scroller::calc() {
            f32 oldScroll = mScroll;

            switch (mState) {
                case Pointer::SCROLL_CON_UP: {
                    mScrollVelocity = mScrollVelocity * mVelocityDecay - mScrollAcceleration;
                    if (mScrollVelocity > 0.0f) {
                        mScrollVelocity = 0.0f;
                    }
                    mScroll += mScrollVelocity;
                    mState = Pointer::SCROLL;
                    break;
                }
                case Pointer::SCROLL_CON_DOWN: {
                    mScrollVelocity = mScrollVelocity * mVelocityDecay;
                    mScrollVelocity += mScrollAcceleration;
                    if (mScrollVelocity < 0.0f) {
                        mScrollVelocity = 0.0f;
                    }
                    mScroll += mScrollVelocity;
                    mState = Pointer::SCROLL;
                    break;
                }
                case Pointer::SCROLL_BTN_UP: {
                    mButtonScrollOrigin = oldScroll;
                    anim.init(0.0f, -300.0f, 20.0f, 0.0f, 0.0f, 0, 1.0f);
                    anim.initFrame();
                    anim.restart();
                    mState = Pointer::SCROLLING_BY_BTN;
                    break;
                }
                case Pointer::SCROLL_BTN_DOWN: {
                    mButtonScrollOrigin = oldScroll;
                    anim.init(0.0f, 300.0f, 20.0f, 0.0f, 0.0f, 0, 1.0f);
                    anim.initFrame();
                    anim.restart();
                    mState = Pointer::SCROLLING_BY_BTN;
                    break;
                }
                case Pointer::SCROLLING_BY_BTN: {
                    anim.calc();
                    mScroll = mButtonScrollOrigin + anim.get();

                    if (!anim.isPlaying()) {
                        mScrollVelocity = 0.0f;
                        anim.initFrame();
                        anim.calc();
                        mState = Pointer::SCROLL;
                    }
                    break;
                }
            }

            if (mScroll > mDownLimit) {
                mScroll = mDownLimit;
            } else if (mScroll < mUpLimit) {
                mScroll = mUpLimit;
            }

            if (math::abs<float>(oldScroll - mScroll) > 1.0f) {
                snd::getSystem()->holdSE("WIPL_SE_MESSAGE_SCROLL");
            }
        }

        f32 Scroller::get() const {
            return mScroll;
        }

        void CharacterCode::shiftJISToUTF16(wchar_t* dest, const u8* src, s32 length) {
            s32 size = length;
            ENCConvertStringSjisToUnicode((u16*)dest, &size, src, (s32*)NULL);
        }

        void CharacterCode::ANSIToUTF16(wchar_t* dest, const u8* src, s32 length) {
            s32 size = length;
            ENCConvertStringLatin1ToUnicode((u16*)dest, &size, src, (s32*)NULL);
        }

        void CharacterCode::UTF16ToANSI(u8* dest, const wchar_t* src, s32 length) {
            s32 size = length;
            s32 result = ENCConvertStringUnicodeToLatin1(dest, &size, (u16*)src, NULL);
            while (result != 0) {
                if (result == -1) {
                    break;
                }
                dest += size;
                src += size + 1;
                length -= size;
                if (length > 0) {
                    *dest++ = ' ';
                    length--;
                }
                size = length;
                result = ENCConvertStringUnicodeToLatin1(dest, &size, (u16*)src, NULL);
            }
        }

        void CharacterCode::UTF8ToUTF16(wchar_t* dest, const char* src, s32 length) {
            s32 size = length;
            ENCConvertStringUtf8ToUtf16((u16*)dest, &size, (u8*)src, (s32*)NULL);
        }

        void CharacterCode::UTF16ToUTF8(char* dest, const wchar_t* src, s32 length) {
            s32 size = length;
            ENCConvertStringUtf16ToUtf8((u8*)dest, &size, (u16*)src, (s32*)NULL);
        }

        void CharacterCode::ANSIToUTF8(char* dest, const u8* src) {
            char* d = dest;
            const u8* s = src;

            while (*s != 0) {
                wchar_t utf16[1];
                s32 sz[3];
                sz[2] = 1;
                sz[1] = 1;
                sz[0] = 3;
                ENCConvertStringLatin1ToUnicode((u16*)utf16, &sz[2], s, &sz[1]);
                ENCConvertStringUtf16ToUtf8((u8*)d, &sz[0], (u16*)utf16, &sz[2]);
                s++;
                d += sz[0];
            }
        }

        void CharacterCode::ANSIToUTF8(char* dest, const u8* src, int count) {
            char* d = dest;
            const u8* s = src;

            for (int i = 0; i < count; i++) {
                wchar_t utf16[1];
                s32 sz[3];
                sz[2] = 1;
                sz[1] = 1;
                sz[0] = 3;
                ENCConvertStringLatin1ToUnicode((u16*)utf16, &sz[2], s, &sz[1]);
                ENCConvertStringUtf16ToUtf8((u8*)d, &sz[0], (u16*)utf16, &sz[2]);
                s += sz[1];
                d += sz[0];
            }
        }

        void CharacterCode::UTF8ToANSI(u8* dest, const char* src) {
            u8* d = dest;
            const char* s = src;

            while ((u8)*s != 0) {
                wchar_t utf16[1];
                s32 sz[3];
                sz[2] = 1;
                sz[1] = 1;
                sz[0] = 3;
                ENCConvertStringUtf8ToUtf16((u16*)utf16, &sz[2], (u8*)s, &sz[0]);
                ENCConvertStringUnicodeToLatin1(d, &sz[1], (u16*)utf16, &sz[2]);
                s += sz[0];
                d += sz[1];
            }
        }

        void CharacterCode::UTF16ToU32(u32* dest, const wchar_t* src) {
            u32 result = 0;
            int len = 0;

            const wchar_t* digit = src;
            while (*digit != 0) {
                len++;
                digit++;
            }

            for (int i = 0; i < len; i++) {
                double exp = pow(10.0, len - i - 1);
                wchar_t in = src[i];
                result += (in - L'0') * exp;
            }
            *dest = result;
        }

        void CharacterCode::WiiIdToUTF16(wchar_t* dest, u64 wiiId) {
            swprintf(dest, 17, L"%016llu", wiiId);
        }

        void CharacterCode::changeEndian(wchar_t* dest, int count) {
            char* bytes = (char*)dest;
            for (int i = 0; i < count; i++) {
                int off = i * 2;
                char firstByte = bytes[off];
                char secondByte = bytes[off + 1];
                bytes[off] = secondByte;
                bytes[off + 1] = firstByte;
            }
        }

        void timer::set_msec(int ms) {
            tick = OSGetTime() + OSMillisecondsToTicks((s64)ms);
        }

        bool timer::operator()() {
            return OSGetTime() > tick;
        }

        namespace layout {
            void set_string(nw4r::lyt::Pane* pane, const wchar_t* str) {
                nw4r::lyt::Pane* pPane = pane;
                const wchar_t* pStr = str;
                const nw4r::ut::detail::RuntimeTypeInfo* textBoxType = &nw4r::lyt::TextBox::typeInfo;
                const nw4r::ut::detail::RuntimeTypeInfo* info = pPane->GetRuntimeTypeInfo();
                bool found;

                while (info != NULL) {
                    if (info == textBoxType) {
                        found = true;
                        // MWCC needs this exit shape to preserve the type-check instruction order.
                        goto check;
                    }
                    info = info->mParentTypeInfo;
                }
                found = false;

            check:
                pPane = found ? pPane : NULL;
                ((nw4r::lyt::TextBox*)pPane)->AllocStringBuffer(wcslen(pStr));
                ((nw4r::lyt::TextBox*)pPane)->SetString(pStr, 0);
            }

            void set_texture(nw4r::lyt::Material* dest, const GXTexObj& texObj) {
                dest->SetTexture(GX_TEXMAP0, texObj);
            }

            void set_texture(nw4r::lyt::Material* dest, const nw4r::lyt::Material* src, u8 texMap) {
                GXTexObj texObj;
                src->GetTexture(&texObj, texMap);
                dest->SetTexture(GX_TEXMAP0, texObj);
            }
        }  // namespace layout

        math::VEC2 get_cursor_pos(const math::VEC2& basePos) {
            nw4r::ut::Rect rect16x9;
            System::getProjectionRect(&rect16x9);
            nw4r::ut::Rect rect4x3;
            System::getProjectionRect4x3(&rect4x3);

            math::VEC2 result;
            math::VEC2 pos;
            math::VEC2* posPtr = __ct__Q33ipl4math4VEC2Fff(&pos, basePos.x * (rect4x3.GetWidth() / rect16x9.GetWidth()), -basePos.y);
            // MWCC needs the original integer-copy order when returning this vector.
            u32* resultWords = (u32*)&result;
            const u32* positionWords = (const u32*)posPtr;
            resultWords[1] = positionWords[1];
            resultWords[0] = positionWords[0];
            return result;
        }

        tpl_validity::tpl_validity(TPLPalette* pal, u32 palSize) {
            mpPalette = pal;
            mpTexDesc = NULL;
            mpTexHeader = NULL;
            mpClutHeader = NULL;
            mpTexData = NULL;
            mpClutData = NULL;

            if (pal == NULL) {
                return;
            }
            if (pal->versionNumber != TPL_MAGIC) {
                return;
            }
#ifdef RVL_WEB
            // Pointers are physical addresses on the web: an unbound TPL holds
            // offsets, which are smaller than the palette's own address.
            if ((u32)pal->descriptorArray < (u32)pal) {
#else
            if ((u32)pal->descriptorArray < 0x80000000) {
#endif
                if ((u32)pal->descriptorArray >= palSize) {
                    return;
                }

                mpTexDesc = (TPLDescriptor*)((u32)pal->descriptorArray + (u32)pal);
                if (mpTexDesc) {
                    if (mpTexDesc->textureHeader) {
                        if ((u32)mpTexDesc->textureHeader < palSize) {
                            mpTexHeader = (TPLHeader*)((u32)mpTexDesc->textureHeader + (u32)pal);
                            if (mpTexHeader) {
                                u32 dataOfs = (u32)mpTexHeader->data;
                                if (dataOfs < palSize) {
                                    mpTexData = (char*)(dataOfs + (u32)pal);
                                }
                            }
                        }
                    }
                }

                if (mpTexDesc) {
                    if (mpTexDesc->CLUTHeader) {
                        if ((u32)mpTexDesc->CLUTHeader < palSize) {
                            mpClutHeader = (TPLClutHeader*)((u32)mpTexDesc->CLUTHeader + (u32)mpPalette);
                            if (mpClutHeader) {
                                u32 dataOfs = (u32)mpClutHeader->data;
                                if (dataOfs < palSize) {
                                    mpClutData = (char*)(dataOfs + (u32)mpPalette);
                                }
                            }
                        }
                    }
                }
            } else {
                mpTexDesc = pal->descriptorArray;
                if (mpTexDesc == NULL) {
                    return;
                }
                mpTexHeader = mpTexDesc->textureHeader;
                mpClutHeader = mpTexDesc->CLUTHeader;
                if (mpTexHeader != NULL) {
                    mpTexData = mpTexHeader->data;
                }
                if (mpClutHeader == NULL) {
                    return;
                }
                mpClutData = mpClutHeader->data;
            }
        }

        BOOL tpl_validity::is_valid_cmn() {
            u32 pal = (u32)mpPalette;
            BOOL r = TRUE;

            if (mpPalette == NULL || mpPalette->versionNumber != TPL_MAGIC) {
                r = FALSE;
            } else {
                u32 end = (u32)mpPalette + 0x100000;
                if (mpTexDesc == NULL || mpTexHeader == NULL) {
                    r = FALSE;
                } else if ((u32)mpTexData & 31) {
                    r = FALSE;
                } else if ((u32)mpClutData & 31) {
                    r = FALSE;
                } else if ((u32)mpTexData < pal || (u32)mpTexData > end) {
                    r = FALSE;
                } else if (mpClutHeader != NULL && ((u32)mpClutData < pal || (u32)mpClutData > end)) {
                    r = FALSE;
                } else if (mpTexHeader->height == 0 || mpTexHeader->width == 0) {
                    r = FALSE;
                } else if (mpClutHeader == NULL && mpTexHeader->format != GX_TF_I4 && mpTexHeader->format != GX_TF_I8 &&
                           mpTexHeader->format != GX_TF_IA4 && mpTexHeader->format != GX_TF_IA8 && mpTexHeader->format != GX_TF_RGB565 &&
                           mpTexHeader->format != GX_TF_RGB5A3 && mpTexHeader->format != GX_TF_CMPR && mpTexHeader->format != GX_TF_RGBA8) {
                    r = FALSE;
                } else if (mpClutHeader != NULL && mpTexHeader->format != GX_TF_C4 && mpTexHeader->format != GX_TF_C8) {
                    r = FALSE;
                } else if (mpClutHeader != NULL && mpClutHeader->format != GX_TL_IA8 && mpClutHeader->format != GX_TL_RGB565 && mpClutHeader->format != GX_TL_RGB5A3) {
                    r = FALSE;
                } else if (mpClutHeader != NULL && mpClutHeader->numEntries > 0x4000) {
                    r = FALSE;
                } else if (mpTexHeader->wrapS != GX_CLAMP && mpTexHeader->wrapS != GX_REPEAT && mpTexHeader->wrapS != GX_MIRROR) {
                    r = FALSE;
                } else if (mpTexHeader->wrapT != GX_CLAMP && mpTexHeader->wrapT != GX_REPEAT && mpTexHeader->wrapT != GX_MIRROR) {
                    r = FALSE;
                } else if (mpTexHeader->minLOD != 0 || mpTexHeader->maxLOD != 0) {
                    r = FALSE;
                } else if (mpTexHeader->minFilter != GX_NEAR && mpTexHeader->minFilter != GX_LINEAR && mpTexHeader->minFilter != GX_NEAR_MIP_NEAR &&
                           mpTexHeader->minFilter != GX_LIN_MIP_NEAR && mpTexHeader->minFilter != GX_NEAR_MIP_LIN &&
                           mpTexHeader->minFilter != GX_LIN_MIP_LIN) {
                    r = FALSE;
                } else if (mpTexHeader->magFilter != GX_NEAR && mpTexHeader->magFilter != GX_LINEAR) {
                    r = FALSE;
                } else if (mpTexHeader->LODBias < -100.f || mpTexHeader->LODBias > 100.f) {
                    r = FALSE;
                } else if (mpTexHeader->edgeLODEnable != GX_FALSE && mpTexHeader->edgeLODEnable != GX_TRUE) {
                    r = FALSE;
                }
            }
            return r;
        }
        BOOL tpl_validity::is_valid() {
            return is_valid_cmn();
        }

        BOOL tpl_validity::is_valid_for_ltx() {
            BOOL r = is_valid_cmn();
            if (r) {
                if (mpPalette->numDescriptors != 1) {
                    r = FALSE;
                } else if (mpTexHeader->height > 0x100) {
                    r = FALSE;
                } else if (mpTexHeader->width > 0x200) {
                    r = FALSE;
                } else if (mpClutHeader == NULL && mpTexHeader->format == GX_TF_RGBA8) {
                    r = FALSE;
                }
            }
            return r;
        }

        void* memcpy_s(void* dest, u32 destSize, const void* src, u32 srcSize) {
            return memcpy(dest, src, destSize < srcSize ? destSize : srcSize);
        }

        namespace wpad {
            BOOL isIncreaseConnectedWpad(u32 prevMask, u32 nextMask) {
                for (u32 i = WPAD_MAX_CONTROLLERS; i > 0; i--) {
                    if (!(prevMask & 1)) {
                        if ((nextMask & 1) == 1) {
                            return TRUE;
                        }
                    }
                    prevMask >>= 1;
                    nextMask >>= 1;
                }
                return FALSE;
            }

            u32 getWpadConnectedMask() {
                u32 mask = 0;
                u32 info[1];
                for (int i = 0; i < WPAD_MAX_CONTROLLERS; i++) {
                    if (WPADProbe(i, info) == WPAD_ERR_OK && info[0] != WPAD_DEV_NOT_FOUND) {
                        mask |= 1 << i;
                    }
                }
                return mask;
            }
        }  // namespace wpad

    }  // namespace utility

#ifndef NON_MATCHING
    // MWCC needs this weak factory to preserve the original object text layout.
    __declspec(weak) math::VEC2 ForceCTORWeak() {
        return math::VEC2(NULL, NULL);
    }
#endif  // NON_MATCHING

}  // namespace ipl
