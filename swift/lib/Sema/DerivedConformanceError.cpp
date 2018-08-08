//===--- DerivedConformanceError.cpp - Derived Error ----------------------===//
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
//  This file implements implicit derivation of the Error
//  protocol.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "DerivedConformances.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/Types.h"

using namespace swift;

static void deriveBodyBridgedNSError_enum_nsErrorDomain(
              AbstractFunctionDecl *domainDecl) {
  // enum SomeEnum {
  //   @derived
  //   static var _nsErrorDomain: String {
  //     return "ModuleName.SomeEnum"
  //   }
  // }

  auto M = domainDecl->getParentModule();
  auto &C = M->getASTContext();
  auto TC = domainDecl->getInnermostTypeContext();
  auto ED = TC->getAsEnumOrEnumExtensionContext();

  std::string buffer = M->getNameStr();
  buffer += ".";
  buffer += ED->getNameStr();
  StringRef value(C.AllocateCopy(buffer));

  auto string = new (C) StringLiteralExpr(value, SourceRange(), /*implicit*/ true);
  auto ret = new (C) ReturnStmt(SourceLoc(), string, /*implicit*/ true);
  auto body = BraceStmt::create(C, SourceLoc(),
                                ASTNode(ret),
                                SourceLoc());
  domainDecl->setBody(body);
}

static ValueDecl *
deriveBridgedNSError_enum_nsErrorDomain(DerivedConformance &derived) {
  // enum SomeEnum {
  //   @derived
  //   static var _nsErrorDomain: String {
  //     return "\(self)"
  //   }
  // }

  // Note that for @objc enums the format is assumed to be "MyModule.SomeEnum".
  // If this changes, please change PrintAsObjC as well.

  ASTContext &C = derived.TC.Context;

  auto stringTy = C.getStringDecl()->getDeclaredType();

  // Define the property.
  VarDecl *propDecl;
  PatternBindingDecl *pbDecl;
  std::tie(propDecl, pbDecl) = derived.declareDerivedProperty(
      C.Id_nsErrorDomain, stringTy, stringTy, /*isStatic=*/true,
      /*isFinal=*/true);

  // Define the getter.
  auto getterDecl = derived.addGetterToReadOnlyDerivedProperty(
      derived.TC, propDecl, stringTy);
  getterDecl->setBodySynthesizer(&deriveBodyBridgedNSError_enum_nsErrorDomain);

  derived.addMembersToConformanceContext({getterDecl, propDecl, pbDecl});

  return propDecl;
}

ValueDecl *DerivedConformance::deriveBridgedNSError(ValueDecl *requirement) {
  if (!isa<EnumDecl>(Nominal))
    return nullptr;

  if (requirement->getBaseName() == TC.Context.Id_nsErrorDomain)
    return deriveBridgedNSError_enum_nsErrorDomain(*this);

  TC.diagnose(requirement->getLoc(), diag::broken_errortype_requirement);
  return nullptr;
}
