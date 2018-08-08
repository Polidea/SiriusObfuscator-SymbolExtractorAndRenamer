//===--- CSBindings.cpp - Constraint Solver -------------------------------===//
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
//
// This file implements selection of bindings for type variables.
//
//===----------------------------------------------------------------------===//
#include "ConstraintGraph.h"
#include "ConstraintSystem.h"
#include <tuple>

using namespace swift;
using namespace constraints;

Optional<ConstraintSystem::PotentialBindings>
ConstraintSystem::determineBestBindings() {
  // Look for potential type variable bindings.
  Optional<PotentialBindings> bestBindings;
  llvm::SmallDenseMap<TypeVariableType *, PotentialBindings> cache;

  // First, let's collect all of the possible bindings.
  for (auto *typeVar : getTypeVariables()) {
    if (typeVar->getImpl().hasRepresentativeOrFixed())
      continue;

    if (auto bindings = getPotentialBindings(typeVar))
      cache.insert({typeVar, std::move(bindings)});
  }

  // Now let's see if we could infer something for related type
  // variables based on other bindings.
  for (auto *typeVar : getTypeVariables()) {
    auto cachedBindings = cache.find(typeVar);
    if (cachedBindings == cache.end())
      continue;

    auto &bindings = cachedBindings->getSecond();
    // All of the relevant relational constraints associated with
    // current type variable should be recored by its potential bindings.
    for (auto *constraint : bindings.Sources) {
      if (constraint->getKind() != ConstraintKind::Subtype)
        continue;

      auto lhs = simplifyType(constraint->getFirstType());
      auto rhs = simplifyType(constraint->getSecondType());

      // We are only interested in 'subtype' constraints which have
      // type variable on the left-hand side.
      if (rhs->getAs<TypeVariableType>() != typeVar)
        continue;

      auto *tv = lhs->getAs<TypeVariableType>();
      if (!tv)
        continue;

      auto relatedBindings = cache.find(tv);
      if (relatedBindings == cache.end())
        continue;

      for (auto &binding : relatedBindings->getSecond().Bindings) {
        // We need the binding kind for the potential binding to
        // either be Exact or Supertypes in order for it to make sense
        // to add Supertype bindings based on the relationship between
        // our type variables.
        if (binding.Kind != AllowedBindingKind::Exact &&
            binding.Kind != AllowedBindingKind::Supertypes)
          continue;

        auto type = binding.BindingType;

        if (ConstraintSystem::typeVarOccursInType(typeVar, type))
          continue;

        bindings.addPotentialBinding(
            {type, AllowedBindingKind::Supertypes, binding.BindingSource});
      }
    }

    if (TC.getLangOpts().DebugConstraintSolver) {
      auto &log = getASTContext().TypeCheckerDebug->getStream();
      bindings.dump(typeVar, log, solverState->depth * 2);
    }

    // If these are the first bindings, or they are better than what
    // we saw before, use them instead.
    if (!bestBindings || bindings < *bestBindings)
      bestBindings = bindings;
  }

  return bestBindings;
}

/// Find the set of type variables that are inferable from the given type.
///
/// \param type The type to search.
/// \param typeVars Collects the type variables that are inferable from the
/// given type. This set is not cleared, so that multiple types can be explored
/// and introduce their results into the same set.
static void
findInferableTypeVars(Type type,
                      SmallPtrSetImpl<TypeVariableType *> &typeVars) {
  type = type->getCanonicalType();
  if (!type->hasTypeVariable())
    return;

  class Walker : public TypeWalker {
    SmallPtrSetImpl<TypeVariableType *> &typeVars;

  public:
    explicit Walker(SmallPtrSetImpl<TypeVariableType *> &typeVars)
        : typeVars(typeVars) {}

    Action walkToTypePre(Type ty) override {
      if (ty->is<DependentMemberType>())
        return Action::SkipChildren;

      if (auto typeVar = ty->getAs<TypeVariableType>())
        typeVars.insert(typeVar);
      return Action::Continue;
    }
  };

  type.walk(Walker(typeVars));
}

