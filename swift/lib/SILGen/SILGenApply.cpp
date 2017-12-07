//===--- SILGenApply.cpp - Constructs call sites for SILGen ---------------===//
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

#include "ArgumentScope.h"
#include "ArgumentSource.h"
#include "Callee.h"
#include "FormalEvaluation.h"
#include "Initialization.h"
#include "LValue.h"
#include "RValue.h"
#include "ResultPlan.h"
#include "Scope.h"
#include "SpecializedEmitter.h"
#include "Varargs.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/Module.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/Basic/ExternalUnion.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/Unicode.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/SILArgument.h"
#include "llvm/Support/Compiler.h"

using namespace swift;
using namespace Lowering;

/// Retrieve the type to use for a method found via dynamic lookup.
static CanAnyFunctionType getDynamicMethodFormalType(SILValue proto,
                                                     ValueDecl *member,
                                                     Type memberType) {
  auto &ctx = member->getASTContext();
  CanType selfTy;
  if (member->isInstanceMember()) {
    selfTy = ctx.TheUnknownObjectType;
  } else {
    selfTy = proto->getType().getSwiftRValueType();
  }
  auto extInfo = FunctionType::ExtInfo()
                   .withRepresentation(FunctionType::Representation::Thin);

  return CanFunctionType::get(selfTy, memberType->getCanonicalType(),
                              extInfo);
}

/// Replace the 'self' parameter in the given type.
static CanSILFunctionType
replaceSelfTypeForDynamicLookup(ASTContext &ctx,
                                CanSILFunctionType fnType,
                                CanType newSelfType,
                                SILDeclRef methodName) {
  auto oldParams = fnType->getParameters();
  SmallVector<SILParameterInfo, 4> newParams;
  newParams.append(oldParams.begin(), oldParams.end() - 1);
  newParams.push_back({newSelfType, oldParams.back().getConvention()});

  // If the method returns Self, substitute AnyObject for the result type.
  SmallVector<SILResultInfo, 4> newResults;
  newResults.append(fnType->getResults().begin(), fnType->getResults().end());
  if (auto fnDecl = dyn_cast<FuncDecl>(methodName.getDecl())) {
    if (fnDecl->hasDynamicSelf()) {
      auto anyObjectTy = ctx.getAnyObjectType();
      for (auto &result : newResults) {
        auto newResultTy
          = result.getType()->replaceCovariantResultType(anyObjectTy, 0);
        result = result.getWithType(newResultTy->getCanonicalType());
      }
    }
  }

  return SILFunctionType::get(nullptr,
                              fnType->getExtInfo(),
                              fnType->getCalleeConvention(),
                              newParams,
                              newResults,
                              fnType->getOptionalErrorResult(),
                              ctx);
}

/// Retrieve the type to use for a method found via dynamic lookup.
static CanSILFunctionType getDynamicMethodLoweredType(SILGenFunction &SGF,
                                           SILValue proto,
                                           SILDeclRef methodName,
                                           CanAnyFunctionType substMemberTy) {
  auto &ctx = SGF.getASTContext();

  // Determine the opaque 'self' parameter type.
  CanType selfTy;
  if (methodName.getDecl()->isInstanceMember()) {
    selfTy = proto->getType().getSwiftRValueType();
    assert(selfTy->is<ArchetypeType>() && "Dynamic lookup needs an archetype");
  } else {
    selfTy = proto->getType().getSwiftRValueType();
  }

  // Replace the 'self' parameter type in the method type with it.
  auto objcFormalTy = substMemberTy.withExtInfo(substMemberTy->getExtInfo()
             .withSILRepresentation(SILFunctionTypeRepresentation::ObjCMethod));

  auto methodTy = SGF.SGM.M.Types
    .getUncachedSILFunctionTypeForConstant(methodName, objcFormalTy);
  return replaceSelfTypeForDynamicLookup(ctx, methodTy, selfTy, methodName);
}

/// Check if we can perform a dynamic dispatch on a super method call.
static bool canUseStaticDispatch(SILGenFunction &SGF,
                                 SILDeclRef constant) {
  auto *funcDecl = cast<AbstractFunctionDecl>(constant.getDecl());

  if (funcDecl->isFinal())
    return true;
  // Extension methods currently must be statically dispatched, unless they're
  // @objc or dynamic.
  if (funcDecl->getDeclContext()->isExtensionContext()
      && !constant.isForeign)
    return true;

  // We cannot form a direct reference to a method body defined in
  // Objective-C.
  if (constant.isForeign)
    return false;

  // If we cannot form a direct reference due to resilience constraints,
  // we have to dynamic dispatch.
  if (SGF.F.isSerialized())
    return false;

  // If the method is defined in the same module, we can reference it
  // directly.
  auto thisModule = SGF.SGM.M.getSwiftModule();
  if (thisModule == funcDecl->getModuleContext())
    return true;

  // Otherwise, we must dynamic dispatch.
  return false;
}

static SILValue getOriginalSelfValue(SILValue selfValue) {
  if (auto *BBI = dyn_cast<BeginBorrowInst>(selfValue))
    selfValue = BBI->getOperand();

  while (auto *UI = dyn_cast<UpcastInst>(selfValue))
    selfValue = UI->getOperand();

  if (auto *UTBCI = dyn_cast<UncheckedTrivialBitCastInst>(selfValue))
    selfValue = UTBCI->getOperand();

  return selfValue;
}

namespace {

/// Abstractly represents a callee, which may be a constant or function value,
/// and knows how to perform dynamic dispatch and reference the appropriate
/// entry point at any valid uncurry level.
class Callee {
public:
  enum class Kind {
    /// An indirect function value.
    IndirectValue,

    /// A direct standalone function call, referenceable by a FunctionRefInst.
    StandaloneFunction,

    /// Enum case constructor call.
    EnumElement,

    VirtualMethod_First,
      /// A method call using class method dispatch.
      ClassMethod = VirtualMethod_First,
      /// A method call using super method dispatch.
      SuperMethod,
    VirtualMethod_Last = SuperMethod,

    GenericMethod_First,
      /// A method call using archetype dispatch.
      WitnessMethod = GenericMethod_First,
      /// A method call using dynamic lookup.
      DynamicMethod,
    GenericMethod_Last = DynamicMethod
  };

  const Kind kind;

  // Move, don't copy.
  Callee(const Callee &) = delete;
  Callee &operator=(const Callee &) = delete;
private:
  ManagedValue IndirectValue;
  SILDeclRef Constant;
  SILValue SelfValue;
  CanAnyFunctionType OrigFormalInterfaceType;
  CanFunctionType SubstFormalInterfaceType;
  SubstitutionList Substitutions;
  Optional<SmallVector<ManagedValue, 2>> Captures;

  // The pointer back to the AST node that produced the callee.
  SILLocation Loc;

  static CanFunctionType
  getSubstFormalInterfaceType(CanAnyFunctionType substFormalType,
                              SubstitutionList subs) {
    if (auto *gft = substFormalType->getAs<GenericFunctionType>()) {
      return cast<FunctionType>(
        gft->substGenericArgs(subs)
          ->getCanonicalType());
    }

    return cast<FunctionType>(substFormalType);
  }

  static CanAnyFunctionType getConstantFormalInterfaceType(SILGenFunction &SGF,
                                                           SILDeclRef fn) {
    return SGF.SGM.Types.getConstantInfo(fn).FormalInterfaceType;
  }

  Callee(ManagedValue indirectValue,
         CanAnyFunctionType origFormalType,
         SILLocation l)
    : kind(Kind::IndirectValue),
      IndirectValue(indirectValue),
      OrigFormalInterfaceType(origFormalType),
      SubstFormalInterfaceType(cast<FunctionType>(origFormalType)),
      Loc(l)
  {}

  Callee(SILGenFunction &SGF, SILDeclRef standaloneFunction,
         CanAnyFunctionType origFormalType,
         CanAnyFunctionType substFormalType,
         SubstitutionList subs, SILLocation l)
    : kind(Kind::StandaloneFunction), Constant(standaloneFunction),
      OrigFormalInterfaceType(origFormalType),
      SubstFormalInterfaceType(getSubstFormalInterfaceType(substFormalType,
                                                           subs)),
      Substitutions(subs),
      Loc(l)
  {
  }

  Callee(Kind methodKind,
         SILGenFunction &SGF,
         SILValue selfValue,
         SILDeclRef methodName,
         CanAnyFunctionType origFormalType,
         CanAnyFunctionType substFormalType,
         SubstitutionList subs,
         SILLocation l)
    : kind(methodKind), Constant(methodName), SelfValue(selfValue),
      OrigFormalInterfaceType(origFormalType),
      SubstFormalInterfaceType(getSubstFormalInterfaceType(substFormalType,
                                                           subs)),
      Substitutions(subs),
      Loc(l)
  {
  }

public:

  static Callee forIndirect(ManagedValue indirectValue,
                            CanAnyFunctionType origFormalType,
                            SILLocation l) {
    return Callee(indirectValue, origFormalType, l);
  }
  static Callee forDirect(SILGenFunction &SGF, SILDeclRef c,
                          SubstitutionList subs,
                          SILLocation l) {
    auto formalType = getConstantFormalInterfaceType(SGF, c);
    return Callee(SGF, c, formalType, formalType, subs, l);
  }
  static Callee forEnumElement(SILGenFunction &SGF, SILDeclRef c,
                               SubstitutionList subs,
                               SILLocation l) {
    assert(isa<EnumElementDecl>(c.getDecl()));
    auto formalType = getConstantFormalInterfaceType(SGF, c);
    return Callee(Kind::EnumElement, SGF, SILValue(), c,
                  formalType, formalType, subs, l);
  }
  static Callee forClassMethod(SILGenFunction &SGF, SILValue selfValue,
                               SILDeclRef c,
                               SubstitutionList subs,
                               SILLocation l) {
    auto base = SGF.SGM.Types.getOverriddenVTableEntry(c);
    auto formalType = getConstantFormalInterfaceType(SGF, base);
    auto substType = getConstantFormalInterfaceType(SGF, c);
    return Callee(Kind::ClassMethod, SGF, selfValue, c,
                  formalType, substType, subs, l);
  }
  static Callee forSuperMethod(SILGenFunction &SGF, SILValue selfValue,
                               SILDeclRef c,
                               SubstitutionList subs,
                               SILLocation l) {
    selfValue = getOriginalSelfValue(selfValue);
    auto formalType = getConstantFormalInterfaceType(SGF, c);
    return Callee(Kind::SuperMethod, SGF, selfValue, c,
                  formalType, formalType, subs, l);
  }
  static Callee forArchetype(SILGenFunction &SGF,
                             CanType protocolSelfType,
                             SILDeclRef c,
                             SubstitutionList subs,
                             SILLocation l) {
    auto *protocol = cast<ProtocolDecl>(c.getDecl()->getDeclContext());
    c = c.asForeign(protocol->isObjC());

    auto formalType = getConstantFormalInterfaceType(SGF, c);
    return Callee(Kind::WitnessMethod, SGF, SILValue(), c,
                  formalType, formalType, subs, l);
  }
  static Callee forDynamic(SILGenFunction &SGF, SILValue proto,
                           SILDeclRef c,
                           Type substFormalType,
                           SubstitutionList subs,
                           SILLocation l) {
    auto formalType = getDynamicMethodFormalType(proto,
                                                 c.getDecl(),
                                                 substFormalType);
    return Callee(Kind::DynamicMethod, SGF, proto, c,
                  formalType, formalType, subs, l);
  }
  Callee(Callee &&) = default;
  Callee &operator=(Callee &&) = default;

  void setCaptures(SmallVectorImpl<ManagedValue> &&captures) {
    Captures = std::move(captures);
  }
  
  ArrayRef<ManagedValue> getCaptures() const {
    if (Captures)
      return *Captures;
    return {};
  }
  
  bool hasCaptures() const {
    return Captures.hasValue();
  }

  AbstractionPattern getOrigFormalType() const {
    return AbstractionPattern(OrigFormalInterfaceType);
  }

  CanFunctionType getSubstFormalType() const {
    return SubstFormalInterfaceType;
  }

  unsigned getNaturalUncurryLevel() const {
    switch (kind) {
    case Kind::IndirectValue:
      return 0;

    case Kind::StandaloneFunction:
    case Kind::EnumElement:
    case Kind::ClassMethod:
    case Kind::SuperMethod:
    case Kind::WitnessMethod:
    case Kind::DynamicMethod:
      return Constant.getUncurryLevel();
    }

    llvm_unreachable("Unhandled Kind in switch.");
  }

  EnumElementDecl *getEnumElementDecl() {
    assert(kind == Kind::EnumElement);
    return cast<EnumElementDecl>(Constant.getDecl());
  }

  std::tuple<ManagedValue, CanSILFunctionType,
             Optional<ForeignErrorConvention>, ImportAsMemberStatus, ApplyOptions>
  getAtUncurryLevel(SILGenFunction &SGF, unsigned level) const {
    ManagedValue mv;
    ApplyOptions options = ApplyOptions::None;
    Optional<SILDeclRef> constant = None;

    if (!Constant) {
      assert(level == 0 && "can't curry indirect function");
    } else {
      unsigned uncurryLevel = Constant.getUncurryLevel();
      assert(level <= uncurryLevel
             && "uncurrying past natural uncurry level of standalone function");
      if (level < uncurryLevel) {
        assert(level == 0);
        constant = Constant.asCurried();
      } else {
        constant = Constant;
      }
    }

    switch (kind) {
    case Kind::IndirectValue:
      mv = IndirectValue;
      assert(Substitutions.empty());
      break;

    case Kind::StandaloneFunction: {
      // If we're currying a direct reference to a class-dispatched method,
      // make sure we emit the right set of thunks.
      if (constant->isCurried && Constant.hasDecl())
        if (auto func = Constant.getAbstractFunctionDecl())
          if (getMethodDispatch(func) == MethodDispatch::Class)
            constant = constant->asDirectReference(true);
      
      auto constantInfo = SGF.getConstantInfo(*constant);
      SILValue ref = SGF.emitGlobalFunctionRef(Loc, *constant, constantInfo);
      mv = ManagedValue::forUnmanaged(ref);
      break;
    }
    case Kind::EnumElement: {
      auto constantInfo = SGF.getConstantInfo(*constant);

      // We should not end up here if the enum constructor call is fully
      // applied.
      assert(constant->isCurried);

      SILValue ref = SGF.emitGlobalFunctionRef(Loc, *constant, constantInfo);
      mv = ManagedValue::forUnmanaged(ref);
      break;
    }
    case Kind::ClassMethod: {
      auto constantInfo = SGF.getConstantInfo(*constant);

      // If the call is curried, emit a direct call to the curry thunk.
      if (constant->isCurried) {
        SILValue ref = SGF.emitGlobalFunctionRef(Loc, *constant, constantInfo);
        mv = ManagedValue::forUnmanaged(ref);
        break;
      }

      // Otherwise, do the dynamic dispatch inline.
      SILValue methodVal = SGF.B.createClassMethod(Loc,
                                                   SelfValue,
                                                   *constant,
                                                   /*volatile*/
                                                     constant->isForeign);

      mv = ManagedValue::forUnmanaged(methodVal);
      break;
    }
    case Kind::SuperMethod: {
      assert(!constant->isCurried);

      auto base = SGF.SGM.Types.getOverriddenVTableEntry(*constant);
      auto constantInfo = SGF.SGM.Types.getConstantOverrideInfo(*constant, base);
      auto methodVal = SGF.B.createSuperMethod(Loc,
                                               SelfValue,
                                               *constant,
                                               constantInfo.getSILType(),
                                               /*volatile*/
                                                 constant->isForeign);
      mv = ManagedValue::forUnmanaged(methodVal);
      break;
    }
    case Kind::WitnessMethod: {
      auto constantInfo = SGF.getConstantInfo(*constant);

      // If the call is curried, emit a direct call to the curry thunk.
      if (constant->isCurried) {
        SILValue ref = SGF.emitGlobalFunctionRef(Loc, *constant, constantInfo);
        mv = ManagedValue::forUnmanaged(ref);
        break;
      }

      auto proto = Constant.getDecl()->getDeclContext()
        ->getAsProtocolOrProtocolExtensionContext();
      auto lookupType = getSubstFormalType().getInput()
        ->getRValueInstanceType()->getCanonicalType();

      SILValue fn = SGF.B.createWitnessMethod(Loc,
                                  lookupType,
                                  ProtocolConformanceRef(proto),
                                  *constant,
                                  constantInfo.getSILType(),
                                  constant->isForeign);
      mv = ManagedValue::forUnmanaged(fn);
      break;
    }
    case Kind::DynamicMethod: {
      assert(!constant->isCurried);

      // Lower the substituted type from the AST, which should have any generic
      // parameters in the original signature erased to their upper bounds.
      auto substFormalType = getSubstFormalType();
      auto objcFormalType = substFormalType.withExtInfo(
         substFormalType->getExtInfo()
           .withSILRepresentation(SILFunctionTypeRepresentation::ObjCMethod));
      auto fnType = SGF.SGM.M.Types
        .getUncachedSILFunctionTypeForConstant(*constant, objcFormalType);

      auto closureType =
        replaceSelfTypeForDynamicLookup(SGF.getASTContext(), fnType,
                                SelfValue->getType().getSwiftRValueType(),
                                Constant);

      SILValue fn = SGF.B.createDynamicMethod(Loc,
                          SelfValue,
                          *constant,
                          SILType::getPrimitiveObjectType(closureType),
                          /*volatile*/ Constant.isForeign);
      mv = ManagedValue::forUnmanaged(fn);
      break;
    }
    }

    Optional<ForeignErrorConvention> foreignError;
    ImportAsMemberStatus foreignSelf;
    if (constant && constant->isForeign) {
      auto func = cast<AbstractFunctionDecl>(constant->getDecl());
      foreignError = func->getForeignErrorConvention();
      foreignSelf = func->getImportAsMemberStatus();
    }

    auto substFnType =
      mv.getType().castTo<SILFunctionType>()->substGenericArgs(
        SGF.SGM.M, Substitutions);

    return std::make_tuple(mv, substFnType, foreignError, foreignSelf, options);
  }

  SubstitutionList getSubstitutions() const {
    return Substitutions;
  }

  SILDeclRef getMethodName() const {
    return Constant;
  }

  /// Return a specialized emission function if this is a function with a known
  /// lowering, such as a builtin, or return null if there is no specialized
  /// emitter.
  Optional<SpecializedEmitter>
  getSpecializedEmitter(SILGenModule &SGM, unsigned uncurryLevel) const {
    // Currently we have no curried known functions.
    if (uncurryLevel != 0)
      return None;

    switch (kind) {
    case Kind::StandaloneFunction: {
      return SpecializedEmitter::forDecl(SGM, Constant);
    }
    case Kind::EnumElement:
    case Kind::IndirectValue:
    case Kind::ClassMethod:
    case Kind::SuperMethod:
    case Kind::WitnessMethod:
    case Kind::DynamicMethod:
      return None;
    }
    llvm_unreachable("bad callee kind");
  }
};

/// For ObjC init methods, we generate a shared-linkage Swift allocating entry
/// point that does the [[T alloc] init] dance. We want to use this native
/// thunk where we expect to be calling an allocating entry point for an ObjC
/// constructor.
static bool isConstructorWithGeneratedAllocatorThunk(ValueDecl *vd) {
  return vd->isObjC() && isa<ConstructorDecl>(vd);
}

/// An ASTVisitor for decomposing a nesting of ApplyExprs into an initial
/// Callee and a list of CallSites. The CallEmission class below uses these
/// to generate the actual SIL call.
///
/// Formally, an ApplyExpr in the AST always has a single argument, which may
/// be of tuple type, possibly empty. Also, some callees have a formal type
/// which is curried -- for example, methods have type Self -> Arg -> Result.
///
/// However, SIL functions take zero or more parameters and the natural entry
/// point of a method takes Self as an additional argument, rather than
/// returning a partial application.
///
/// Therefore, nested ApplyExprs applied to a constant are flattened into a
/// single call of the most uncurried entry point fitting the call site.
/// This avoids intermediate closure construction.
///
/// For example, a method reference 'self.method' decomposes into curry thunk
/// as the callee, with a single call site '(self)'.
///
/// On the other hand, a call of a method 'self.method(x)(y)' with a function
/// return type decomposes into the method's natural entry point as the callee,
/// and two call sites, first '(x, self)' then '(y)'.
class SILGenApply : public Lowering::ExprVisitor<SILGenApply> {
public:
  /// The SILGenFunction that we are emitting SIL into.
  SILGenFunction &SGF;

  /// The apply callee that abstractly represents the entry point that is being
  /// called.
  Optional<Callee> ApplyCallee;

  /// The lvalue or rvalue representing the argument source of self.
  ArgumentSource SelfParam;
  Expr *SelfApplyExpr = nullptr;
  Type SelfType;
  std::vector<ApplyExpr*> CallSites;
  Expr *SideEffect = nullptr;

  /// When visiting expressions, sometimes we need to emit self before we know
  /// what the actual callee is. In such cases, we assume that we are passing
  /// self at +0 and then after we know what the callee is, we check if the
  /// self is passed at +1. If so, we add an extra retain.
  bool AssumedPlusZeroSelf = false;

  SILGenApply(SILGenFunction &SGF)
    : SGF(SGF)
  {}

  void setCallee(Callee &&c) {
    assert(!ApplyCallee && "already set callee!");
    ApplyCallee.emplace(std::move(c));
  }

  void setSideEffect(Expr *sideEffectExpr) {
    assert(!SideEffect && "already set side effect!");
    SideEffect = sideEffectExpr;
  }

  void setSelfParam(ArgumentSource &&theSelfParam, Expr *theSelfApplyExpr) {
    assert(!SelfParam && "already set this!");
    SelfParam = std::move(theSelfParam);
    SelfApplyExpr = theSelfApplyExpr;
    SelfType = theSelfApplyExpr->getType();
  }
  void setSelfParam(ArgumentSource &&theSelfParam, Type selfType) {
    assert(!SelfParam && "already set this!");
    SelfParam = std::move(theSelfParam);
    SelfApplyExpr = nullptr;
    SelfType = selfType;
  }

  void decompose(Expr *e) {
    visit(e);
  }

  /// Fall back to an unknown, indirect callee.
  void visitExpr(Expr *e) {
    ManagedValue fn = SGF.emitRValueAsSingleValue(e);
    auto origType = cast<AnyFunctionType>(e->getType()->getCanonicalType());
    setCallee(Callee::forIndirect(fn, origType, e));
  }

  void visitLoadExpr(LoadExpr *e) {
    // TODO: preserve the function pointer at its original abstraction level
    ManagedValue fn = SGF.emitRValueAsSingleValue(e);
    auto origType = cast<AnyFunctionType>(e->getType()->getCanonicalType());
    setCallee(Callee::forIndirect(fn, origType, e));
  }

  /// Add a call site to the curry.
  void visitApplyExpr(ApplyExpr *e) {
    if (e->isSuper()) {
      applySuper(e);
    } else if (applyInitDelegation(e)) {
      // Already done
    } else {
      CallSites.push_back(e);
      visit(e->getFn());
    }
  }

  /// Given a metatype value for the type, allocate an Objective-C
  /// object (with alloc_ref_dynamic) of that type.
  ///
  /// \returns the self object.
  ManagedValue allocateObjCObject(ManagedValue selfMeta, SILLocation loc) {
    auto metaType = selfMeta.getType().castTo<AnyMetatypeType>();
    CanType type = metaType.getInstanceType();

    // Convert to an Objective-C metatype representation, if needed.
    ManagedValue selfMetaObjC;
    if (metaType->getRepresentation() == MetatypeRepresentation::ObjC) {
      selfMetaObjC = selfMeta;
    } else {
      CanAnyMetatypeType objcMetaType;
      if (isa<MetatypeType>(metaType)) {
        objcMetaType = CanMetatypeType::get(type, MetatypeRepresentation::ObjC);
      } else {
        objcMetaType = CanExistentialMetatypeType::get(type,
                                                  MetatypeRepresentation::ObjC);
      }
      // ObjC metatypes are trivial and thus do not have a cleanup. Only if we
      // convert them to an object do they become non-trivial.
      assert(!selfMeta.hasCleanup());
      selfMetaObjC = ManagedValue::forUnmanaged(SGF.B.emitThickToObjCMetatype(
          loc, selfMeta.getValue(), SGF.SGM.getLoweredType(objcMetaType)));
    }

    // Allocate the object.
    return SGF.emitManagedRValueWithCleanup(
        SGF.B.createAllocRefDynamic(
                          loc,
                          selfMetaObjC.getValue(),
                          SGF.SGM.getLoweredType(type),
                          /*objc=*/true, {}, {}));
  }

