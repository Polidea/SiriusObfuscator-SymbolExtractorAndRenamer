//===--- TypeCheckGeneric.cpp - Generics ----------------------------------===//
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
// This file implements support for generics.
//
//===----------------------------------------------------------------------===//
#include "TypeChecker.h"
#include "GenericTypeResolver.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignatureBuilder.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Defer.h"
#include "llvm/Support/ErrorHandling.h"

using namespace swift;

///
/// GenericTypeResolver implementations
///

Type DependentGenericTypeResolver::resolveGenericTypeParamType(
                                     GenericTypeParamType *gp) {
  assert(gp->getDecl() && "Missing generic parameter declaration");

  // Don't resolve generic parameters.
  return gp;
}

Type DependentGenericTypeResolver::resolveDependentMemberType(
                                     Type baseTy,
                                     DeclContext *DC,
                                     SourceRange baseRange,
                                     ComponentIdentTypeRepr *ref) {
  return DependentMemberType::get(baseTy, ref->getIdentifier());
}

Type DependentGenericTypeResolver::resolveSelfAssociatedType(
       Type selfTy, AssociatedTypeDecl *assocType) {
  return DependentMemberType::get(selfTy, assocType);
}

Type DependentGenericTypeResolver::resolveTypeOfContext(DeclContext *dc) {
  return dc->getSelfInterfaceType();
}

Type DependentGenericTypeResolver::resolveTypeOfDecl(TypeDecl *decl) {
  return decl->getDeclaredInterfaceType();
}

bool DependentGenericTypeResolver::areSameType(Type type1, Type type2) {
  if (!type1->hasTypeParameter() && !type2->hasTypeParameter())
    return type1->isEqual(type2);

  // Conservative answer: they could be the same.
  return true;
}

void DependentGenericTypeResolver::recordParamType(ParamDecl *decl, Type type) {
  // Do nothing
}

Type GenericTypeToArchetypeResolver::resolveGenericTypeParamType(
                                       GenericTypeParamType *gp) {
  return GenericEnv->mapTypeIntoContext(gp);
}

Type GenericTypeToArchetypeResolver::resolveDependentMemberType(
                                  Type baseTy,
                                  DeclContext *DC,
                                  SourceRange baseRange,
                                  ComponentIdentTypeRepr *ref) {
  llvm_unreachable("Dependent type after archetype substitution");
}

Type GenericTypeToArchetypeResolver::resolveSelfAssociatedType(
       Type selfTy, AssociatedTypeDecl *assocType) {
  llvm_unreachable("Dependent type after archetype substitution");  
}

Type GenericTypeToArchetypeResolver::resolveTypeOfContext(DeclContext *dc) {
  return GenericEnvironment::mapTypeIntoContext(
      GenericEnv, dc->getSelfInterfaceType());
}

Type GenericTypeToArchetypeResolver::resolveTypeOfDecl(TypeDecl *decl) {
  return GenericEnvironment::mapTypeIntoContext(
      GenericEnv, decl->getDeclaredInterfaceType());
}

bool GenericTypeToArchetypeResolver::areSameType(Type type1, Type type2) {
  return type1->isEqual(type2);
}

void GenericTypeToArchetypeResolver::recordParamType(ParamDecl *decl, Type type) {
  decl->setType(type);

  // When type checking a closure or subscript index, this is the only
  // resolver that runs, so make sure we also set the interface type,
  // if one was not already set.
  //
  // When type checking functions, the CompleteGenericTypeResolver sets
  // the interface type.
  if (!decl->hasInterfaceType())
    decl->setInterfaceType(GenericEnvironment::mapTypeOutOfContext(
        GenericEnv, type));
}

Type ProtocolRequirementTypeResolver::resolveGenericTypeParamType(
    GenericTypeParamType *gp) {
  assert(gp->isEqual(Proto->getSelfInterfaceType()) &&
         "found non-Self-shaped GTPT when resolving protocol requirement");
  return gp;
}

Type ProtocolRequirementTypeResolver::resolveDependentMemberType(
    Type baseTy, DeclContext *DC, SourceRange baseRange,
    ComponentIdentTypeRepr *ref) {
  return DependentMemberType::get(baseTy, ref->getIdentifier());
}

Type ProtocolRequirementTypeResolver::resolveSelfAssociatedType(
    Type selfTy, AssociatedTypeDecl *assocType) {
  assert(selfTy->isEqual(Proto->getSelfInterfaceType()));
  (void)Proto;
  return assocType->getDeclaredInterfaceType();
}

Type ProtocolRequirementTypeResolver::resolveTypeOfContext(DeclContext *dc) {
  return dc->getSelfInterfaceType();
}

Type ProtocolRequirementTypeResolver::resolveTypeOfDecl(TypeDecl *decl) {
  return decl->getDeclaredInterfaceType();
}

bool ProtocolRequirementTypeResolver::areSameType(Type type1, Type type2) {
  if (type1->isEqual(type2))
    return true;

  // If both refer to associated types with the same name, they'll implicitly
  // be considered equivalent.
  auto depMem1 = type1->getAs<DependentMemberType>();
  if (!depMem1) return false;

  auto depMem2 = type2->getAs<DependentMemberType>();
  if (!depMem2) return false;

  if (depMem1->getName() != depMem2->getName()) return false;

  return areSameType(depMem1->getBase(), depMem2->getBase());
}

void ProtocolRequirementTypeResolver::recordParamType(ParamDecl *decl,
                                                      Type type) {
  llvm_unreachable(
      "recording a param type of a protocol requirement doesn't make sense");
}

Type CompleteGenericTypeResolver::resolveGenericTypeParamType(
                                              GenericTypeParamType *gp) {
  assert(gp->getDecl() && "Missing generic parameter declaration");
  return gp;
}


