//===--- IRBuilder.h - Swift IR Builder -------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines Swift's specialization of llvm::IRBuilder.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_IRBUILDER_H
#define SWIFT_IRGEN_IRBUILDER_H

#include "llvm/ADT/PointerUnion.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "swift/Basic/LLVM.h"
#include "Address.h"
#include "IRGen.h"

namespace swift {
namespace irgen {
class FunctionPointer;
class IRGenModule;

using IRBuilderBase = llvm::IRBuilder<>;

class IRBuilder : public IRBuilderBase {
public:
  // Without this, it keeps resolving to llvm::IRBuilderBase because
  // of the injected class name.
  using IRBuilderBase = irgen::IRBuilderBase;

private:
  /// The block containing the insertion point when the insertion
  /// point was last cleared.  Used only for preserving block
  /// ordering.
  llvm::BasicBlock *ClearedIP;
  unsigned NumTrapBarriers = 0;

#ifndef NDEBUG
  /// Whether debug information is requested. Only used in assertions.
  bool DebugInfo;
#endif

  // Set calling convention of the call instruction using
  // the same calling convention as the callee function.
  // This ensures that they are always compatible.
  void setCallingConvUsingCallee(llvm::CallInst *Call) {
    auto CalleeFn = Call->getCalledFunction();
    if (CalleeFn) {
      auto CC = CalleeFn->getCallingConv();
      Call->setCallingConv(CC);
    }
  }

public:
  IRBuilder(llvm::LLVMContext &Context, bool DebugInfo)
    : IRBuilderBase(Context), ClearedIP(nullptr)
#ifndef NDEBUG
    , DebugInfo(DebugInfo)
#endif
    {}

  /// Determines if the current location is apparently reachable.  The
  /// invariant we maintain is that the insertion point of the builder
  /// always points within a block unless the current location is
  /// logically unreachable.  All the low-level routines which emit
  /// branches leave the insertion point in the original block, just
  /// after the branch.  High-level routines may then indicate
  /// unreachability by clearing the insertion point.
  bool hasValidIP() const { return GetInsertBlock() != nullptr; }

  /// Determines whether we're currently inserting after a terminator.
  /// This is really just there for asserts.
  bool hasPostTerminatorIP() const {
    return GetInsertBlock() != nullptr &&
           !GetInsertBlock()->empty() &&
           isa<llvm::TerminatorInst>(GetInsertBlock()->back());
  }

  void ClearInsertionPoint() {
    assert(hasValidIP() && "clearing invalid insertion point!");
    assert(ClearedIP == nullptr);

    /// Whenever we clear the insertion point, remember where we were.
    ClearedIP = GetInsertBlock();
    IRBuilderBase::ClearInsertionPoint();
  }

  void SetInsertPoint(llvm::BasicBlock *BB) {
    ClearedIP = nullptr;
    IRBuilderBase::SetInsertPoint(BB);
  }
  
  void SetInsertPoint(llvm::BasicBlock *BB, llvm::BasicBlock::iterator before) {
    ClearedIP = nullptr;
    IRBuilderBase::SetInsertPoint(BB, before);
  }

  void SetInsertPoint(llvm::Instruction *I) {
    ClearedIP = nullptr;
    IRBuilderBase::SetInsertPoint(I);
  }

  /// A stable insertion point in the function.  "Stable" means that
  /// it will point to the same location in the function, even if
  /// instructions are subsequently added to the current basic block.
  class StableIP {
    /// Either an instruction that we're inserting after or the basic
    /// block that we're inserting at the beginning of.
    using UnionTy = llvm::PointerUnion<llvm::Instruction *, llvm::BasicBlock *>;
    UnionTy After;
  public:
    StableIP() = default;
    explicit StableIP(const IRBuilder &Builder) {
      if (!Builder.hasValidIP()) {
        After = UnionTy();
        assert(!isValid());
        return;
      }

      llvm::BasicBlock *curBlock = Builder.GetInsertBlock();
      assert(Builder.GetInsertPoint() == curBlock->end());
      if (curBlock->empty())
        After = curBlock;
      else
        After = &curBlock->back();
    }

