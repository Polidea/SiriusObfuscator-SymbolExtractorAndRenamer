//==-- DiagnoseInfiniteRecursion.cpp - Find infinitely-recursive applies --==//
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
// This file implements a diagnostic pass that detects deleterious forms of
// recursive functions.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "infinite-recursion"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Expr.h"
#include "swift/Parse/Lexer.h"
#include "swift/SIL/CFG.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/Devirtualize.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/Debug.h"

using namespace swift;

template<typename...T, typename...U>
static void diagnose(ASTContext &Context, SourceLoc loc, Diag<T...> diag,
                     U &&...args) {
  Context.Diags.diagnose(loc,
                         diag, std::forward<U>(args)...);
}

static bool hasRecursiveCallInPath(SILBasicBlock &Block,
                                   SILFunction *Target,
                                   ModuleDecl *TargetModule) {
  // Process all instructions in the block to find applies that reference
  // the parent function.  Also looks through vtables for statically
  // dispatched (witness) methods.
  for (auto &I : Block) {
    auto *AI = dyn_cast<ApplyInst>(&I);
    if (AI && AI->getCalleeFunction() && AI->getCalleeFunction() == Target)
      return true;

    if (FullApplySite FAI = FullApplySite::isa(&I)) {
      // Don't touch dynamic dispatch.
      if (isa<ObjCMethodInst>(FAI.getCallee()))
        continue;

      auto &M = FAI.getModule();
      if (auto *CMI = dyn_cast<ClassMethodInst>(FAI.getCallee())) {
        auto ClassType = CMI->getOperand()->getType();

        // FIXME: If we're not inside the module context of the method,
        // we may have to deserialize vtables.  If the serialized tables
        // are damaged, the pass will crash.
        //
        // Though, this has the added bonus of not looking into vtables
        // outside the current module.  Because we're not doing IPA, let
        // alone cross-module IPA, this is all well and good.
        auto *BGC = ClassType.getNominalOrBoundGenericNominal();
        if (BGC && BGC->getModuleContext() != TargetModule) {
          continue;
        }

        auto *F = getTargetClassMethod(M, ClassType, CMI);
        if (F == Target)
          return true;

        continue;
      }

      if (auto *WMI = dyn_cast<WitnessMethodInst>(FAI.getCallee())) {
        SILFunction *F;
        SILWitnessTable *WT;

        std::tie(F, WT) = M.lookUpFunctionInWitnessTable(
            WMI->getConformance(), WMI->getMember());
        if (F == Target)
          return true;

        continue;
      }
    }
  }
  return false;
}

static bool hasInfinitelyRecursiveApply(SILFunction &Fn,
                                        SILFunction *TargetFn) {
  SmallPtrSet<SILBasicBlock *, 16> Visited;
  SmallVector<SILBasicBlock *, 16> WorkList;
  // Keep track of whether we found at least one recursive path.
  bool foundRecursion = false;

  auto *TargetModule = TargetFn->getModule().getSwiftModule();
  auto analyzeSuccessor = [&](SILBasicBlock *Succ) -> bool {
    if (!Visited.insert(Succ).second)
      return false;

    // If the successor block contains a recursive call, end analysis there.
    if (!hasRecursiveCallInPath(*Succ, TargetFn, TargetModule)) {
      WorkList.push_back(Succ);
      return false;
    }
    return true;
  };

  // Seed the work list with the entry block.
  foundRecursion |= analyzeSuccessor(Fn.getEntryBlock());
  
  while (!WorkList.empty()) {
    SILBasicBlock *CurBlock = WorkList.pop_back_val();
    // Found a path to the exit node without a recursive call.
    if (CurBlock->getTerminator()->isFunctionExiting())
      return false;

    for (SILBasicBlock *Succ : CurBlock->getSuccessorBlocks())
      foundRecursion |= analyzeSuccessor(Succ);
  }
  return foundRecursion;
}

namespace {
  class DiagnoseInfiniteRecursion : public SILFunctionTransform {
  public:
    DiagnoseInfiniteRecursion() {}

  private:
    void run() override {
      SILFunction *Fn = getFunction();
      // Don't rerun diagnostics on deserialized functions.
      if (Fn->wasDeserializedCanonical())
        return;

      // Ignore empty functions and straight-line thunks.
      if (Fn->empty() || Fn->isThunk() != IsNotThunk)
        return;

      // If we can't diagnose it, there's no sense analyzing it.
      if (!Fn->hasLocation() || Fn->getLocation().getSourceLoc().isInvalid())
        return;

      if (hasInfinitelyRecursiveApply(*Fn, Fn)) {
        diagnose(Fn->getModule().getASTContext(),
                 Fn->getLocation().getSourceLoc(),
                 diag::warn_infinite_recursive_function);
      }
    }
  };
} // end anonymous namespace

SILTransform *swift::createDiagnoseInfiniteRecursion() {
  return new DiagnoseInfiniteRecursion();
}
