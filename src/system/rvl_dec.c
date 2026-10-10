#include "system/rvl_dec.h"

#define SZS_SIGNATURE 0x0059617A /* 'Yaz */
#define ASH_SIGNATURE 0x00415348 /* 'ASH' */
#define ASR_SIGNATURE 0x00415352 /* 'ASR' */

static u8 work[0x9008];

int Rvl_decode(u8* out, u8* in) {
#if defined(TARGET_RVL) && !defined(RVL_WEB)
    // Original ASM Implementation

    register u8* dataIn = in;

    // clang-format off
#ifdef __MWERKS__
    asm volatile {
        // Get magic
        lwz     r5, 0(dataIn)
        lis     r6, SZS_SIGNATURE@h
        srwi    r5, r5, 8

        // Check is it's SZS with the magic 'Yaz'
        ori     r6, r6, SZS_SIGNATURE@l
        cmpl    r5, r6
        beq     Rvl_decode_szs

        // Check is it's ASH with the magic 'ASH'
        lis     r6, ASH_SIGNATURE@h
        ori     r6, r6, ASH_SIGNATURE@l
        cmpl    r5, r6
        beq     Rvl_decode_ash

        // Check is it's ASR with the magic 'ASR'
        lis     r6, ASR_SIGNATURE@h
        ori     r6, r6, ASR_SIGNATURE@l
        cmpl    r5, r6
        beq     Rvl_decode_asr
    };
#endif // __MWERKS__
    // clang-format on

    // Otherwise
    return 0;
#else
    // Custom C Implementation

    // Get magic
    u32 magic = Rvl_decode_get_magic(in);
    switch (magic) {
        // Check is it's SZS with the magic 'Yaz'
        case SZS_SIGNATURE: {
            return Rvl_decode_szs(out, in);
        }
        // Check is it's ASH with the magic 'ASH'
        case ASH_SIGNATURE: {
            return Rvl_decode_ash(out, in);
        }
        // Check is it's ASR with the magic 'ASR'
        case ASR_SIGNATURE: {
            return Rvl_decode_asr(out, in);
        }
        // Otherwise
        default: {
            return 0;
        }
    }
#endif  // TARGET_RVL
}

