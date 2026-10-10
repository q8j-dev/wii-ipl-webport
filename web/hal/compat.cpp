// Definitions the web build needs that the original binary got elsewhere:
// functions that were pure virtual (or dead-stripped) in the original NW4R
// build, explicit specializations whose only definitions were dropped for
// clang, and entry points the decompilation reaches through MWCC-mangled
// extern "C" names.

#include <nw4r/lyt/animation.h>
#include <nw4r/lyt/textBox.h>
#include <nw4r/snd/WsdTrack.h>
#include <nw4r/ut/FileStream.h>


namespace nw4r {
    namespace lyt {
        void AnimTransform::SetResource(const res::AnimationBlock*, ResourceAccessor*) {}
        void AnimTransform::Bind(Pane*, bool) {}
        void AnimTransform::Bind(Material*) {}
        void AnimTransform::Animate(u32, Pane*) {}
        void AnimTransform::Animate(u32, Material*) {}
    }  // namespace lyt

    namespace ut {
        void FileStream::Seek(s32, u32) {}
    }  // namespace ut

    namespace snd {
        namespace detail {
            bool WsdTrack::WsdCallback::GetWaveSoundData(WaveSoundInfo*, WaveSoundNoteInfo*, WaveData*, const void*, int,
                                                         int, u32) const {
                return false;
            }
        }  // namespace detail
    }  // namespace snd
}  // namespace nw4r

extern "C" nw4r::ut::Color GetTextColor__Q34nw4r3lyt7TextBoxCFUl(const nw4r::lyt::TextBox* box, u32 type) {
    return box->GetTextColor(type);
}

// Debug-only objects that are referenced but never used.
extern const nw4r::ut::detail::RuntimeTypeInfo
    __web_mcs_file_stream_type_info __asm__("_ZN4nw4r3snd15McsSoundArchive13McsFileStream8typeInfoE");
const nw4r::ut::detail::RuntimeTypeInfo __web_mcs_file_stream_type_info(&nw4r::ut::FileStream::typeInfo);

extern const char __web_www_surface_zero[] __asm__("_ZN7ext_ead3www5print20IPL_WWW_SURFACE_ZEROE");
const char __web_www_surface_zero[] = "";