Type CompleteGenericTypeResolver::resolveDependentMemberType(
                                    Type baseTy,
                                    DeclContext *DC,
                                    SourceRange baseRange,
                                    ComponentIdentTypeRepr *ref) {
  // Resolve the base to a potential archetype.
  auto basePA =
    Builder.resolveArchetype(baseTy,
                             ArchetypeResolutionKind::CompleteWellFormed);
  assert(basePA && "Missing potential archetype for base");

  // Retrieve the potential archetype for the nested type.
  auto nestedPA = basePA->getNestedType(ref->getIdentifier(),
                                        Builder);

  // If there was no such nested type, produce an error.
  if (!nestedPA) {
    // Perform typo correction.
    LookupResult corrections;
    TC.performTypoCorrection(DC, DeclRefKind::Ordinary,
                             MetatypeType::get(baseTy),
                             ref->getIdentifier(), ref->getIdLoc(),
                             NameLookupFlags::ProtocolMembers,
                             corrections, &Builder);

    // Filter out non-types.
    corrections.filter([](const LookupResult::Result &result) {
      return isa<TypeDecl>(result.Decl);
    });

    // Check whether we have a single type result.
    auto singleType = corrections.getSingleTypeResult();

    // If we don't have a single result, complain and fail.
    if (!singleType) {
      Identifier name = ref->getIdentifier();
      SourceLoc nameLoc = ref->getIdLoc();
      TC.diagnose(nameLoc, diag::invalid_member_type, name, baseTy)
        .highlight(baseRange);
      for (const auto &suggestion : corrections)
        TC.noteTypoCorrection(name, DeclNameLoc(nameLoc), suggestion);

      return ErrorType::get(TC.Context);
    }

    // We have a single type result. Suggest it.
    TC.diagnose(ref->getIdLoc(), diag::invalid_member_type_suggest,
                baseTy, ref->getIdentifier(),
                singleType->getBaseName())
      .fixItReplace(ref->getIdLoc(),
                    singleType->getBaseName().getIdentifier().str());

    // Correct to the single type result.
    ref->overwriteIdentifier(singleType->getBaseName());
    ref->setValue(singleType);
  } else if (auto assocType = nestedPA->getResolvedAssociatedType()) {
    ref->setValue(assocType);
  } else {
    assert(nestedPA->getConcreteTypeDecl());
    ref->setValue(nestedPA->getConcreteTypeDecl());
  }

  // If the nested type has been resolved to an associated type, use it.
  if (auto assocType = dyn_cast<AssociatedTypeDecl>(ref->getBoundDecl())) {
    return DependentMemberType::get(baseTy, assocType);
  }

  // Otherwise, the nested type comes from a concrete tpye. Substitute the
  // base type into it.
  auto concrete = ref->getBoundDecl();
  TC.validateDeclForNameLookup(concrete);
  if (!concrete->hasInterfaceType())
    return ErrorType::get(TC.Context);
  if (baseTy->isTypeParameter()) {
    if (auto proto =
          concrete->getDeclContext()
            ->getAsProtocolOrProtocolExtensionContext()) {
      TC.validateDecl(proto);
      auto subMap = SubstitutionMap::getProtocolSubstitutions(
                      proto, baseTy, ProtocolConformanceRef(proto));
      return concrete->getDeclaredInterfaceType().subst(subMap);
    }

    if (auto superclass = basePA->getSuperclass()) {
      return superclass->getTypeOfMember(
                                       DC->getParentModule(), concrete,
                                       concrete->getDeclaredInterfaceType());
    }

    llvm_unreachable("shouldn't have a concrete decl here");
  }

  return TC.substMemberTypeWithBase(DC->getParentModule(), concrete, baseTy);
}

Type CompleteGenericTypeResolver::resolveSelfAssociatedType(
       Type selfTy, AssociatedTypeDecl *assocType) {
  return Builder.resolveArchetype(selfTy,
                                  ArchetypeResolutionKind::CompleteWellFormed)
           ->getNestedType(assocType, Builder)
           ->getDependentType(GenericParams);
}

Type CompleteGenericTypeResolver::resolveTypeOfContext(DeclContext *dc) {
  return dc->getSelfInterfaceType();
}

Type CompleteGenericTypeResolver::resolveTypeOfDecl(TypeDecl *decl) {
  return decl->getDeclaredInterfaceType();
}


bool CompleteGenericTypeResolver::areSameType(Type type1, Type type2) {
  if (!type1->hasTypeParameter() && !type2->hasTypeParameter())
    return type1->isEqual(type2);

  auto pa1 =
    Builder.resolveArchetype(type1,
                             ArchetypeResolutionKind::CompleteWellFormed);
  auto pa2 =
    Builder.resolveArchetype(type2,
                             ArchetypeResolutionKind::CompleteWellFormed);
  if (pa1 && pa2)
    return pa1->isInSameEquivalenceClassAs(pa2);

  return type1->isEqual(type2);
}

void
CompleteGenericTypeResolver::recordParamType(ParamDecl *decl, Type type) {
  decl->setInterfaceType(type);
}

///
/// Common code for generic functions, generic types
///

/// Check the generic parameters in the given generic parameter list (and its
/// parent generic parameter lists) according to the given resolver.
void TypeChecker::checkGenericParamList(GenericSignatureBuilder *builder,
                                        GenericParamList *genericParams,
                                        GenericSignature *parentSig,
                                        GenericTypeResolver *resolver) {
  // If there is a parent context, add the generic parameters and requirements
  // from that context.
  if (builder)
    builder->addGenericSignature(parentSig);

  // If there aren't any generic parameters at this level, we're done.
  if (!genericParams)
    return;

  assert(genericParams->size() > 0 &&
         "Parsed an empty generic parameter list?");

  // Determine where and how to perform name lookup for the generic
  // parameter lists and where clause.
  TypeResolutionOptions options;
  DeclContext *lookupDC = genericParams->begin()[0]->getDeclContext();
  if (!lookupDC->isModuleScopeContext()) {
    assert((isa<GenericTypeDecl>(lookupDC) ||
            isa<ExtensionDecl>(lookupDC) ||
            isa<AbstractFunctionDecl>(lookupDC) ||
            isa<SubscriptDecl>(lookupDC)) &&
           "not a proper generic parameter context?");
    options = TR_GenericSignature;
  }    

  // First, add the generic parameters to the generic signature builder.
  // Do this before checking the inheritance clause, since it may
  // itself be dependent on one of these parameters.
  if (builder) {
    for (auto param : *genericParams)
      builder->addGenericParameter(param);
  }

  // Now, check the inheritance clauses of each parameter.
  for (auto param : *genericParams) {
    checkInheritanceClause(param, resolver);

    if (builder)
      builder->addGenericParameterRequirements(param);
  }

  // Visit each of the requirements, adding them to the builder.
  // Add the requirements clause to the builder, validating the types in
  // the requirements clause along the way.
  for (auto &req : genericParams->getRequirements()) {
    if (validateRequirement(genericParams->getWhereLoc(), req, lookupDC,
                            options, resolver))
      continue;

    if (builder &&
        isErrorResult(builder->addRequirement(&req,
                                              lookupDC->getParentModule())))
      req.setInvalid();
  }
}