    /// Does this stable IP point to a valid location?
    bool isValid() const {
      return !After.isNull();
    }

    /// Insert an unparented instruction at this insertion point.
    /// Note that inserting multiple instructions at an IP will cause
    /// them to end up in reverse order.
    void insert(llvm::Instruction *I) {
      assert(isValid() && "inserting at invalid location!");
      assert(I->getParent() == nullptr);
      if (auto *block = After.dyn_cast<llvm::BasicBlock*>()) {
        block->getInstList().push_front(I);
      } else {
        llvm::Instruction *afterInsn = After.get<llvm::Instruction*>();
        afterInsn->getParent()->getInstList().insertAfter(
            afterInsn->getIterator(), I);
      }
    }

    // Support for being placed in pointer unions.
    void *getOpaqueValue() const { return After.getOpaqueValue(); }
    static StableIP getFromOpaqueValue(void *p) {
      StableIP result;
      result.After = UnionTy::getFromOpaqueValue(p);
      return result;
    }
    enum { NumLowBitsAvailable
             = llvm::PointerLikeTypeTraits<UnionTy>::NumLowBitsAvailable };
  };

  /// Capture a stable reference to the current IP.
  StableIP getStableIP() const {
    return StableIP(*this);
  }

  /// Return the LLVM module we're inserting into.
  llvm::Module *getModule() const {
    if (auto BB = GetInsertBlock())
      return BB->getModule();
    assert(ClearedIP && "IRBuilder has no active or cleared insertion block");
    return ClearedIP->getModule();
  }

  /// Don't create allocas this way; you'll get a dynamic alloca.
  /// Use IGF::createAlloca or IGF::emitDynamicAlloca.
  llvm::Value *CreateAlloca(llvm::Type *type, llvm::Value *arraySize,
                            const llvm::Twine &name = "") = delete;

  llvm::LoadInst *CreateLoad(llvm::Value *addr, Alignment align,
                             const llvm::Twine &name = "") {
    llvm::LoadInst *load = IRBuilderBase::CreateLoad(addr, name);
    load->setAlignment(align.getValue());
    return load;
  }
  llvm::LoadInst *CreateLoad(Address addr, const llvm::Twine &name = "") {
    return CreateLoad(addr.getAddress(), addr.getAlignment(), name);
  }

  llvm::StoreInst *CreateStore(llvm::Value *value, llvm::Value *addr,
                               Alignment align) {
    llvm::StoreInst *store = IRBuilderBase::CreateStore(value, addr);
    store->setAlignment(align.getValue());
    return store;
  }
  llvm::StoreInst *CreateStore(llvm::Value *value, Address addr) {
    return CreateStore(value, addr.getAddress(), addr.getAlignment());
  }

  // These are deleted because we want to force the caller to specify
  // an alignment.
  llvm::LoadInst *CreateLoad(llvm::Value *addr,
                             const llvm::Twine &name = "") = delete;
  llvm::StoreInst *CreateStore(llvm::Value *value, llvm::Value *addr) = delete;
  
  using IRBuilderBase::CreateStructGEP;
  Address CreateStructGEP(Address address, unsigned index, Size offset,
                          const llvm::Twine &name = "") {
    llvm::Value *addr = CreateStructGEP(
        address.getType()->getElementType(), address.getAddress(),
        index, name);
    return Address(addr, address.getAlignment().alignmentAtOffset(offset));
  }
  Address CreateStructGEP(Address address, unsigned index,
                          const llvm::StructLayout *layout,
                          const llvm::Twine &name = "") {
    Size offset = Size(layout->getElementOffset(index));
    return CreateStructGEP(address, index, offset, name);
  }

  /// Given a pointer to an array element, GEP to the array element
  /// N elements past it.  The type is not changed.
  Address CreateConstArrayGEP(Address base, unsigned index, Size eltSize,
                              const llvm::Twine &name = "") {
    auto addr = CreateConstInBoundsGEP1_32(
        base.getType()->getElementType(), base.getAddress(), index, name);
    return Address(addr,
                   base.getAlignment().alignmentAtOffset(eltSize * index));
  }

