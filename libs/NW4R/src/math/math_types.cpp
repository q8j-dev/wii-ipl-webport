#include <nw4r/math.h>

namespace nw4r {
    namespace math {
        void MTX44Identity(register MTX44* pMtx) {
            register f32 a, b, c;
            a = 0.0f;
            b = 1.0f;
            // clang-format off
            #ifdef __MWERKS__
                asm {
                    psq_st a, 8(pMtx), 0, 0
                    ps_merge01 c, a, b
                    ps_merge10 b, b, a
                    psq_st a, 24(pMtx), 0, 0
                    psq_st a, 32(pMtx), 0, 0
                    psq_st c, 16(pMtx), 0, 0
                    psq_st b, 0(pMtx), 0, 0
                    psq_st b, 40(pMtx), 0, 0
                    psq_st a, 48(pMtx), 0, 0
                    psq_st c, 56(pMtx), 0, 0
                }
            #elif defined(RVL_WEB)
                for (int i = 0; i < 4; i++)
                    for (int j = 0; j < 4; j++)
                        pMtx->m[i][j] = i == j ? 1.0f : 0.0f;
            #endif // __MWERKS__
            // clang-format on
        }

        VEC4* VEC4Transform(VEC4* pOut, const MTX44* pM, const VEC4* pV) {
            VEC4 transformed;
            transformed.x = pM->_00 * pV->x + pM->_01 * pV->y + pM->_02 * pV->z + pM->_03 * pV->w;
            transformed.y = pM->_10 * pV->x + pM->_11 * pV->y + pM->_12 * pV->z + pM->_13 * pV->w;
            transformed.z = pM->_20 * pV->x + pM->_21 * pV->y + pM->_22 * pV->z + pM->_23 * pV->w;
            transformed.w = pM->_30 * pV->x + pM->_31 * pV->y + pM->_32 * pV->z + pM->_33 * pV->w;

            pOut->x = transformed.x;
            pOut->y = transformed.y;
            pOut->z = transformed.z;
            pOut->w = transformed.w;
            return pOut;
        }
    }  // namespace math
}  // namespace nw4r
