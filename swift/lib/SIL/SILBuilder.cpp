//===--- SILBuilder.cpp - Class for creating SIL Constructs ---------------===//
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

#include "swift/AST/Expr.h"
#include "swift/SIL/SILBuilder.h"

using namespace swift;

//===----------------------------------------------------------------------===//
// SILBuilder Implementation
//===----------------------------------------------------------------------===//

IntegerLiteralInst *SILBuilder::createIntegerLiteral(IntegerLiteralExpr *E) {
  return insert(IntegerLiteralInst::create(E, getSILDebugLocation(E), F));
}

FloatLiteralInst *SILBuilder::createFloatLiteral(FloatLiteralExpr *E) {
  return insert(FloatLiteralInst::create(E, getSILDebugLocation(E), F));
}

TupleInst *SILBuilder::createTuple(SILLocation loc, ArrayRef<SILValue> elts) {
  // Derive the tuple type from the elements.
  SmallVector<TupleTypeElt, 4> eltTypes;
  for (auto elt : elts)
    eltTypes.push_back(elt->getType().getSwiftRValueType());
  auto tupleType = SILType::getPrimitiveObjectType(
      CanType(TupleType::get(eltTypes, F.getASTContext())));

  return createTuple(loc, tupleType, elts);
}

SILType SILBuilder::getPartialApplyResultType(SILType origTy, unsigned argCount,
                                        SILModule &M,
                                        SubstitutionList subs,
                                        ParameterConvention calleeConvention) {
  CanSILFunctionType FTI = origTy.castTo<SILFunctionType>();
  if (!subs.empty())
    FTI = FTI->substGenericArgs(M, subs);
  
  assert(!FTI->isPolymorphic()
         && "must provide substitutions for generic partial_apply");
  auto params = FTI->getParameters();
  auto newParams = params.slice(0, params.size() - argCount);

  auto extInfo = FTI->getExtInfo().withRepresentation(
      SILFunctionType::Representation::Thick);

  // If the original method has an @unowned_inner_pointer return, the partial
  // application thunk will lifetime-extend 'self' for us, converting the
  // return value to @unowned.
  //
  // If the original method has an @autoreleased return, the partial application
  // thunk will retain it for us, converting the return value to @owned.
  SmallVector<SILResultInfo, 4> results;
  results.append(FTI->getResults().begin(), FTI->getResults().end());
  for (auto &result : results) {
    if (result.getConvention() == ResultConvention::UnownedInnerPointer)
      result = SILResultInfo(result.getType(), ResultConvention::Unowned);
    else if (result.getConvention() == ResultConvention::Autoreleased)
      result = SILResultInfo(result.getType(), ResultConvention::Owned);
  }

  auto appliedFnType = SILFunctionType::get(nullptr, extInfo,
                                            calleeConvention,
                                            newParams,
                                            results,
                                            FTI->getOptionalErrorResult(),
                                            M.getASTContext());

  return SILType::getPrimitiveObjectType(appliedFnType);
}

// If legal, create an unchecked_ref_cast from the given operand and result
// type, otherwise return null.
SILInstruction *SILBuilder::tryCreateUncheckedRefCast(SILLocation Loc,
                                                      SILValue Op,
                                                      SILType ResultTy) {
  auto &M = F.getModule();
  if (!SILType::canRefCast(Op->getType(), ResultTy, M))
    return nullptr;

  return insert(UncheckedRefCastInst::create(getSILDebugLocation(Loc), Op,
                                             ResultTy, F, OpenedArchetypes));
}

// Create the appropriate cast instruction based on result type.
SILInstruction *SILBuilder::createUncheckedBitCast(SILLocation Loc,
                                                   SILValue Op,
                                                   SILType Ty) {
  auto &M = F.getModule();
  if (Ty.isTrivial(M))
    return insert(UncheckedTrivialBitCastInst::create(
        getSILDebugLocation(Loc), Op, Ty, F, OpenedArchetypes));

  if (auto refCast = tryCreateUncheckedRefCast(Loc, Op, Ty))
    return refCast;

  // The destination type is nontrivial, and may be smaller than the source
  // type, so RC identity cannot be assumed.
  return insert(UncheckedBitwiseCastInst::create(getSILDebugLocation(Loc), Op,
                                                 Ty, F, OpenedArchetypes));
}

