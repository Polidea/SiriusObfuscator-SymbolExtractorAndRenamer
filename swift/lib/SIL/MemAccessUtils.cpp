//===--- MemAccessUtils.cpp - Utilities for SIL memory access. ------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-access-utils"

#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/SILUndef.h"

using namespace swift;

AccessedStorage::Kind AccessedStorage::classify(SILValue base) {
  switch (base->getKind()) {
  // An AllocBox is a fully identified memory location.
  case ValueKind::AllocBoxInst:
    return Box;
  // An AllocStack is a fully identified memory location, which may occur
  // after inlining code already subjected to stack promotion.
  case ValueKind::AllocStackInst:
    return Stack;
  case ValueKind::GlobalAddrInst:
    return Global;
  case ValueKind::RefElementAddrInst:
    return Class;
  // A function argument is effectively a nested access, enforced
  // independently in the caller and callee.
  case ValueKind::SILFunctionArgument:
    return Argument;
  // View the outer begin_access as a separate location because nested
  // accesses do not conflict with each other.
  case ValueKind::BeginAccessInst:
    return Nested;
  default:
    return Unidentified;
  }
}

AccessedStorage::AccessedStorage(SILValue base, Kind kind) : kind(kind) {
  assert(base && "invalid storage base");

  switch (kind) {
  case Box:
    assert(isa<AllocBoxInst>(base));
    value = base;
    break;
  case Stack:
    assert(isa<AllocStackInst>(base));
    value = base;
    break;
  case Nested:
    assert(isa<BeginAccessInst>(base));
    value = base;
    break;
  case Unidentified:
    value = base;
    break;
  case Argument:
    paramIndex = cast<SILFunctionArgument>(base)->getIndex();
    break;
  case Global:
    global = cast<GlobalAddrInst>(base)->getReferencedGlobal();
    break;
  case Class: {
    // Do a best-effort to find the identity of the object being projected
    // from. It is OK to be unsound here (i.e. miss when two ref_element_addrs
    // actually refer the same address) because these will be dynamically
    // checked.
    auto *REA = cast<RefElementAddrInst>(base);
    SILValue Object = stripBorrow(REA->getOperand());
    objProj = ObjectProjection(Object, Projection(REA));
  }
  }
}

const ValueDecl *AccessedStorage::getDecl(SILFunction *F) const {
  switch(kind) {
  case Box:
    return cast<AllocBoxInst>(value)->getLoc().getAsASTNode<VarDecl>();

  case Stack:
    return cast<AllocStackInst>(value)->getDecl();

  case Global:
    return global->getDecl();

  case Class:
    return objProj.getProjection().getVarDecl(objProj.getObject()->getType());

  case Argument:
    return getArgument(F)->getDecl();

  case Nested:
    return nullptr;

  case Unidentified:
    return nullptr;
  }
}

// An address base is a block argument. Verify that it is actually a box
// projected from a switch_enum.
static void checkSwitchEnumBlockArg(SILPHIArgument *arg) {
  assert(!arg->getType().isAddress());
  SILBasicBlock *Pred = arg->getParent()->getSinglePredecessorBlock();
  if (!Pred || !isa<SwitchEnumInst>(Pred->getTerminator())) {
    arg->dump();
    llvm_unreachable("unexpected box source.");
  }
}

/// Return true if the given address value is produced by a special address
/// producer that is only used for local initialization, not formal access.
static bool isAddressForLocalInitOnly(SILValue sourceAddr) {
  switch (sourceAddr->getKind()) {
  default:
    return false;

  // Value to address conversions: the operand is the non-address source
  // value. These allow local mutation of the value but should never be used
  // for formal access of an lvalue.
  case ValueKind::OpenExistentialBoxInst:
  case ValueKind::ProjectExistentialBoxInst:
    return true;

  // Self-evident local initialization.
  case ValueKind::InitEnumDataAddrInst:
  case ValueKind::InitExistentialAddrInst:
  case ValueKind::AllocExistentialBoxInst:
  case ValueKind::AllocValueBufferInst:
  case ValueKind::ProjectValueBufferInst:
    return true;
  }
}