  //
  // Known callees.
  //
  void visitDeclRefExpr(DeclRefExpr *e) {
    // If we need to perform dynamic dispatch for the given function,
    // emit class_method to do so.
    if (auto afd = dyn_cast<AbstractFunctionDecl>(e->getDecl())) {
      Optional<SILDeclRef::Kind> kind;
      bool isDynamicallyDispatched;
      bool requiresAllocRefDynamic = false;

      // Determine whether the method is dynamically dispatched.
      if (auto *proto = dyn_cast<ProtocolDecl>(afd->getDeclContext())) {
        // We have four cases to deal with here:
        //
        //  1) for a "static" / "type" method, the base is a metatype.
        //  2) for a classbound protocol, the base is a class-bound protocol rvalue,
        //     which is loadable.
        //  3) for a mutating method, the base has inout type.
        //  4) for a nonmutating method, the base is a general archetype
        //     rvalue, which is address-only.  The base is passed at +0, so it isn't
        //     consumed.
        //
        // In the last case, the AST has this call typed as being applied
        // to an rvalue, but the witness is actually expecting a pointer
        // to the +0 value in memory.  We just pass in the address since
        // archetypes are address-only.

        assert(!CallSites.empty());
        ApplyExpr *thisCallSite = CallSites.back();
        CallSites.pop_back();

        ArgumentSource selfValue = thisCallSite->getArg();

        SubstitutionList subs = e->getDeclRef().getSubstitutions();

        SILDeclRef::Kind kind = SILDeclRef::Kind::Func;
        if (isa<ConstructorDecl>(afd)) {
          if (proto->isObjC()) {
            SILLocation loc = thisCallSite->getArg();

            // For Objective-C initializers, we only have an initializing
            // initializer. We need to allocate the object ourselves.
            kind = SILDeclRef::Kind::Initializer;

            auto metatype = std::move(selfValue).getAsSingleValue(SGF);
            auto allocated = allocateObjCObject(metatype, loc);
            auto allocatedType = allocated.getType().getSwiftRValueType();
            selfValue = ArgumentSource(loc, RValue(SGF, loc,
                                                   allocatedType, allocated));
          } else {
            // For non-Objective-C initializers, we have an allocating
            // initializer to call.
            kind = SILDeclRef::Kind::Allocator;
          }
        }

        SILDeclRef constant = SILDeclRef(afd, kind);

        // Prepare the callee.  This can modify both selfValue and subs.
        Callee theCallee = Callee::forArchetype(SGF,
                                                selfValue.getSubstRValueType(),
                                                constant, subs, e);
        AssumedPlusZeroSelf = selfValue.isRValue()
          && selfValue.forceAndPeekRValue(SGF).peekIsPlusZeroRValueOrTrivial();

        setSelfParam(std::move(selfValue), thisCallSite);
        setCallee(std::move(theCallee));

        return;
      }

      if (e->getAccessSemantics() != AccessSemantics::Ordinary) {
        isDynamicallyDispatched = false;
      } else {
        switch (getMethodDispatch(afd)) {
        case MethodDispatch::Class:
          isDynamicallyDispatched = true;
          break;
        case MethodDispatch::Static:
          isDynamicallyDispatched = false;
          break;
        }
      }

      if (isa<FuncDecl>(afd) && isDynamicallyDispatched) {
        kind = SILDeclRef::Kind::Func;
      } else if (auto ctor = dyn_cast<ConstructorDecl>(afd)) {
        ApplyExpr *thisCallSite = CallSites.back();
        // Required constructors are dynamically dispatched when the 'self'
        // value is not statically derived.
        if (ctor->isRequired() &&
            thisCallSite->getArg()->getType()->is<AnyMetatypeType>() &&
            !thisCallSite->getArg()->isStaticallyDerivedMetatype()) {
          if (requiresForeignEntryPoint(afd)) {
            // When we're performing Objective-C dispatch, we don't have an
            // allocating constructor to call. So, perform an alloc_ref_dynamic
            // and pass that along to the initializer.
            requiresAllocRefDynamic = true;
            kind = SILDeclRef::Kind::Initializer;
          } else {
            kind = SILDeclRef::Kind::Allocator;
          }
        } else {
          isDynamicallyDispatched = false;
        }
      }

      if (isDynamicallyDispatched) {
        ApplyExpr *thisCallSite = CallSites.back();
        CallSites.pop_back();

        // Emit the rvalue for self, allowing for guaranteed plus zero if we
        // have a func.
        bool AllowPlusZero = kind && *kind == SILDeclRef::Kind::Func;
        RValue self =
          SGF.emitRValue(thisCallSite->getArg(),
                         AllowPlusZero ? SGFContext::AllowGuaranteedPlusZero :
                                         SGFContext());

        // If we allowed for PlusZero and we *did* get the value back at +0,
        // then we assumed that self could be passed at +0. We will check later
        // if the actual callee passes self at +1 later when we know its actual
        // type.
        AssumedPlusZeroSelf =
          AllowPlusZero && self.peekIsPlusZeroRValueOrTrivial();

        // If we require a dynamic allocation of the object here, do so now.
        if (requiresAllocRefDynamic) {
          SILLocation loc = thisCallSite->getArg();
          auto selfValue = allocateObjCObject(
                             std::move(self).getAsSingleValue(SGF, loc),
                             loc);
          self = RValue(SGF, loc, selfValue.getType().getSwiftRValueType(),
                        selfValue);
        }

        auto selfValue = self.peekScalarValue();

        setSelfParam(ArgumentSource(thisCallSite->getArg(), std::move(self)),
                     thisCallSite);
        auto constant = SILDeclRef(afd, kind.getValue())
          .asForeign(requiresForeignEntryPoint(afd));

        auto subs = e->getDeclRef().getSubstitutions();
        setCallee(Callee::forClassMethod(SGF, selfValue, constant, subs, e));
        return;
      }
    }

    // If this is a direct reference to a vardecl, just emit its value directly.
    // Recursive references to callable declarations are allowed.
    if (isa<VarDecl>(e->getDecl())) {
      visitExpr(e);
      return;
    }

    auto constant = SILDeclRef(e->getDecl())
      .asForeign(!isConstructorWithGeneratedAllocatorThunk(e->getDecl())
                 && requiresForeignEntryPoint(e->getDecl()));

    auto afd = dyn_cast<AbstractFunctionDecl>(e->getDecl());

    CaptureInfo captureInfo;

    // Otherwise, we have a statically-dispatched call.
    SubstitutionList subs = e->getDeclRef().getSubstitutions();

    if (afd) {
      captureInfo = SGF.SGM.Types.getLoweredLocalCaptures(afd);
      if (afd->getDeclContext()->isLocalContext() &&
          !captureInfo.hasGenericParamCaptures())
        subs = SubstitutionList();
    }

    // Enum case constructor references are open-coded.
    if (isa<EnumElementDecl>(e->getDecl()))
      setCallee(Callee::forEnumElement(SGF, constant, subs, e));
    else
      setCallee(Callee::forDirect(SGF, constant, subs, e));
    
    // If the decl ref requires captures, emit the capture params.
    if (afd) {
      if (!captureInfo.getCaptures().empty()) {
        SmallVector<ManagedValue, 4> captures;
        SGF.emitCaptures(e, afd, CaptureEmission::ImmediateApplication,
                         captures);
        ApplyCallee->setCaptures(std::move(captures));
      }
    }
  }
  
  void visitAbstractClosureExpr(AbstractClosureExpr *e) {
    // Emit the closure body.
    SGF.SGM.emitClosure(e);

    // If we're in top-level code, we don't need to physically capture script
    // globals, but we still need to mark them as escaping so that DI can flag
    // uninitialized uses.
    if (&SGF == SGF.SGM.TopLevelSGF) {
      SGF.SGM.emitMarkFunctionEscapeForTopLevelCodeGlobals(e,e->getCaptureInfo());
    }
    
    // A directly-called closure can be emitted as a direct call instead of
    // really producing a closure object.
    SILDeclRef constant(e);

    SubstitutionList subs;
    if (e->getCaptureInfo().hasGenericParamCaptures())
      subs = SGF.getForwardingSubstitutions();

    setCallee(Callee::forDirect(SGF, constant, subs, e));
    
    // If the closure requires captures, emit them.
    bool hasCaptures = SGF.SGM.M.Types.hasLoweredLocalCaptures(e);
    if (hasCaptures) {
      SmallVector<ManagedValue, 4> captures;
      SGF.emitCaptures(e, e, CaptureEmission::ImmediateApplication,
                       captures);
      ApplyCallee->setCaptures(std::move(captures));
    }
  }
  
  void visitOtherConstructorDeclRefExpr(OtherConstructorDeclRefExpr *e) {
    auto subs = e->getDeclRef().getSubstitutions();

    // FIXME: We might need to go through ObjC dispatch for references to
    // constructors imported from Clang (which won't have a direct entry point)
    // or to delegate to a designated initializer.
    setCallee(Callee::forDirect(SGF,
                SILDeclRef(e->getDecl(), SILDeclRef::Kind::Initializer), subs, e));
  }
  void visitDotSyntaxBaseIgnoredExpr(DotSyntaxBaseIgnoredExpr *e) {
    setSideEffect(e->getLHS());
    visit(e->getRHS());
  }

  void visitFunctionConversionExpr(FunctionConversionExpr *e) {
    // FIXME: Check whether this function conversion requires us to build a
    // thunk.
    visit(e->getSubExpr());
  }

  void visitCovariantFunctionConversionExpr(CovariantFunctionConversionExpr *e){
    // FIXME: These expressions merely adjust the result type for DynamicSelf
    // in an unchecked, ABI-compatible manner. They shouldn't prevent us form
    // forming a complete call.
    visitExpr(e);
  }

  void visitIdentityExpr(IdentityExpr *e) {
    visit(e->getSubExpr());
  }

  void applySuper(ApplyExpr *apply) {
    // Load the 'super' argument.
    Expr *arg = apply->getArg();
    ManagedValue super;

    // The callee for a super call has to be either a method or constructor.
    Expr *fn = apply->getFn();
    SubstitutionList substitutions;
    SILDeclRef constant;
    if (auto *ctorRef = dyn_cast<OtherConstructorDeclRefExpr>(fn)) {
      constant = SILDeclRef(ctorRef->getDecl(), SILDeclRef::Kind::Initializer)
        .asForeign(requiresForeignEntryPoint(ctorRef->getDecl()));

      if (ctorRef->getDeclRef().isSpecialized())
        substitutions = ctorRef->getDeclRef().getSubstitutions();

      assert(SGF.SelfInitDelegationState ==
             SILGenFunction::WillSharedBorrowSelf);
      SGF.SelfInitDelegationState = SILGenFunction::WillExclusiveBorrowSelf;
      super = SGF.emitRValueAsSingleValue(arg);
      assert(SGF.SelfInitDelegationState ==
             SILGenFunction::DidExclusiveBorrowSelf);

      // Check if super is not the same as our base type. This means that we
      // performed an upcast. Set SuperInitDelegationState to super.
      if (super.getValue() != SGF.InitDelegationSelf.getValue()) {
        assert(super.getCleanup() == SGF.InitDelegationSelf.getCleanup());
        SILValue underlyingSelf = SGF.InitDelegationSelf.forward(SGF);
        SGF.InitDelegationSelf = ManagedValue::forUnmanaged(underlyingSelf);
        CleanupHandle newWriteback = SGF.enterDelegateInitSelfWritebackCleanup(
            SGF.InitDelegationLoc.getValue(), SGF.InitDelegationSelfBox,
            super.getValue());
        SGF.SuperInitDelegationSelf =
            ManagedValue(super.getValue(), newWriteback);
        super = SGF.SuperInitDelegationSelf;
      }
    } else if (auto *declRef = dyn_cast<DeclRefExpr>(fn)) {
      assert(isa<FuncDecl>(declRef->getDecl()) && "non-function super call?!");
      constant = SILDeclRef(declRef->getDecl())
        .asForeign(requiresForeignEntryPoint(declRef->getDecl()));

      if (declRef->getDeclRef().isSpecialized())
        substitutions = declRef->getDeclRef().getSubstitutions();
      super = SGF.emitRValueAsSingleValue(arg);
    } else {
      llvm_unreachable("invalid super callee");
    }

    CanType superFormalType = arg->getType()->getCanonicalType();
    setSelfParam(ArgumentSource(arg, RValue(SGF, apply, superFormalType, super)),
                 apply);

    if (!canUseStaticDispatch(SGF, constant)) {
      // ObjC super calls require dynamic dispatch.
      setCallee(Callee::forSuperMethod(SGF, super.getValue(), constant,
                                       substitutions, fn));
    } else {
      // Native Swift super calls to final methods are direct.
      setCallee(Callee::forDirect(SGF, constant,
                                  substitutions, fn));
    }
  }

  /// Walk the given \c selfArg expression that produces the appropriate
  /// `self` for a call, applying the same transformations to the provided
  /// \c selfValue (which might be a metatype).
  ///
  /// This is used for initializer delegation, so it covers only the narrow
  /// subset of expressions used there.
  ManagedValue emitCorrespondingSelfValue(ManagedValue selfValue,
                                          Expr *selfArg) {
    while (true) {
      // Handle archetype-to-super and derived-to-base upcasts.
      if (isa<ArchetypeToSuperExpr>(selfArg) ||
          isa<DerivedToBaseExpr>(selfArg)) {
        auto ice = cast<ImplicitConversionExpr>(selfArg);
        auto resultTy = ice->getType()->getCanonicalType();

        // If the 'self' value is a metatype, update the target type
        // accordingly.
        if (auto selfMetaTy = selfValue.getType().getAs<AnyMetatypeType>()) {
          resultTy = CanMetatypeType::get(resultTy,
                                          selfMetaTy->getRepresentation());
        }
        auto loweredResultTy = SGF.getLoweredLoadableType(resultTy);
        if (loweredResultTy != selfValue.getType()) {
          auto upcast = SGF.B.createUpcast(ice,
                                           selfValue.getValue(),
                                           loweredResultTy);
          selfValue = ManagedValue(upcast, selfValue.getCleanup());
        }

        selfArg = ice->getSubExpr();
        continue;
      }

      // Skip over loads.
      if (auto load = dyn_cast<LoadExpr>(selfArg)) {
        selfArg = load->getSubExpr();
        continue;
      }

      // Skip over inout expressions.
      if (auto inout = dyn_cast<InOutExpr>(selfArg)) {
        selfArg = inout->getSubExpr();
        continue;
      }

      // Declaration references terminate the search.
      if (isa<DeclRefExpr>(selfArg))
        break;

      llvm_unreachable("unhandled conversion for metatype value");
    }

    return selfValue;
  }

  /// Try to emit the given application as initializer delegation.
  bool applyInitDelegation(ApplyExpr *expr) {
    // Dig out the constructor we're delegating to.
    Expr *fn = expr->getFn();
    auto ctorRef = dyn_cast<OtherConstructorDeclRefExpr>(
                     fn->getSemanticsProvidingExpr());
    if (!ctorRef)
      return false;

    // Determine whether we'll need to use an allocating constructor (vs. the
    // initializing constructor).
    auto nominal = ctorRef->getDecl()->getDeclContext()
                     ->getAsNominalTypeOrNominalTypeExtensionContext();
    bool useAllocatingCtor;

    // Value types only have allocating initializers.
    if (isa<StructDecl>(nominal) || isa<EnumDecl>(nominal))
      useAllocatingCtor = true;
    // Protocols only witness allocating initializers, except for @objc
    // protocols, which only witness initializing initializers.
    else if (auto proto = dyn_cast<ProtocolDecl>(nominal)) {
      useAllocatingCtor = !proto->isObjC();
    // Factory initializers are effectively "allocating" initializers with no
    // corresponding initializing entry point.
    } else if (ctorRef->getDecl()->isFactoryInit()) {
      useAllocatingCtor = true;
    } else {
      // We've established we're in a class initializer or a protocol extension
      // initializer for a class-bound protocol, In either case, we're
      // delegating initialization, but we only have an instance in the former
      // case.
      assert(isa<ClassDecl>(nominal)
             && "some new kind of init context we haven't implemented");
      useAllocatingCtor = static_cast<bool>(SGF.AllocatorMetatype) &&
                          !ctorRef->getDecl()->isObjC();
    }

    // Load the 'self' argument.
    Expr *arg = expr->getArg();
    ManagedValue self;
    CanType selfFormalType = arg->getType()->getCanonicalType();

    // If we're using the allocating constructor, we need to pass along the
    // metatype.
    if (useAllocatingCtor) {
      selfFormalType = CanMetatypeType::get(
          selfFormalType->getInOutObjectType()->getCanonicalType());

      // If the initializer is a C function imported as a member,
      // there is no 'self' parameter. Mark it undef.
      if (ctorRef->getDecl()->isImportAsMember()) {
        self = SGF.emitUndef(expr, selfFormalType);
      } else if (SGF.AllocatorMetatype) {
        self = emitCorrespondingSelfValue(
            ManagedValue::forUnmanaged(SGF.AllocatorMetatype), arg);
      } else {
        self = ManagedValue::forUnmanaged(SGF.emitMetatypeOfValue(expr, arg));
      }
    } else {
      // If we're in a protocol extension initializer, we haven't allocated
      // "self" yet at this point. Do so. Use alloc_ref_dynamic since we should
      // only ever get here in ObjC protocol extensions currently.
      if (SGF.AllocatorMetatype) {
        assert(ctorRef->getDecl()->isObjC()
               && "only expect to delegate an initializer from an allocator "
                  "in objc protocol extensions");
        
        self = allocateObjCObject(
                        ManagedValue::forUnmanaged(SGF.AllocatorMetatype), arg);

        // Perform any adjustments needed to 'self'.
        self = emitCorrespondingSelfValue(self, arg);
      } else {
        assert(SGF.SelfInitDelegationState ==
               SILGenFunction::WillSharedBorrowSelf);
        SGF.SelfInitDelegationState = SILGenFunction::WillExclusiveBorrowSelf;
        self = SGF.emitRValueAsSingleValue(arg);
        assert(SGF.SelfInitDelegationState ==
               SILGenFunction::DidExclusiveBorrowSelf);
      }
    }

    setSelfParam(ArgumentSource(arg, RValue(SGF, expr, selfFormalType, self)),
                 expr);

    auto subs = ctorRef->getDeclRef().getSubstitutions();

    // Determine the callee. For structs and enums, this is the allocating
    // constructor (because there is no initializing constructor). For protocol
    // default implementations, we also use the allocating constructor, because
    // that's the only thing that's witnessed. For classes,
    // this is the initializing constructor, to which we will dynamically
    // dispatch.
    if (SelfParam.getSubstRValueType()->getRValueInstanceType()
          ->is<ArchetypeType>()
        && isa<ProtocolDecl>(ctorRef->getDecl()->getDeclContext())) {
      // Look up the witness for the constructor.
      auto constant = SILDeclRef(ctorRef->getDecl(),
                             useAllocatingCtor
                               ? SILDeclRef::Kind::Allocator
                               : SILDeclRef::Kind::Initializer);
      setCallee(Callee::forArchetype(SGF,
                                     self.getType().getSwiftRValueType(),
                                     constant, subs, expr));
    } else if (getMethodDispatch(ctorRef->getDecl())
                 == MethodDispatch::Class) {
      // Dynamic dispatch to the initializer.
      setCallee(Callee::forClassMethod(
                  SGF,
                  self.getValue(),
                  SILDeclRef(ctorRef->getDecl(),
                             useAllocatingCtor
                               ? SILDeclRef::Kind::Allocator
                             : SILDeclRef::Kind::Initializer)
                    .asForeign(requiresForeignEntryPoint(ctorRef->getDecl())),
                  subs,
                  fn));
    } else {
      // Directly call the peer constructor.
      setCallee(
        Callee::forDirect(
          SGF,
          SILDeclRef(ctorRef->getDecl(),
                     useAllocatingCtor
                       ? SILDeclRef::Kind::Allocator
                     : SILDeclRef::Kind::Initializer)
            .asForeign(requiresForeignEntryPoint(ctorRef->getDecl())),
          subs,
          fn));
    }

    return true;
  }

  Callee getCallee() {
    assert(ApplyCallee && "did not find callee?!");
    return std::move(*ApplyCallee);
  }

  /// Ignore parentheses and implicit conversions.
  static Expr *ignoreParensAndImpConversions(Expr *expr) {
    while (true) {
      if (auto ice = dyn_cast<ImplicitConversionExpr>(expr)) {
        expr = ice->getSubExpr();
        continue;
      }

      // Simple optional-to-optional conversions.  This doesn't work
      // for the full generality of OptionalEvaluationExpr, but it
      // works given that we check the result for certain forms.
      if (auto eval = dyn_cast<OptionalEvaluationExpr>(expr)) {
        if (auto inject = dyn_cast<InjectIntoOptionalExpr>(eval->getSubExpr())) {
          if (auto bind = dyn_cast<BindOptionalExpr>(inject->getSubExpr())) {
            if (bind->getDepth() == 0)
              return bind->getSubExpr();
          }
        }
      }

      auto valueProviding = expr->getValueProvidingExpr();
      if (valueProviding != expr) {
        expr = valueProviding;
        continue;
      }

      return expr;
    }
  }

  void visitForceValueExpr(ForceValueExpr *e) {
    // If this application is a dynamic member reference that is forced to
    // succeed with the '!' operator, emit it as a direct invocation of the
    // method we found.
    if (emitForcedDynamicMemberRef(e))
      return;

    visitExpr(e);
  }

  /// If this application forces a dynamic member reference with !, emit
  /// a direct reference to the member.
  bool emitForcedDynamicMemberRef(ForceValueExpr *e) {
    // Check whether the argument is a dynamic member reference.
    auto arg = ignoreParensAndImpConversions(e->getSubExpr());

    auto openExistential = dyn_cast<OpenExistentialExpr>(arg);
    if (openExistential)
      arg = openExistential->getSubExpr();

    auto dynamicMemberRef = dyn_cast<DynamicMemberRefExpr>(arg);
    if (!dynamicMemberRef)
      return false;

    // Since we'll be collapsing this call site, make sure there's another
    // call site that will actually perform the invocation.
    if (CallSites.empty())
      return false;

    // Only @objc methods can be forced.
    auto *fd = dyn_cast<FuncDecl>(dynamicMemberRef->getMember().getDecl());
    if (!fd || !fd->isObjC())
      return false;

    // Local function that actually emits the dynamic member reference.
    auto emitDynamicMemberRef = [&] {
      // We found it. Emit the base.
      ManagedValue base =
        SGF.emitRValueAsSingleValue(dynamicMemberRef->getBase());

      setSelfParam(ArgumentSource(dynamicMemberRef->getBase(),
                                  RValue(SGF, dynamicMemberRef,
                                    base.getType().getSwiftRValueType(), base)),
                   dynamicMemberRef);

      // Determine the type of the method we referenced, by replacing the
      // class type of the 'Self' parameter with Builtin.UnknownObject.
      auto member = SILDeclRef(fd).asForeign();

      auto substFormalType = dynamicMemberRef->getType()
          ->getAnyOptionalObjectType();
      setCallee(Callee::forDynamic(SGF, base.getValue(), member,
                                   substFormalType, {}, e));
    };

    // When we have an open existential, open it and then emit the
    // member reference.
    if (openExistential) {
      SGF.emitOpenExistentialExpr(openExistential,
                                  [&](Expr*) { emitDynamicMemberRef(); });
    } else {
      emitDynamicMemberRef();
    }
    return true;
  }
};

} // end anonymous namespace

/// Emit either an 'apply' or a 'try_apply', with the error branch of
/// the 'try_apply' simply branching out of all cleanups and throwing.
SILValue SILGenFunction::emitApplyWithRethrow(SILLocation loc,
                                              SILValue fn,
                                              SILType substFnType,
                                              SubstitutionList subs,
                                              ArrayRef<SILValue> args) {
  CanSILFunctionType silFnType = substFnType.castTo<SILFunctionType>();
  SILFunctionConventions fnConv(silFnType, SGM.M);
  SILType resultType = fnConv.getSILResultType();

  if (!silFnType->hasErrorResult()) {
    return B.createApply(loc, fn, substFnType, resultType, subs, args);
  }

  SILBasicBlock *errorBB = createBasicBlock();
  SILBasicBlock *normalBB = createBasicBlock();
  B.createTryApply(loc, fn, substFnType, subs, args, normalBB, errorBB);

  // Emit the rethrow logic.
  {
    B.emitBlock(errorBB);
    SILValue error = errorBB->createPHIArgument(fnConv.getSILErrorType(),
                                                ValueOwnershipKind::Owned);

    B.createBuiltin(loc, SGM.getASTContext().getIdentifier("willThrow"),
                    SGM.Types.getEmptyTupleType(), {}, {error});

    Cleanups.emitCleanupsForReturn(CleanupLocation::get(loc));
    B.createThrow(loc, error);
  }

  // Enter the normal path.
  B.emitBlock(normalBB);
  return normalBB->createPHIArgument(resultType, ValueOwnershipKind::Owned);
}

static RValue emitStringLiteral(SILGenFunction &SGF, Expr *E, StringRef Str,
                                SGFContext C,
                                StringLiteralExpr::Encoding encoding) {
  uint64_t Length;
  bool isASCII = true;
  for (unsigned char c : Str) {
    if (c > 127) {
      isASCII = false;
      break;
    }
  }
  bool useConstantStringBuiltin = false;
  StringLiteralInst::Encoding instEncoding;
  ConstStringLiteralInst::Encoding constInstEncoding;
  switch (encoding) {
  case StringLiteralExpr::UTF8:
    instEncoding = StringLiteralInst::Encoding::UTF8;
    Length = Str.size();
    break;

  case StringLiteralExpr::UTF16: {
    instEncoding = StringLiteralInst::Encoding::UTF16;
    Length = unicode::getUTF16Length(Str);
    break;
  }
  case StringLiteralExpr::UTF8ConstString:
    constInstEncoding = ConstStringLiteralInst::Encoding::UTF8;
    useConstantStringBuiltin = true;
    break;

  case StringLiteralExpr::UTF16ConstString: {
    constInstEncoding = ConstStringLiteralInst::Encoding::UTF16;
    useConstantStringBuiltin = true;
    break;
  }
  case StringLiteralExpr::OneUnicodeScalar: {
    SILType Int32Ty = SILType::getBuiltinIntegerType(32, SGF.getASTContext());
    SILValue UnicodeScalarValue =
        SGF.B.createIntegerLiteral(E, Int32Ty,
                                   unicode::extractFirstUnicodeScalar(Str));
    return RValue(SGF, E, Int32Ty.getSwiftRValueType(),
                  ManagedValue::forUnmanaged(UnicodeScalarValue));
  }
  }

  // Should we build a constant string literal?
  if (useConstantStringBuiltin) {
    auto *string = SGF.B.createConstStringLiteral(E, Str, constInstEncoding);
    ManagedValue Elts[] = {ManagedValue::forUnmanaged(string)};
    TupleTypeElt TypeElts[] = {Elts[0].getType().getSwiftRValueType()};
    CanType ty =
        TupleType::get(TypeElts, SGF.getASTContext())->getCanonicalType();
    return RValue::withPreExplodedElements(Elts, ty);
  }

  // The string literal provides the data.
  auto *string = SGF.B.createStringLiteral(E, Str, instEncoding);

  // The length is lowered as an integer_literal.
  auto WordTy = SILType::getBuiltinWordType(SGF.getASTContext());
  auto *lengthInst = SGF.B.createIntegerLiteral(E, WordTy, Length);

  // The 'isascii' bit is lowered as an integer_literal.
  auto Int1Ty = SILType::getBuiltinIntegerType(1, SGF.getASTContext());
  auto *isASCIIInst = SGF.B.createIntegerLiteral(E, Int1Ty, isASCII);

  ManagedValue EltsArray[] = {
    ManagedValue::forUnmanaged(string),
    ManagedValue::forUnmanaged(lengthInst),
    ManagedValue::forUnmanaged(isASCIIInst)
  };

  TupleTypeElt TypeEltsArray[] = {
    EltsArray[0].getType().getSwiftRValueType(),
    EltsArray[1].getType().getSwiftRValueType(),
    EltsArray[2].getType().getSwiftRValueType()
  };

  ArrayRef<ManagedValue> Elts;
  ArrayRef<TupleTypeElt> TypeElts;
  switch (instEncoding) {
  case StringLiteralInst::Encoding::UTF16:
    Elts = llvm::makeArrayRef(EltsArray).slice(0, 2);
    TypeElts = llvm::makeArrayRef(TypeEltsArray).slice(0, 2);
    break;

  case StringLiteralInst::Encoding::UTF8:
    Elts = EltsArray;
    TypeElts = TypeEltsArray;
    break;

  case StringLiteralInst::Encoding::ObjCSelector:
    llvm_unreachable("Objective-C selectors cannot be formed here");
  }

  CanType ty =
    TupleType::get(TypeElts, SGF.getASTContext())->getCanonicalType();
  return RValue::withPreExplodedElements(Elts, ty);
}