BranchInst *SILBuilder::createBranch(SILLocation Loc,
                                     SILBasicBlock *TargetBlock,
                                     OperandValueArrayRef Args) {
  SmallVector<SILValue, 6> ArgsCopy;
  ArgsCopy.reserve(Args.size());
  for (auto I = Args.begin(), E = Args.end(); I != E; ++I)
    ArgsCopy.push_back(*I);
  return createBranch(Loc, TargetBlock, ArgsCopy);
}

/// \brief Branch to the given block if there's an active insertion point,
/// then move the insertion point to the end of that block.
void SILBuilder::emitBlock(SILBasicBlock *BB, SILLocation BranchLoc) {
  if (!hasValidInsertionPoint()) {
    return emitBlock(BB);
  }

  // Fall though from the currently active block into the given block.
  assert(BB->args_empty() && "cannot fall through to bb with args");

  // This is a fall through into BB, emit the fall through branch.
  createBranch(BranchLoc, BB);

  // Start inserting into that block.
  setInsertionPoint(BB);
}

/// splitBlockForFallthrough - Prepare for the insertion of a terminator.  If
/// the builder's insertion point is at the end of the current block (as when
/// SILGen is creating the initial code for a function), just create and
/// return a new basic block that will be later used for the continue point.
///
/// If the insertion point is valid (i.e., pointing to an existing
/// instruction) then split the block at that instruction and return the
/// continuation block.
SILBasicBlock *SILBuilder::splitBlockForFallthrough() {
  // If we are concatenating, just create and return a new block.
  if (insertingAtEndOfBlock()) {
    return F.createBasicBlock(BB);
  }

  // Otherwise we need to split the current block at the insertion point.
  auto *NewBB = BB->split(InsertPt);
  InsertPt = BB->end();
  return NewBB;
}

static bool setAccessToDeinit(BeginAccessInst *beginAccess) {
  // It's possible that AllocBoxToStack could catch some cases that
  // AccessEnforcementSelection does not promote to [static]. Ultimately, this
  // should be an assert, but only after we the two passes can be fixed to share
  // a common analysis.
  if (beginAccess->getEnforcement() == SILAccessEnforcement::Dynamic)
    return false;

  beginAccess->setAccessKind(SILAccessKind::Deinit);
  return true;
}

PointerUnion<CopyAddrInst *, DestroyAddrInst *>
SILBuilder::emitDestroyAddr(SILLocation Loc, SILValue Operand) {
  // Check to see if the instruction immediately before the insertion point is a
  // copy_addr from the specified operand.  If so, we can fold this into the
  // copy_addr as a take.
  BeginAccessInst *beginAccess = nullptr;
  CopyAddrInst *copyAddrTake = nullptr;
  auto I = getInsertionPoint(), BBStart = getInsertionBB()->begin();
  while (I != BBStart) {
    auto *Inst = &*--I;

    if (auto CA = dyn_cast<CopyAddrInst>(Inst)) {
      if (!CA->isTakeOfSrc()) {
        if (CA->getSrc() == Operand && !CA->isTakeOfSrc()) {
          CA->setIsTakeOfSrc(IsTake);
          return CA;
        }
        // If this copy_addr is accessing the same source, continue searching
        // backward until we see the begin_access. If any side effects occur
        // between the `%adr = begin_access %src` and `copy_addr %adr` then we
        // cannot promote the access to a deinit. `[deinit]` requires exclusive
        // access, but an instruction with side effects may require shared
        // access.
        if (CA->getSrc() == beginAccess) {
          copyAddrTake = CA;
          continue;
        }
      }
    }

    // If we've already seen a copy_addr that can be convert to `take`, then
    // stop at the begin_access for the copy's source.
    if (copyAddrTake && beginAccess == Inst) {
      // If `setAccessToDeinit()` returns `true` it has modified the access
      // instruction, so we are committed to the transformation on that path.
      if (setAccessToDeinit(beginAccess)) {
        copyAddrTake->setIsTakeOfSrc(IsTake);
        return copyAddrTake;
      }
    }

    // destroy_addrs commonly exist in a block of dealloc_stack's, which don't
    // affect take-ability.
    if (isa<DeallocStackInst>(Inst))
      continue;

    // An end_access of the same address may be able to be rewritten as a
    // [deinit] access.
    if (auto endAccess = dyn_cast<EndAccessInst>(Inst)) {
      if (endAccess->getSource() == Operand) {
        beginAccess = endAccess->getBeginAccess();
        continue;
      }
    }

    // This code doesn't try to prove tricky validity constraints about whether
    // it is safe to push the destroy_addr past interesting instructions.
    if (Inst->mayHaveSideEffects())
      break;
  }

  // If we didn't find a copy_addr to fold this into, emit the destroy_addr.
  return createDestroyAddr(Loc, Operand);
}