bool TypeChecker::validateRequirement(SourceLoc whereLoc, RequirementRepr &req,
                                      DeclContext *lookupDC,
                                      TypeResolutionOptions options,
                                      GenericTypeResolver *resolver) {
  if (req.isInvalid())
    return true;

  switch (req.getKind()) {
  case RequirementReprKind::TypeConstraint: {
    // Validate the types.
    if (validateType(req.getSubjectLoc(), lookupDC, options, resolver)) {
      req.setInvalid();
    }

    if (validateType(req.getConstraintLoc(), lookupDC, options, resolver)) {
      req.setInvalid();
    }

    return req.isInvalid();
  }

  case RequirementReprKind::LayoutConstraint: {
    // Validate the types.
    if (validateType(req.getSubjectLoc(), lookupDC, options, resolver)) {
      req.setInvalid();
    }

    if (req.getLayoutConstraintLoc().isNull()) {
      req.setInvalid();
    }
    return req.isInvalid();
  }

  case RequirementReprKind::SameType: {
    if (validateType(req.getFirstTypeLoc(), lookupDC, options, resolver)) {
      req.setInvalid();
    }

    if (validateType(req.getSecondTypeLoc(), lookupDC, options, resolver)) {
      req.setInvalid();
    }

    return req.isInvalid();
  }
  }

  llvm_unreachable("Unhandled RequirementKind in switch.");
}

void
TypeChecker::prepareGenericParamList(GenericParamList *gp,
                                     DeclContext *dc) {
  Accessibility access;
  if (auto *fd = dyn_cast<FuncDecl>(dc))
    access = fd->getFormalAccess();
  else if (auto *nominal = dyn_cast<NominalTypeDecl>(dc))
    access = nominal->getFormalAccess();
  else
    access = Accessibility::Internal;
  access = std::max(access, Accessibility::Internal);

  unsigned depth = gp->getDepth();
  for (auto paramDecl : *gp) {
    paramDecl->setDepth(depth);
    if (!paramDecl->hasAccessibility())
      paramDecl->setAccessibility(access);
  }
}

/// Add the generic parameter types from the given list to the vector.
static void addGenericParamTypes(GenericParamList *gpList,
                                 SmallVectorImpl<GenericTypeParamType *> &params) {
  if (!gpList) return;

  for (auto gpDecl : *gpList) {
    params.push_back(
            gpDecl->getDeclaredInterfaceType()->castTo<GenericTypeParamType>());
  }
}

static void revertDependentTypeLoc(TypeLoc &tl) {
  // If there's no type representation, there's nothing to revert.
  if (!tl.getTypeRepr())
    return;

  // Don't revert an error type; we've already complained.
  if (tl.wasValidated() && tl.isError())
    return;

  // Make sure we validate the type again.
  tl.setType(Type(), /*validated=*/false);
}

/// Revert the dependent types within the given generic parameter list.
void TypeChecker::revertGenericParamList(GenericParamList *genericParams) {
  // Revert the inherited clause of the generic parameter list.
  for (auto param : *genericParams) {
    param->setCheckedInheritanceClause(false);
    for (auto &inherited : param->getInherited())
      revertDependentTypeLoc(inherited);
  }

  // Revert the requirements of the generic parameter list.
  for (auto &req : genericParams->getRequirements()) {
    if (req.isInvalid())
      continue;

    switch (req.getKind()) {
    case RequirementReprKind::TypeConstraint: {
      revertDependentTypeLoc(req.getSubjectLoc());
      revertDependentTypeLoc(req.getConstraintLoc());
      break;
    }
    case RequirementReprKind::LayoutConstraint: {
      revertDependentTypeLoc(req.getSubjectLoc());
      break;
    }
    case RequirementReprKind::SameType:
      revertDependentTypeLoc(req.getFirstTypeLoc());
      revertDependentTypeLoc(req.getSecondTypeLoc());
      break;
    }
  }
}

///
/// Generic functions
///

