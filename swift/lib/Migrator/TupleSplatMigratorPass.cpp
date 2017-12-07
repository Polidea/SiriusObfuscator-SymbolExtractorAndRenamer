//===--- TupleSplatMigratorPass.cpp ---------------------------------------===//
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

#include "swift/AST/ASTVisitor.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Types.h"
#include "swift/Migrator/ASTMigratorPass.h"
#include "swift/Parse/Lexer.h"

using namespace swift;
using namespace swift::migrator;

namespace {

/// Builds a mapping from each ParamDecl of a ClosureExpr to its references in
/// in the closure body. This is used below to rewrite shorthand param
/// references from $0.1 to $1 and vice versa.
class ShorthandFinder: public ASTWalker {
private:
  /// A mapping from each ParamDecl of the supplied ClosureExpr to a list of
  /// each referencing DeclRefExpr (e.g. $0) or its immediately containing
  /// TupleElementExpr (e.g $0.1) if one exists
  llvm::DenseMap<ParamDecl*, std::vector<Expr*>> References;

  std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
    Expr *ParentElementExpr = nullptr;
    Expr *OrigE = E;

    if (auto *TupleElem = dyn_cast<TupleElementExpr>(E)) {
      ParentElementExpr = TupleElem;
      E = TupleElem->getBase()->getSemanticsProvidingExpr();
    }

    if (auto *DeclRef = dyn_cast<DeclRefExpr>(E)) {
      ParamDecl *Decl = dyn_cast<ParamDecl>(DeclRef->getDecl());
      Expr *Reference = ParentElementExpr? ParentElementExpr : DeclRef;
      if (References.count(Decl) && !Reference->isImplicit()) {
        References[Decl].push_back(Reference);
        return { false, OrigE };
      }
    }
    return { true, OrigE };
  }

public:
  ShorthandFinder(ClosureExpr *Expr) {
    if (!Expr->hasAnonymousClosureVars())
      return;
    References.clear();
    for (auto *Param: *Expr->getParameters()) {
      References[Param] = {};
    }
    Expr->walk(*this);
  }

  void forEachReference(llvm::function_ref<void(Expr*, ParamDecl*)> Callback) {
    for (auto Entry: References) {
      for (auto *Expr : Entry.getSecond()) {
        Callback(Expr, Entry.getFirst());
      }
    }
  }
};

struct TupleSplatMigratorPass : public ASTMigratorPass,
  public SourceEntityWalker {
    
  bool handleClosureShorthandMismatch(FunctionConversionExpr *FC) {
    if (!SF->getASTContext().LangOpts.isSwiftVersion3() || !FC->isImplicit() ||
        !isa<ClosureExpr>(FC->getSubExpr())) {
      return false;
    }

    auto *Closure = cast<ClosureExpr>(FC->getSubExpr());
    if (Closure->getInLoc().isValid())
      return false;

    FunctionType *FuncTy = FC->getType()->getAs<FunctionType>();

    unsigned NativeArity = 0;
    if (isa<ParenType>(FuncTy->getInput().getPointer())) {
      NativeArity = 1;
    } else if (auto TT = FuncTy->getInput()->getAs<TupleType>()) {
      NativeArity = TT->getNumElements();
    }

    unsigned ClosureArity = Closure->getParameters()->size();
    if (NativeArity <= ClosureArity)
      return false;

    ShorthandFinder Finder(Closure);

    if (ClosureArity == 1 && NativeArity > 1) {
      // Remove $0. from existing references or if it's only $0, replace it
      // with a tuple of the native arity, e.g. ($0, $1, $2)
      Finder.forEachReference([this, NativeArity](Expr *Ref, ParamDecl *Def) {
        if (auto *TE = dyn_cast<TupleElementExpr>(Ref)) {
          SourceLoc Start = TE->getStartLoc();
          SourceLoc End = TE->getLoc();
          Editor.replace(CharSourceRange(SM, Start, End), "$");
        } else {
          std::string TupleText;
          {
            llvm::raw_string_ostream OS(TupleText);
            for (size_t i = 1; i < NativeArity; ++i) {
              OS << ", $" << i;
            }
            OS << ")";
          }
          Editor.insert(Ref->getStartLoc(), "(");
          Editor.insertAfterToken(Ref->getEndLoc(), TupleText);
        }
      });
      return true;
    }
    return false;
  }

      /// Migrates code that compiles fine in Swift 3 but breaks in Swift 4 due to
  /// changes in how the typechecker handles tuple arguments.
  void handleTupleArgumentMismatches(const CallExpr *E) {
    if (!SF->getASTContext().LangOpts.isSwiftVersion3())
      return;
    if (E->isImplicit())
      return;

    // Handles such kind of cases:
    // \code
    //   func test(_: ()) {}
    //   test()
    // \endcode
    // This compiles fine in Swift 3 but Swift 4 complains with
    //   error: missing argument for parameter #1 in call
    //
    // It will fix the code to "test(())".
    //
    auto handleCallsToEmptyTuple = [&](const CallExpr *E) -> bool {
      auto fnTy = E->getFn()->getType()->getAs<FunctionType>();
      if (!fnTy)
        return false;
      auto parenT = dyn_cast<ParenType>(fnTy->getInput().getPointer());
      if (!parenT)
        return false;
      auto inp = dyn_cast<TupleType>(parenT->getUnderlyingType().getPointer());
      if (!inp)
        return false;
      if (inp->getNumElements() != 0)
        return false;
      auto argTupleT = dyn_cast<TupleType>(E->getArg()->getType().getPointer());
      if (!argTupleT)
        return false;
      if (argTupleT->getNumElements() != 0)
        return false;
      Editor.insertWrap("(", E->getArg()->getSourceRange(), ")");
      return true;
    };

    if (handleCallsToEmptyTuple(E))
      return;
  }

  bool walkToExprPre(Expr *E) override {
    if (auto *FCE = dyn_cast<FunctionConversionExpr>(E)) {
      handleClosureShorthandMismatch(FCE);
    } else if (auto *CE = dyn_cast<CallExpr>(E)) {
      handleTupleArgumentMismatches(CE);
    }
    return true;
  }
public:

  TupleSplatMigratorPass(EditorAdapter &Editor,
                         SourceFile *SF,
                         const MigratorOptions &Opts)
    : ASTMigratorPass(Editor, SF, Opts) {}
};

} // end anonymous namespace

void migrator::runTupleSplatMigratorPass(EditorAdapter &Editor,
                                         SourceFile *SF,
                                         const MigratorOptions &Opts) {
  TupleSplatMigratorPass { Editor, SF, Opts }.walk(SF);
}