AccessedStorage swift::findAccessedStorage(SILValue sourceAddr) {
  SILValue address = sourceAddr;
  while (true) {
    AccessedStorage::Kind kind = AccessedStorage::classify(address);
    // First handle identified cases: these are always valid as the base of a
    // formal access.
    if (kind != AccessedStorage::Unidentified)
      return AccessedStorage(address, kind);

    // Handle other unidentified address sources.
    switch (address->getKind()) {
    default:
      if (isAddressForLocalInitOnly(address))
        return AccessedStorage(address, AccessedStorage::Unidentified);
      return AccessedStorage();

    case ValueKind::PointerToAddressInst:
    case ValueKind::SILUndef:
      return AccessedStorage(address, AccessedStorage::Unidentified);

    // A block argument may be a box value projected out of
    // switch_enum. Address-type block arguments are not allowed.
    case ValueKind::SILPHIArgument:
      checkSwitchEnumBlockArg(cast<SILPHIArgument>(address));
      return AccessedStorage(address, AccessedStorage::Unidentified);

    // Load a box from an indirect payload of an opaque enum.
    // We must have peeked past the project_box earlier in this loop.
    // (the indirectness makes it a box, the load is for address-only).
    // 
    // %payload_adr = unchecked_take_enum_data_addr %enum : $*Enum, #Enum.case
    // %box = load [take] %payload_adr : $*{ var Enum }
    //
    // FIXME: this case should go away with opaque values.
    case ValueKind::LoadInst: {
      assert(address->getType().is<SILBoxType>());
      address = cast<LoadInst>(address)->getOperand();
      assert(isa<UncheckedTakeEnumDataAddrInst>(address));
      continue;
    }
    // Inductive cases:
    // Look through address casts to find the source address.
    case ValueKind::MarkUninitializedInst:
    case ValueKind::OpenExistentialAddrInst:
    case ValueKind::UncheckedAddrCastInst:
    // Inductive cases that apply to any type.
    case ValueKind::CopyValueInst:
    case ValueKind::MarkDependenceInst:
    // Look through a project_box to identify the underlying alloc_box as the
    // accesed object. It must be possible to reach either the alloc_box or the
    // containing enum in this loop, only looking through simple value
    // propagation such as copy_value.
    case ValueKind::ProjectBoxInst:
    // Handle project_block_storage just like project_box.
    case ValueKind::ProjectBlockStorageInst:
    // Look through begin_borrow in case a local box is borrowed.
    case ValueKind::BeginBorrowInst:
      address = cast<SingleValueInstruction>(address)->getOperand(0);
      continue;

    // Subobject projections.
    case ValueKind::StructElementAddrInst:
    case ValueKind::TupleElementAddrInst:
    case ValueKind::UncheckedTakeEnumDataAddrInst:
    case ValueKind::RefTailAddrInst:
    case ValueKind::TailAddrInst:
    case ValueKind::IndexAddrInst:
      address = cast<SingleValueInstruction>(address)->getOperand(0);
      continue;
    }
  }
}

AccessedStorage swift::findAccessedStorageOrigin(SILValue sourceAddr) {
  while (true) {
    const AccessedStorage &storage = findAccessedStorage(sourceAddr);
    if (!storage || storage.getKind() != AccessedStorage::Nested)
      return storage;
    
    sourceAddr = cast<BeginAccessInst>(storage.getValue())->getSource();
  }
}

// Return true if the given access is on a 'let' lvalue.
static bool isLetAccess(const AccessedStorage &storage, SILFunction *F) {
  if (auto *decl = dyn_cast_or_null<VarDecl>(storage.getDecl(F)))
    return decl->isLet();

  // It's unclear whether a global will ever be missing it's varDecl, but
  // technically we only preserve it for debug info. So if we don't have a decl,
  // check the flag on SILGlobalVariable, which is guaranteed valid, 
  if (storage.getKind() == AccessedStorage::Global)
    return storage.getGlobal()->isLet();

  return false;
}

static bool isScratchBuffer(SILValue value) {
  // Special case unsafe value buffer access.
  return isa<BuiltinUnsafeValueBufferType>(
    value->getType().getSwiftRValueType());
}

