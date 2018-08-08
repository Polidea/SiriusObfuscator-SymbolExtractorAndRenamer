//===--- Existential.cpp - Functions analyzing existentials.  -------------===//
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

#include "swift/SILOptimizer/Utils/Existential.h"
#include "swift/SILOptimizer/Utils/CFG.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace swift;

/// Determine the pattern for global_addr.
/// %3 = global_addr @$P : $*SomeP
/// %4 = init_existential_addr %3 : $*SomeP, $SomeC
/// %5 = alloc_ref $SomeC
/// store %5 to %4 : $*SomeC
/// %8 = alloc_stack $SomeP
/// copy_addr %3 to [initialization] %8 : $*SomeP
/// %9 = open_existential_addr immutable_access %8 : $*SomeP to $*@opened SomeP
SILValue swift::findInitExistentialFromGlobalAddr(GlobalAddrInst *GAI,
                                                  CopyAddrInst *CAI) {
  assert(CAI->getSrc() == SILValue(GAI) &&
         "Broken Assumption! Global Addr is not the source of the passed in "
         "copy_addr?!");

  /// Check for a single InitExistential usage for GAI and
  /// a simple dominance check: both InitExistential and CAI are in
  /// the same basic block and only one InitExistential
  /// occurs between GAI and CAI.
  llvm::SmallPtrSet<SILInstruction *, 8> IEUses;
  for (auto *Use : GAI->getUses()) {
    if (auto *InitExistential =
            dyn_cast<InitExistentialAddrInst>(Use->getUser())) {
      IEUses.insert(InitExistential);
    }
  }

  /// No InitExistential found in the basic block.
  if (IEUses.empty())
    return SILValue();

  /// Walk backwards from CAI instruction till the begining of the basic block
  /// looking for InitExistential.
  SILValue SingleIE;
  for (auto II = CAI->getIterator().getReverse(), IE = CAI->getParent()->rend();
       II != IE; ++II) {
    if (!IEUses.count(&*II))
      continue;
    if (SingleIE)
      return SILValue();

    SingleIE = cast<InitExistentialAddrInst>(&*II);
  }
  return SingleIE;
}

/// Returns the address of an object with which the stack location \p ASI is
/// initialized. This is either a init_existential_addr or the destination of a
/// copy_addr. Returns a null value if the address does not dominate the
/// alloc_stack user \p ASIUser.
/// If the value is copied from another stack location, \p isCopied is set to
/// true.
SILValue swift::getAddressOfStackInit(AllocStackInst *ASI,
                                      SILInstruction *ASIUser, bool &isCopied) {
  SILInstruction *SingleWrite = nullptr;
  // Check that this alloc_stack is initialized only once.
  for (auto Use : ASI->getUses()) {
    auto *User = Use->getUser();

    // Ignore instructions which don't write to the stack location.
    // Also ignore ASIUser (only kicks in if ASIUser is the original apply).
    if (isa<DeallocStackInst>(User) || isa<DebugValueAddrInst>(User) ||
        isa<DestroyAddrInst>(User) || isa<WitnessMethodInst>(User) ||
        isa<DeinitExistentialAddrInst>(User) ||
        isa<OpenExistentialAddrInst>(User) || User == ASIUser) {
      continue;
    }
    if (auto *CAI = dyn_cast<CopyAddrInst>(User)) {
      if (CAI->getDest() == ASI) {
        if (SingleWrite)
          return SILValue();
        SingleWrite = CAI;
        isCopied = true;
      }
      continue;
    }
    if (isa<InitExistentialAddrInst>(User)) {
      if (SingleWrite)
        return SILValue();
      SingleWrite = User;
      continue;
    }
    if (isa<ApplyInst>(User) || isa<TryApplyInst>(User)) {
      // Ignore function calls which do not write to the stack location.
      auto Idx =
          Use->getOperandNumber() - ApplyInst::getArgumentOperandNumber();
      auto Conv = FullApplySite(User).getArgumentConvention(Idx);
      if (Conv != SILArgumentConvention::Indirect_In &&
          Conv != SILArgumentConvention::Indirect_In_Guaranteed)
        return SILValue();
      continue;
    }
    // Bail if there is any unknown (and potentially writing) instruction.
    return SILValue();
  }
  if (!SingleWrite)
    return SILValue();

  // A very simple dominance check. As ASI is an operand of ASIUser,
  // SingleWrite dominates ASIUser if it is in the same block as ASI or ASIUser.
  SILBasicBlock *BB = SingleWrite->getParent();
  if (BB != ASI->getParent() && BB != ASIUser->getParent())
    return SILValue();

  if (auto *CAI = dyn_cast<CopyAddrInst>(SingleWrite)) {
    // Try to derive the type from the copy_addr that was used to
    // initialize the alloc_stack.
    assert(isCopied && "isCopied not set for a copy_addr");
    SILValue CAISrc = CAI->getSrc();
    if (auto *ASI = dyn_cast<AllocStackInst>(CAISrc))
      return getAddressOfStackInit(ASI, CAI, isCopied);
    // Check if the CAISrc is a global_addr.
    if (auto *GAI = dyn_cast<GlobalAddrInst>(CAISrc)) {
      return findInitExistentialFromGlobalAddr(GAI, CAI);
    }
    return CAISrc;
  }
  return cast<InitExistentialAddrInst>(SingleWrite);
}