static bool couldReduceStrongRefcount(SILInstruction *Inst) {
  // Simple memory accesses cannot reduce refcounts.
  if (isa<LoadInst>(Inst) || isa<StoreInst>(Inst) ||
      isa<RetainValueInst>(Inst) || isa<UnownedRetainInst>(Inst) ||
      isa<UnownedReleaseInst>(Inst) || isa<StrongRetainUnownedInst>(Inst) ||
      isa<StoreWeakInst>(Inst) || isa<StrongRetainInst>(Inst) ||
      isa<AllocStackInst>(Inst) || isa<DeallocStackInst>(Inst))
    return false;

  // Assign and copyaddr of trivial types cannot drop refcounts, and 'inits'
  // never can either.  Nontrivial ones can though, because the overwritten
  // value drops a retain.  We would have to do more alias analysis to be able
  // to safely ignore one of those.
  if (auto AI = dyn_cast<AssignInst>(Inst)) {
    auto StoredType = AI->getOperand(0)->getType();
    if (StoredType.isTrivial(Inst->getModule()) ||
        StoredType.is<ReferenceStorageType>())
      return false;
  }

  if (auto *CAI = dyn_cast<CopyAddrInst>(Inst)) {
    // Initializations can only increase refcounts.
    if (CAI->isInitializationOfDest())
      return false;

    SILType StoredType = CAI->getOperand(0)->getType().getObjectType();
    if (StoredType.isTrivial(Inst->getModule()) ||
        StoredType.is<ReferenceStorageType>())
      return false;
  }

  // This code doesn't try to prove tricky validity constraints about whether
  // it is safe to push the release past interesting instructions.
  return Inst->mayHaveSideEffects();
}


/// Perform a strong_release instruction at the current location, attempting
/// to fold it locally into nearby retain instructions or emitting an explicit
/// strong release if necessary.  If this inserts a new instruction, it
/// returns it, otherwise it returns null.
PointerUnion<StrongRetainInst *, StrongReleaseInst *>
SILBuilder::emitStrongRelease(SILLocation Loc, SILValue Operand) {
  // Release on a functionref is a noop.
  if (isa<FunctionRefInst>(Operand)) {
    return static_cast<StrongReleaseInst *>(nullptr);
  }

  // Check to see if the instruction immediately before the insertion point is a
  // strong_retain of the specified operand.  If so, we can zap the pair.
  auto I = getInsertionPoint(), BBStart = getInsertionBB()->begin();
  while (I != BBStart) {
    auto *Inst = &*--I;

    if (auto *SRA = dyn_cast<StrongRetainInst>(Inst)) {
      if (SRA->getOperand() == Operand)
        return SRA;
      // Skip past unrelated retains.
      continue;
    }

    // Scan past simple instructions that cannot reduce strong refcounts.
    if (couldReduceStrongRefcount(Inst))
      break;
  }

  // If we didn't find a retain to fold this into, emit the release.
  return createStrongRelease(Loc, Operand, getDefaultAtomicity());
}

/// Emit a release_value instruction at the current location, attempting to
/// fold it locally into another nearby retain_value instruction.  This
/// returns the new instruction if it inserts one, otherwise it returns null.
PointerUnion<RetainValueInst *, ReleaseValueInst *>
SILBuilder::emitReleaseValue(SILLocation Loc, SILValue Operand) {
  // Check to see if the instruction immediately before the insertion point is a
  // retain_value of the specified operand.  If so, we can zap the pair.
  auto I = getInsertionPoint(), BBStart = getInsertionBB()->begin();
  while (I != BBStart) {
    auto *Inst = &*--I;

    if (auto *SRA = dyn_cast<RetainValueInst>(Inst)) {
      if (SRA->getOperand() == Operand)
        return SRA;
      // Skip past unrelated retains.
      continue;
    }

    // Scan past simple instructions that cannot reduce refcounts.
    if (couldReduceStrongRefcount(Inst))
      break;
  }

  // If we didn't find a retain to fold this into, emit the release.
  return createReleaseValue(Loc, Operand, getDefaultAtomicity());
}