bool swift::memInstMustInitialize(Operand *memOper) {
  SILValue address = memOper->get();
  SILInstruction *memInst = memOper->getUser();

  switch (memInst->getKind()) {
  default:
    return false;

  case SILInstructionKind::CopyAddrInst: {
    auto *CAI = cast<CopyAddrInst>(memInst);
    return CAI->getDest() == address && CAI->isInitializationOfDest();
  }
  case SILInstructionKind::InitExistentialAddrInst:
  case SILInstructionKind::InitEnumDataAddrInst:
  case SILInstructionKind::InjectEnumAddrInst:
    return true;

  case SILInstructionKind::StoreInst:
    return cast<StoreInst>(memInst)->getOwnershipQualifier()
           == StoreOwnershipQualifier::Init;

  case SILInstructionKind::StoreWeakInst:
    return cast<StoreWeakInst>(memInst)->isInitializationOfDest();

  case SILInstructionKind::StoreUnownedInst:
    return cast<StoreUnownedInst>(memInst)->isInitializationOfDest();
  }
}

bool swift::isPossibleFormalAccessBase(const AccessedStorage &storage,
                                       SILFunction *F) {
  switch (storage.getKind()) {
  case AccessedStorage::Box:
  case AccessedStorage::Stack:
    if (isScratchBuffer(storage.getValue()))
      return false;
    break;
  case AccessedStorage::Global:
    break;
  case AccessedStorage::Class:
    break;
  case AccessedStorage::Argument:
    // Function arguments are accessed by the caller.
    return false;
  case AccessedStorage::Nested: {
    // A begin_access is considered a separate base for the purpose of conflict
    // checking. However, for the purpose of inserting unenforced markers and
    // performaing verification, it needs to be ignored.
    auto *BAI = cast<BeginAccessInst>(storage.getValue());
    const AccessedStorage &nestedStorage =
      findAccessedStorage(BAI->getSource());
    if (!nestedStorage)
      return false;

    return isPossibleFormalAccessBase(nestedStorage, F);
  }
  case AccessedStorage::Unidentified:
    if (isAddressForLocalInitOnly(storage.getValue()))
      return false;

    if (isa<SILPHIArgument>(storage.getValue())) {
      checkSwitchEnumBlockArg(cast<SILPHIArgument>(storage.getValue()));
      return false;
    }
    // Pointer-to-address exclusivity cannot be enforced. `baseAddress` may be
    // pointing anywhere within an object.
    if (isa<PointerToAddressInst>(storage.getValue()))
      return false;

    if (isa<SILUndef>(storage.getValue()))
      return false;

    if (isScratchBuffer(storage.getValue()))
      return false;
  }
  // Additional checks that apply to anything that may fal through.

  // Immutable values are only accessed for initialization.
  if (isLetAccess(storage, F))
    return false;

  return true;
}

/// Helper for visitApplyAccesses that visits address-type call arguments,
/// including arguments to @noescape functions that are passed as closures to
/// the current call.
static void visitApplyAccesses(ApplySite apply,
                               std::function<void(Operand *)> visitor) {
  for (Operand &oper : apply.getArgumentOperands()) {
    // Consider any address-type operand an access. Whether it is read or modify
    // depends on the argument convention.
    if (oper.get()->getType().isAddress()) {
      visitor(&oper);
      continue;
    }
    auto fnType = oper.get()->getType().getAs<SILFunctionType>();
    if (!fnType || !fnType->isNoEscape())
      continue;

    // When @noescape function closures are passed as arguments, their
    // arguments are considered accessed at the call site.
    FindClosureResult result = findClosureForAppliedArg(oper.get());
    if (!result.PAI)
      continue;

    // Recursively visit @noescape function closure arguments.
    visitApplyAccesses(result.PAI, visitor);
  }
}

