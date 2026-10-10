// BESwap: turns a module compiled for big-endian PowerPC (powerpc-unknown-eabi)
// into a wasm32 module that keeps big-endian byte order in linear memory.
//
// The Wii code base parses big-endian resources by overlaying structs, type
// puns colors and words, and lays bitfields out MSB first. Instead of porting
// every such site, the whole program is compiled with big-endian semantics:
//
//   * every multi-byte scalar load/store is followed/preceded by a byte swap,
//   * global initializers are re-encoded in big-endian byte order,
//   * relocated fields (pointers, function table indices) cannot be encoded
//     big-endian by the linker, so they are swapped in place by a priority 0
//     constructor that runs before any other code,
//   * variadic arguments are passed pre-swapped so that va_arg (a plain load)
//     reads them back correctly, and the va_list pointer written by
//     llvm.va_start is converted to big-endian.
//
// Wii addresses are translated so that a wasm address equals the Wii physical
// address: MEM1 at 0x00000000 and MEM2 at 0x10000000 (the program itself lives
// above 0x14000000). Constant cached/uncached addresses are rewritten to
// physical ones, hardware registers are redirected to a dummy array, dynamic
// inttoptr conversions are masked with 0x3FFFFFFF, and globals declared with
// ADDRESS(x) (annotate("wiiaddr", x)) become aliases of their fixed address.
//
// Finally the triple/datalayout are switched to wasm32-unknown-emscripten. The
// two datalayouts agree on every size and alignment used by the code.

#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/AttributeMask.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

using namespace llvm;

namespace {

static const char* kWasmDL = "e-m:e-p:32:32-p10:8:8-p20:8:8-i64:64-i128:128-f128:64-n32:64-S128-ni:1:10:20";
static const char* kWasmTriple = "wasm32-unknown-emscripten";

static bool gErrors = false;

static void fatal(const Twine& msg) {
    errs() << "beswap: error: " << msg << "\n";
    gErrors = true;
}

struct Swapper {
    Module& M;
    const DataLayout& DL;
    LLVMContext& C;

    Swapper(Module& m) : M(m), DL(m.getDataLayout()), C(m.getContext()) {}

    // ---- address translation ----

    GlobalVariable* hwRegs = nullptr;

    Constant* hwPtr(uint64_t off) {
        if (!hwRegs) {
            hwRegs = M.getGlobalVariable("__wii_hwreg");
            if (!hwRegs)
                hwRegs = new GlobalVariable(M, ArrayType::get(Type::getInt8Ty(C), 0x30000), false,
                                            GlobalValue::ExternalLinkage, nullptr, "__wii_hwreg");
        }
        return ConstantExpr::getGetElementPtr(Type::getInt8Ty(C), hwRegs,
                                              ConstantInt::get(Type::getInt32Ty(C), off));
    }

    // Returns the translated pointer for a constant Wii address, or null if
    // the address needs no translation.
    Constant* translateAddr(uint64_t a, Type* PT) {
        a &= 0xFFFFFFFFull;
        auto phys = [&](uint64_t p) { return ConstantExpr::getIntToPtr(ConstantInt::get(Type::getInt32Ty(C), p), PT); };
        if (a >= 0x80000000 && a < 0x81800000)
            return phys(a - 0x80000000);
        if (a >= 0xC0000000 && a < 0xC1800000)
            return phys(a - 0xC0000000);
        if (a >= 0x90000000 && a < 0x94000000)
            return phys(a - 0x80000000);
        if (a >= 0xD0000000 && a < 0xD4000000)
            return phys(a - 0xC0000000);
        uint64_t hw = a & 0x0FFFFFFF;
        if ((a >> 28) == 0xC || (a >> 28) == 0x0) {
            if (hw >= 0x0C000000 && hw < 0x0C010000)
                return hwPtr(hw - 0x0C000000);
            if (hw >= 0x0D000000 && hw < 0x0D010000)
                return hwPtr(0x10000 + hw - 0x0D000000);
            if (hw >= 0x0D800000 && hw < 0x0D810000)
                return hwPtr(0x20000 + hw - 0x0D800000);
        }
        if (a >= 0x80000000 && a < 0xFFFF0000)
            errs() << "beswap: warning: untranslated constant address 0x" << Twine::utohexstr(a) << " in " << M.getName()
                   << "\n";
        return nullptr;
    }

    std::map<Constant*, Constant*> constCache;

    Constant* fixConst(Constant* K) {
        auto it = constCache.find(K);
        if (it != constCache.end())
            return it->second;
        Constant* R = K;
        if (auto* CE = dyn_cast<ConstantExpr>(K)) {
            if (CE->getOpcode() == Instruction::IntToPtr && isa<ConstantInt>(CE->getOperand(0))) {
                if (Constant* T = translateAddr(cast<ConstantInt>(CE->getOperand(0))->getZExtValue(), CE->getType()))
                    R = T;
            } else {
                bool changed = false;
                std::vector<Constant*> ops;
                for (unsigned i = 0; i < CE->getNumOperands(); i++) {
                    Constant* O = fixConst(CE->getOperand(i));
                    changed |= O != CE->getOperand(i);
                    ops.push_back(O);
                }
                if (changed)
                    R = CE->getWithOperands(ops);
            }
        } else if (isa<ConstantAggregate>(K)) {
            bool changed = false;
            std::vector<Constant*> ops;
            for (unsigned i = 0; i < K->getNumOperands(); i++) {
                Constant* O = fixConst(cast<Constant>(K->getOperand(i)));
                changed |= O != K->getOperand(i);
                ops.push_back(O);
            }
            if (changed) {
                if (auto* CS = dyn_cast<ConstantStruct>(K))
                    R = ConstantStruct::get(CS->getType(), ops);
                else if (auto* CA = dyn_cast<ConstantArray>(K))
                    R = ConstantArray::get(CA->getType(), ops);
                else if (auto* CV = dyn_cast<ConstantVector>(K))
                    R = ConstantVector::get(ops);
            }
        }
        constCache[K] = R;
        return R;
    }