/// Emit a raw apply operation, performing no additional lowering of
/// either the arguments or the result.
static SILValue emitRawApply(SILGenFunction &SGF,
                             SILLocation loc,
                             ManagedValue fn,
                             SubstitutionList subs,
                             ArrayRef<ManagedValue> args,
                             CanSILFunctionType substFnType,
                             ApplyOptions options,
                             ArrayRef<SILValue> indirectResultAddrs) {
  SILFunctionConventions substFnConv(substFnType, SGF.SGM.M);
  // Get the callee value.
  SILValue fnValue = substFnType->isCalleeConsumed()
    ? fn.forward(SGF)
    : fn.getValue();

  SmallVector<SILValue, 4> argValues;

  // Add the buffers for the indirect results if needed.
#ifndef NDEBUG
  assert(indirectResultAddrs.size() == substFnConv.getNumIndirectSILResults());
  unsigned resultIdx = 0;
  for (auto indResultTy : substFnConv.getIndirectSILResultTypes()) {
    assert(indResultTy == indirectResultAddrs[resultIdx++]->getType());
  }
#endif
  argValues.append(indirectResultAddrs.begin(), indirectResultAddrs.end());

  auto inputParams = substFnType->getParameters();
  assert(inputParams.size() == args.size());

  // Gather the arguments.
  for (auto i : indices(args)) {
    auto argValue = (inputParams[i].isConsumed() ? args[i].forward(SGF)
                                                 : args[i].getValue());
#ifndef NDEBUG
    auto inputTy = substFnConv.getSILType(inputParams[i]);
    if (argValue->getType() != inputTy) {
      auto &out = llvm::errs();
      out << "TYPE MISMATCH IN ARGUMENT " << i << " OF APPLY AT ";
      printSILLocationDescription(out, loc, SGF.getASTContext());
      out << "  argument value: ";
      argValue->print(out);
      out << "  parameter type: ";
      inputTy.print(out);
      out << "\n";
      abort();
    }
#endif
    argValues.push_back(argValue);
  }

  auto resultType = substFnConv.getSILResultType();
  auto calleeType = SILType::getPrimitiveObjectType(substFnType);

  // If we don't have an error result, we can make a simple 'apply'.
  SILValue result;
  if (!substFnType->hasErrorResult()) {
    result = SGF.B.createApply(loc, fnValue, calleeType,
                               resultType, subs, argValues);

  // Otherwise, we need to create a try_apply.
  } else {
    SILBasicBlock *normalBB = SGF.createBasicBlock();
    result = normalBB->createPHIArgument(resultType, ValueOwnershipKind::Owned);

    SILBasicBlock *errorBB =
      SGF.getTryApplyErrorDest(loc, substFnType->getErrorResult(),
                               options & ApplyOptions::DoesNotThrow);

    SGF.B.createTryApply(loc, fnValue, calleeType, subs, argValues,
                         normalBB, errorBB);
    SGF.B.emitBlock(normalBB);
  }

  // Given any guaranteed arguments that are not being passed at +0, insert the
  // decrement here instead of at the end of scope. Guaranteed just means that
  // we guarantee the lifetime of the object for the duration of the call.
  // Be sure to use a CleanupLocation so that unreachable code diagnostics don't
  // trigger.
  for (auto i : indices(args)) {
    if (!inputParams[i].isGuaranteed() || args[i].isPlusZeroRValueOrTrivial())
      continue;

    SILValue argValue = args[i].forward(SGF);
    SILType argType = argValue->getType();
    CleanupLocation cleanupLoc = CleanupLocation::get(loc);
    if (!argType.isAddress())
      SGF.getTypeLowering(argType).emitDestroyRValue(SGF.B, cleanupLoc, argValue);
    else
      SGF.getTypeLowering(argType).emitDestroyAddress(SGF.B, cleanupLoc, argValue);
  }

  return result;
}

static bool hasUnownedInnerPointerResult(CanSILFunctionType fnType) {
  for (auto result : fnType->getResults()) {
    if (result.getConvention() == ResultConvention::UnownedInnerPointer)
      return true;
  }
  return false;
}

/// Emit a function application, assuming that the arguments have been
/// lowered appropriately for the abstraction level but that the
/// result does need to be turned back into something matching a
/// formal type.
RValue SILGenFunction::emitApply(ResultPlanPtr &&resultPlan,
                                 ArgumentScope &&argScope, SILLocation loc,
                                 ManagedValue fn, SubstitutionList subs,
                                 ArrayRef<ManagedValue> args,
                                 const CalleeTypeInfo &calleeTypeInfo,
                                 ApplyOptions options, SGFContext evalContext) {
  auto substFnType = calleeTypeInfo.substFnType;
  auto substResultType = calleeTypeInfo.substResultType;

  // Create the result plan.
  SmallVector<SILValue, 4> indirectResultAddrs;
  resultPlan->gatherIndirectResultAddrs(*this, loc, indirectResultAddrs);

  // If the function returns an inner pointer, we'll need to lifetime-extend
  // the 'self' parameter.
  SILValue lifetimeExtendedSelf;
  bool hasAlreadyLifetimeExtendedSelf = false;
  if (hasUnownedInnerPointerResult(substFnType)) {
    auto selfMV = args.back();
    lifetimeExtendedSelf = selfMV.getValue();

    switch (substFnType->getParameters().back().getConvention()) {
    case ParameterConvention::Direct_Owned:
      // If the callee will consume the 'self' parameter, let's retain it so we
      // can keep it alive.
      lifetimeExtendedSelf = B.emitCopyValueOperation(loc, lifetimeExtendedSelf);
      break;
    case ParameterConvention::Direct_Guaranteed:
    case ParameterConvention::Direct_Unowned:
      // We'll manually manage the argument's lifetime after the
      // call. Disable its cleanup, forcing a copy if it was emitted +0.
      if (selfMV.hasCleanup()) {
        selfMV.forwardCleanup(*this);
      } else {
        lifetimeExtendedSelf = selfMV.copyUnmanaged(*this, loc).forward(*this);
      }
      break;

    case ParameterConvention::Indirect_In_Guaranteed:
    case ParameterConvention::Indirect_In:
    case ParameterConvention::Indirect_In_Constant:
    case ParameterConvention::Indirect_Inout:
    case ParameterConvention::Indirect_InoutAliasable:
      // We may need to support this at some point, but currently only imported
      // objc methods are returns_inner_pointer.
      llvm_unreachable("indirect self argument to method that"
                       " returns_inner_pointer?!");
    }
  }

  // If there's a foreign error parameter, fill it in.
  ManagedValue errorTemp;
  if (auto foreignError = calleeTypeInfo.foreignError) {
    unsigned errorParamIndex =
        calleeTypeInfo.foreignError->getErrorParameterIndex();

    // This is pretty evil.
    auto &errorArgSlot = const_cast<ManagedValue &>(args[errorParamIndex]);

    std::tie(errorTemp, errorArgSlot) =
        resultPlan->emitForeignErrorArgument(*this, loc).getValue();
  }

  // Emit the raw application.
  SILValue rawDirectResult = emitRawApply(*this, loc, fn, subs, args,
                                          substFnType, options,
                                          indirectResultAddrs);

  // Pop the argument scope.
  argScope.pop();

  // Explode the direct results.
  SILFunctionConventions substFnConv(substFnType, SGM.M);
  SmallVector<ManagedValue, 4> directResults;
  auto addManagedDirectResult = [&](SILValue result,
                                    const SILResultInfo &resultInfo) {
    auto &resultTL = getTypeLowering(resultInfo.getType());

    switch (resultInfo.getConvention()) {
    case ResultConvention::Indirect:
      assert(!substFnConv.isSILIndirect(resultInfo)
             && "indirect direct result?");
      break;

    case ResultConvention::Owned:
      break;

    // For autoreleased results, the reclaim is implicit, so the value is
    // effectively +1.
    case ResultConvention::Autoreleased:
      break;

    // Autorelease the 'self' value to lifetime-extend it.
    case ResultConvention::UnownedInnerPointer:
      assert(lifetimeExtendedSelf
             && "did not save lifetime-extended self param");
      if (!hasAlreadyLifetimeExtendedSelf) {
        B.createAutoreleaseValue(loc, lifetimeExtendedSelf, B.getDefaultAtomicity());
        hasAlreadyLifetimeExtendedSelf = true;
      }
      LLVM_FALLTHROUGH;

    case ResultConvention::Unowned:
      // Unretained. Retain the value.
      result = resultTL.emitCopyValue(B, loc, result);
      break;
    }

    directResults.push_back(emitManagedRValueWithCleanup(result, resultTL));
  };

  auto directSILResults = substFnConv.getDirectSILResults();
  if (directSILResults.empty()) {
    // Nothing to do.
  } else if (substFnConv.getNumDirectSILResults() == 1) {
    addManagedDirectResult(rawDirectResult, *directSILResults.begin());
  } else {
    llvm::SmallVector<std::pair<SILValue, const SILResultInfo &>, 8> copiedResults;
    {
      Scope S(Cleanups, CleanupLocation::get(loc));

      // First create an rvalue cleanup for our direct result.
      ManagedValue managedDirectResult = emitManagedRValueWithCleanup(rawDirectResult);
      // Then borrow the managed direct result.
      ManagedValue borrowedDirectResult = managedDirectResult.borrow(*this, loc);
      // Then create unmanaged copies of the direct result and forward the
      // result as expected by addManageDirectResult.
      unsigned Index = 0;
      for (const SILResultInfo &directResult : directSILResults) {
        ManagedValue elt = B.createTupleExtract(loc, borrowedDirectResult, Index,
                                                substFnConv.getSILType(directResult));
        SILValue v = elt.copyUnmanaged(*this, loc).forward(*this);
        // We assume that unowned inner pointers, autoreleased values, and
        // indirect values are never returned in tuples.
        // FIXME: can this assertion be removed without lowered addresses?
        assert(directResult.getConvention() == ResultConvention::Owned
               || directResult.getConvention() == ResultConvention::Unowned
               || !substFnConv.useLoweredAddresses());
        copiedResults.push_back({v, directResult});
        ++Index;
      }
      // Then allow the cleanups to be emitted in the proper reverse order.
    }
    // Finally add our managed direct results.
    for (auto p : copiedResults) {
      addManagedDirectResult(p.first, p.second);
    }
  }

  // If there was a foreign error convention, consider it.
  // TODO: maybe this should happen after managing the result if it's
  // not a result-checking convention?
  if (auto foreignError = calleeTypeInfo.foreignError) {
    bool doesNotThrow = (options & ApplyOptions::DoesNotThrow);
    emitForeignErrorCheck(loc, directResults, errorTemp,
                          doesNotThrow, *foreignError);
  }

  auto directResultsArray = makeArrayRef(directResults);
  RValue result =
    resultPlan->finish(*this, loc, substResultType, directResultsArray);
  assert(directResultsArray.empty() && "didn't claim all direct results");

  return result;
}

RValue SILGenFunction::emitMonomorphicApply(SILLocation loc,
                                            ManagedValue fn,
                                            ArrayRef<ManagedValue> args,
                                            CanType resultType,
                                            ApplyOptions options,
                           Optional<SILFunctionTypeRepresentation> overrideRep,
                     const Optional<ForeignErrorConvention> &foreignError){
  auto fnType = fn.getType().castTo<SILFunctionType>();
  assert(!fnType->isPolymorphic());
  SGFContext evalContext;
  CalleeTypeInfo calleeTypeInfo(fnType, AbstractionPattern(resultType),
                                resultType, foreignError, overrideRep);
  ResultPlanPtr resultPlan = ResultPlanBuilder::computeResultPlan(
      *this, calleeTypeInfo, loc, evalContext);
  ArgumentScope argScope(*this, loc);
  return emitApply(std::move(resultPlan), std::move(argScope), loc, fn, {},
                   args, calleeTypeInfo, options, evalContext);
}

/// Count the number of SILParameterInfos that are needed in order to
/// pass the given argument.
static unsigned getFlattenedValueCount(AbstractionPattern origType,
                                       CanType substType,
                                       ImportAsMemberStatus foreignSelf) {
  // C functions imported as static methods don't consume any real arguments.
  if (foreignSelf.isStatic())
    return 0;

  // The count is always 1 unless the substituted type is a tuple.
  auto substTuple = dyn_cast<TupleType>(substType);
  if (!substTuple) return 1;

  // If the original type is opaque and the substituted type is
  // materializable, the count is 1 anyway.
  if (origType.isTypeParameter() && substTuple->isMaterializable())
    return 1;

  // Otherwise, add up the elements.
  unsigned count = 0;
  for (auto i : indices(substTuple.getElementTypes())) {
    count += getFlattenedValueCount(origType.getTupleElementType(i),
                                    substTuple.getElementType(i),
                                    ImportAsMemberStatus());
  }
  return count;
}

static AbstractionPattern claimNextParamClause(AbstractionPattern &type) {
  auto result = type.getFunctionInputType();
  type = type.getFunctionResultType();
  return result;
}

static CanType claimNextParamClause(CanAnyFunctionType &type) {
  auto result = type.getInput();
  type = dyn_cast<AnyFunctionType>(type.getResult());
  return result;
}

namespace {

/// The original argument expression for some sort of complex
/// argument emission.
class OriginalArgument {
  llvm::PointerIntPair<Expr*, 1, bool> ExprAndIsIndirect;

public:
  OriginalArgument() = default;
  OriginalArgument(Expr *expr, bool indirect)
    : ExprAndIsIndirect(expr, indirect) {}

  Expr *getExpr() const { return ExprAndIsIndirect.getPointer(); }
  bool isIndirect() const { return ExprAndIsIndirect.getInt(); }
};

/// A delayed argument.  Call arguments are evaluated in two phases:
/// a formal evaluation phase and a formal access phase.  The primary
/// example of this is an l-value that is passed by reference, where
/// the access to the l-value does not begin until the formal access
/// phase, but there are other examples, generally relating to pointer
/// conversions.
///
/// A DelayedArgument represents the part of evaluating an argument
/// that's been delayed until the formal access phase.
class DelayedArgument {
public:
  enum KindTy {
    /// This is a true inout argument.
    InOut,

    /// This is a borrowed direct argument.
    BorrowDirect,

    /// This is a borrowed indirect argument.
    BorrowIndirect,

    LastLVKindWithoutExtra = BorrowIndirect,

    /// The l-value needs to be converted to a pointer type.
    LValueToPointer,

    /// An array l-value needs to be converted to a pointer type.
    LValueArrayToPointer,

    LastLVKind = LValueArrayToPointer,

    /// An array r-value needs to be converted to a pointer type.
    RValueArrayToPointer,

    /// A string r-value needs to be converted to a pointer type.
    RValueStringToPointer,
  };

private:
  KindTy Kind;

  struct LValueStorage {
    LValue LV;
    SILLocation Loc;

    LValueStorage(LValue &&lv, SILLocation loc) : LV(std::move(lv)), Loc(loc) {}
  };
  struct RValueStorage {
    ManagedValue RV;

    RValueStorage(ManagedValue rv) : RV(rv) {}
  };

  static int getUnionIndexForValue(KindTy kind) {
    return (kind <= LastLVKind ? 0 : 1);
  }

  /// Storage for either the l-value or the r-value.
  ExternalUnion<KindTy, getUnionIndexForValue,
                LValueStorage, RValueStorage> Value;

  LValueStorage &LV() { return Value.get<LValueStorage>(Kind); }
  const LValueStorage &LV() const { return Value.get<LValueStorage>(Kind); }
  RValueStorage &RV() { return Value.get<RValueStorage>(Kind); }
  const RValueStorage &RV() const { return Value.get<RValueStorage>(Kind); }

  /// The original argument expression, which will be emitted down
  /// to the point from which the l-value or r-value was generated.
  OriginalArgument Original;

  using PointerAccessInfo = SILGenFunction::PointerAccessInfo;
  using ArrayAccessInfo = SILGenFunction::ArrayAccessInfo;
  static int getUnionIndexForExtra(KindTy kind) {
    switch (kind) {
    case LValueToPointer:
      return 0;
    case LValueArrayToPointer:
    case RValueArrayToPointer:
      return 1;
    default:
      return -1;
    }
  }
  ExternalUnion<KindTy, getUnionIndexForExtra,
                PointerAccessInfo, ArrayAccessInfo> Extra;

public:
  DelayedArgument(KindTy kind, LValue &&lv, SILLocation loc)
      : Kind(kind) {
    assert(kind <= LastLVKindWithoutExtra &&
           "this constructor should only be used for simple l-value kinds");
    Value.emplace<LValueStorage>(Kind, std::move(lv), loc);
  }

  DelayedArgument(KindTy kind, ManagedValue rv, OriginalArgument original)
      : Kind(kind), Original(original) {
    Value.emplace<RValueStorage>(Kind, rv);
  }

  DelayedArgument(SILGenFunction::PointerAccessInfo pointerInfo,
                  LValue &&lv, SILLocation loc, OriginalArgument original)
      : Kind(LValueToPointer), Original(original) {
    Value.emplace<LValueStorage>(Kind, std::move(lv), loc);
    Extra.emplace<PointerAccessInfo>(Kind, pointerInfo);
  }

  DelayedArgument(SILGenFunction::ArrayAccessInfo arrayInfo,
                  LValue &&lv, SILLocation loc, OriginalArgument original)
      : Kind(LValueArrayToPointer), Original(original) {
    Value.emplace<LValueStorage>(Kind, std::move(lv), loc);
    Extra.emplace<ArrayAccessInfo>(Kind, arrayInfo);
  }

  DelayedArgument(KindTy kind,
                  SILGenFunction::ArrayAccessInfo arrayInfo,
                  ManagedValue rv, OriginalArgument original)
      : Kind(kind), Original(original) {
    Value.emplace<RValueStorage>(Kind, rv);
    Extra.emplace<ArrayAccessInfo>(Kind, arrayInfo);
  }

  DelayedArgument(DelayedArgument &&other)
      : Kind(other.Kind), Original(other.Original) {
    Value.moveConstruct(Kind, std::move(other.Value));
    Extra.moveConstruct(Kind, std::move(other.Extra));
  }

  DelayedArgument &operator=(DelayedArgument &&other) {
    Value.moveAssign(Kind, other.Kind, std::move(other.Value));
    Extra.moveAssign(Kind, other.Kind, std::move(other.Extra));
    Kind = other.Kind;
    Original = other.Original;
    return *this;
  }

  ~DelayedArgument() {
    Extra.destruct(Kind);
    Value.destruct(Kind);
  }

  bool isSimpleInOut() const { return Kind == InOut; }
  SILLocation getInOutLocation() const {
    assert(isSimpleInOut());
    return LV().Loc;
  }

  ManagedValue emit(SILGenFunction &SGF) {
    switch (Kind) {
    case InOut:
      return emitInOut(SGF);
    case BorrowDirect:
      return emitBorrowDirect(SGF);
    case BorrowIndirect:
      return emitBorrowIndirect(SGF);
    case LValueToPointer:
    case LValueArrayToPointer:
    case RValueArrayToPointer:
    case RValueStringToPointer:
      return finishOriginalArgument(SGF);
    }
    llvm_unreachable("bad kind");
  }

private:
  ManagedValue emitInOut(SILGenFunction &SGF) {
    return emitAddress(SGF, AccessKind::ReadWrite);
  }

  ManagedValue emitBorrowIndirect(SILGenFunction &SGF) {
    return emitAddress(SGF, AccessKind::Read);
  }

  ManagedValue emitBorrowDirect(SILGenFunction &SGF) {
    ManagedValue address = emitAddress(SGF, AccessKind::Read);
    return SGF.B.createLoadBorrow(LV().Loc, address);
  }

  ManagedValue emitAddress(SILGenFunction &SGF, AccessKind accessKind) {
    auto tsanKind =
      (accessKind == AccessKind::Read ? TSanKind::None : TSanKind::InoutAccess);
    return SGF.emitAddressOfLValue(LV().Loc, std::move(LV().LV),
                                   accessKind, tsanKind);
  }

  /// Replay the original argument expression.
  ManagedValue finishOriginalArgument(SILGenFunction &SGF) {
    auto results = finishOriginalExpr(SGF, Original.getExpr());
    auto value = results.first; // just let the owner go

    if (Original.isIndirect() && !value.getType().isAddress()) {
      value = value.materialize(SGF, Original.getExpr());
    }

    return value;
  }

  // (value, owner)
  std::pair<ManagedValue, ManagedValue>
  finishOriginalExpr(SILGenFunction &SGF, Expr *expr) {

    // This needs to handle all of the recursive cases from
    // ArgEmission::maybeEmitDelayed.

    expr = expr->getSemanticsProvidingExpr();

    // Handle injections into optionals.
    if (auto inject = dyn_cast<InjectIntoOptionalExpr>(expr)) {
      auto ownedValue =
        finishOriginalExpr(SGF, inject->getSubExpr());
      auto &optionalTL = SGF.getTypeLowering(expr->getType());

      auto optValue = SGF.emitInjectOptional(inject, optionalTL, SGFContext(),
                              [&](SGFContext ctx) { return ownedValue.first; });
      return {optValue, ownedValue.second};
    }

    // Handle try!.
    if (auto forceTry = dyn_cast<ForceTryExpr>(expr)) {
      // Handle throws from the accessor?  But what if the writeback throws?
      SILGenFunction::ForceTryEmission emission(SGF, forceTry);
      return finishOriginalExpr(SGF, forceTry->getSubExpr());
    }

    // Handle optional evaluations.
    if (auto optEval = dyn_cast<OptionalEvaluationExpr>(expr)) {
      return finishOptionalEvaluation(SGF, optEval);
    }

    // Done with the recursive cases.  Make sure we handled everything.
    assert(isa<InOutToPointerExpr>(expr) ||
           isa<ArrayToPointerExpr>(expr) ||
           isa<StringToPointerExpr>(expr));

    switch (Kind) {
    case InOut:
    case BorrowDirect:
    case BorrowIndirect:
      llvm_unreachable("no original expr to finish in these cases");

    case LValueToPointer:
      return {SGF.emitLValueToPointer(LV().Loc, std::move(LV().LV),
                                      Extra.get<PointerAccessInfo>(Kind)),
              /*owner*/ ManagedValue()};

    case LValueArrayToPointer:
      return SGF.emitArrayToPointer(LV().Loc, std::move(LV().LV),
                                    Extra.get<ArrayAccessInfo>(Kind));

    case RValueArrayToPointer: {
      auto pointerExpr = cast<ArrayToPointerExpr>(expr);
      auto optArrayValue = RV().RV;
      auto arrayValue = emitBindOptionals(SGF, optArrayValue,
                                          pointerExpr->getSubExpr());
      return SGF.emitArrayToPointer(pointerExpr, arrayValue,
                                    Extra.get<ArrayAccessInfo>(Kind));
    }

    case RValueStringToPointer: {
      auto pointerExpr = cast<StringToPointerExpr>(expr);
      auto optStringValue = RV().RV;
      auto stringValue =
        emitBindOptionals(SGF, optStringValue, pointerExpr->getSubExpr());
      return SGF.emitStringToPointer(pointerExpr, stringValue,
                                     pointerExpr->getType());
    }
    }
    llvm_unreachable("bad kind");
  }

  ManagedValue emitBindOptionals(SILGenFunction &SGF, ManagedValue optValue,
                                 Expr *expr) {
    expr = expr->getSemanticsProvidingExpr();
    auto bind = dyn_cast<BindOptionalExpr>(expr);

    // If we don't find a bind, the value isn't optional.
    if (!bind) return optValue;

    // Recurse.
    optValue = emitBindOptionals(SGF, optValue, bind->getSubExpr());

    // Check whether the value is non-nil.
    SGF.emitBindOptional(bind, optValue, bind->getDepth());

    // Extract the non-optional value.
    auto &optTL = SGF.getTypeLowering(optValue.getType());
    auto value = SGF.emitUncheckedGetOptionalValueFrom(bind, optValue, optTL);
    return value;
  }

  std::pair<ManagedValue, ManagedValue>
  finishOptionalEvaluation(SILGenFunction &SGF, OptionalEvaluationExpr *eval) {
    SmallVector<ManagedValue, 2> results;

    SGF.emitOptionalEvaluation(eval, eval->getType(), results, SGFContext(),
      [&](SmallVectorImpl<ManagedValue> &results, SGFContext C) {
        // Recurse.
        auto values = finishOriginalExpr(SGF, eval->getSubExpr());

        // Our primary result is the value.
        results.push_back(values.first);

        // Our secondary result is the owner, if we have one.
        if (auto owner = values.second) results.push_back(owner);
      });

    assert(results.size() == 1 || results.size() == 2);

    ManagedValue value = results[0];

    ManagedValue owner;
    if (results.size() == 2) {
      owner = results[1];

      // Create a new value-dependence here if the primary result is
      // trivial.
      auto &valueTL = SGF.getTypeLowering(value.getType());
      if (valueTL.isTrivial()) {
        SILValue dependentValue =
          SGF.B.createMarkDependence(eval, value.forward(SGF),
                                     owner.getValue());
        value = SGF.emitManagedRValueWithCleanup(dependentValue, valueTL);
      }
    }

    return {value, owner};
  }
};

} // end anonymous namespace

/// Perform the formal-access phase of call argument emission by emitting
/// all of the delayed arguments.
static void emitDelayedArguments(SILGenFunction &SGF,
                                 MutableArrayRef<DelayedArgument> delayedArgs,
                         MutableArrayRef<SmallVector<ManagedValue, 4>> args) {
  assert(!delayedArgs.empty());

  SmallVector<std::pair<SILValue, SILLocation>, 4> emittedInoutArgs;
  auto delayedNext = delayedArgs.begin();

  // The assumption we make is that 'args' and 'inoutArgs' were built
  // up in parallel, with empty spots being dropped into 'args'
  // wherever there's an inout argument to insert.
  //
  // Note that this also begins the formal accesses in evaluation order.
  for (auto &siteArgs : args) {
    for (ManagedValue &siteArg : siteArgs) {
      if (siteArg) continue;

      assert(delayedNext != delayedArgs.end());
      auto &delayedArg = *delayedNext;

      // Emit the delayed argument and replace it in the arguments array.
      auto value = delayedArg.emit(SGF);
      siteArg = value;

      // Remember all the simple inouts we emitted so we can perform
      // a basic inout-aliasing analysis.
      // This should be completely obviated by static enforcement.
      if (delayedArg.isSimpleInOut()) {
        emittedInoutArgs.push_back({value.getValue(),
                                    delayedArg.getInOutLocation()});
      }

      if (++delayedNext == delayedArgs.end())
        goto done;
    }
  }

  llvm_unreachable("ran out of null arguments before we ran out of inouts");

done:

  // Check to see if we have multiple inout arguments which obviously
  // alias.  Note that we could do this in a later SILDiagnostics pass
  // as well: this would be stronger (more equivalences exposed) but
  // would have worse source location information.
  for (auto i = emittedInoutArgs.begin(), e = emittedInoutArgs.end();
         i != e; ++i) {
    for (auto j = emittedInoutArgs.begin(); j != i; ++j) {
      if (!RValue::areObviouslySameValue(i->first, j->first)) continue;

      SGF.SGM.diagnose(i->second, diag::inout_argument_alias)
        .highlight(i->second.getSourceRange());
      SGF.SGM.diagnose(j->second, diag::previous_inout_alias)
        .highlight(j->second.getSourceRange());
    }
  }
}

