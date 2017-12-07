//===--- GenericEnvironment.cpp - GenericEnvironment AST ------------------===//
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
// This file implements the GenericEnvironment class.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/GenericSignatureBuilder.h"
#include "swift/AST/ProtocolConformance.h"

using namespace swift;

GenericEnvironment::GenericEnvironment(GenericSignature *signature,
                                       GenericSignatureBuilder *builder)
  : Signature(signature), Builder(builder)
{
  NumMappingsRecorded = 0;
  NumArchetypeToInterfaceMappings = 0;

  // Clear out the memory that holds the context types.
  std::uninitialized_fill(getContextTypes().begin(), getContextTypes().end(),
                          Type());
}

void GenericEnvironment::setOwningDeclContext(DeclContext *newOwningDC) {
  if (!OwningDC) {
    OwningDC = newOwningDC;
    return;
  }

  if (!newOwningDC || OwningDC == newOwningDC)
    return;

  // Find the least common ancestor context to be the owner.
  unsigned oldDepth = OwningDC->getSyntacticDepth();
  unsigned newDepth = newOwningDC->getSyntacticDepth();

  while (oldDepth > newDepth) {
    OwningDC = OwningDC->getParent();
    --oldDepth;
  }

  while (newDepth > oldDepth) {
    newOwningDC = newOwningDC->getParent();
    --newDepth;
  }

  while (OwningDC != newOwningDC) {
    OwningDC = OwningDC->getParent();
    newOwningDC = newOwningDC->getParent();
  }
}

void GenericEnvironment::addMapping(GenericParamKey key,
                                    Type contextType) {
  // Find the index into the parallel arrays of generic parameters and
  // context types.
  auto genericParams = Signature->getGenericParams();
  unsigned index = key.findIndexIn(genericParams);
  assert(genericParams[index] == key && "Bad generic parameter");

  // Add the mapping from the generic parameter to the context type.
  assert(getContextTypes()[index].isNull() && "Already recoded this mapping");
  getContextTypes()[index] = contextType;

  // If we mapped the generic parameter to an archetype, add it to the
  // reverse mapping.
  if (auto *archetype = contextType->getAs<ArchetypeType>()) {
    auto genericParam = genericParams[index];

    // Check whether we've already recorded a generic parameter for this
    // archetype. Note that we always perform a linear search, because we
    // won't have sorted the list yet.
    bool found = false;
    for (auto &mapping : getActiveArchetypeToInterfaceMappings()) {
      if (mapping.first != archetype) continue;

      // Multiple generic parameters map to the same archetype. If the
      // existing entry comes from a later generic parameter, replace it with
      // the earlier generic parameter. This gives us a deterministic reverse
      // mapping.
      auto otherGP = mapping.second->castTo<GenericTypeParamType>();
      if (GenericParamKey(genericParam) < GenericParamKey(otherGP))
        mapping.second = genericParam;
      found = true;
      break;
    }

    // If we haven't recorded a generic parameter for this archetype, do so now.
    if (!found) {
      void *ptr = getArchetypeToInterfaceMappingsBuffer().data()
                + NumArchetypeToInterfaceMappings;
      new (ptr) ArchetypeToInterfaceMapping(archetype, genericParam);
      ++NumArchetypeToInterfaceMappings;
    }
  }

  // Note that we've recorded this mapping.
  ++NumMappingsRecorded;

  // If we've recorded all of the mappings, go ahead and sort the array of
  // archetype-to-interface-type mappings.
  if (NumMappingsRecorded == genericParams.size()) {
    llvm::array_pod_sort(getActiveArchetypeToInterfaceMappings().begin(),
                         getActiveArchetypeToInterfaceMappings().end(),
                         [](const ArchetypeToInterfaceMapping *lhs,
                            const ArchetypeToInterfaceMapping *rhs) -> int {
                           std::less<ArchetypeType *> compare;
                           if (compare(lhs->first, rhs->first)) return -1;
                           if (compare(rhs->first, lhs->first)) return 1;
                           return 0;
                         });
  }
}

Optional<Type> GenericEnvironment::getMappingIfPresent(
                                                    GenericParamKey key) const {
  // Find the index into the parallel arrays of generic parameters and
  // context types.
  auto genericParams = Signature->getGenericParams();
  unsigned index = key.findIndexIn(genericParams);
  assert(genericParams[index] == key && "Bad generic parameter");

  if (auto type = getContextTypes()[index])
    return type;

  return None;
}