  /// Given an i8*, GEP to N bytes past it.
  Address CreateConstByteArrayGEP(Address base, Size offset,
                                  const llvm::Twine &name = "") {
    auto addr = CreateConstInBoundsGEP1_32(
        base.getType()->getElementType(), base.getAddress(), offset.getValue(),
        name);
    return Address(addr, base.getAlignment().alignmentAtOffset(offset));
  }

  using IRBuilderBase::CreateBitCast;
  Address CreateBitCast(Address address, llvm::Type *type,
                        const llvm::Twine &name = "") {
    llvm::Value *addr = CreateBitCast(address.getAddress(), type, name);
    return Address(addr, address.getAlignment());
  }

  /// Cast the given address to be a pointer to the given element type,
  /// preserving the original address space.
  Address CreateElementBitCast(Address address, llvm::Type *type,
                               const llvm::Twine &name = "") {
    // Do nothing if the type doesn't change.
    auto origPtrType = address.getType();
    if (origPtrType->getElementType() == type) return address;

    // Otherwise, cast to a pointer to the correct type.
    auto ptrType = type->getPointerTo(origPtrType->getAddressSpace());
    return CreateBitCast(address, ptrType, name);
  }

  /// Insert the given basic block after the IP block and move the
  /// insertion point to it.  Only valid if the IP is valid.
  void emitBlock(llvm::BasicBlock *BB);

  using IRBuilderBase::CreateMemCpy;
  llvm::CallInst *CreateMemCpy(Address dest, Address src, Size size) {
    return CreateMemCpy(dest.getAddress(), src.getAddress(),
                        size.getValue(),
                        std::min(dest.getAlignment(),
                                 src.getAlignment()).getValue());
  }

  using IRBuilderBase::CreateMemSet;
  llvm::CallInst *CreateMemSet(Address dest, llvm::Value *value, Size size) {
    return CreateMemSet(dest.getAddress(), value, size.getValue(),
                        dest.getAlignment().getValue());
  }
  llvm::CallInst *CreateMemSet(Address dest, llvm::Value *value,
                               llvm::Value *size) {
    return CreateMemSet(dest.getAddress(), value, size,
                        dest.getAlignment().getValue());
  }
  
  using IRBuilderBase::CreateLifetimeStart;
  llvm::CallInst *CreateLifetimeStart(Address buf, Size size) {
    return CreateLifetimeStart(buf.getAddress(),
                   llvm::ConstantInt::get(Context, APInt(64, size.getValue())));
  }
  
  using IRBuilderBase::CreateLifetimeEnd;
  llvm::CallInst *CreateLifetimeEnd(Address buf, Size size) {
    return CreateLifetimeEnd(buf.getAddress(),
                   llvm::ConstantInt::get(Context, APInt(64, size.getValue())));
  }

  // We're intentionally not allowing direct use of
  // llvm::IRBuilder::CreateCall in order to push code towards using
  // FunctionPointer.

  bool isTrapIntrinsic(llvm::Value *Callee) {
    return Callee == llvm::Intrinsic::getDeclaration(getModule(),
                                                     llvm::Intrinsic::ID::trap);
  }
  bool isTrapIntrinsic(llvm::Intrinsic::ID intrinsicID) {
    return intrinsicID == llvm::Intrinsic::ID::trap;
  }

  llvm::CallInst *CreateCall(llvm::Value *Callee, ArrayRef<llvm::Value *> Args,
                             const Twine &Name = "",
                             llvm::MDNode *FPMathTag = nullptr) = delete;

  llvm::CallInst *CreateCall(llvm::FunctionType *FTy, llvm::Constant *Callee,
                             ArrayRef<llvm::Value *> Args,
                             const Twine &Name = "",
                             llvm::MDNode *FPMathTag = nullptr) {
    assert((!DebugInfo || getCurrentDebugLocation()) && "no debugloc on call");
    assert(!isTrapIntrinsic(Callee) && "Use CreateNonMergeableTrap");
    auto Call = IRBuilderBase::CreateCall(FTy, Callee, Args, Name, FPMathTag);
    setCallingConvUsingCallee(Call);
    return Call;
  }