PointerUnion<CopyValueInst *, DestroyValueInst *>
SILBuilder::emitDestroyValue(SILLocation Loc, SILValue Operand) {
  // Check to see if the instruction immediately before the insertion point is a
  // retain_value of the specified operand.  If so, we can zap the pair.
  auto I = getInsertionPoint(), BBStart = getInsertionBB()->begin();
  while (I != BBStart) {
    auto *Inst = &*--I;

    if (auto *CVI = dyn_cast<CopyValueInst>(Inst)) {
      if (SILValue(CVI) == Operand || CVI->getOperand() == Operand)
        return CVI;
      // Skip past unrelated retains.
      continue;
    }

    // Scan past simple instructions that cannot reduce refcounts.
    if (couldReduceStrongRefcount(Inst))
      break;
  }

  // If we didn't find a retain to fold this into, emit the release.
  return createDestroyValue(Loc, Operand);
}

SILValue SILBuilder::emitThickToObjCMetatype(SILLocation Loc, SILValue Op,
                                             SILType Ty) {
  // If the operand is an otherwise-unused 'metatype' instruction in the
  // same basic block, zap it and create a 'metatype' instruction that
  // directly produces an Objective-C metatype.
  if (auto metatypeInst = dyn_cast<MetatypeInst>(Op)) {
    if (metatypeInst->use_empty() &&
        metatypeInst->getParent() == getInsertionBB()) {
      auto origLoc = metatypeInst->getLoc();
      metatypeInst->eraseFromParent();
      return createMetatype(origLoc, Ty);
    }
  }

  // Just create the thick_to_objc_metatype instruction.
  return createThickToObjCMetatype(Loc, Op, Ty);
}

SILValue SILBuilder::emitObjCToThickMetatype(SILLocation Loc, SILValue Op,
                                             SILType Ty) {
  // If the operand is an otherwise-unused 'metatype' instruction in the
  // same basic block, zap it and create a 'metatype' instruction that
  // directly produces a thick metatype.
  if (auto metatypeInst = dyn_cast<MetatypeInst>(Op)) {
    if (metatypeInst->use_empty() &&
        metatypeInst->getParent() == getInsertionBB()) {
      auto origLoc = metatypeInst->getLoc();
      metatypeInst->eraseFromParent();
      return createMetatype(origLoc, Ty);
    }
  }

  // Just create the objc_to_thick_metatype instruction.
  return createObjCToThickMetatype(Loc, Op, Ty);
}

/// Add opened archetypes defined or used by the current instruction.
/// If there are no such opened archetypes in the current instruction
/// and it is an instruction with just one operand, try to perform
/// the same action for the instruction defining an operand, because
/// it may have some opened archetypes used or defined.
void SILBuilder::addOpenedArchetypeOperands(SILInstruction *I) {
  // The list of archetypes from the previous instruction needs
  // to be replaced, because it may reference a removed instruction.
  OpenedArchetypes.addOpenedArchetypeOperands(I->getTypeDependentOperands());
  if (I && I->getNumTypeDependentOperands() > 0)
    return;

  // Keep track of already visited instructions to avoid infinite loops.
  SmallPtrSet<SILInstruction *, 8> Visited;

  while (I && I->getNumOperands() == 1 &&
         I->getNumTypeDependentOperands() == 0) {
    I = dyn_cast<SILInstruction>(I->getOperand(0));
    // Within SimplifyCFG this function may be called for an instruction
    // within unreachable code. And within an unreachable block it can happen
    // that defs do not dominate uses (because there is no dominance defined).
    // To avoid the infinite loop when following the chain of instructions via
    // their operands, bail if the operand is not an instruction or this
    // instruction was seen already.
    if (!I || !Visited.insert(I).second)
      return;
    // If it is a definition of an opened archetype,
    // register it and exit.
    auto Archetype = getOpenedArchetypeOf(I);
    if (!Archetype)
      continue;
    auto Def = OpenedArchetypes.getOpenedArchetypeDef(Archetype);
    // Return if it is a known open archetype.
    if (Def)
      return;
    // Otherwise register it and return.
    if (OpenedArchetypesTracker)
      OpenedArchetypesTracker->addOpenedArchetypeDef(Archetype, I);
    return;
  }

  if (I && I->getNumTypeDependentOperands() > 0) {
    OpenedArchetypes.addOpenedArchetypeOperands(I->getTypeDependentOperands());
  }
}