    void translateAddresses() {
        // ADDRESS(x) globals.
        if (GlobalVariable* GA = M.getGlobalVariable("llvm.global.annotations")) {
            std::vector<std::pair<GlobalVariable*, uint64_t>> fixed;
            if (auto* CA = dyn_cast<ConstantArray>(GA->getInitializer())) {
                for (unsigned i = 0; i < CA->getNumOperands(); i++) {
                    auto* E = dyn_cast<ConstantStruct>(CA->getOperand(i));
                    if (!E || E->getNumOperands() < 5)
                        continue;
                    auto* G = dyn_cast<GlobalVariable>(E->getOperand(0)->stripPointerCasts());
                    auto* S = dyn_cast<GlobalVariable>(E->getOperand(1)->stripPointerCasts());
                    auto* A = dyn_cast<GlobalVariable>(E->getOperand(4)->stripPointerCasts());
                    if (!G || !S || !A || !S->hasInitializer() || !A->hasInitializer())
                        continue;
                    auto* SD = dyn_cast<ConstantDataArray>(S->getInitializer());
                    if (!SD || !SD->isCString() || SD->getAsCString() != "wiiaddr")
                        continue;
                    auto* AS = dyn_cast<ConstantStruct>(A->getInitializer());
                    if (!AS || AS->getNumOperands() < 1)
                        continue;
                    if (auto* CI = dyn_cast<ConstantInt>(AS->getOperand(0)))
                        fixed.push_back({G, CI->getZExtValue()});
                }
            }
            GA->eraseFromParent();
            for (auto& f : fixed) {
                Constant* T = translateAddr(f.second, f.first->getType());
                if (!T) {
                    fatal("cannot place ADDRESS global " + f.first->getName());
                    continue;
                }
                f.first->replaceAllUsesWith(T);
                f.first->eraseFromParent();
            }
        }
        // Constant addresses in code and data.
        Type* I32 = Type::getInt32Ty(C);
        for (Function& F : M) {
            for (Instruction& I : instructions(F)) {
                for (unsigned i = 0; i < I.getNumOperands(); i++) {
                    auto* K = dyn_cast<Constant>(I.getOperand(i));
                    if (!K || isa<GlobalValue>(K) || isa<ConstantData>(K))
                        continue;
                    Constant* N = fixConst(K);
                    if (N != K)
                        I.setOperand(i, N);
                }
            }
            std::vector<IntToPtrInst*> casts;
            for (Instruction& I : instructions(F))
                if (auto* IP = dyn_cast<IntToPtrInst>(&I))
                    casts.push_back(IP);
            for (IntToPtrInst* IP : casts) {
                Value* V = IP->getOperand(0);
                if (auto* CI = dyn_cast<ConstantInt>(V)) {
                    if (Constant* T = translateAddr(CI->getZExtValue(), IP->getType())) {
                        IP->replaceAllUsesWith(T);
                        IP->eraseFromParent();
                    }
                    continue;
                }
                if (V->getType()->isVectorTy())
                    continue;
                IRBuilder<> B(IP);
                Value* W = B.CreateZExtOrTrunc(V, I32);
                W = B.CreateAnd(W, 0x3FFFFFFF);
                IP->setOperand(0, B.CreateZExtOrTrunc(W, V->getType()));
            }
        }
        for (GlobalVariable& G : M.globals()) {
            if (!G.hasInitializer() || G.getName().starts_with("llvm."))
                continue;
            Constant* N = fixConst(G.getInitializer());
            if (N != G.getInitializer())
                G.setInitializer(N);
        }
    }

    // Swap the bytes of an integer whose width is a multiple of 8.
    Value* bswapInt(IRBuilder<>& B, Value* V) {
        IntegerType* T = cast<IntegerType>(V->getType());
        unsigned bits = T->getBitWidth();
        if (bits <= 8)
            return V;
        if (bits % 16 == 0)
            return B.CreateUnaryIntrinsic(Intrinsic::bswap, V);
        // i24, i40, i56 ...
        IntegerType* W = IntegerType::get(C, bits + 8);
        Value* X = B.CreateZExt(V, W);
        X = B.CreateUnaryIntrinsic(Intrinsic::bswap, X);
        X = B.CreateLShr(X, 8);
        return B.CreateTrunc(X, T);
    }

    Value* bswapVecInt(IRBuilder<>& B, Value* V) {
        return B.CreateUnaryIntrinsic(Intrinsic::bswap, V);
    }

    // Integer type with the store size of T, used for the actual memory access.
    Type* memType(Type* T) {
        if (auto* VT = dyn_cast<FixedVectorType>(T)) {
            Type* E = VT->getElementType();
            unsigned eb = DL.getTypeStoreSizeInBits(E);
            if (eb != DL.getTypeSizeInBits(E) || (eb != 16 && eb != 32 && eb != 64))
                return nullptr;
            return FixedVectorType::get(IntegerType::get(C, eb), VT->getNumElements());
        }
        if (T->isIntegerTy() || T->isFloatingPointTy() || T->isPointerTy()) {
            uint64_t sz = DL.getTypeStoreSize(T);
            return IntegerType::get(C, sz * 8);
        }
        return nullptr;
    }