namespace {

/// A destination for an argument other than just "onto to the end
/// of the arguments lists".
///
/// This allows us to re-use the argument expression emitter for
/// some weird cases, like a shuffled tuple where some of the
/// arguments are going into a varargs array.
struct ArgSpecialDest {
  VarargsInfo *SharedInfo;
  unsigned Index;
  CleanupHandle Cleanup;

  ArgSpecialDest() : SharedInfo(nullptr) {}
  explicit ArgSpecialDest(VarargsInfo &info, unsigned index)
    : SharedInfo(&info), Index(index) {}

  // Reference semantics: need to preserve the cleanup handle.
  ArgSpecialDest(const ArgSpecialDest &) = delete;
  ArgSpecialDest &operator=(const ArgSpecialDest &) = delete;
  ArgSpecialDest(ArgSpecialDest &&other)
    : SharedInfo(other.SharedInfo), Index(other.Index),
      Cleanup(other.Cleanup) {
    other.SharedInfo = nullptr;
  }
  ArgSpecialDest &operator=(ArgSpecialDest &&other) {
    assert(!isValid() && "overwriting valid special destination!");
    SharedInfo = other.SharedInfo;
    Index = other.Index;
    Cleanup = other.Cleanup;
    other.SharedInfo = nullptr;
    return *this;
  }

  ~ArgSpecialDest() {
    assert(!isValid() && "failed to deactivate special dest");
  }

  /// Is this a valid special destination?
  ///
  /// Most of the time, most arguments don't have special
  /// destinations, and making an array of Optional<Special special
  /// destinations has t
  bool isValid() const { return SharedInfo != nullptr; }

  /// Fill this special destination with a value.
  void fill(SILGenFunction &SGF, ArgumentSource &&arg,
            AbstractionPattern _unused_origType,
            SILType loweredSubstParamType) {
    assert(isValid() && "filling an invalid destination");

    SILLocation loc = arg.getLocation();
    auto destAddr = SharedInfo->getBaseAddress();
    if (Index != 0) {
      SILValue index = SGF.B.createIntegerLiteral(loc,
                  SILType::getBuiltinWordType(SGF.getASTContext()), Index);
      destAddr = SGF.B.createIndexAddr(loc, destAddr, index);
    }

    assert(destAddr->getType() == loweredSubstParamType.getAddressType());

    auto &destTL = SharedInfo->getBaseTypeLowering();
    Cleanup =
        SGF.enterDormantFormalAccessTemporaryCleanup(destAddr, loc, destTL);

    TemporaryInitialization init(destAddr, Cleanup);
    std::move(arg).forwardInto(SGF, SharedInfo->getBaseAbstractionPattern(),
                               &init, destTL);
  }

  /// Deactivate this special destination.  Must always be called
  /// before destruction.
  void deactivate(SILGenFunction &SGF) {
    assert(isValid() && "deactivating an invalid destination");
    if (Cleanup.isValid())
      SGF.Cleanups.forwardCleanup(Cleanup);
    SharedInfo = nullptr;
  }
};

/// A possibly-discontiguous slice of function parameters claimed by a
/// function application.
class ClaimedParamsRef {
public:
  static constexpr const unsigned NoSkip = (unsigned)-1;
private:
  ArrayRef<SILParameterInfo> Params;
  
  // The index of the param excluded from this range, if any, or ~0.
  unsigned SkipParamIndex;
  
  friend struct ParamLowering;
  explicit ClaimedParamsRef(ArrayRef<SILParameterInfo> params,
                            unsigned skip)
    : Params(params), SkipParamIndex(skip)
  {
    // Eagerly chop a skipped parameter off either end.
    if (SkipParamIndex == 0) {
      Params = Params.slice(1);
      SkipParamIndex = NoSkip;
    }
    assert(!hasSkip() || SkipParamIndex < Params.size());
  }
  
  bool hasSkip() const {
    return SkipParamIndex != (unsigned)NoSkip;
  }
public:
  ClaimedParamsRef() : Params({}), SkipParamIndex(-1) {}
  explicit ClaimedParamsRef(ArrayRef<SILParameterInfo> params)
    : Params(params), SkipParamIndex(NoSkip)
  {}

  struct iterator : public std::iterator<std::random_access_iterator_tag,
                                         SILParameterInfo>
  {
    const SILParameterInfo *Base;
    unsigned I, SkipParamIndex;
    
    iterator(const SILParameterInfo *Base,
             unsigned I, unsigned SkipParamIndex)
      : Base(Base), I(I), SkipParamIndex(SkipParamIndex)
    {}
    
    iterator &operator++() {
      ++I;
      if (I == SkipParamIndex)
        ++I;
      return *this;
    }
    iterator operator++(int) {
      iterator old(*this);
      ++*this;
      return old;
    }
    iterator &operator--() {
      --I;
      if (I == SkipParamIndex)
        --I;
      return *this;
    }
    iterator operator--(int) {
      iterator old(*this);
      --*this;
      return old;
    }
    
    const SILParameterInfo &operator*() const {
      return Base[I];
    }
    const SILParameterInfo *operator->() const {
      return Base + I;
    }
    
    bool operator==(iterator other) const {
      return Base == other.Base && I == other.I
          && SkipParamIndex == other.SkipParamIndex;
    }
    
    bool operator!=(iterator other) const {
      return !(*this == other);
    }
    
    iterator operator+(std::ptrdiff_t distance) const {
      if (distance > 0)
        return goForward(distance);
      if (distance < 0)
        return goBackward(distance);
      return *this;
    }
    iterator operator-(std::ptrdiff_t distance) const {
      if (distance > 0)
        return goBackward(distance);
      if (distance < 0)
        return goForward(distance);
      return *this;
    }
    std::ptrdiff_t operator-(iterator other) const {
      assert(Base == other.Base && SkipParamIndex == other.SkipParamIndex);
      auto baseDistance = (std::ptrdiff_t)I - (std::ptrdiff_t)other.I;
      if (std::min(I, other.I) < SkipParamIndex &&
          std::max(I, other.I) > SkipParamIndex)
        return baseDistance - 1;
      return baseDistance;
    }
    
    iterator goBackward(unsigned distance) const {
      auto result = *this;
      if (I > SkipParamIndex && I <= SkipParamIndex + distance)
        result.I -= (distance + 1);
      result.I -= distance;
      return result;
    }
    
    iterator goForward(unsigned distance) const {
      auto result = *this;
      if (I < SkipParamIndex && I + distance >= SkipParamIndex)
        result.I += distance + 1;
      result.I += distance;
      return result;
    }
  };
  
  iterator begin() const {
    return iterator{Params.data(), 0, SkipParamIndex};
  }
  
  iterator end() const {
    return iterator{Params.data(), (unsigned)Params.size(), SkipParamIndex};
  }
  
  unsigned size() const {
    return Params.size() - (hasSkip() ? 1 : 0);
  }
  
  bool empty() const { return size() == 0; }
  
  SILParameterInfo front() const { return *begin(); }
  
  ClaimedParamsRef slice(unsigned start) const {
    if (start >= SkipParamIndex)
      return ClaimedParamsRef(Params.slice(start + 1), NoSkip);
    return ClaimedParamsRef(Params.slice(start),
                            hasSkip() ? SkipParamIndex - start : NoSkip);
  }
  ClaimedParamsRef slice(unsigned start, unsigned count) const {
    if (start >= SkipParamIndex)
      return ClaimedParamsRef(Params.slice(start + 1, count), NoSkip);
    unsigned newSkip = SkipParamIndex;
    if (hasSkip())
      newSkip -= start;
    
    if (newSkip < count)
      return ClaimedParamsRef(Params.slice(start, count+1), newSkip);
    return ClaimedParamsRef(Params.slice(start, count), NoSkip);
  }
};

using ArgSpecialDestArray = MutableArrayRef<ArgSpecialDest>;

class TupleShuffleArgEmitter;

class ArgEmitter {
  // TODO: Refactor out the parts of ArgEmitter needed by TupleShuffleArgEmitter
  // into its own "context struct".
  friend class TupleShuffleArgEmitter;

  SILGenFunction &SGF;
  SILFunctionTypeRepresentation Rep;
  const Optional<ForeignErrorConvention> &ForeignError;
  ImportAsMemberStatus ForeignSelf;
  ClaimedParamsRef ParamInfos;
  SmallVectorImpl<ManagedValue> &Args;

  /// Track any delayed arguments that are emitted.  Each corresponds
  /// in order to a "hole" (a null value) in Args.
  SmallVectorImpl<DelayedArgument> &DelayedArguments;

  Optional<ArgSpecialDestArray> SpecialDests;
public:
  ArgEmitter(SILGenFunction &SGF, SILFunctionTypeRepresentation Rep,
             ClaimedParamsRef paramInfos,
             SmallVectorImpl<ManagedValue> &args,
             SmallVectorImpl<DelayedArgument> &delayedArgs,
             const Optional<ForeignErrorConvention> &foreignError,
             ImportAsMemberStatus foreignSelf,
             Optional<ArgSpecialDestArray> specialDests = None)
    : SGF(SGF), Rep(Rep), ForeignError(foreignError),
      ForeignSelf(foreignSelf),
      ParamInfos(paramInfos),
      Args(args), DelayedArguments(delayedArgs), SpecialDests(specialDests) {
    assert(!specialDests || specialDests->size() == paramInfos.size());
  }

  void emitTopLevel(ArgumentSource &&arg, AbstractionPattern origParamType) {
    emit(std::move(arg), origParamType);
    maybeEmitForeignErrorArgument();
  }

private:
  void emit(ArgumentSource &&arg, AbstractionPattern origParamType) {
    // If it was a tuple in the original type, or the argument
    // requires the callee to evaluate, the parameters will have
    // been exploded.
    if (origParamType.isTuple() || arg.requiresCalleeToEvaluate()) {
      emitExpanded(std::move(arg), origParamType);
      return;
    }

    auto substArgType = arg.getSubstType();

    // Otherwise, if the substituted type is a tuple, then we should
    // emit the tuple in its most general form, because there's a
    // substitution of an opaque archetype to a tuple or function
    // type in play.  The most general convention is generally to
    // pass the entire tuple indirectly, but if it's not
    // materializable, the convention is actually to break it up
    // into materializable chunks.  See the comment in SILType.cpp.
    if (isUnmaterializableTupleType(substArgType)) {
      assert(origParamType.isTypeParameter());
      emitExpanded(std::move(arg), origParamType);
      return;
    }

    // Okay, everything else will be passed as a single value, one
    // way or another.

    // If this is a discarded foreign static 'self' parameter, force the
    // argument and discard it.
    if (ForeignSelf.isStatic()) {
      std::move(arg).getAsRValue(SGF);
      return;
    }

    // Adjust for the foreign-error argument if necessary.
    maybeEmitForeignErrorArgument();

    // The substituted parameter type.  Might be different from the
    // substituted argument type by abstraction and/or bridging.
    SILParameterInfo param = claimNextParameter();
    ArgSpecialDest *specialDest = claimNextSpecialDest();

    // Make sure we use the same value category for these so that we
    // can hereafter just use simple equality checks to test for
    // abstraction.
    SILType loweredSubstArgType = SGF.getLoweredType(substArgType);
    SILType loweredSubstParamType =
      SILType::getPrimitiveType(param.getType(),
                                loweredSubstArgType.getCategory());

    // If the caller takes the argument indirectly, the argument has an
    // inout type.
    if (param.isIndirectInOut()) {
      assert(!specialDest);
      assert(isa<InOutType>(substArgType));
      emitInOut(std::move(arg), loweredSubstArgType, loweredSubstParamType,
                origParamType, substArgType);
      return;
    }

    // If the original type is passed indirectly, copy to memory if
    // it's not already there.  (Note that this potentially includes
    // conventions which pass indirectly without transferring
    // ownership, like Itanium C++.)
    if (specialDest) {
      assert(param.isFormalIndirect() &&
             "SpecialDest should imply indirect parameter");
      // TODO: Change the way we initialize array storage in opaque mode
      emitIndirectInto(std::move(arg), origParamType, loweredSubstParamType,
                       *specialDest);
      Args.push_back(ManagedValue::forInContext());
      return;
    } else if (SGF.silConv.isSILIndirect(param)) {
      emitIndirect(std::move(arg), loweredSubstArgType, origParamType, param);
      return;
    }

    // Okay, if the original parameter is passed directly, then we
    // just need to handle abstraction differences and bridging.
    assert(!specialDest);
    emitDirect(std::move(arg), loweredSubstArgType, origParamType, param);
  }

  SILParameterInfo claimNextParameter() {
    assert(!ParamInfos.empty());
    auto param = ParamInfos.front();
    ParamInfos = ParamInfos.slice(1);
    return param;
  }

  /// Claim the next destination, returning a null pointer if there
  /// is no special destination.
  ArgSpecialDest *claimNextSpecialDest() {
    if (!SpecialDests) return nullptr;
    assert(!SpecialDests->empty());
    auto dest = &SpecialDests->front();
    SpecialDests = SpecialDests->slice(1);
    return (dest->isValid() ? dest : nullptr);
  }

  bool isUnmaterializableTupleType(CanType type) {
    if (auto tuple = dyn_cast<TupleType>(type))
      if (!tuple->isMaterializable())
        return true;
    return false;
  }

  /// Emit an argument as an expanded tuple.
  void emitExpanded(ArgumentSource &&arg, AbstractionPattern origParamType) {
    assert(!arg.isLValue() && "argument is l-value but parameter is tuple?");

    // If we're working with an r-value, just expand it out and emit
    // all the elements individually.
    if (arg.isRValue()) {
      if (CanTupleType substArgType =
              dyn_cast<TupleType>(arg.getSubstType())) {
        // The original type isn't necessarily a tuple.
        assert(origParamType.matchesTuple(substArgType));

        auto loc = arg.getKnownRValueLocation();
        SmallVector<RValue, 4> elts;
        std::move(arg).asKnownRValue().extractElements(elts);
        for (auto i : indices(substArgType.getElementTypes())) {
          emit({ loc, std::move(elts[i]) },
               origParamType.getTupleElementType(i));
        }
        return;
      }

      auto loc = arg.getKnownRValueLocation();
      SmallVector<RValue, 1> elts;
      std::move(arg).asKnownRValue().extractElements(elts);
      emit({ loc, std::move(elts[0]) },
           origParamType.getTupleElementType(0));
      return;
    }

    // Otherwise, we're working with an expression.
    Expr *e = std::move(arg).asKnownExpr();
    e = e->getSemanticsProvidingExpr();

    // If the source expression is a tuple literal, we can break it
    // up directly.
    if (auto tuple = dyn_cast<TupleExpr>(e)) {
      for (auto i : indices(tuple->getElements())) {
        emit(tuple->getElement(i),
             origParamType.getTupleElementType(i));
      }
      return;
    }

    if (auto shuffle = dyn_cast<TupleShuffleExpr>(e)) {
      emitShuffle(shuffle, origParamType);
      return;
    }

    // Fall back to the r-value case.
    emitExpanded({ e, SGF.emitRValue(e) }, origParamType);
  }

  void emitShuffle(TupleShuffleExpr *shuffle, AbstractionPattern origType);

  void emitIndirect(ArgumentSource &&arg,
                    SILType loweredSubstArgType,
                    AbstractionPattern origParamType,
                    SILParameterInfo param) {
    auto contexts = getRValueEmissionContexts(loweredSubstArgType, param);
    ManagedValue result;

    // If no abstraction is required, try to honor the emission contexts.
    if (!contexts.RequiresReabstraction) {
      auto loc = arg.getLocation();

      // Peephole certain argument emissions.
      if (arg.isExpr()) {
        auto expr = std::move(arg).asKnownExpr();

        // Try the peepholes.
        if (maybeEmitDelayed(expr, OriginalArgument(expr, /*indirect*/ true)))
          return;

        // Otherwise, just use the default logic.
        result = SGF.emitRValueAsSingleValue(expr, contexts.ForEmission);
      } else {
        result = std::move(arg).getAsSingleValue(SGF, contexts.ForEmission);
      }

      // If it's not already in memory, put it there.
      if (!result.getType().isAddress()) {
        result = result.materialize(SGF, loc);
      }

    // Otherwise, simultaneously emit and reabstract.
    } else {
      result = std::move(arg).materialize(SGF, origParamType,
                                          SGF.getSILType(param));
    }

    Args.push_back(result);
  }

  void emitIndirectInto(ArgumentSource &&arg,
                        AbstractionPattern origType,
                        SILType loweredSubstParamType,
                        ArgSpecialDest &dest) {
    dest.fill(SGF, std::move(arg), origType, loweredSubstParamType);
  }

  void emitInOut(ArgumentSource &&arg,
                 SILType loweredSubstArgType, SILType loweredSubstParamType,
                 AbstractionPattern origType, CanType substType) {
    SILLocation loc = arg.getLocation();

    LValue lv = [&]{
      // If the argument is already lowered to an LValue, it must be the
      // receiver of a self argument, which will be the first inout.
      if (arg.isLValue()) {
        return std::move(arg).asKnownLValue();

      // This is logically wrong, but propagating l-values within
      // RValues is hard to avoid in custom argument-emission code
      // without making ArgumentSource capable of holding mixed
      // RValue/LValue tuples.  (materializeForSet has to do this,
      // for one.)  The onus is on the caller to ensure that formal
      // access semantics are honored.
      } else if (arg.isRValue()) {
        auto address = std::move(arg).asKnownRValue()
          .getAsSingleValue(SGF, arg.getKnownRValueLocation());
        assert(address.isLValue());
        auto substObjectType = cast<InOutType>(substType).getObjectType();
        return LValue::forAddress(address, None,
                                  AbstractionPattern(substObjectType),
                                  substObjectType);
      } else {
        auto *e = cast<InOutExpr>(std::move(arg).asKnownExpr()->
                                  getSemanticsProvidingExpr());
        return SGF.emitLValue(e->getSubExpr(), AccessKind::ReadWrite);
      }
    }();

    if (loweredSubstParamType.hasAbstractionDifference(Rep,
                                                       loweredSubstArgType)) {
      AbstractionPattern origObjectType = origType.transformType(
        [](CanType type)->CanType {
          return CanType(type->getInOutObjectType());
        });
      lv.addSubstToOrigComponent(origObjectType, loweredSubstParamType);
    }

    // Leave an empty space in the ManagedValue sequence and
    // remember that we had an inout argument.
    DelayedArguments.emplace_back(DelayedArgument::InOut, std::move(lv), loc);
    Args.push_back(ManagedValue());
    return;
  }

  void emitDirect(ArgumentSource &&arg, SILType loweredSubstArgType,
                  AbstractionPattern origParamType,
                  SILParameterInfo param) {
    ManagedValue value;
    auto contexts = getRValueEmissionContexts(loweredSubstArgType, param);
    if (contexts.RequiresReabstraction) {
      switch (getSILFunctionLanguage(Rep)) {
      case SILFunctionLanguage::Swift:
        value = emitSubstToOrigArgument(std::move(arg), loweredSubstArgType,
                                        origParamType, param);
        break;
      case SILFunctionLanguage::C:
        value = emitNativeToBridgedArgument(
            std::move(arg), loweredSubstArgType, origParamType, param);
        break;
      }

    // Peephole certain argument emissions.
    } else if (arg.isExpr()) {
      auto expr = std::move(arg).asKnownExpr();

      // Try the peepholes.
      if (maybeEmitDelayed(expr, OriginalArgument(expr, /*indirect*/ false)))
        return;

      // Otherwise, just use the default logic.
      value = SGF.emitRValueAsSingleValue(expr, contexts.ForEmission);
    } else {
      value = std::move(arg).getAsSingleValue(SGF, contexts.ForEmission);
    }

    if (param.isConsumed() &&
        value.getOwnershipKind() == ValueOwnershipKind::Guaranteed) {
      value = value.copyUnmanaged(SGF, arg.getLocation());
      Args.push_back(value);
      return;
    }

    if (SGF.F.getModule().getOptions().EnableSILOwnership) {
      if (param.isDirectGuaranteed() &&
          value.getOwnershipKind() == ValueOwnershipKind::Owned) {
        value = value.borrow(SGF, arg.getLocation());
        Args.push_back(value);
        return;
      }
    }

    Args.push_back(value);
  }

  bool maybeEmitDelayed(Expr *expr, OriginalArgument original) {
    expr = expr->getSemanticsProvidingExpr();

    // Delay accessing inout-to-pointer arguments until the call.
    if (auto inoutToPointer = dyn_cast<InOutToPointerExpr>(expr)) {
      return emitDelayedConversion(inoutToPointer, original);
    }

    // Delay accessing array-to-pointer arguments until the call.
    if (auto arrayToPointer = dyn_cast<ArrayToPointerExpr>(expr)) {
      return emitDelayedConversion(arrayToPointer, original);
    }

    // Delay accessing string-to-pointer arguments until the call.
    if (auto stringToPointer = dyn_cast<StringToPointerExpr>(expr)) {
      return emitDelayedConversion(stringToPointer, original);
    }

    // Any recursive cases we handle here need to be handled in
    // DelayedArgument::finishOriginalExpr.

    // Handle optional evaluations.
    if (auto optional = dyn_cast<OptionalEvaluationExpr>(expr)) {
      // The validity of just recursing here depends on the fact
      // that we only return true for the specific conversions above,
      // which are constrained by the ASTVerifier to only appear in
      // specific forms.
      return maybeEmitDelayed(optional->getSubExpr(), original);
    }

    // Handle injections into optionals.
    if (auto inject = dyn_cast<InjectIntoOptionalExpr>(expr)) {
      return maybeEmitDelayed(inject->getSubExpr(), original);
    }

    // Handle try! expressions.
    if (auto forceTry = dyn_cast<ForceTryExpr>(expr)) {
      // Any expressions in the l-value must be routed appropriately.
      SILGenFunction::ForceTryEmission emission(SGF, forceTry);

      return maybeEmitDelayed(forceTry->getSubExpr(), original);
    }

    return false;
  }

  bool emitDelayedConversion(InOutToPointerExpr *pointerExpr,
                             OriginalArgument original) {
    auto info = SGF.getPointerAccessInfo(pointerExpr->getType());
    LValueOptions options;
    options.IsNonAccessing = pointerExpr->isNonAccessing();
    LValue lv = SGF.emitLValue(pointerExpr->getSubExpr(), info.AccessKind,
                               options);
    DelayedArguments.emplace_back(info, std::move(lv), pointerExpr, original);
    Args.push_back(ManagedValue());
    return true;
  }

  bool emitDelayedConversion(ArrayToPointerExpr *pointerExpr,
                             OriginalArgument original) {
    auto arrayExpr = pointerExpr->getSubExpr();

    // If the source of the conversion is an inout, emit the l-value
    // but delay the formal access.
    if (auto inoutType = arrayExpr->getType()->getAs<InOutType>()) {
      auto info = SGF.getArrayAccessInfo(pointerExpr->getType(),
                                         inoutType->getObjectType());
      LValueOptions options;
      options.IsNonAccessing = pointerExpr->isNonAccessing();
      LValue lv = SGF.emitLValue(arrayExpr, info.AccessKind, options);
      DelayedArguments.emplace_back(info, std::move(lv), pointerExpr,
                                    original);
      Args.push_back(ManagedValue());
      return true;
    }

    // Otherwise, it's an r-value conversion.
    auto info = SGF.getArrayAccessInfo(pointerExpr->getType(),
                                       arrayExpr->getType());

    auto rvalueExpr = lookThroughBindOptionals(arrayExpr);
    ManagedValue value = SGF.emitRValueAsSingleValue(rvalueExpr);
    DelayedArguments.emplace_back(DelayedArgument::RValueArrayToPointer,
                                  info, value, original);
    Args.push_back(ManagedValue());
    return true;
  }

  /// Emit an rvalue-array-to-pointer conversion as a delayed argument.
  bool emitDelayedConversion(StringToPointerExpr *pointerExpr,
                             OriginalArgument original) {
    auto rvalueExpr = lookThroughBindOptionals(pointerExpr->getSubExpr());
    ManagedValue value = SGF.emitRValueAsSingleValue(rvalueExpr);
    DelayedArguments.emplace_back(DelayedArgument::RValueStringToPointer,
                                  value, original);
    Args.push_back(ManagedValue());
    return true;
  }

  static Expr *lookThroughBindOptionals(Expr *expr) {
    while (true) {
      expr = expr->getSemanticsProvidingExpr();
      if (auto bind = dyn_cast<BindOptionalExpr>(expr)) {
        expr = bind->getSubExpr();
      } else {
        return expr;
      }
    }
  }

  ManagedValue emitSubstToOrigArgument(ArgumentSource &&arg,
                                       SILType loweredSubstArgType,
                                       AbstractionPattern origParamType,
                                       SILParameterInfo param) {
    Scope scope(SGF, arg.getLocation());

    // TODO: We should take the opportunity to peephole certain abstraction
    // changes here, for instance, directly emitting a closure literal at the
    // callee's expected abstraction level instead of emitting it maximally
    // substituted and thunking.
    auto emitted = emitArgumentFromSource(std::move(arg), loweredSubstArgType,
                                          origParamType, param);
    ManagedValue result = SGF.emitSubstToOrigValue(
        emitted.loc, std::move(emitted.value).getScalarValue(), origParamType,
        emitted.value.getType(), emitted.contextForReabstraction);
    return scope.popPreservingValue(result);
  }
  
  CanType getAnyObjectType() {
    return SGF.getASTContext().getAnyObjectType();
  }
  bool isAnyObjectType(CanType t) {
    return t == getAnyObjectType();
  }
  