bool GenericEnvironment::containsPrimaryArchetype(
                                              ArchetypeType *archetype) const {
  return static_cast<bool>(
                       QueryArchetypeToInterfaceSubstitutions(this)(archetype));
}

Type GenericEnvironment::mapTypeIntoContext(GenericEnvironment *env,
                                            Type type) {
  assert(!type->hasArchetype() && "already have a contextual type");

  if (!env)
    return type.substDependentTypesWithErrorTypes();

  return env->mapTypeIntoContext(type);
}

Type
GenericEnvironment::mapTypeOutOfContext(GenericEnvironment *env,
                                        Type type) {
  assert(!type->hasTypeParameter() && "already have an interface type");

  if (!env)
    return type.substDependentTypesWithErrorTypes();

  return env->mapTypeOutOfContext(type);
}

Type GenericEnvironment::mapTypeOutOfContext(Type type) const {
  type = type.subst(QueryArchetypeToInterfaceSubstitutions(this),
                    MakeAbstractConformanceForGenericType(),
                    SubstFlags::AllowLoweredTypes);
  assert(!type->hasArchetype() && "not fully substituted");
  return type;
}

Type GenericEnvironment::QueryInterfaceTypeSubstitutions::operator()(
                                                SubstitutableType *type) const {
  if (auto gp = type->getAs<GenericTypeParamType>()) {
    // Find the index into the parallel arrays of generic parameters and
    // context types.
    auto genericParams = self->Signature->getGenericParams();
    GenericParamKey key(gp);

    // Make sure that this generic parameter is from this environment.
    unsigned index = key.findIndexIn(genericParams);
    if (index == genericParams.size() || genericParams[index] != key)
      return Type();

    // If the context type isn't already known, lazily create it.
    Type contextType = self->getContextTypes()[index];
    if (!contextType) {
      assert(self->Builder && "Missing generic signature builder for lazy query");
      auto potentialArchetype =
        self->Builder->resolveArchetype(
                                  type,
                                  ArchetypeResolutionKind::CompleteWellFormed);

      auto mutableSelf = const_cast<GenericEnvironment *>(self);
      contextType =
        potentialArchetype->getTypeInContext(*mutableSelf->Builder,
                                             mutableSelf);

      // FIXME: Redundant mapping from key -> index.
      if (self->getContextTypes()[index].isNull())
        mutableSelf->addMapping(key, contextType);
    }

    return contextType;
  }

  return Type();
}

Type GenericEnvironment::QueryArchetypeToInterfaceSubstitutions::operator()(
                                                SubstitutableType *type) const {
  auto archetype = type->getAs<ArchetypeType>();
  if (!archetype) return Type();

  // Only top-level archetypes need to be substituted directly; nested
  // archetypes will be handled via their root archetypes.
  if (archetype->getParent()) return Type();

  // If not all generic parameters have had their context types recorded,
  // perform a linear search.
  auto genericParams = self->Signature->getGenericParams();
  unsigned numGenericParams = genericParams.size();
  if (self->NumMappingsRecorded < numGenericParams) {
    // Search through all of the active archetype-to-interface mappings.
    for (auto &mapping : self->getActiveArchetypeToInterfaceMappings())
      if (mapping.first == archetype) return mapping.second;

    // We don't know if the archetype is from a different context or if we
    // simply haven't recorded it yet. Spin through all of the generic
    // parameters looking for one that provides this mapping.
    for (auto gp : genericParams) {
      // Map the generic parameter into our context. If we get back an
      // archetype that matches, we're done.
      auto gpArchetype = self->mapTypeIntoContext(gp)->getAs<ArchetypeType>();
      if (gpArchetype == archetype) return gp;
    }

    // We have checked all of the generic parameters and not found anything;
    // there is no substitution.
    return Type();
  }

  // All generic parameters have ad their context types recorded, which means
  // that the archetypes-to-interface-types array is sorted by address. Use a
  // binary search.
  struct MappingComparison {
    bool operator()(const ArchetypeToInterfaceMapping &lhs,
                    const ArchetypeType *rhs) const {
      std::less<const ArchetypeType *> compare;

      return compare(lhs.first, rhs);
    }

    bool operator()(const ArchetypeType *lhs,
                    const ArchetypeToInterfaceMapping &rhs) const {
      std::less<const ArchetypeType *> compare;

      return compare(lhs, rhs.first);
    }

    bool operator()(const ArchetypeToInterfaceMapping &lhs,
                    const ArchetypeToInterfaceMapping &rhs) const {
      std::less<const ArchetypeType *> compare;

      return compare(lhs.first, rhs.first);
    }
  } mappingComparison;

  auto mappings = self->getActiveArchetypeToInterfaceMappings();
  auto known = std::lower_bound(mappings.begin(), mappings.end(), archetype,
                                mappingComparison);
  if (known != mappings.end() && known->first == archetype)
    return known->second;

  return Type();
}

