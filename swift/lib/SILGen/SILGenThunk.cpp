//===--- SILGenThunk.cpp - SILGen for thunks ------------------------------===//
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
// This file contains code for emitting various types of thunks that can be
// referenced from code, such as dynamic thunks, curry thunks, native to foreign
// thunks and foreign to native thunks.
//
// VTable thunks and witness thunks can be found in SILGenType.cpp, and the
// meat of the bridging thunk implementation is in SILGenBridging.cpp, and
// re-abstraction thunks are in SILGenPoly.cpp.
//
//===----------------------------------------------------------------------===//

#include "SILGenFunction.h"
#include "Scope.h"
#include "ManagedValue.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/TypeLowering.h"

using namespace swift;
using namespace Lowering;

SILFunction *SILGenModule::getDynamicThunk(SILDeclRef constant,
                                           SILConstantInfo constantInfo) {
  assert(constant.kind != SILDeclRef::Kind::Allocator &&
         "allocating entry point for constructor is never dynamic");
  // Mangle the constant with a _TTD header.
  auto name = constant.mangle(SILDeclRef::ManglingKind::DynamicThunk);

  auto F = M.getOrCreateFunction(constant.getDecl(), name, SILLinkage::Shared,
                                 constantInfo.SILFnType, IsBare, IsTransparent,
                                 IsSerializable, IsThunk);

  if (F->empty()) {
    // Emit the thunk if we haven't yet.
    // Currently a dynamic thunk looks just like a foreign-to-native thunk around
    // an ObjC method. This would change if we introduced a native
    // runtime-hookable mechanism.
    SILGenFunction SGF(*this, *F);
    SGF.emitForeignToNativeThunk(constant);
  }

  return F;
}

SILValue SILGenFunction::emitDynamicMethodRef(SILLocation loc,
                                              SILDeclRef constant,
                                              SILConstantInfo constantInfo) {
  // If the method is foreign, its foreign thunk will handle the dynamic
  // dispatch for us.
  if (constant.isForeignToNativeThunk()) {
    if (!SGM.hasFunction(constant))
      SGM.emitForeignToNativeThunk(constant);
    return B.createFunctionRef(loc, SGM.getFunction(constant, NotForDefinition));
  }

  // Otherwise, we need a dynamic dispatch thunk.
  SILFunction *F = SGM.getDynamicThunk(constant, constantInfo);

  return B.createFunctionRef(loc, F);
}

static SILValue getNextUncurryLevelRef(SILGenFunction &gen,
                                       SILLocation loc,
                                       SILDeclRef thunk,
                                       SILValue selfArg,
                                       SubstitutionList curriedSubs) {
  auto *vd = thunk.getDecl();

  // Reference the next uncurrying level of the function.
  SILDeclRef next = SILDeclRef(vd, thunk.kind);
  assert(!next.isCurried);

  // If the function is natively foreign, reference its foreign entry point.
  if (requiresForeignToNativeThunk(vd))
    return gen.emitGlobalFunctionRef(loc, next);

  // If the thunk is a curry thunk for a direct method reference, we are
  // doing a direct dispatch (eg, a fragile 'super.foo()' call).
  if (thunk.isDirectReference)
    return gen.emitGlobalFunctionRef(loc, next);

  auto constantInfo = gen.SGM.Types.getConstantInfo(next);

  if (auto *func = dyn_cast<AbstractFunctionDecl>(vd)) {
    if (getMethodDispatch(func) == MethodDispatch::Class) {
      // Use the dynamic thunk if dynamic.
      if (vd->isDynamic()) {
        auto dynamicThunk = gen.SGM.getDynamicThunk(next, constantInfo);
        return gen.B.createFunctionRef(loc, dynamicThunk);
      }

      return gen.B.createClassMethod(loc, selfArg, next);
    }

    // If the fully-uncurried reference is to a generic method, look up the
    // witness.
    if (constantInfo.SILFnType->getRepresentation()
          == SILFunctionTypeRepresentation::WitnessMethod) {
      auto protocol =
        func->getDeclContext()->getAsProtocolOrProtocolExtensionContext();
      auto subMap = func->getGenericSignature()
        ->getSubstitutionMap(curriedSubs);
      auto origSelfType = protocol->getSelfInterfaceType()->getCanonicalType();
      auto substSelfType = origSelfType.subst(subMap)->getCanonicalType();
      auto conformance = subMap.lookupConformance(origSelfType, protocol);
      SILValue OpenedExistential;
      if (substSelfType->isOpenedExistential())
        OpenedExistential = selfArg;
      return gen.B.createWitnessMethod(loc, substSelfType, *conformance, next,
                                      constantInfo.getSILType(),
                                      OpenedExistential);
    }
  }

  // Otherwise, emit a direct call.
  return gen.emitGlobalFunctionRef(loc, next);
}