/// Check the signature of a generic function.
static bool checkGenericFuncSignature(TypeChecker &tc,
                                      GenericSignatureBuilder *builder,
                                      AbstractFunctionDecl *func,
                                      GenericTypeResolver &resolver) {
  bool badType = false;

  // Check the generic parameter list.
  auto genericParams = func->getGenericParams();

  tc.checkGenericParamList(
                         builder, genericParams,
                         func->getDeclContext()->getGenericSignatureOfContext(),
                         &resolver);

  // Check the parameter patterns.
  for (auto params : func->getParameterLists()) {
    // Check the pattern.
    if (tc.typeCheckParameterList(params, func, TypeResolutionOptions(),
                                  resolver))
      badType = true;

    // Infer requirements from the pattern.
    if (builder) {
      builder->inferRequirements(*func->getParentModule(), params,
                                 genericParams);
    }
  }

  // If there is a declared result type, check that as well.
  if (auto fn = dyn_cast<FuncDecl>(func)) {
    if (!fn->getBodyResultTypeLoc().isNull()) {
      // Check the result type of the function.
      TypeResolutionOptions options;
      if (fn->hasDynamicSelf())
        options |= TR_DynamicSelfResult;

      if (tc.validateType(fn->getBodyResultTypeLoc(), fn, options, &resolver)) {
        badType = true;
      }

      // Infer requirements from it.
      if (builder && genericParams &&
          fn->getBodyResultTypeLoc().getTypeRepr()) {
        auto source =
          GenericSignatureBuilder::FloatingRequirementSource::forInferred(
              fn->getBodyResultTypeLoc().getTypeRepr(),
              /*quietly=*/true);
        builder->inferRequirements(*func->getParentModule(),
                                   fn->getBodyResultTypeLoc(),
                                   source);
      }
    }

    // If this is a materializeForSet, infer requirements from the
    // storage type instead, since it's not part of the accessor's
    // type signature.
    if (fn->getAccessorKind() == AccessorKind::IsMaterializeForSet) {
      if (builder) {
        auto *storage = fn->getAccessorStorageDecl();
        if (auto *subscriptDecl = dyn_cast<SubscriptDecl>(storage)) {
          auto source =
            GenericSignatureBuilder::FloatingRequirementSource::forInferred(
                subscriptDecl->getElementTypeLoc().getTypeRepr(),
                /*quietly=*/true);

          TypeLoc type(nullptr, subscriptDecl->getElementInterfaceType());
          assert(type.getType());
          builder->inferRequirements(*func->getParentModule(),
                                     type, source);
        }
      }
    }
  }

  return badType;
}

void TypeChecker::revertGenericFuncSignature(AbstractFunctionDecl *func) {
  // Revert the result type.
  if (auto fn = dyn_cast<FuncDecl>(func))
    if (!fn->getBodyResultTypeLoc().isNull())
      revertDependentTypeLoc(fn->getBodyResultTypeLoc());

  // Revert the body parameter types.
  for (auto paramList : func->getParameterLists()) {
    for (auto &param : *paramList) {
      revertDependentTypeLoc(param->getTypeLoc());
    }
  }
}

/// Determine whether the given type is \c Self, an associated type of \c Self,
/// or a concrete type.
static bool isSelfDerivedOrConcrete(Type protoSelf, Type type) {
  // Check for a concrete type.
  if (!type->hasTypeParameter())
    return true;

  if (type->isTypeParameter() &&
      type->getRootGenericParam()->isEqual(protoSelf))
    return true;

  return false;
}

// For a generic requirement in a protocol, make sure that the requirement
// set didn't add any requirements to Self or its associated types.
static bool checkProtocolSelfRequirements(GenericSignature *sig,
                                          ValueDecl *decl,
                                          TypeChecker &TC) {
  // For a generic requirement in a protocol, make sure that the requirement
  // set didn't add any requirements to Self or its associated types.
  if (auto *proto = dyn_cast<ProtocolDecl>(decl->getDeclContext())) {
    auto protoSelf = proto->getSelfInterfaceType();
    for (auto req : sig->getRequirements()) {
      // If one of the types in the requirement is dependent on a non-Self
      // type parameter, this requirement is okay.
      if (!isSelfDerivedOrConcrete(protoSelf, req.getFirstType()) ||
          !isSelfDerivedOrConcrete(protoSelf, req.getSecondType()))
        continue;

      // The conformance of 'Self' to the protocol is okay.
      if (req.getKind() == RequirementKind::Conformance &&
          req.getSecondType()->getAs<ProtocolType>()->getDecl() == proto &&
          req.getFirstType()->is<GenericTypeParamType>())
        continue;

      TC.diagnose(decl,
                  TC.Context.LangOpts.EffectiveLanguageVersion[0] >= 4
                    ? diag::requirement_restricts_self
                    : diag::requirement_restricts_self_swift3,
                  decl->getDescriptiveKind(), decl->getFullName(),
                  req.getFirstType().getString(),
                  static_cast<unsigned>(req.getKind()),
                  req.getSecondType().getString());

      if (TC.Context.LangOpts.EffectiveLanguageVersion[0] >= 4)
        return true;
    }
  }

  return false;
}