Type GenericEnvironment::mapTypeIntoContext(
                                Type type,
                                LookupConformanceFn lookupConformance) const {
  assert(!type->hasOpenedExistential() &&
         "Opened existentials are special and so are you");

  Type result = type.subst(QueryInterfaceTypeSubstitutions(this),
                           lookupConformance,
                           (SubstFlags::AllowLoweredTypes|
                            SubstFlags::UseErrorType));
  assert((!result->hasTypeParameter() || result->hasError()) &&
         "not fully substituted");
  return result;

}

Type GenericEnvironment::mapTypeIntoContext(Type type) const {
  auto *sig = getGenericSignature();
  return mapTypeIntoContext(type, LookUpConformanceInSignature(*sig));
}

Type GenericEnvironment::mapTypeIntoContext(GenericTypeParamType *type) const {
  auto self = const_cast<GenericEnvironment *>(this);
  Type result = QueryInterfaceTypeSubstitutions(self)(type);
  if (!result)
    return ErrorType::get(type);
  return result;
}

GenericTypeParamType *GenericEnvironment::getSugaredType(
    GenericTypeParamType *type) const {
  for (auto *sugaredType : getGenericParams())
    if (sugaredType->isEqual(type))
      return sugaredType;

  llvm_unreachable("missing generic parameter");
}

Type GenericEnvironment::getSugaredType(Type type) const {
  if (!type->hasTypeParameter())
    return type;

  return type.transform([this](Type Ty) -> Type {
    if (auto GP = dyn_cast<GenericTypeParamType>(Ty.getPointer())) {
      return Type(getSugaredType(GP));
    }
    return Ty;
  });
}

SubstitutionList
GenericEnvironment::getForwardingSubstitutions() const {
  auto *genericSig = getGenericSignature();

  SubstitutionMap subMap = genericSig->getSubstitutionMap(
    QueryInterfaceTypeSubstitutions(this),
    MakeAbstractConformanceForGenericType());

  SmallVector<Substitution, 4> result;
  genericSig->getSubstitutions(subMap, result);
  return genericSig->getASTContext().AllocateCopy(result);
}

SubstitutionMap
GenericEnvironment::
getSubstitutionMap(TypeSubstitutionFn subs,
                   LookupConformanceFn lookupConformance) const {
  SubstitutionMap subMap(const_cast<GenericEnvironment *>(this));

  getGenericSignature()->enumeratePairedRequirements(
    [&](Type depTy, ArrayRef<Requirement> reqs) -> bool {
      auto canTy = depTy->getCanonicalType();

      // Map the interface type to a context type.
      auto contextTy = depTy.subst(QueryInterfaceTypeSubstitutions(this),
                                   MakeAbstractConformanceForGenericType());

      // Compute the replacement type.
      Type currentReplacement = contextTy.subst(subs, lookupConformance,
                                                SubstFlags::UseErrorType);

      if (auto paramTy = dyn_cast<GenericTypeParamType>(canTy))
        subMap.addSubstitution(paramTy, currentReplacement);

      // Collect the conformances.
      for (auto req: reqs) {
        assert(req.getKind() == RequirementKind::Conformance);
        auto protoType = req.getSecondType()->castTo<ProtocolType>();
        auto conformance = lookupConformance(canTy,
                                             currentReplacement,
                                             protoType);
        if (conformance)
          subMap.addConformance(canTy, *conformance);
      }

      return false;
    });

  subMap.verify();
  return subMap;
}