/// \brief Return whether a relational constraint between a type variable and a
/// trivial wrapper type (autoclosure, unary tuple) should result in the type
/// variable being potentially bound to the value type, as opposed to the
/// wrapper type.
static bool shouldBindToValueType(Constraint *constraint) {
  switch (constraint->getKind()) {
  case ConstraintKind::OperatorArgumentConversion:
  case ConstraintKind::OperatorArgumentTupleConversion:
  case ConstraintKind::ArgumentConversion:
  case ConstraintKind::ArgumentTupleConversion:
  case ConstraintKind::Conversion:
  case ConstraintKind::BridgingConversion:
  case ConstraintKind::Subtype:
    return true;
  case ConstraintKind::Bind:
  case ConstraintKind::Equal:
  case ConstraintKind::BindParam:
  case ConstraintKind::BindToPointerType:
  case ConstraintKind::ConformsTo:
  case ConstraintKind::LiteralConformsTo:
  case ConstraintKind::CheckedCast:
  case ConstraintKind::SelfObjectOfProtocol:
  case ConstraintKind::ApplicableFunction:
  case ConstraintKind::BindOverload:
  case ConstraintKind::OptionalObject:
    return false;
  case ConstraintKind::DynamicTypeOf:
  case ConstraintKind::EscapableFunctionOf:
  case ConstraintKind::OpenedExistentialOf:
  case ConstraintKind::KeyPath:
  case ConstraintKind::KeyPathApplication:
  case ConstraintKind::ValueMember:
  case ConstraintKind::UnresolvedValueMember:
  case ConstraintKind::Defaultable:
  case ConstraintKind::Disjunction:
    llvm_unreachable("shouldBindToValueType() may only be called on "
                     "relational constraints");
  }

  llvm_unreachable("Unhandled ConstraintKind in switch.");
}

void ConstraintSystem::PotentialBindings::addPotentialBinding(
    PotentialBinding binding, bool allowJoinMeet) {
  assert(!binding.BindingType->is<ErrorType>());

  // If this is a non-defaulted supertype binding,
  // check whether we can combine it with another
  // supertype binding by computing the 'join' of the types.
  if (binding.Kind == AllowedBindingKind::Supertypes &&
      !binding.BindingType->hasTypeVariable() &&
      !binding.BindingType->hasUnboundGenericType() &&
      !binding.DefaultedProtocol && !binding.isDefaultableBinding() &&
      allowJoinMeet) {
    if (lastSupertypeIndex) {
      auto &lastBinding = Bindings[*lastSupertypeIndex];
      auto lastType = lastBinding.BindingType->getWithoutSpecifierType();
      auto bindingType = binding.BindingType->getWithoutSpecifierType();

      auto join = Type::join(lastType, bindingType);
      if (join && !(*join)->isAny() &&
          (!(*join)->getOptionalObjectType()
           || !(*join)->getOptionalObjectType()->isAny())) {
        // Replace the last supertype binding with the join. We're done.
        lastBinding.BindingType = *join;
        return;
      }
    }

    // Record this as the most recent supertype index.
    lastSupertypeIndex = Bindings.size();
  }

  if (auto *literalProtocol = binding.DefaultedProtocol)
    foundLiteralBinding(literalProtocol);

  Bindings.push_back(std::move(binding));
}