/// All generic parameters of a generic function must be referenced in the
/// declaration's type, otherwise we have no way to infer them.
static void checkReferencedGenericParams(GenericContext *dc,
                                         GenericSignature *sig,
                                         TypeChecker &TC) {
  auto *genericParams = dc->getGenericParams();
  if (!genericParams)
    return;

  auto *decl = cast<ValueDecl>(dc->getInnermostDeclarationDeclContext());

  // A helper class to collect referenced generic type parameters
  // and dependent member types.
  class ReferencedGenericTypeWalker : public TypeWalker {
    SmallPtrSet<CanType, 4> ReferencedGenericParams;

  public:
    ReferencedGenericTypeWalker() {}
    Action walkToTypePre(Type ty) override {
      // Find generic parameters or dependent member types.
      // Once such a type is found, don't recurse into its children.
      if (!ty->hasTypeParameter())
        return Action::SkipChildren;
      if (ty->isTypeParameter()) {
        ReferencedGenericParams.insert(ty->getCanonicalType());
        return Action::SkipChildren;
      }
      return Action::Continue;
    }

    SmallPtrSet<CanType, 4> &getReferencedGenericParams() {
      return ReferencedGenericParams;
    }
  };

  // Collect all generic params referenced in parameter types and
  // return type.
  ReferencedGenericTypeWalker paramsAndResultWalker;
  auto *funcTy = decl->getInterfaceType()->castTo<GenericFunctionType>();
  funcTy->getInput().walk(paramsAndResultWalker);
  funcTy->getResult().walk(paramsAndResultWalker);

  // Set of generic params referenced in parameter types,
  // return type or requirements.
  auto &referencedGenericParams =
      paramsAndResultWalker.getReferencedGenericParams();

  // Check if at least one of the generic params in the requirement refers
  // to an already referenced generic parameter. If this is the case,
  // then the other type is also considered as referenced, because
  // it is used to put requirements on the first type.
  auto reqTypesVisitor = [&referencedGenericParams](Requirement req) -> bool {
    Type first;
    Type second;

    switch (req.getKind()) {
    case RequirementKind::Superclass:
    case RequirementKind::SameType:
      second = req.getSecondType();
      LLVM_FALLTHROUGH;

    case RequirementKind::Conformance:
    case RequirementKind::Layout:
      first = req.getFirstType();
      break;
    }

    // Collect generic parameter types referenced by types used in a requirement.
    ReferencedGenericTypeWalker walker;
    if (first && first->hasTypeParameter())
      first.walk(walker);
    if (second && second->hasTypeParameter())
      second.walk(walker);
    auto &genericParamsUsedByRequirementTypes =
        walker.getReferencedGenericParams();

    // If at least one of the collected generic types or a root generic
    // parameter of dependent member types is known to be referenced by
    // parameter types, return types or other types known to be "referenced",
    // then all the types used in the requirement are considered to be
    // referenced, because they are used to defined something that is known
    // to be referenced.
    bool foundNewReferencedGenericParam = false;
    if (std::any_of(genericParamsUsedByRequirementTypes.begin(),
                    genericParamsUsedByRequirementTypes.end(),
                    [&referencedGenericParams](CanType t) {
                      assert(t->isTypeParameter());
                      return referencedGenericParams.find(
                                 t->getRootGenericParam()
                                     ->getCanonicalType()) !=
                             referencedGenericParams.end();
                    })) {
      std::for_each(genericParamsUsedByRequirementTypes.begin(),
                    genericParamsUsedByRequirementTypes.end(),
                    [&referencedGenericParams,
                     &foundNewReferencedGenericParam](CanType t) {
                      // Add only generic type parameters, but ignore any
                      // dependent member types, because requirement
                      // on a dependent member type does not provide enough
                      // information to infer the base generic type
                      // parameter.
                      if (!t->is<GenericTypeParamType>())
                        return;
                      if (referencedGenericParams.insert(t).second)
                        foundNewReferencedGenericParam = true;
                    });
    }
    return foundNewReferencedGenericParam;
  };

  ArrayRef<Requirement> requirements;

  auto FindReferencedGenericParamsInRequirements = [&requirements, sig, &reqTypesVisitor] {
    requirements = sig->getRequirements();
    // Try to find new referenced generic parameter types in requirements until
    // we reach a fix point. We need to iterate until a fix point, because we
    // may have e.g. chains of same-type requirements like:
    // not-yet-referenced-T1 == not-yet-referenced-T2.DepType2,
    // not-yet-referenced-T2 == not-yet-referenced-T3.DepType3,
    // not-yet-referenced-T3 == referenced-T4.DepType4.
    // When we process the first of these requirements, we don't know yet that
    // T2
    // will be referenced, because T3 will be referenced,
    // because T3 == T4.DepType4.
    while (true) {
      bool foundNewReferencedGenericParam = false;
      for (auto req : requirements) {
        if (reqTypesVisitor(req))
          foundNewReferencedGenericParam = true;
      }
      if (!foundNewReferencedGenericParam)
        break;
    }
  };

  // Find the depth of the function's own generic parameters.
  unsigned fnGenericParamsDepth = genericParams->getDepth();

  // Check that every generic parameter type from the signature is
  // among referencedGenericParams.
  for (auto *genParam : sig->getGenericParams()) {
    auto *paramDecl = genParam->getDecl();
    if (paramDecl->getDepth() != fnGenericParamsDepth)
      continue;
    if (!referencedGenericParams.count(genParam->getCanonicalType())) {
      // Lazily search for generic params that are indirectly used in the
      // function signature. Do it only if there is a generic parameter
      // that is not known to be referenced yet.
      if (requirements.empty()) {
        FindReferencedGenericParamsInRequirements();
        // Nothing to do if this generic parameter is considered to be
        // referenced after analyzing the requirements from the generic
        // signature.
        if (referencedGenericParams.count(genParam->getCanonicalType()))
          continue;
      }
      // Produce an error that this generic parameter cannot be bound.
      TC.diagnose(paramDecl->getLoc(), diag::unreferenced_generic_parameter,
                  paramDecl->getNameStr());
      decl->setInterfaceType(ErrorType::get(TC.Context));
      decl->setInvalid();
    }
  }
}

GenericSignature *
TypeChecker::validateGenericFuncSignature(AbstractFunctionDecl *func) {
  bool invalid = false;

  auto *gp = func->getGenericParams();
  if (gp)
    prepareGenericParamList(gp, func);

  // Collect the generic parameters.
  SmallVector<GenericTypeParamType *, 4> allGenericParams;
  if (auto parentSig = func->getDeclContext()->getGenericSignatureOfContext())
    allGenericParams.append(parentSig->getGenericParams().begin(),
                            parentSig->getGenericParams().end());
  addGenericParamTypes(gp, allGenericParams);

  // Create the generic signature builder.
  GenericSignatureBuilder builder(Context, LookUpConformance(*this, func));

  // Type check the function declaration, treating all generic type
  // parameters as dependent, unresolved.
  DependentGenericTypeResolver dependentResolver;
  if (checkGenericFuncSignature(*this, &builder, func, dependentResolver))
    invalid = true;

  // Finalize the generic requirements.
  (void)builder.finalize(func->getLoc(), allGenericParams);

  // The generic signature builder now has all of the requirements, although
  // there might still be errors that have not yet been diagnosed. Revert the
  // generic function signature and type-check it again, completely.
  revertGenericFuncSignature(func);
  if (gp)
    revertGenericParamList(gp);

  CompleteGenericTypeResolver completeResolver(*this, builder,
                                               allGenericParams);
  if (checkGenericFuncSignature(*this, nullptr, func, completeResolver))
    invalid = true;

  // The generic function signature is complete and well-formed. Determine
  // the type of the generic function.
  auto sig = builder.getGenericSignature();

  if (!invalid)
    invalid = checkProtocolSelfRequirements(sig, func, *this);

  // Debugging of the generic signature builder and generic signature generation.
  if (Context.LangOpts.DebugGenericSignatures) {
    func->dumpRef(llvm::errs());
    llvm::errs() << "\n";
    builder.dump(llvm::errs());
    llvm::errs() << "Generic signature: ";
    sig->print(llvm::errs());
    llvm::errs() << "\n";
    llvm::errs() << "Canonical generic signature: ";
    sig->getCanonicalSignature()->print(llvm::errs());
    llvm::errs() << "\n";
  }

  if (invalid) {
    func->setInterfaceType(ErrorType::get(Context));
    func->setInvalid();
    // null doesn't mean error here: callers still expect the signature.
    return sig;
  }

  configureInterfaceType(func, sig);
  return sig;
}