    Value* toMem(IRBuilder<>& B, Value* V, Type* MT) {
        Type* T = V->getType();
        if (T == MT)
            return V;
        if (T->isPtrOrPtrVectorTy())
            return B.CreatePtrToInt(V, MT);
        if (T->isFPOrFPVectorTy())
            return B.CreateBitCast(V, MT);
        if (T->isIntOrIntVectorTy())
            return B.CreateZExt(V, MT);
        fatal("toMem: unsupported type");
        return V;
    }

    Value* fromMem(IRBuilder<>& B, Value* V, Type* T) {
        Type* MT = V->getType();
        if (T == MT)
            return V;
        if (T->isPtrOrPtrVectorTy())
            return B.CreateIntToPtr(V, T);
        if (T->isFPOrFPVectorTy())
            return B.CreateBitCast(V, T);
        if (T->isIntOrIntVectorTy())
            return B.CreateTrunc(V, T);
        fatal("fromMem: unsupported type");
        return V;
    }

    Value* swapMem(IRBuilder<>& B, Value* X) {
        if (X->getType()->isVectorTy())
            return bswapVecInt(B, X);
        return bswapInt(B, X);
    }

    // Byte swap a value of any scalar type, keeping its type.
    Value* swapValue(IRBuilder<>& B, Value* V) {
        Type* T = V->getType();
        Type* MT = memType(T);
        if (!MT)
            return nullptr;
        if (DL.getTypeStoreSize(T) <= 1)
            return V;
        return fromMem(B, swapMem(B, toMem(B, V, MT)), T);
    }

    bool needsSwap(Type* T) {
        if (T->isAggregateType())
            return true;
        if (auto* VT = dyn_cast<VectorType>(T))
            return DL.getTypeStoreSize(VT->getElementType()) > 1;
        return DL.getTypeStoreSize(T) > 1;
    }

    void rewriteLoad(LoadInst* LI) {
        Type* T = LI->getType();
        if (!needsSwap(T))
            return;
        Type* MT = memType(T);
        if (!MT) {
            std::string s;
            raw_string_ostream os(s);
            T->print(os);
            fatal("load of unsupported type " + os.str() + " in " + LI->getFunction()->getName());
            return;
        }
        IRBuilder<> B(LI);
        LoadInst* NL = B.CreateAlignedLoad(MT, LI->getPointerOperand(), LI->getAlign(), LI->isVolatile());
        NL->setAtomic(LI->getOrdering(), LI->getSyncScopeID());
        NL->copyMetadata(*LI, {LLVMContext::MD_nontemporal, LLVMContext::MD_invariant_load});
        Value* V = fromMem(B, swapMem(B, NL), T);
        V->takeName(LI);
        LI->replaceAllUsesWith(V);
        LI->eraseFromParent();
    }

    void rewriteStore(StoreInst* SI) {
        Value* V = SI->getValueOperand();
        Type* T = V->getType();
        if (!needsSwap(T))
            return;
        Type* MT = memType(T);
        if (!MT) {
            std::string s;
            raw_string_ostream os(s);
            T->print(os);
            fatal("store of unsupported type " + os.str() + " in " + SI->getFunction()->getName());
            return;
        }
        IRBuilder<> B(SI);
        Value* X = swapMem(B, toMem(B, V, MT));
        StoreInst* NS = B.CreateAlignedStore(X, SI->getPointerOperand(), SI->getAlign(), SI->isVolatile());
        NS->setAtomic(SI->getOrdering(), SI->getSyncScopeID());
        SI->eraseFromParent();
    }

    void rewriteVarargCall(CallBase* CB) {
        FunctionType* FT = CB->getFunctionType();
        if (!FT->isVarArg())
            return;
        if (isa<IntrinsicInst>(CB))
            return;
        unsigned np = FT->getNumParams();
        for (unsigned i = np; i < CB->arg_size(); i++) {
            if (CB->paramHasAttr(i, Attribute::ByVal) || CB->paramHasAttr(i, Attribute::InAlloca) ||
                CB->paramHasAttr(i, Attribute::Preallocated))
                continue;
            Value* A = CB->getArgOperand(i);
            if (!needsSwap(A->getType()))
                continue;
            IRBuilder<> B(CB);
            Value* S = swapValue(B, A);
            if (!S) {
                fatal("unsupported vararg type in " + CB->getFunction()->getName());
                continue;
            }
            CB->setArgOperand(i, S);
        }
    }

    void fixVaStart(CallInst* CI) {
        // wasm's va_start stores a native (little-endian) pointer to the
        // variadic buffer into the first word of the va_list.
        IRBuilder<> B(CI->getNextNode());
        Value* AP = CI->getArgOperand(0);
        Type* I32 = Type::getInt32Ty(C);
        LoadInst* L = B.CreateAlignedLoad(I32, AP, Align(4));
        Value* S = bswapInt(B, L);
        B.CreateAlignedStore(S, AP, Align(4));
    }