Optional<ConstraintSystem::PotentialBinding>
ConstraintSystem::getPotentialBindingForRelationalConstraint(
    PotentialBindings &result, Constraint *constraint,
    bool &hasDependentMemberRelationalConstraints,
    bool &hasNonDependentMemberRelationalConstraints,
    bool &addOptionalSupertypeBindings) {
  assert(constraint->getClassification() ==
             ConstraintClassification::Relational &&
         "only relational constraints handled here");

  auto *typeVar = result.TypeVar;

  // Record constraint which contributes to the
  // finding of potential bindings.
  result.Sources.insert(constraint);

  auto first = simplifyType(constraint->getFirstType());
  auto second = simplifyType(constraint->getSecondType());

  if (first->is<TypeVariableType>() && first->isEqual(second))
    return None;

  Type type;
  AllowedBindingKind kind;
  if (first->getAs<TypeVariableType>() == typeVar) {
    // Upper bound for this type variable.
    type = second;
    kind = AllowedBindingKind::Subtypes;
  } else if (second->getAs<TypeVariableType>() == typeVar) {
    // Lower bound for this type variable.
    type = first;
    kind = AllowedBindingKind::Supertypes;
  } else {
    // Can't infer anything.
    if (result.InvolvesTypeVariables)
      return None;

    // Check whether both this type and another type variable are
    // inferable.
    SmallPtrSet<TypeVariableType *, 4> typeVars;
    findInferableTypeVars(first, typeVars);
    findInferableTypeVars(second, typeVars);
    if (typeVars.size() > 1 && typeVars.count(typeVar))
      result.InvolvesTypeVariables = true;
    return None;
  }

  // Do not attempt to bind to ErrorType.
  if (type->hasError())
    return None;

  // Don't deduce autoclosure types or single-element, non-variadic
  // tuples.
  if (shouldBindToValueType(constraint)) {
    if (auto funcTy = type->getAs<FunctionType>()) {
      if (funcTy->isAutoClosure())
        type = funcTy->getResult();
    }

    type = type->getWithoutImmediateLabel();
  }

  // If the source of the binding is 'OptionalObject' constraint
  // and type variable is on the left-hand side, that means
  // that it _has_ to be of optional type, since the right-hand
  // side of the constraint is object type of the optional.
  if (constraint->getKind() == ConstraintKind::OptionalObject &&
      kind == AllowedBindingKind::Subtypes) {
    type = OptionalType::get(type);
  }

  // If the type we'd be binding to is a dependent member, don't try to
  // resolve this type variable yet.
  if (type->is<DependentMemberType>()) {
    if (!ConstraintSystem::typeVarOccursInType(typeVar, type,
                                               &result.InvolvesTypeVariables)) {
      hasDependentMemberRelationalConstraints = true;
    }
    return None;
  }
  hasNonDependentMemberRelationalConstraints = true;

  // If our binding choice is a function type and we're attempting
  // to bind to a type variable that is the result of opening a
  // generic parameter, strip the noescape bit so that we only allow
  // bindings of escaping functions in this position. We do this
  // because within the generic function we have no indication of
  // whether the parameter is a function type and if so whether it
  // should be allowed to escape. As a result we allow anything
  // passed in to escape.
  if (auto *fnTy = type->getAs<AnyFunctionType>())
    if (typeVar->getImpl().getArchetype() && !shouldAttemptFixes())
      type = fnTy->withExtInfo(fnTy->getExtInfo().withNoEscape(false));

  // Check whether we can perform this binding.
  // FIXME: this has a super-inefficient extraneous simplifyType() in it.
  bool isNilLiteral = false;
  bool *isNilLiteralPtr = nullptr;
  if (!addOptionalSupertypeBindings && kind == AllowedBindingKind::Supertypes)
    isNilLiteralPtr = &isNilLiteral;
  if (auto boundType = checkTypeOfBinding(typeVar, type, isNilLiteralPtr)) {
    type = *boundType;
    if (type->hasTypeVariable())
      result.InvolvesTypeVariables = true;
  } else {
    // If the bound is a 'nil' literal type, add optional supertype bindings.
    if (isNilLiteral) {
      addOptionalSupertypeBindings = true;
      return None;
    }

    result.InvolvesTypeVariables = true;
    return None;
  }

  // Make sure we aren't trying to equate type variables with different
  // lvalue-binding rules.
  if (auto otherTypeVar =
          type->lookThroughAllOptionalTypes()->getAs<TypeVariableType>()) {
    if (typeVar->getImpl().canBindToLValue() !=
        otherTypeVar->getImpl().canBindToLValue())
      return None;
  }

  // BindParam constraints are not reflexive and must be treated specially.
  if (constraint->getKind() == ConstraintKind::BindParam) {
    if (kind == AllowedBindingKind::Subtypes) {
      if (auto *lvt = type->getAs<LValueType>()) {
        type = InOutType::get(lvt->getObjectType());
      }
    } else if (kind == AllowedBindingKind::Supertypes) {
      if (auto *iot = type->getAs<InOutType>()) {
        type = LValueType::get(iot->getObjectType());
      }
    }
    kind = AllowedBindingKind::Exact;
  }

  return PotentialBinding{type, kind, constraint->getKind()};
}