void TypeChecker::configureInterfaceType(AbstractFunctionDecl *func,
                                         GenericSignature *sig) {
  Type funcTy;
  Type initFuncTy;

  if (auto fn = dyn_cast<FuncDecl>(func)) {
    funcTy = fn->getBodyResultTypeLoc().getType();
    if (!funcTy)
      funcTy = TupleType::getEmpty(Context);

  } else if (auto ctor = dyn_cast<ConstructorDecl>(func)) {
    auto *dc = ctor->getDeclContext();

    funcTy = dc->getSelfInterfaceType();
    if (!funcTy)
      funcTy = ErrorType::get(Context);

    // Adjust result type for failability.
    if (ctor->getFailability() != OTK_None)
      funcTy = OptionalType::get(ctor->getFailability(), funcTy);

    initFuncTy = funcTy;
  } else {
    assert(isa<DestructorDecl>(func));
    funcTy = TupleType::getEmpty(Context);
  }

  auto paramLists = func->getParameterLists();
  SmallVector<ParameterList*, 4> storedParamLists;

  // FIXME: Destructors don't have the '()' pattern in their signature, so
  // paste it here.
  if (isa<DestructorDecl>(func)) {
    assert(paramLists.size() == 1 && "Only the self paramlist");
    storedParamLists.push_back(paramLists[0]);
    storedParamLists.push_back(ParameterList::createEmpty(Context));
    paramLists = storedParamLists;
  }

  bool hasSelf = func->getDeclContext()->isTypeContext();
  for (unsigned i = 0, e = paramLists.size(); i != e; ++i) {
    Type argTy;
    Type initArgTy;

    Type selfTy;
    if (i == e-1 && hasSelf) {
      selfTy = ParenType::get(Context, func->computeInterfaceSelfType());

      // Substitute in our own 'self' parameter.

      argTy = selfTy;
      if (initFuncTy) {
        initArgTy = func->computeInterfaceSelfType(/*isInitializingCtor=*/true);
      }
    } else {
      argTy = paramLists[e - i - 1]->getInterfaceType(Context);

      if (initFuncTy)
        initArgTy = argTy;
    }

    // 'throws' only applies to the innermost function.
    AnyFunctionType::ExtInfo info;
    if (i == 0 && func->hasThrows())
      info = info.withThrows();

    assert(!argTy->hasArchetype());
    assert(!funcTy->hasArchetype());
    if (initFuncTy)
      assert(!initFuncTy->hasArchetype());

    if (sig && i == e-1) {
      funcTy = GenericFunctionType::get(sig, argTy, funcTy, info);
      if (initFuncTy)
        initFuncTy = GenericFunctionType::get(sig, initArgTy, initFuncTy, info);
    } else {
      funcTy = FunctionType::get(argTy, funcTy, info);
      if (initFuncTy)
        initFuncTy = FunctionType::get(initArgTy, initFuncTy, info);
    }
  }

  // Record the interface type.
  func->setInterfaceType(funcTy);
  if (initFuncTy)
    cast<ConstructorDecl>(func)->setInitializerInterfaceType(initFuncTy);

  // We get bogus errors here with generic subscript materializeForSet.
  if (!isa<FuncDecl>(func) ||
      cast<FuncDecl>(func)->getAccessorKind() !=
        AccessorKind::IsMaterializeForSet)
    checkReferencedGenericParams(func, sig, *this);
}

///
/// Generic subscripts
///
/// FIXME: A lot of this code is duplicated from the generic functions
/// path above. We could consolidate more of this.
///

/// Check the signature of a generic subscript.
static bool checkGenericSubscriptSignature(TypeChecker &tc,
                                           GenericSignatureBuilder *builder,
                                           SubscriptDecl *subscript,
                                           GenericTypeResolver &resolver) {
  bool badType = false;

  // Check the generic parameter list.
  auto genericParams = subscript->getGenericParams();

  auto *dc = subscript->getDeclContext();

  tc.checkGenericParamList(
                    builder, genericParams,
                    dc->getGenericSignatureOfContext(),
                    &resolver);

  // Check the element type.
  badType |= tc.validateType(subscript->getElementTypeLoc(), subscript,
                             TypeResolutionOptions(),
                             &resolver);

  // Infer requirements from it.
  if (genericParams && builder) {
    auto source =
      GenericSignatureBuilder::FloatingRequirementSource::forInferred(
          subscript->getElementTypeLoc().getTypeRepr(),
          /*quietly=*/true);

    builder->inferRequirements(*subscript->getParentModule(),
                               subscript->getElementTypeLoc(),
                               source);
  }

  // Check the indices.
  auto params = subscript->getIndices();

  badType |= tc.typeCheckParameterList(params, subscript,
                                       TR_SubscriptParameters,
                                       resolver);

  // Infer requirements from the pattern.
  if (builder) {
    builder->inferRequirements(*subscript->getParentModule(), params,
                               genericParams);
  }

  return badType;
}