#ifdef __MWERKS__
asm int Rvl_decode_szs(register u8* out, register u8* in) {
    nofralloc
    lwz r5, 0x4(r4)
    li r6, 0x0
    or r0, r5, r5
    addi r4, r4, 0x10
Rvl_decode_szs_bit_loop:
    rlwinm. r6, r6, 0x1f, 0x1, 0x1f
    bne Rvl_decode_szs_bit_loop_body
    lbz r7, 0x0(r4)
    li r6, 0x80
    addi r4, r4, 0x1
Rvl_decode_szs_bit_loop_body:
    and. r8, r6, r7
    lbz r8, 0x0(r4)
    beq Rvl_decode_szs_backref
    addi r4, r4, 0x1
    subic. r5, r5, 0x1
    stb r8, 0x0(r3)
    addi r3, r3, 0x1
    bne Rvl_decode_szs_bit_loop
    b Rvl_decode_szs_done
Rvl_decode_szs_backref:
    lbz r9, 0x1(r4)
    rlwinm. r10, r8, 0x1c, 0x4, 0x1f
    addi r4, r4, 0x2
    bne Rvl_decode_szs_copy_length
    lbz r10, 0x0(r4)
    addi r4, r4, 0x1
    addi r10, r10, 0x10
Rvl_decode_szs_copy_length:
    rlwimi r9, r8, 0x8, 0x14, 0x17
    subf r5, r10, r5
    subf r8, r9, r3
    lbz r9, -0x1(r8)
    subi r5, r5, 0x2
    stb r9, 0x0(r3)
    lbz r9, 0x0(r8)
    stb r9, 0x1(r3)
    addi r8, r8, 0x2
    rlwinm. r9, r10, 0x1d, 0x3, 0x1f
    addi r3, r3, 0x2
    beq Rvl_decode_szs_tail
    mtspr CTR, r9
Rvl_decode_szs_copy_loop:
    lbz r9, -0x1(r8)
    stb r9, 0x0(r3)
    lbz r9, 0x0(r8)
    stb r9, 0x1(r3)
    lbz r9, 0x1(r8)
    stb r9, 0x2(r3)
    lbz r9, 0x2(r8)
    stb r9, 0x3(r3)
    lbz r9, 0x3(r8)
    stb r9, 0x4(r3)
    lbz r9, 0x4(r8)
    stb r9, 0x5(r3)
    lbz r9, 0x5(r8)
    stb r9, 0x6(r3)
    lbz r9, 0x6(r8)
    stb r9, 0x7(r3)
    addi r8, r8, 0x8
    addi r3, r3, 0x8
    bdnz Rvl_decode_szs_copy_loop
    andi. r10, r10, 0x7
    beq Rvl_decode_szs_done_copy
Rvl_decode_szs_tail:
    mtspr CTR, r10
Rvl_decode_szs_tail_loop:
    lbz r9, -0x1(r8)
    addi r8, r8, 0x1
    stb r9, 0x0(r3)
    addi r3, r3, 0x1
    bdnz Rvl_decode_szs_tail_loop
Rvl_decode_szs_done_copy:
    cmpwi r5, 0x0
    bgt Rvl_decode_szs_bit_loop
Rvl_decode_szs_done:
    or r3, r0, r0
    blr
}
#else
int Rvl_decode_szs(u8* out, u8* in) {
    register u8* copySrc;
    register u8 hdrByte;
    register u8 extLenByte;
    register u8* inPtr;
    register int decodedSize;
    register int remaining;
    register u32 bitMask;
    register u32 copyLen;
    register u32 blockCount;
    register u32 flags;
    decodedSize = *(int*)(in + 4);
    bitMask = 0;
    in += 0x10;
    remaining = decodedSize;
    do {
next:
        while (1) {
            bitMask >>= 1;
            inPtr = in;
            if (bitMask == 0) {
                flags = *in;
                bitMask = 0x80;
                inPtr = in + 1;
            }
            hdrByte = *inPtr;
            if ((bitMask & flags) == 0) {
                break;
            }
            in = inPtr + 1;
            remaining--;
            *out = hdrByte;
            out++;
            if (remaining != 0) {
                goto next;
            }
            goto done;
        }
        copyLen = hdrByte >> 4;
        in = inPtr + 2;
        if (copyLen == 0) {
            extLenByte = *in;
            in = inPtr + 3;
            copyLen = extLenByte + 0x10;
        }
        inPtr = out - (((hdrByte & 0xf) << 8) | inPtr[1]);
        remaining = (remaining - copyLen) - 2;
        *out = inPtr[-1];
        out[1] = *inPtr;
        inPtr += 2;
        blockCount = copyLen >> 3;
        out += 2;
        if (blockCount != 0) {
            do {
                *out = inPtr[-1];
                out[1] = *inPtr;
                out[2] = inPtr[1];
                out[3] = inPtr[2];
                out[4] = inPtr[3];
                out[5] = inPtr[4];
                out[6] = inPtr[5];
                out[7] = inPtr[6];
                inPtr += 8;
                out += 8;
                blockCount--;
            } while (blockCount != 0);
        }
        for (copyLen &= 7; copyLen != 0; copyLen--) {
            copySrc = inPtr - 1;
            inPtr++;
            *out = *copySrc;
            out++;
        }
    } while (0 < remaining);
done:
    return decodedSize;
}
#endif
#ifdef __MWERKS__
asm int Rvl_decode_ash(register u8* out, register u8* in) {
    nofralloc
    stwu r1, -0x40(r1)
    stmw r21, 0x10(r1)
    lwz r5, 0x4(r4)
    clrlwi r5, r5, 8
    li r24, 0x10
    lwz r28, 0x8(r4)
    li r25, 0x0
    li r29, 0x0
    lwz r26, 0xc(r4)
    lwzx r30, r28, r4
    addi r28, r28, 0x4
    lis r8, work@h
    ori r8, r8, work@l
    addi r9, r8, 0x7fe
    addi r10, r9, 0x7fe
    addi r11, r10, 0x1ffe
    addi r31, r11, 0x1ffe
    li r23, 0x200
    li r22, 0x200
    li r27, 0x0
L_Rvl_decode_ash_primary_bit:
    cmpwi r25, 0x1f
    bne L_Rvl_decode_ash_primary_shift
    srwi. r0, r26, 31
    lwzx r26, r4, r24
    li r25, 0x0
    addi r24, r24, 0x4
    b L_Rvl_decode_ash_primary_bit_test
L_Rvl_decode_ash_primary_shift:
    srwi. r0, r26, 31
    addi r25, r25, 0x1
    slwi r26, r26, 1
L_Rvl_decode_ash_primary_bit_test:
    beq L_Rvl_decode_ash_primary_symbol
    ori r0, r23, 0x8000
    sth r0, 0x0(r31)
    ori r0, r23, 0x4000
    sth r0, 0x2(r31)
    addi r31, r31, 0x4
    addi r27, r27, 0x2
    addi r23, r23, 0x1
    addi r22, r22, 0x1
    b L_Rvl_decode_ash_primary_bit
L_Rvl_decode_ash_primary_symbol:
    li r12, 0x9
    add r21, r25, r12
    cmpwi r21, 0x20
    bgt L_Rvl_decode_ash_primary_cross
    subfic r21, r12, 0x20
    srw r6, r26, r21
    beq L_Rvl_decode_ash_primary_reload
    slw r26, r26, r12
    add r25, r25, r12
    b L_Rvl_decode_ash_primary_tree
L_Rvl_decode_ash_primary_reload:
    lwzx r26, r4, r24
    li r25, 0x0
    addi r24, r24, 0x4
    b L_Rvl_decode_ash_primary_tree
L_Rvl_decode_ash_primary_cross:
    subfic r0, r12, 0x20
    srw r6, r26, r0
    lwzx r26, r4, r24
    subfic r0, r21, 0x40
    addi r24, r24, 0x4
    srw r0, r26, r0
    or r6, r6, r0
    subi r25, r21, 0x20
    slw r26, r26, r25
L_Rvl_decode_ash_primary_tree:
    lhzu r12, -0x2(r31)
    subi r27, r27, 0x1
    andi. r0, r12, 0x8000
    clrlslwi r12, r12, 18, 1
    beq L_Rvl_decode_ash_primary_leaf
    sthx r6, r9, r12
    extrwi r6, r12, 14, 17
    cmpwi r27, 0x0
    bne L_Rvl_decode_ash_primary_tree
    b L_Rvl_decode_ash_secondary_init
L_Rvl_decode_ash_primary_leaf:
    sthx r6, r8, r12
    mr r23, r22
    b L_Rvl_decode_ash_primary_bit
L_Rvl_decode_ash_secondary_init:
    li r23, 0x800
    li r22, 0x800
L_Rvl_decode_ash_secondary_bit:
    cmpwi r29, 0x1f
    bne L_Rvl_decode_ash_secondary_shift
    srwi. r0, r30, 31
    lwzx r30, r4, r28
    li r29, 0x0
    addi r28, r28, 0x4
    b L_Rvl_decode_ash_secondary_bit_test
L_Rvl_decode_ash_secondary_shift:
    srwi. r0, r30, 31
    addi r29, r29, 0x1
    slwi r30, r30, 1
L_Rvl_decode_ash_secondary_bit_test:
    beq L_Rvl_decode_ash_secondary_symbol
    ori r0, r23, 0x8000
    sth r0, 0x0(r31)
    ori r0, r23, 0x4000
    sth r0, 0x2(r31)
    addi r31, r31, 0x4
    addi r27, r27, 0x2
    addi r23, r23, 0x1
    addi r22, r22, 0x1
    b L_Rvl_decode_ash_secondary_bit
L_Rvl_decode_ash_secondary_symbol:
    li r12, 0xb
    add r21, r29, r12
    cmpwi r21, 0x20
    bgt L_Rvl_decode_ash_secondary_cross
    subfic r21, r12, 0x20
    srw r7, r30, r21
    beq L_Rvl_decode_ash_secondary_reload
    slw r30, r30, r12
    add r29, r29, r12
    b L_Rvl_decode_ash_secondary_tree
L_Rvl_decode_ash_secondary_reload:
    lwzx r30, r4, r28
    li r29, 0x0
    addi r28, r28, 0x4
    b L_Rvl_decode_ash_secondary_tree
L_Rvl_decode_ash_secondary_cross:
    subfic r0, r12, 0x20
    srw r7, r30, r0
    lwzx r30, r4, r28
    subfic r0, r21, 0x40
    addi r28, r28, 0x4
    srw r0, r30, r0
    or r7, r7, r0
    subi r29, r21, 0x20
    slw r30, r30, r29
L_Rvl_decode_ash_secondary_tree:
    lhzu r12, -0x2(r31)
    subi r27, r27, 0x1
    andi. r0, r12, 0x8000
    clrlslwi r12, r12, 18, 1
    beq L_Rvl_decode_ash_secondary_leaf
    sthx r7, r11, r12
    extrwi r7, r12, 14, 17
    cmpwi r27, 0x0
    bne L_Rvl_decode_ash_secondary_tree
    b L_Rvl_decode_ash_decode_init
L_Rvl_decode_ash_secondary_leaf:
    sthx r7, r10, r12
    mr r23, r22
    b L_Rvl_decode_ash_secondary_bit
L_Rvl_decode_ash_decode_init:
    mr r0, r5
L_Rvl_decode_ash_decode_symbol:
    mr r12, r6
L_Rvl_decode_ash_decode_primary:
    cmpwi r12, 0x200
    blt L_Rvl_decode_ash_decode_literal
    cmpwi r25, 0x1f
    bne L_Rvl_decode_ash_decode_primary_shift
    srwi. r31, r26, 31
    lwzx r26, r4, r24
    addi r24, r24, 0x4
    li r25, 0x0
    b L_Rvl_decode_ash_decode_primary_bit_test
L_Rvl_decode_ash_decode_primary_shift:
    srwi. r31, r26, 31
    addi r25, r25, 0x1
    slwi r26, r26, 1
L_Rvl_decode_ash_decode_primary_bit_test:
    slwi r27, r12, 1
    bne L_Rvl_decode_ash_decode_primary_table
    lhzx r12, r8, r27
    b L_Rvl_decode_ash_decode_primary
L_Rvl_decode_ash_decode_primary_table:
    lhzx r12, r9, r27
    b L_Rvl_decode_ash_decode_primary
L_Rvl_decode_ash_decode_literal:
    cmpwi r12, 0x100
    bge L_Rvl_decode_ash_decode_copy
    stb r12, 0x0(r3)
    addi r3, r3, 0x1
    subi r5, r5, 0x1
    cmpwi r5, 0x0
    bne L_Rvl_decode_ash_decode_symbol
    b L_Rvl_decode_ash_return
L_Rvl_decode_ash_decode_copy:
    mr r23, r7
L_Rvl_decode_ash_copy_distance:
    cmpwi r23, 0x800
    blt L_Rvl_decode_ash_copy_start
    cmpwi r29, 0x1f
    bne L_Rvl_decode_ash_copy_bit_shift
    srwi. r31, r30, 31
    lwzx r30, r4, r28
    addi r28, r28, 0x4
    li r29, 0x0
    b L_Rvl_decode_ash_copy_bit_test
L_Rvl_decode_ash_copy_bit_shift:
    srwi. r31, r30, 31
    addi r29, r29, 0x1
    slwi r30, r30, 1
L_Rvl_decode_ash_copy_bit_test:
    slwi r27, r23, 1
    bne L_Rvl_decode_ash_copy_secondary_table
    lhzx r23, r10, r27
    b L_Rvl_decode_ash_copy_distance
L_Rvl_decode_ash_copy_secondary_table:
    lhzx r23, r11, r27
    b L_Rvl_decode_ash_copy_distance
L_Rvl_decode_ash_copy_start:
    subi r12, r12, 0xfd
    subf r23, r23, r3
    subf r5, r12, r5
    srwi. r31, r12, 3
    beq L_Rvl_decode_ash_copy_tail
    mtctr r31
L_Rvl_decode_ash_copy_block:
    lbz r31, -0x1(r23)
    stb r31, 0x0(r3)
    lbz r31, 0x0(r23)
    stb r31, 0x1(r3)
    lbz r31, 0x1(r23)
    stb r31, 0x2(r3)
    lbz r31, 0x2(r23)
    stb r31, 0x3(r3)
    lbz r31, 0x3(r23)
    stb r31, 0x4(r3)
    lbz r31, 0x4(r23)
    stb r31, 0x5(r3)
    lbz r31, 0x5(r23)
    stb r31, 0x6(r3)
    lbz r31, 0x6(r23)
    stb r31, 0x7(r3)
    addi r23, r23, 0x8
    addi r3, r3, 0x8
    bdnz L_Rvl_decode_ash_copy_block
    andi. r12, r12, 0x7
    beq L_Rvl_decode_ash_copy_done
L_Rvl_decode_ash_copy_tail:
    mtctr r12
L_Rvl_decode_ash_copy_tail_loop:
    lbz r31, -0x1(r23)
    addi r23, r23, 0x1
    stb r31, 0x0(r3)
    addi r3, r3, 0x1
    bdnz L_Rvl_decode_ash_copy_tail_loop
L_Rvl_decode_ash_copy_done:
    cmpwi r5, 0x0
    bne L_Rvl_decode_ash_decode_symbol
L_Rvl_decode_ash_return:
    mr r3, r0
    lmw r21, 0x10(r1)
    addi r1, r1, 0x40
    blr
}
#else
int Rvl_decode_ash(u8* out, u8* in) {
    u8* dst;
    u32* inWordPtr;
    u32 decodedSize;
    u32 literalSymbol;
    u32 distanceSymbol;
    u32 nodeVal;
    int needBits;
    u16 nodeCount;
    u16 nextNode;
    u8* copySource;
    int literalWordOffset;
    u32 literalBitPosition;
    u32 shifted;
    u32 literalBitWord;
    u32 decodeBitsOrCount;
    int stackDepth;
    int distanceWordOffset;
    u32 distanceBitPosition;
    u32 distanceBitWord;
    u32 nextDistanceBits;
    u16* stackPtr;

    decodedSize = *(u32*)(in + 4) & 0xffffff;
    literalWordOffset = 0x10;
    literalBitPosition = 0;
    distanceBitPosition = 0;
    distanceBitWord = *(u32*)(*(int*)(in + 8) + in);
    distanceWordOffset = *(int*)(in + 8) + 4;
    stackPtr = (u16*)&work[0x4ff8];
    nodeCount = 0x200;
    stackDepth = 0;
    literalBitWord = *(u32*)(in + 0xc);
    nextNode = 0x200;
    do {
        while (1) {
            if (literalBitPosition == 0x1f) {
                shifted = *(u32*)(in + literalWordOffset);
                literalBitPosition = 0;
                literalWordOffset = literalWordOffset + 4;
            } else {
                literalBitPosition = literalBitPosition + 1;
                shifted = literalBitWord << 1;
            }
            if (-1 < (int)literalBitWord) {
                break;
            }
            *stackPtr = nextNode | 0x8000;
            stackPtr[1] = nextNode | 0x4000;
            stackPtr = stackPtr + 2;
            stackDepth = stackDepth + 2;
            nodeCount = nodeCount + 1;
            literalBitWord = shifted;
            nextNode = nextNode + 1;
        }
        needBits = literalBitPosition + 9;
        if (needBits < 0x21) {
            literalSymbol = shifted >> 0x17;
            if (needBits == 0x20) {
                literalBitWord = *(u32*)(in + literalWordOffset);
                literalBitPosition = 0;
                literalWordOffset = literalWordOffset + 4;
            } else {
                literalBitWord = shifted << 9;
                literalBitPosition = literalBitPosition + 9;
            }
        } else {
            inWordPtr = (u32*)(in + literalWordOffset);
            literalWordOffset = literalWordOffset + 4;
            literalSymbol = shifted >> 0x17 | *inWordPtr >> (0x40U - needBits & 0x3f);
            literalBitPosition = literalBitPosition - 0x17;
            literalBitWord = *inWordPtr << (literalBitPosition & 0x3f);
        }
        while (1) {
            stackPtr = stackPtr - 1;
            nextNode = *stackPtr;
            stackDepth = stackDepth - 1;
            needBits = (nextNode & 0x3fff) * 2;
            if ((nextNode & 0x8000) == 0) {
                break;
            }
            *(u16*)(&work[0x7fe] + needBits) = (u16)literalSymbol;
            literalSymbol = nextNode & 0x3fff;
            if (stackDepth == 0) {
                nodeCount = 0x800;
                stackDepth = 0;
                nextNode = 0x800;
                do {
                    while (1) {
                        if (distanceBitPosition == 0x1f) {
                            shifted = *(u32*)(in + distanceWordOffset);
                            distanceBitPosition = 0;
                            distanceWordOffset = distanceWordOffset + 4;
                        } else {
                            distanceBitPosition = distanceBitPosition + 1;
                            shifted = distanceBitWord << 1;
                        }
                        if (-1 < (int)distanceBitWord) {
                            break;
                        }
                        *stackPtr = nextNode | 0x8000;
                        stackPtr[1] = nextNode | 0x4000;
                        stackPtr = stackPtr + 2;
                        stackDepth = stackDepth + 2;
                        nodeCount = nodeCount + 1;
                        distanceBitWord = shifted;
                        nextNode = nextNode + 1;
                    }
                    needBits = distanceBitPosition + 0xb;
                    if (needBits < 0x21) {
                        distanceSymbol = shifted >> 0x15;
                        if (needBits == 0x20) {
                            distanceBitWord = *(u32*)(in + distanceWordOffset);
                            distanceBitPosition = 0;
                            distanceWordOffset = distanceWordOffset + 4;
                        } else {
                            distanceBitWord = shifted << 0xb;
                            distanceBitPosition = distanceBitPosition + 0xb;
                        }
                    } else {
                        inWordPtr = (u32*)(in + distanceWordOffset);
                        distanceWordOffset = distanceWordOffset + 4;
                        distanceSymbol = shifted >> 0x15 | *inWordPtr >> (0x40U - needBits & 0x3f);
                        distanceBitPosition = distanceBitPosition - 0x15;
                        distanceBitWord = *inWordPtr << (distanceBitPosition & 0x3f);
                    }
                    while (1) {
                        stackPtr = stackPtr - 1;
                        nextNode = *stackPtr;
                        stackDepth = stackDepth - 1;
                        needBits = (nextNode & 0x3fff) * 2;
                        if ((nextNode & 0x8000) == 0) {
                            break;
                        }
                        *(u16*)(&work[0x2ffa] + needBits) = (u16)distanceSymbol;
                        distanceSymbol = nextNode & 0x3fff;
                        nodeVal = literalSymbol;
                        shifted = decodedSize;
                        if (stackDepth == 0) {
                            do {
                                while (0x1ff < nodeVal) {
                                    if (literalBitPosition == 0x1f) {
                                        decodeBitsOrCount = *(u32*)(in + literalWordOffset);
                                        literalWordOffset = literalWordOffset + 4;
                                        literalBitPosition = 0;
                                    } else {
                                        literalBitPosition = literalBitPosition + 1;
                                        decodeBitsOrCount = literalBitWord << 1;
                                    }
                                    if ((int)literalBitWord < 0) {
                                        nodeVal = *(u16*)(&work[0x7fe] + nodeVal * 2);
                                    } else {
                                        nodeVal = *(u16*)((u8*)work + nodeVal * 2);
                                    }
                                    literalBitWord = decodeBitsOrCount;
                                }
                                decodeBitsOrCount = distanceSymbol;
                                if (nodeVal < 0x100) {
                                    *out = (char)nodeVal;
                                    out = out + 1;
                                    shifted = shifted - 1;
                                } else {
                                    while (0x7ff < decodeBitsOrCount) {
                                        if (distanceBitPosition == 0x1f) {
                                            nextDistanceBits = *(u32*)(in + distanceWordOffset);
                                            distanceWordOffset = distanceWordOffset + 4;
                                            distanceBitPosition = 0;
                                        } else {
                                            distanceBitPosition = distanceBitPosition + 1;
                                            nextDistanceBits = distanceBitWord << 1;
                                        }
                                        if ((int)distanceBitWord < 0) {
                                            decodeBitsOrCount = *(u16*)(&work[0x2ffa] + decodeBitsOrCount * 2);
                                        } else {
                                            decodeBitsOrCount = *(u16*)(&work[0xffc] + decodeBitsOrCount * 2);
                                        }
                                        distanceBitWord = nextDistanceBits;
                                    }
                                    nodeVal = nodeVal - 0xfd;
                                    copySource = out - decodeBitsOrCount;
                                    shifted = shifted - nodeVal;
                                    decodeBitsOrCount = nodeVal >> 3;
                                    if (decodeBitsOrCount == 0) {
                                        goto copy_lit_tail;
                                    }
                                    do {
                                        *out = copySource[-1];
                                        out[1] = *copySource;
                                        out[2] = copySource[1];
                                        out[3] = copySource[2];
                                        out[4] = copySource[3];
                                        out[5] = copySource[4];
                                        out[6] = copySource[5];
                                        out[7] = copySource[6];
                                        copySource = copySource + 8;
                                        out = out + 8;
                                        decodeBitsOrCount = decodeBitsOrCount - 1;
                                    } while (decodeBitsOrCount != 0);
                                    for (nodeVal = nodeVal & 7; nodeVal != 0; nodeVal = nodeVal - 1) {
copy_lit_tail:
                                        dst = copySource - 1;
                                        copySource = copySource + 1;
                                        *out = *dst;
                                        out = out + 1;
                                    }
                                }
                                nodeVal = literalSymbol;
                            } while (shifted != 0);
                            return decodedSize;
                        }
                    }
                    *(u16*)(&work[0xffc] + needBits) = (u16)distanceSymbol;
                    nextNode = nodeCount;
                } while (1);
            }
        }
        *(u16*)((u8*)work + needBits) = (u16)literalSymbol;
        nextNode = nodeCount;
    } while (1);
}
#endif