  ManagedValue emitNativeToBridgedArgument(ArgumentSource &&arg,
                                           SILType loweredSubstArgType,
                                           AbstractionPattern origParamType,
                                           SILParameterInfo param) {
    Scope scope(SGF, arg.getLocation());

    // If we're bridging a concrete type to `id` via Any, skip the Any
    // boxing.
    
    // TODO: Generalize. Similarly, when bridging from NSFoo -> Foo -> NSFoo,
    // we should elide the bridge altogether and pass the original object.
    auto paramObjTy = param.getType();
    if (auto objTy = paramObjTy.getAnyOptionalObjectType())
      paramObjTy = objTy;
    if (isAnyObjectType(paramObjTy) && !arg.isRValue()) {
      return scope.popPreservingValue(emitNativeToBridgedObjectArgument(
          std::move(arg).asKnownExpr(), loweredSubstArgType, origParamType,
          param));
    }
    
    auto emitted = emitArgumentFromSource(std::move(arg), loweredSubstArgType,
                                          origParamType, param);

    return scope.popPreservingValue(SGF.emitNativeToBridgedValue(
        emitted.loc,
        std::move(emitted.value).getAsSingleValue(SGF, emitted.loc), Rep,
        param.getType()));
  }
  
  enum class ExistentialPeepholeOptionality {
    /// A non-optional value erased to a non-optional existential.
    Nonoptional,
    
    /// A non-optional value erased to an optional existential.
    NonoptionalToOptional,
    
    /// An optional value erased to an optional existential.
    OptionalToOptional,
  };
  
  std::pair<Expr *, ExistentialPeepholeOptionality>
  lookThroughExistentialErasures(Expr *argExpr) {
    auto origArgExpr = argExpr;
  
    auto optionality = ExistentialPeepholeOptionality::Nonoptional;
    argExpr = argExpr->getSemanticsProvidingExpr();
    
    // Check for an OptionalEvaluation. If we see one we'll want to match it
    // to the inner BindOptional.
    if (auto optEval = dyn_cast<OptionalEvaluationExpr>(argExpr)) {
      
      // The result of the conversion should be promoted back to optional
      // at the outermost level.
      if (auto inject = dyn_cast<InjectIntoOptionalExpr>(
                       optEval->getSubExpr()->getSemanticsProvidingExpr())) {
        optionality = ExistentialPeepholeOptionality::OptionalToOptional;
        argExpr = inject->getSubExpr()->getSemanticsProvidingExpr();
      }
    }
    
    // Look through a BindOptionalExpr if we have an optional-to-optional
    // peephole, or fail the peephole if there isn't a BindOptionalToOptional.
    auto tryToBindOptional =
      [&](Expr *subExpr) -> std::pair<Expr *, ExistentialPeepholeOptionality> {
        if (optionality ==
              ExistentialPeepholeOptionality::OptionalToOptional) {
          // If we see the binding, look through it.
          if (auto bind = dyn_cast<BindOptionalExpr>(subExpr))
            return {bind->getSubExpr()->getSemanticsProvidingExpr(),
                    optionality};
          // Otherwise, we don't know what we're seeing. Back out of the
          // peephole.
          return {origArgExpr, ExistentialPeepholeOptionality::Nonoptional};
        }
        
        return {subExpr, optionality};
      };
    
    // Look through an optional injection.
    if (auto inject = dyn_cast<InjectIntoOptionalExpr>(argExpr)) {
      optionality = ExistentialPeepholeOptionality::NonoptionalToOptional;
      argExpr = inject->getSubExpr()->getSemanticsProvidingExpr();
    }

    // When converting from an existential type to a more general existential,
    // the inner existential is opened first. Look through this pattern.
    if (auto open = dyn_cast<OpenExistentialExpr>(argExpr)) {
      auto subExpr = open->getSubExpr()->getSemanticsProvidingExpr();
      while (auto erasure = dyn_cast<ErasureExpr>(subExpr)) {
        subExpr = erasure->getSubExpr()->getSemanticsProvidingExpr();
      }
      // If we drilled down to the underlying opened existential, look
      // through it.
      if (subExpr == open->getOpaqueValue())
        return tryToBindOptional(open->getExistentialValue());
      // TODO: Maybe there are other peepholes we could attempt on opened
      // existentials?
      return tryToBindOptional(open);
    }
    
    // Look through ErasureExprs and try to bridge the underlying
    // concrete value instead.
    while (auto erasure = dyn_cast<ErasureExpr>(argExpr))
      argExpr = erasure->getSubExpr()->getSemanticsProvidingExpr();

    return tryToBindOptional(argExpr);
  }
  
  /// Emit an argument expression that we know will be bridged to an
  /// Objective-C object.
  ManagedValue emitNativeToBridgedObjectArgument(Expr *argExpr,
                                             SILType loweredSubstArgType,
                                             AbstractionPattern origParamType,
                                             SILParameterInfo param) {
    auto origArgExpr = argExpr;
    // Look through existential erasures.
    ExistentialPeepholeOptionality optionality;
    std::tie(argExpr, optionality) = lookThroughExistentialErasures(argExpr);
    
    // TODO: Only do the peephole for trivially-lowered types, since we
    // unfortunately don't plumb formal types through
    // emitNativeToBridgedValue, so can't correctly construct the
    // substitution for the call to _bridgeAnythingToObjectiveC for function
    // or metatype values.
    if (!argExpr->getType()->isLegalSILType()) {
      argExpr = origArgExpr;
      optionality = ExistentialPeepholeOptionality::Nonoptional;
    }
    
    // Emit the argument.
    auto contexts = getRValueEmissionContexts(loweredSubstArgType, param);
    ManagedValue emittedArg = SGF.emitRValue(argExpr, contexts.ForEmission)
      .getAsSingleValue(SGF, argExpr);
    
    // Early exit if we already exactly match the parameter type.
    if (emittedArg.getType() == SGF.getSILType(param)) {
      return emittedArg;
    }
    
    // Factor the bridging conversion out in case we need to do it as an
    // optional-to-optional transform.
    auto doBridge = [&](SILGenFunction &SGF,
                        SILLocation loc,
                        ManagedValue emittedArg,
                        SILType loweredResultTy) -> ManagedValue {
      // If the argument is not already a class instance, bridge it.
      if (!emittedArg.getType().getSwiftRValueType()->mayHaveSuperclass()
          && !emittedArg.getType().isClassExistentialType()) {
        emittedArg = SGF.emitNativeToBridgedValue(loc, emittedArg, Rep,
                                        loweredResultTy.getSwiftRValueType());
      }
      auto emittedArgTy = emittedArg.getType().getSwiftRValueType();
      assert(emittedArgTy->mayHaveSuperclass()
        || emittedArgTy->isClassExistentialType());
      
      // Upcast reference types to AnyObject.
      if (!isAnyObjectType(emittedArgTy)) {
        // Open class existentials first to upcast the reference inside.
        if (emittedArgTy->isClassExistentialType()) {
          emittedArgTy = ArchetypeType::getOpened(emittedArgTy);
          auto opened = SGF.B.createOpenExistentialRef(loc,
                               emittedArg.getValue(),
                               SILType::getPrimitiveObjectType(emittedArgTy));
          emittedArg = ManagedValue(opened, emittedArg.getCleanup());
        }
        
        auto erased = SGF.B.createInitExistentialRef(loc,
                         SILType::getPrimitiveObjectType(getAnyObjectType()),
                         emittedArgTy, emittedArg.getValue(), {});
        emittedArg = ManagedValue(erased, emittedArg.getCleanup());
      }
      
      assert(isAnyObjectType(emittedArg.getType().getSwiftRValueType()));
      return emittedArg;
    };
    
    // Bind the optional value if we started with an optional.
    bool nativeIsOptional = (bool)emittedArg.getType().getSwiftRValueType()
      ->getAnyOptionalObjectType();
    bool bridgedIsOptional =
        (bool)param.getType()->getAnyOptionalObjectType();
    if (nativeIsOptional && bridgedIsOptional) {
      return SGF.emitOptionalToOptional(argExpr, emittedArg,
                                        SGF.getSILType(param), doBridge);
    } else if (!nativeIsOptional && bridgedIsOptional) {
      auto paramObjTy = SGF.getSILType(param).getAnyOptionalObjectType();
      auto transformed = doBridge(SGF, argExpr, emittedArg,
                                  paramObjTy);
      // Inject into optional.
      auto opt = SGF.B.createEnum(argExpr, transformed.getValue(),
                                  SGF.getASTContext().getOptionalSomeDecl(),
                                  SGF.getSILType(param));
      return ManagedValue(opt, transformed.getCleanup());
    } else {
      return doBridge(SGF, argExpr, emittedArg, SGF.getSILType(param));
    }
  }
  
  struct EmittedArgument {
    SILLocation loc;
    RValue value;
    SGFContext contextForReabstraction;
  };
  EmittedArgument emitArgumentFromSource(ArgumentSource &&arg,
                                         SILType loweredSubstArgType,
                                         AbstractionPattern origParamType,
                                         SILParameterInfo param) {
    auto contexts = getRValueEmissionContexts(loweredSubstArgType, param);
    Optional<SILLocation> loc;
    RValue rv;
    if (arg.isRValue()) {
      loc = arg.getKnownRValueLocation();
      rv = std::move(arg).asKnownRValue();
    } else {
      Expr *e = std::move(arg).asKnownExpr();
      loc = e;
      rv = SGF.emitRValue(e, contexts.ForEmission);
    }
    return {*loc, std::move(rv), contexts.ForReabstraction};
  }
  
  void maybeEmitForeignErrorArgument() {
    if (!ForeignError ||
        ForeignError->getErrorParameterIndex() != Args.size())
      return;

    SILParameterInfo param = claimNextParameter();
    ArgSpecialDest *specialDest = claimNextSpecialDest();

    assert(param.getConvention() == ParameterConvention::Direct_Unowned);
    assert(!specialDest && "special dest for error argument?");
    (void) param; (void) specialDest;

    // Leave a placeholder in the position.
    Args.push_back(ManagedValue::forInContext());
  }

  struct EmissionContexts {
    /// The context for emitting the r-value.
    SGFContext ForEmission;
    /// The context for reabstracting the r-value.
    SGFContext ForReabstraction;
    /// If the context requires reabstraction
    bool RequiresReabstraction;
  };
  static EmissionContexts getRValueEmissionContexts(SILType loweredArgType,
                                                    SILParameterInfo param) {
    bool requiresReabstraction =
        loweredArgType.getSwiftRValueType() != param.getType();
    // If the parameter is consumed, we have to emit at +1.
    if (param.isConsumed()) {
      return {SGFContext(), SGFContext(), requiresReabstraction};
    }

    // Otherwise, we can emit the final value at +0 (but only with a
    // guarantee that the value will survive).
    //
    // TODO: we can pass at +0 (immediate) to an unowned parameter
    // if we know that there will be no arbitrary side-effects
    // between now and the call.
    SGFContext finalContext = SGFContext::AllowGuaranteedPlusZero;

    // If the r-value doesn't require reabstraction, the final context
    // is the emission context.
    if (!requiresReabstraction) {
      return {finalContext, SGFContext(), requiresReabstraction};
    }

    // Otherwise, the final context is the reabstraction context.
    return {SGFContext(), finalContext, requiresReabstraction};
  }
};

} // end anonymous namespace

/// Decompose a type, whether it is a tuple or a single type, into an
/// array of tuple type elements.
static ArrayRef<TupleTypeElt> decomposeTupleOrSingle(Type type,
                                                     TupleTypeElt &single) {
  if (auto tupleTy = type->getAs<TupleType>()) {
    return tupleTy->getElements();
  }

  single = TupleTypeElt(type);
  return single;
}

namespace {

struct ElementExtent {
  /// The parameters which go into this tuple element.
  /// This is set in the first pass.
  ClaimedParamsRef Params;
  /// The destination index, if any.
  /// This is set in the first pass.
  unsigned DestIndex : 30;
  unsigned HasDestIndex : 1;
#ifndef NDEBUG
  unsigned Used : 1;
#endif
  /// The arguments which feed this tuple element.
  /// This is set in the second pass.
  ArrayRef<ManagedValue> Args;
  /// The inout arguments which feed this tuple element.
  /// This is set in the second pass.
  MutableArrayRef<DelayedArgument> DelayedArgs;

  ElementExtent()
      : HasDestIndex(false)
#ifndef NDEBUG
        ,
        Used(false)
#endif
  {
  }
};

class TupleShuffleArgEmitter {
  Expr *inner;
  Expr *outer;
  ArrayRef<TupleTypeElt> innerElts;
  ConcreteDeclRef defaultArgsOwner;
  ArrayRef<Expr *> callerDefaultArgs;
  ArrayRef<int> elementMapping;
  ArrayRef<unsigned> variadicArgs;
  Type varargsArrayType;
  AbstractionPattern origParamType;

  TupleTypeElt singleOuterElement;
  ArrayRef<TupleTypeElt> outerElements;
  CanType canVarargsArrayType;

  /// The original parameter type.
  SmallVector<AbstractionPattern, 8> origInnerElts;
  AbstractionPattern innerOrigParamType;
  /// Flattened inner parameter sequence.
  SmallVector<SILParameterInfo, 8> innerParams;
  /// Extents of the inner elements.
  SmallVector<ElementExtent, 8> innerExtents;
  Optional<VarargsInfo> varargsInfo;
  SILParameterInfo variadicParamInfo; // innerExtents will point at this
  Optional<SmallVector<ArgSpecialDest, 8>> innerSpecialDests;

  // Used by flattenPatternFromInnerExtendIntoInnerParams and
  // splitInnerArgumentsCorrectly.
  SmallVector<ManagedValue, 8> innerArgs;
  SmallVector<DelayedArgument, 2> innerDelayedArgs;

public:
  TupleShuffleArgEmitter(TupleShuffleExpr *e, ArrayRef<TupleTypeElt> innerElts,
                         AbstractionPattern origParamType)
      : inner(e->getSubExpr()), outer(e), innerElts(innerElts),
        defaultArgsOwner(e->getDefaultArgsOwner()),
        callerDefaultArgs(e->getCallerDefaultArgs()),
        elementMapping(e->getElementMapping()),
        variadicArgs(e->getVariadicArgs()),
        varargsArrayType(e->getVarargsArrayTypeOrNull()),
        origParamType(origParamType), singleOuterElement(), outerElements(),
        canVarargsArrayType(),
        origInnerElts(innerElts.size(), AbstractionPattern::getInvalid()),
        innerOrigParamType(AbstractionPattern::getInvalid()), innerParams(),
        innerExtents(innerElts.size()), varargsInfo(), variadicParamInfo(),
        innerSpecialDests() {
    outerElements = decomposeTupleOrSingle(outer->getType()->getCanonicalType(),
                                           singleOuterElement);
    if (varargsArrayType)
      canVarargsArrayType = varargsArrayType->getCanonicalType();
  }

  TupleShuffleArgEmitter(const TupleShuffleArgEmitter &) = delete;
  TupleShuffleArgEmitter &operator=(const TupleShuffleArgEmitter &) = delete;
  TupleShuffleArgEmitter(TupleShuffleArgEmitter &&) = delete;
  TupleShuffleArgEmitter &operator=(TupleShuffleArgEmitter &&) = delete;

  void emit(ArgEmitter &parent);

private:
  void constructInnerTupleTypeInfo(ArgEmitter &parent);
  void flattenPatternFromInnerExtendIntoInnerParams(ArgEmitter &parent);
  void splitInnerArgumentsCorrectly(ArgEmitter &parent);
  void emitDefaultArgsAndFinalize(ArgEmitter &parent);
};

} // end anonymous namespace

void TupleShuffleArgEmitter::constructInnerTupleTypeInfo(ArgEmitter &parent) {
  unsigned nextParamIndex = 0;
  for (unsigned outerIndex : indices(outerElements)) {
    CanType substEltType =
        outerElements[outerIndex].getType()->getCanonicalType();
    AbstractionPattern origEltType =
        origParamType.getTupleElementType(outerIndex);
    unsigned numParams =
        getFlattenedValueCount(origEltType, substEltType, parent.ForeignSelf);

    // Skip the foreign-error parameter.
    assert((!parent.ForeignError ||
            parent.ForeignError->getErrorParameterIndex() <= nextParamIndex ||
            parent.ForeignError->getErrorParameterIndex() >=
                nextParamIndex + numParams) &&
           "error parameter falls within shuffled range?");
    if (numParams && // Don't skip it twice if there's an empty tuple.
        parent.ForeignError &&
        parent.ForeignError->getErrorParameterIndex() == nextParamIndex) {
      nextParamIndex++;
    }

    // Grab the parameter infos corresponding to this tuple element
    // (but don't drop them from ParamInfos yet).
    auto eltParams = parent.ParamInfos.slice(nextParamIndex, numParams);
    nextParamIndex += numParams;

    int innerIndex = elementMapping[outerIndex];
    if (innerIndex >= 0) {
#ifndef NDEBUG
      assert(!innerExtents[innerIndex].Used && "using element twice");
      innerExtents[innerIndex].Used = true;
#endif
      innerExtents[innerIndex].Params = eltParams;
      origInnerElts[innerIndex] = origEltType;
    } else if (innerIndex == TupleShuffleExpr::Variadic) {
      auto &varargsField = outerElements[outerIndex];
      assert(varargsField.isVararg());
      assert(!varargsInfo.hasValue() && "already had varargs entry?");

      CanType varargsEltType = CanType(varargsField.getVarargBaseTy());
      unsigned numVarargs = variadicArgs.size();
      assert(canVarargsArrayType == substEltType);

      // Create the array value.
      varargsInfo.emplace(emitBeginVarargs(parent.SGF, outer, varargsEltType,
                                           canVarargsArrayType, numVarargs));

      // If we have any varargs, we'll need to actually initialize
      // the array buffer.
      if (numVarargs) {
        // For this, we'll need special destinations.
        assert(!innerSpecialDests);
        innerSpecialDests.emplace();

        // Prepare the variadic "arguments" as single +1 indirect parameters
        // with the array's desired abstraction pattern.  The vararg element
        // type should be materializable, and the abstraction pattern should be
        // opaque, so ArgEmitter's lowering should always generate exactly one
        // "argument" per element even if the substituted element type is a
        // tuple.
        variadicParamInfo =
          SILParameterInfo(varargsInfo->getBaseTypeLowering()
                           .getLoweredType().getSwiftRValueType(),
                           ParameterConvention::Indirect_In);

        unsigned i = 0;
        for (unsigned innerIndex : variadicArgs) {
          // Find out where the next varargs element is coming from.
          assert(innerIndex >= 0 && "special source for varargs element??");
#ifndef NDEBUG
          assert(!innerExtents[innerIndex].Used && "using element twice");
          innerExtents[innerIndex].Used = true;
#endif

          // Set the destination index.
          innerExtents[innerIndex].HasDestIndex = true;
          innerExtents[innerIndex].DestIndex = i++;

          // Use the singleton param info we prepared before.
          innerExtents[innerIndex].Params =
            ClaimedParamsRef(variadicParamInfo);

          // Propagate the element abstraction pattern.
          origInnerElts[innerIndex] =
            varargsInfo->getBaseAbstractionPattern();
        }
      }
    }
  }
}

void TupleShuffleArgEmitter::flattenPatternFromInnerExtendIntoInnerParams(
    ArgEmitter &parent) {
  for (auto &extent : innerExtents) {
    assert(extent.Used && "didn't use all the inner tuple elements!");

    for (auto param : extent.Params) {
      innerParams.push_back(param);
    }

    // Fill in the special destinations array.
    if (innerSpecialDests) {
      // Use the saved index if applicable.
      if (extent.HasDestIndex) {
        assert(extent.Params.size() == 1);
        innerSpecialDests->push_back(
            ArgSpecialDest(*varargsInfo, extent.DestIndex));

        // Otherwise, fill in with the appropriate number of invalid
        // special dests.
      } else {
        // ArgSpecialDest isn't copyable, so we can't just use append.
        for (auto &p : extent.Params) {
          (void)p;
          innerSpecialDests->push_back(ArgSpecialDest());
        }
      }
    }
  }
}

void TupleShuffleArgEmitter::splitInnerArgumentsCorrectly(ArgEmitter &parent) {
  ArrayRef<ManagedValue> nextArgs = innerArgs;
  MutableArrayRef<DelayedArgument> nextDelayedArgs = innerDelayedArgs;
  for (auto &extent : innerExtents) {
    auto length = extent.Params.size();

    // Claim the next N inner args for this inner argument.
    extent.Args = nextArgs.slice(0, length);
    nextArgs = nextArgs.slice(length);

    // Claim the correct number of inout arguments as well.
    size_t numDelayed = 0;
    for (auto arg : extent.Args) {
      assert(!arg.isInContext() || extent.HasDestIndex);
      if (!arg)
        numDelayed++;
    }
    extent.DelayedArgs = nextDelayedArgs.slice(0, numDelayed);
    nextDelayedArgs = nextDelayedArgs.slice(numDelayed);
  }

  assert(nextArgs.empty() && "didn't claim all args");
  assert(nextDelayedArgs.empty() && "didn't claim all inout args");
}

void TupleShuffleArgEmitter::emitDefaultArgsAndFinalize(ArgEmitter &parent) {
  unsigned nextCallerDefaultArg = 0;
  for (unsigned outerIndex = 0, e = outerElements.size();
         outerIndex != e; ++outerIndex) {
    // If this comes from an inner element, move the appropriate
    // inner element values over.
    int innerIndex = elementMapping[outerIndex];
    if (innerIndex >= 0) {
      auto &extent = innerExtents[innerIndex];
      auto numArgs = extent.Args.size();

      parent.maybeEmitForeignErrorArgument();

      // Drop N parameters off of ParamInfos.
      parent.ParamInfos = parent.ParamInfos.slice(numArgs);

      // Move the appropriate inner arguments over as outer arguments.
      parent.Args.append(extent.Args.begin(), extent.Args.end());
      for (auto &delayedArg : extent.DelayedArgs)
        parent.DelayedArguments.push_back(std::move(delayedArg));
      continue;
    }

    // If this is default initialization, call the default argument
    // generator.
    if (innerIndex == TupleShuffleExpr::DefaultInitialize) {
      // Otherwise, emit the default initializer, then map that as a
      // default argument.
      CanType eltType = outerElements[outerIndex].getType()->getCanonicalType();
      auto origType = origParamType.getTupleElementType(outerIndex);
      RValue value = parent.SGF.emitApplyOfDefaultArgGenerator(
          outer, defaultArgsOwner, outerIndex, eltType, origType);
      parent.emit(ArgumentSource(outer, std::move(value)), origType);
      continue;
    }

    // If this is caller default initialization, generate the
    // appropriate value.
    if (innerIndex == TupleShuffleExpr::CallerDefaultInitialize) {
      auto arg = callerDefaultArgs[nextCallerDefaultArg++];
      parent.emit(ArgumentSource(arg),
                  origParamType.getTupleElementType(outerIndex));
      continue;
    }

    // If we're supposed to create a varargs array with the rest, do so.
    if (innerIndex == TupleShuffleExpr::Variadic) {
      auto &varargsField = outerElements[outerIndex];
      assert(varargsField.isVararg() &&
             "Cannot initialize nonvariadic element");
      assert(varargsInfo.hasValue());
      (void) varargsField;

      // We've successfully built the varargs array; deactivate all
      // the special destinations.
      if (innerSpecialDests) {
        for (auto &dest : *innerSpecialDests) {
          if (dest.isValid())
            dest.deactivate(parent.SGF);
        }
      }

      CanType eltType = outerElements[outerIndex].getType()->getCanonicalType();
      ManagedValue varargs =
          emitEndVarargs(parent.SGF, outer, std::move(*varargsInfo));
      parent.emit(
          ArgumentSource(outer, RValue(parent.SGF, outer, eltType, varargs)),
          origParamType.getTupleElementType(outerIndex));
      continue;
    }

    // That's the last special case defined so far.
    llvm_unreachable("unexpected special case in tuple shuffle!");
  }
}

void TupleShuffleArgEmitter::emit(ArgEmitter &parent) {
  // We could support dest addrs here, but it can't actually happen
  // with the current limitations on default arguments in tuples.
  assert(!parent.SpecialDests && "shuffle nested within varargs expansion?");

  // First, construct an abstraction pattern and parameter sequence
  // which we can use to emit the inner tuple.
  constructInnerTupleTypeInfo(parent);

  // The inner abstraction pattern is opaque if we started with an
  // opaque pattern; otherwise, it's a tuple of the de-shuffled
  // tuple elements.
  innerOrigParamType = origParamType;
  if (!origParamType.isTypeParameter()) {
    // That "tuple" might not actually be a tuple.
    if (innerElts.size() == 1 && !innerElts[0].hasName()) {
      innerOrigParamType = origInnerElts[0];
    } else {
      innerOrigParamType = AbstractionPattern::getTuple(origInnerElts);
    }
  }

  flattenPatternFromInnerExtendIntoInnerParams(parent);

  // Emit the inner expression.
  if (!innerParams.empty()) {
    ArgEmitter(parent.SGF, parent.Rep, ClaimedParamsRef(innerParams), innerArgs,
               innerDelayedArgs,
               /*foreign error*/ None, /*foreign self*/ ImportAsMemberStatus(),
               (innerSpecialDests ? ArgSpecialDestArray(*innerSpecialDests)
                                  : Optional<ArgSpecialDestArray>()))
        .emitTopLevel(ArgumentSource(inner), innerOrigParamType);
  }

  // Make a second pass to split the inner arguments correctly.
  splitInnerArgumentsCorrectly(parent);

  // Make a final pass to emit default arguments and move things into
  // the outer arguments lists.
  emitDefaultArgsAndFinalize(parent);
}

void ArgEmitter::emitShuffle(TupleShuffleExpr *E,
                             AbstractionPattern origParamType) {
  ArrayRef<TupleTypeElt> srcElts;
  TupleTypeElt singletonSrcElt;
  if (E->isSourceScalar()) {
    singletonSrcElt = E->getSubExpr()->getType()->getCanonicalType();
    srcElts = singletonSrcElt;
  } else {
    srcElts = cast<TupleType>(E->getSubExpr()->getType()->getCanonicalType())
                     ->getElements();
  }

  TupleShuffleArgEmitter(E, srcElts, origParamType).emit(*this);
}

namespace {
/// Cleanup to destroy an uninitialized box.
class DeallocateUninitializedBox : public Cleanup {
  SILValue box;
public:
  DeallocateUninitializedBox(SILValue box) : box(box) {}

  void emit(SILGenFunction &SGF, CleanupLocation l) override {
    SGF.B.createDeallocBox(l, box);
  }

  void dump(SILGenFunction &SGF) const override {
#ifndef NDEBUG
    llvm::errs() << "DeallocateUninitializedBox "
                 << "State:" << getState() << " "
                 << "Box: " << box << "\n";
#endif
  }
};
} // end anonymous namespace

static CleanupHandle enterDeallocBoxCleanup(SILGenFunction &SGF, SILValue box) {
  SGF.Cleanups.pushCleanup<DeallocateUninitializedBox>(box);
  return SGF.Cleanups.getTopCleanup();
}