/// Find the init_existential, which could be used to determine a concrete
/// type of the \p Self.
/// If the value is copied from another stack location, \p isCopied is set to
/// true.
SILInstruction *swift::findInitExistential(Operand &openedUse,
                                           ArchetypeType *&OpenedArchetype,
                                           SILValue &OpenedArchetypeDef,
                                           bool &isCopied) {
  SILValue Self = openedUse.get();
  SILInstruction *User = openedUse.getUser();
  isCopied = false;
  if (auto *Instance = dyn_cast<AllocStackInst>(Self)) {
    // In case the Self operand is an alloc_stack where a copy_addr copies the
    // result of an open_existential_addr to this stack location.
    if (SILValue Src = getAddressOfStackInit(Instance, User, isCopied))
      Self = Src;
  }

  if (auto *Open = dyn_cast<OpenExistentialAddrInst>(Self)) {
    auto Op = Open->getOperand();
    auto *ASI = dyn_cast<AllocStackInst>(Op);
    if (!ASI)
      return nullptr;

    SILValue StackWrite = getAddressOfStackInit(ASI, Open, isCopied);
    if (!StackWrite)
      return nullptr;

    auto *IE = dyn_cast<InitExistentialAddrInst>(StackWrite);
    if (!IE)
      return nullptr;

    OpenedArchetype = Open->getType().castTo<ArchetypeType>();
    OpenedArchetypeDef = Open;
    return IE;
  }

  if (auto *Open = dyn_cast<OpenExistentialRefInst>(Self)) {
    if (auto *IE = dyn_cast<InitExistentialRefInst>(Open->getOperand())) {
      OpenedArchetype = Open->getType().castTo<ArchetypeType>();
      OpenedArchetypeDef = Open;
      return IE;
    }
    return nullptr;
  }

  if (auto *Open = dyn_cast<OpenExistentialMetatypeInst>(Self)) {
    if (auto *IE = dyn_cast<InitExistentialMetatypeInst>(Open->getOperand())) {
      auto Ty = Open->getType().getASTType();
      while (auto Metatype = dyn_cast<MetatypeType>(Ty))
        Ty = Metatype.getInstanceType();
      OpenedArchetype = cast<ArchetypeType>(Ty);
      OpenedArchetypeDef = Open;
      return IE;
    }
    return nullptr;
  }
  return nullptr;
}

/// Derive a concrete type of self and conformance from the init_existential
/// instruction.
/// If successful, initializes a valid ConformanceAndConcreteType.
ConcreteExistentialInfo::ConcreteExistentialInfo(Operand &openedUse) {
  // Try to find the init_existential, which could be used to
  // determine a concrete type of the self.
  // Returns: InitExistential, OpenedArchetype, OpenedArchetypeDef, isCopied.
  InitExistential = findInitExistential(openedUse, OpenedArchetype,
                                        OpenedArchetypeDef, isCopied);
  if (!InitExistential)
    return;

  if (auto IE = dyn_cast<InitExistentialAddrInst>(InitExistential)) {
    ExistentialType = IE->getOperand()->getType().getASTType();
    ExistentialConformances = IE->getConformances();
    ConcreteType = IE->getFormalConcreteType();
    ConcreteValue = IE;
  } else if (auto IER = dyn_cast<InitExistentialRefInst>(InitExistential)) {
    ExistentialType = IER->getType().getASTType();
    ExistentialConformances = IER->getConformances();
    ConcreteType = IER->getFormalConcreteType();
    ConcreteValue = IER->getOperand();
  } else if (auto IEM =
                 dyn_cast<InitExistentialMetatypeInst>(InitExistential)) {
    ExistentialType = IEM->getType().getASTType();
    ExistentialConformances = IEM->getConformances();
    ConcreteValue = IEM->getOperand();
    ConcreteType = ConcreteValue->getType().getASTType();
    while (auto InstanceType =
               dyn_cast<ExistentialMetatypeType>(ExistentialType)) {
      ExistentialType = InstanceType.getInstanceType();
      ConcreteType = cast<MetatypeType>(ConcreteType).getInstanceType();
    }
  } else {
    assert(!isValid());
    return;
  }
  // Construct a single-generic-parameter substitution map directly to the
  // ConcreteType with this existential's full list of conformances.
  SILModule &M = InitExistential->getModule();
  CanGenericSignature ExistentialSig =
      M.getASTContext().getExistentialSignature(ExistentialType,
                                                M.getSwiftModule());
  ExistentialSubs = ExistentialSig->getSubstitutionMap(
    {Substitution(ConcreteType, ExistentialConformances)});

  // If the concrete type is another existential, we're "forwarding" an
  // opened existential type, so we must keep track of the original
  // defining instruction.
  if (ConcreteType->isOpenedExistential()) {
    if (InitExistential->getTypeDependentOperands().empty()) {
      auto op = InitExistential->getOperand(0);
      assert(op->getType().hasOpenedExistential()
             && "init_existential is supposed to have a typedef operand");
      ConcreteTypeDef = cast<SingleValueInstruction>(op);
    } else {
      ConcreteTypeDef = cast<SingleValueInstruction>(
          InitExistential->getTypeDependentOperands()[0].get());
    }
  }
  assert(isValid());
}