#ifdef __MWERKS__
asm int Rvl_decode_asr(register u8* out, register u8* in) {
    nofralloc
        stwu r1, -0x40(r1)
        stmw r21, 0x10(r1)
        lwz r5, 4(r4)
        li r12, 0x4000
        rlwinm. r0, r5, 0, 0, 0
        lwz r6, 8(r4)
        clrlwi r5, r5, 8
        li r7, 0
        add r6, r6, r4
        li r31, 0x200
        addi r4, r4, 0xc
        bne Rvl_decode_asr_L013
        li r12, 0x800
    Rvl_decode_asr_L013:
        lis r8, work@h
        ori r8, r8, work@l
        mtctr r31
        addi r9, r8, 0x800
        addi r10, r9, 0x804
        addi r11, r10, 0x4000
        stw r7, 0(r9)
        li r31, 1
        li r26, 1
        stw r7, 0(r11)
    Rvl_decode_asr_L023:
        stwx r31, r8, r7
        addi r7, r7, 4
        stwx r26, r9, r7
        addi r26, r26, 1
        bdnz Rvl_decode_asr_L023
        srwi r30, r12, 2
        li r7, 0
        li r27, 1
        mtctr r30
    Rvl_decode_asr_L032:
        stwx r31, r10, r7
        addi r7, r7, 4
        stwx r27, r11, r7
        addi r27, r27, 1
        bdnz Rvl_decode_asr_L032
        lwz r30, 0(r4)
        li r7, 0
        lwz r31, 0(r6)
        li r28, 0
        li r29, 0
        addi r26, r28, -1
        addi r27, r29, -1
        addi r4, r4, 4
        addi r6, r6, 4
    Rvl_decode_asr_L046:
        lwz r25, 0x800(r9)
        divwu r24, r26, r25
        subf r0, r28, r30
        li r21, 0
        li r22, 0x800
        divwu r23, r0, r24
    Rvl_decode_asr_L052:
        add r0, r21, r22
        rlwinm r25, r0, 0x1f, 1, 0x1d
        lwzx r0, r9, r25
        cmplw r23, r0
        bge Rvl_decode_asr_L059
        mr r22, r25
        b Rvl_decode_asr_L060
    Rvl_decode_asr_L059:
        addi r21, r25, 4
    Rvl_decode_asr_L060:
        cmplw r21, r22
        blt Rvl_decode_asr_L052
        add r22, r9, r25
    Rvl_decode_asr_L063:
        lwz r0, 0(r22)
        cmplw r0, r23
        bgt Rvl_decode_asr_L069
        lwz r0, 4(r22)
        cmplw r23, r0
        blt Rvl_decode_asr_L072
    Rvl_decode_asr_L069:
        addic. r25, r25, -4
        addi r22, r22, -4
        bgt Rvl_decode_asr_L063
    Rvl_decode_asr_L072:
        lwzx r21, r9, r25
        mullw r23, r21, r24
        lwzx r21, r8, r25
        mullw r26, r21, r24
        add r28, r28, r23
        addi r21, r21, 1
        stwx r21, r8, r25
        addi r24, r25, 4
        srwi r25, r25, 2
    Rvl_decode_asr_L081:
        cmpwi r24, 0x800
        bgt Rvl_decode_asr_L088
        lwzx r22, r9, r24
        addi r22, r22, 1
        stwx r22, r9, r24
        addi r24, r24, 4
        b Rvl_decode_asr_L081
    Rvl_decode_asr_L088:
        lwz r21, 0x800(r9)
        rlwinm. r21, r21, 0x10, 0x10, 0x1f
        beq Rvl_decode_asr_L147
        li r0, 0x40
        li r22, 0
        mr r23, r8
        mr r24, r9
        stw r22, 0(r9)
        mtctr r0
    Rvl_decode_asr_L097:
        lwz r21, 0(r23)
        srwi r21, r21, 1
        ori r21, r21, 1
        stw r21, 0(r23)
        add r22, r22, r21
        stw r22, 4(r24)
        lwz r21, 4(r23)
        srwi r21, r21, 1
        ori r21, r21, 1
        stw r21, 4(r23)
        add r22, r22, r21
        stw r22, 8(r24)
        lwz r21, 8(r23)
        srwi r21, r21, 1
        ori r21, r21, 1
        stw r21, 8(r23)
        add r22, r22, r21
        stw r22, 0xc(r24)
        lwz r21, 0xc(r23)
        srwi r21, r21, 1
        ori r21, r21, 1
        stw r21, 0xc(r23)
        add r22, r22, r21
        stw r22, 0x10(r24)
        lwz r21, 0x10(r23)
        srwi r21, r21, 1
        ori r21, r21, 1
        stw r21, 0x10(r23)
        add r22, r22, r21
        stw r22, 0x14(r24)
        lwz r21, 0x14(r23)
        srwi r21, r21, 1
        ori r21, r21, 1
        stw r21, 0x14(r23)
        add r22, r22, r21
        stw r22, 0x18(r24)
        lwz r21, 0x18(r23)
        srwi r21, r21, 1
        ori r21, r21, 1
        stw r21, 0x18(r23)
        add r22, r22, r21
        stw r22, 0x1c(r24)
        lwz r21, 0x1c(r23)
        srwi r21, r21, 1
        ori r21, r21, 1
        stw r21, 0x1c(r23)
        add r22, r22, r21
        stwu r22, 0x20(r24)
        addi r23, r23, 0x20
        bdnz Rvl_decode_asr_L097
    Rvl_decode_asr_L147:
        rlwinm r21, r28, 0, 0, 7
        add r23, r28, r26
        rlwinm r22, r23, 0, 0, 7
        cmplw r21, r22
        bne Rvl_decode_asr_L159
        lbz r24, 0(r4)
        slwi r30, r30, 8
        slwi r28, r28, 8
        slwi r26, r26, 8
        or r30, r30, r24
        addi r4, r4, 1
        b Rvl_decode_asr_L147
    Rvl_decode_asr_L159:
        rlwinm. r0, r26, 0x10, 0x10, 0x1f
        bne Rvl_decode_asr_L171
        clrlwi r21, r28, 0x10
        lbz r24, 0(r4)
        lis r22, 1
        subf r23, r21, r22
        slwi r30, r30, 8
        slwi r28, r28, 8
        slwi r26, r23, 8
        or r30, r30, r24
        addi r4, r4, 1
        b Rvl_decode_asr_L159
    Rvl_decode_asr_L171:
        cmpwi r25, 0x100
        stbx r25, r3, r7
        bge Rvl_decode_asr_L178
        addi r7, r7, 1
        cmplw r7, r5
        blt Rvl_decode_asr_L046
        b Rvl_decode_asr_L341
    Rvl_decode_asr_L178:
        lwzx r23, r11, r12
        divwu r24, r27, r23
        subf r0, r29, r31
        li r21, 0
        mr r22, r12
        divwu r23, r0, r24
    Rvl_decode_asr_L184:
        add r0, r21, r22
        rlwinm r25, r0, 0x1f, 1, 0x1d
        lwzx r0, r11, r25
        cmplw r23, r0
        bge Rvl_decode_asr_L191
        mr r22, r25
        b Rvl_decode_asr_L192
    Rvl_decode_asr_L191:
        addi r21, r25, 4
    Rvl_decode_asr_L192:
        cmplw r21, r22
        blt Rvl_decode_asr_L184
        add r22, r11, r25
    Rvl_decode_asr_L195:
        lwz r0, 0(r22)
        cmplw r0, r23
        bgt Rvl_decode_asr_L201
        lwz r0, 4(r22)
        cmplw r23, r0
        blt Rvl_decode_asr_L204
    Rvl_decode_asr_L201:
        addic. r25, r25, -4
        addi r22, r22, -4
        bgt Rvl_decode_asr_L195
    Rvl_decode_asr_L204:
        lwzx r21, r11, r25
        mullw r23, r21, r24
        lwzx r21, r10, r25
        mullw r27, r21, r24
        add r29, r29, r23
        addi r21, r21, 1
        stwx r21, r10, r25
        addi r24, r25, 4
        srwi r25, r25, 2
        lbzx r22, r3, r7
        subf r21, r25, r7
        addi r22, r22, 3
        add r25, r3, r7
        rlwinm. r0, r22, 0x1d, 3, 0x1f
        add r23, r3, r21
        add r7, r7, r22
        beq Rvl_decode_asr_L243
        mtctr r0
    Rvl_decode_asr_L222:
        lbz r0, -1(r23)
        stb r0, 0(r25)
        lbz r0, 0(r23)
        stb r0, 1(r25)
        lbz r0, 1(r23)
        stb r0, 2(r25)
        lbz r0, 2(r23)
        stb r0, 3(r25)
        lbz r0, 3(r23)
        stb r0, 4(r25)
        lbz r0, 4(r23)
        stb r0, 5(r25)
        lbz r0, 5(r23)
        stb r0, 6(r25)
        lbz r0, 6(r23)
        stb r0, 7(r25)
        addi r23, r23, 8
        addi r25, r25, 8
        bdnz Rvl_decode_asr_L222
        andi. r22, r22, 7
        beq Rvl_decode_asr_L249
    Rvl_decode_asr_L243:
        mtctr r22
    Rvl_decode_asr_L244:
        lbz r0, -1(r23)
        addi r23, r23, 1
        stb r0, 0(r25)
        addi r25, r25, 1
        bdnz Rvl_decode_asr_L244
    Rvl_decode_asr_L249:
        cmplw r24, r12
        bgt Rvl_decode_asr_L256
        lwzx r22, r11, r24
        addi r22, r22, 1
        stwx r22, r11, r24
        addi r24, r24, 4
        b Rvl_decode_asr_L249
    Rvl_decode_asr_L256:
        lwzx r21, r11, r12
        rlwinm. r21, r21, 0x10, 0x10, 0x1f
        lis r25, 1
        beq Rvl_decode_asr_L316
        srwi r0, r12, 5
        li r22, 0
        mr r23, r10
        mr r24, r11
        stw r22, 0(r11)
        mtctr r0
    Rvl_decode_asr_L266:
        lwz r21, 0(r23)
        srwi r21, r21, 1
        ori r21, r21, 1
        stw r21, 0(r23)
        add r22, r22, r21
        stw r22, 4(r24)
        lwz r21, 4(r23)
        srwi r21, r21, 1
        ori r21, r21, 1
        stw r21, 4(r23)
        add r22, r22, r21
        stw r22, 8(r24)
        lwz r21, 8(r23)
        srwi r21, r21, 1
        ori r21, r21, 1
        stw r21, 8(r23)
        add r22, r22, r21
        stw r22, 0xc(r24)
        lwz r21, 0xc(r23)
        srwi r21, r21, 1
        ori r21, r21, 1
        stw r21, 0xc(r23)
        add r22, r22, r21
        stw r22, 0x10(r24)
        lwz r21, 0x10(r23)
        srwi r21, r21, 1
        ori r21, r21, 1
        stw r21, 0x10(r23)
        add r22, r22, r21
        stw r22, 0x14(r24)
        lwz r21, 0x14(r23)
        srwi r21, r21, 1
        ori r21, r21, 1
        stw r21, 0x14(r23)
        add r22, r22, r21
        stw r22, 0x18(r24)
        lwz r21, 0x18(r23)
        srwi r21, r21, 1
        ori r21, r21, 1
        stw r21, 0x18(r23)
        add r22, r22, r21
        stw r22, 0x1c(r24)
        lwz r21, 0x1c(r23)
        srwi r21, r21, 1
        ori r21, r21, 1
        stw r21, 0x1c(r23)
        add r22, r22, r21
        stwu r22, 0x20(r24)
        addi r23, r23, 0x20
        bdnz Rvl_decode_asr_L266
    Rvl_decode_asr_L316:
        rlwinm r21, r29, 0, 0, 7
        add r23, r29, r27
        rlwinm r22, r23, 0, 0, 7
        cmplw r21, r22
        bne Rvl_decode_asr_L328
        lbz r24, 0(r6)
        slwi r31, r31, 8
        slwi r29, r29, 8
        slwi r27, r27, 8
        or r31, r31, r24
        addi r6, r6, 1
        b Rvl_decode_asr_L316
    Rvl_decode_asr_L328:
        rlwinm. r0, r27, 0x10, 0x10, 0x1f
        bne Rvl_decode_asr_L339
        clrlwi r21, r29, 0x10
        lbz r24, 0(r6)
        subf r23, r21, r25
        slwi r31, r31, 8
        slwi r29, r29, 8
        slwi r27, r23, 8
        or r31, r31, r24
        addi r6, r6, 1
        b Rvl_decode_asr_L328
    Rvl_decode_asr_L339:
        cmplw r7, r5
        blt Rvl_decode_asr_L046
    Rvl_decode_asr_L341:
        mr r3, r5
        lmw r21, 0x10(r1)
        addi r1, r1, 0x40
        blr

}
#else
int Rvl_decode_asr(u8* out, u8* in) {
    return 0;
}
#endif