static void visitBuiltinAddress(BuiltinInst *builtin,
                                std::function<void(Operand *)> visitor) {
  if (auto kind = builtin->getBuiltinKind()) {
    switch (kind.getValue()) {
    default:
      builtin->dump();
      llvm_unreachable("unexpected bulitin memory access.");

      // Buitins that affect memory but can't be formal accesses.
    case BuiltinValueKind::UnexpectedError:
    case BuiltinValueKind::ErrorInMain:
    case BuiltinValueKind::IsOptionalType:
    case BuiltinValueKind::AllocRaw:
    case BuiltinValueKind::DeallocRaw:
    case BuiltinValueKind::Fence:
    case BuiltinValueKind::StaticReport:
    case BuiltinValueKind::Once:
    case BuiltinValueKind::OnceWithContext:
    case BuiltinValueKind::Unreachable:
    case BuiltinValueKind::CondUnreachable:
    case BuiltinValueKind::DestroyArray:
    case BuiltinValueKind::UnsafeGuaranteed:
    case BuiltinValueKind::UnsafeGuaranteedEnd:
    case BuiltinValueKind::Swift3ImplicitObjCEntrypoint:
    case BuiltinValueKind::TSanInoutAccess:
      return;

      // General memory access to a pointer in first operand position.
    case BuiltinValueKind::CmpXChg:
    case BuiltinValueKind::AtomicLoad:
    case BuiltinValueKind::AtomicStore:
    case BuiltinValueKind::AtomicRMW:
      // Currently ignored because the access is on a RawPointer, not a
      // SIL address.
      // visitor(&builtin->getAllOperands()[0]);
      return;

      // Arrays: (T.Type, Builtin.RawPointer, Builtin.RawPointer,
      // Builtin.Word)
    case BuiltinValueKind::CopyArray:
    case BuiltinValueKind::TakeArrayNoAlias:
    case BuiltinValueKind::TakeArrayFrontToBack:
    case BuiltinValueKind::TakeArrayBackToFront:
    case BuiltinValueKind::AssignCopyArrayNoAlias:
    case BuiltinValueKind::AssignCopyArrayFrontToBack:
    case BuiltinValueKind::AssignCopyArrayBackToFront:
    case BuiltinValueKind::AssignTakeArray:
      // Currently ignored because the access is on a RawPointer.
      // visitor(&builtin->getAllOperands()[1]);
      // visitor(&builtin->getAllOperands()[2]);
      return;
    }
  }
  if (auto ID = builtin->getIntrinsicID()) {
    switch (ID.getValue()) {
      // Exhaustively verifying all LLVM instrinsics that access memory is
      // impractical. Instead, we call out the few common cases and return in
      // the default case.
    default:
      return;
    case llvm::Intrinsic::memcpy:
    case llvm::Intrinsic::memmove:
      // Currently ignored because the access is on a RawPointer.
      // visitor(&builtin->getAllOperands()[0]);
      // visitor(&builtin->getAllOperands()[1]);
      return;
    case llvm::Intrinsic::memset:
      // Currently ignored because the access is on a RawPointer.
      // visitor(&builtin->getAllOperands()[0]);
      return;
    }
  }
  llvm_unreachable("Must be either a builtin or intrinsic.");
}