/// \brief Retrieve the set of potential type bindings for the given
/// representative type variable, along with flags indicating whether
/// those types should be opened.
ConstraintSystem::PotentialBindings
ConstraintSystem::getPotentialBindings(TypeVariableType *typeVar) {
  assert(typeVar->getImpl().getRepresentative(nullptr) == typeVar &&
         "not a representative");
  assert(!typeVar->getImpl().getFixedType(nullptr) && "has a fixed type");

  // Gather the constraints associated with this type variable.
  SmallVector<Constraint *, 8> constraints;
  llvm::SmallPtrSet<Constraint *, 4> visitedConstraints;
  getConstraintGraph().gatherConstraints(
      typeVar, constraints, ConstraintGraph::GatheringKind::EquivalenceClass);

  PotentialBindings result(typeVar);

  // Consider each of the constraints related to this type variable.
  llvm::SmallPtrSet<CanType, 4> exactTypes;
  SmallVector<Constraint *, 2> defaultableConstraints;
  SmallVector<PotentialBinding, 4> literalBindings;
  bool addOptionalSupertypeBindings = false;
  auto &tc = getTypeChecker();
  bool hasNonDependentMemberRelationalConstraints = false;
  bool hasDependentMemberRelationalConstraints = false;
  for (auto constraint : constraints) {
    // Only visit each constraint once.
    if (!visitedConstraints.insert(constraint).second)
      continue;

    switch (constraint->getKind()) {
    case ConstraintKind::Bind:
    case ConstraintKind::Equal:
    case ConstraintKind::BindParam:
    case ConstraintKind::BindToPointerType:
    case ConstraintKind::Subtype:
    case ConstraintKind::Conversion:
    case ConstraintKind::ArgumentConversion:
    case ConstraintKind::ArgumentTupleConversion:
    case ConstraintKind::OperatorArgumentTupleConversion:
    case ConstraintKind::OperatorArgumentConversion:
    case ConstraintKind::OptionalObject: {
      auto binding = getPotentialBindingForRelationalConstraint(
          result, constraint, hasDependentMemberRelationalConstraints,
          hasNonDependentMemberRelationalConstraints,
          addOptionalSupertypeBindings);
      if (!binding)
        break;

      auto type = binding->BindingType;
      if (exactTypes.insert(type->getCanonicalType()).second) {
        result.addPotentialBinding(*binding);

        if (auto *locator = typeVar->getImpl().getLocator()) {
          auto path = locator->getPath();
          auto voidType = getASTContext().TheEmptyTupleType;

          // If this is a type variable representing closure result,
          // which is on the right-side of some relational constraint
          // let's have it try `Void` as well because there is an
          // implicit conversion `() -> T` to `() -> Void` and this
          // helps to avoid creating a thunk to support it.
          if (!path.empty() &&
              path.back().getKind() == ConstraintLocator::ClosureResult &&
              binding->Kind == AllowedBindingKind::Supertypes &&
              exactTypes.insert(voidType).second) {
            result.addPotentialBinding(
                {voidType, binding->Kind, constraint->getKind()},
                /*allowJoinMeet=*/false);
          }
        }
      }
      break;
    }

    case ConstraintKind::BridgingConversion:
    case ConstraintKind::CheckedCast:
    case ConstraintKind::EscapableFunctionOf:
    case ConstraintKind::OpenedExistentialOf:
    case ConstraintKind::KeyPath:
    case ConstraintKind::KeyPathApplication:
      // Constraints from which we can't do anything.
      break;

    case ConstraintKind::DynamicTypeOf: {
      // Direct binding of the left-hand side could result
      // in `DynamicTypeOf` failure if right-hand side is
      // bound (because 'Bind' requires equal types to
      // succeed), or left is bound to Any which is not an
      // [existential] metatype.
      auto dynamicType = constraint->getFirstType();
      if (auto *tv = dynamicType->getAs<TypeVariableType>()) {
        if (tv->getImpl().getRepresentative(nullptr) == typeVar)
          return {typeVar};
      }

      // This is right-hand side, let's continue.
      break;
    }

    case ConstraintKind::Defaultable:
      // Do these in a separate pass.
      if (getFixedTypeRecursive(constraint->getFirstType(), true)
              ->getAs<TypeVariableType>() == typeVar) {
        defaultableConstraints.push_back(constraint);
        hasNonDependentMemberRelationalConstraints = true;
      }
      break;

    case ConstraintKind::Disjunction:
      // FIXME: Recurse into these constraints to see whether this
      // type variable is fully bound by any of them.
      result.InvolvesTypeVariables = true;
      break;

    case ConstraintKind::ConformsTo:
    case ConstraintKind::SelfObjectOfProtocol:
      // Swift 3 allowed the use of default types for normal conformances
      // to expressible-by-literal protocols.
      if (tc.Context.LangOpts.EffectiveLanguageVersion[0] >= 4)
        continue;

      if (!constraint->getSecondType()->is<ProtocolType>())
        continue;

      LLVM_FALLTHROUGH;

    case ConstraintKind::LiteralConformsTo: {
      // If there is a 'nil' literal constraint, we might need optional
      // supertype bindings.
      if (constraint->getProtocol()->isSpecificProtocol(
              KnownProtocolKind::ExpressibleByNilLiteral))
        addOptionalSupertypeBindings = true;

      // If there is a default literal type for this protocol, it's a
      // potential binding.
      auto defaultType = tc.getDefaultType(constraint->getProtocol(), DC);
      if (!defaultType)
        continue;

      hasNonDependentMemberRelationalConstraints = true;

      // Handle unspecialized types directly.
      if (!defaultType->hasUnboundGenericType()) {
        if (!exactTypes.insert(defaultType->getCanonicalType()).second)
          continue;

        literalBindings.push_back({defaultType, AllowedBindingKind::Subtypes,
                                   constraint->getKind(),
                                   constraint->getProtocol()});
        continue;
      }

      // For generic literal types, check whether we already have a
      // specialization of this generic within our list.
      // FIXME: This assumes that, e.g., the default literal
      // int/float/char/string types are never generic.
      auto nominal = defaultType->getAnyNominal();
      if (!nominal)
        continue;

      bool matched = false;
      for (auto exactType : exactTypes) {
        if (auto exactNominal = exactType->getAnyNominal()) {
          // FIXME: Check parents?
          if (nominal == exactNominal) {
            matched = true;
            break;
          }
        }
      }

      if (!matched) {
        exactTypes.insert(defaultType->getCanonicalType());
        literalBindings.push_back({defaultType, AllowedBindingKind::Subtypes,
                                   constraint->getKind(),
                                   constraint->getProtocol()});
      }

      break;
    }

    case ConstraintKind::ApplicableFunction:
    case ConstraintKind::BindOverload: {
      if (result.FullyBound && result.InvolvesTypeVariables)
        continue;

      // If this variable is in the left-hand side, it is fully bound.
      SmallPtrSet<TypeVariableType *, 4> typeVars;
      findInferableTypeVars(simplifyType(constraint->getFirstType()), typeVars);
      if (typeVars.count(typeVar))
        result.FullyBound = true;

      if (result.InvolvesTypeVariables)
        continue;

      // If this and another type variable occur, this result involves
      // type variables.
      findInferableTypeVars(simplifyType(constraint->getSecondType()),
                            typeVars);
      if (typeVars.size() > 1 && typeVars.count(typeVar))
        result.InvolvesTypeVariables = true;

      break;
    }

    case ConstraintKind::ValueMember:
    case ConstraintKind::UnresolvedValueMember:
      // If our type variable shows up in the base type, there's
      // nothing to do.
      // FIXME: Can we avoid simplification here?
      if (ConstraintSystem::typeVarOccursInType(
              typeVar, simplifyType(constraint->getFirstType()),
              &result.InvolvesTypeVariables)) {
        continue;
      }

      // If the type variable is in the list of member type
      // variables, it is fully bound.
      // FIXME: Can we avoid simplification here?
      if (ConstraintSystem::typeVarOccursInType(
              typeVar, simplifyType(constraint->getSecondType()),
              &result.InvolvesTypeVariables)) {
        result.FullyBound = true;
      }
      break;
    }
  }

  // If we have any literal constraints, check whether there is already a
  // binding that provides a type that conforms to that literal protocol. In
  // such cases, remove the default binding suggestion because the existing
  // suggestion is better.
  if (!literalBindings.empty()) {
    SmallPtrSet<ProtocolDecl *, 5> coveredLiteralProtocols;
    for (auto &binding : result.Bindings) {
      Type testType;

      switch (binding.Kind) {
      case AllowedBindingKind::Exact:
        testType = binding.BindingType;
        break;

      case AllowedBindingKind::Subtypes:
      case AllowedBindingKind::Supertypes:
        testType = binding.BindingType->getRValueType();
        break;
      }

      // Attempting to check conformance of the type variable,
      // or unresolved type is invalid since it would result
      // in lose of viable literal bindings because that check
      // always returns trivial conformance.
      if (testType->isTypeVariableOrMember() || testType->is<UnresolvedType>())
        continue;

      // Check each non-covered literal protocol to determine which ones
      // might be covered by non-defaulted bindings.
      bool updatedBindingType = false;
      for (auto &literalBinding : literalBindings) {
        auto *protocol = literalBinding.DefaultedProtocol;

        assert(protocol);

        // Has already been covered by one of the bindings.
        if (coveredLiteralProtocols.count(protocol))
          continue;

        do {
          // If the type conforms to this protocol, we're covered.
          if (tc.conformsToProtocol(
                  testType, protocol, DC,
                  (ConformanceCheckFlags::InExpression |
                   ConformanceCheckFlags::SkipConditionalRequirements))) {
            coveredLiteralProtocols.insert(protocol);
            break;
          }

          // If we're allowed to bind to subtypes, look through optionals.
          // FIXME: This is really crappy special case of computing a reasonable
          // result based on the given constraints.
          if (binding.Kind == AllowedBindingKind::Subtypes) {
            if (auto objTy = testType->getOptionalObjectType()) {
              updatedBindingType = true;
              testType = objTy;
              continue;
            }
          }

          updatedBindingType = false;
          break;
        } while (true);
      }

      if (updatedBindingType)
        binding.BindingType = testType;
    }

    for (auto &literalBinding : literalBindings) {
      auto *protocol = literalBinding.DefaultedProtocol;
      // For any literal type that has been covered, skip them.
      if (coveredLiteralProtocols.count(protocol) == 0)
        result.addPotentialBinding(std::move(literalBinding));
    }
  }

  /// Add defaultable constraints last.
  for (auto constraint : defaultableConstraints) {
    Type type = constraint->getSecondType();
    if (!exactTypes.insert(type->getCanonicalType()).second)
      continue;

    ++result.NumDefaultableBindings;
    result.addPotentialBinding({type, AllowedBindingKind::Exact,
                                constraint->getKind(), nullptr,
                                constraint->getLocator()});
  }

  // Determine if the bindings only constrain the type variable from above with
  // an existential type; such a binding is not very helpful because it's
  // impossible to enumerate the existential type's subtypes.
  result.SubtypeOfExistentialType =
      std::all_of(result.Bindings.begin(), result.Bindings.end(),
                  [](const PotentialBinding &binding) {
                    return binding.BindingType->isExistentialType() &&
                           binding.Kind == AllowedBindingKind::Subtypes;
                  });

  // If we're supposed to add optional supertype bindings, do so now.
  if (addOptionalSupertypeBindings) {
    for (unsigned i : indices(result.Bindings)) {
      auto &binding = result.Bindings[i];
      bool wrapInOptional = false;

      if (binding.Kind == AllowedBindingKind::Supertypes) {
        // If the type doesn't conform to ExpressibleByNilLiteral,
        // produce an optional of that type as a potential binding. We
        // overwrite the binding in place because the non-optional type
        // will fail to type-check against the nil-literal conformance.
        auto nominalBindingDecl =
            binding.BindingType->getRValueType()->getAnyNominal();
        bool conformsToExprByNilLiteral = false;
        if (nominalBindingDecl) {
          SmallVector<ProtocolConformance *, 2> conformances;
          conformsToExprByNilLiteral = nominalBindingDecl->lookupConformance(
              DC->getParentModule(),
              getASTContext().getProtocol(
                  KnownProtocolKind::ExpressibleByNilLiteral),
              conformances);
        }
        wrapInOptional = !conformsToExprByNilLiteral;
      } else if (binding.isDefaultableBinding() &&
                 binding.BindingType->isAny()) {
        wrapInOptional = true;
      }

      if (wrapInOptional) {
        binding.BindingType = OptionalType::get(binding.BindingType);
      }
    }
  }

  // If there were both dependent-member and non-dependent-member relational
  // constraints, consider this "fully bound"; we don't want to touch it.
  if (hasDependentMemberRelationalConstraints) {
    if (hasNonDependentMemberRelationalConstraints)
      result.FullyBound = true;
    else
      result.Bindings.clear();
  }

  return result;
}