void TypeChecker::revertGenericSubscriptSignature(SubscriptDecl *subscript) {
  // Revert the element type.
  if (!subscript->getElementTypeLoc().isNull())
    revertDependentTypeLoc(subscript->getElementTypeLoc());

  // Revert the indices.
  for (auto &param : *subscript->getIndices())
    revertDependentTypeLoc(param->getTypeLoc());
}

GenericSignature *
TypeChecker::validateGenericSubscriptSignature(SubscriptDecl *subscript) {
  bool invalid = false;

  auto *gp = subscript->getGenericParams();
  if (gp)
    prepareGenericParamList(gp, subscript);

  // Collect the generic parameters.
  SmallVector<GenericTypeParamType *, 4> allGenericParams;
  if (auto parentSig = subscript->getDeclContext()->getGenericSignatureOfContext())
    allGenericParams.append(parentSig->getGenericParams().begin(),
                            parentSig->getGenericParams().end());
  addGenericParamTypes(gp, allGenericParams);

  // Create the generic signature builder.
  GenericSignatureBuilder builder(Context, LookUpConformance(*this, subscript));

  // Type check the function declaration, treating all generic type
  // parameters as dependent, unresolved.
  DependentGenericTypeResolver dependentResolver;
  if (checkGenericSubscriptSignature(*this, &builder, subscript,
                                     dependentResolver))
    invalid = true;

  // Finalize the generic requirements.
  (void)builder.finalize(subscript->getLoc(), allGenericParams);

  // The generic signature builder now has all of the requirements, although
  // there might still be errors that have not yet been diagnosed. Revert the
  // generic function signature and type-check it again, completely.
  revertGenericSubscriptSignature(subscript);
  if (gp)
    revertGenericParamList(gp);

  CompleteGenericTypeResolver completeResolver(*this, builder,
                                               allGenericParams);
  if (checkGenericSubscriptSignature(*this, nullptr, subscript, completeResolver))
    invalid = true;

  // The generic subscript signature is complete and well-formed. Determine
  // the type of the generic subscript.
  auto sig = builder.getGenericSignature();

  if (!invalid)
    invalid = checkProtocolSelfRequirements(sig, subscript, *this);

  // Debugging of the generic signature builder and generic signature generation.
  if (Context.LangOpts.DebugGenericSignatures) {
    subscript->dumpRef(llvm::errs());
    llvm::errs() << "\n";
    builder.dump(llvm::errs());
    llvm::errs() << "Generic signature: ";
    sig->print(llvm::errs());
    llvm::errs() << "\n";
    llvm::errs() << "Canonical generic signature: ";
    sig->getCanonicalSignature()->print(llvm::errs());
    llvm::errs() << "\n";
  }

  if (invalid) {
    subscript->setInterfaceType(ErrorType::get(Context));
    subscript->setInvalid();
    // null doesn't mean error here: callers still expect the signature.
    return sig;
  }

  configureInterfaceType(subscript, sig);
  return sig;
}

void TypeChecker::configureInterfaceType(SubscriptDecl *subscript,
                                         GenericSignature *sig) {
  auto elementTy = subscript->getElementTypeLoc().getType();
  auto indicesTy = subscript->getIndices()->getInterfaceType(Context);
  Type funcTy;

  if (sig)
    funcTy = GenericFunctionType::get(sig, indicesTy, elementTy,
                                      AnyFunctionType::ExtInfo());
  else
    funcTy = FunctionType::get(indicesTy, elementTy);

  // Record the interface type.
  subscript->setInterfaceType(funcTy);

  checkReferencedGenericParams(subscript, sig, *this);
}

///
/// Generic types
///

/// Visit the given generic parameter lists from the outermost to the innermost,
/// calling the visitor function for each list.
static void visitOuterToInner(
                      GenericParamList *genericParams,
                      llvm::function_ref<void(GenericParamList *)> visitor) {
  if (auto outerGenericParams = genericParams->getOuterParameters())
    visitOuterToInner(outerGenericParams, visitor);

  visitor(genericParams);
}

GenericEnvironment *TypeChecker::checkGenericEnvironment(
                      GenericParamList *genericParams,
                      DeclContext *dc,
                      GenericSignature *parentSig,
                      bool allowConcreteGenericParams,
                      llvm::function_ref<void(GenericSignatureBuilder &)>
                        inferRequirements) {
  assert(genericParams && "Missing generic parameters?");
  bool recursivelyVisitGenericParams =
    genericParams->getOuterParameters() && !parentSig;

  // Collect the generic parameters.
  SmallVector<GenericTypeParamType *, 4> allGenericParams;
  if (recursivelyVisitGenericParams) {
    visitOuterToInner(genericParams,
                      [&](GenericParamList *gpList) {
      addGenericParamTypes(gpList, allGenericParams);
    });
  } else {
    if (parentSig) {
      allGenericParams.append(parentSig->getGenericParams().begin(),
                              parentSig->getGenericParams().end());
    }

    addGenericParamTypes(genericParams, allGenericParams);
  }

  // Create the generic signature builder.
  ModuleDecl *module = dc->getParentModule();
  GenericSignatureBuilder builder(Context, LookUpConformance(*this, dc));

  // Type check the generic parameters, treating all generic type
  // parameters as dependent, unresolved.
  DependentGenericTypeResolver dependentResolver;
  if (recursivelyVisitGenericParams) {
    visitOuterToInner(genericParams,
                      [&](GenericParamList *gpList) {
      checkGenericParamList(&builder, gpList, nullptr, &dependentResolver);
    });
  } else {
    checkGenericParamList(&builder, genericParams, parentSig,
                          &dependentResolver);
  }

  /// Perform any necessary requirement inference.
  inferRequirements(builder);

  // Finalize the generic requirements.
  (void)builder.finalize(genericParams->getSourceRange().Start,
                         allGenericParams,
                         allowConcreteGenericParams);

  // The generic signature builder now has all of the requirements, although
  // there might still be errors that have not yet been diagnosed. Revert the
  // signature and type-check it again, completely.
  if (recursivelyVisitGenericParams) {
    visitOuterToInner(genericParams,
                      [&](GenericParamList *gpList) {
      revertGenericParamList(gpList);
    });
  } else {
    revertGenericParamList(genericParams);
  }

  CompleteGenericTypeResolver completeResolver(*this, builder,
                                               allGenericParams);
  if (recursivelyVisitGenericParams) {
    visitOuterToInner(genericParams,
                      [&](GenericParamList *gpList) {
      checkGenericParamList(nullptr, gpList, nullptr, &completeResolver);
    });
  } else {
    checkGenericParamList(nullptr, genericParams, parentSig,
                          &completeResolver);
  }

  // Record the generic type parameter types and the requirements.
  auto sig = builder.getGenericSignature();

  // Debugging of the generic signature builder and generic signature generation.
  if (Context.LangOpts.DebugGenericSignatures) {
    dc->printContext(llvm::errs());
    llvm::errs() << "\n";
    builder.dump(llvm::errs());
    llvm::errs() << "Generic signature: ";
    sig->print(llvm::errs());
    llvm::errs() << "\n";
    llvm::errs() << "Canonical generic signature: ";
    sig->getCanonicalSignature()->print(llvm::errs());
    llvm::errs() << "\n";
  }

  // Form the generic environment.
  return sig->createGenericEnvironment(*module);
}