    void processFunction(Function& F) {
        std::vector<Instruction*> work;
        for (Instruction& I : instructions(F))
            work.push_back(&I);
        for (Instruction* I : work) {
            if (auto* LI = dyn_cast<LoadInst>(I)) {
                rewriteLoad(LI);
            } else if (auto* SI = dyn_cast<StoreInst>(I)) {
                rewriteStore(SI);
            } else if (isa<AtomicRMWInst>(I) || isa<AtomicCmpXchgInst>(I)) {
                fatal("atomic rmw/cmpxchg not supported in " + F.getName());
            } else if (isa<VAArgInst>(I)) {
                fatal("va_arg instruction in " + F.getName());
            } else if (auto* CB = dyn_cast<CallBase>(I)) {
                if (CB->isInlineAsm()) {
                    fatal("inline asm in " + F.getName());
                    continue;
                }
                if (auto* II = dyn_cast<IntrinsicInst>(CB)) {
                    Intrinsic::ID id = II->getIntrinsicID();
                    if (id == Intrinsic::vastart) {
                        fixVaStart(II);
                    } else if (id == Intrinsic::masked_load || id == Intrinsic::masked_store ||
                               id == Intrinsic::masked_gather || id == Intrinsic::masked_scatter) {
                        fatal("masked memory intrinsic in " + F.getName());
                    } else {
                        StringRef n = II->getCalledFunction()->getName();
                        if (n.starts_with("llvm.ppc."))
                            fatal("PPC intrinsic " + n + " in " + F.getName());
                    }
                    continue;
                }
                rewriteVarargCall(CB);
            }
        }
    }

    // ---- global initializers ----

    struct Reloc {
        uint64_t off;
        Constant* c;
        uint64_t size;
    };

    void putBE(std::vector<uint8_t>& bytes, uint64_t off, const APInt& v, uint64_t size) {
        for (uint64_t i = 0; i < size; i++) {
            uint64_t shift = (size - 1 - i) * 8;
            bytes[off + i] = (uint8_t)v.lshr(shift).getZExtValue();
        }
    }

    bool flatten(Constant* K, uint64_t off, std::vector<uint8_t>& bytes, std::vector<Reloc>& relocs) {
        Type* T = K->getType();
        if (isa<ConstantAggregateZero>(K) || isa<ConstantPointerNull>(K) || isa<UndefValue>(K))
            return true;
        if (auto* CI = dyn_cast<ConstantInt>(K)) {
            uint64_t sz = DL.getTypeStoreSize(T);
            putBE(bytes, off, CI->getValue().zext(sz * 8), sz);
            return true;
        }
        if (auto* CF = dyn_cast<ConstantFP>(K)) {
            uint64_t sz = DL.getTypeStoreSize(T);
            putBE(bytes, off, CF->getValueAPF().bitcastToAPInt(), sz);
            return true;
        }
        if (auto* CDS = dyn_cast<ConstantDataSequential>(K)) {
            Type* E = CDS->getElementType();
            uint64_t es = DL.getTypeAllocSize(E);
            for (unsigned i = 0; i < CDS->getNumElements(); i++)
                if (!flatten(CDS->getElementAsConstant(i), off + i * es, bytes, relocs))
                    return false;
            return true;
        }
        if (auto* CA = dyn_cast<ConstantArray>(K)) {
            uint64_t es = DL.getTypeAllocSize(CA->getType()->getElementType());
            for (unsigned i = 0; i < CA->getNumOperands(); i++)
                if (!flatten(CA->getOperand(i), off + i * es, bytes, relocs))
                    return false;
            return true;
        }
        if (auto* CV = dyn_cast<ConstantVector>(K)) {
            uint64_t es = DL.getTypeAllocSize(CV->getType()->getElementType());
            for (unsigned i = 0; i < CV->getNumOperands(); i++)
                if (!flatten(CV->getOperand(i), off + i * es, bytes, relocs))
                    return false;
            return true;
        }
        if (auto* CS = dyn_cast<ConstantStruct>(K)) {
            const StructLayout* SL = DL.getStructLayout(CS->getType());
            for (unsigned i = 0; i < CS->getNumOperands(); i++)
                if (!flatten(CS->getOperand(i), off + SL->getElementOffset(i), bytes, relocs))
                    return false;
            return true;
        }
        if (auto* CE = dyn_cast<ConstantExpr>(K)) {
            if (CE->getOpcode() == Instruction::IntToPtr) {
                if (auto* CI = dyn_cast<ConstantInt>(CE->getOperand(0))) {
                    uint64_t sz = DL.getTypeStoreSize(T);
                    putBE(bytes, off, CI->getValue().zextOrTrunc(sz * 8), sz);
                    return true;
                }
            }
        }
        if (T->isPointerTy() || T->isIntegerTy()) {
            uint64_t sz = DL.getTypeStoreSize(T);
            if (sz == 1) {
                // A one byte relocation has no byte order; keep it.
                relocs.push_back({off, K, 0});
            } else {
                relocs.push_back({off, K, sz});
            }
            return true;
        }
        std::string s;
        raw_string_ostream os(s);
        K->print(os);
        fatal("cannot flatten initializer constant: " + os.str());
        return false;
    }

    static bool isDupLinkage(GlobalValue::LinkageTypes L) {
        return GlobalValue::isLinkOnceLinkage(L) || GlobalValue::isWeakLinkage(L) || GlobalValue::isCommonLinkage(L);
    }

    struct Fixup {
        GlobalVariable* gv;
        std::vector<Reloc> relocs;
    };

    std::vector<Fixup> fixups;

