// Interpolator destructors. Their declarations depend on per-file macros of
// the decompilation (iplBoardObject.cpp / iplGCWindow.cpp), where the original
// definitions were emitted.

#define IPL_BOARD_OBJECT_INLINE_INTERPOLATION_DTORS
#define IPL_GC_WINDOW_CPP
#include "math/iplMathTypes.h"
#include "math/iplInterporation.h"

namespace ipl {
    namespace math {
        template <>
        Interporation<VEC2>::~Interporation() {}
        template <>
        LinearIntp<VEC2>::~LinearIntp() {}
        LinearIntp<VEC3>::~LinearIntp() {}
    }  // namespace math
}  // namespace ipl