  llvm::CallInst *CreateCall(llvm::Constant *Callee,
                             ArrayRef<llvm::Value *> Args,
                             const Twine &Name = "",
                             llvm::MDNode *FPMathTag = nullptr) {
    // assert((!DebugInfo || getCurrentDebugLocation()) && "no debugloc on
    // call");
    assert(!isTrapIntrinsic(Callee) && "Use CreateNonMergeableTrap");
    auto Call = IRBuilderBase::CreateCall(Callee, Args, Name, FPMathTag);
    setCallingConvUsingCallee(Call);
    return Call;
  }

  llvm::CallInst *CreateCall(const FunctionPointer &fn,
                             ArrayRef<llvm::Value *> args);

  llvm::CallInst *CreateAsmCall(llvm::InlineAsm *asmBlock,
                                ArrayRef<llvm::Value *> args) {
    return IRBuilderBase::CreateCall(asmBlock, args);
  }

  /// Call an intrinsic with no type arguments.
  llvm::CallInst *CreateIntrinsicCall(llvm::Intrinsic::ID intrinsicID,
                                      ArrayRef<llvm::Value *> args,
                                      const Twine &name = "") {
    assert(!isTrapIntrinsic(intrinsicID) && "Use CreateNonMergeableTrap");
    auto intrinsicFn =
      llvm::Intrinsic::getDeclaration(getModule(), intrinsicID);
    return CreateCall(intrinsicFn, args, name);
  }

  /// Call an intrinsic with type arguments.
  llvm::CallInst *CreateIntrinsicCall(llvm::Intrinsic::ID intrinsicID,
                                      ArrayRef<llvm::Type*> typeArgs,
                                      ArrayRef<llvm::Value *> args,
                                      const Twine &name = "") {
    assert(!isTrapIntrinsic(intrinsicID) && "Use CreateNonMergeableTrap");
    auto intrinsicFn =
      llvm::Intrinsic::getDeclaration(getModule(), intrinsicID, typeArgs);
    return CreateCall(intrinsicFn, args, name);
  }

  /// Call the trap intrinsic. If optimizations are enabled, an inline asm
  /// gadget is emitted before the trap. The gadget inhibits transforms which
  /// merge trap calls together, which makes debugging crashes easier.
  llvm::CallInst *CreateNonMergeableTrap(IRGenModule &IGM);

  /// Split a first-class aggregate value into its component pieces.
  template <unsigned N>
  std::array<llvm::Value *, N> CreateSplit(llvm::Value *aggregate) {
    assert(isa<llvm::StructType>(aggregate->getType()));
    assert(cast<llvm::StructType>(aggregate->getType())->getNumElements() == N);
    std::array<llvm::Value *, N> results;
    for (unsigned i = 0; i != N; ++i) {
      results[i] = CreateExtractValue(aggregate, i);
    }
    return results;
  }

  /// Combine the given values into a first-class aggregate.
  llvm::Value *CreateCombine(llvm::StructType *aggregateType,
                             ArrayRef<llvm::Value*> values) {
    assert(aggregateType->getNumElements() == values.size());
    llvm::Value *result = llvm::UndefValue::get(aggregateType);
    for (unsigned i = 0, e = values.size(); i != e; ++i) {
      result = CreateInsertValue(result, values[i], i);
    }
    return result;
  }
};

} // end namespace irgen
} // end namespace swift

namespace llvm {
  template <> struct PointerLikeTypeTraits<swift::irgen::IRBuilder::StableIP> {
    using type = swift::irgen::IRBuilder::StableIP;

  public:
    static void *getAsVoidPointer(type IP) {
      return IP.getOpaqueValue();
    }
    static type getFromVoidPointer(void *p) {
      return type::getFromOpaqueValue(p);
    }

    // The number of bits available are the min of the two pointer types.
    enum {
      NumLowBitsAvailable = type::NumLowBitsAvailable
    };
  };
}

#endif