    bool isTrivialInit(Constant* K) {
        if (isa<ConstantAggregateZero>(K) || isa<ConstantPointerNull>(K) || isa<UndefValue>(K))
            return true;
        if (auto* CDS = dyn_cast<ConstantDataSequential>(K))
            return DL.getTypeStoreSize(CDS->getElementType()) == 1;
        if (auto* CI = dyn_cast<ConstantInt>(K))
            return DL.getTypeStoreSize(CI->getType()) == 1;
        return false;
    }

    void processGlobal(GlobalVariable* GV) {
        if (!GV->hasInitializer())
            return;
        if (GV->getName().starts_with("llvm."))
            return;
        if (GV->hasAvailableExternallyLinkage()) {
            GV->setInitializer(nullptr);
            GV->setLinkage(GlobalValue::ExternalLinkage);
            return;
        }
        Constant* K = GV->getInitializer();
        if (isTrivialInit(K))
            return;
        uint64_t size = DL.getTypeAllocSize(K->getType());
        std::vector<uint8_t> bytes(size, 0);
        std::vector<Reloc> relocs;
        if (!flatten(K, 0, bytes, relocs)) {
            fatal("in global " + GV->getName());
            return;
        }
        // Build a packed struct: byte runs interleaved with relocation leaves.
        std::vector<Constant*> elems;
        std::vector<Type*> types;
        uint64_t pos = 0;
        auto emitBytes = [&](uint64_t end) {
            if (end > pos) {
                ArrayRef<uint8_t> run(bytes.data() + pos, end - pos);
                Constant* A = ConstantDataArray::get(C, run);
                elems.push_back(A);
                types.push_back(A->getType());
                pos = end;
            }
        };
        std::sort(relocs.begin(), relocs.end(), [](const Reloc& a, const Reloc& b) { return a.off < b.off; });
        std::vector<Reloc> swapRelocs;
        for (auto& r : relocs) {
            emitBytes(r.off);
            elems.push_back(r.c);
            types.push_back(r.c->getType());
            pos = r.off + DL.getTypeStoreSize(r.c->getType());
            if (r.size > 1)
                swapRelocs.push_back(r);
        }
        emitBytes(size);
        StructType* ST = StructType::get(C, types, /*packed*/ true);
        Constant* NI = ConstantStruct::get(ST, elems);

        GlobalVariable* NG = new GlobalVariable(M, ST, GV->isConstant() && swapRelocs.empty(), GV->getLinkage(), NI, "",
                                                GV, GV->getThreadLocalMode(), GV->getAddressSpace(),
                                                GV->isExternallyInitialized());
        NG->copyAttributesFrom(GV);
        NG->setAlignment(GV->getAlign().value_or(DL.getPrefTypeAlign(K->getType())));
        NG->setComdat(GV->getComdat());
        NG->takeName(GV);
        GV->replaceAllUsesWith(NG);
        GV->eraseFromParent();
        if (!swapRelocs.empty())
            fixups.push_back({NG, swapRelocs});
    }

    void emitFixups() {
        if (fixups.empty())
            return;
        Type* VoidT = Type::getVoidTy(C);
        Function* F = Function::Create(FunctionType::get(VoidT, false), GlobalValue::InternalLinkage,
                                       "__beswap_fixup", &M);
        BasicBlock* BB = BasicBlock::Create(C, "entry", F);
        IRBuilder<> B(BB);
        for (auto& fx : fixups) {
            GlobalVariable* G = fx.gv;
            BasicBlock* cont = nullptr;
            if (!G->hasLocalLinkage()) {
                // Several translation units may carry this definition (an
                // inline/template definition, or a vtable some units emit
                // weakly while another emits it strongly); only the first
                // fixup that runs may swap it.
                std::string gname = ("__beswap_done." + G->getName()).str();
                GlobalVariable* guard = M.getGlobalVariable(gname, true);
                if (!guard) {
                    guard = new GlobalVariable(M, Type::getInt8Ty(C), false, GlobalValue::WeakAnyLinkage,
                                               ConstantInt::get(Type::getInt8Ty(C), 0), gname);
                    guard->setVisibility(G->getVisibility());
                }
                Value* done = B.CreateLoad(Type::getInt8Ty(C), guard);
                Value* isDone = B.CreateICmpNE(done, ConstantInt::get(Type::getInt8Ty(C), 0));
                BasicBlock* doit = BasicBlock::Create(C, "fix", F);
                cont = BasicBlock::Create(C, "cont", F);
                B.CreateCondBr(isDone, cont, doit);
                B.SetInsertPoint(doit);
                B.CreateStore(ConstantInt::get(Type::getInt8Ty(C), 1), guard);
            }
            for (auto& r : fx.relocs) {
                Value* P = B.CreateConstInBoundsGEP1_64(Type::getInt8Ty(C), G, r.off);
                Type* IT = IntegerType::get(C, r.size * 8);
                LoadInst* L = B.CreateAlignedLoad(IT, P, Align(1), true);
                Value* S = bswapInt(B, L);
                B.CreateAlignedStore(S, P, Align(1), true);
            }
            if (cont) {
                B.CreateBr(cont);
                B.SetInsertPoint(cont);
            }
        }
        B.CreateRetVoid();
        appendToGlobalCtors(M, F, 0);
    }