void swift::visitAccessedAddress(SILInstruction *I,
                                 std::function<void(Operand *)> visitor) {
  assert(I->mayReadOrWriteMemory());

  // Reference counting instructions do not access user visible memory.
  if (isa<RefCountingInst>(I))
    return;

  if (isa<DeallocationInst>(I))
    return;

  if (auto apply = FullApplySite::isa(I)) {
    visitApplyAccesses(apply, visitor);
    return;
  }

  if (auto builtin = dyn_cast<BuiltinInst>(I)) {
    visitBuiltinAddress(builtin, visitor);
    return;
  }

  switch (I->getKind()) {
  default:
    I->dump();
    llvm_unreachable("unexpected memory access.");

  case SILInstructionKind::AssignInst:
    visitor(&I->getAllOperands()[AssignInst::Dest]);
    return;

  case SILInstructionKind::CheckedCastAddrBranchInst:
    visitor(&I->getAllOperands()[CheckedCastAddrBranchInst::Src]);
    visitor(&I->getAllOperands()[CheckedCastAddrBranchInst::Dest]);
    return;

  case SILInstructionKind::CopyAddrInst:
    visitor(&I->getAllOperands()[CopyAddrInst::Src]);
    visitor(&I->getAllOperands()[CopyAddrInst::Dest]);
    return;

  case SILInstructionKind::StoreInst:
  case SILInstructionKind::StoreBorrowInst:
  case SILInstructionKind::StoreUnownedInst:
  case SILInstructionKind::StoreWeakInst:
    visitor(&I->getAllOperands()[StoreInst::Dest]);
    return;

  case SILInstructionKind::SelectEnumAddrInst:
    visitor(&I->getAllOperands()[0]);
    return;

  case SILInstructionKind::InitExistentialAddrInst:
  case SILInstructionKind::InjectEnumAddrInst:
  case SILInstructionKind::LoadInst:
  case SILInstructionKind::LoadBorrowInst:
  case SILInstructionKind::LoadWeakInst:
  case SILInstructionKind::LoadUnownedInst:
  case SILInstructionKind::OpenExistentialAddrInst:
  case SILInstructionKind::SwitchEnumAddrInst:
  case SILInstructionKind::UncheckedTakeEnumDataAddrInst:
  case SILInstructionKind::UnconditionalCheckedCastInst: {
    // Assuming all the above have only a single address operand.
    assert(I->getNumOperands() - I->getNumTypeDependentOperands() == 1);
    Operand *singleOperand = &I->getAllOperands()[0];
    // Check the operand type because UnconditionalCheckedCastInst may operate
    // on a non-address.
    if (singleOperand->get()->getType().isAddress())
      visitor(singleOperand);
    return;
  }
  // Non-access cases: these are marked with memory side effects, but, by
  // themselves, do not access formal memory.
  case SILInstructionKind::AbortApplyInst:
  case SILInstructionKind::AllocBoxInst:
  case SILInstructionKind::AllocExistentialBoxInst:
  case SILInstructionKind::AllocGlobalInst:
  case SILInstructionKind::BeginAccessInst:
  case SILInstructionKind::BeginApplyInst:
  case SILInstructionKind::BeginBorrowInst:
  case SILInstructionKind::BeginUnpairedAccessInst:
  case SILInstructionKind::BindMemoryInst:
  case SILInstructionKind::CheckedCastValueBranchInst:
  case SILInstructionKind::CondFailInst:
  case SILInstructionKind::CopyBlockInst:
  case SILInstructionKind::CopyBlockWithoutEscapingInst:
  case SILInstructionKind::CopyValueInst:
  case SILInstructionKind::CopyUnownedValueInst:
  case SILInstructionKind::DeinitExistentialAddrInst:
  case SILInstructionKind::DeinitExistentialValueInst:
  case SILInstructionKind::DestroyAddrInst:
  case SILInstructionKind::DestroyValueInst:
  case SILInstructionKind::EndAccessInst:
  case SILInstructionKind::EndApplyInst:
  case SILInstructionKind::EndBorrowArgumentInst:
  case SILInstructionKind::EndBorrowInst:
  case SILInstructionKind::EndUnpairedAccessInst:
  case SILInstructionKind::EndLifetimeInst:
  case SILInstructionKind::ExistentialMetatypeInst:
  case SILInstructionKind::FixLifetimeInst:
  case SILInstructionKind::InitExistentialValueInst:
  case SILInstructionKind::IsUniqueInst:
  case SILInstructionKind::IsEscapingClosureInst:
  case SILInstructionKind::IsUniqueOrPinnedInst:
  case SILInstructionKind::KeyPathInst:
  case SILInstructionKind::OpenExistentialBoxInst:
  case SILInstructionKind::OpenExistentialBoxValueInst:
  case SILInstructionKind::OpenExistentialValueInst:
  case SILInstructionKind::PartialApplyInst:
  case SILInstructionKind::ProjectValueBufferInst:
  case SILInstructionKind::StrongPinInst:
  case SILInstructionKind::YieldInst:
  case SILInstructionKind::UnwindInst:
  case SILInstructionKind::UncheckedOwnershipConversionInst:
  case SILInstructionKind::UncheckedRefCastAddrInst:
  case SILInstructionKind::UnconditionalCheckedCastAddrInst:
  case SILInstructionKind::UnconditionalCheckedCastValueInst:
  case SILInstructionKind::UnownedReleaseInst:
  case SILInstructionKind::UnownedRetainInst:
  case SILInstructionKind::ValueMetatypeInst:
    return;
  }
}