/// This is an initialization for a box.
class BoxInitialization : public SingleBufferInitialization {
  SILValue box;
  SILValue addr;
  CleanupHandle uninitCleanup;
  CleanupHandle initCleanup;

public:
  BoxInitialization(SILValue box, SILValue addr,
                    CleanupHandle uninitCleanup,
                    CleanupHandle initCleanup)
    : box(box), addr(addr),
      uninitCleanup(uninitCleanup),
      initCleanup(initCleanup) {}

  void finishInitialization(SILGenFunction &SGF) override {
    SingleBufferInitialization::finishInitialization(SGF);
    SGF.Cleanups.setCleanupState(uninitCleanup, CleanupState::Dead);
    if (initCleanup.isValid())
        SGF.Cleanups.setCleanupState(initCleanup, CleanupState::Active);
  }

  SILValue getAddressForInPlaceInitialization(SILGenFunction &SGF,
                                              SILLocation loc) override {
    return addr;
  }

  bool isInPlaceInitializationOfGlobal() const override {
    return false;
  }

  ManagedValue getManagedBox() const {
    return ManagedValue(box, initCleanup);
  }
};

/// Emits SIL instructions to create an enum value. Attempts to avoid
/// unnecessary copies by emitting the payload directly into the enum
/// payload, or into the box in the case of an indirect payload.
ManagedValue SILGenFunction::emitInjectEnum(SILLocation loc,
                                            ArgumentSource payload,
                                            SILType enumTy,
                                            EnumElementDecl *element,
                                            SGFContext C) {
  element = SGM.getLoweredEnumElementDecl(element);

  // Easy case -- no payload
  if (!payload) {
    if (enumTy.isLoadable(SGM.M) || !silConv.useLoweredAddresses()) {
      return emitManagedRValueWithCleanup(
        B.createEnum(loc, SILValue(), element,
                     enumTy.getObjectType()));
    }

    // Emit the enum directly into the context if possible
    return B.bufferForExpr(loc, enumTy, getTypeLowering(enumTy), C,
                           [&](SILValue newAddr) {
                             B.createInjectEnumAddr(loc, newAddr, element);
                           });
  }

  ManagedValue payloadMV;
  AbstractionPattern origFormalType =
    (element == getASTContext().getOptionalSomeDecl()
      ? AbstractionPattern(payload.getSubstType())
      : SGM.M.Types.getAbstractionPattern(element));
  auto &payloadTL = getTypeLowering(origFormalType,
                                    payload.getSubstType());

  SILType loweredPayloadType = payloadTL.getLoweredType();

  // If the payload is indirect, emit it into a heap allocated box.
  //
  // To avoid copies, evaluate it directly into the box, being
  // careful to stage the cleanups so that if the expression
  // throws, we know to deallocate the uninitialized box.
  if (element->isIndirect() ||
      element->getParentEnum()->isIndirect()) {
    auto boxTy = SILBoxType::get(payloadTL.getLoweredType().getSwiftRValueType());
    auto *box = B.createAllocBox(loc, boxTy);
    auto *addr = B.createProjectBox(loc, box, 0);

    CleanupHandle initCleanup = enterDestroyCleanup(box);
    Cleanups.setCleanupState(initCleanup, CleanupState::Dormant);
    CleanupHandle uninitCleanup = enterDeallocBoxCleanup(*this, box);

    BoxInitialization dest(box, addr, uninitCleanup, initCleanup);

    std::move(payload).forwardInto(*this, origFormalType,
                                   &dest, payloadTL);

    payloadMV = dest.getManagedBox();
    loweredPayloadType = payloadMV.getType();
  }

  // Loadable with payload
  if (enumTy.isLoadable(SGM.M) || !silConv.useLoweredAddresses()) {
    if (!payloadMV) {
      // If the payload was indirect, we already evaluated it and
      // have a single value. Otherwise, evaluate the payload.
      payloadMV = std::move(payload).getAsSingleValue(*this, origFormalType);
    }

    SILValue argValue = payloadMV.forward(*this);

    return emitManagedRValueWithCleanup(
               B.createEnum(loc, argValue, element,
                            enumTy.getObjectType()));
  }

  // Address-only with payload
  return B.bufferForExpr(
      loc, enumTy, getTypeLowering(enumTy), C,
      [&](SILValue bufferAddr) {
        SILValue resultData =
          B.createInitEnumDataAddr(loc, bufferAddr, element,
                                   loweredPayloadType.getAddressType());

        if (payloadMV) {
          // If the payload was indirect, we already evaluated it and
          // have a single value. Store it into the result.
          B.emitStoreValueOperation(loc, payloadMV.forward(*this), resultData,
                                    StoreOwnershipQualifier::Init);
        } else if (payloadTL.isLoadable()) {
          // The payload of this specific enum case might be loadable
          // even if the overall enum is address-only.
          payloadMV = std::move(payload).getAsSingleValue(*this, origFormalType);
          B.emitStoreValueOperation(loc, payloadMV.forward(*this), resultData,
                                    StoreOwnershipQualifier::Init);
        } else {
          // The payload is address-only. Evaluate it directly into
          // the enum.
          
          TemporaryInitialization dest(resultData, CleanupHandle::invalid());
          std::move(payload).forwardInto(*this, origFormalType,
                                         &dest, payloadTL);
        }

        // The payload is initialized, now apply the tag.
        B.createInjectEnumAddr(loc, bufferAddr, element);
  });
}

namespace {
  /// A structure for conveniently claiming sets of uncurried parameters.
  struct ParamLowering {
    ArrayRef<SILParameterInfo> Params;
    unsigned ClaimedForeignSelf = -1;
    SILFunctionTypeRepresentation Rep;
    SILFunctionConventions fnConv;

    ParamLowering(CanSILFunctionType fnType, SILGenFunction &SGF)
        : Params(fnType->getParameters()), Rep(fnType->getRepresentation()),
          fnConv(fnType, SGF.SGM.M) {}

    ClaimedParamsRef
    claimParams(AbstractionPattern origParamType, CanType substParamType,
                const Optional<ForeignErrorConvention> &foreignError,
                const ImportAsMemberStatus &foreignSelf) {
      unsigned count = getFlattenedValueCount(origParamType, substParamType,
                                              foreignSelf);
      if (foreignError) count++;
      
      if (foreignSelf.isImportAsMember()) {
        // Claim only the self parameter.
        assert(ClaimedForeignSelf == (unsigned)-1
               && "already claimed foreign self?!");
        if (foreignSelf.isStatic()) {
          // Imported as a static method, no real self param to claim.
          return {};
        }
        ClaimedForeignSelf = foreignSelf.getSelfIndex();
        return ClaimedParamsRef(Params[ClaimedForeignSelf],
                                ClaimedParamsRef::NoSkip);
      }
      
      if (ClaimedForeignSelf != (unsigned)-1) {
        assert(count + 1 == Params.size()
               && "not claiming all params after foreign self?!");
        auto result = Params;
        Params = {};
        return ClaimedParamsRef(result, ClaimedForeignSelf);
      }
      
      assert(count <= Params.size());
      auto result = Params.slice(Params.size() - count, count);
      Params = Params.slice(0, Params.size() - count);
      return ClaimedParamsRef(result, (unsigned)-1);
    }
    
    ArrayRef<SILParameterInfo>
    claimCaptureParams(ArrayRef<ManagedValue> captures) {
      auto firstCapture = Params.size() - captures.size();
#ifndef NDEBUG
      assert(Params.size() >= captures.size()
             && "more captures than params?!");
      for (unsigned i = 0; i < captures.size(); ++i) {
        assert(fnConv.getSILType(Params[i + firstCapture])
                   == captures[i].getType()
               && "capture doesn't match param type");
      }
#endif
      
      auto result = Params.slice(firstCapture, captures.size());
      Params = Params.slice(0, firstCapture);
      return result;
    }

    ~ParamLowering() {
      assert(Params.empty() && "didn't consume all the parameters");
    }
  };

  /// An application of possibly unevaluated arguments in the form of an
  /// ArgumentSource to a Callee.
  class CallSite {
  public:
    SILLocation Loc;
    CanType SubstResultType;

  private:
    ArgumentSource ArgValue;
    bool Throws;

  public:
    CallSite(ApplyExpr *apply)
      : Loc(apply), SubstResultType(apply->getType()->getCanonicalType()),
        ArgValue(apply->getArg()), Throws(apply->throws()) {
    }

    CallSite(SILLocation loc, ArgumentSource &&value,
             CanType resultType, bool throws)
      : Loc(loc), SubstResultType(resultType),
        ArgValue(std::move(value)), Throws(throws) {
    }

    CallSite(SILLocation loc, ArgumentSource &&value,
             CanAnyFunctionType fnType)
      : CallSite(loc, std::move(value), fnType.getResult(), fnType->throws()) {
    }

    /// Return the substituted, unlowered AST type of the argument.
    CanType getSubstArgType() const {
      return ArgValue.getSubstType();
    }

    /// Return the substituted, unlowered AST type of the result of
    /// this application.
    CanType getSubstResultType() const {
      return SubstResultType;
    }

    bool throws() const { return Throws; }

    /// Evaluate arguments and begin any inout formal accesses.
    void emit(SILGenFunction &SGF, AbstractionPattern origParamType,
              ParamLowering &lowering, SmallVectorImpl<ManagedValue> &args,
              SmallVectorImpl<DelayedArgument> &delayedArgs,
              const Optional<ForeignErrorConvention> &foreignError,
              const ImportAsMemberStatus &foreignSelf) && {
      auto params = lowering.claimParams(origParamType, getSubstArgType(),
                                         foreignError, foreignSelf);

      ArgEmitter emitter(SGF, lowering.Rep, params, args, delayedArgs,
                         foreignError, foreignSelf);
      emitter.emitTopLevel(std::move(ArgValue), origParamType);
    }

    /// Take the arguments for special processing, in place of the above.
    ArgumentSource &&forward() && {
      return std::move(ArgValue);
    }

    /// Returns true if the argument of this value is a single valued RValue
    /// that is passed either at plus zero or is trivial.
    bool isArgPlusZeroOrTrivialRValue() {
      if (!ArgValue.isRValue())
        return false;
      return ArgValue.peekRValue().peekIsPlusZeroRValueOrTrivial();
    }

    /// If callsite has an argument that is a plus zero or trivial rvalue, emit
    /// a retain so that the argument is at PlusOne.
    void convertToPlusOneFromPlusZero(SILGenFunction &SGF) {
      assert(isArgPlusZeroOrTrivialRValue() && "Must have a plus zero or "
             "trivial rvalue as an argument.");
      SILValue ArgSILValue = ArgValue.peekRValue().peekScalarValue();
      SILType ArgTy = ArgSILValue->getType();

      // If we are trivial, there is no difference in between +1 and +0 since
      // a trivial object is not reference counted.
      if (ArgTy.isTrivial(SGF.SGM.M))
        return;

      // Grab the SILLocation and the new managed value.
      SILLocation ArgLoc = ArgValue.getKnownRValueLocation();
      ManagedValue ArgManagedValue;
      if (ArgSILValue->getType().isAddress()) {
        auto result = SGF.emitTemporaryAllocation(ArgLoc,
                                                  ArgSILValue->getType());
        SGF.B.createCopyAddr(ArgLoc, ArgSILValue, result,
                             IsNotTake, IsInitialization);
        ArgManagedValue = SGF.emitManagedBufferWithCleanup(result);
      } else {
        ArgManagedValue = SGF.emitManagedRetain(ArgLoc, ArgSILValue);
      }

      // Ok now we make our transformation. First set ArgValue to a used albeit
      // invalid, empty ArgumentSource.
      ArgValue = ArgumentSource();

      // Reassign ArgValue.
      RValue NewRValue = RValue(SGF, ArgLoc, ArgTy.getSwiftRValueType(),
                                 ArgManagedValue);
      ArgValue = ArgumentSource(ArgLoc, std::move(NewRValue));
    }
  };

  /// Once the Callee and CallSites have been prepared by SILGenApply,
  /// generate SIL for a fully-formed call.
  ///
  /// The lowered function type of the callee defines an abstraction pattern
  /// for evaluating argument values of tuple type directly into explosions of
  /// scalars where possible.
  ///
  /// If there are more call sites than the natural uncurry level, they are
  /// have to be applied recursively to each intermediate callee.
  ///
  /// Also inout formal access and parameter and result conventions are
  /// handled here, with some special logic required for calls with +0 self.
  class CallEmission {
    SILGenFunction &SGF;

    std::vector<CallSite> uncurriedSites;
    std::vector<CallSite> extraSites;
    Callee callee;
    FormalEvaluationScope initialWritebackScope;
    unsigned uncurries;
    bool applied;
    bool assumedPlusZeroSelf;

  public:
    /// Create an emission for a call of the given callee.
    CallEmission(SILGenFunction &SGF, Callee &&callee,
                 FormalEvaluationScope &&writebackScope,
                 bool assumedPlusZeroSelf = false)
        : SGF(SGF), callee(std::move(callee)),
          initialWritebackScope(std::move(writebackScope)),
          uncurries(callee.getNaturalUncurryLevel() + 1), applied(false),
          assumedPlusZeroSelf(assumedPlusZeroSelf) {}

    /// Add a level of function application by passing in its possibly
    /// unevaluated arguments and their formal type.
    void addCallSite(CallSite &&site) {
      assert(!applied && "already applied!");

      // Append to the main argument list if we have uncurry levels remaining.
      if (uncurries > 0) {
        --uncurries;
        uncurriedSites.push_back(std::move(site));
        return;
      }

      // Otherwise, apply these arguments to the result of the previous call.
      extraSites.push_back(std::move(site));
    }
    
    /// Add a level of function application by passing in its possibly
    /// unevaluated arguments and their formal type
    template<typename...T>
    void addCallSite(T &&...args) {
      addCallSite(CallSite{std::forward<T>(args)...});
    }

    /// If we assumed that self was being passed at +0 before we knew what the
    /// final uncurried level of the callee was, but given the final uncurried
    /// level of the callee, we are actually passing self at +1, add in a retain
    /// of self.
    void convertSelfToPlusOneFromPlusZero() {
      // Self is always the first callsite.
      if (!uncurriedSites[0].isArgPlusZeroOrTrivialRValue())
        return;

      // Insert an invalid ArgumentSource into uncurriedSites[0] so it is.
      uncurriedSites[0].convertToPlusOneFromPlusZero(SGF);
    }

    /// Is this a fully-applied enum element constructor call?
    bool isEnumElementConstructor() {
      return (callee.kind == Callee::Kind::EnumElement && uncurries == 0);
    }

    /// True if this is a completely unapplied super method call
    bool isPartiallyAppliedSuperMethod(unsigned uncurryLevel) {
      return (callee.kind == Callee::Kind::SuperMethod &&
              uncurryLevel == 0);
    }

    RValue apply(SGFContext C = SGFContext()) {
      initialWritebackScope.verify();

      assert(!applied && "already applied!");

      applied = true;

      // Get the callee value at the needed uncurry level, uncurrying as
      // much as possible. If the number of calls is less than the natural
      // uncurry level, the callee emission might create a curry thunk.
      unsigned uncurryLevel = callee.getNaturalUncurryLevel() - uncurries;

      // Emit the first level of call.
      CanFunctionType formalType;
      Optional<ForeignErrorConvention> foreignError;
      ImportAsMemberStatus foreignSelf;
      RValue result =
          applyFirstLevelCallee(formalType,
                                foreignError, foreignSelf, uncurryLevel, C);

      // End of the initial writeback scope.
      initialWritebackScope.verify();
      initialWritebackScope.pop();

      // Then handle the remaining call sites.
      result = applyRemainingCallSites(std::move(result), formalType,
                                       foreignSelf, foreignError, C);
      return result;
    }

    ~CallEmission() { assert(applied && "never applied!"); }

    // Movable, but not copyable.
    CallEmission(CallEmission &&e)
        : SGF(e.SGF), uncurriedSites(std::move(e.uncurriedSites)),
          extraSites(std::move(e.extraSites)), callee(std::move(e.callee)),
          initialWritebackScope(std::move(e.initialWritebackScope)),
          uncurries(e.uncurries), applied(e.applied),
          assumedPlusZeroSelf(e.assumedPlusZeroSelf) {
      e.applied = true;
    }

  private:
    CallEmission(const CallEmission &) = delete;
    CallEmission &operator=(const CallEmission &) = delete;

    void emitArgumentsForNormalApply(
        CanFunctionType &formalType, AbstractionPattern &origFormalType,
        CanSILFunctionType substFnType,
        Optional<ForeignErrorConvention> &foreignError,
        ImportAsMemberStatus &foreignSelf, ApplyOptions &initialOptions,
        SmallVectorImpl<ManagedValue> &uncurriedArgs,
        Optional<SILLocation> &uncurriedLoc, CanFunctionType &formalApplyType);

    RValue
    applySpecializedEmitter(CanFunctionType &formalType,
                            Optional<ForeignErrorConvention> &foreignError,
                            ImportAsMemberStatus &foreignSelf,
                            SpecializedEmitter &specializedEmitter,
                            unsigned uncurryLevel, SGFContext C);

    RValue applyPartiallyAppliedSuperMethod(
        CanFunctionType &formalType,
        Optional<ForeignErrorConvention> &foreignError,
        ImportAsMemberStatus &foreignSelf, unsigned uncurryLevel, SGFContext C);

    RValue
    applyEnumElementConstructor(CanFunctionType &formalType,
                                Optional<ForeignErrorConvention> &foreignError,
                                ImportAsMemberStatus &foreignSelf,
                                unsigned uncurryLevel, SGFContext C);

    RValue applyNormalCall(CanFunctionType &formalType,
                           Optional<ForeignErrorConvention> &foreignError,
                           ImportAsMemberStatus &foreignSelf,
                           unsigned uncurryLevel, SGFContext C);

    RValue applyFirstLevelCallee(CanFunctionType &formalType,
                                 Optional<ForeignErrorConvention> &foreignError,
                                 ImportAsMemberStatus &foreignSelf,
                                 unsigned uncurryLevel, SGFContext C);

    RValue
    applyRemainingCallSites(RValue &&result, CanFunctionType formalType,
                            ImportAsMemberStatus foreignSelf,
                            Optional<ForeignErrorConvention> foreignError,
                            SGFContext C);

    AbstractionPattern
    getUncurriedOrigFormalType(AbstractionPattern origFormalType) {
      for (unsigned i = 0, e = uncurriedSites.size(); i < e; ++i) {
        claimNextParamClause(origFormalType);
      }

      return origFormalType;
    }
  };
} // end anonymous namespace

RValue CallEmission::applyFirstLevelCallee(
    CanFunctionType &formalType,
    Optional<ForeignErrorConvention> &foreignError,
    ImportAsMemberStatus &foreignSelf, unsigned uncurryLevel, SGFContext C) {

  // Check for a specialized emitter.
  if (auto emitter = callee.getSpecializedEmitter(SGF.SGM, uncurryLevel)) {
    return applySpecializedEmitter(formalType,
                                   foreignError, foreignSelf,
                                   emitter.getValue(), uncurryLevel, C);
  }

  if (isPartiallyAppliedSuperMethod(uncurryLevel)) {
    return applyPartiallyAppliedSuperMethod(formalType, foreignError,
                                            foreignSelf, uncurryLevel, C);
  }

  if (isEnumElementConstructor()) {
    return applyEnumElementConstructor(formalType,
                                       foreignError, foreignSelf, uncurryLevel,
                                       C);
  }

  return applyNormalCall(formalType, foreignError,
                         foreignSelf, uncurryLevel, C);
}

RValue CallEmission::applyNormalCall(
    CanFunctionType &formalType,
    Optional<ForeignErrorConvention> &foreignError,
    ImportAsMemberStatus &foreignSelf, unsigned uncurryLevel, SGFContext C) {
  // We use the context emit-into initialization only for the
  // outermost call.
  SGFContext uncurriedContext = (extraSites.empty() ? C : SGFContext());
  ManagedValue mv;
  ApplyOptions initialOptions = ApplyOptions::None;

  formalType = callee.getSubstFormalType();
  auto origFormalType = callee.getOrigFormalType();

  CanSILFunctionType substFnType;

  // Get the callee type information.
  std::tie(mv, substFnType, foreignError, foreignSelf, initialOptions) =
      callee.getAtUncurryLevel(SGF, uncurryLevel);

  CalleeTypeInfo calleeTypeInfo(
      substFnType, getUncurriedOrigFormalType(origFormalType),
      uncurriedSites.back().getSubstResultType(), foreignError);
  ResultPlanPtr resultPlan = ResultPlanBuilder::computeResultPlan(
      SGF, calleeTypeInfo, uncurriedSites.back().Loc, uncurriedContext);
  ArgumentScope argScope(SGF, uncurriedSites.back().Loc);

  // Now that we know the substFnType, check if we assumed that we were
  // passing self at +0. If we did and self is not actually passed at +0,
  // retain Self.
  if (assumedPlusZeroSelf) {
    // If the final emitted function does not have a self param or it does
    // have a self param that is consumed, convert what we think is self
    // to
    // be plus zero.
    if (!substFnType->hasSelfParam() ||
        substFnType->getSelfParameter().isConsumed()) {
      convertSelfToPlusOneFromPlusZero();
    }
  }

  // Emit the arguments.
  SmallVector<ManagedValue, 4> uncurriedArgs;
  Optional<SILLocation> uncurriedLoc;
  CanFunctionType formalApplyType;
  emitArgumentsForNormalApply(formalType, origFormalType,
                              substFnType, foreignError, foreignSelf,
                              initialOptions, uncurriedArgs, uncurriedLoc,
                              formalApplyType);
  // Emit the uncurried call.
  return SGF.emitApply(std::move(resultPlan), std::move(argScope),
                       uncurriedLoc.getValue(), mv, callee.getSubstitutions(),
                       uncurriedArgs, calleeTypeInfo, initialOptions,
                       uncurriedContext);
}

RValue CallEmission::applyEnumElementConstructor(
    CanFunctionType &formalType,
    Optional<ForeignErrorConvention> &foreignError,
    ImportAsMemberStatus &foreignSelf, unsigned uncurryLevel, SGFContext C) {
  assert(!assumedPlusZeroSelf);
  SGFContext uncurriedContext = (extraSites.empty() ? C : SGFContext());

  // Get the callee type information.
  //
  // Enum payloads are always stored at the abstraction level of the
  // unsubstituted payload type. This means that unlike with specialized
  // emitters above, enum constructors use the AST-level abstraction
  // pattern, to ensure that function types in payloads are re-abstracted
  // correctly.
  formalType = callee.getSubstFormalType();
  auto origFormalType = callee.getOrigFormalType();
  auto substFnType = SGF.getSILFunctionType(origFormalType, formalType,
                                            uncurryLevel);

  // Now that we know the substFnType, check if we assumed that we were
  // passing self at +0. If we did and self is not actually passed at +0,
  // retain Self.
  if (assumedPlusZeroSelf) {
    // If the final emitted function does not have a self param or it does
    // have a self param that is consumed, convert what we think is self
    // to
    // be plus zero.
    if (!substFnType->hasSelfParam() ||
        substFnType->getSelfParameter().isConsumed()) {
      convertSelfToPlusOneFromPlusZero();
    }
  }

  // We have a fully-applied enum element constructor: open-code the
  // construction.
  EnumElementDecl *element = callee.getEnumElementDecl();

  SILLocation uncurriedLoc = uncurriedSites[0].Loc;

  CanType formalResultType = formalType.getResult();

  // Ignore metatype argument
  claimNextParamClause(origFormalType);
  claimNextParamClause(formalType);
  std::move(uncurriedSites[0]).forward().getAsSingleValue(SGF);

  // Get the payload argument.
  ArgumentSource payload;
  if (element->hasAssociatedValues()) {
    assert(uncurriedSites.size() == 2);
    formalResultType = formalType.getResult();
    claimNextParamClause(origFormalType);
    claimNextParamClause(formalType);
    payload = std::move(uncurriedSites[1]).forward();
  } else {
    assert(uncurriedSites.size() == 1);
  }

  assert(substFnType->getNumResults() == 1);
  ManagedValue resultMV = SGF.emitInjectEnum(
      uncurriedLoc, std::move(payload), SGF.getLoweredType(formalResultType),
      element, uncurriedContext);
  return RValue(SGF, uncurriedLoc, formalResultType, resultMV);
}

RValue CallEmission::applyPartiallyAppliedSuperMethod(
    CanFunctionType &formalType,
    Optional<ForeignErrorConvention> &foreignError,
    ImportAsMemberStatus &foreignSelf, unsigned uncurryLevel, SGFContext C) {

  ApplyOptions initialOptions = ApplyOptions::None;

  // We want to emit the arguments as fully-substituted values
  // because that's what the partially applied super method expects;
  formalType = callee.getSubstFormalType();
  auto origFormalType = AbstractionPattern(formalType);
  auto substFnType = SGF.getSILFunctionType(origFormalType, formalType,
                                            uncurryLevel);

  // Now that we know the substFnType, check if we assumed that we were
  // passing self at +0. If we did and self is not actually passed at +0,
  // retain Self.
  if (assumedPlusZeroSelf) {
    // If the final emitted function does not have a self param or it does
    // have a self param that is consumed, convert what we think is self
    // to
    // be plus zero.
    if (!substFnType->hasSelfParam() ||
        substFnType->getSelfParameter().isConsumed()) {
      convertSelfToPlusOneFromPlusZero();
    }
  }

  // Emit the arguments.
  SmallVector<ManagedValue, 4> uncurriedArgs;
  Optional<SILLocation> uncurriedLoc;
  CanFunctionType formalApplyType;
  emitArgumentsForNormalApply(formalType, origFormalType,
                              substFnType, foreignError, foreignSelf,
                              initialOptions, uncurriedArgs, uncurriedLoc,
                              formalApplyType);

  // Emit the uncurried call.
  assert(uncurriedArgs.size() == 1 && "Can only partially apply the "
                                      "self parameter of a super "
                                      "method call");

  auto constant = callee.getMethodName();
  auto loc = uncurriedLoc.getValue();
  auto subs = callee.getSubstitutions();
  auto upcastedSelf = uncurriedArgs.back();
  auto upcastedSelfValue = upcastedSelf.getValue();
  auto self = getOriginalSelfValue(upcastedSelfValue);
  auto constantInfo = SGF.getConstantInfo(callee.getMethodName());
  auto functionTy = constantInfo.getSILType();
  SILValue superMethodVal =
      SGF.B.createSuperMethod(loc, self, constant, functionTy,
                              /*volatile*/
                              constant.isForeign);

  auto closureTy = SILGenBuilder::getPartialApplyResultType(
      constantInfo.getSILType(), 1, SGF.B.getModule(), subs,
      ParameterConvention::Direct_Owned);

  auto &module = SGF.getFunction().getModule();

  auto partialApplyTy = functionTy;
  if (constantInfo.SILFnType->isPolymorphic() && !subs.empty())
    partialApplyTy = partialApplyTy.substGenericArgs(module, subs);

  SILValue partialApply =
      SGF.B.createPartialApply(loc, superMethodVal, partialApplyTy, subs,
                               {upcastedSelf.forward(SGF)}, closureTy);
  return RValue(SGF, loc, formalApplyType.getResult(),
                ManagedValue::forUnmanaged(partialApply));
}