    // C declarations without a prototype (`void f();`) are emitted by the PPC
    // frontend as `void f(...)`. On wasm a variadic call passes a buffer, which
    // would not match the real definition, so calls through such declarations
    // become ordinary calls with the argument types of the call site.
    void fixUnprototyped() {
        std::vector<Function*> decls;
        for (Function& F : M)
            if (F.isDeclaration() && F.getFunctionType()->isVarArg() && F.getFunctionType()->getNumParams() == 0 &&
                !F.isIntrinsic())
                decls.push_back(&F);
        for (Function* F : decls) {
            std::vector<CallBase*> calls;
            for (User* U : F->users())
                if (auto* CB = dyn_cast<CallBase>(U))
                    if (CB->getCalledOperand() == F)
                        calls.push_back(CB);
            std::vector<Type*> params;
            if (!calls.empty())
                for (Value* A : calls[0]->args())
                    params.push_back(A->getType());
            FunctionType* NFT = FunctionType::get(F->getReturnType(), params, false);
            Function* NF = Function::Create(NFT, F->getLinkage(), "", &M);
            NF->copyAttributesFrom(F);
            NF->takeName(F);
            for (CallBase* CB : calls) {
                std::vector<Type*> ap;
                for (Value* A : CB->args())
                    ap.push_back(A->getType());
                CB->setCalledOperand(NF);
                CB->mutateFunctionType(FunctionType::get(CB->getType(), ap, false));
            }
            F->replaceAllUsesWith(NF);
            F->eraseFromParent();
        }
    }

    bool run() {
        if (!DL.isBigEndian()) {
            fatal("module is not big-endian");
            return false;
        }
        translateAddresses();
        fixUnprototyped();
        for (Function& F : M) {
            if (F.isDeclaration())
                continue;
            processFunction(F);
        }
        std::vector<GlobalVariable*> gvs;
        for (GlobalVariable& G : M.globals())
            gvs.push_back(&G);
        for (GlobalVariable* G : gvs)
            processGlobal(G);
        emitFixups();

        for (Function& F : M) {
            // MWCC emits non-static C inline functions once per program.
            if (!F.isDeclaration() && F.hasExternalLinkage() && F.hasFnAttribute(Attribute::InlineHint))
                F.setLinkage(GlobalValue::LinkOnceODRLinkage);
            F.removeFnAttr("target-cpu");
            F.removeFnAttr("target-features");
            F.removeFnAttr("tune-cpu");
            if (F.hasSection())
                F.setSection("");
        }
        // llvm-link only materializes linkonce definitions that are already
        // referenced when their module is merged; weak ones are always kept
        // (and dropped by globaldce after internalization if unused).
        for (GlobalValue& GV : M.global_values()) {
            if (GV.hasLinkOnceODRLinkage())
                GV.setLinkage(GlobalValue::WeakODRLinkage);
            else if (GV.hasLinkOnceAnyLinkage())
                GV.setLinkage(GlobalValue::WeakAnyLinkage);
        }
        M.setTargetTriple(kWasmTriple);
        M.setDataLayout(kWasmDL);
        return true;
    }
};

// Code that falls off the end of a non-void function (the decompiled sources
// have a few, matching the original binary) gets an `unreachable` from the C++
// frontend, which the optimizer turns into traps or worse. On the PowerPC the
// function simply returned whatever was in r3/f1, which usually is the first
// argument, so return that (or zero) instead. Must run before any other pass.
struct FixRetPass : PassInfoMixin<FixRetPass> {
    PreservedAnalyses run(Module& M, ModuleAnalysisManager&) {
        bool changed = false;
        for (Function& F : M) {
            if (F.isDeclaration() || F.getReturnType()->isVoidTy())
                continue;
            Type* RT = F.getReturnType();
            for (BasicBlock& BB : F) {
                if (BB.size() != 1 || !isa<UnreachableInst>(BB.front()) || pred_empty(&BB))
                    continue;
                Value* V = nullptr;
                if (F.arg_size() > 0 && F.getArg(0)->getType() == RT)
                    V = F.getArg(0);
                else
                    V = Constant::getNullValue(RT);
                BB.front().eraseFromParent();
                ReturnInst::Create(F.getContext(), V, &BB);
                changed = true;
                AttributeMask AM;
                AM.addAttribute(Attribute::NonNull);
                AM.addAttribute(Attribute::NoUndef);
                AM.addAttribute(Attribute::Dereferenceable);
                AM.addAttribute(Attribute::DereferenceableOrNull);
                AM.addAttribute(Attribute::NoAlias);
                F.removeRetAttrs(AM);
            }
        }
        return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
    }
};

// Member function pointers: the PowerPC (Itanium) C++ ABI marks a virtual
// member pointer by setting bit 0 of its pointer field (vtable offset + 1),
// which assumes functions are at even addresses. Wasm function pointers are
// table indices, so the link places the table at TABLE_BASE (0x10000) and
// every "is virtual" test also requires the value to be below it (vtable
// offsets are tiny). Needs the frontend's value names (memptr.isvirtual).
static const uint32_t kTableBase = 0x10000;

struct FixPmfPass : PassInfoMixin<FixPmfPass> {
    PreservedAnalyses run(Module& M, ModuleAnalysisManager&) {
        std::vector<ICmpInst*> work;
        for (Function& F : M)
            for (Instruction& I : instructions(F))
                if (auto* IC = dyn_cast<ICmpInst>(&I))
                    if (IC->getName().starts_with("memptr.isvirtual"))
                        work.push_back(IC);
        for (ICmpInst* IC : work) {
            auto* A = dyn_cast<BinaryOperator>(IC->getOperand(0));
            if (!A || A->getOpcode() != Instruction::And) {
                errs() << "fixpmf: unexpected memptr.isvirtual in " << IC->getFunction()->getName() << "\n";
                continue;
            }
            Value* X = A->getOperand(0);
            IRBuilder<> B(IC->getNextNode());
            Value* Small = B.CreateICmpULT(X, ConstantInt::get(X->getType(), kTableBase), "memptr.small");
            Value* R = B.CreateAnd(IC, Small, "memptr.isvirtual.web");
            IC->replaceUsesWithIf(R, [&](Use& U) { return U.getUser() != R; });
        }
        return work.empty() ? PreservedAnalyses::all() : PreservedAnalyses::none();
    }
};

struct BESwapPass : PassInfoMixin<BESwapPass> {
    PreservedAnalyses run(Module& M, ModuleAnalysisManager&) {
        Swapper S(M);
        S.run();
        if (gErrors)
            report_fatal_error("beswap failed", false);
        return PreservedAnalyses::none();
    }
    static bool isRequired() { return true; }
};

// Whole-program fixups, run on the llvm-link'ed module: MWCC tolerated
// prototypes that disagree between translation units (int vs void returns,
// extra or missing arguments). wasm requires exact signatures, so direct calls
// are adapted to the callee's real type.
// The program defines C library functions of its own (MSL's printf family,
// string and wide string functions...) that work on big-endian memory. LLVM
// recognizes such names as library calls and may rewrite them into calls to
// Emscripten's native libc (sprintf -> siprintf, printf -> puts, ...), so
// definitions that take or return pointers get a name LLVM does not know.
// Pure value functions (math) keep their names and optimizations.
static void renameLibFuncs(Module& M) {
    TargetLibraryInfoImpl TLII(Triple(M.getTargetTriple()));
    TargetLibraryInfo TLI(TLII);
    for (Function& F : M) {
        if (F.isDeclaration() || F.isIntrinsic())
            continue;
        LibFunc LF;
        if (!TLI.getLibFunc(F, LF))
            continue;
        bool pointers = F.getReturnType()->isPointerTy() || F.isVarArg();
        for (Argument& A : F.args())
            pointers |= A.getType()->isPointerTy();
        if (!pointers)
            continue;
        std::string old = F.getName().str();
        F.setName("__be_" + old);
        errs() << "befix: renamed library function " << old << "\n";
    }
}

struct BEFixPass : PassInfoMixin<BEFixPass> {
    static Value* convert(IRBuilder<>& B, Value* V, Type* T) {
        Type* S = V->getType();
        if (S == T)
            return V;
        if (T->isVoidTy())
            return nullptr;
        if (S->isIntegerTy() && T->isIntegerTy())
            return B.CreateZExtOrTrunc(V, T);
        if (S->isPointerTy() && T->isIntegerTy())
            return B.CreateZExtOrTrunc(B.CreatePtrToInt(V, B.getInt32Ty()), T);
        if (S->isIntegerTy() && T->isPointerTy())
            return B.CreateIntToPtr(B.CreateZExtOrTrunc(V, B.getInt32Ty()), T);
        if (S->isFloatingPointTy() && T->isFloatingPointTy())
            return B.CreateFPCast(V, T);
        const DataLayout& DL = B.GetInsertBlock()->getModule()->getDataLayout();
        if (DL.getTypeSizeInBits(S) == DL.getTypeSizeInBits(T) && S->isSingleValueType() && T->isSingleValueType())
            return B.CreateBitCast(V, T);
        return Constant::getNullValue(T);
    }