void SILGenFunction::emitCurryThunk(SILDeclRef thunk) {
  assert(thunk.isCurried);

  auto *vd = thunk.getDecl();

  if (auto *fd = dyn_cast<AbstractFunctionDecl>(vd)) {
    assert(!SGM.M.Types.hasLoweredLocalCaptures(fd) &&
           "methods cannot have captures");
    (void) fd;
  }

  auto selfTy = vd->getInterfaceType()->castTo<AnyFunctionType>()
    ->getInput();
  selfTy = vd->getInnermostDeclContext()->mapTypeIntoContext(selfTy);
  auto selfArg = F.begin()->createFunctionArgument(getLoweredType(selfTy));

  // Forward substitutions.
  auto subs = F.getForwardingSubstitutions();

  SILValue toFn = getNextUncurryLevelRef(*this, vd, thunk,
                                         selfArg, subs);

  // FIXME: Using the type from the ConstantInfo instead of looking at
  // getConstantOverrideInfo() for methods looks suspect in the presence
  // of covariant overrides and multiple vtable entries.
  SILFunctionConventions fromConv(
      SGM.Types.getConstantInfo(thunk).SILFnType, SGM.M);
  SILType resultTy = fromConv.getSingleSILResultType();
  resultTy = F.mapTypeIntoContext(resultTy);
  auto substTy = toFn->getType().substGenericArgs(SGM.M,  subs);

  // Partially apply the next uncurry level and return the result closure.
  auto closureTy =
    SILGenBuilder::getPartialApplyResultType(toFn->getType(), /*appliedParams=*/1,
                                             SGM.M, subs,
                                             ParameterConvention::Direct_Owned);
  SILInstruction *toClosure =
    B.createPartialApply(vd, toFn, substTy, subs, {selfArg}, closureTy);
  if (resultTy != closureTy)
    toClosure = B.createConvertFunction(vd, toClosure, resultTy);
  B.createReturn(ImplicitReturnLocation::getImplicitReturnLoc(vd), toClosure);
}

void SILGenModule::emitCurryThunk(SILDeclRef constant) {
  assert(constant.isCurried);

  // Thunks are always emitted by need, so don't need delayed emission.
  SILFunction *f = getFunction(constant, ForDefinition);
  f->setThunk(IsThunk);
  f->setBare(IsBare);

  auto *fd = constant.getDecl();
  preEmitFunction(constant, fd, f, fd);
  PrettyStackTraceSILFunction X("silgen emitCurryThunk", f);

  SILGenFunction(*this, *f).emitCurryThunk(constant);
  postEmitFunction(constant, f);
}

void SILGenModule::emitForeignToNativeThunk(SILDeclRef thunk) {
  // Thunks are always emitted by need, so don't need delayed emission.
  assert(!thunk.isForeign && "foreign-to-native thunks only");
  SILFunction *f = getFunction(thunk, ForDefinition);
  f->setThunk(IsThunk);
  if (thunk.asForeign().isClangGenerated())
    f->setSerialized(IsSerialized);
  preEmitFunction(thunk, thunk.getDecl(), f, thunk.getDecl());
  PrettyStackTraceSILFunction X("silgen emitForeignToNativeThunk", f);
  SILGenFunction(*this, *f).emitForeignToNativeThunk(thunk);
  postEmitFunction(thunk, f);
}

void SILGenModule::emitNativeToForeignThunk(SILDeclRef thunk) {
  // Thunks are always emitted by need, so don't need delayed emission.
  assert(thunk.isForeign && "native-to-foreign thunks only");
  
  SILFunction *f = getFunction(thunk, ForDefinition);
  if (thunk.hasDecl())
    preEmitFunction(thunk, thunk.getDecl(), f, thunk.getDecl());
  else
    preEmitFunction(thunk, thunk.getAbstractClosureExpr(), f,
                    thunk.getAbstractClosureExpr());
  PrettyStackTraceSILFunction X("silgen emitNativeToForeignThunk", f);
  f->setBare(IsBare);
  f->setThunk(IsThunk);
  SILGenFunction(*this, *f).emitNativeToForeignThunk(thunk);
  postEmitFunction(thunk, f);
}

SILValue SILGenFunction::emitGlobalFunctionRef(SILLocation loc,
                                               SILDeclRef constant,
                                               SILConstantInfo constantInfo) {
  assert(constantInfo == getConstantInfo(constant));

  // Builtins must be fully applied at the point of reference.
  if (constant.hasDecl() &&
      isa<BuiltinUnit>(constant.getDecl()->getDeclContext())) {
    SGM.diagnose(loc.getSourceLoc(), diag::not_implemented,
                 "delayed application of builtin");
    return SILUndef::get(constantInfo.getSILType(), SGM.M);
  }
  
  // If the constant is a thunk we haven't emitted yet, emit it.
  if (!SGM.hasFunction(constant)) {
    if (constant.isCurried) {
      SGM.emitCurryThunk(constant);
    } else if (constant.isForeignToNativeThunk()) {
      SGM.emitForeignToNativeThunk(constant);
    } else if (constant.isNativeToForeignThunk()) {
      SGM.emitNativeToForeignThunk(constant);
    } else if (constant.kind == SILDeclRef::Kind::EnumElement) {
      SGM.emitEnumConstructor(cast<EnumElementDecl>(constant.getDecl()));
    }
  }

  auto f = SGM.getFunction(constant, NotForDefinition);
  assert(f->getLoweredFunctionType() == constantInfo.SILFnType);
  return B.createFunctionRef(loc, f);
}

SILFunction *SILGenModule::
getOrCreateReabstractionThunk(GenericEnvironment *genericEnv,
                              CanSILFunctionType thunkType,
                              CanSILFunctionType fromType,
                              CanSILFunctionType toType,
                              IsSerialized_t Serialized) {
  // Mangle the reabstraction thunk.
  // Substitute context parameters out of the "from" and "to" types.
  auto fromInterfaceType
      = GenericEnvironment::mapTypeOutOfContext(genericEnv, fromType)
              ->getCanonicalType();
  auto toInterfaceType
      = GenericEnvironment::mapTypeOutOfContext(genericEnv, toType)
              ->getCanonicalType();

  Mangle::ASTMangler NewMangler;
  std::string name = NewMangler.mangleReabstractionThunkHelper(thunkType,
                       fromInterfaceType, toInterfaceType, M.getSwiftModule());
  
  auto loc = RegularLocation::getAutoGeneratedLocation();
  return M.getOrCreateSharedFunction(loc, name, thunkType, IsBare,
                                     IsTransparent, IsSerializable,
                                     IsReabstractionThunk);
}