RValue CallEmission::applySpecializedEmitter(
    CanFunctionType &formalType,
    Optional<ForeignErrorConvention> &foreignError,
    ImportAsMemberStatus &foreignSelf, SpecializedEmitter &specializedEmitter,
    unsigned uncurryLevel, SGFContext C) {

  // We use the context emit-into initialization only for the
  // outermost call.
  SGFContext uncurriedContext = (extraSites.empty() ? C : SGFContext());

  ManagedValue mv;
  ApplyOptions initialOptions = ApplyOptions::None;

  // Get the callee type information. We want to emit the arguments as
  // fully-substituted values because that's what the specialized emitters
  // expect.
  formalType = callee.getSubstFormalType();
  auto origFormalType = AbstractionPattern(formalType);
  auto substFnType = SGF.getSILFunctionType(origFormalType, formalType,
                                            uncurryLevel);

  // Now that we know the substFnType, check if we assumed that we were
  // passing self at +0. If we did and self is not actually passed at +0,
  // retain Self.
  if (assumedPlusZeroSelf) {
    // If the final emitted function does not have a self param or it does
    // have a self param that is consumed, convert what we think is self to
    // be plus zero.
    if (!substFnType->hasSelfParam() ||
        substFnType->getSelfParameter().isConsumed()) {
      convertSelfToPlusOneFromPlusZero();
    }
  }

  // If we have an early emitter, just let it take over for the
  // uncurried call site.
  if (specializedEmitter.isEarlyEmitter()) {
    auto emitter = specializedEmitter.getEarlyEmitter();

    assert(uncurriedSites.size() == 1);
    CanFunctionType formalApplyType = cast<FunctionType>(formalType);
    assert(!formalApplyType->getExtInfo().throws());
    CanType formalResultType = formalApplyType.getResult();
    SILLocation uncurriedLoc = uncurriedSites[0].Loc;
    claimNextParamClause(origFormalType);
    claimNextParamClause(formalType);

    // We should be able to enforce that these arguments are
    // always still expressions.
    Expr *argument = std::move(uncurriedSites[0]).forward().asKnownExpr();
    ManagedValue resultMV =
        emitter(SGF, uncurriedLoc, callee.getSubstitutions(), argument,
                uncurriedContext);
    return RValue(SGF, uncurriedLoc, formalResultType, resultMV);
  }

  // Emit the arguments.
  SmallVector<ManagedValue, 4> uncurriedArgs;
  Optional<SILLocation> uncurriedLoc;
  CanFunctionType formalApplyType;
  emitArgumentsForNormalApply(formalType, origFormalType,
                              substFnType, foreignError, foreignSelf,
                              initialOptions, uncurriedArgs, uncurriedLoc,
                              formalApplyType);

  // Emit the uncurried call.
  if (specializedEmitter.isLateEmitter()) {
    auto emitter = specializedEmitter.getLateEmitter();
    return RValue(SGF, *uncurriedLoc, formalApplyType.getResult(),
                  emitter(SGF, uncurriedLoc.getValue(),
                          callee.getSubstitutions(), uncurriedArgs,
                          uncurriedContext));
  }

  // Builtins.
  assert(specializedEmitter.isNamedBuiltin());
  auto builtinName = specializedEmitter.getBuiltinName();
  SmallVector<SILValue, 4> consumedArgs;
  for (auto arg : uncurriedArgs) {
    consumedArgs.push_back(arg.forward(SGF));
  }
  SILFunctionConventions substConv(substFnType, SGF.SGM.M);
  auto resultVal = SGF.B.createBuiltin(uncurriedLoc.getValue(), builtinName,
                                       substConv.getSILResultType(),
                                       callee.getSubstitutions(), consumedArgs);
  return RValue(SGF, *uncurriedLoc, formalApplyType.getResult(),
                SGF.emitManagedRValueWithCleanup(resultVal));
}

void CallEmission::emitArgumentsForNormalApply(
    CanFunctionType &formalType, AbstractionPattern &origFormalType,
    CanSILFunctionType substFnType,
    Optional<ForeignErrorConvention> &foreignError,
    ImportAsMemberStatus &foreignSelf, ApplyOptions &initialOptions,
    SmallVectorImpl<ManagedValue> &uncurriedArgs,
    Optional<SILLocation> &uncurriedLoc, CanFunctionType &formalApplyType) {
  SmallVector<SmallVector<ManagedValue, 4>, 2> args;
  SmallVector<DelayedArgument, 2> delayedArgs;
  auto expectedUncurriedOrigFormalType =
      getUncurriedOrigFormalType(origFormalType);
  (void)expectedUncurriedOrigFormalType;

  args.reserve(uncurriedSites.size());
  {
    ParamLowering paramLowering(substFnType, SGF);

    assert(!foreignError || uncurriedSites.size() == 1 ||
           (uncurriedSites.size() == 2 && substFnType->hasSelfParam()));

    if (!uncurriedSites.back().throws()) {
      initialOptions |= ApplyOptions::DoesNotThrow;
    }

    // Collect the captures, if any.
    if (callee.hasCaptures()) {
      (void)paramLowering.claimCaptureParams(callee.getCaptures());
      args.push_back({});
      args.back().append(callee.getCaptures().begin(),
                         callee.getCaptures().end());
    }

    // Collect the arguments to the uncurried call.
    for (auto &site : uncurriedSites) {
      AbstractionPattern origParamType = claimNextParamClause(origFormalType);
      formalApplyType = cast<FunctionType>(formalType);
      claimNextParamClause(formalType);
      uncurriedLoc = site.Loc;
      args.push_back({});

      bool isParamSite = &site == &uncurriedSites.back();

      std::move(site).emit(SGF, origParamType, paramLowering, args.back(),
                           delayedArgs,
                           // Claim the foreign error with the method
                           // formal params.
                           isParamSite ? foreignError : None,
                           // Claim the foreign "self" with the self
                           // param.
                           isParamSite ? ImportAsMemberStatus() : foreignSelf);
    }
  }
  assert(uncurriedLoc);
  assert(formalApplyType);
  assert(origFormalType.getType() ==
             expectedUncurriedOrigFormalType.getType() &&
         "getUncurriedOrigFormalType and emitArgumentsForNormalCall are out of "
         "sync");

  // Emit any delayed arguments: formal accesses to inout arguments, etc.
  if (!delayedArgs.empty()) {
    emitDelayedArguments(SGF, delayedArgs, args);
  }

  // Uncurry the arguments in calling convention order.
  for (auto &argSet : reversed(args))
    uncurriedArgs.append(argSet.begin(), argSet.end());
  args = {};

  // Move the foreign "self" argument into position.
  if (foreignSelf.isInstance()) {
    auto selfArg = uncurriedArgs.back();
    std::move_backward(uncurriedArgs.begin() + foreignSelf.getSelfIndex(),
                       uncurriedArgs.end() - 1, uncurriedArgs.end());
    uncurriedArgs[foreignSelf.getSelfIndex()] = selfArg;
  }
}

RValue CallEmission::applyRemainingCallSites(
    RValue &&result, CanFunctionType formalType,
    ImportAsMemberStatus foreignSelf,
    Optional<ForeignErrorConvention> foreignError, SGFContext C) {
  // If there are remaining call sites, apply them to the result function.
  // Each chained call gets its own writeback scope.
  for (unsigned i = 0, size = extraSites.size(); i < size; ++i) {
    FormalEvaluationScope writebackScope(SGF);

    SILLocation loc = extraSites[i].Loc;

    auto functionMV = std::move(result).getAsSingleValue(SGF, loc);

    auto substFnType = functionMV.getType().castTo<SILFunctionType>();
    ParamLowering paramLowering(substFnType, SGF);

    SmallVector<ManagedValue, 4> siteArgs;
    SmallVector<DelayedArgument, 2> delayedArgs;

    // TODO: foreign errors for block or function pointer values?
    assert(substFnType->hasErrorResult() ||
           !cast<FunctionType>(formalType)->getExtInfo().throws());
    foreignError = None;

    // The result function has already been reabstracted to the substituted
    // type, so use the substituted formal type as the abstraction pattern
    // for argument passing now.
    AbstractionPattern origResultType(formalType.getResult());
    AbstractionPattern origParamType(claimNextParamClause(formalType));

    SGFContext context = i == size - 1 ? C : SGFContext();

    // Create the callee type info and initialize our indirect results.
    CalleeTypeInfo calleeTypeInfo(substFnType, origResultType,
                                  extraSites[i].getSubstResultType(),
                                  foreignError);
    ResultPlanPtr resultPtr =
        ResultPlanBuilder::computeResultPlan(SGF, calleeTypeInfo, loc, context);
    ArgumentScope argScope(SGF, loc);

    std::move(extraSites[i])
        .emit(SGF, origParamType, paramLowering, siteArgs, delayedArgs,
              foreignError, foreignSelf);
    if (!delayedArgs.empty()) {
      emitDelayedArguments(SGF, delayedArgs, siteArgs);
    }

    ApplyOptions options = ApplyOptions::None;

    result = SGF.emitApply(std::move(resultPtr), std::move(argScope), loc,
                           functionMV, {}, siteArgs, calleeTypeInfo, options,
                           context);
  }

  return std::move(result);
}

static CallEmission prepareApplyExpr(SILGenFunction &SGF, Expr *e) {
  // Set up writebacks for the call(s).
  FormalEvaluationScope writebacks(SGF);

  SILGenApply apply(SGF);

  // Decompose the call site.
  apply.decompose(e);

  // Evaluate and discard the side effect if present.
  if (apply.SideEffect)
    SGF.emitRValue(apply.SideEffect);

  // Build the call.
  // Pass the writeback scope on to CallEmission so it can thread scopes through
  // nested calls.
  CallEmission emission(SGF, apply.getCallee(), std::move(writebacks),
                        apply.AssumedPlusZeroSelf);

  // Apply 'self' if provided.
  if (apply.SelfParam) {
    emission.addCallSite(RegularLocation(e), std::move(apply.SelfParam),
                         apply.SelfType->getCanonicalType(), /*throws*/ false);
  }

  // Apply arguments from call sites, innermost to outermost.
  for (auto site = apply.CallSites.rbegin(), end = apply.CallSites.rend();
       site != end;
       ++site) {
    emission.addCallSite(*site);
  }

  return emission;
}

RValue SILGenFunction::emitApplyExpr(Expr *e, SGFContext c) {
  CallEmission emission = prepareApplyExpr(*this, e);
  return emission.apply(c);
}

RValue
SILGenFunction::emitApplyOfLibraryIntrinsic(SILLocation loc,
                                            FuncDecl *fn,
                                            const SubstitutionMap &subMap,
                                            ArrayRef<ManagedValue> args,
                                            SGFContext ctx) {
  SmallVector<Substitution, 4> subs;
  if (auto *genericSig = fn->getGenericSignature())
    genericSig->getSubstitutions(subMap, subs);

  auto callee = Callee::forDirect(*this, SILDeclRef(fn), subs, loc);

  auto origFormalType = callee.getOrigFormalType();
  auto substFormalType = callee.getSubstFormalType();

  ManagedValue mv;
  CanSILFunctionType substFnType;
  Optional<ForeignErrorConvention> foreignError;
  ImportAsMemberStatus foreignSelf;
  ApplyOptions options;
  std::tie(mv, substFnType, foreignError, foreignSelf, options)
    = callee.getAtUncurryLevel(*this, 0);

  assert(!foreignError);
  assert(!foreignSelf.isImportAsMember());
  assert(substFnType->getExtInfo().getLanguage()
           == SILFunctionLanguage::Swift);

  CalleeTypeInfo calleeTypeInfo(
      substFnType, origFormalType.getFunctionResultType(),
      substFormalType.getResult());
  ResultPlanPtr resultPlan =
      ResultPlanBuilder::computeResultPlan(*this, calleeTypeInfo, loc, ctx);
  ArgumentScope argScope(*this, loc);
  return emitApply(std::move(resultPlan), std::move(argScope), loc, mv, subs,
                   args, calleeTypeInfo, options, ctx);
}

static StringRef
getMagicFunctionString(SILGenFunction &SGF) {
  assert(SGF.MagicFunctionName
         && "asking for #function but we don't have a function name?!");
  if (SGF.MagicFunctionString.empty()) {
    llvm::raw_string_ostream os(SGF.MagicFunctionString);
    SGF.MagicFunctionName.printPretty(os);
  }
  return SGF.MagicFunctionString;
}

/// Emit an application of the given allocating initializer.
static RValue emitApplyAllocatingInitializer(SILGenFunction &SGF,
                                             SILLocation loc,
                                             ConcreteDeclRef init,
                                             RValue &&args,
                                             Type overriddenSelfType,
                                             SGFContext C) {
  ConstructorDecl *ctor = cast<ConstructorDecl>(init.getDecl());

  // Form the reference to the allocating initializer.
  auto initRef = SILDeclRef(ctor, SILDeclRef::Kind::Allocator)
    .asForeign(requiresForeignEntryPoint(ctor));
  auto initConstant = SGF.getConstantInfo(initRef);
  auto subs = init.getSubstitutions();

  // Scope any further writeback just within this operation.
  FormalEvaluationScope writebackScope(SGF);

  // Form the metatype argument.
  ManagedValue selfMetaVal;
  SILType selfMetaTy;
  {
    // Determine the self metatype type.
    CanSILFunctionType substFnType =
      initConstant.SILFnType->substGenericArgs(SGF.SGM.M, subs);
    SILType selfParamMetaTy = SGF.getSILType(substFnType->getSelfParameter());

    if (overriddenSelfType) {
      // If the 'self' type has been overridden, form a metatype to the
      // overriding 'Self' type.
      Type overriddenSelfMetaType =
        MetatypeType::get(overriddenSelfType,
                          selfParamMetaTy.castTo<MetatypeType>()
                              ->getRepresentation());
      selfMetaTy =
        SGF.getLoweredType(overriddenSelfMetaType->getCanonicalType());
    } else {
      selfMetaTy = selfParamMetaTy;
    }

    // Form the metatype value.
    SILValue selfMeta = SGF.B.createMetatype(loc, selfMetaTy);

    // If the types differ, we need an upcast.
    if (selfMetaTy != selfParamMetaTy)
      selfMeta = SGF.B.createUpcast(loc, selfMeta, selfParamMetaTy);

    selfMetaVal = ManagedValue::forUnmanaged(selfMeta);
  }

  // Form the callee.
  Optional<Callee> callee;
  if (isa<ProtocolDecl>(ctor->getDeclContext())) {
    callee.emplace(Callee::forArchetype(SGF,
                                        selfMetaVal.getType().getSwiftRValueType(),
                                        initRef, subs, loc));
  } else {
    callee.emplace(Callee::forDirect(SGF, initRef, subs, loc));
  }

  auto substFormalType = callee->getSubstFormalType();

  // For an inheritable initializer, determine whether we'll need to adjust the
  // result type.
  bool requiresDowncast = false;
  if (ctor->isInheritable() && overriddenSelfType) {
    CanType substResultType = substFormalType;
    for (unsigned i : range(ctor->getNumParameterLists())) {
      (void)i;
      substResultType = cast<FunctionType>(substResultType).getResult();
    }

    if (!substResultType->isEqual(overriddenSelfType))
      requiresDowncast = true;
  }

  // Form the call emission.
  CallEmission emission(SGF, std::move(*callee), std::move(writebackScope));

  // Self metatype.
  emission.addCallSite(loc,
                       ArgumentSource(loc,
                                      RValue(SGF, loc,
                                             selfMetaVal.getType()
                                               .getSwiftRValueType(),
                                             std::move(selfMetaVal))),
                       substFormalType);

  // Arguments
  emission.addCallSite(loc, ArgumentSource(loc, std::move(args)),
                       cast<FunctionType>(substFormalType.getResult()));

  // Perform the call.
  RValue result = emission.apply(requiresDowncast ? SGFContext() : C);

  // If we need a downcast, do it down.
  if (requiresDowncast) {
    ManagedValue v = std::move(result).getAsSingleValue(SGF, loc);
    CanType canOverriddenSelfType = overriddenSelfType->getCanonicalType();
    SILType loweredResultTy = SGF.getLoweredType(canOverriddenSelfType);
    v = ManagedValue(SGF.B.createUncheckedRefCast(loc,
                                                  v.getValue(),
                                                  loweredResultTy),
                     v.getCleanup());
    result = RValue(SGF, loc, canOverriddenSelfType, v);
  }

  return result;
}

/// Emit a literal that applies the various initializers.
RValue SILGenFunction::emitLiteral(LiteralExpr *literal, SGFContext C) {
  ConcreteDeclRef builtinInit;
  ConcreteDeclRef init;
  // Emit the raw, builtin literal arguments.
  RValue builtinLiteralArgs;
  if (auto stringLiteral = dyn_cast<StringLiteralExpr>(literal)) {
    builtinLiteralArgs = emitStringLiteral(*this, literal,
                                           stringLiteral->getValue(), C,
                                           stringLiteral->getEncoding());
    builtinInit = stringLiteral->getBuiltinInitializer();
    init = stringLiteral->getInitializer();
  } else {
    ASTContext &ctx = getASTContext();
    SourceLoc loc;
  
    // If "overrideLocationForMagicIdentifiers" is set, then we use it as the
    // location point for these magic identifiers.
    if (overrideLocationForMagicIdentifiers)
      loc = overrideLocationForMagicIdentifiers.getValue();
    else
      loc = literal->getStartLoc();

    auto magicLiteral = cast<MagicIdentifierLiteralExpr>(literal);
    switch (magicLiteral->getKind()) {
    case MagicIdentifierLiteralExpr::File: {
      StringRef value = "";
      if (loc.isValid())
        value = ctx.SourceMgr.getBufferIdentifierForLoc(loc);
      builtinLiteralArgs = emitStringLiteral(*this, literal, value, C,
                                             magicLiteral->getStringEncoding());
      builtinInit = magicLiteral->getBuiltinInitializer();
      init = magicLiteral->getInitializer();
      break;
    }

    case MagicIdentifierLiteralExpr::Function: {
      StringRef value = "";
      if (loc.isValid())
        value = getMagicFunctionString(*this);
      builtinLiteralArgs = emitStringLiteral(*this, literal, value, C,
                                             magicLiteral->getStringEncoding());
      builtinInit = magicLiteral->getBuiltinInitializer();
      init = magicLiteral->getInitializer();
      break;
    }

    case MagicIdentifierLiteralExpr::Line:
    case MagicIdentifierLiteralExpr::Column:
    case MagicIdentifierLiteralExpr::DSOHandle:
      llvm_unreachable("handled elsewhere");
    }
  }

  // Helper routine to add an argument label if we need one.
  auto relabelArgument = [&](ConcreteDeclRef callee, RValue &arg) {
    auto name = callee.getDecl()->getFullName();
    auto argLabels = name.getArgumentNames();
    if (argLabels.size() == 1 && !argLabels[0].empty() &&
        !isa<TupleType>(arg.getType())) {
      Type newType = TupleType::get({TupleTypeElt(arg.getType(), argLabels[0])},
                                    getASTContext());
      arg.rewriteType(newType->getCanonicalType());
    }
  };

  // Call the builtin initializer.
  relabelArgument(builtinInit, builtinLiteralArgs);
  RValue builtinLiteral =
    emitApplyAllocatingInitializer(*this, literal, builtinInit,
                                   std::move(builtinLiteralArgs),
                                   Type(),
                                   init ? SGFContext() : C);

  // If we were able to directly initialize the literal we wanted, we're done.
  if (!init) return builtinLiteral;

  // Otherwise, perform the second initialization step.
  relabelArgument(init, builtinLiteral);
  RValue result = emitApplyAllocatingInitializer(*this, literal, init,
                                                 std::move(builtinLiteral),
                                                 literal->getType(), C);
  return result;
}

/// Allocate an uninitialized array of a given size, returning the array
/// and a pointer to its uninitialized contents, which must be initialized
/// before the array is valid.
std::pair<ManagedValue, SILValue>
SILGenFunction::emitUninitializedArrayAllocation(Type ArrayTy,
                                                 SILValue Length,
                                                 SILLocation Loc) {
  auto &Ctx = getASTContext();
  auto allocate = Ctx.getAllocateUninitializedArray(nullptr);

  // Invoke the intrinsic, which returns a tuple.
  auto subMap = ArrayTy->getContextSubstitutionMap(SGM.M.getSwiftModule(),
                                                   Ctx.getArrayDecl());
  auto result = emitApplyOfLibraryIntrinsic(Loc, allocate,
                                            subMap,
                                            ManagedValue::forUnmanaged(Length),
                                            SGFContext());

  // Explode the tuple.
  SmallVector<ManagedValue, 2> resultElts;
  std::move(result).getAll(resultElts);

  return {resultElts[0], resultElts[1].getUnmanagedValue()};
}

/// Deallocate an uninitialized array.
void SILGenFunction::emitUninitializedArrayDeallocation(SILLocation loc,
                                                        SILValue array) {
  auto &Ctx = getASTContext();
  auto deallocate = Ctx.getDeallocateUninitializedArray(nullptr);

  CanType arrayTy = array->getType().getSwiftRValueType();

  // Invoke the intrinsic.
  auto subMap = arrayTy->getContextSubstitutionMap(SGM.M.getSwiftModule(),
                                                   Ctx.getArrayDecl());
  emitApplyOfLibraryIntrinsic(loc, deallocate, subMap,
                              ManagedValue::forUnmanaged(array),
                              SGFContext());
}

namespace {
  /// A cleanup that deallocates an uninitialized array.
  class DeallocateUninitializedArray: public Cleanup {
    SILValue Array;
  public:
    DeallocateUninitializedArray(SILValue array)
      : Array(array) {}

    void emit(SILGenFunction &SGF, CleanupLocation l) override {
      SGF.emitUninitializedArrayDeallocation(l, Array);
    }

    void dump(SILGenFunction &SGF) const override {
#ifndef NDEBUG
      llvm::errs() << "DeallocateUninitializedArray "
                   << "State:" << getState() << " "
                   << "Array:" << Array << "\n";
#endif
    }
  };
} // end anonymous namespace

CleanupHandle
SILGenFunction::enterDeallocateUninitializedArrayCleanup(SILValue array) {
  Cleanups.pushCleanup<DeallocateUninitializedArray>(array);
  return Cleanups.getTopCleanup();
}

static Callee getBaseAccessorFunctionRef(SILGenFunction &SGF,
                                         SILLocation loc,
                                         SILDeclRef constant,
                                         ArgumentSource &selfValue,
                                         bool isSuper,
                                         bool isDirectUse,
                                         SubstitutionList subs) {
  auto *decl = cast<AbstractFunctionDecl>(constant.getDecl());

  // The accessor might be a local function that does not capture any
  // generic parameters, in which case we don't want to pass in any
  // substitutions.
  auto captureInfo = SGF.SGM.Types.getLoweredLocalCaptures(decl);
  if (decl->getDeclContext()->isLocalContext() &&
      !captureInfo.hasGenericParamCaptures()) {
    subs = SubstitutionList();
  }

  // If this is a method in a protocol, generate it as a protocol call.
  if (isa<ProtocolDecl>(decl->getDeclContext())) {
    assert(!isDirectUse && "direct use of protocol accessor?");
    assert(!isSuper && "super call to protocol method?");

    return Callee::forArchetype(SGF,
                                selfValue.getSubstRValueType(),
                                constant, subs, loc);
  }

  bool isClassDispatch = false;
  if (!isDirectUse) {
    switch (getMethodDispatch(decl)) {
    case MethodDispatch::Class:
      isClassDispatch = true;
      break;
    case MethodDispatch::Static:
      isClassDispatch = false;
      break;
    }
  }

  // Dispatch in a struct/enum or to a final method is always direct.
  if (!isClassDispatch || decl->isFinal())
    return Callee::forDirect(SGF, constant, subs, loc);

  // Otherwise, if we have a non-final class dispatch to a normal method,
  // perform a dynamic dispatch.
  auto self = selfValue.forceAndPeekRValue(SGF).peekScalarValue();
  if (!isSuper)
    return Callee::forClassMethod(SGF, self, constant, subs, loc);

  // If this is a "super." dispatch, we do a dynamic dispatch for objc methods
  // or non-final native Swift methods.
  if (!canUseStaticDispatch(SGF, constant))
    return Callee::forSuperMethod(SGF, self, constant, subs, loc);

  return Callee::forDirect(SGF, constant, subs, loc);
}

static Callee
emitSpecializedAccessorFunctionRef(SILGenFunction &SGF,
                                   SILLocation loc,
                                   SILDeclRef constant,
                                   SubstitutionList substitutions,
                                   ArgumentSource &selfValue,
                                   bool isSuper,
                                   bool isDirectUse)
{
  // Get the accessor function. The type will be a polymorphic function if
  // the Self type is generic.
  Callee callee = getBaseAccessorFunctionRef(SGF, loc, constant, selfValue,
                                             isSuper, isDirectUse,
                                             substitutions);
  
  // Collect captures if the accessor has them.
  auto accessorFn = cast<AbstractFunctionDecl>(constant.getDecl());
  if (SGF.SGM.M.Types.hasLoweredLocalCaptures(accessorFn)) {
    assert(!selfValue && "local property has self param?!");
    SmallVector<ManagedValue, 4> captures;
    SGF.emitCaptures(loc, accessorFn, CaptureEmission::ImmediateApplication,
                     captures);
    callee.setCaptures(std::move(captures));
  }

  return callee;
}

namespace {

/// A builder class that creates the base argument for accessors.
///
/// *NOTE* All cleanups created inside of this builder on base arguments must be
/// formal access to ensure that we do not extend the lifetime of a guaranteed
/// base after the accessor is evaluated.
struct AccessorBaseArgPreparer final {
  SILGenFunction &SGF;
  SILLocation loc;
  ManagedValue base;
  CanType baseFormalType;
  SILDeclRef accessor;
  SILParameterInfo selfParam;
  SILType baseLoweredType;

  AccessorBaseArgPreparer(SILGenFunction &SGF, SILLocation loc,
                          ManagedValue base, CanType baseFormalType,
                          SILDeclRef accessor);
  ArgumentSource prepare();

private:
  /// Prepare our base if we have an address base.
  ArgumentSource prepareAccessorAddressBaseArg();
  /// Prepare our base if we have an object base.
  ArgumentSource prepareAccessorObjectBaseArg();