    // Complete-object and base-object constructors/destructors are equivalent
    // without virtual bases, so a missing variant aliases the existing one.
    // Explicit aliases (MWCC-mangled names used through extern "C") come from
    // the file named by BEFIX_ALIASES: "<missing> <existing>" per line.
    void resolveAliases(Module& M) {
        std::vector<std::pair<std::string, std::string>> pairs;
        for (Function& F : M) {
            if (!F.isDeclaration())
                continue;
            std::string n = F.getName().str();
            static const char* swaps[][2] = {{"C1E", "C2E"}, {"C2E", "C1E"}, {"D1Ev", "D2Ev"}, {"D2Ev", "D1Ev"}};
            for (auto& sw : swaps) {
                size_t pos = n.rfind(sw[0]);
                if (pos == std::string::npos)
                    continue;
                std::string alt = n.substr(0, pos) + sw[1] + n.substr(pos + strlen(sw[0]));
                if (Function* G = M.getFunction(alt))
                    if (!G->isDeclaration()) {
                        pairs.push_back({n, alt});
                        break;
                    }
            }
        }
        if (const char* path = getenv("BEFIX_ALIASES")) {
            if (FILE* f = fopen(path, "r")) {
                char line[1024], a[512], b[512];
                while (fgets(line, sizeof(line), f))
                    if (line[0] != '#' && sscanf(line, "%511s %511s", a, b) == 2)
                        pairs.push_back({a, b});
                fclose(f);
            }
        }
        for (auto& p : pairs) {
            GlobalValue* missing = M.getNamedValue(p.first);
            GlobalValue* target = M.getNamedValue(p.second);
            if (!missing || !target || !missing->isDeclaration() || target->isDeclaration())
                continue;
            GlobalAlias* A = GlobalAlias::create(target->getValueType(), 0, GlobalValue::ExternalLinkage, "", target, &M);
            missing->replaceAllUsesWith(A);
            A->takeName(missing);
            missing->eraseFromParent();
        }
    }