void TypeChecker::validateGenericTypeSignature(GenericTypeDecl *typeDecl) {
  auto *gp = typeDecl->getGenericParams();
  auto *dc = typeDecl->getDeclContext();

  if (!gp) {
    auto *parentEnv = dc->getGenericEnvironmentOfContext();
    typeDecl->setGenericEnvironment(parentEnv);
    return;
  }

  gp->setOuterParameters(dc->getGenericParamsOfContext());

  prepareGenericParamList(gp, typeDecl);

  // For a protocol, compute the requirement signature first. It will be used
  // by clients of the protocol.
  if (auto proto = dyn_cast<ProtocolDecl>(typeDecl)) {
    if (!proto->isRequirementSignatureComputed())
      proto->computeRequirementSignature();
  }

  auto *env = checkGenericEnvironment(gp, dc, dc->getGenericSignatureOfContext(),
                                      /*allowConcreteGenericParams=*/false);
  typeDecl->setGenericEnvironment(env);
}

///
/// Checking bound generic type arguments
///

RequirementCheckResult TypeChecker::checkGenericArguments(
    DeclContext *dc, SourceLoc loc, SourceLoc noteLoc, Type owner,
    GenericSignature *genericSig, TypeSubstitutionFn substitutions,
    LookupConformanceFn conformances,
    UnsatisfiedDependency *unsatisfiedDependency,
    ConformanceCheckOptions conformanceOptions,
    GenericRequirementsCheckListener *listener,
    SubstOptions options) {
  bool valid = true;

  for (const auto &rawReq : genericSig->getRequirements()) {
    auto req = rawReq.subst(substitutions, conformances, options);
    if (!req) {
      // Another requirement will fail later; just continue.
      valid = false;
      continue;
    }

    auto kind = req->getKind();
    Type rawFirstType = rawReq.getFirstType();
    Type firstType = req->getFirstType();
    Type rawSecondType, secondType;
    if (kind != RequirementKind::Layout) {
      rawSecondType = rawReq.getSecondType();
      secondType = req->getSecondType();
    }

    bool requirementFailure = false;
    if (listener && !listener->shouldCheck(kind, firstType, secondType))
      continue;

    Diag<Type, Type, Type> diagnostic;
    Diag<Type, Type, StringRef> diagnosticNote;

    switch (kind) {
    case RequirementKind::Conformance: {
      // Protocol conformance requirements.
      auto proto = secondType->castTo<ProtocolType>();
      // FIXME: This should track whether this should result in a private
      // or non-private dependency.
      // FIXME: Do we really need "used" at this point?
      // FIXME: Poor location information. How much better can we do here?
      // FIXME: This call should support listener to be able to properly
      //        diagnose problems with conformances.
      auto result =
          conformsToProtocol(firstType, proto->getDecl(), dc,
                             conformanceOptions, loc, unsatisfiedDependency);

      // Unsatisfied dependency case.
      auto status = result.getStatus();
      switch (status) {
      case RequirementCheckResult::UnsatisfiedDependency:
      case RequirementCheckResult::Failure:
      case RequirementCheckResult::SubstitutionFailure:
        // pass it on up.
        return status;
      case RequirementCheckResult::Success:
        // Report the conformance.
        if (listener) {
          listener->satisfiedConformance(rawReq.getFirstType(), firstType,
                                         result.getConformance());
        }
        continue;
      }
    }

    case RequirementKind::Layout: {
      // TODO: Statically check if a the first type
      // conforms to the layout constraint, once we
      // support such static checks.
      continue;
    }

    case RequirementKind::Superclass:
      // Superclass requirements.
      if (!isSubclassOf(firstType, secondType, dc)) {
        diagnostic = diag::type_does_not_inherit;
        diagnosticNote = diag::type_does_not_inherit_requirement;
        requirementFailure = true;
      }
      break;

    case RequirementKind::SameType:
      if (!firstType->isEqual(secondType)) {
        diagnostic = diag::types_not_equal;
        diagnosticNote = diag::types_not_equal_requirement;
        requirementFailure = true;
      }
      break;
    }

    if (!requirementFailure)
      continue;

    if (listener &&
        listener->diagnoseUnsatisfiedRequirement(rawReq, firstType, secondType))
      return RequirementCheckResult::Failure;

    if (loc.isValid()) {
      // FIXME: Poor source-location information.
      diagnose(loc, diagnostic, owner, firstType, secondType);
      diagnose(noteLoc, diagnosticNote, rawFirstType, rawSecondType,
               genericSig->gatherGenericParamBindingsText(
                   {rawFirstType, rawSecondType}, substitutions));
    }

    return RequirementCheckResult::Failure;
  }

  if (valid)
    return RequirementCheckResult::Success;
  return RequirementCheckResult::SubstitutionFailure;
}