  /// Returns true if given an address base, we need to load the underlying
  /// address. Asserts if baseLoweredType is not an address.
  bool shouldLoadBaseAddress() const;
};

} // end anonymous namespace

bool AccessorBaseArgPreparer::shouldLoadBaseAddress() const {
  assert(baseLoweredType.isAddress() &&
         "Should only call this helper method if the base is an address");
  switch (selfParam.getConvention()) {
  // If the accessor wants the value 'inout', always pass the
  // address we were given.  This is semantically required.
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
    return false;

  // If the accessor wants the value 'in', we have to copy if the
  // base isn't a temporary.  We aren't allowed to pass aliased
  // memory to 'in', and we have pass at +1.
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Indirect_In_Constant:
  case ParameterConvention::Indirect_In_Guaranteed:
    // TODO: We shouldn't be able to get an lvalue here, but the AST
    // sometimes produces an inout base for non-mutating accessors.
    // rdar://problem/19782170
    // assert(!base.isLValue());
    return base.isLValue() || base.isPlusZeroRValueOrTrivial();

  // If the accessor wants the value directly, we definitely have to
  // load.
  case ParameterConvention::Direct_Owned:
  case ParameterConvention::Direct_Unowned:
  case ParameterConvention::Direct_Guaranteed:
    return true;
  }
  llvm_unreachable("bad convention");
}

ArgumentSource AccessorBaseArgPreparer::prepareAccessorAddressBaseArg() {
  // If the base is currently an address, we may have to copy it.
  if (shouldLoadBaseAddress()) {
    if (selfParam.isConsumed() ||
        base.getType().isAddressOnly(SGF.getModule())) {
      // The load can only be a take if the base is a +1 rvalue.
      auto shouldTake = IsTake_t(base.hasCleanup());

      base = SGF.emitFormalAccessLoad(loc, base.forward(SGF),
                                      SGF.getTypeLowering(baseLoweredType),
                                      SGFContext(), shouldTake);
      return ArgumentSource(loc, RValue(SGF, loc, baseFormalType, base));
    }

    // If we do not have a consumed base and need to perform a load, perform a
    // formal access load borrow.
    base = SGF.B.createFormalAccessLoadBorrow(loc, base);
    return ArgumentSource(loc, RValue(SGF, loc, baseFormalType, base));
  }

  // Handle inout bases specially here.
  if (selfParam.isIndirectInOut()) {
    // It sometimes happens that we get r-value bases here, e.g. when calling a
    // mutating setter on a materialized temporary.  Just don't claim the value.
    if (!base.isLValue()) {
      base = ManagedValue::forLValue(base.getValue());
    }

    // FIXME: this assumes that there's never meaningful reabstraction of self
    // arguments.
    return ArgumentSource(
        loc, LValue::forAddress(base, None, AbstractionPattern(baseFormalType),
                                baseFormalType));
  }

  // Otherwise, we have a value that we can forward without any additional
  // handling.
  return ArgumentSource(loc, RValue(SGF, loc, baseFormalType, base));
}

ArgumentSource AccessorBaseArgPreparer::prepareAccessorObjectBaseArg() {
  // If the base is currently scalar, we may have to drop it in
  // memory or copy it.
  assert(!base.isLValue());

  // We need to produce the value at +1 if it's going to be consumed.
  if (selfParam.isConsumed() && !base.hasCleanup()) {
    base = base.formalAccessCopyUnmanaged(SGF, loc);
  }

  // If the parameter is indirect, we need to drop the value into
  // temporary memory.
  if (SGF.silConv.isSILIndirect(selfParam)) {
    // It's usually a really bad idea to materialize when we're
    // about to pass a value to an inout argument, because it's a
    // really easy way to silently drop modifications (e.g. from a
    // mutating getter in a writeback pair).  Our caller should
    // always take responsibility for that decision (by doing the
    // materialization itself).
    //
    // However, when the base is a reference type and the target is
    // a non-class protocol, this is innocuous.
#ifndef NDEBUG
    auto isNonClassProtocolMember = [](Decl *d) {
      auto p = d->getDeclContext()->getAsProtocolOrProtocolExtensionContext();
      return (p && !p->requiresClass());
    };
#endif
    assert((!selfParam.isIndirectMutating() ||
            (baseFormalType->isAnyClassReferenceType() &&
             isNonClassProtocolMember(accessor.getDecl()))) &&
           "passing unmaterialized r-value as inout argument");

    base = base.materialize(SGF, loc);
    if (selfParam.isIndirectInOut()) {
      // Drop the cleanup if we have one.
      auto baseLV = ManagedValue::forLValue(base.getValue());
      return ArgumentSource(
          loc, LValue::forAddress(baseLV, None,
                                  AbstractionPattern(baseFormalType),
                                  baseFormalType));
    }
  }

  return ArgumentSource(loc, RValue(SGF, loc, baseFormalType, base));
}

AccessorBaseArgPreparer::AccessorBaseArgPreparer(SILGenFunction &SGF,
                                                 SILLocation loc,
                                                 ManagedValue base,
                                                 CanType baseFormalType,
                                                 SILDeclRef accessor)
    : SGF(SGF), loc(loc), base(base), baseFormalType(baseFormalType),
      accessor(accessor),
      selfParam(SGF.SGM.Types.getConstantSelfParameter(accessor)),
      baseLoweredType(base.getType()) {
  assert(!base.isInContext());
  assert(!base.isLValue() || !base.hasCleanup());
}

ArgumentSource AccessorBaseArgPreparer::prepare() {
  // If the base is a boxed existential, we will open it later.
  if (baseLoweredType.getPreferredExistentialRepresentation(SGF.SGM.M) ==
      ExistentialRepresentation::Boxed) {
    assert(!baseLoweredType.isAddress() &&
           "boxed existential should not be an address");
    return ArgumentSource(loc, RValue(SGF, loc, baseFormalType, base));
  }

  if (baseLoweredType.isAddress())
    return prepareAccessorAddressBaseArg();

  // At this point, we know we have an object.
  assert(baseLoweredType.isObject());
  return prepareAccessorObjectBaseArg();
}

ArgumentSource SILGenFunction::prepareAccessorBaseArg(SILLocation loc,
                                                      ManagedValue base,
                                                      CanType baseFormalType,
                                                      SILDeclRef accessor) {
  AccessorBaseArgPreparer Preparer(*this, loc, base, baseFormalType, accessor);
  return Preparer.prepare();
}

static bool shouldReferenceForeignAccessor(AbstractStorageDecl *storage,
                                           bool isDirectUse) {
  // C functions imported as members should be referenced as C functions.
  if (storage->getGetter()->isImportAsMember())
    return true;
  
  // Otherwise, favor native entry points for direct accesses.
  if (isDirectUse)
    return false;
  
  return storage->requiresForeignGetterAndSetter();
}

SILDeclRef SILGenFunction::getGetterDeclRef(AbstractStorageDecl *storage,
                                            bool isDirectUse) {
  // Use the ObjC entry point
  return SILDeclRef(storage->getGetter(), SILDeclRef::Kind::Func)
    .asForeign(shouldReferenceForeignAccessor(storage, isDirectUse));
}

/// Emit a call to a getter.
RValue SILGenFunction::
emitGetAccessor(SILLocation loc, SILDeclRef get,
                SubstitutionList substitutions,
                ArgumentSource &&selfValue,
                bool isSuper, bool isDirectUse,
                RValue &&subscripts, SGFContext c) {
  // Scope any further writeback just within this operation.
  FormalEvaluationScope writebackScope(*this);

  Callee getter = emitSpecializedAccessorFunctionRef(*this, loc, get,
                                                     substitutions, selfValue,
                                                     isSuper, isDirectUse);
  bool hasSelf = (bool)selfValue;
  CanAnyFunctionType accessType = getter.getSubstFormalType();

  CallEmission emission(*this, std::move(getter), std::move(writebackScope));
  // Self ->
  if (hasSelf) {
    emission.addCallSite(loc, std::move(selfValue), accessType);
    accessType = cast<AnyFunctionType>(accessType.getResult());
  }
  // Index or () if none.
  if (!subscripts)
    subscripts = emitEmptyTupleRValue(loc, SGFContext());

  emission.addCallSite(loc, ArgumentSource(loc, std::move(subscripts)),
                       accessType);

  // T
  return emission.apply(c);
}

SILDeclRef SILGenFunction::getSetterDeclRef(AbstractStorageDecl *storage,
                                            bool isDirectUse) {
  return SILDeclRef(storage->getSetter(), SILDeclRef::Kind::Func)
    .asForeign(shouldReferenceForeignAccessor(storage, isDirectUse));
}

void SILGenFunction::emitSetAccessor(SILLocation loc, SILDeclRef set,
                                     SubstitutionList substitutions,
                                     ArgumentSource &&selfValue,
                                     bool isSuper, bool isDirectUse,
                                     RValue &&subscripts, RValue &&setValue) {
  // Scope any further writeback just within this operation.
  FormalEvaluationScope writebackScope(*this);

  Callee setter = emitSpecializedAccessorFunctionRef(*this, loc, set,
                                                     substitutions, selfValue,
                                                     isSuper, isDirectUse);
  bool hasSelf = (bool)selfValue;
  CanAnyFunctionType accessType = setter.getSubstFormalType();

  CallEmission emission(*this, std::move(setter), std::move(writebackScope));
  // Self ->
  if (hasSelf) {
    emission.addCallSite(loc, std::move(selfValue), accessType);
    accessType = cast<AnyFunctionType>(accessType.getResult());
  }

  // (value)  or (value, indices)
  if (subscripts) {
    // If we have a value and index list, create a new rvalue to represent the
    // both of them together.  The value goes first.
    SmallVector<ManagedValue, 4> Elts;
    std::move(setValue).getAll(Elts);
    std::move(subscripts).getAll(Elts);
    setValue = RValue::withPreExplodedElements(Elts, accessType.getInput());
  } else {
    setValue.rewriteType(accessType.getInput());
  }
  emission.addCallSite(loc, ArgumentSource(loc, std::move(setValue)),
                       accessType);
  // ()
  emission.apply();
}

SILDeclRef
SILGenFunction::getMaterializeForSetDeclRef(AbstractStorageDecl *storage,
                                            bool isDirectUse) {
  return SILDeclRef(storage->getMaterializeForSetFunc(),
                    SILDeclRef::Kind::Func);
}

MaterializedLValue SILGenFunction::
emitMaterializeForSetAccessor(SILLocation loc, SILDeclRef materializeForSet,
                              SubstitutionList substitutions,
                              ArgumentSource &&selfValue,
                              bool isSuper, bool isDirectUse,
                              RValue &&subscripts, SILValue buffer,
                              SILValue callbackStorage) {
  // Scope any further writeback just within this operation.
  FormalEvaluationScope writebackScope(*this);

  Callee callee = emitSpecializedAccessorFunctionRef(*this, loc,
                                                     materializeForSet,
                                                     substitutions, selfValue,
                                                     isSuper, isDirectUse);
  bool hasSelf = (bool)selfValue;
  auto accessType = callee.getSubstFormalType();

  CallEmission emission(*this, std::move(callee), std::move(writebackScope));
  // Self ->
  if (hasSelf) {
    emission.addCallSite(loc, std::move(selfValue), accessType);
    accessType = cast<FunctionType>(accessType.getResult());
  }

  // (buffer, callbackStorage)  or (buffer, callbackStorage, indices) ->
  // Note that this "RValue" stores a mixed LValue/RValue tuple.
  RValue args = [&] {
    SmallVector<ManagedValue, 4> elts;

    auto bufferPtr =
      B.createAddressToPointer(loc, buffer,
                               SILType::getRawPointerType(getASTContext()));
    elts.push_back(ManagedValue::forUnmanaged(bufferPtr));

    elts.push_back(ManagedValue::forLValue(callbackStorage));

    if (subscripts) {
      std::move(subscripts).getAll(elts);
    }
    return RValue::withPreExplodedElements(elts, accessType.getInput());
  }();
  emission.addCallSite(loc, ArgumentSource(loc, std::move(args)), accessType);
  // (buffer, optionalCallback)
  SmallVector<ManagedValue, 2> results;
  emission.apply().getAll(results);

  // Project out the materialized address. The address directly returned by
  // materialize for set is strictly typed, whether it is the local buffer or
  // stored property.
  SILValue address = results[0].getUnmanagedValue();
  address = B.createPointerToAddress(loc, address, buffer->getType(),
                                     /*isStrict*/ true,
                                     /*isInvariant*/ false);

  // Project out the optional callback.
  SILValue optionalCallback = results[1].getUnmanagedValue();

  auto origAccessType = SGM.Types.getConstantInfo(materializeForSet)
      .FormalInterfaceType;

  auto origSelfType = origAccessType->getInput()
      ->getInOutObjectType()
      ->getCanonicalType();

  CanGenericSignature genericSig;
  if (auto genericFnType = dyn_cast<GenericFunctionType>(origAccessType))
    genericSig = genericFnType.getGenericSignature();

  return MaterializedLValue(ManagedValue::forUnmanaged(address),
                            origSelfType, genericSig,
                            optionalCallback, callbackStorage);
}

SILDeclRef SILGenFunction::getAddressorDeclRef(AbstractStorageDecl *storage,
                                               AccessKind accessKind,
                                               bool isDirectUse) {
  FuncDecl *addressorFunc = storage->getAddressorForAccess(accessKind);
  return SILDeclRef(addressorFunc, SILDeclRef::Kind::Func);
}

/// Emit a call to an addressor.
///
/// The first return value is the address, which will always be an
/// l-value managed value.  The second return value is the owner
/// pointer, if applicable.
std::pair<ManagedValue, ManagedValue> SILGenFunction::
emitAddressorAccessor(SILLocation loc, SILDeclRef addressor,
                      SubstitutionList substitutions,
                      ArgumentSource &&selfValue,
                      bool isSuper, bool isDirectUse,
                      RValue &&subscripts, SILType addressType) {
  // Scope any further writeback just within this operation.
  FormalEvaluationScope writebackScope(*this);

  Callee callee =
    emitSpecializedAccessorFunctionRef(*this, loc, addressor,
                                       substitutions, selfValue,
                                       isSuper, isDirectUse);
  bool hasSelf = (bool)selfValue;
  CanAnyFunctionType accessType = callee.getSubstFormalType();

  CallEmission emission(*this, std::move(callee), std::move(writebackScope));
  // Self ->
  if (hasSelf) {
    emission.addCallSite(loc, std::move(selfValue), accessType);
    accessType = cast<AnyFunctionType>(accessType.getResult());
  }
  // Index or () if none.
  if (!subscripts)
    subscripts = emitEmptyTupleRValue(loc, SGFContext());

  emission.addCallSite(loc, ArgumentSource(loc, std::move(subscripts)),
                       accessType);

  // Unsafe{Mutable}Pointer<T> or
  // (Unsafe{Mutable}Pointer<T>, Builtin.UnknownPointer) or
  // (Unsafe{Mutable}Pointer<T>, Builtin.NativePointer) or
  // (Unsafe{Mutable}Pointer<T>, Builtin.NativePointer?) or
  SmallVector<ManagedValue, 2> results;
  emission.apply().getAll(results);

  SILValue pointer;
  ManagedValue owner;
  switch (cast<FuncDecl>(addressor.getDecl())->getAddressorKind()) {
  case AddressorKind::NotAddressor:
    llvm_unreachable("not an addressor!");
  case AddressorKind::Unsafe:
    assert(results.size() == 1);
    pointer = results[0].getUnmanagedValue();
    owner = ManagedValue();
    break;
  case AddressorKind::Owning:
  case AddressorKind::NativeOwning:
  case AddressorKind::NativePinning:
    assert(results.size() == 2);
    pointer = results[0].getUnmanagedValue();
    owner = results[1];
    break;
  }

  // Drill down to the raw pointer using intrinsic knowledge of those types.
  auto pointerType =
    pointer->getType().castTo<BoundGenericStructType>()->getDecl();
  auto props = pointerType->getStoredProperties();
  assert(props.begin() != props.end());
  assert(std::next(props.begin()) == props.end());
  VarDecl *rawPointerField = *props.begin();
  pointer = B.createStructExtract(loc, pointer, rawPointerField,
                                  SILType::getRawPointerType(getASTContext()));

  // Convert to the appropriate address type and return.
  SILValue address = B.createPointerToAddress(loc, pointer, addressType,
                                              /*isStrict*/ true,
                                              /*isInvariant*/ false);

  // Mark dependence as necessary.
  switch (cast<FuncDecl>(addressor.getDecl())->getAddressorKind()) {
  case AddressorKind::NotAddressor:
    llvm_unreachable("not an addressor!");
  case AddressorKind::Unsafe:
    // TODO: we should probably mark dependence on the base.
    break;
  case AddressorKind::Owning:
  case AddressorKind::NativeOwning:
  case AddressorKind::NativePinning:
    address = B.createMarkDependence(loc, address, owner.getValue());
    break;
  }

  return { ManagedValue::forLValue(address), owner };
}


RValue SILGenFunction::emitApplyConversionFunction(SILLocation loc,
                                                   Expr *funcExpr,
                                                   Type resultType,
                                                   RValue &&operand) {
  // Walk the function expression, which should produce a reference to the
  // callee, leaving the final curry level unapplied.
  CallEmission emission = prepareApplyExpr(*this, funcExpr);
  // Rewrite the operand type to the expected argument type, to handle tuple
  // conversions etc.
  auto funcTy = cast<FunctionType>(funcExpr->getType()->getCanonicalType());
  operand.rewriteType(funcTy.getInput());
  // Add the operand as the final callsite.
  emission.addCallSite(loc, ArgumentSource(loc, std::move(operand)),
                       resultType->getCanonicalType(), funcTy->throws());
  return emission.apply();
}

// Create a partial application of a dynamic method, applying bridging thunks
// if necessary.
static SILValue emitDynamicPartialApply(SILGenFunction &SGF,
                                        SILLocation loc,
                                        SILValue method,
                                        SILValue self,
                                        CanFunctionType methodTy) {
  auto partialApplyTy = SILBuilder::getPartialApplyResultType(method->getType(),
                                            /*argCount*/1,
                                            SGF.SGM.M,
                                            /*subs*/{},
                                            ParameterConvention::Direct_Owned);

  // Retain 'self' because the partial apply will take ownership.
  // We can't simply forward 'self' because the partial apply is conditional.
  if (!self->getType().isAddress())
    self = SGF.B.emitCopyValueOperation(loc, self);

  SILValue result = SGF.B.createPartialApply(loc, method, method->getType(), {},
                                             self, partialApplyTy);
  // If necessary, thunk to the native ownership conventions and bridged types.
  auto nativeTy = SGF.getLoweredLoadableType(methodTy).castTo<SILFunctionType>();

  if (nativeTy != partialApplyTy.getSwiftRValueType()) {
    result = SGF.emitBlockToFunc(loc, ManagedValue::forUnmanaged(result),
                                 nativeTy).forward(SGF);
  }

  return result;
}

RValue SILGenFunction::emitDynamicMemberRefExpr(DynamicMemberRefExpr *e,
                                                SGFContext c) {
  // Emit the operand.
  ManagedValue base = emitRValueAsSingleValue(e->getBase());

  SILValue operand = base.getValue();
  if (!e->getMember().getDecl()->isInstanceMember()) {
    auto metatype = operand->getType().castTo<MetatypeType>();
    assert(metatype->getRepresentation() == MetatypeRepresentation::Thick);
    metatype = CanMetatypeType::get(metatype.getInstanceType(),
                                    MetatypeRepresentation::ObjC);
    operand = B.createThickToObjCMetatype(e, operand,
                                    SILType::getPrimitiveObjectType(metatype));
  }

  // Create the continuation block.
  SILBasicBlock *contBB = createBasicBlock();

  // Create the no-member block.
  SILBasicBlock *noMemberBB = createBasicBlock();

  // Create the has-member block.
  SILBasicBlock *hasMemberBB = createBasicBlock();

  // The continuation block
  auto memberMethodTy = e->getType()->getAnyOptionalObjectType();
  
  const TypeLowering &optTL = getTypeLowering(e->getType());
  auto loweredOptTy = optTL.getLoweredType();

  SILValue optTemp = emitTemporaryAllocation(e, loweredOptTy);

  // Create the branch.
  FuncDecl *memberFunc;
  if (auto *VD = dyn_cast<VarDecl>(e->getMember().getDecl())) {
    memberFunc = VD->getGetter();
    memberMethodTy = FunctionType::get(getASTContext().TheEmptyTupleType,
                                       memberMethodTy);
  } else
    memberFunc = cast<FuncDecl>(e->getMember().getDecl());
  auto member = SILDeclRef(memberFunc, SILDeclRef::Kind::Func)
    .asForeign();
  B.createDynamicMethodBranch(e, operand, member, hasMemberBB, noMemberBB);

  // Create the has-member branch.
  {
    B.emitBlock(hasMemberBB);

    FullExpr hasMemberScope(Cleanups, CleanupLocation(e));

    // The argument to the has-member block is the uncurried method.
    auto valueTy = e->getType()->getCanonicalType().getAnyOptionalObjectType();
    auto methodTy = valueTy;

    // For a computed variable, we want the getter.
    if (isa<VarDecl>(e->getMember().getDecl()))
      methodTy = CanFunctionType::get(TupleType::getEmpty(getASTContext()),
                                      methodTy);

    auto memberFnTy = CanFunctionType::get(
                                       operand->getType().getSwiftRValueType(),
                                       memberMethodTy->getCanonicalType());

    auto dynamicMethodTy = getDynamicMethodLoweredType(*this, operand, member,
                                                       memberFnTy);
    auto loweredMethodTy = SILType::getPrimitiveObjectType(dynamicMethodTy);
    SILValue memberArg = hasMemberBB->createPHIArgument(
        loweredMethodTy, ValueOwnershipKind::Owned);

    // Create the result value.
    SILValue result = emitDynamicPartialApply(*this, e, memberArg, operand,
                                              cast<FunctionType>(methodTy));
    Scope applyScope(Cleanups, CleanupLocation(e));
    RValue resultRV;
    if (isa<VarDecl>(e->getMember().getDecl())) {
      resultRV = emitMonomorphicApply(e, ManagedValue::forUnmanaged(result),
                                      {}, valueTy,
                                      ApplyOptions::DoesNotThrow,
                                      None, None);
    } else {
      resultRV = RValue(*this, e, valueTy,
                        emitManagedRValueWithCleanup(result));
    }

    // Package up the result in an optional.
    emitInjectOptionalValueInto(e, {e, std::move(resultRV)}, optTemp, optTL);

    applyScope.pop();
    // Branch to the continuation block.
    B.createBranch(e, contBB);
  }

  // Create the no-member branch.
  {
    B.emitBlock(noMemberBB);

    emitInjectOptionalNothingInto(e, optTemp, optTL);

    // Branch to the continuation block.
    B.createBranch(e, contBB);
  }

  // Emit the continuation block.
  B.emitBlock(contBB);

  // Package up the result.
  auto optResult = optTemp;
  if (optTL.isLoadable())
    optResult = optTL.emitLoad(B, e, optResult, LoadOwnershipQualifier::Take);
  return RValue(*this, e, emitManagedRValueWithCleanup(optResult, optTL));
}

RValue SILGenFunction::emitDynamicSubscriptExpr(DynamicSubscriptExpr *e,
                                                SGFContext c) {
  // Emit the base operand.
  ManagedValue managedBase = emitRValueAsSingleValue(e->getBase());

  SILValue base = managedBase.getValue();

  // Emit the index.
  RValue index = emitRValue(e->getIndex());

  // Create the continuation block.
  SILBasicBlock *contBB = createBasicBlock();

  // Create the no-member block.
  SILBasicBlock *noMemberBB = createBasicBlock();

  // Create the has-member block.
  SILBasicBlock *hasMemberBB = createBasicBlock();

  const TypeLowering &optTL = getTypeLowering(e->getType());
  auto loweredOptTy = optTL.getLoweredType();
  SILValue optTemp = emitTemporaryAllocation(e, loweredOptTy);

  // Create the branch.
  auto subscriptDecl = cast<SubscriptDecl>(e->getMember().getDecl());
  auto member = SILDeclRef(subscriptDecl->getGetter(),
                           SILDeclRef::Kind::Func)
    .asForeign();
  B.createDynamicMethodBranch(e, base, member, hasMemberBB, noMemberBB);

  // Create the has-member branch.
  {
    B.emitBlock(hasMemberBB);

    FullExpr hasMemberScope(Cleanups, CleanupLocation(e));

    // The argument to the has-member block is the uncurried method.
    // Build the substituted getter type from the AST nodes.
    auto valueTy = e->getType()->getCanonicalType().getAnyOptionalObjectType();
    auto indexTy = e->getIndex()->getType()->getCanonicalType();
    auto methodTy = CanFunctionType::get(indexTy,
                                         valueTy);
    
    auto functionTy = CanFunctionType::get(base->getType().getSwiftRValueType(),
                                           methodTy);
    auto dynamicMethodTy = getDynamicMethodLoweredType(*this, base, member,
                                                       functionTy);
    auto loweredMethodTy = SILType::getPrimitiveObjectType(dynamicMethodTy);
    SILValue memberArg = hasMemberBB->createPHIArgument(
        loweredMethodTy, ValueOwnershipKind::Owned);
    // Emit the application of 'self'.
    SILValue result = emitDynamicPartialApply(*this, e, memberArg, base,
                                              cast<FunctionType>(methodTy));
    // Emit the index.
    llvm::SmallVector<ManagedValue, 2> indexArgs;
    std::move(index).getAll(indexArgs);
    
    Scope applyScope(Cleanups, CleanupLocation(e));
    auto resultRV = emitMonomorphicApply(e, ManagedValue::forUnmanaged(result),
                                         indexArgs, valueTy,
                                         ApplyOptions::DoesNotThrow,
                                         None, None);

    // Package up the result in an optional.
    emitInjectOptionalValueInto(e, {e, std::move(resultRV)}, optTemp, optTL);

    applyScope.pop();
    // Branch to the continuation block.
    B.createBranch(e, contBB);
  }

  // Create the no-member branch.
  {
    B.emitBlock(noMemberBB);

    emitInjectOptionalNothingInto(e, optTemp, optTL);

    // Branch to the continuation block.
    B.createBranch(e, contBB);
  }

  // Emit the continuation block.
  B.emitBlock(contBB);

  // Package up the result.
  auto optResult = optTemp;
  if (optTL.isLoadable())
    optResult = optTL.emitLoad(B, e, optResult, LoadOwnershipQualifier::Take);
  return RValue(*this, e, emitManagedRValueWithCleanup(optResult, optTL));
}

ManagedValue ArgumentScope::popPreservingValue(ManagedValue mv) {
  CleanupCloner cloner(SGF, mv);
  SILValue value = mv.forward(SGF);
  pop();
  return cloner.clone(value);
}