    // Undefined vtables become zero-filled tables (bases only used while a
    // derived object is constructed); other undefined functions, unless the
    // runtime provides them (BEFIX_PROVIDED lists those), get a stub that
    // reports the call through __web_unimplemented and returns zero.
    void stubUndefined(Module& M) {
        const char* path = getenv("BEFIX_PROVIDED");
        if (!path)
            return;
        std::set<std::string> provided;
        if (FILE* f = fopen(path, "r")) {
            char line[1024];
            while (fgets(line, sizeof(line), f)) {
                std::string n(line);
                while (!n.empty() && (n.back() == '\n' || n.back() == '\r' || n.back() == ' '))
                    n.pop_back();
                provided.insert(n);
            }
            fclose(f);
        }
        LLVMContext& C = M.getContext();
        for (GlobalVariable& G : M.globals()) {
            if (!G.isDeclaration() || G.getName().starts_with("llvm.") || provided.count(G.getName().str()))
                continue;
            if (G.getName().starts_with("_ZTV")) {
                Type* T = ArrayType::get(PointerType::get(C, 0), 64);
                GlobalVariable* N = new GlobalVariable(M, T, false, GlobalValue::ExternalLinkage,
                                                       Constant::getNullValue(T), "", &G);
                G.replaceAllUsesWith(N);
                N->takeName(&G);
                errs() << "befix: zero vtable " << N->getName() << "\n";
            } else if (!G.getName().starts_with("__")) {
                errs() << "befix: undefined global " << G.getName() << "\n";
            } else if (G.getName() != "__dso_handle") {
                errs() << "befix: undefined global " << G.getName() << "\n";
            }
        }
        for (auto it = M.global_begin(); it != M.global_end();) {
            GlobalVariable& G = *it++;
            if (G.use_empty() && G.isDeclaration() && G.getName().starts_with("_ZTV"))
                G.eraseFromParent();
        }
        FunctionCallee report = M.getOrInsertFunction("__web_unimplemented", Type::getVoidTy(C), PointerType::get(C, 0));
        for (Function& F : M) {
            if (!F.isDeclaration() || F.isIntrinsic() || provided.count(F.getName().str()) ||
                F.getName().starts_with("webjs_") || F.getName().starts_with("emscripten_") ||
                F.getName().starts_with("__cxa_") || F.getName() == "__web_unimplemented")
                continue;
            if (F.use_empty())
                continue;
            BasicBlock* BB = BasicBlock::Create(C, "entry", &F);
            IRBuilder<> B(BB);
            Constant* name = B.CreateGlobalStringPtr(F.getName(), "__web_unimpl_name");
            B.CreateCall(report, {name});
            if (F.getReturnType()->isVoidTy())
                B.CreateRetVoid();
            else
                B.CreateRet(Constant::getNullValue(F.getReturnType()));
            F.setLinkage(GlobalValue::InternalLinkage);
            errs() << "befix: stub " << F.getName() << "\n";
        }
    }

    PreservedAnalyses run(Module& M, ModuleAnalysisManager&) {
        resolveAliases(M);
        std::vector<CallBase*> work;
        for (Function& F : M)
            for (Instruction& I : instructions(F))
                if (auto* CB = dyn_cast<CallBase>(&I))
                    if (auto* Callee = dyn_cast<Function>(CB->getCalledOperand()->stripPointerCasts()))
                        if (CB->getFunctionType() != Callee->getFunctionType() && !Callee->isIntrinsic())
                            work.push_back(CB);
        for (CallBase* CB : work) {
            auto* CI = dyn_cast<CallInst>(CB);
            Function* Callee = cast<Function>(CB->getCalledOperand()->stripPointerCasts());
            if (!CI) {
                errs() << "befix: cannot adapt invoke of " << Callee->getName() << "\n";
                continue;
            }
            FunctionType* FT = Callee->getFunctionType();
            errs() << "befix: adapting call to " << Callee->getName() << " in " << CB->getFunction()->getName() << "\n";
            IRBuilder<> B(CI);
            std::vector<Value*> args;
            for (unsigned i = 0; i < FT->getNumParams(); i++) {
                Type* PT = FT->getParamType(i);
                if (i < CI->arg_size())
                    args.push_back(convert(B, CI->getArgOperand(i), PT));
                else
                    args.push_back(Constant::getNullValue(PT));
            }
            if (FT->isVarArg())
                for (unsigned i = FT->getNumParams(); i < CI->arg_size(); i++)
                    args.push_back(CI->getArgOperand(i));
            CallInst* NC = B.CreateCall(FT, Callee, args);
            NC->setCallingConv(Callee->getCallingConv());
            if (!CI->getType()->isVoidTy()) {
                Value* R = FT->getReturnType()->isVoidTy() ? Constant::getNullValue(CI->getType())
                                                           : convert(B, NC, CI->getType());
                CI->replaceAllUsesWith(R);
            }
            CI->eraseFromParent();
        }
        stubUndefined(M);
        renameLibFuncs(M);
        return PreservedAnalyses::none();
    }
    static bool isRequired() { return true; }
};

}  // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "BESwap", "1.0", [](PassBuilder& PB) {
                PB.registerPipelineParsingCallback(
                    [](StringRef Name, ModulePassManager& MPM, ArrayRef<PassBuilder::PipelineElement>) {
                        if (Name == "fixpmf") {
                            MPM.addPass(FixPmfPass());
                            return true;
                        }
                        if (Name == "fixret") {
                            MPM.addPass(FixRetPass());
                            return true;
                        }
                        if (Name == "beswap") {
                            MPM.addPass(BESwapPass());
                            return true;
                        }
                        if (Name == "befix") {
                            MPM.addPass(BEFixPass());
                            return true;
                        }
                        return false;
                    });
            }};
}
