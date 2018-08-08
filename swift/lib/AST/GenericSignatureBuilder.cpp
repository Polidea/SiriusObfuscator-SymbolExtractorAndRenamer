//===--- GenericSignatureBuilder.cpp - Generic Requirement Builder --------===//
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
// Support for collecting a set of generic requirements, both explicitly stated
// and inferred, and computing the archetypes and required witness tables from
// those requirements.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/GenericSignatureBuilder.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeMatcher.h"
#include "swift/AST/TypeRepr.h"
#include "swift/AST/TypeWalker.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Statistic.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SaveAndRestore.h"
#include <algorithm>

using namespace swift;
using llvm::DenseMap;

/// Define this to 1 to enable expensive assertions.
#define SWIFT_GSB_EXPENSIVE_ASSERTIONS 0

namespace {
  typedef GenericSignatureBuilder::RequirementSource RequirementSource;
  typedef GenericSignatureBuilder::FloatingRequirementSource
    FloatingRequirementSource;
  typedef GenericSignatureBuilder::ConstraintResult ConstraintResult;
  typedef GenericSignatureBuilder::PotentialArchetype PotentialArchetype;
  typedef GenericSignatureBuilder::ConcreteConstraint ConcreteConstraint;
  template<typename T> using Constraint =
    GenericSignatureBuilder::Constraint<T>;
  typedef GenericSignatureBuilder::EquivalenceClass EquivalenceClass;
  typedef EquivalenceClass::DerivedSameTypeComponent DerivedSameTypeComponent;
  typedef GenericSignatureBuilder::DelayedRequirement DelayedRequirement;
  typedef GenericSignatureBuilder::ResolvedType ResolvedType;
  typedef GenericSignatureBuilder::UnresolvedType GSBUnresolvedType;
  typedef GenericSignatureBuilder::RequirementRHS RequirementRHS;
} // end anonymous namespace

namespace llvm {
  // Equivalence classes are bump-ptr-allocated.
  template <> struct ilist_alloc_traits<EquivalenceClass> {
    static void deleteNode(EquivalenceClass *ptr) { ptr->~EquivalenceClass(); }
  };
}

#define DEBUG_TYPE "Generic signature builder"
STATISTIC(NumPotentialArchetypes, "# of potential archetypes");
STATISTIC(NumConformances, "# of conformances tracked");
STATISTIC(NumConformanceConstraints, "# of conformance constraints tracked");
STATISTIC(NumSameTypeConstraints, "# of same-type constraints tracked");
STATISTIC(NumConcreteTypeConstraints,
          "# of same-type-to-concrete constraints tracked");
STATISTIC(NumSuperclassConstraints, "# of superclass constraints tracked");
STATISTIC(NumSuperclassConstraintsExtra,
          "# of superclass constraints that add no information");
STATISTIC(NumLayoutConstraints, "# of layout constraints tracked");
STATISTIC(NumLayoutConstraintsExtra,
          "# of layout constraints  that add no information");
STATISTIC(NumSelfDerived, "# of self-derived constraints removed");
STATISTIC(NumArchetypeAnchorCacheHits,
          "# of hits in the archetype anchor cache");
STATISTIC(NumArchetypeAnchorCacheMisses,
          "# of misses in the archetype anchor cache");
STATISTIC(NumNestedTypeCacheHits,
         "# of hits in the equivalence class nested type cache");
STATISTIC(NumNestedTypeCacheMisses,
         "# of misses in the equivalence class nested type cache");
STATISTIC(NumProcessDelayedRequirements,
          "# of times we process delayed requirements");
STATISTIC(NumProcessDelayedRequirementsUnchanged,
          "# of times we process delayed requirements without change");
STATISTIC(NumDelayedRequirementConcrete,
          "Delayed requirements resolved as concrete");
STATISTIC(NumDelayedRequirementResolved,
          "Delayed requirements resolved");
STATISTIC(NumDelayedRequirementUnresolved,
          "Delayed requirements left unresolved");
STATISTIC(NumConditionalRequirementsAdded,
          "# of conditional requirements added");
STATISTIC(NumRewriteMinimizations,
          "# of rewrite system minimizations performed");
STATISTIC(NumRewriteRhsSimplified,
          "# of rewrite rule right-hand sides simplified");
STATISTIC(NumRewriteRhsSimplifiedToLhs,
          "# of rewrite rule right-hand sides simplified to lhs (and removed)");
STATISTIC(NumRewriteRulesRedundant,
          "# of rewrite rules that are redundant (and removed)");

namespace  {

/// A purely-relative rewrite path consisting of a (possibly empty)
/// sequence of associated type references.
using RelativeRewritePath = ArrayRef<AssociatedTypeDecl *>;

class AnchorPathCache;

/// Describes a rewrite path, which contains an optional base (generic
/// parameter) followed by a sequence of associated type references.
class RewritePath {
  Optional<GenericParamKey> base;
  TinyPtrVector<AssociatedTypeDecl *> path;

public:
  RewritePath() { }

  enum PathOrder {
    Forward,
    Reverse,
  };

  /// Form a rewrite path given an optional base and a relative rewrite path.
  RewritePath(Optional<GenericParamKey> base, RelativeRewritePath path,
              PathOrder order);

  /// Retrieve the base of the given rewrite path.
  ///
  /// When present, it indicates that the entire path will be rebased on
  /// the given base generic parameter. This is required for describing
  /// rewrites on type parameters themselves, e.g., T == U.
  ///
  /// When absent, the path is relative to the root of the tree from which
  /// the search began.
  Optional<GenericParamKey> getBase() const { return base; }

  /// Retrieve the sequence of associated type references that describes
  /// the path.
  ArrayRef<AssociatedTypeDecl *> getPath() const { return path; }

  /// Whether this path is completely empty.
  bool isEmpty() const { return getBase() == None && getPath().empty(); }

  /// whether this describes a valid path.
  explicit operator bool() const { return !isEmpty(); }

  /// Decompose a type into a path.
  ///
  /// \returns the path, or None if it contained unresolved dependent member
  /// types.
  RewritePath static createPath(Type type);

  /// Decompose a type into a path.
  ///
  /// \param path Will be filled in with the components of the path, in
  /// reverse order.
  ///
  /// \returns the generic parameter at the start of the path.
  static GenericParamKey createPath(
                                Type type,
                                SmallVectorImpl<AssociatedTypeDecl *> &path);

  /// Compute the longer common prefix between this path and \c other.
  RewritePath commonPath(const RewritePath &other) const;

  /// Form a canonical, dependent type.
  ///
  /// This requires that either the rewrite path have a base, or the
  /// \c baseEquivClass to be non-null (which substitutes in a base).
  CanType formDependentType(ASTContext &ctx,
                            AnchorPathCache *anchorPathCache = nullptr) const;

  /// Compare the given rewrite paths.
  int compare(const RewritePath &other) const;

  /// Print this path.
  void print(llvm::raw_ostream &out) const;

  LLVM_ATTRIBUTE_DEPRECATED(void dump() const LLVM_ATTRIBUTE_USED,
                            "only for use within the debugger") {
    print(llvm::errs());
  }

  friend bool operator==(const RewritePath &lhs, const RewritePath &rhs) {
    return lhs.getBase() == rhs.getBase() && lhs.getPath() == rhs.getPath();
  }
};

/// A cache that lazily computes the anchor path for the given equivalence
/// class.
class AnchorPathCache {
  GenericSignatureBuilder &builder;
  EquivalenceClass &equivClass;
  Optional<RewritePath> anchorPath;

public:
  AnchorPathCache(GenericSignatureBuilder &builder,
                  EquivalenceClass &equivClass)
    : builder(builder), equivClass(equivClass) { }

  const RewritePath &getAnchorPath() {
    if (anchorPath) return *anchorPath;

    anchorPath = RewritePath::createPath(equivClass.getAnchor(builder, { }));
    return *anchorPath;
  }
};

/// A node within the prefix tree that is used to match associated type
/// references.
class RewriteTreeNode {
  /// The associated type that leads to this node.
  ///
  /// The bit indicates whether there is a rewrite rule for this particular
  /// node. If the bit is not set, \c rewrite is invalid.
  llvm::PointerIntPair<AssociatedTypeDecl *, 1, bool> assocTypeAndHasRewrite;

  /// The sequence of associated types to which a reference to this associated
  /// type (from the equivalence class root) can be rewritten. This field is
  /// only valid when the bit of \c assocTypeAndHasRewrite is set.
  ///
  /// Consider a requirement "Self.A.B.C == C". This will be encoded as
  /// a prefix tree starting at the equivalence class for Self with
  /// the following nodes:
  ///
  /// (assocType: A,
  ///   children: [
  ///     (assocType: B,
  ///       children: [
  ///         (assocType: C, rewrite: [C], children: [])
  ///       ])
  ///   ])
  RewritePath rewrite;

  /// The child nodes, which extend the sequence to be matched.
  ///
  /// The child nodes are sorted by the associated type declaration
  /// pointers, so we can perform binary searches quickly.
  llvm::TinyPtrVector<RewriteTreeNode *> children;

public:
  ~RewriteTreeNode();

  RewriteTreeNode(AssociatedTypeDecl *assocType)
    : assocTypeAndHasRewrite(assocType, false) { }

  /// Retrieve the associated type declaration one must match to use this
  /// node, which may the
  AssociatedTypeDecl *getMatch() const {
    return assocTypeAndHasRewrite.getPointer();
  }

  /// Determine whether this particular node has a rewrite rule.
  bool hasRewriteRule() const {
    return assocTypeAndHasRewrite.getInt();
  }

  /// Set a new rewrite rule for this particular node. This can only be
  /// performed once.
  void setRewriteRule(RewritePath replacementPath) {
    assert(!hasRewriteRule());
    assocTypeAndHasRewrite.setInt(true);
    rewrite = replacementPath;
  }

  /// Remove the rewrite rule.
  void removeRewriteRule() {
    assert(hasRewriteRule());
    assocTypeAndHasRewrite.setInt(false);
  }

  /// Retrieve the path to which this node will be rewritten.
  const RewritePath &getRewriteRule() const & {
    assert(hasRewriteRule());
    return rewrite;
  }

  /// Retrieve the path to which this node will be rewritten.
  RewritePath &&getRewriteRule() && {
    assert(hasRewriteRule());
    return std::move(rewrite);
  }

  /// Add a new rewrite rule to this tree node.
  ///
  /// \param matchPath The path of associated type declarations that must
  /// be matched to produce a rewrite.
  ///
  /// \param replacementPath The sequence of associated type declarations
  /// with which a match will be replaced.
  ///
  /// \returns true if a rewrite rule was added, and false if it already
  /// existed.
  bool addRewriteRule(RelativeRewritePath matchPath,
                      RewritePath replacementPath);

  /// Enumerate all of the paths to which the given matched path can be
  /// rewritten.
  ///
  /// \param matchPath The path to match.
  ///
  /// \param callback A callback that will be invoked with (prefix, rewrite)
  /// pairs, where \c prefix is the length of the matching prefix of
  /// \c matchPath that matched and \c rewrite is the path to which it can
  /// be rewritten.
  void enumerateRewritePaths(
               RelativeRewritePath matchPath,
               llvm::function_ref<void(unsigned, RewritePath)> callback) const {
    return enumerateRewritePathsImpl(matchPath, callback, /*depth=*/0);
  }

private:
  void enumerateRewritePathsImpl(
               RelativeRewritePath matchPath,
               llvm::function_ref<void(unsigned, RewritePath)> callback,
               unsigned depth) const;

public:

  /// Find the best rewrite rule to match the given path.
  ///
  /// \param path The path to match.
  /// \param prefixLength The length of the prefix leading up to \c path.
  Optional<std::pair<unsigned, RewritePath>>
  bestRewritePath(GenericParamKey base, RelativeRewritePath path,
            unsigned prefixLength);

  /// Merge the given rewrite tree into \c other.
  ///
  /// \returns true if any rules were created by this merge.
  bool mergeInto(RewriteTreeNode *other);

  /// An action to perform for the given rule
  class RuleAction {
    enum Kind {
      /// No action; continue traversal.
      None,

      /// Stop traversal.
      Stop,

      /// Remove the given rule completely.
      Remove,

      /// Replace the right-hand side of the rule with the given new path.
      Replace,
    } kind;

    RewritePath path;

    RuleAction(Kind kind, RewritePath path = {})
      : kind(kind), path(path) { }

    friend class RewriteTreeNode;

  public:
    static RuleAction none() { return RuleAction(None); }
    static RuleAction stop() { return RuleAction(Stop); }
    static RuleAction remove() { return RuleAction(Remove); }

    static RuleAction replace(RewritePath path) {
      return RuleAction(Replace, std::move(path));
    }

    operator Kind() const { return kind; }
  };

  /// Callback function for enumerating rules in a tree.
  using EnumerateCallback =
    RuleAction(RelativeRewritePath lhs, const RewritePath &rhs);

  /// Enumerate all of the rewrite rules, calling \c fn with the left and
  /// right-hand sides of each rule.
  ///
  /// \returns true if the action function returned \c Stop at any point.
  bool enumerateRules(llvm::function_ref<EnumerateCallback> fn,
                      bool temporarilyDisableVisitedRule = false) {
    SmallVector<AssociatedTypeDecl *, 4> lhs;
    return enumerateRulesRec(fn, temporarilyDisableVisitedRule, lhs);
  }

  LLVM_ATTRIBUTE_DEPRECATED(void dump() const LLVM_ATTRIBUTE_USED,
                            "only for use within the debugger");

  /// Dump the tree.
  void dump(llvm::raw_ostream &out, bool lastChild = true) const;

private:
  /// Enumerate all of the rewrite rules, calling \c fn with the left and
  /// right-hand sides of each rule.
  ///
  /// \returns true if the action function returned \c Stop at any point.
  bool enumerateRulesRec(llvm::function_ref<EnumerateCallback> &fn,
                         bool temporarilyDisableVisitedRule,
                         llvm::SmallVectorImpl<AssociatedTypeDecl *> &lhs);
};
}

struct GenericSignatureBuilder::Implementation {
  /// Allocator.
  llvm::BumpPtrAllocator Allocator;

  /// The generic parameters that this generic signature builder is working
  /// with.
  SmallVector<Type, 4> GenericParams;

  /// The potential archetypes for the generic parameters in \c GenericParams.
  SmallVector<PotentialArchetype *, 4> PotentialArchetypes;

  /// The requirement sources used in this generic signature builder.
  llvm::FoldingSet<RequirementSource> RequirementSources;

  /// The set of requirements that have been delayed for some reason.
  SmallVector<DelayedRequirement, 4> DelayedRequirements;

  /// The set of equivalence classes.
  llvm::iplist<EquivalenceClass> EquivalenceClasses;

  /// Equivalence classes that are not currently being used.
  std::vector<void *> FreeEquivalenceClasses;

  /// The roots of the rewrite tree, keyed on the canonical, dependent
  /// types.
  DenseMap<CanType, std::unique_ptr<RewriteTreeNode>> RewriteTreeRoots;

  /// The generation number for the term-rewriting system, which is
  /// increased every time a new rule gets added.
  unsigned RewriteGeneration = 0;

  /// The generation at which the term-rewriting system was last minimized.
  unsigned LastRewriteMinimizedGeneration = 0;

  /// The generation number, which is incremented whenever we successfully
  /// introduce a new constraint.
  unsigned Generation = 0;

  /// The generation at which we last processed all of the delayed requirements.
  unsigned LastProcessedGeneration = 0;

  /// Whether we are currently processing delayed requirements.
  bool ProcessingDelayedRequirements = false;

  /// Whether we are currently minimizing the term-rewriting system.
  bool MinimizingRewriteSystem = false;

  /// Whether there were any errors.
  bool HadAnyError = false;

  /// FIXME: Hack to work around a small number of minimization bugs.
  bool HadAnyRedundantConstraints = false;

#ifndef NDEBUG
  /// Whether we've already finalized the builder.
  bool finalized = false;
#endif

  /// Tear down an implementation.
  ~Implementation();

  /// Allocate a new equivalence class with the given representative.
  EquivalenceClass *allocateEquivalenceClass(
                                       PotentialArchetype *representative);

  /// Deallocate the given equivalence class, returning it to the free list.
  void deallocateEquivalenceClass(EquivalenceClass *equivClass);

  /// Retrieve the rewrite tree root for the given anchor type.
  RewriteTreeNode *getRewriteTreeRootIfPresent(CanType anchor);

  /// Retrieve the rewrite tree root for the given anchor type,
  /// creating it if needed.
  RewriteTreeNode *getOrCreateRewriteTreeRoot(CanType anchor);

  /// Minimize the rewrite tree by minimizing the right-hand sides and
  /// removing redundant rules.
  void minimizeRewriteTree(GenericSignatureBuilder &builder);

private:
  /// Minimize the right-hand sides of the rewrite tree, simplifying them
  /// as far as possible and removing any changes that result in trivial
  /// rules.
  void minimizeRewriteTreeRhs(GenericSignatureBuilder &builder);

  /// Minimize the right-hand sides of the rewrite tree, simplifying them
  /// as far as possible and removing any changes that result in trivial
  /// rules.
  void removeRewriteTreeRedundancies(GenericSignatureBuilder &builder);
};

#pragma mark Memory management
GenericSignatureBuilder::Implementation::~Implementation() {
  for (auto pa : PotentialArchetypes)
    pa->~PotentialArchetype();
}

EquivalenceClass *
GenericSignatureBuilder::Implementation::allocateEquivalenceClass(
                                          PotentialArchetype *representative) {
  void *mem;
  if (FreeEquivalenceClasses.empty()) {
    // Allocate a new equivalence class.
    mem = Allocator.Allocate<EquivalenceClass>();
  } else {
    // Take an equivalence class from the free list.
    mem = FreeEquivalenceClasses.back();
    FreeEquivalenceClasses.pop_back();
  }

  auto equivClass = new (mem) EquivalenceClass(representative);
  EquivalenceClasses.push_back(equivClass);
  return equivClass;
}

void GenericSignatureBuilder::Implementation::deallocateEquivalenceClass(
                                               EquivalenceClass *equivClass) {
  EquivalenceClasses.erase(equivClass);
  FreeEquivalenceClasses.push_back(equivClass);
}

#pragma mark GraphViz visualization
namespace {
  /// A node in the equivalence class, used for visualization.
  struct EquivalenceClassVizNode {
    const EquivalenceClass *first;
    Type second;

    operator const void *() const { return second.getPointer(); }
  };

  /// Iterator through the adjacent nodes in an equivalence class, for
  /// visualization.
  class EquivalenceClassVizIterator {
    using BaseIterator = const Constraint<Type> *;

    EquivalenceClassVizNode node;
    BaseIterator base;
    BaseIterator baseEnd;

  public:
    using difference_type = ptrdiff_t;
    using value_type = EquivalenceClassVizNode;
    using reference = value_type;
    using pointer = value_type*;
    using iterator_category = std::forward_iterator_tag;

    EquivalenceClassVizIterator(EquivalenceClassVizNode node,
                                BaseIterator base, BaseIterator baseEnd)
        : node(node), base(base), baseEnd(baseEnd) {
    }

    BaseIterator &getBase() { return base; }
    const BaseIterator &getBase() const { return base; }

    reference operator*() const {
      return { node.first, getBase()->value };
    }

    EquivalenceClassVizIterator& operator++() {
      ++getBase();
      return *this;
    }

    EquivalenceClassVizIterator operator++(int) {
      EquivalenceClassVizIterator result = *this;
      ++(*this);
      return result;
    }

    friend bool operator==(const EquivalenceClassVizIterator &lhs,
                           const EquivalenceClassVizIterator &rhs) {
      return lhs.getBase() == rhs.getBase();
    }

    friend bool operator!=(const EquivalenceClassVizIterator &lhs,
                           const EquivalenceClassVizIterator &rhs) {
      return !(lhs == rhs);
    }
  };
}

namespace std {
  // FIXME: Egregious hack to work around a bogus static_assert in
  // llvm::GraphWriter. Good thing nobody else cares about this trait...
  template<>
  struct is_pointer<EquivalenceClassVizNode>
    : std::integral_constant<bool, true> { };
}

namespace llvm {
  // Visualize the same-type constraints within an equivalence class.
  template<>
  struct GraphTraits<const EquivalenceClass *> {
    using NodeRef = EquivalenceClassVizNode;

    static NodeRef getEntryNode(const EquivalenceClass *equivClass) {
      return { equivClass, equivClass->members.front()->getDependentType({ }) };
    }

    class nodes_iterator {
      using BaseIterator = PotentialArchetype * const *;

      const EquivalenceClass *equivClass;
      BaseIterator base;

    public:
      using difference_type = ptrdiff_t;
      using value_type = EquivalenceClassVizNode;
      using reference = value_type;
      using pointer = value_type*;
      using iterator_category = std::forward_iterator_tag;

      nodes_iterator(const EquivalenceClass *equivClass, BaseIterator base)
        : equivClass(equivClass), base(base) { }

      BaseIterator &getBase() { return base; }
      const BaseIterator &getBase() const { return base; }

      reference operator*() const {
        return { equivClass, (*getBase())->getDependentType({ }) };
      }

      nodes_iterator& operator++() {
        ++getBase();
        return *this;
      }

      nodes_iterator operator++(int) {
        nodes_iterator result = *this;
        ++(*this);
        return result;
      }

      friend bool operator==(const nodes_iterator &lhs,
                             const nodes_iterator &rhs) {
        return lhs.getBase() == rhs.getBase();
      }

      friend bool operator!=(const nodes_iterator &lhs,
                             const nodes_iterator &rhs) {
        return lhs.getBase() != rhs.getBase();
      }
    };

    static nodes_iterator nodes_begin(const EquivalenceClass *equivClass) {
      return nodes_iterator(equivClass, equivClass->members.begin());
    }

    static nodes_iterator nodes_end(const EquivalenceClass *equivClass) {
      return nodes_iterator(equivClass, equivClass->members.end());
    }

    static unsigned size(const EquivalenceClass *equivClass) {
      return equivClass->members.size();
    }

    using ChildIteratorType = EquivalenceClassVizIterator;

    static ChildIteratorType child_begin(NodeRef node) {
      auto base = node.first->sameTypeConstraints.data();
      auto baseEnd = base + node.first->sameTypeConstraints.size();
      return ChildIteratorType(node, base, baseEnd);
    }

    static ChildIteratorType child_end(NodeRef node) {
      auto base = node.first->sameTypeConstraints.data();
      auto baseEnd = base + node.first->sameTypeConstraints.size();
      return ChildIteratorType(node, baseEnd, baseEnd);
    }
  };

  template <>
  struct DOTGraphTraits<const EquivalenceClass *>
    : public DefaultDOTGraphTraits
  {
    DOTGraphTraits(bool = false) { }

    static std::string getGraphName(const EquivalenceClass *equivClass) {
      return "Equivalence class for '" +
        equivClass->members.front()->getDebugName() + "'";
    }

    std::string getNodeLabel(EquivalenceClassVizNode node,
                             const EquivalenceClass *equivClass) const {
      return node.second.getString();
    }

    static std::string getEdgeAttributes(EquivalenceClassVizNode node,
                                         EquivalenceClassVizIterator iter,
                                         const EquivalenceClass *equivClass) {
      if (iter.getBase()->source->kind
            == RequirementSource::NestedTypeNameMatch)
        return "color=\"blue\"";

      if (iter.getBase()->source->isDerivedRequirement())
        return "color=\"gray\"";

      return "color=\"red\"";
    }
  };
} // end namespace llvm

namespace {
  /// Retrieve the type described by the given unresolved tyoe.
  Type getUnresolvedType(GSBUnresolvedType type,
                         TypeArrayView<GenericTypeParamType> genericParams) {
    if (auto concrete = type.dyn_cast<Type>())
      return concrete;

    if (auto pa = type.dyn_cast<PotentialArchetype *>())
      return pa->getDependentType(genericParams);

    return Type();
  }
}

#pragma mark Requirement sources

#ifndef NDEBUG
bool RequirementSource::isAcceptableStorageKind(Kind kind,
                                                StorageKind storageKind) {
  switch (kind) {
  case Explicit:
  case Inferred:
  case RequirementSignatureSelf:
  case NestedTypeNameMatch:
  case ConcreteTypeBinding:
  case EquivalentType:
    switch (storageKind) {
    case StorageKind::StoredType:
      return true;

    case StorageKind::ProtocolConformance:
    case StorageKind::AssociatedTypeDecl:
    case StorageKind::None:
      return false;
    }

  case Parent:
    switch (storageKind) {
    case StorageKind::AssociatedTypeDecl:
      return true;

    case StorageKind::StoredType:
    case StorageKind::ProtocolConformance:
    case StorageKind::None:
      return false;
    }

  case ProtocolRequirement:
  case InferredProtocolRequirement:
    switch (storageKind) {
    case StorageKind::StoredType:
      return true;

    case StorageKind::ProtocolConformance:
    case StorageKind::AssociatedTypeDecl:
    case StorageKind::None:
      return false;
    }

  case Superclass:
  case Concrete:
    switch (storageKind) {
    case StorageKind::ProtocolConformance:
      return true;

    case StorageKind::StoredType:
    case StorageKind::AssociatedTypeDecl:
    case StorageKind::None:
      return false;
    }

  case Derived:
    switch (storageKind) {
    case StorageKind::None:
      return true;

    case StorageKind::StoredType:
    case StorageKind::ProtocolConformance:
    case StorageKind::AssociatedTypeDecl:
      return false;
    }
  }

  llvm_unreachable("Unhandled RequirementSourceKind in switch.");
}
#endif

const void *RequirementSource::getOpaqueStorage1() const {
  switch (storageKind) {
  case StorageKind::None:
    return nullptr;

  case StorageKind::ProtocolConformance:
    return storage.conformance;

  case StorageKind::StoredType:
    return storage.type;

  case StorageKind::AssociatedTypeDecl:
    return storage.assocType;
  }

  llvm_unreachable("Unhandled StorageKind in switch.");
}

const void *RequirementSource::getOpaqueStorage2() const {
  if (numTrailingObjects(OverloadToken<ProtocolDecl *>()) == 1)
    return getTrailingObjects<ProtocolDecl *>()[0];
  if (numTrailingObjects(OverloadToken<WrittenRequirementLoc>()) == 1)
    return getTrailingObjects<WrittenRequirementLoc>()[0].getOpaqueValue();

  return nullptr;
}

const void *RequirementSource::getOpaqueStorage3() const {
  if (numTrailingObjects(OverloadToken<ProtocolDecl *>()) == 1 &&
      numTrailingObjects(OverloadToken<WrittenRequirementLoc>()) == 1)
    return getTrailingObjects<WrittenRequirementLoc>()[0].getOpaqueValue();

  return nullptr;
}

bool RequirementSource::isInferredRequirement() const {
  for (auto source = this; source; source = source->parent) {
    switch (source->kind) {
    case Inferred:
    case InferredProtocolRequirement:
    case NestedTypeNameMatch:
      return true;

    case ConcreteTypeBinding:
    case EquivalentType:
      return false;

    case Concrete:
    case Explicit:
    case Parent:
    case ProtocolRequirement:
    case RequirementSignatureSelf:
    case Superclass:
    case Derived:
      break;
    }
  }

  return false;
}

unsigned RequirementSource::classifyDiagKind() const {
  if (isInferredRequirement()) return 2;
  if (isDerivedRequirement()) return 1;
  return 0;
}

bool RequirementSource::isDerivedRequirement() const {
  switch (kind) {
  case Explicit:
  case Inferred:
    return false;

  case NestedTypeNameMatch:
  case ConcreteTypeBinding:
  case Parent:
  case Superclass:
  case Concrete:
  case RequirementSignatureSelf:
  case Derived:
  case EquivalentType:
    return true;

  case ProtocolRequirement:
  case InferredProtocolRequirement:
    // Requirements based on protocol requirements are derived unless they are
    // direct children of the requirement-signature source, in which case we
    // need to keep them for the requirement signature.
    return parent->kind != RequirementSignatureSelf;
  }

  llvm_unreachable("Unhandled RequirementSourceKind in switch.");
}

bool RequirementSource::shouldDiagnoseRedundancy(bool primary) const {
  return !isInferredRequirement() && getLoc().isValid() &&
         (!primary || !isDerivedRequirement());
}

bool RequirementSource::isSelfDerivedSource(GenericSignatureBuilder &builder,
                                            Type type,
                                            bool &derivedViaConcrete) const {
  return getMinimalConformanceSource(builder, type, /*proto=*/nullptr,
                                     derivedViaConcrete)
    != this;
}

/// Replace 'Self' in the given dependent type (\c depTy) with the given
/// dependent type, producing a type that refers to
/// the nested type. This limited operation makes sure that it does not
/// create any new potential archetypes along the way, so it should only be
/// used in cases where we're reconstructing something that we know exists.
static Type replaceSelfWithType(Type selfType, Type depTy) {
  if (auto depMemTy = depTy->getAs<DependentMemberType>()) {
    Type baseType = replaceSelfWithType(selfType, depMemTy->getBase());
    assert(depMemTy->getAssocType() && "Missing associated type");
    return DependentMemberType::get(baseType, depMemTy->getAssocType());
  }

  assert(depTy->is<GenericTypeParamType>() && "missing Self?");
  return selfType;
}

/// Determine whether the given protocol requirement is self-derived when it
/// occurs within the requirement signature of its own protocol.
static bool isSelfDerivedProtocolRequirementInProtocol(
                                             const RequirementSource *source,
                                             ProtocolDecl *selfProto,
                                             GenericSignatureBuilder &builder) {
  assert(source->isProtocolRequirement());

  // This can only happen if the requirement points comes from the protocol
  // itself.
  if (source->getProtocolDecl() != selfProto) return false;

  // This only applies if the parent is not the anchor for computing the
  // requirement signature. Anywhere else, we can use the protocol requirement.
  if (source->parent->kind == RequirementSource::RequirementSignatureSelf)
    return false;

  // If the relative type of the protocol requirement itself is in the
  // same equivalence class as what we've proven with this requirement,
  // it's a self-derived requirement.
  return
    builder.resolveEquivalenceClass(source->getAffectedType(),
                                    ArchetypeResolutionKind::WellFormed) ==
      builder.resolveEquivalenceClass(source->getStoredType(),
                                      ArchetypeResolutionKind::AlreadyKnown);
}

const RequirementSource *RequirementSource::getMinimalConformanceSource(
                                             GenericSignatureBuilder &builder,
                                             Type currentType,
                                             ProtocolDecl *proto,
                                             bool &derivedViaConcrete) const {
  derivedViaConcrete = false;

  // If it's not a derived requirement, it's not self-derived.
  if (!isDerivedRequirement()) return this;

  /// Keep track of all of the requirements we've seen along the way. If
  /// we see the same requirement twice, we have found a shorter path.
  llvm::DenseMap<std::pair<EquivalenceClass *, ProtocolDecl *>,
                 const RequirementSource *>
    constraintsSeen;

  /// Note that we've now seen a new constraint (described on an equivalence
  /// class).
  auto addConstraint = [&](EquivalenceClass *equivClass, ProtocolDecl *proto,
                           const RequirementSource *source)
      -> const RequirementSource * {
    auto &storedSource = constraintsSeen[{equivClass, proto}];
    if (storedSource) return storedSource;

    storedSource = source;
    return nullptr;
  };

  // Note that we've now seen a new constraint, returning true if we've seen
  // it before.
  auto addTypeConstraint = [&](Type type, ProtocolDecl *proto,
                           const RequirementSource *source)
      -> const RequirementSource * {
    auto equivClass =
        builder.resolveEquivalenceClass(type,
                                        ArchetypeResolutionKind::WellFormed);
    assert(equivClass && "Not a well-formed type?");
    return addConstraint(equivClass, proto, source);
  };

  bool sawProtocolRequirement = false;
  ProtocolDecl *requirementSignatureSelfProto = nullptr;

  Type rootType = nullptr;
  Optional<std::pair<const RequirementSource *, const RequirementSource *>>
    redundantSubpath;
  bool isSelfDerived = visitPotentialArchetypesAlongPath(
          [&](Type parentType, const RequirementSource *source) {
    switch (source->kind) {
    case ProtocolRequirement:
    case InferredProtocolRequirement: {
      // Note that we've seen a protocol requirement.
      sawProtocolRequirement = true;

      // If the base has been made concrete, note it.
      auto parentEquivClass =
          builder.resolveEquivalenceClass(parentType,
                                          ArchetypeResolutionKind::WellFormed);
      assert(parentEquivClass && "Not a well-formed type?");

      if (parentEquivClass->concreteType)
        derivedViaConcrete = true;

      // The parent potential archetype must conform to the protocol in which
      // this requirement resides. Add this constraint.
      if (auto startOfPath =
              addConstraint(parentEquivClass, source->getProtocolDecl(),
                            source->parent)) {
        // We found a redundant subpath; record it and stop the algorithm.
        assert(startOfPath != source->parent);
        redundantSubpath = { startOfPath, source->parent };
        return true;
      }

      // If this is a self-derived protocol requirement, fail.
      if (requirementSignatureSelfProto &&
          isSelfDerivedProtocolRequirementInProtocol(
                                               source,
                                               requirementSignatureSelfProto,
                                               builder)) {
        redundantSubpath = { source->getRoot(), source->parent };
        return true;
      }

      // No redundancy thus far.
      return false;
    }

    case Parent:
      // FIXME: Ad hoc detection of recursive same-type constraints.
      return !proto &&
        builder.areInSameEquivalenceClass(parentType, currentType);

    case Concrete:
    case Superclass:
    case Derived:
    case EquivalentType:
      return false;

    case RequirementSignatureSelf:
      // Note the protocol whose requirement signature the requirement is
      // based on.
      requirementSignatureSelfProto = source->getProtocolDecl();
      LLVM_FALLTHROUGH;

    case Explicit:
    case Inferred:
    case NestedTypeNameMatch:
    case ConcreteTypeBinding:
      rootType = parentType;
      return false;
    }
  }).isNull();

  // If we didn't already find a redundancy, check our end state.
  if (!redundantSubpath && proto) {
    if (auto startOfPath = addTypeConstraint(currentType, proto, this)) {
      redundantSubpath = { startOfPath, this };
      assert(startOfPath != this);
      isSelfDerived = true;
    }
  }

  // If we saw a constraint twice, it's self-derived.
  if (redundantSubpath) {
    assert(isSelfDerived && "Not considered self-derived?");
    auto shorterSource =
      withoutRedundantSubpath(builder,
                              redundantSubpath->first,
                              redundantSubpath->second);
    return shorterSource
      ->getMinimalConformanceSource(builder, currentType, proto, derivedViaConcrete);
  }

  // It's self-derived but we don't have a redundant subpath to eliminate.
  if (isSelfDerived)
    return nullptr;

  // If we haven't seen a protocol requirement, we're done.
  if (!sawProtocolRequirement) return this;

  // The root might be a nested type, which implies constraints
  // for each of the protocols of the associated types referenced (if any).
  for (auto depMemTy = rootType->getAs<DependentMemberType>(); depMemTy;
       depMemTy = depMemTy->getBase()->getAs<DependentMemberType>()) {
    auto assocType = depMemTy->getAssocType();
    assert(assocType);
    if (addTypeConstraint(depMemTy->getBase(), assocType->getProtocol(),
                          nullptr))
      return nullptr;
  }

  return this;
}

#define REQUIREMENT_SOURCE_FACTORY_BODY(ProfileArgs, ConstructorArgs,      \
                                        NumProtocolDecls, WrittenReq)      \
  llvm::FoldingSetNodeID nodeID;                                           \
  Profile ProfileArgs;                                                     \
                                                                           \
  void *insertPos = nullptr;                                               \
  if (auto known =                                                         \
        builder.Impl->RequirementSources.FindNodeOrInsertPos(nodeID,       \
                                                             insertPos))   \
    return known;                                                          \
                                                                           \
  unsigned size =                                                          \
    totalSizeToAlloc<ProtocolDecl *, WrittenRequirementLoc>(               \
                                           NumProtocolDecls,               \
                                           WrittenReq.isNull()? 0 : 1);    \
  void *mem =                                                              \
    builder.Impl->Allocator.Allocate(size, alignof(RequirementSource));    \
  auto result = new (mem) RequirementSource ConstructorArgs;               \
  builder.Impl->RequirementSources.InsertNode(result, insertPos);          \
  return result

const RequirementSource *RequirementSource::forAbstract(
                                            GenericSignatureBuilder &builder,
                                            Type rootType) {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Explicit, nullptr, rootType.getPointer(),
                         nullptr, nullptr),
                        (Explicit, rootType, nullptr, WrittenRequirementLoc()),
                        0, WrittenRequirementLoc());
}

const RequirementSource *RequirementSource::forExplicit(
                  GenericSignatureBuilder &builder,
                  Type rootType,
                  GenericSignatureBuilder::WrittenRequirementLoc writtenLoc) {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Explicit, nullptr, rootType.getPointer(),
                         writtenLoc.getOpaqueValue(), nullptr),
                        (Explicit, rootType, nullptr, writtenLoc),
                        0, writtenLoc);
}

const RequirementSource *RequirementSource::forInferred(
                                              GenericSignatureBuilder &builder,
                                              Type rootType,
                                              const TypeRepr *typeRepr) {
  WrittenRequirementLoc writtenLoc = typeRepr;
  REQUIREMENT_SOURCE_FACTORY_BODY(
      (nodeID, Inferred, nullptr, rootType.getPointer(),
       writtenLoc.getOpaqueValue(), nullptr),
       (Inferred, rootType, nullptr, writtenLoc),
       0, writtenLoc);
}

const RequirementSource *RequirementSource::forRequirementSignature(
                                              GenericSignatureBuilder &builder,
                                              Type rootType,
                                              ProtocolDecl *protocol) {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, RequirementSignatureSelf, nullptr,
                         rootType.getPointer(), protocol, nullptr),
                        (RequirementSignatureSelf, rootType, protocol,
                         WrittenRequirementLoc()),
                        1, WrittenRequirementLoc());

}

const RequirementSource *RequirementSource::forNestedTypeNameMatch(
                                             GenericSignatureBuilder &builder,
                                             Type rootType) {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, NestedTypeNameMatch, nullptr,
                         rootType.getPointer(), nullptr, nullptr),
                        (NestedTypeNameMatch, rootType, nullptr,
                         WrittenRequirementLoc()),
                        0, WrittenRequirementLoc());
}

const RequirementSource *RequirementSource::forConcreteTypeBinding(
                                             GenericSignatureBuilder &builder,
                                             Type rootType) {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, ConcreteTypeBinding, nullptr,
                         rootType.getPointer(), nullptr, nullptr),
                        (ConcreteTypeBinding, rootType, nullptr,
                         WrittenRequirementLoc()),
                        0, WrittenRequirementLoc());
}

const RequirementSource *RequirementSource::viaProtocolRequirement(
            GenericSignatureBuilder &builder, Type dependentType,
            ProtocolDecl *protocol,
            bool inferred,
            GenericSignatureBuilder::WrittenRequirementLoc writtenLoc) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID,
                         inferred ? InferredProtocolRequirement
                                  : ProtocolRequirement,
                         this,
                         dependentType.getPointer(), protocol,
                         writtenLoc.getOpaqueValue()),
                        (inferred ? InferredProtocolRequirement
                                  : ProtocolRequirement,
                         this, dependentType,
                         protocol, writtenLoc),
                        1, writtenLoc);
}

const RequirementSource *RequirementSource::viaSuperclass(
                                    GenericSignatureBuilder &builder,
                                    ProtocolConformanceRef conformance) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Superclass, this, conformance.getOpaqueValue(),
                         nullptr, nullptr),
                        (Superclass, this, conformance),
                        0, WrittenRequirementLoc());
}

const RequirementSource *RequirementSource::viaConcrete(
                                    GenericSignatureBuilder &builder,
                                    ProtocolConformanceRef conformance) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Concrete, this, conformance.getOpaqueValue(),
                         nullptr, nullptr),
                        (Concrete, this, conformance),
                        0, WrittenRequirementLoc());
}

const RequirementSource *RequirementSource::viaParent(
                                      GenericSignatureBuilder &builder,
                                      AssociatedTypeDecl *assocType) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Parent, this, assocType, nullptr, nullptr),
                        (Parent, this, assocType),
                        0, WrittenRequirementLoc());
}

const RequirementSource *RequirementSource::viaDerived(
                           GenericSignatureBuilder &builder) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Derived, this, nullptr, nullptr, nullptr),
                        (Derived, this),
                        0, WrittenRequirementLoc());
}

const RequirementSource *RequirementSource::viaEquivalentType(
                                           GenericSignatureBuilder &builder,
                                           Type newType) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, EquivalentType, this, newType.getPointer(),
                         nullptr, nullptr),
                        (EquivalentType, this, newType),
                        0, WrittenRequirementLoc());
}

#undef REQUIREMENT_SOURCE_FACTORY_BODY

const RequirementSource *RequirementSource::withoutRedundantSubpath(
                                        GenericSignatureBuilder &builder,
                                        const RequirementSource *start,
                                        const RequirementSource *end) const {
  // Replace the end with the start; the caller has guaranteed that they
  // produce the same thing.
  if (this == end) {
#ifndef NDEBUG
    // Sanity check: make sure the 'start' precedes the 'end'.
    bool foundStart = false;
    for (auto source = this; source; source = source->parent) {
      if (source == start) {
        foundStart = true;
        break;
      }
    }
    assert(foundStart && "Start doesn't precede end!");
#endif
    return start;
  }

  switch (kind) {
  case Explicit:
  case Inferred:
  case RequirementSignatureSelf:
  case NestedTypeNameMatch:
  case ConcreteTypeBinding:
    llvm_unreachable("Subpath end doesn't occur within path");

  case ProtocolRequirement:
    return parent->withoutRedundantSubpath(builder, start, end)
      ->viaProtocolRequirement(builder, getStoredType(),
                               getProtocolDecl(), /*inferred=*/false,
                               getWrittenRequirementLoc());

  case InferredProtocolRequirement:
    return parent->withoutRedundantSubpath(builder, start, end)
      ->viaProtocolRequirement(builder, getStoredType(),
                               getProtocolDecl(), /*inferred=*/true,
                               getWrittenRequirementLoc());

  case Concrete:
    return parent->withoutRedundantSubpath(builder, start, end)
      ->viaConcrete(builder, getProtocolConformance());

  case Derived:
    return parent->withoutRedundantSubpath(builder, start, end)
      ->viaDerived(builder);

  case EquivalentType:
    return parent->withoutRedundantSubpath(builder, start, end)
      ->viaEquivalentType(builder, Type(storage.type));

  case Parent:
    return parent->withoutRedundantSubpath(builder, start, end)
      ->viaParent(builder, getAssociatedType());

  case Superclass:
    return parent->withoutRedundantSubpath(builder, start, end)
      ->viaSuperclass(builder, getProtocolConformance());
  }
}

const RequirementSource *RequirementSource::getRoot() const {
  auto root = this;
  while (auto parent = root->parent)
    root = parent;
  return root;
}

Type RequirementSource::getRootType() const {
  /// Find the root.
  auto root = getRoot();

  // We're at the root, so it's in the inline storage.
  assert(root->storageKind == StorageKind::StoredType);
  return Type(root->storage.type);
}

Type RequirementSource::getAffectedType() const {
  return visitPotentialArchetypesAlongPath(
                         [](Type, const RequirementSource *) {
                           return false;
                         });
}

Type
RequirementSource::visitPotentialArchetypesAlongPath(
     llvm::function_ref<bool(Type, const RequirementSource *)> visitor) const {
  switch (kind) {
  case RequirementSource::Parent: {
    Type parentType = parent->visitPotentialArchetypesAlongPath(visitor);
    if (!parentType) return nullptr;

    if (visitor(parentType, this)) return nullptr;

    return replaceSelfWithType(parentType,
                               getAssociatedType()->getDeclaredInterfaceType());
  }

  case RequirementSource::NestedTypeNameMatch:
  case RequirementSource::ConcreteTypeBinding:
  case RequirementSource::Explicit:
  case RequirementSource::Inferred:
  case RequirementSource::RequirementSignatureSelf: {
    Type rootType = getRootType();
    if (visitor(rootType, this)) return nullptr;

    return rootType;
  }

  case RequirementSource::Concrete:
  case RequirementSource::Superclass:
  case RequirementSource::Derived:
    return parent->visitPotentialArchetypesAlongPath(visitor);

  case RequirementSource::EquivalentType: {
    auto parentType = parent->visitPotentialArchetypesAlongPath(visitor);
    if (!parentType) return nullptr;

    if (visitor(parentType, this)) return nullptr;

    return Type(storage.type);
  }

  case RequirementSource::ProtocolRequirement:
  case RequirementSource::InferredProtocolRequirement: {
    Type parentType = parent->visitPotentialArchetypesAlongPath(visitor);
    if (!parentType) return nullptr;

    if (visitor(parentType, this)) return nullptr;

    return replaceSelfWithType(parentType, getStoredType());
  }
  }
}

Type RequirementSource::getStoredType() const {
  switch (storageKind) {
  case StorageKind::None:
  case StorageKind::ProtocolConformance:
  case StorageKind::AssociatedTypeDecl:
    return Type();

  case StorageKind::StoredType:
    return storage.type;
  }

  llvm_unreachable("Unhandled StorageKind in switch.");
}

ProtocolDecl *RequirementSource::getProtocolDecl() const {
  switch (storageKind) {
  case StorageKind::None:
    return nullptr;

  case StorageKind::StoredType:
    if (isProtocolRequirement() || kind == RequirementSignatureSelf)
      return getTrailingObjects<ProtocolDecl *>()[0];
    return nullptr;

  case StorageKind::ProtocolConformance:
    return getProtocolConformance().getRequirement();

  case StorageKind::AssociatedTypeDecl:
    return storage.assocType->getProtocol();
  }

  llvm_unreachable("Unhandled StorageKind in switch.");
}

SourceLoc RequirementSource::getLoc() const {
  // Don't produce locations for protocol requirements unless the parent is
  // the protocol self.
  // FIXME: We should have a better notion of when to emit diagnostics
  // for a particular requirement, rather than turning on/off location info.
  // Locations that fall into this category should be advisory, emitted via
  // notes rather than as the normal location.
  if (isProtocolRequirement() && parent &&
      parent->kind != RequirementSignatureSelf)
    return parent->getLoc();

  if (auto typeRepr = getTypeRepr())
    return typeRepr->getStartLoc();

  if (auto requirementRepr = getRequirementRepr()) {
    switch (requirementRepr->getKind()) {
    case RequirementReprKind::LayoutConstraint:
    case RequirementReprKind::TypeConstraint:
      return requirementRepr->getColonLoc();

    case RequirementReprKind::SameType:
      return requirementRepr->getEqualLoc();
    }
  }
  if (parent)
    return parent->getLoc();

  if (kind == RequirementSignatureSelf)
    return getProtocolDecl()->getLoc();

  return SourceLoc();
}

/// Compute the path length of a requirement source, counting only the number
/// of \c ProtocolRequirement elements.
static unsigned sourcePathLength(const RequirementSource *source) {
  unsigned count = 0;
  for (; source; source = source->parent) {
    if (source->isProtocolRequirement())
      ++count;
  }
  return count;
}

int RequirementSource::compare(const RequirementSource *other) const {
  // Prefer the derived option, if there is one.
  bool thisIsDerived = this->isDerivedRequirement();
  bool otherIsDerived = other->isDerivedRequirement();
  if (thisIsDerived != otherIsDerived)
    return thisIsDerived ? -1 : +1;

  // Prefer the shorter path.
  unsigned thisLength = sourcePathLength(this);
  unsigned otherLength = sourcePathLength(other);
  if (thisLength != otherLength)
    return thisLength < otherLength ? -1 : +1;

  // FIXME: Arbitrary hack to allow later requirement sources to stomp on
  // earlier ones. We need a proper ordering here.
  return +1;
}

void RequirementSource::dump() const {
  dump(llvm::errs(), nullptr, 0);
  llvm::errs() << "\n";
}

/// Dump the constraint source.
void RequirementSource::dump(llvm::raw_ostream &out, SourceManager *srcMgr,
                             unsigned indent) const {
  // FIXME: Implement for real, so we actually dump the structure.
  out.indent(indent);
  print(out, srcMgr);
}

void RequirementSource::print() const {
  print(llvm::errs(), nullptr);
}

void RequirementSource::print(llvm::raw_ostream &out,
                              SourceManager *srcMgr) const {
  if (parent) {
    parent->print(out, srcMgr);
    out << " -> ";
  } else {
    out << getRootType().getString() << ": ";
  }

  switch (kind) {
  case Concrete:
    out << "Concrete";
    break;

  case Explicit:
    out << "Explicit";
    break;

  case Inferred:
    out << "Inferred";
    break;

  case NestedTypeNameMatch:
    out << "Nested type match";
    break;

  case RequirementSource::ConcreteTypeBinding:
    out << "Concrete type binding";
    break;

  case Parent:
    out << "Parent";
    break;

  case ProtocolRequirement:
    out << "Protocol requirement";
    break;

  case InferredProtocolRequirement:
    out << "Inferred protocol requirement";
    break;

  case RequirementSignatureSelf:
    out << "Requirement signature self";
    break;

  case Superclass:
    out << "Superclass";
    break;

  case Derived:
    out << "Derived";
    break;

  case EquivalentType:
    out << "Equivalent type";
    break;
  }

  // Local function to dump a source location, if we can.
  auto dumpSourceLoc = [&](SourceLoc loc) {
    if (!srcMgr) return;
    if (loc.isInvalid()) return;

    unsigned bufferID = srcMgr->findBufferContainingLoc(loc);

    auto lineAndCol = srcMgr->getLineAndColumn(loc, bufferID);
    out << " @ " << lineAndCol.first << ':' << lineAndCol.second;
  };

  switch (storageKind) {
  case StorageKind::None:
    break;

  case StorageKind::StoredType:
    if (auto proto = getProtocolDecl()) {
      out << " (via " << storage.type->getString() << " in " << proto->getName()
          << ")";
    }
    break;

  case StorageKind::ProtocolConformance: {
    auto conformance = getProtocolConformance();
    if (conformance.isConcrete()) {
      out << " (" << conformance.getConcrete()->getType()->getString() << ": "
          << conformance.getConcrete()->getProtocol()->getName() << ")";
    } else {
      out << " (abstract " << conformance.getRequirement()->getName() << ")";
    }
    break;
  }

  case StorageKind::AssociatedTypeDecl:
    out << " (" << storage.assocType->getProtocol()->getName()
        << "::" << storage.assocType->getName() << ")";
    break;
  }

  if (getTypeRepr() || getRequirementRepr()) {
    dumpSourceLoc(getLoc());
  }
}

/// Form the dependent type such that the given protocol's \c Self can be
/// replaced by \c baseType to reach \c type.
static Type formProtocolRelativeType(ProtocolDecl *proto,
                                     Type baseType,
                                     Type type) {
  // Basis case: we've hit the base potential archetype.
  if (baseType->isEqual(type))
    return proto->getSelfInterfaceType();

  // Recursive case: form a dependent member type.
  auto depMemTy = type->castTo<DependentMemberType>();
  Type newBaseType = formProtocolRelativeType(proto, baseType,
                                              depMemTy->getBase());
  auto assocType = depMemTy->getAssocType();
  return DependentMemberType::get(newBaseType, assocType);
}

const RequirementSource *FloatingRequirementSource::getSource(
                                              GenericSignatureBuilder &builder,
                                              Type type) const {
  switch (kind) {
  case Resolved:
    return storage.get<const RequirementSource *>();

  case Explicit:
    if (auto requirementRepr = storage.dyn_cast<const RequirementRepr *>())
      return RequirementSource::forExplicit(builder, type, requirementRepr);
    if (auto typeRepr = storage.dyn_cast<const TypeRepr *>())
      return RequirementSource::forExplicit(builder, type, typeRepr);
    return RequirementSource::forAbstract(builder, type);

  case Inferred:
    return RequirementSource::forInferred(builder, type,
                                          storage.get<const TypeRepr *>());

  case AbstractProtocol: {
    // Derive the dependent type on which this requirement was written. It is
    // the path from the requirement source on which this requirement is based
    // to the potential archetype on which the requirement is being placed.
    auto baseSource = storage.get<const RequirementSource *>();
    auto baseSourceType = baseSource->getAffectedType();

    auto dependentType =
      formProtocolRelativeType(protocolReq.protocol, baseSourceType, type);

    return storage.get<const RequirementSource *>()
      ->viaProtocolRequirement(builder, dependentType,
                               protocolReq.protocol, protocolReq.inferred,
                               protocolReq.written);
  }

  case NestedTypeNameMatch:
    return RequirementSource::forNestedTypeNameMatch(builder, type);
  }

  llvm_unreachable("Unhandled FloatingPointRequirementSourceKind in switch.");
}

SourceLoc FloatingRequirementSource::getLoc() const {
  if (auto source = storage.dyn_cast<const RequirementSource *>())
    return source->getLoc();

  if (auto typeRepr = storage.dyn_cast<const TypeRepr *>())
    return typeRepr->getLoc();

  if (auto requirementRepr = storage.dyn_cast<const RequirementRepr *>()) {
    switch (requirementRepr->getKind()) {
    case RequirementReprKind::LayoutConstraint:
    case RequirementReprKind::TypeConstraint:
      return requirementRepr->getColonLoc();

    case RequirementReprKind::SameType:
      return requirementRepr->getEqualLoc();
    }
  }

  return SourceLoc();
}

bool FloatingRequirementSource::isExplicit() const {
  switch (kind) {
  case Explicit:
    return true;

  case Inferred:
  case NestedTypeNameMatch:
    return false;

  case AbstractProtocol:
    // Requirements implied by other protocol conformance requirements are
    // implicit, except when computing a requirement signature, where
    // non-inferred ones are explicit, to allow flagging of redundant
    // requirements.
    switch (storage.get<const RequirementSource *>()->kind) {
    case RequirementSource::RequirementSignatureSelf:
      return !protocolReq.inferred;

    case RequirementSource::Concrete:
    case RequirementSource::Explicit:
    case RequirementSource::Inferred:
    case RequirementSource::NestedTypeNameMatch:
    case RequirementSource::ConcreteTypeBinding:
    case RequirementSource::Parent:
    case RequirementSource::ProtocolRequirement:
    case RequirementSource::InferredProtocolRequirement:
    case RequirementSource::Superclass:
    case RequirementSource::Derived:
    case RequirementSource::EquivalentType:
      return false;
    }

  case Resolved:
    switch (storage.get<const RequirementSource *>()->kind) {
    case RequirementSource::Explicit:
      return true;

    case RequirementSource::ProtocolRequirement:
      return storage.get<const RequirementSource *>()->parent->kind
        == RequirementSource::RequirementSignatureSelf;

    case RequirementSource::Inferred:
    case RequirementSource::InferredProtocolRequirement:
    case RequirementSource::RequirementSignatureSelf:
    case RequirementSource::Concrete:
    case RequirementSource::NestedTypeNameMatch:
    case RequirementSource::ConcreteTypeBinding:
    case RequirementSource::Parent:
    case RequirementSource::Superclass:
    case RequirementSource::Derived:
    case RequirementSource::EquivalentType:
      return false;
    }
  }
}


FloatingRequirementSource FloatingRequirementSource::asInferred(
                                          const TypeRepr *typeRepr) const {
  switch (kind) {
  case Explicit:
    return forInferred(typeRepr);

  case Inferred:
  case Resolved:
  case NestedTypeNameMatch:
    return *this;

  case AbstractProtocol:
    return viaProtocolRequirement(storage.get<const RequirementSource *>(),
                                  protocolReq.protocol, typeRepr,
                                  /*inferred=*/true);
  }
}

bool FloatingRequirementSource::isRecursive(
                                    Type rootType,
                                    GenericSignatureBuilder &builder) const {
  llvm::SmallSet<std::pair<CanType, ProtocolDecl *>, 4> visitedAssocReqs;
  for (auto storedSource = storage.dyn_cast<const RequirementSource *>();
       storedSource; storedSource = storedSource->parent) {
    // FIXME: isRecursive() is completely misnamed
    if (storedSource->kind == RequirementSource::EquivalentType)
      return true;

    if (!storedSource->isProtocolRequirement())
      continue;

    if (!visitedAssocReqs.insert(
                          {storedSource->getStoredType()->getCanonicalType(),
                           storedSource->getProtocolDecl()}).second)
      return true;
  }

  return false;
}

GenericSignatureBuilder::PotentialArchetype::~PotentialArchetype() {
  ++NumPotentialArchetypes;

  for (const auto &nested : NestedTypes) {
    for (auto pa : nested.second) {
      pa->~PotentialArchetype();
    }
  }
}

std::string GenericSignatureBuilder::PotentialArchetype::getDebugName() const {
  llvm::SmallString<64> result;

  auto parent = getParent();
  if (!parent) {
    static const char *tau = u8"\u03C4_";

    llvm::raw_svector_ostream os(result);
    os << tau << getGenericParamKey().Depth << '_'
       << getGenericParamKey().Index;
    return os.str().str();
  }

  // Nested types.
  result += parent->getDebugName();

  // When building the name for debugging purposes, include the protocol into
  // which the associated type or type alias was resolved.
  auto *proto = getResolvedType()->getProtocol();

  if (proto) {
    result.push_back('[');
    result.push_back('.');
    result.append(proto->getName().str().begin(), proto->getName().str().end());
    result.push_back(']');
  }

  result.push_back('.');
  result.append(getNestedName().str().begin(), getNestedName().str().end());

  return result.str().str();
}

unsigned GenericSignatureBuilder::PotentialArchetype::getNestingDepth() const {
  unsigned Depth = 0;
  for (auto P = getParent(); P; P = P->getParent())
    ++Depth;
  return Depth;
}

void EquivalenceClass::addMember(PotentialArchetype *pa) {
  assert(find(members, pa) == members.end() &&
         "Already have this potential archetype!");
  members.push_back(pa);
  if (members.back()->getNestingDepth() < members.front()->getNestingDepth()) {
    MutableArrayRef<PotentialArchetype *> mutMembers = members;
    std::swap(mutMembers.front(), mutMembers.back());
  }
}

class GenericSignatureBuilder::ResolvedType {
  llvm::PointerUnion<PotentialArchetype *, Type> type;
  EquivalenceClass *equivClass;

  /// For a type that could not be resolved further unless the given
  /// equivalence class changes.
  ResolvedType(EquivalenceClass *equivClass)
    : type(), equivClass(equivClass) { }

public:
  /// A specific resolved potential archetype.
  ResolvedType(PotentialArchetype *pa)
    : type(pa), equivClass(pa->getEquivalenceClassIfPresent()) { }

  /// A resolved type within the given equivalence class.
  ResolvedType(Type type, EquivalenceClass *equivClass)
      : type(type), equivClass(equivClass) {
    assert(type->isTypeParameter() == static_cast<bool>(equivClass) &&
           "type parameters must have equivalence classes");
  }

  /// Return an unresolved result, which could be resolved when we
  /// learn more information about the given equivalence class.
  static ResolvedType forUnresolved(EquivalenceClass *equivClass) {
    return ResolvedType(equivClass);
  }

  /// Return a result for a concrete type.
  static ResolvedType forConcrete(Type concreteType) {
    return ResolvedType(concreteType, nullptr);
  }

  /// Determine whether this result was resolved.
  explicit operator bool() const { return !type.isNull(); }

  /// Retrieve the dependent type.
  Type getDependentType(GenericSignatureBuilder &builder) const;

  /// Retrieve the concrete type, or a null type if this result doesn't store
  /// a concrete type.
  Type getAsConcreteType() const {
    assert(*this && "Doesn't contain any result");
    if (equivClass) return Type();
    return type.dyn_cast<Type>();
  }

  /// Realize a potential archetype for this type parameter.
  PotentialArchetype *realizePotentialArchetype(
                                            GenericSignatureBuilder &builder);

  /// Retrieve the potential archetype, if already known.
  PotentialArchetype *getPotentialArchetypeIfKnown() const {
    return type.dyn_cast<PotentialArchetype *>();
  }

  /// Retrieve the equivalence class into which a resolved type refers.
  EquivalenceClass *getEquivalenceClass(
                     GenericSignatureBuilder &builder) const {
    assert(*this && "Only for resolved types");
    if (equivClass) return equivClass;

    // Create the equivalence class now.
    return type.get<PotentialArchetype *>()
             ->getOrCreateEquivalenceClass(builder);
  }

  /// Retrieve the equivalence class into which a resolved type refers.
  EquivalenceClass *getEquivalenceClassIfPresent() const {
    assert(*this && "Only for resolved types");
    if (equivClass) return equivClass;

    // Create the equivalence class now.
    return type.get<PotentialArchetype *>()->getEquivalenceClassIfPresent();
  }

  /// Retrieve the unresolved result.
  EquivalenceClass *getUnresolvedEquivClass() const {
    assert(!*this);
    return equivClass;
  }

  /// Return an unresolved type.
  ///
  /// This loses equivalence-class information that could be useful, which
  /// is unfortunate.
  UnresolvedType getUnresolvedType() const {
    return type;
  }
};

bool EquivalenceClass::recordConformanceConstraint(
                                 GenericSignatureBuilder &builder,
                                 ResolvedType type,
                                 ProtocolDecl *proto,
                                 FloatingRequirementSource source) {
  // If we haven't seen a conformance to this protocol yet, add it.
  bool inserted = false;
  auto known = conformsTo.find(proto);
  if (known == conformsTo.end()) {
    known = conformsTo.insert({ proto, { }}).first;
    inserted = true;
    modified(builder);
    ++NumConformances;

    // If there is a concrete type that resolves this conformance requirement,
    // record the conformance.
    if (!builder.resolveConcreteConformance(type, proto)) {
      // Otherwise, determine whether there is a superclass constraint where the
      // superclass conforms to this protocol.
      (void)builder.resolveSuperConformance(type, proto);
    }
  }

  // Record this conformance source.
  known->second.push_back({type.getUnresolvedType(), proto,
                           source.getSource(builder,
                                            type.getDependentType(builder))});
  ++NumConformanceConstraints;

  return inserted;
}

template<typename T>
Type Constraint<T>::getSubjectDependentType(
                      TypeArrayView<GenericTypeParamType> genericParams) const {
  if (auto type = subject.dyn_cast<Type>())
    return type;

  return subject.get<PotentialArchetype *>()->getDependentType(genericParams);
}

template<typename T>
bool Constraint<T>::isSubjectEqualTo(Type type) const {
  return getSubjectDependentType({ })->isEqual(type);
}

template<typename T>
bool Constraint<T>::isSubjectEqualTo(const PotentialArchetype *pa) const {
  return getSubjectDependentType({ })->isEqual(pa->getDependentType({ }));
}

template<typename T>
bool Constraint<T>::hasSameSubjectAs(const Constraint<T> &other) const {
  return getSubjectDependentType({ })
    ->isEqual(other.getSubjectDependentType({ }));
}

Optional<ConcreteConstraint>
EquivalenceClass::findAnyConcreteConstraintAsWritten(Type preferredType) const {
  // If we don't have a concrete type, there's no source.
  if (!concreteType) return None;

  // Go look for a source with source-location information.
  Optional<ConcreteConstraint> result;
  for (const auto &constraint : concreteTypeConstraints) {
    if (constraint.source->getLoc().isValid()) {
      result = constraint;
      if (!preferredType ||
          constraint.getSubjectDependentType({ })->isEqual(preferredType))
        return result;
    }
  }

  return result;
}

Optional<ConcreteConstraint>
EquivalenceClass::findAnySuperclassConstraintAsWritten(
                                                   Type preferredType) const {
  // If we don't have a superclass, there's no source.
  if (!superclass) return None;

  // Go look for a source with source-location information.
  Optional<ConcreteConstraint> result;
  for (const auto &constraint : superclassConstraints) {
    if (constraint.source->getLoc().isValid() &&
        constraint.value->isEqual(superclass)) {
      result = constraint;

      if (!preferredType ||
          constraint.getSubjectDependentType({ })->isEqual(preferredType))
        return result;
    }
  }

  return result;
}

bool EquivalenceClass::isConformanceSatisfiedBySuperclass(
                                                    ProtocolDecl *proto) const {
  auto known = conformsTo.find(proto);
  assert(known != conformsTo.end() && "doesn't conform to this protocol");
  for (const auto &constraint: known->second) {
    if (constraint.source->kind == RequirementSource::Superclass)
      return true;
  }

  return false;
}

/// Compare two associated types.
static int compareAssociatedTypes(AssociatedTypeDecl *assocType1,
                                  AssociatedTypeDecl *assocType2) {
  // - by name.
  if (int result = assocType1->getName().str().compare(
                                              assocType2->getName().str()))
    return result;

  // Prefer an associated type with no overrides (i.e., an anchor) to one
  // that has overrides.
  bool hasOverridden1 = !assocType1->getOverriddenDecls().empty();
  bool hasOverridden2 = !assocType2->getOverriddenDecls().empty();
  if (hasOverridden1 != hasOverridden2)
    return hasOverridden1 ? +1 : -1;

  // - by protocol, so t_n_m.`P.T` < t_n_m.`Q.T` (given P < Q)
  auto proto1 = assocType1->getProtocol();
  auto proto2 = assocType2->getProtocol();
  if (int compareProtocols = TypeDecl::compare(proto1, proto2))
    return compareProtocols;

  // Error case: if we have two associated types with the same name in the
  // same protocol, just tie-break based on address.
  if (assocType1 != assocType2)
    return assocType1 < assocType2 ? -1 : +1;

  return 0;
}

TypeDecl *EquivalenceClass::lookupNestedType(
                             GenericSignatureBuilder &builder,
                             Identifier name,
                             SmallVectorImpl<TypeDecl *> *otherConcreteTypes) {
  // Populates the result structures from the given cache entry.
  auto populateResult = [&](const CachedNestedType &cache) -> TypeDecl * {
    if (otherConcreteTypes)
      otherConcreteTypes->clear();

    // If there aren't any types in the cache, we're done.
    if (cache.types.empty()) return nullptr;

    // The first type in the cache is always the final result.
    // Collect the rest in the concrete-declarations list, if needed.
    if (otherConcreteTypes) {
      for (auto type : ArrayRef<TypeDecl *>(cache.types).slice(1)) {
        otherConcreteTypes->push_back(type);
      }
    }

    return cache.types.front();
  };

  // If we have a cached value that is up-to-date, use that.
  auto cached = nestedTypeNameCache.find(name);
  if (cached != nestedTypeNameCache.end() &&
      cached->second.numConformancesPresent == conformsTo.size() &&
      (!superclass ||
       cached->second.superclassPresent == superclass->getCanonicalType())) {
    ++NumNestedTypeCacheHits;
    return populateResult(cached->second);
  }

  // Cache miss; go compute the result.
  ++NumNestedTypeCacheMisses;

  // Look for types with the given name in protocols that we know about.
  AssociatedTypeDecl *bestAssocType = nullptr;
  llvm::SmallSetVector<AssociatedTypeDecl *, 4> assocTypeAnchors;
  SmallVector<TypeDecl *, 4> concreteDecls;
  for (const auto &conforms : conformsTo) {
    ProtocolDecl *proto = conforms.first;

    // Look for an associated type and/or concrete type with this name.
    for (auto member : proto->lookupDirect(name,
                                           /*ignoreNewExtensions=*/true)) {
      // If this is an associated type, record whether it is the best
      // associated type we've seen thus far.
      if (auto assocType = dyn_cast<AssociatedTypeDecl>(member)) {
        // Retrieve the associated type anchor.
        assocType = assocType->getAssociatedTypeAnchor();
        assocTypeAnchors.insert(assocType);

        if (!bestAssocType ||
             compareAssociatedTypes(assocType, bestAssocType) < 0)
          bestAssocType = assocType;

        continue;
      }

      // If this is another type declaration, determine whether we should
      // record it.
      if (auto type = dyn_cast<TypeDecl>(member)) {
        // FIXME: Filter out type declarations that aren't in the same
        // module as the protocol itself. This is an unprincipled hack, but
        // provides consistent lookup semantics for the generic signature
        // builder in all contents.
        if (type->getDeclContext()->getParentModule()
              != proto->getParentModule())
          continue;

        // Resolve the signature of this type.
        if (!type->hasInterfaceType()) {
          type->getASTContext().getLazyResolver()->resolveDeclSignature(type);
          if (!type->hasInterfaceType())
            continue;
        }

        concreteDecls.push_back(type);
        continue;
      }
    }
  }

  // If we haven't found anything yet but have a superclass, look for a type
  // in the superclass.
  // FIXME: Shouldn't we always look in the superclass?
  if (!bestAssocType && concreteDecls.empty() && superclass) {
    if (auto classDecl = superclass->getClassOrBoundGenericClass()) {
      SmallVector<ValueDecl *, 2> superclassMembers;
      classDecl->getParentModule()->lookupQualified(
          superclass, name,
          NL_QualifiedDefault | NL_OnlyTypes | NL_ProtocolMembers, nullptr,
          superclassMembers);
      for (auto member : superclassMembers) {
        if (auto type = dyn_cast<TypeDecl>(member)) {
          // Resolve the signature of this type.
          if (!type->hasInterfaceType()) {
            type->getASTContext().getLazyResolver()->resolveDeclSignature(type);
            if (!type->hasInterfaceType())
              continue;
          }

          concreteDecls.push_back(type);
        }
      }
    }
  }

  // Infer same-type constraints among same-named associated type anchors.
  if (assocTypeAnchors.size() > 1) {
    auto anchorType = getAnchor(builder, builder.getGenericParams());
    auto inferredSource = FloatingRequirementSource::forInferred(nullptr);
    for (auto assocType : assocTypeAnchors) {
      if (assocType == bestAssocType) continue;

      builder.addRequirement(
        Requirement(RequirementKind::SameType,
                    DependentMemberType::get(anchorType, bestAssocType),
                    DependentMemberType::get(anchorType, assocType)),
        inferredSource, nullptr);
    }
  }

  // Form the new cache entry.
  CachedNestedType entry;
  entry.numConformancesPresent = conformsTo.size();
  entry.superclassPresent =
    superclass ? superclass->getCanonicalType() : CanType();
  if (bestAssocType) {
    entry.types.push_back(bestAssocType);
    entry.types.insert(entry.types.end(),
                       concreteDecls.begin(), concreteDecls.end());
    assert(bestAssocType->getOverriddenDecls().empty() &&
           "Lookup should never keep a non-anchor associated type");
  } else if (!concreteDecls.empty()) {
    // Find the best concrete type.
    auto bestConcreteTypeIter =
      std::min_element(concreteDecls.begin(), concreteDecls.end(),
                       [](TypeDecl *type1, TypeDecl *type2) {
                         return TypeDecl::compare(type1, type2) < 0;
                       });

    // Put the best concrete type first; the rest will follow.
    entry.types.push_back(*bestConcreteTypeIter);
    entry.types.insert(entry.types.end(),
                       concreteDecls.begin(), bestConcreteTypeIter);
    entry.types.insert(entry.types.end(),
                       bestConcreteTypeIter + 1, concreteDecls.end());
  }

  return populateResult((nestedTypeNameCache[name] = std::move(entry)));
}

Type EquivalenceClass::getAnchor(
                            GenericSignatureBuilder &builder,
                            TypeArrayView<GenericTypeParamType> genericParams) {
  // Substitute into the anchor with the given generic parameters.
  auto substAnchor = [&] {
    if (genericParams.empty()) return archetypeAnchorCache.anchor;

    return archetypeAnchorCache.anchor.subst(
             [&](SubstitutableType *dependentType) {
               if (auto gp = dyn_cast<GenericTypeParamType>(dependentType)) {
                 unsigned index =
                   GenericParamKey(gp).findIndexIn(genericParams);
                 return Type(genericParams[index]);
               }

               return Type(dependentType);
             },
             MakeAbstractConformanceForGenericType());

  };

  // Check whether the cache is valid.
  if (archetypeAnchorCache.anchor &&
      archetypeAnchorCache.lastGeneration == builder.Impl->Generation) {
    ++NumArchetypeAnchorCacheHits;
    return substAnchor();
  }

  // Check whether we already have an anchor, in which case we
  // can simplify it further.
  if (archetypeAnchorCache.anchor) {
    // Record the cache miss.
    ++NumArchetypeAnchorCacheMisses;

    // Update the anchor by simplifying it further.
    archetypeAnchorCache.anchor =
      builder.getCanonicalTypeParameter(archetypeAnchorCache.anchor);
    archetypeAnchorCache.lastGeneration = builder.Impl->Generation;
    return substAnchor();
  }

  // Form the anchor.
  bool updatedAnchor = false;
  for (auto member : members) {
    auto anchorType =
      builder.getCanonicalTypeParameter(
                                    member->getDependentType(genericParams));

#ifndef NDEBUG
    // Check that we get consistent results from all of the anchors.
    if (updatedAnchor) {
      assert(anchorType->isEqual(archetypeAnchorCache.anchor) &&
             "Inconsistent anchor computation");
      continue;
    }
#endif

    // Record the cache miss and update the cache.
    ++NumArchetypeAnchorCacheMisses;
    archetypeAnchorCache.anchor = anchorType;
    archetypeAnchorCache.lastGeneration = builder.Impl->Generation;
    updatedAnchor = true;

#if NDEBUG
    break;
#endif
  }

  assert(updatedAnchor && "Couldn't update anchor?");
  return substAnchor();
}

Type EquivalenceClass::getTypeInContext(GenericSignatureBuilder &builder,
                                        GenericEnvironment *genericEnv) {
  auto genericParams = genericEnv->getGenericParams();

  // The anchor descr
  Type anchor = getAnchor(builder, genericParams);

  // If this equivalence class is mapped to a concrete type, produce that
  // type.
  if (concreteType) {
    if (recursiveConcreteType)
      return ErrorType::get(anchor);

    return genericEnv->mapTypeIntoContext(concreteType,
                                          builder.getLookupConformanceFn());
  }

  // Local function to check whether we have a generic parameter that has
  // already been recorded
  auto getAlreadyRecoveredGenericParam = [&]() -> Type {
    auto genericParam = anchor->getAs<GenericTypeParamType>();
    if (!genericParam) return Type();

    auto type = genericEnv->getMappingIfPresent(genericParam);
    if (!type) return Type();

    // We already have a mapping for this generic parameter in the generic
    // environment. Return it.
    return *type;
  };

  AssociatedTypeDecl *assocType = nullptr;
  ArchetypeType *parentArchetype = nullptr;
  if (auto depMemTy = anchor->getAs<DependentMemberType>()) {
    // Resolve the equivalence class of the parent.
    auto parentEquivClass =
      builder.resolveEquivalenceClass(
                          depMemTy->getBase(),
                          ArchetypeResolutionKind::CompleteWellFormed);
    if (!parentEquivClass)
      return ErrorType::get(anchor);

    // Map the parent type into this context.
    Type parentType = parentEquivClass->getTypeInContext(builder, genericEnv);

    // If the parent is concrete, handle the
    parentArchetype = parentType->getAs<ArchetypeType>();
    if (!parentArchetype) {
      // Resolve the member type.
      Type memberType =
        depMemTy->substBaseType(parentType, builder.getLookupConformanceFn());

      return genericEnv->mapTypeIntoContext(memberType,
                                            builder.getLookupConformanceFn());
    }

    // If we already have a nested type with this name, return it.
    assocType = depMemTy->getAssocType();
    if (auto nested =
          parentArchetype->getNestedTypeIfKnown(assocType->getName())) {
      return *nested;
    }

    // We will build the archetype below.
  } else if (auto result = getAlreadyRecoveredGenericParam()) {
    // Return already-contextualized generic type parameter.
    return result;
  }

  // Substitute into the superclass.
  Type superclass = this->recursiveSuperclassType ? Type() : this->superclass;
  if (superclass && superclass->hasTypeParameter()) {
    superclass = genericEnv->mapTypeIntoContext(
                                            superclass,
                                            builder.getLookupConformanceFn());
    if (superclass->is<ErrorType>())
      superclass = Type();

    // We might have recursively recorded the archetype; if so, return early.
    // FIXME: This should be detectable before we end up building archetypes.
    if (auto result = getAlreadyRecoveredGenericParam())
      return result;
  }

  // Build a new archetype.

  // Collect the protocol conformances for the archetype.
  SmallVector<ProtocolDecl *, 4> protos;
  for (const auto &conforms : conformsTo) {
    auto proto = conforms.first;

    if (!isConformanceSatisfiedBySuperclass(proto))
      protos.push_back(proto);
  }

  ArchetypeType *archetype;
  ASTContext &ctx = builder.getASTContext();
  if (parentArchetype) {
    // Create a nested archetype.
    auto *depMemTy = anchor->castTo<DependentMemberType>();
    archetype = ArchetypeType::getNew(ctx, parentArchetype, depMemTy, protos,
                                      superclass, layout);

    // Register this archetype with its parent.
    parentArchetype->registerNestedType(assocType->getName(), archetype);
  } else {
    // Create a top-level archetype.
    auto genericParam = anchor->castTo<GenericTypeParamType>();
    archetype = ArchetypeType::getNew(ctx, genericEnv, genericParam, protos,
                                      superclass, layout);

    // Register the archetype with the generic environment.
    genericEnv->addMapping(genericParam, archetype);
  }

  return archetype;
}

void EquivalenceClass::dump(llvm::raw_ostream &out,
                            GenericSignatureBuilder *builder) const {
  out << "Equivalence class represented by "
    << members.front()->getRepresentative()->getDebugName() << ":\n";
  out << "Members: ";
  interleave(members, [&](PotentialArchetype *pa) {
    out << pa->getDebugName();
  }, [&]() {
    out << ", ";
  });
  out << "\nConformances:";
  interleave(conformsTo,
             [&](const std::pair<
                        ProtocolDecl *,
                        std::vector<Constraint<ProtocolDecl *>>> &entry) {
               out << entry.first->getNameStr();
             },
             [&] { out << ", "; });
  out << "\nSame-type constraints:";
  interleave(sameTypeConstraints,
             [&](const Constraint<Type> &constraint) {
               out << "\n  " << constraint.getSubjectDependentType({ })
                   << " == " << constraint.value;

               if (constraint.source->isDerivedRequirement())
                 out << " [derived]";
             }, [&] {
               out << ", ";
             });
  if (concreteType)
    out << "\nConcrete type: " << concreteType.getString();
  if (superclass)
    out << "\nSuperclass: " << superclass.getString();
  if (layout)
    out << "\nLayout: " << layout.getString();

  if (!delayedRequirements.empty()) {
    out << "\nDelayed requirements:";
    for (const auto &req : delayedRequirements) {
      out << "\n  ";
      req.dump(out);
    }
  }

  out << "\n";

  if (builder) {
    CanType anchorType =
      const_cast<EquivalenceClass *>(this)->getAnchor(*builder, { })
        ->getCanonicalType();
    if (auto rewriteRoot =
          builder->Impl->getRewriteTreeRootIfPresent(anchorType)) {
      out << "---Rewrite tree---\n";
      rewriteRoot->dump(out);
    }
  }

  {
    out << "---GraphViz output for same-type constraints---\n";

    // Render the output
    std::string graphviz;
    {
      llvm::raw_string_ostream graphvizOut(graphviz);
      llvm::WriteGraph(graphvizOut, this);
    }

    // Clean up the output to turn it into an undirected graph.
    // FIXME: This is horrible, GraphWriter should be able to support
    // undirected graphs.
    auto digraphPos = graphviz.find("digraph");
    if (digraphPos != std::string::npos) {
      // digraph -> graph
      graphviz.erase(graphviz.begin() + digraphPos,
                     graphviz.begin() + digraphPos + 2);
    }

    // Directed edges to undirected edges: -> to --
    while (true) {
      auto arrowPos = graphviz.find("->");
      if (arrowPos == std::string::npos) break;

      graphviz.replace(arrowPos, 2, "--");
    }

    out << graphviz;
  }
}

void EquivalenceClass::dump(GenericSignatureBuilder *builder) const {
  dump(llvm::errs(), builder);
}

void DelayedRequirement::dump(llvm::raw_ostream &out) const {
  // Print LHS.
  if (auto lhsPA = lhs.dyn_cast<PotentialArchetype *>())
    out << lhsPA->getDebugName();
  else
    lhs.get<swift::Type>().print(out);

  switch (kind) {
  case Type:
  case Layout:
    out << ": ";
      break;

  case SameType:
    out << " == ";
    break;
  }

  // Print RHS.
  if (auto rhsPA = rhs.dyn_cast<PotentialArchetype *>())
    out << rhsPA->getDebugName();
  else if (auto rhsType = rhs.dyn_cast<swift::Type>())
    rhsType.print(out);
  else
    rhs.get<LayoutConstraint>().print(out);
}

void DelayedRequirement::dump() const {
  dump(llvm::errs());
  llvm::errs() << "\n";
}

ConstraintResult GenericSignatureBuilder::handleUnresolvedRequirement(
                                   RequirementKind kind,
                                   UnresolvedType lhs,
                                   UnresolvedRequirementRHS rhs,
                                   FloatingRequirementSource source,
                                   EquivalenceClass *unresolvedEquivClass,
                                   UnresolvedHandlingKind unresolvedHandling) {
  // Record the delayed requirement.
  DelayedRequirement::Kind delayedKind;
  switch (kind) {
  case RequirementKind::Conformance:
  case RequirementKind::Superclass:
    delayedKind = DelayedRequirement::Type;
    break;

  case RequirementKind::Layout:
    delayedKind = DelayedRequirement::Layout;
    break;

  case RequirementKind::SameType:
    delayedKind = DelayedRequirement::SameType;
    break;
  }

  if (unresolvedEquivClass) {
    unresolvedEquivClass->delayedRequirements.push_back(
                                          {delayedKind, lhs, rhs, source});
  } else {
    Impl->DelayedRequirements.push_back({delayedKind, lhs, rhs, source});
  }

  switch (unresolvedHandling) {
  case UnresolvedHandlingKind::GenerateConstraints:
    return ConstraintResult::Resolved;

  case UnresolvedHandlingKind::GenerateUnresolved:
    return ConstraintResult::Unresolved;
  }
}

bool GenericSignatureBuilder::addConditionalRequirements(
    ProtocolConformanceRef conformance, ModuleDecl *inferForModule,
    SourceLoc loc) {
  // Abstract conformances don't have associated decl-contexts/modules, but also
  // don't have conditional requirements.
  if (conformance.isConcrete()) {
    if (auto condReqs = conformance.getConditionalRequirementsIfAvailable()) {
      auto source = FloatingRequirementSource::forInferred(nullptr);
      for (auto requirement : *condReqs) {
        addRequirement(requirement, source, inferForModule);
        ++NumConditionalRequirementsAdded;
      }
    } else {
      if (loc.isValid())
        Diags.diagnose(loc, diag::unsupported_recursive_requirements);

      Impl->HadAnyError = true;
      return true;
    }
  }

  return false;
}

const RequirementSource *
GenericSignatureBuilder::resolveConcreteConformance(ResolvedType type,
                                                    ProtocolDecl *proto) {
  auto equivClass = type.getEquivalenceClass(*this);
  auto concrete = equivClass->concreteType;
  if (!concrete) return nullptr;

  // Conformance to this protocol is redundant; update the requirement source
  // appropriately.
  const RequirementSource *concreteSource;
  if (auto writtenSource =
        equivClass->findAnyConcreteConstraintAsWritten(nullptr))
    concreteSource = writtenSource->source;
  else
    concreteSource = equivClass->concreteTypeConstraints.front().source;

  // Lookup the conformance of the concrete type to this protocol.
  auto conformance =
      lookupConformance(type.getDependentType(*this)->getCanonicalType(),
                        concrete,
                        proto->getDeclaredInterfaceType()
                          ->castTo<ProtocolType>());
  if (!conformance) {
    if (!concrete->hasError() && concreteSource->getLoc().isValid()) {
      Impl->HadAnyError = true;

      Diags.diagnose(concreteSource->getLoc(),
                     diag::requires_generic_param_same_type_does_not_conform,
                     concrete, proto->getName());
    }

    Impl->HadAnyError = true;
    equivClass->invalidConcreteType = true;
    return nullptr;
  }

  concreteSource = concreteSource->viaConcrete(*this, *conformance);
  equivClass->recordConformanceConstraint(*this, type, proto, concreteSource);
  if (addConditionalRequirements(*conformance, /*inferForModule=*/nullptr,
                                 concreteSource->getLoc()))
    return nullptr;

  return concreteSource;
}
const RequirementSource *GenericSignatureBuilder::resolveSuperConformance(
                                                        ResolvedType type,
                                                        ProtocolDecl *proto) {
  // Get the superclass constraint.
  auto equivClass = type.getEquivalenceClass(*this);
  Type superclass = equivClass->superclass;
  if (!superclass) return nullptr;

  // Lookup the conformance of the superclass to this protocol.
  auto conformance =
    lookupConformance(type.getDependentType(*this)->getCanonicalType(),
                      superclass,
                      proto->getDeclaredInterfaceType()
                        ->castTo<ProtocolType>());
  if (!conformance) return nullptr;

  // Conformance to this protocol is redundant; update the requirement source
  // appropriately.
  const RequirementSource *superclassSource;
  if (auto writtenSource =
        equivClass->findAnySuperclassConstraintAsWritten())
    superclassSource = writtenSource->source;
  else
    superclassSource = equivClass->superclassConstraints.front().source;

  superclassSource =
    superclassSource->viaSuperclass(*this, *conformance);
  equivClass->recordConformanceConstraint(*this, type, proto, superclassSource);
  if (addConditionalRequirements(*conformance, /*inferForModule=*/nullptr,
                                 superclassSource->getLoc()))
    return nullptr;

  return superclassSource;
}

/// Realize a potential archetype for this type parameter.
PotentialArchetype *ResolvedType::realizePotentialArchetype(
                                           GenericSignatureBuilder &builder) {
  // Realize and cache the potential archetype.
  return builder.realizePotentialArchetype(type);
}

Type ResolvedType::getDependentType(GenericSignatureBuilder &builder) const {
  // Already-resolved potential archetype.
  if (auto pa = type.dyn_cast<PotentialArchetype *>())
    return pa->getDependentType(builder.getGenericParams());

  Type result = type.get<Type>();
  return result->isTypeParameter() ? result : Type();
}

/// If there is a same-type requirement to be added for the given nested type
/// due to a superclass constraint on the parent type, add it now.
static void maybeAddSameTypeRequirementForNestedType(
                                          ResolvedType nested,
                                          const RequirementSource *superSource,
                                          GenericSignatureBuilder &builder) {
  // If there's no super conformance, we're done.
  if (!superSource) return;

  // If the nested type is already concrete, we're done.
  if (nested.getAsConcreteType()) return;

  // Dig out the associated type.
  AssociatedTypeDecl *assocType = nullptr;
  if (auto depMemTy =
        nested.getDependentType(builder)->getAs<DependentMemberType>())
    assocType = depMemTy->getAssocType();
  else
    return;

  // Dig out the type witness.
  auto superConformance = superSource->getProtocolConformance().getConcrete();
  auto concreteType =
    superConformance->getTypeWitness(assocType, builder.getLazyResolver());
  if (!concreteType) return;

  // We should only have interface types here.
  assert(!superConformance->getType()->hasArchetype());
  assert(!concreteType->hasArchetype());

  // Add the same-type constraint.
  auto nestedSource = superSource->viaParent(builder, assocType);

  builder.addSameTypeRequirement(
        nested.getUnresolvedType(), concreteType, nestedSource,
        GenericSignatureBuilder::UnresolvedHandlingKind::GenerateConstraints);
}

auto PotentialArchetype::getOrCreateEquivalenceClass(
                                       GenericSignatureBuilder &builder) const
    -> EquivalenceClass * {
  // The equivalence class is stored on the representative.
  auto representative = getRepresentative();
  if (representative != this)
    return representative->getOrCreateEquivalenceClass(builder);

  // If we already have an equivalence class, return it.
  if (auto equivClass = getEquivalenceClassIfPresent())
    return equivClass;

  // Create a new equivalence class.
  auto equivClass =
    builder.Impl->allocateEquivalenceClass(
      const_cast<PotentialArchetype *>(this));
  representativeOrEquivClass = equivClass;
  return equivClass;
}

auto PotentialArchetype::getRepresentative() const -> PotentialArchetype * {
  auto representative =
    representativeOrEquivClass.dyn_cast<PotentialArchetype *>();
  if (!representative)
    return const_cast<PotentialArchetype *>(this);

  // Find the representative.
  PotentialArchetype *result = representative;
  while (auto nextRepresentative =
           result->representativeOrEquivClass.dyn_cast<PotentialArchetype *>())
    result = nextRepresentative;

  // Perform (full) path compression.
  const PotentialArchetype *fixUp = this;
  while (auto nextRepresentative =
           fixUp->representativeOrEquivClass.dyn_cast<PotentialArchetype *>()) {
    fixUp->representativeOrEquivClass = nextRepresentative;
    fixUp = nextRepresentative;
  }

  return result;
}

/// Canonical ordering for dependent types.
int swift::compareDependentTypes(Type type1, Type type2) {
  // Fast-path check for equality.
  if (type1->isEqual(type2)) return 0;

  // Ordering is as follows:
  // - Generic params
  auto gp1 = type1->getAs<GenericTypeParamType>();
  auto gp2 = type2->getAs<GenericTypeParamType>();
  if (gp1 && gp2)
    return GenericParamKey(gp1) < GenericParamKey(gp2) ? -1 : +1;

  // A generic parameter is always ordered before a nested type.
  if (static_cast<bool>(gp1) != static_cast<bool>(gp2))
    return gp1 ? -1 : +1;

  // - Dependent members
  auto depMemTy1 = type1->castTo<DependentMemberType>();
  auto depMemTy2 = type2->castTo<DependentMemberType>();

  // - by base, so t_0_n.`P.T` < t_1_m.`P.T`
  if (int compareBases =
        compareDependentTypes(depMemTy1->getBase(), depMemTy2->getBase()))
    return compareBases;

  // - by name, so t_n_m.`P.T` < t_n_m.`P.U`
  if (int compareNames = depMemTy1->getName().str().compare(
                                                  depMemTy2->getName().str()))
    return compareNames;

  auto *assocType1 = depMemTy1->getAssocType();
  auto *assocType2 = depMemTy2->getAssocType();
  if (int result = compareAssociatedTypes(assocType1, assocType2))
    return result;

  return 0;
}

/// Compare two dependent paths to determine which is better.
static int compareDependentPaths(ArrayRef<AssociatedTypeDecl *> path1,
                                 ArrayRef<AssociatedTypeDecl *> path2) {
  // Shorter paths win.
  if (path1.size() != path2.size())
    return path1.size() < path2.size() ? -1 : 1;

  // The paths are the same length, so order by comparing the associted
  // types.
  for (unsigned index : indices(path1)) {
    if (int result = compareAssociatedTypes(path1[index], path2[index]))
      return result;
  }

  // Identical paths.
  return 0;
}

namespace {
  /// Function object used to suppress conflict diagnoses when we know we'll
  /// see them again later.
  struct SameTypeConflictCheckedLater {
    void operator()(Type type1, Type type2) const { }
  };
} // end anonymous namespace

// Give a nested type the appropriately resolved concrete type, based off a
// parent PA that has a concrete type.
static void concretizeNestedTypeFromConcreteParent(
    GenericSignatureBuilder::PotentialArchetype *parent,
    GenericSignatureBuilder::PotentialArchetype *nestedPA,
    GenericSignatureBuilder &builder) {
  auto parentEquiv = parent->getEquivalenceClassIfPresent();
  assert(parentEquiv && "can't have a concrete type without an equiv class");
  auto concreteParent = parentEquiv->concreteType;
  assert(concreteParent &&
         "attempting to resolve concrete nested type of non-concrete PA");

  // These requirements are all implied based on the parent's concrete
  // conformance.
  auto assocType = nestedPA->getResolvedType();
  if (!assocType) return;

  auto proto = assocType->getProtocol();

  // If we don't already have a conformance of the parent to this protocol,
  // add it now; it was elided earlier.
  if (parentEquiv->conformsTo.count(proto) == 0) {
    auto source = parentEquiv->concreteTypeConstraints.front().source;
    parentEquiv->recordConformanceConstraint(builder, parent, proto, source);
  }

  assert(parentEquiv->conformsTo.count(proto) > 0 &&
         "No conformance requirement");
  const RequirementSource *parentConcreteSource = nullptr;
  for (const auto &constraint : parentEquiv->conformsTo.find(proto)->second) {
    if (constraint.source->kind == RequirementSource::Concrete) {
      parentConcreteSource = constraint.source;
    }
  }

  // Error condition: parent did not conform to this protocol, so there is no
  // way to resolve the nested type via concrete conformance.
  if (!parentConcreteSource) return;

  auto source = parentConcreteSource->viaParent(builder, assocType);
  auto conformance = parentConcreteSource->getProtocolConformance();

  Type witnessType;
  if (conformance.isConcrete()) {
    witnessType =
      conformance.getConcrete()
        ->getTypeWitness(assocType, builder.getLazyResolver());
    if (!witnessType || witnessType->hasError())
      return; // FIXME: should we delay here?
  } else {
    witnessType = DependentMemberType::get(concreteParent, assocType);
  }

  builder.addSameTypeRequirement(
         nestedPA, witnessType, source,
         GenericSignatureBuilder::UnresolvedHandlingKind::GenerateConstraints,
         SameTypeConflictCheckedLater());
}

PotentialArchetype *PotentialArchetype::updateNestedTypeForConformance(
    GenericSignatureBuilder &builder, AssociatedTypeDecl *assocType,
    ArchetypeResolutionKind kind) {
  if (!assocType)
    return nullptr;

  // Always refer to the archetype anchor.
  if (assocType)
    assocType = assocType->getAssociatedTypeAnchor();

  // If we were asked for a complete, well-formed archetype, make sure we
  // process delayed requirements if anything changed.
  SWIFT_DEFER {
    if (kind == ArchetypeResolutionKind::CompleteWellFormed)
      builder.processDelayedRequirements();
  };

  Identifier name = assocType->getName();
  auto *proto = assocType->getProtocol();

  // Look for either an unresolved potential archetype (which we can resolve
  // now) or a potential archetype with the appropriate associated type.
  PotentialArchetype *resultPA = nullptr;
  auto knownNestedTypes = NestedTypes.find(name);
  bool shouldUpdatePA = false;
  if (knownNestedTypes != NestedTypes.end()) {
    for (auto existingPA : knownNestedTypes->second) {
      // Do we have an associated-type match?
      if (assocType && existingPA->getResolvedType() == assocType) {
        resultPA = existingPA;
        break;
      }
    }
  }

  // If we don't have a result potential archetype yet, we may need to add one.
  if (!resultPA) {
    switch (kind) {
    case ArchetypeResolutionKind::CompleteWellFormed:
    case ArchetypeResolutionKind::WellFormed: {
      // Creating a new potential archetype in an equivalence class is a
      // modification.
      getOrCreateEquivalenceClass(builder)->modified(builder);

      void *mem = builder.Impl->Allocator.Allocate<PotentialArchetype>();
      resultPA = new (mem) PotentialArchetype(this, assocType);

      NestedTypes[name].push_back(resultPA);
      builder.addedNestedType(resultPA);
      shouldUpdatePA = true;
      break;
    }

    case ArchetypeResolutionKind::AlreadyKnown:
      return nullptr;
    }
  }

  // If we have a potential archetype that requires more processing, do so now.
  if (shouldUpdatePA) {
    // If there's a superclass constraint that conforms to the protocol,
    // add the appropriate same-type relationship.
    if (proto) {
      if (auto superSource = builder.resolveSuperConformance(this, proto)) {
        maybeAddSameTypeRequirementForNestedType(resultPA, superSource,
                                                 builder);
      }
    }

    // We know something concrete about the parent PA, so we need to propagate
    // that information to this new archetype.
    if (isConcreteType()) {
      concretizeNestedTypeFromConcreteParent(this, resultPA, builder);
    }
  }

  return resultPA;
}

void ArchetypeType::resolveNestedType(
                                    std::pair<Identifier, Type> &nested) const {
  auto genericEnv = getGenericEnvironment();
  auto &builder = *genericEnv->getGenericSignatureBuilder();

  Type interfaceType = getInterfaceType();
  Type memberInterfaceType =
    DependentMemberType::get(interfaceType, nested.first);
  auto equivClass =
    builder.resolveEquivalenceClass(
                                  memberInterfaceType,
                                  ArchetypeResolutionKind::CompleteWellFormed);
  auto result = equivClass->getTypeInContext(builder, genericEnv);
  assert(!nested.second ||
         nested.second->isEqual(result) ||
         (nested.second->hasError() && result->hasError()));
  nested.second = result;
}

Type GenericSignatureBuilder::PotentialArchetype::getDependentType(
                      TypeArrayView<GenericTypeParamType> genericParams) const {
  if (auto parent = getParent()) {
    Type parentType = parent->getDependentType(genericParams);
    if (parentType->hasError())
      return parentType;

    // If we've resolved to an associated type, use it.
    if (auto assocType = getResolvedType())
      return DependentMemberType::get(parentType, assocType);

    return DependentMemberType::get(parentType, getNestedName());
  }
  
  assert(isGenericParam() && "Not a generic parameter?");

  if (genericParams.empty()) {
    return GenericTypeParamType::get(getGenericParamKey().Depth,
                                     getGenericParamKey().Index,
                                     getASTContext());
  }

  unsigned index = getGenericParamKey().findIndexIn(genericParams);
  return genericParams[index];
}

ASTContext &PotentialArchetype::getASTContext() const {
  if (auto context = parentOrContext.dyn_cast<ASTContext *>())
    return *context;

  return getResolvedType()->getASTContext();
}

void GenericSignatureBuilder::PotentialArchetype::dump() const {
  dump(llvm::errs(), nullptr, 0);
}

void GenericSignatureBuilder::PotentialArchetype::dump(llvm::raw_ostream &Out,
                                                       SourceManager *SrcMgr,
                                                       unsigned Indent) const {
  // Print name.
  if (Indent == 0 || isGenericParam())
    Out << getDebugName();
  else
    Out.indent(Indent) << getNestedName();

  auto equivClass = getEquivalenceClassIfPresent();

  // Print superclass.
  if (equivClass && equivClass->superclass) {
    for (const auto &constraint : equivClass->superclassConstraints) {
      if (!constraint.isSubjectEqualTo(this)) continue;

      Out << " : ";
      constraint.value.print(Out);

      Out << " ";
      if (!constraint.source->isDerivedRequirement())
        Out << "*";
      Out << "[";
      constraint.source->print(Out, SrcMgr);
      Out << "]";
    }
  }

  // Print concrete type.
  if (equivClass && equivClass->concreteType) {
    for (const auto &constraint : equivClass->concreteTypeConstraints) {
      if (!constraint.isSubjectEqualTo(this)) continue;

      Out << " == ";
      constraint.value.print(Out);

      Out << " ";
      if (!constraint.source->isDerivedRequirement())
        Out << "*";
      Out << "[";
      constraint.source->print(Out, SrcMgr);
      Out << "]";
    }
  }

  // Print requirements.
  if (equivClass) {
    bool First = true;
    for (const auto &entry : equivClass->conformsTo) {
      for (const auto &constraint : entry.second) {
        if (!constraint.isSubjectEqualTo(this)) continue;

        if (First) {
          First = false;
          Out << ": ";
        } else {
          Out << " & ";
        }

        Out << constraint.value->getName().str() << " ";
        if (!constraint.source->isDerivedRequirement())
          Out << "*";
        Out << "[";
        constraint.source->print(Out, SrcMgr);
        Out << "]";
      }
    }
  }

  if (getRepresentative() != this) {
    Out << " [represented by " << getRepresentative()->getDebugName() << "]";
  }

  if (getEquivalenceClassMembers().size() > 1) {
    Out << " [equivalence class ";
    bool isFirst = true;
    for (auto equiv : getEquivalenceClassMembers()) {
      if (equiv == this) continue;

      if (isFirst) isFirst = false;
      else Out << ", ";

      Out << equiv->getDebugName();
    }
    Out << "]";
  }

  Out << "\n";

  // Print nested types.
  for (const auto &nestedVec : NestedTypes) {
    for (auto nested : nestedVec.second) {
      nested->dump(Out, SrcMgr, Indent + 2);
    }
  }
}

#pragma mark Rewrite tree
RewritePath::RewritePath(Optional<GenericParamKey> base,
                         RelativeRewritePath path,
                         PathOrder order)
  : base(base)
{
  switch (order) {
  case Forward:
    this->path.insert(this->path.begin(), path.begin(), path.end());
    break;

  case Reverse:
    this->path.insert(this->path.begin(), path.rbegin(), path.rend());
    break;
  }
}

RewritePath RewritePath::createPath(Type type) {
  SmallVector<AssociatedTypeDecl *, 4> path;
  auto genericParam = createPath(type, path);
  return RewritePath(genericParam, path, Reverse);
}

GenericParamKey RewritePath::createPath(
                                Type type,
                                SmallVectorImpl<AssociatedTypeDecl *> &path) {
  while (auto depMemTy = type->getAs<DependentMemberType>()) {
    auto assocType = depMemTy->getAssocType();
    assert(assocType && "Unresolved dependent member type");
    path.push_back(assocType);
    type = depMemTy->getBase();
  }

  return type->castTo<GenericTypeParamType>();
}

RewritePath RewritePath::commonPath(const RewritePath &other) const {
  assert(getBase().hasValue() && other.getBase().hasValue());

  if (*getBase() != *other.getBase()) return RewritePath();

  // Find the longest common prefix.
  RelativeRewritePath path1 = getPath();
  RelativeRewritePath path2 = other.getPath();
  if (path1.size() > path2.size())
    std::swap(path1, path2);
  unsigned prefixLength =
    std::mismatch(path1.begin(), path1.end(), path2.begin()).first
      - path1.begin();

  // Form the common path.
  return RewritePath(getBase(), path1.slice(0, prefixLength), Forward);
}

/// Form a dependent type with the given generic parameter, then following the
/// path of associated types.
static Type formDependentType(GenericTypeParamType *base,
                              RelativeRewritePath path) {
  return std::accumulate(path.begin(), path.end(), Type(base),
                         [](Type type, AssociatedTypeDecl *assocType) -> Type {
                           return DependentMemberType::get(type, assocType);
                         });
}

/// Form a dependent type with the (canonical) generic parameter for the given
/// parameter key, then following the path of associated types.
static Type formDependentType(ASTContext &ctx, GenericParamKey genericParam,
                              RelativeRewritePath path) {
  return formDependentType(GenericTypeParamType::get(genericParam.Depth,
                                                     genericParam.Index,
                                                     ctx),
                           path);
}

CanType RewritePath::formDependentType(
                                     ASTContext &ctx,
                                     AnchorPathCache *anchorPathCache) const {
  if (auto base = getBase())
    return CanType(::formDependentType(ctx, *base, getPath()));

  assert(anchorPathCache && "Need an anchor path cache");
  const RewritePath &anchorPath = anchorPathCache->getAnchorPath();

  // Add the relative path to the anchor path.
  SmallVector<AssociatedTypeDecl *, 4> absolutePath;
  absolutePath.append(anchorPath.getPath().begin(),
                      anchorPath.getPath().end());
  absolutePath.append(getPath().begin(), getPath().end());
  return CanType(::formDependentType(ctx, *anchorPath.getBase(),
                                     absolutePath));

}

int RewritePath::compare(const RewritePath &other) const {
  // Prefer relative to absolute paths.
  if (getBase().hasValue() != other.getBase().hasValue()) {
    return other.getBase().hasValue() ? -1 : 1;
  }

  // Order based on the bases.
  if (getBase() && *getBase() != *other.getBase())
    return (*getBase() < *other.getBase()) ? -1 : 1;

  // Order based on the path contents.
  return compareDependentPaths(getPath(), other.getPath());
}

void RewritePath::print(llvm::raw_ostream &out) const {
  out << "[";

  if (getBase()) {
    out << "(" << getBase()->Depth << ", " << getBase()->Index << ")";
    if (!getPath().empty()) out << " -> ";
  }

  interleave(getPath().begin(), getPath().end(),
             [&](AssociatedTypeDecl *assocType) {
               out.changeColor(raw_ostream::BLUE);
               out << assocType->getProtocol()->getName() << "."
               << assocType->getName();
               out.resetColor();
             }, [&] {
               out << " -> ";
             });
  out << "]";
}

RewriteTreeNode::~RewriteTreeNode() {
  for (auto child : children)
    delete child;
}

namespace {
/// Function object used to order rewrite tree nodes based on the address
/// of the associated type.
class OrderTreeRewriteNode {
  bool compare(AssociatedTypeDecl *lhs, AssociatedTypeDecl *rhs) const {
    // Make sure null pointers precede everything else.
    if (static_cast<bool>(lhs) != static_cast<bool>(rhs))
      return static_cast<bool>(rhs);

    // Use std::less to provide a defined ordering.
    return std::less<AssociatedTypeDecl *>()(lhs, rhs);
  }

public:
  bool operator()(RewriteTreeNode *lhs, AssociatedTypeDecl *rhs) const {
    return compare(lhs->getMatch(), rhs);
  }

  bool operator()(AssociatedTypeDecl *lhs, RewriteTreeNode *rhs) const {
    return compare(lhs, rhs->getMatch());
  }

  bool operator()(RewriteTreeNode *lhs, RewriteTreeNode *rhs) const {
    return compare(lhs->getMatch(), rhs->getMatch());
  }
};
}

bool RewriteTreeNode::addRewriteRule(RelativeRewritePath matchPath,
                                     RewritePath replacementPath) {
  // If the match path is empty, we're adding the rewrite rule to this node.
  if (matchPath.empty()) {
    // If we don't already have a rewrite rule, add it.
    if (!hasRewriteRule()) {
      setRewriteRule(replacementPath);
      return true;
    }

    // If we already have this rewrite rule, we're done.
    if (getRewriteRule() == replacementPath) return false;

    // Check whether any of the continuation children matches.
    auto insertPos = children.begin();
    while (insertPos != children.end() && !(*insertPos)->getMatch()) {
      if ((*insertPos)->hasRewriteRule() &&
          (*insertPos)->getRewriteRule() == replacementPath)
        return false;

      ++insertPos;
    }

    // We already have a rewrite rule, so add a new child with a
    // null associated type match to hold the rewrite rule.
    auto newChild = new RewriteTreeNode(nullptr);
    newChild->setRewriteRule(replacementPath);
    children.insert(insertPos, newChild);
    return true;
  }

  // Find (or create) a child node describing the next step in the match.
  auto matchFront = matchPath.front();
  auto childPos =
    std::lower_bound(children.begin(), children.end(), matchFront,
                     OrderTreeRewriteNode());
  if (childPos == children.end() || (*childPos)->getMatch() != matchFront) {
    childPos = children.insert(childPos, new RewriteTreeNode(matchFront));
  }

  // Add the rewrite rule to the child.
  return (*childPos)->addRewriteRule(matchPath.slice(1), replacementPath);
}

void RewriteTreeNode::enumerateRewritePathsImpl(
                       RelativeRewritePath matchPath,
                       llvm::function_ref<void(unsigned, RewritePath)> callback,
                       unsigned depth) const {
  // Determine whether we know anything about the next step in the path.
  auto childPos =
    depth < matchPath.size()
      ? std::lower_bound(children.begin(), children.end(),
                         matchPath[depth], OrderTreeRewriteNode())
      : children.end();
  if (childPos != children.end() &&
      (*childPos)->getMatch() == matchPath[depth]) {
    // Try to match the rest of the path.
    (*childPos)->enumerateRewritePathsImpl(matchPath, callback, depth + 1);
  }

  // If we have a rewrite rule at this position, invoke it.
  if (hasRewriteRule()) {
    // Invoke the callback with the first result.
    callback(depth, rewrite);
  }

  // Walk any children with NULL associated types; they might have more matches.
  for (auto otherRewrite : children) {
    if (otherRewrite->getMatch()) break;
    otherRewrite->enumerateRewritePathsImpl(matchPath, callback, depth);
  }
}

Optional<std::pair<unsigned, RewritePath>>
RewriteTreeNode::bestRewritePath(GenericParamKey base, RelativeRewritePath path,
                           unsigned prefixLength) {
  Optional<std::pair<unsigned, RewritePath>> best;
  unsigned bestAdjustedLength = 0;
  enumerateRewritePaths(path,
                        [&](unsigned length, RewritePath path) {
    // Determine how much of the original path will be replaced by the rewrite.
    unsigned adjustedLength = length;
    bool changesBase = false;
    if (auto newBase = path.getBase()) {
      adjustedLength += prefixLength;

      // If the base is unchanged, make sure we're reducing the length.
      changesBase = *newBase != base;
      if (!changesBase && adjustedLength <= path.getPath().size())
        return;
    }

    if (adjustedLength == 0 && !changesBase) return;

    if (adjustedLength > bestAdjustedLength || !best ||
        (adjustedLength == bestAdjustedLength &&
         path.compare(best->second) < 0)) {
      best = { length, path };
      bestAdjustedLength = adjustedLength;
    }
  });

  return best;
}

bool RewriteTreeNode::mergeInto(RewriteTreeNode *other) {
  // FIXME: A destructive version of this operation would be more efficient,
  // since we generally don't care about \c other after doing this.
  bool anyAdded = false;
  (void)enumerateRules([other, &anyAdded](RelativeRewritePath lhs,
                                          const RewritePath &rhs) {
    if (other->addRewriteRule(lhs, rhs))
      anyAdded = true;
    return RuleAction::none();
  });

  return anyAdded;
}

bool RewriteTreeNode::enumerateRulesRec(
                            llvm::function_ref<EnumerateCallback> &fn,
                            bool temporarilyDisableVisitedRule,
                            llvm::SmallVectorImpl<AssociatedTypeDecl *> &lhs) {
  if (auto assocType = getMatch())
    lhs.push_back(assocType);

  SWIFT_DEFER {
    if (auto assocType = getMatch())
      lhs.pop_back();
  };

  // If there is a rewrite rule, invoke the callback.
  if (hasRewriteRule()) {
    // If we're supposed to temporarily disabled the visited rule, do so
    // now.
    Optional<RewritePath> rewriteRule;
    if (temporarilyDisableVisitedRule) {
      rewriteRule = std::move(*this).getRewriteRule();
      removeRewriteRule();
    }

    // Make sure that we put the rewrite rule back in place if we moved it
    // aside.
    SWIFT_DEFER {
      if (temporarilyDisableVisitedRule && rewriteRule)
        setRewriteRule(*std::move(rewriteRule));
    };

    switch (auto action =
                fn(lhs, rewriteRule ? *rewriteRule : getRewriteRule())) {
    case RuleAction::None:
      break;

    case RuleAction::Stop:
      return true;

    case RuleAction::Remove:
      if (temporarilyDisableVisitedRule)
        rewriteRule = None;
      else
        removeRewriteRule();
      break;

    case RuleAction::Replace:
      if (temporarilyDisableVisitedRule) {
        rewriteRule = std::move(action.path);
      } else {
        removeRewriteRule();
        setRewriteRule(action.path);
      }
      break;
    }
  }

  // Recurse into the child nodes.
  for (auto child : children) {
    if (child->enumerateRulesRec(fn, temporarilyDisableVisitedRule, lhs))
      return true;
  }

  return false;
}

void RewriteTreeNode::dump() const {
  dump(llvm::errs());
}

void RewriteTreeNode::dump(llvm::raw_ostream &out, bool lastChild) const {
  std::string prefixStr;

  std::function<void(const RewriteTreeNode *, bool lastChild)> print;
  print = [&](const RewriteTreeNode *node, bool lastChild) {
    out << prefixStr << " `--";

    // Print the node name.
    out.changeColor(raw_ostream::GREEN);
    if (auto assoc = node->getMatch())
      out << assoc->getProtocol()->getName() << "." << assoc->getName();
    else
      out << "(cont'd)";
    out.resetColor();

    // Print the rewrite, if there is one.
    if (node->hasRewriteRule()) {
      out << " --> ";
      node->rewrite.print(out);
    }

    out << "\n";

    // Print children.
    prefixStr += ' ';
    prefixStr += (lastChild ? ' ' : '|');
    prefixStr += "  ";

    for (auto child : node->children) {
      print(child, child == node->children.back());
    }

    prefixStr.erase(prefixStr.end() - 4, prefixStr.end());
  };

  print(this, lastChild);
}

RewriteTreeNode *
GenericSignatureBuilder::Implementation::getRewriteTreeRootIfPresent(
                                          CanType anchor) {
  auto known = RewriteTreeRoots.find(anchor);
  if (known != RewriteTreeRoots.end()) return known->second.get();

  return nullptr;
}

RewriteTreeNode *
GenericSignatureBuilder::Implementation::getOrCreateRewriteTreeRoot(
                                          CanType anchor) {
  auto known = RewriteTreeRoots.find(anchor);
  if (known != RewriteTreeRoots.end()) return known->second.get();

  auto &root = RewriteTreeRoots[anchor];
  root = std::unique_ptr<RewriteTreeNode>(new RewriteTreeNode(nullptr));
  return root.get();
}

void GenericSignatureBuilder::Implementation::minimizeRewriteTree(
                                            GenericSignatureBuilder &builder) {
  // Only perform minimization if the term-rewriting tree has changed.
  if (LastRewriteMinimizedGeneration == RewriteGeneration
      || MinimizingRewriteSystem)
    return;

  ++NumRewriteMinimizations;
  llvm::SaveAndRestore<bool> minimizingRewriteSystem(MinimizingRewriteSystem,
                                                     true);
  SWIFT_DEFER {
    LastRewriteMinimizedGeneration = RewriteGeneration;
  };

  minimizeRewriteTreeRhs(builder);
  removeRewriteTreeRedundancies(builder);
}

void GenericSignatureBuilder::Implementation::minimizeRewriteTreeRhs(
                                            GenericSignatureBuilder &builder) {
  assert(MinimizingRewriteSystem);

  // Minimize the right-hand sides of each rule in the tree.
  for (auto &equivClass : EquivalenceClasses) {
    CanType anchorType = equivClass.getAnchor(builder, { })->getCanonicalType();
    auto root = RewriteTreeRoots.find(anchorType);
    if (root == RewriteTreeRoots.end()) continue;

    AnchorPathCache anchorPathCache(builder, equivClass);

    ASTContext &ctx = builder.getASTContext();
    root->second->enumerateRules([&](RelativeRewritePath lhs,
                                     const RewritePath &rhs) {
      // Compute the type of the right-hand side.
      Type rhsType = rhs.formDependentType(ctx, &anchorPathCache);
      if (!rhsType) return RewriteTreeNode::RuleAction::none();

      // Compute the canonical type for the right-hand side.
      Type canonicalRhsType = builder.getCanonicalTypeParameter(rhsType);

      // If the canonicalized result is equivalent to the right-hand side we
      // had, there's nothing to do.
      if (rhsType->isEqual(canonicalRhsType))
        return RewriteTreeNode::RuleAction::none();

      // We have a canonical replacement path. Determine its encoding and
      // perform the replacement.
      ++NumRewriteRhsSimplified;

      // Determine replacement path, which might be relative to the anchor.
      auto canonicalRhsPath = RewritePath::createPath(canonicalRhsType);
      auto anchorPath = anchorPathCache.getAnchorPath();
      if (auto prefix = anchorPath.commonPath(canonicalRhsPath)) {
        unsigned prefixLength = prefix.getPath().size();
        RelativeRewritePath replacementRhsPath =
          canonicalRhsPath.getPath().slice(prefixLength);

        // If the left and right-hand sides are equivalent, just remove the
        // rule.
        if (lhs == replacementRhsPath) {
          ++NumRewriteRhsSimplifiedToLhs;
          return RewriteTreeNode::RuleAction::remove();
        }

        RewritePath replacementRhs(None, replacementRhsPath,
                                   RewritePath::Forward);
        return RewriteTreeNode::RuleAction::replace(std::move(replacementRhs));
      }

      return RewriteTreeNode::RuleAction::replace(canonicalRhsPath);
    });
  }
}

void GenericSignatureBuilder::Implementation::removeRewriteTreeRedundancies(
                                            GenericSignatureBuilder &builder) {
  assert(MinimizingRewriteSystem);

  // Minimize the right-hand sides of each rule in the tree.
  for (auto &equivClass : EquivalenceClasses) {
    CanType anchorType = equivClass.getAnchor(builder, { })->getCanonicalType();
    auto root = RewriteTreeRoots.find(anchorType);
    if (root == RewriteTreeRoots.end()) continue;

    AnchorPathCache anchorPathCache(builder, equivClass);

    ASTContext &ctx = builder.getASTContext();
    root->second->enumerateRules([&](RelativeRewritePath lhs,
                                     const RewritePath &rhs) {
      /// Left-hand side type.
      Type lhsType = RewritePath(None, lhs, RewritePath::Forward)
                       .formDependentType(ctx, &anchorPathCache);
      if (!lhsType) return RewriteTreeNode::RuleAction::none();

      // Simplify the left-hand type.
      Type simplifiedLhsType = builder.getCanonicalTypeParameter(lhsType);

      // Compute the type of the right-hand side.
      Type rhsType = rhs.formDependentType(ctx, &anchorPathCache);
      if (!rhsType) return RewriteTreeNode::RuleAction::none();

      if (simplifiedLhsType->isEqual(rhsType)) {
        ++NumRewriteRulesRedundant;
        return RewriteTreeNode::RuleAction::remove();
      }

      return RewriteTreeNode::RuleAction::none();
    },
    /*temporarilyDisableVisitedRule=*/true);
  }
}

bool GenericSignatureBuilder::addSameTypeRewriteRule(CanType type1,
                                                     CanType type2) {
  // We already effectively have this rewrite rule.
  if (type1 == type2) return false;

  auto path1 = RewritePath::createPath(type1);
  auto path2 = RewritePath::createPath(type2);

  // Look for a common prefix. When we have one, form a rewrite rule using
  // relative paths.
  if (auto prefix = path1.commonPath(path2)) {
    // Find the better relative rewrite path.
    RelativeRewritePath relPath1
      = path1.getPath().slice(prefix.getPath().size());
    RelativeRewritePath relPath2
      = path2.getPath().slice(prefix.getPath().size());
    // Order the paths so that we go to the more-canonical path.
    if (compareDependentPaths(relPath1, relPath2) < 0)
      std::swap(relPath1, relPath2);

    // Find the anchor for the prefix.
    CanType commonType = prefix.formDependentType(getASTContext());
    CanType commonAnchor =
      getCanonicalTypeParameter(commonType)->getCanonicalType();

    // Add the rewrite rule.
    auto root = Impl->getOrCreateRewriteTreeRoot(commonAnchor);
    return root->addRewriteRule(
                          relPath1,
                          RewritePath(None, relPath2, RewritePath::Forward));
  }

  // Otherwise, form a rewrite rule with absolute paths.

  // Find the better path and make sure it's in path2.
  if (compareDependentTypes(type1, type2) < 0) {
    std::swap(path1, path2);
    std::swap(type1, type2);
  }

  // Add the rewrite rule.
  Type firstBase =
    GenericTypeParamType::get(path1.getBase()->Depth, path1.getBase()->Index,
                              getASTContext());
  CanType baseAnchor =
    getCanonicalTypeParameter(firstBase)->getCanonicalType();
  auto root = Impl->getOrCreateRewriteTreeRoot(baseAnchor);
  return root->addRewriteRule(path1.getPath(), path2);
}

Type GenericSignatureBuilder::getCanonicalTypeParameter(Type type) {
  auto initialPath = RewritePath::createPath(type);
  auto genericParamType =
    GenericTypeParamType::get(initialPath.getBase()->Depth,
                              initialPath.getBase()->Index,
                              getASTContext());

  unsigned startIndex = 0;
  Type currentType = genericParamType;
  SmallVector<AssociatedTypeDecl *, 4> path(initialPath.getPath().begin(),
                                            initialPath.getPath().end());
  bool simplified = false;
  do {
    CanType currentAnchor = currentType->getCanonicalType();
    if (auto rootNode = Impl->getRewriteTreeRootIfPresent(currentAnchor)) {
      // Find the best rewrite rule for the path starting at startIndex.
      auto match =
        rootNode->bestRewritePath(genericParamType,
                            llvm::makeArrayRef(path).slice(startIndex),
                            startIndex);

      // If we have a match, replace the matched path with the replacement
      // path.
      if (match) {
        // Determine the range in the path which we'll be replacing.
        unsigned replaceStartIndex = match->second.getBase() ? 0 : startIndex;
        unsigned replaceEndIndex = startIndex + match->first;

        // Overwrite the beginning of the match.
        auto replacementPath = match->second.getPath();
        assert((replaceEndIndex - replaceStartIndex) >= replacementPath.size());
        auto replacementStartPos = path.begin() + replaceStartIndex;
        std::copy(replacementPath.begin(), replacementPath.end(),
                  replacementStartPos);

        // Erase the rest.
        path.erase(replacementStartPos + replacementPath.size(),
                   path.begin() + replaceEndIndex);

        // If this is an absolute path, use the new base.
        if (auto newBase = match->second.getBase()) {
          genericParamType =
            GenericTypeParamType::get(newBase->Depth, newBase->Index,
                                      getASTContext());
        }

        // Move back to the beginning; we may have opened up other rewrites.
        simplified = true;
        startIndex = 0;
        currentType = genericParamType;
        continue;
      }
    }

    // If we've hit the end of the path, we're done.
    if (startIndex >= path.size()) break;

    currentType = DependentMemberType::get(currentType, path[startIndex++]);
  } while (true);

  return formDependentType(genericParamType, path);
}

#pragma mark Equivalence classes
EquivalenceClass::EquivalenceClass(PotentialArchetype *representative)
  : recursiveConcreteType(false), invalidConcreteType(false),
    recursiveSuperclassType(false)
{
  members.push_back(representative);
}

void EquivalenceClass::modified(GenericSignatureBuilder &builder) {
  builder.Impl->Generation++;

  // Transfer any delayed requirements to the primary queue, because they
  // might be resolvable now.
  builder.Impl->DelayedRequirements.append(delayedRequirements.begin(),
                                           delayedRequirements.end());
  delayedRequirements.clear();
}

GenericSignatureBuilder::GenericSignatureBuilder(
                               ASTContext &ctx)
  : Context(ctx), Diags(Context.Diags), Impl(new Implementation) {
  if (Context.Stats)
    Context.Stats->getFrontendCounters().NumGenericSignatureBuilders++;
}

GenericSignatureBuilder::GenericSignatureBuilder(
                                         GenericSignatureBuilder &&other)
  : Context(other.Context), Diags(other.Diags), Impl(std::move(other.Impl))
{
  other.Impl.reset();

  if (Impl) {
    // Update the generic parameters to their canonical types.
    for (auto &gp : Impl->GenericParams) {
      gp = gp->getCanonicalType()->castTo<GenericTypeParamType>();
    }
  }
}

GenericSignatureBuilder::~GenericSignatureBuilder() = default;

auto
GenericSignatureBuilder::getLookupConformanceFn()
    -> LookUpConformanceInBuilder {
  return LookUpConformanceInBuilder(this);
}

Optional<ProtocolConformanceRef>
GenericSignatureBuilder::lookupConformance(CanType dependentType,
                                           Type conformingReplacementType,
                                           ProtocolType *conformedProtocol) {
  if (conformingReplacementType->isTypeParameter())
    return ProtocolConformanceRef(conformedProtocol->getDecl());

  // Figure out which module to look into.
  // FIXME: When lookupConformance() starts respecting modules, we'll need
  // to do some filtering here.
  ModuleDecl *searchModule = conformedProtocol->getDecl()->getParentModule();
  auto result = searchModule->lookupConformance(conformingReplacementType,
                                                conformedProtocol->getDecl());
  if (result && getLazyResolver())
    getLazyResolver()->markConformanceUsed(*result, searchModule);

  return result;
}

LazyResolver *GenericSignatureBuilder::getLazyResolver() const { 
  return Context.getLazyResolver();
}

/// Resolve any unresolved dependent member types using the given builder.
static Type resolveDependentMemberTypes(GenericSignatureBuilder &builder,
                                        Type type) {
  if (!type->hasTypeParameter()) return type;

  return type.transformRec([&builder](TypeBase *type) -> Optional<Type> {
    if (!type->isTypeParameter())
      return None;

    auto resolved = builder.maybeResolveEquivalenceClass(
        Type(type), ArchetypeResolutionKind::WellFormed, true);

    if (!resolved)
      return ErrorType::get(Type(type));

    if (auto concreteType = resolved.getAsConcreteType())
      return concreteType;

    // Map the type parameter to an equivalence class.
    auto equivClass = resolved.getEquivalenceClass(builder);
    if (!equivClass)
      return ErrorType::get(Type(type));

    // If there is a concrete type in this equivalence class, use that.
    if (equivClass->concreteType) {
      // .. unless it's recursive.
      if (equivClass->recursiveConcreteType)
        return ErrorType::get(Type(type));

      return resolveDependentMemberTypes(builder, equivClass->concreteType);
    }

    return equivClass->getAnchor(builder, builder.getGenericParams());
  });
}

PotentialArchetype *GenericSignatureBuilder::realizePotentialArchetype(
                                                     UnresolvedType &type) {
  if (auto pa = type.dyn_cast<PotentialArchetype *>())
    return pa;

  auto pa = maybeResolveEquivalenceClass(type.get<Type>(),
                                         ArchetypeResolutionKind::WellFormed,
                                         /*wantExactPotentialArchetype=*/true)
    .getPotentialArchetypeIfKnown();
  if (pa) type = pa;

  return pa;
}

static Type substituteConcreteType(GenericSignatureBuilder &builder,
                                   PotentialArchetype *basePA,
                                   TypeDecl *concreteDecl) {
  assert(concreteDecl);

  auto *proto =
      concreteDecl->getDeclContext()->getAsProtocolOrProtocolExtensionContext();

  if (!concreteDecl->hasInterfaceType())
    builder.getLazyResolver()->resolveDeclSignature(concreteDecl);

  if (!concreteDecl->hasInterfaceType())
    return Type();

  // The protocol concrete type has an underlying type written in terms
  // of the protocol's 'Self' type.
  auto typealias = dyn_cast<TypeAliasDecl>(concreteDecl);
  auto type = typealias ? typealias->getUnderlyingTypeLoc().getType()
                        : concreteDecl->getDeclaredInterfaceType();

  Type parentType;
  SubstitutionMap subMap;
  if (proto) {
    // Substitute in the type of the current PotentialArchetype in
    // place of 'Self' here.
    parentType = basePA->getDependentType(builder.getGenericParams());

    subMap = SubstitutionMap::getProtocolSubstitutions(
        proto, parentType, ProtocolConformanceRef(proto));

    type = type.subst(subMap, SubstFlags::UseErrorType);
  } else {
    // Substitute in the superclass type.
    parentType = basePA->getEquivalenceClassIfPresent()->superclass;
    auto superclassDecl = parentType->getClassOrBoundGenericClass();

    subMap = parentType->getMemberSubstitutionMap(
                           superclassDecl->getParentModule(), concreteDecl);
    type = type.subst(subMap, SubstFlags::UseErrorType);
  }

  // If we had a typealias, form a sugared type.
  if (typealias) {
    type = NameAliasType::get(typealias, parentType, subMap, type);
  }

  return type;
};

ResolvedType GenericSignatureBuilder::maybeResolveEquivalenceClass(
                                    Type type,
                                    ArchetypeResolutionKind resolutionKind,
                                    bool wantExactPotentialArchetype) {
  // The equivalence class of a generic type is known directly.
  if (auto genericParam = type->getAs<GenericTypeParamType>()) {
    unsigned index = GenericParamKey(genericParam).findIndexIn(
                                                            getGenericParams());
    if (index < Impl->GenericParams.size()) {
      return ResolvedType(Impl->PotentialArchetypes[index]);
    }

    return ResolvedType::forUnresolved(nullptr);
  }

  // The equivalence class of a dependent member type is determined by its
  // base equivalence class.
  if (auto depMemTy = type->getAs<DependentMemberType>()) {
    // Find the equivalence class of the base.
    auto resolvedBase =
      maybeResolveEquivalenceClass(depMemTy->getBase(),
                                   resolutionKind,
                                   wantExactPotentialArchetype);
    if (!resolvedBase) return resolvedBase;

    // Find the nested type declaration for this.
    auto baseEquivClass = resolvedBase.getEquivalenceClass(*this);

    // Retrieve the "smallest" type in the equivalence class, by depth, and
    // use that to find a nested potential archetype. We used the smallest
    // type by depth to limit expansion of the type graph.
    PotentialArchetype *basePA;
    if (wantExactPotentialArchetype) {
      basePA = resolvedBase.getPotentialArchetypeIfKnown();
      if (!basePA)
        return ResolvedType::forUnresolved(baseEquivClass);
    } else {
      basePA = baseEquivClass->members.front();
    }

    AssociatedTypeDecl *nestedTypeDecl = nullptr;
    if (auto assocType = depMemTy->getAssocType()) {
      // Check whether this associated type references a protocol to which
      // the base conforms. If not, it's unresolved.
      if (baseEquivClass->conformsTo.find(assocType->getProtocol())
            == baseEquivClass->conformsTo.end())
        return ResolvedType::forUnresolved(baseEquivClass);

      nestedTypeDecl = assocType;
    } else {
      auto *typeAlias =
          baseEquivClass->lookupNestedType(*this, depMemTy->getName());

      if (!typeAlias)
        return ResolvedType::forUnresolved(baseEquivClass);

      auto type = substituteConcreteType(*this, basePA, typeAlias);
      return maybeResolveEquivalenceClass(type, resolutionKind,
                                          wantExactPotentialArchetype);
    }

    auto nestedPA =
      basePA->updateNestedTypeForConformance(*this, nestedTypeDecl,
                                             resolutionKind);
    if (!nestedPA)
      return ResolvedType::forUnresolved(baseEquivClass);

    // If base resolved to the anchor, then the nested potential archetype
    // we found is the resolved potential archetype. Return it directly,
    // so it doesn't need to be resolved again.
    if (basePA == resolvedBase.getPotentialArchetypeIfKnown())
      return ResolvedType(nestedPA);

    // Compute the resolved dependent type to return.
    Type resolvedBaseType = resolvedBase.getDependentType(*this);
    Type resolvedMemberType;
    if (auto assocType = dyn_cast<AssociatedTypeDecl>(nestedTypeDecl)) {
      resolvedMemberType =
        DependentMemberType::get(resolvedBaseType, assocType);
    } else {
      // Note: strange case that might not even really be dependent.
      resolvedMemberType =
        DependentMemberType::get(resolvedBaseType, depMemTy->getName());
    }

    return ResolvedType(resolvedMemberType,
                         nestedPA->getOrCreateEquivalenceClass(*this));
  }

  // If it's not a type parameter, it won't directly resolve to one.
  // FIXME: Generic typealiases contradict the assumption above.
  // If there is a type parameter somewhere in this type, resolve it.
  if (type->hasTypeParameter()) {
    Type resolved = resolveDependentMemberTypes(*this, type);
    if (resolved->hasError() && !type->hasError())
      return ResolvedType::forUnresolved(nullptr);

    type = resolved;
  }

  return ResolvedType::forConcrete(type);
}

EquivalenceClass *GenericSignatureBuilder::resolveEquivalenceClass(
                                    Type type,
                                    ArchetypeResolutionKind resolutionKind) {
  if (auto resolved =
        maybeResolveEquivalenceClass(type, resolutionKind,
                                     /*wantExactPotentialArchetype=*/false))
    return resolved.getEquivalenceClass(*this);

  return nullptr;
}

auto GenericSignatureBuilder::resolve(UnresolvedType paOrT,
                                      FloatingRequirementSource source)
    -> ResolvedType {
  if (auto pa = paOrT.dyn_cast<PotentialArchetype *>())
    return ResolvedType(pa);

  // Determine what kind of resolution we want.
    Type type = paOrT.dyn_cast<Type>();
  ArchetypeResolutionKind resolutionKind =
    ArchetypeResolutionKind::WellFormed;
  if (!source.isExplicit() && source.isRecursive(type, *this))
    resolutionKind = ArchetypeResolutionKind::AlreadyKnown;

  return maybeResolveEquivalenceClass(type, resolutionKind,
                                      /*wantExactPotentialArchetype=*/true);
}

bool GenericSignatureBuilder::areInSameEquivalenceClass(Type type1,
                                                        Type type2) {
  return resolveEquivalenceClass(type1, ArchetypeResolutionKind::WellFormed)
    == resolveEquivalenceClass(type2, ArchetypeResolutionKind::WellFormed);
}

TypeArrayView<GenericTypeParamType>
GenericSignatureBuilder::getGenericParams() const {
  return TypeArrayView<GenericTypeParamType>(Impl->GenericParams);
}

void GenericSignatureBuilder::addGenericParameter(GenericTypeParamDecl *GenericParam) {
  addGenericParameter(
     GenericParam->getDeclaredInterfaceType()->castTo<GenericTypeParamType>());
}

bool GenericSignatureBuilder::addGenericParameterRequirements(
                                           GenericTypeParamDecl *GenericParam) {
  GenericParamKey Key(GenericParam);
  auto PA = Impl->PotentialArchetypes[Key.findIndexIn(getGenericParams())];
  
  // Add the requirements from the declaration.
  return isErrorResult(
           addInheritedRequirements(GenericParam, PA, nullptr,
                                    GenericParam->getModuleContext()));
}

void GenericSignatureBuilder::addGenericParameter(GenericTypeParamType *GenericParam) {
  GenericParamKey Key(GenericParam);
  auto params = getGenericParams();
  (void)params;
  assert(params.empty() ||
         ((Key.Depth == params.back()->getDepth() &&
           Key.Index == params.back()->getIndex() + 1) ||
          (Key.Depth > params.back()->getDepth() &&
           Key.Index == 0)));

  // Create a potential archetype for this type parameter.
  void *mem = Impl->Allocator.Allocate<PotentialArchetype>();
  auto PA = new (mem) PotentialArchetype(getASTContext(), GenericParam);
  Impl->GenericParams.push_back(GenericParam);
  Impl->PotentialArchetypes.push_back(PA);
}

/// Visit all of the types that show up in the list of inherited
/// types.
static ConstraintResult visitInherited(
         ArrayRef<TypeLoc> inheritedTypes,
         llvm::function_ref<ConstraintResult(Type, const TypeRepr *)> visitType) {
  // Local function that (recursively) adds inherited types.
  ConstraintResult result = ConstraintResult::Resolved;
  std::function<void(Type, const TypeRepr *)> visitInherited;

  visitInherited = [&](Type inheritedType, const TypeRepr *typeRepr) {
    // Decompose explicitly-written protocol compositions.
    if (auto composition = dyn_cast_or_null<CompositionTypeRepr>(typeRepr)) {
      if (auto compositionType
            = inheritedType->getAs<ProtocolCompositionType>()) {
        unsigned index = 0;
        for (auto memberType : compositionType->getMembers()) {
          visitInherited(memberType, composition->getTypes()[index]);
          index++;
        }

        return;
      }
    }

    auto recursiveResult = visitType(inheritedType, typeRepr);
    if (isErrorResult(recursiveResult) && !isErrorResult(result))
      result = recursiveResult;
  };

  // Visit all of the inherited types.
  for (auto inherited : inheritedTypes) {
    visitInherited(inherited.getType(), inherited.getTypeRepr());
  }

  return result;
}

ConstraintResult GenericSignatureBuilder::expandConformanceRequirement(
                                            ResolvedType selfType,
                                            ProtocolDecl *proto,
                                            const RequirementSource *source,
                                            bool onlySameTypeConstraints) {
  auto protocolSubMap = SubstitutionMap::getProtocolSubstitutions(
      proto, selfType.getDependentType(*this), ProtocolConformanceRef(proto));

  // Use the requirement signature to avoid rewalking the entire protocol.  This
  // cannot compute the requirement signature directly, because that may be
  // infinitely recursive: this code is also used to construct it.
  if (proto->isRequirementSignatureComputed()) {
    auto innerSource =
      FloatingRequirementSource::viaProtocolRequirement(source, proto,
                                                        /*inferred=*/false);
    for (const auto &req : proto->getRequirementSignature()) {
      // If we're only looking at same-type constraints, skip everything else.
      if (onlySameTypeConstraints && req.getKind() != RequirementKind::SameType)
        continue;

      auto substReq = req.subst(protocolSubMap);
      auto reqResult = substReq
                           ? addRequirement(*substReq, innerSource, nullptr)
                           : ConstraintResult::Conflicting;
      if (isErrorResult(reqResult)) return reqResult;
    }

    return ConstraintResult::Resolved;
  }

  if (!onlySameTypeConstraints) {
    // Add all of the inherited protocol requirements, recursively.
    if (auto resolver = getLazyResolver())
      resolver->resolveInheritedProtocols(proto);

    auto inheritedReqResult =
      addInheritedRequirements(proto, selfType.getUnresolvedType(), source,
                               proto->getModuleContext());
    if (isErrorResult(inheritedReqResult))
      return inheritedReqResult;
  }

  // Add any requirements in the where clause on the protocol.
  if (auto WhereClause = proto->getTrailingWhereClause()) {
    for (auto &req : WhereClause->getRequirements()) {
      // If we're only looking at same-type constraints, skip everything else.
      if (onlySameTypeConstraints &&
          req.getKind() != RequirementReprKind::SameType)
        continue;

      auto innerSource = FloatingRequirementSource::viaProtocolRequirement(
          source, proto, &req, /*inferred=*/false);
      addRequirement(&req, innerSource, &protocolSubMap,
                     proto->getModuleContext());
    }
  }

  // Remaining logic is not relevant in ObjC protocol cases.
  if (proto->isObjC())
    return ConstraintResult::Resolved;

  // Collect all of the inherited associated types and typealiases in the
  // inherited protocols (recursively).
  llvm::MapVector<DeclName, TinyPtrVector<TypeDecl *>> inheritedTypeDecls;
  {
    proto->walkInheritedProtocols(
        [&](ProtocolDecl *inheritedProto) -> TypeWalker::Action {
      if (inheritedProto == proto) return TypeWalker::Action::Continue;

      for (auto req : inheritedProto->getMembers()) {
        if (auto typeReq = dyn_cast<TypeDecl>(req))
          inheritedTypeDecls[typeReq->getFullName()].push_back(typeReq);
      }
      return TypeWalker::Action::Continue;
    });
  }

  // Local function to find the insertion point for the protocol's "where"
  // clause, as well as the string to start the insertion ("where" or ",");
  auto getProtocolWhereLoc = [&]() -> std::pair<SourceLoc, const char *> {
    // Already has a trailing where clause.
    if (auto trailing = proto->getTrailingWhereClause())
      return { trailing->getRequirements().back().getSourceRange().End, ", " };

    // Inheritance clause.
    return { proto->getInherited().back().getSourceRange().End, " where " };
  };

  // Retrieve the set of requirements that a given associated type declaration
  // produces, in the form that would be seen in the where clause.
  auto getAssociatedTypeReqs = [&](AssociatedTypeDecl *assocType,
                                   const char *start) {
    std::string result;
    {
      llvm::raw_string_ostream out(result);
      out << start;
      interleave(assocType->getInherited(), [&](TypeLoc inheritedType) {
        out << assocType->getFullName() << ": ";
        if (auto inheritedTypeRepr = inheritedType.getTypeRepr())
          inheritedTypeRepr->print(out);
        else
          inheritedType.getType().print(out);
      }, [&] {
        out << ", ";
      });
    }
    return result;
  };

  // Retrieve the requirement that a given typealias introduces when it
  // overrides an inherited associated type with the same name, as a string
  // suitable for use in a where clause.
  auto getConcreteTypeReq = [&](TypeDecl *type, const char *start) {
    std::string result;
    {
      llvm::raw_string_ostream out(result);
      out << start;
      out << type->getFullName() << " == ";
      if (auto typealias = dyn_cast<TypeAliasDecl>(type)) {
        if (auto underlyingTypeRepr =
              typealias->getUnderlyingTypeLoc().getTypeRepr())
          underlyingTypeRepr->print(out);
        else
          typealias->getUnderlyingTypeLoc().getType().print(out);
      } else {
        type->print(out);
      }
    }
    return result;
  };

  // Form an unsubstituted type referring to the given type declaration,
  // for use in an inferred same-type requirement.
  auto formUnsubstitutedType = [&](TypeDecl *typeDecl) -> Type {
    if (auto assocType = dyn_cast<AssociatedTypeDecl>(typeDecl)) {
      return DependentMemberType::get(
                               assocType->getProtocol()->getSelfInterfaceType(),
                               assocType);
    }

    // Resolve the underlying type, if we haven't done so yet.
    if (!typeDecl->hasInterfaceType()) {
      getLazyResolver()->resolveDeclSignature(typeDecl);
    }

    if (auto typealias = dyn_cast<TypeAliasDecl>(typeDecl)) {
      return typealias->getUnderlyingTypeLoc().getType();
    }

    return typeDecl->getDeclaredInterfaceType();
  };

  // An inferred same-type requirement between the two type declarations
  // within this protocol or a protocol it inherits.
  auto addInferredSameTypeReq = [&](TypeDecl *first, TypeDecl *second) {
    Type firstType = formUnsubstitutedType(first);
    if (!firstType) return;

    Type secondType = formUnsubstitutedType(second);
    if (!secondType) return;

    auto inferredSameTypeSource =
      FloatingRequirementSource::viaProtocolRequirement(
                    source, proto, WrittenRequirementLoc(), /*inferred=*/true);

    auto rawReq = Requirement(RequirementKind::SameType, firstType, secondType);
    if (auto req = rawReq.subst(protocolSubMap))
      addRequirement(*req, inferredSameTypeSource, proto->getParentModule());
  };

  // Add requirements for each of the associated types.
  for (auto Member : proto->getMembers()) {
    if (auto assocTypeDecl = dyn_cast<AssociatedTypeDecl>(Member)) {
      // Add requirements placed directly on this associated type.
      Type assocType =
        DependentMemberType::get(selfType.getDependentType(*this), assocTypeDecl);
      if (!onlySameTypeConstraints) {
        auto assocResult =
          addInheritedRequirements(assocTypeDecl, assocType, source,
                                   /*inferForModule=*/nullptr);
        if (isErrorResult(assocResult))
          return assocResult;
      }

      // Add requirements from this associated type's where clause.
      if (auto WhereClause = assocTypeDecl->getTrailingWhereClause()) {
        for (auto &req : WhereClause->getRequirements()) {
          // If we're only looking at same-type constraints, skip everything
          // else.
          if (onlySameTypeConstraints &&
              req.getKind() != RequirementReprKind::SameType)
            continue;

          auto innerSource =
            FloatingRequirementSource::viaProtocolRequirement(
                                      source, proto, &req, /*inferred=*/false);
          addRequirement(&req, innerSource, &protocolSubMap,
                         /*inferForModule=*/nullptr);
        }
      }

      // Check whether we inherited any types with the same name.
      auto knownInherited =
        inheritedTypeDecls.find(assocTypeDecl->getFullName());
      if (knownInherited == inheritedTypeDecls.end()) continue;

      bool shouldWarnAboutRedeclaration =
        source->kind == RequirementSource::RequirementSignatureSelf &&
        assocTypeDecl->getDefaultDefinitionLoc().isNull() &&
        (!assocTypeDecl->getInherited().empty() ||
         assocTypeDecl->getTrailingWhereClause());
      for (auto inheritedType : knownInherited->second) {
        // If we have inherited associated type...
        if (auto inheritedAssocTypeDecl =
              dyn_cast<AssociatedTypeDecl>(inheritedType)) {
          // Complain about the first redeclaration.
          if (shouldWarnAboutRedeclaration) {
            auto inheritedFromProto = inheritedAssocTypeDecl->getProtocol();
            auto fixItWhere = getProtocolWhereLoc();
            Diags.diagnose(assocTypeDecl,
                           diag::inherited_associated_type_redecl,
                           assocTypeDecl->getFullName(),
                           inheritedFromProto->getDeclaredInterfaceType())
              .fixItInsertAfter(
                       fixItWhere.first,
                       getAssociatedTypeReqs(assocTypeDecl, fixItWhere.second))
              .fixItRemove(assocTypeDecl->getSourceRange());

            Diags.diagnose(inheritedAssocTypeDecl, diag::decl_declared_here,
                           inheritedAssocTypeDecl->getFullName());

            shouldWarnAboutRedeclaration = false;
          }

          continue;
        }

        // We inherited a type; this associated type will be identical
        // to that typealias.
        if (source->kind == RequirementSource::RequirementSignatureSelf) {
          auto inheritedOwningDecl =
            inheritedType->getDeclContext()
              ->getAsNominalTypeOrNominalTypeExtensionContext();
          Diags.diagnose(assocTypeDecl,
                         diag::associated_type_override_typealias,
                         assocTypeDecl->getFullName(),
                         inheritedOwningDecl->getDescriptiveKind(),
                         inheritedOwningDecl->getDeclaredInterfaceType());
        }

        addInferredSameTypeReq(assocTypeDecl, inheritedType);
      }

      inheritedTypeDecls.erase(knownInherited);
      continue;
    }
  }

  // Check all remaining inherited type declarations to determine if
  // this protocol has a non-associated-type type with the same name.
  inheritedTypeDecls.remove_if(
    [&](const std::pair<DeclName, TinyPtrVector<TypeDecl *>> &inherited) {
      auto name = inherited.first;
      for (auto found : proto->lookupDirect(name)) {
        // We only want concrete type declarations.
        auto type = dyn_cast<TypeDecl>(found);
        if (!type || isa<AssociatedTypeDecl>(type)) continue;

        // ... from the same module as the protocol.
        if (type->getModuleContext() != proto->getModuleContext()) continue;

        // Ignore types defined in constrained extensions; their equivalence
        // to the associated type would have to be conditional, which we cannot
        // model.
        if (auto ext = dyn_cast<ExtensionDecl>(type->getDeclContext())) {
          if (ext->isConstrainedExtension()) continue;
        }

        // We found something.
        bool shouldWarnAboutRedeclaration =
          source->kind == RequirementSource::RequirementSignatureSelf;

        for (auto inheritedType : inherited.second) {
          // If we have inherited associated type...
          if (auto inheritedAssocTypeDecl =
                dyn_cast<AssociatedTypeDecl>(inheritedType)) {
            // Infer a same-type requirement between the typealias' underlying
            // type and the inherited associated type.
            addInferredSameTypeReq(inheritedAssocTypeDecl, type);

            // Warn that one should use where clauses for this.
            if (shouldWarnAboutRedeclaration) {
              auto inheritedFromProto = inheritedAssocTypeDecl->getProtocol();
              auto fixItWhere = getProtocolWhereLoc();
              Diags.diagnose(type,
                             diag::typealias_override_associated_type,
                             name,
                             inheritedFromProto->getDeclaredInterfaceType())
                .fixItInsertAfter(fixItWhere.first,
                                  getConcreteTypeReq(type, fixItWhere.second))
                .fixItRemove(type->getSourceRange());
              Diags.diagnose(inheritedAssocTypeDecl, diag::decl_declared_here,
                             inheritedAssocTypeDecl->getFullName());

              shouldWarnAboutRedeclaration = false;
            }

            continue;
          }

          // Two typealiases that should be the same.
          addInferredSameTypeReq(inheritedType, type);
        }

        // We can remove this entry.
        return true;
      }

      return false;
  });

  // Infer same-type requirements among inherited type declarations.
  for (auto &entry : inheritedTypeDecls) {
    if (entry.second.size() < 2) continue;

    auto firstDecl = entry.second.front();
    for (auto otherDecl : ArrayRef<TypeDecl *>(entry.second).slice(1)) {
      addInferredSameTypeReq(firstDecl, otherDecl);
    }
  }

  return ConstraintResult::Resolved;
}

ConstraintResult GenericSignatureBuilder::addConformanceRequirement(
                               ResolvedType type,
                               ProtocolDecl *proto,
                               FloatingRequirementSource source) {
  // Add the conformance requirement, bailing out earlier if we've already
  // seen it.
  auto equivClass = type.getEquivalenceClass(*this);
  if (!equivClass->recordConformanceConstraint(*this, type, proto, source))
    return ConstraintResult::Resolved;

  auto resolvedSource = source.getSource(*this,
                                         type.getDependentType(*this));
  return expandConformanceRequirement(type, proto, resolvedSource,
                                      /*onlySameTypeRequirements=*/false);
}

ConstraintResult GenericSignatureBuilder::addLayoutRequirementDirect(
                                             ResolvedType type,
                                             LayoutConstraint layout,
                                             FloatingRequirementSource source) {
  auto equivClass = type.getEquivalenceClass(*this);

  // Update the layout in the equivalence class, if we didn't have one already.
  bool anyChanges = false;
  if (!equivClass->layout) {
    equivClass->layout = layout;
    anyChanges = true;
  } else {
    // Try to merge layout constraints.
    auto mergedLayout = equivClass->layout.merge(layout);
    if (mergedLayout->isKnownLayout() && mergedLayout != equivClass->layout) {
      equivClass->layout = mergedLayout;
      anyChanges = true;
    }
  }

  // Record this layout constraint.
  equivClass->layoutConstraints.push_back({type.getUnresolvedType(),
    layout, source.getSource(*this, type.getDependentType(*this))});
  equivClass->modified(*this);
  ++NumLayoutConstraints;
  if (!anyChanges) ++NumLayoutConstraintsExtra;

  return ConstraintResult::Resolved;
}

ConstraintResult GenericSignatureBuilder::addLayoutRequirement(
                                             UnresolvedType subject,
                                             LayoutConstraint layout,
                                             FloatingRequirementSource source,
                                             UnresolvedHandlingKind unresolvedHandling) {
  // Resolve the subject.
  auto resolvedSubject = resolve(subject, source);
  if (!resolvedSubject) {
    return handleUnresolvedRequirement(
                               RequirementKind::Layout, subject,
                               layout, source,
                               resolvedSubject.getUnresolvedEquivClass(),
                               unresolvedHandling);
  }

  // If this layout constraint applies to a concrete type, we can fully
  // resolve it now.
  if (auto concreteType = resolvedSubject.getAsConcreteType()) {
    // If a layout requirement was explicitly written on a concrete type,
    // complain.
    if (source.isExplicit() && source.getLoc().isValid()) {
      Impl->HadAnyError = true;

      Diags.diagnose(source.getLoc(), diag::requires_not_suitable_archetype,
                     TypeLoc::withoutLoc(concreteType));
      return ConstraintResult::Concrete;
    }

    // FIXME: Check whether the layout constraint makes sense for this
    // concrete type!

    return ConstraintResult::Resolved;
  }

  return addLayoutRequirementDirect(resolvedSubject, layout, source);
}

bool GenericSignatureBuilder::updateSuperclass(
                                           ResolvedType type,
                                           Type superclass,
                                           FloatingRequirementSource source) {
  auto equivClass = type.getEquivalenceClass(*this);

  // Local function to handle the update of superclass conformances
  // when the superclass constraint changes.
  auto updateSuperclassConformances = [&] {
    for (const auto &conforms : equivClass->conformsTo) {
      (void)resolveSuperConformance(type, conforms.first);
    }
  };

  // If we haven't yet recorded a superclass constraint for this equivalence
  // class, do so now.
  if (!equivClass->superclass) {
    equivClass->superclass = superclass;
    updateSuperclassConformances();

    // Presence of a superclass constraint implies a _Class layout
    // constraint.
    auto layoutReqSource =
      source.getSource(*this,
                       type.getDependentType(*this))->viaDerived(*this);
    addLayoutRequirementDirect(type,
                         LayoutConstraint::getLayoutConstraint(
                             superclass->getClassOrBoundGenericClass()->isObjC()
                                 ? LayoutConstraintKind::Class
                                 : LayoutConstraintKind::NativeClass,
                             getASTContext()),
                         layoutReqSource);
    return true;
  }

  // T already has a superclass; make sure it's related.
  auto existingSuperclass = equivClass->superclass;
  // TODO: In principle, this could be isBindableToSuperclassOf instead of
  // isExactSubclassOf. If you had:
  //
  //   class Foo<T>
  //   class Bar: Foo<Int>
  //
  //   func foo<T, U where U: Foo<T>, U: Bar>(...) { ... }
  //
  // then the second constraint should be allowed, constraining U to Bar
  // and secondarily imposing a T == Int constraint.
  if (existingSuperclass->isExactSuperclassOf(superclass)) {
    equivClass->superclass = superclass;

    // We've strengthened the bound, so update superclass conformances.
    updateSuperclassConformances();
    return true;
  }

  return false;
}

ConstraintResult GenericSignatureBuilder::addSuperclassRequirementDirect(
                                            ResolvedType type,
                                            Type superclass,
                                            FloatingRequirementSource source) {
  auto resolvedSource =
    source.getSource(*this, type.getDependentType(*this));

  // Record the constraint.
  auto equivClass = type.getEquivalenceClass(*this);
  equivClass->superclassConstraints.push_back(
    ConcreteConstraint{type.getUnresolvedType(), superclass, resolvedSource});
  equivClass->modified(*this);
  ++NumSuperclassConstraints;

  // Update the equivalence class with the constraint.
  if (!updateSuperclass(type, superclass, source))
    ++NumSuperclassConstraintsExtra;

  return ConstraintResult::Resolved;
}

/// Map an unresolved type to a requirement right-hand-side.
static GenericSignatureBuilder::UnresolvedRequirementRHS
toUnresolvedRequirementRHS(GenericSignatureBuilder::UnresolvedType unresolved) {
  if (auto pa = unresolved.dyn_cast<PotentialArchetype *>())
    return pa;

  return unresolved.dyn_cast<Type>();
}

ConstraintResult GenericSignatureBuilder::addTypeRequirement(
    UnresolvedType subject, UnresolvedType constraint,
    FloatingRequirementSource source, UnresolvedHandlingKind unresolvedHandling,
    ModuleDecl *inferForModule) {
  // Resolve the constraint.
  auto resolvedConstraint = resolve(constraint, source);
  if (!resolvedConstraint) {
    return handleUnresolvedRequirement(
                             RequirementKind::Conformance, subject,
                             toUnresolvedRequirementRHS(constraint), source,
                             resolvedConstraint.getUnresolvedEquivClass(),
                             unresolvedHandling);
  }

  // The right-hand side needs to be concrete.
  Type constraintType = resolvedConstraint.getAsConcreteType();
  if (!constraintType) {
    constraintType = resolvedConstraint.getDependentType(*this);
    assert(constraintType && "No type to express resolved constraint?");
  }

  // Check whether we have a reasonable constraint type at all.
  if (!constraintType->isExistentialType() &&
      !constraintType->getClassOrBoundGenericClass()) {
    if (source.getLoc().isValid() && !constraintType->hasError()) {
      auto subjectType = subject.dyn_cast<Type>();
      if (!subjectType)
        subjectType = subject.get<PotentialArchetype *>()
                        ->getDependentType(getGenericParams());

      Impl->HadAnyError = true;
      Diags.diagnose(source.getLoc(), diag::requires_conformance_nonprotocol,
                     TypeLoc::withoutLoc(subjectType),
                     TypeLoc::withoutLoc(constraintType));
    }

    return ConstraintResult::Conflicting;
  }

  // Resolve the subject. If we can't, delay the constraint.
  auto resolvedSubject = resolve(subject, source);
  if (!resolvedSubject) {
    auto recordedKind =
      constraintType->isExistentialType()
        ? RequirementKind::Conformance
        : RequirementKind::Superclass;
    return handleUnresolvedRequirement(
                               recordedKind, subject, constraintType,
                               source,
                               resolvedSubject.getUnresolvedEquivClass(),
                               unresolvedHandling);
  }

  // If the resolved subject is a type, there may be things we can infer (if it
  // conditionally conforms to the protocol), and we can probably perform
  // diagnostics here.
  if (auto subjectType = resolvedSubject.getAsConcreteType()) {
    if (constraintType->isExistentialType()) {
      auto layout = constraintType->getExistentialLayout();
      for (auto *proto : layout.getProtocols()) {
        // We have a pure concrete type, and there's no guarantee of dependent
        // type that makes sense to use here, but, in practice, all
        // getLookupConformanceFns used in here don't use that parameter anyway.
        auto dependentType = CanType();
        auto conformance =
            getLookupConformanceFn()(dependentType, subjectType, proto);

        // FIXME: diagnose if there's no conformance.
        if (conformance) {
          if (addConditionalRequirements(*conformance, inferForModule,
                                         source.getLoc()))
            return ConstraintResult::Conflicting;
        }
      }
    }

    // One cannot explicitly write a constraint on a concrete type.
    if (source.isExplicit()) {
      if (source.getLoc().isValid()) {
        Impl->HadAnyError = true;
        Diags.diagnose(source.getLoc(), diag::requires_not_suitable_archetype,
                       TypeLoc::withoutLoc(subjectType));
      }

      return ConstraintResult::Concrete;
    }

    return ConstraintResult::Resolved;
  }

  // Protocol requirements.
  if (constraintType->isExistentialType()) {
    bool anyErrors = false;
    auto layout = constraintType->getExistentialLayout();

    if (auto layoutConstraint = layout.getLayoutConstraint()) {
      if (isErrorResult(addLayoutRequirementDirect(resolvedSubject,
                                                   layoutConstraint,
                                                   source)))
        anyErrors = true;
    }

    if (layout.superclass) {
      if (isErrorResult(addSuperclassRequirementDirect(resolvedSubject,
                                                       layout.superclass,
                                                       source)))
        anyErrors = true;
    }

    for (auto *proto : layout.getProtocols()) {
      auto *protoDecl = proto->getDecl();
      if (isErrorResult(addConformanceRequirement(resolvedSubject, protoDecl,
                                                  source)))
        anyErrors = true;
    }

    return anyErrors ? ConstraintResult::Conflicting
                     : ConstraintResult::Resolved;
  }

  // Superclass constraint.
  return addSuperclassRequirementDirect(resolvedSubject, constraintType,
                                        source);
}

void GenericSignatureBuilder::addedNestedType(PotentialArchetype *nestedPA) {
  // If there was already another type with this name within the parent
  // potential archetype, equate this type with that one.
  auto parentPA = nestedPA->getParent();
  auto &allNested = parentPA->NestedTypes[nestedPA->getNestedName()];
  assert(!allNested.empty());
  assert(allNested.back() == nestedPA);
  if (allNested.size() > 1) {
    auto firstPA = allNested.front();
    auto inferredSource =
      FloatingRequirementSource::forInferred(nullptr);

    addSameTypeRequirement(firstPA, nestedPA, inferredSource,
                           UnresolvedHandlingKind::GenerateConstraints);
    return;
  }

  // If our parent type is not the representative, equate this nested
  // potential archetype to the equivalent nested type within the
  // representative.
  auto parentRepPA = parentPA->getRepresentative();
  if (parentPA == parentRepPA) return;

  PotentialArchetype *existingPA =
    parentRepPA->updateNestedTypeForConformance(
                                        *this,
                                        nestedPA->getResolvedType(),
                                        ArchetypeResolutionKind::WellFormed);

  auto sameNamedSource =
    FloatingRequirementSource::forNestedTypeNameMatch(
                                                nestedPA->getNestedName());
  addSameTypeRequirement(existingPA, nestedPA, sameNamedSource,
                         UnresolvedHandlingKind::GenerateConstraints);
}

ConstraintResult
GenericSignatureBuilder::addSameTypeRequirementBetweenTypeParameters(
                                         ResolvedType type1, ResolvedType type2,
                                         const RequirementSource *source)
{
  ++NumSameTypeConstraints;

  Type depType1 = type1.getDependentType(*this);
  Type depType2 = type2.getDependentType(*this);

  // Record the same-type constraint, and bail out if it was already known.
  auto equivClass = type1.getEquivalenceClassIfPresent();
  auto equivClass2 = type2.getEquivalenceClassIfPresent();
  if (depType1->isEqual(depType2) ||
      ((equivClass || equivClass2) && equivClass == equivClass2)) {
    // Make sure we have an equivalence class in which we can record the
    // same-type constraint.
    if (!equivClass) {
      if (equivClass2) {
        equivClass = equivClass2;
      } else {
        auto pa1 = type1.realizePotentialArchetype(*this);
        equivClass = pa1->getOrCreateEquivalenceClass(*this);
      }
    }

    // FIXME: We could determine equivalence based on both sides canonicalizing
    // to the same type. 
    equivClass->sameTypeConstraints.push_back({depType1, depType2, source});
    return ConstraintResult::Resolved;
  }

  // Both sides are type parameters; equate them.
  // FIXME: Realizes potential archetypes far too early.
  auto OrigT1 = type1.realizePotentialArchetype(*this);
  auto OrigT2 = type2.realizePotentialArchetype(*this);

  // Operate on the representatives
  auto T1 = OrigT1->getRepresentative();
  auto T2 = OrigT2->getRepresentative();

  // Decide which potential archetype is to be considered the representative.
  // We prefer potential archetypes with lower nesting depths, because it
  // prevents us from unnecessarily building deeply nested potential archetypes.
  unsigned nestingDepth1 = T1->getNestingDepth();
  unsigned nestingDepth2 = T2->getNestingDepth();
  if (nestingDepth2 < nestingDepth1) {
    std::swap(T1, T2);
    std::swap(OrigT1, OrigT2);
    std::swap(equivClass, equivClass2);
  }

  // T1 must have an equivalence class; create one if we don't already have
  // one.
  if (!equivClass)
    equivClass = T1->getOrCreateEquivalenceClass(*this);

  // Record this same-type constraint.
  equivClass->sameTypeConstraints.push_back({depType1, depType2, source});

  // Determine the anchor types of the two equivalence classes.
  CanType anchor1 = equivClass->getAnchor(*this, { })->getCanonicalType();
  CanType anchor2 =
    (equivClass2 ? equivClass2->getAnchor(*this, { })
                 : getCanonicalTypeParameter(T2->getDependentType({ })))
      ->getCanonicalType();

  // Merge the equivalence classes.
  equivClass->modified(*this);
  auto equivClass1Members = equivClass->members;
  auto equivClass2Members = T2->getEquivalenceClassMembers();
  for (auto equiv : equivClass2Members)
    equivClass->addMember(equiv);

  // Grab the old equivalence class, if present. We'll deallocate it at the end.
  SWIFT_DEFER {
    if (equivClass2)
      Impl->deallocateEquivalenceClass(equivClass2);
  };

  // Consider the second equivalence class to be modified.
  if (equivClass2)
    equivClass->modified(*this);

  // Same-type requirements.
  if (equivClass2) {
    equivClass->sameTypeConstraints.insert(
                                   equivClass->sameTypeConstraints.end(),
                                   equivClass2->sameTypeConstraints.begin(),
                                   equivClass2->sameTypeConstraints.end());
  }

  // Combine the rewrite rules.
  if (auto rewriteRoot2 = Impl->getOrCreateRewriteTreeRoot(anchor2)) {
    if (auto rewriteRoot1 = Impl->getOrCreateRewriteTreeRoot(anchor1)) {
      // Merge the second rewrite tree into the first.
      if (rewriteRoot2->mergeInto(rewriteRoot1))
        ++Impl->RewriteGeneration;
      Impl->RewriteTreeRoots.erase(anchor2);
    } else {
      // Take the second rewrite tree and make it the first.
      auto root2Entry = Impl->RewriteTreeRoots.find(anchor2);
      auto root2Ptr = std::move(root2Entry->second);
      Impl->RewriteTreeRoots.erase(root2Entry);
      (void)Impl->RewriteTreeRoots.insert({anchor1, std::move(root2Ptr)});
    }
  }

  // Add a rewrite rule to map the anchor of T2 down to the anchor of T1.
  if (addSameTypeRewriteRule(anchor2, anchor1))
    ++Impl->RewriteGeneration;

  // Same-type-to-concrete requirements.
  bool t1IsConcrete = !equivClass->concreteType.isNull();
  bool t2IsConcrete = equivClass2 && !equivClass2->concreteType.isNull();
  if (t2IsConcrete) {
    if (t1IsConcrete) {
      (void)addSameTypeRequirement(equivClass->concreteType,
                                   equivClass2->concreteType, source,
                                   UnresolvedHandlingKind::GenerateConstraints,
                                   SameTypeConflictCheckedLater());
    } else {
      equivClass->concreteType = equivClass2->concreteType;
      equivClass->invalidConcreteType = equivClass2->invalidConcreteType;
    }

    equivClass->concreteTypeConstraints.insert(
                                 equivClass->concreteTypeConstraints.end(),
                                 equivClass2->concreteTypeConstraints.begin(),
                                 equivClass2->concreteTypeConstraints.end());
  }

  // Make T1 the representative of T2, merging the equivalence classes.
  T2->representativeOrEquivClass = T1;

  // Layout requirements.
  if (equivClass2 && equivClass2->layout) {
    if (!equivClass->layout) {
      equivClass->layout = equivClass2->layout;
      equivClass->layoutConstraints = std::move(equivClass2->layoutConstraints);
    } else {
      const RequirementSource *source2
        = equivClass2->layoutConstraints.front().source;
      addLayoutRequirementDirect(T1, equivClass2->layout, source2);
      equivClass->layoutConstraints.insert(
                                   equivClass->layoutConstraints.end(),
                                   equivClass2->layoutConstraints.begin() + 1,
                                   equivClass2->layoutConstraints.end());
    }
  }

  // Superclass requirements.
  if (equivClass2 && equivClass2->superclass) {
    const RequirementSource *source2;
    if (auto existingSource2 =
          equivClass2->findAnySuperclassConstraintAsWritten(
            OrigT2->getDependentType(getGenericParams())))
      source2 = existingSource2->source;
    else
      source2 = equivClass2->superclassConstraints.front().source;

    // Add the superclass constraints from the second equivalence class.
    equivClass->superclassConstraints.insert(
                                   equivClass->superclassConstraints.end(),
                                   equivClass2->superclassConstraints.begin(),
                                   equivClass2->superclassConstraints.end());

    (void)updateSuperclass(T1, equivClass2->superclass, source2);
  }

  // Add all of the protocol conformance requirements of T2 to T1.
  if (equivClass2) {
    for (const auto &entry : equivClass2->conformsTo) {
      equivClass->recordConformanceConstraint(*this, T1, entry.first,
                                              entry.second.front().source);

      auto &constraints1 = equivClass->conformsTo[entry.first];
      // FIXME: Go through recordConformanceConstraint()?
      constraints1.insert(constraints1.end(),
                          entry.second.begin() + 1,
                          entry.second.end());
    }
  }

  // Recursively merge the associated types of T2 into T1.
  auto dependentT1 = T1->getDependentType(getGenericParams());
  SmallPtrSet<Identifier, 4> visited;
  for (auto equivT2 : equivClass2Members) {
    for (auto T2Nested : equivT2->NestedTypes) {
      // Only visit each name once.
      if (!visited.insert(T2Nested.first).second) continue;

      // If T1 is concrete but T2 is not, concretize the nested types of T2.
      if (t1IsConcrete && !t2IsConcrete) {
        concretizeNestedTypeFromConcreteParent(T1, T2Nested.second.front(),
                                               *this);
        continue;
      }

      // Otherwise, make the nested types equivalent.
      AssociatedTypeDecl *assocTypeT2 = nullptr;
      for (auto T2 : T2Nested.second) {
        assocTypeT2 = T2->getResolvedType();
        if (assocTypeT2) break;
      }

      if (!assocTypeT2) continue;

      Type nestedT1 = DependentMemberType::get(dependentT1, assocTypeT2);
      if (isErrorResult(
            addSameTypeRequirement(
               nestedT1, T2Nested.second.front(),
               FloatingRequirementSource::forNestedTypeNameMatch(
                                                      assocTypeT2->getName()),
               UnresolvedHandlingKind::GenerateConstraints)))
        return ConstraintResult::Conflicting;
    }
  }

  // If T2 is concrete but T1 was not, concretize the nested types of T1.
  visited.clear();
  if (t2IsConcrete && !t1IsConcrete) {
    for (auto equivT1 : equivClass1Members) {
      for (auto T1Nested : equivT1->NestedTypes) {
        // Only visit each name once.
        if (!visited.insert(T1Nested.first).second) continue;

        concretizeNestedTypeFromConcreteParent(T2, T1Nested.second.front(),
                                               *this);
      }
    }
  }

  return ConstraintResult::Resolved;
}

ConstraintResult GenericSignatureBuilder::addSameTypeRequirementToConcrete(
                                           ResolvedType type,
                                           Type concrete,
                                           const RequirementSource *source) {
auto equivClass = type.getEquivalenceClass(*this);

  // Record the concrete type and its source.
  equivClass->concreteTypeConstraints.push_back(
    ConcreteConstraint{type.getUnresolvedType(), concrete, source});
  equivClass->modified(*this);
  ++NumConcreteTypeConstraints;

  // If we've already been bound to a type, match that type.
  if (equivClass->concreteType) {
    return addSameTypeRequirement(equivClass->concreteType, concrete, source,
                                  UnresolvedHandlingKind::GenerateConstraints,
                                  SameTypeConflictCheckedLater());

  }

  // Record the requirement.
  equivClass->concreteType = concrete;

  // Make sure the concrete type fulfills the conformance requirements of
  // this equivalence class.
  for (const auto &conforms : equivClass->conformsTo) {
    if (!resolveConcreteConformance(type, conforms.first))
      return ConstraintResult::Conflicting;
  }

  // Eagerly resolve any existing nested types to their concrete forms (others
  // will be "concretized" as they are constructed, in getNestedType).
  for (auto equivT : equivClass->members) {
    for (auto nested : equivT->getNestedTypes()) {
      concretizeNestedTypeFromConcreteParent(equivT, nested.second.front(),
                                             *this);
    }
  }

  return ConstraintResult::Resolved;
}

ConstraintResult GenericSignatureBuilder::addSameTypeRequirementBetweenConcrete(
    Type type1, Type type2, FloatingRequirementSource source,
    llvm::function_ref<void(Type, Type)> diagnoseMismatch) {
  // Local class to handle matching the two sides of the same-type constraint.
  class ReqTypeMatcher : public TypeMatcher<ReqTypeMatcher> {
    GenericSignatureBuilder &builder;
    FloatingRequirementSource source;
    Type outerType1, outerType2;
    llvm::function_ref<void(Type, Type)> diagnoseMismatch;

  public:
    ReqTypeMatcher(GenericSignatureBuilder &builder,
                   FloatingRequirementSource source,
                   Type outerType1, Type outerType2,
                   llvm::function_ref<void(Type, Type)> diagnoseMismatch)
        : builder(builder), source(source), outerType1(outerType1),
          outerType2(outerType2), diagnoseMismatch(diagnoseMismatch) {}

    bool mismatch(TypeBase *firstType, TypeBase *secondType,
                  Type sugaredFirstType) {
      // If the mismatch was in the first layer (i.e. what was fed to
      // addSameTypeRequirementBetweenConcrete), then this is a fundamental
      // mismatch, and we need to diagnose it. This is what breaks the mutual
      // recursion between addSameTypeRequirement and
      // addSameTypeRequirementBetweenConcrete.
      if (outerType1->isEqual(firstType) && outerType2->isEqual(secondType)) {
        diagnoseMismatch(sugaredFirstType, secondType);
        return false;
      }

      auto failed = builder.addSameTypeRequirement(
          sugaredFirstType, Type(secondType), source,
          UnresolvedHandlingKind::GenerateConstraints, diagnoseMismatch);
      return !isErrorResult(failed);
    }
  } matcher(*this, source, type1, type2, diagnoseMismatch);

  return matcher.match(type1, type2) ? ConstraintResult::Resolved
                                     : ConstraintResult::Conflicting;
}

ConstraintResult GenericSignatureBuilder::addSameTypeRequirement(
                                 UnresolvedType paOrT1,
                                 UnresolvedType paOrT2,
                                 FloatingRequirementSource source,
                                 UnresolvedHandlingKind unresolvedHandling) {
  return addSameTypeRequirement(paOrT1, paOrT2, source, unresolvedHandling,
                                [&](Type type1, Type type2) {
      Impl->HadAnyError = true;
      if (source.getLoc().isValid()) {
        Diags.diagnose(source.getLoc(), diag::requires_same_concrete_type,
                       type1, type2);
      }
    });
}

ConstraintResult GenericSignatureBuilder::addSameTypeRequirement(
    UnresolvedType paOrT1, UnresolvedType paOrT2,
    FloatingRequirementSource source,
    UnresolvedHandlingKind unresolvedHandling,
    llvm::function_ref<void(Type, Type)> diagnoseMismatch) {

  auto resolved1 = resolve(paOrT1, source);
  if (!resolved1) {
    return handleUnresolvedRequirement(RequirementKind::SameType, paOrT1,
                                       toUnresolvedRequirementRHS(paOrT2),
                                       source,
                                       resolved1.getUnresolvedEquivClass(),
                                       unresolvedHandling);
  }

  auto resolved2 = resolve(paOrT2, source);
  if (!resolved2) {
    return handleUnresolvedRequirement(RequirementKind::SameType, paOrT1,
                                       toUnresolvedRequirementRHS(paOrT2),
                                       source,
                                       resolved2.getUnresolvedEquivClass(),
                                       unresolvedHandling);
  }

  return addSameTypeRequirementDirect(resolved1, resolved2, source,
                                      diagnoseMismatch);
}

ConstraintResult GenericSignatureBuilder::addSameTypeRequirementDirect(
    ResolvedType type1, ResolvedType type2,
    FloatingRequirementSource source,
    llvm::function_ref<void(Type, Type)> diagnoseMismatch) {
  auto concreteType1 = type1.getAsConcreteType();
  auto concreteType2 = type2.getAsConcreteType();

  // If both sides of the requirement are concrete, equate them.
  if (concreteType1 && concreteType2) {
    return addSameTypeRequirementBetweenConcrete(concreteType1,
                                                 concreteType2, source,
                                                 diagnoseMismatch);
  }

  // If one side is concrete, map the other side to that concrete type.
  if (concreteType1) {
    return addSameTypeRequirementToConcrete(type2, concreteType1,
                       source.getSource(*this, type2.getDependentType(*this)));
  }

  if (concreteType2) {
    return addSameTypeRequirementToConcrete(type1, concreteType2,
                        source.getSource(*this, type1.getDependentType(*this)));
  }

  return addSameTypeRequirementBetweenTypeParameters(
                     type1, type2,
                     source.getSource(*this, type2.getDependentType(*this)));
}

ConstraintResult GenericSignatureBuilder::addInheritedRequirements(
                             TypeDecl *decl,
                             UnresolvedType type,
                             const RequirementSource *parentSource,
                             ModuleDecl *inferForModule) {
  if (isa<AssociatedTypeDecl>(decl) &&
      decl->hasInterfaceType() &&
      decl->getInterfaceType()->is<ErrorType>())
    return ConstraintResult::Resolved;

  // Walk the 'inherited' list to identify requirements.
  if (auto resolver = getLazyResolver())
    resolver->resolveInheritanceClause(decl);

  // Local function to get the source.
  auto getFloatingSource = [&](const TypeRepr *typeRepr, bool forInferred) {
    if (parentSource) {
      if (auto assocType = dyn_cast<AssociatedTypeDecl>(decl)) {
        auto proto = assocType->getProtocol();
        return FloatingRequirementSource::viaProtocolRequirement(
          parentSource, proto, typeRepr, forInferred);
      }

      auto proto = cast<ProtocolDecl>(decl);
      return FloatingRequirementSource::viaProtocolRequirement(
        parentSource, proto, typeRepr, forInferred);
    }

    // We are inferring requirements.
    if (forInferred) {
      return FloatingRequirementSource::forInferred(typeRepr);
    }

    // Explicit requirement.
    if (typeRepr)
      return FloatingRequirementSource::forExplicit(typeRepr);

    // An abstract explicit requirement.
    return FloatingRequirementSource::forAbstract();
  };

  auto visitType = [&](Type inheritedType, const TypeRepr *typeRepr) {
    if (inferForModule) {
      inferRequirements(*inferForModule,
                        TypeLoc(const_cast<TypeRepr *>(typeRepr),
                                inheritedType),
                        getFloatingSource(typeRepr, /*forInferred=*/true));
    }

    return addTypeRequirement(
        type, inheritedType, getFloatingSource(typeRepr,
                                               /*forInferred=*/false),
        UnresolvedHandlingKind::GenerateConstraints, inferForModule);
  };

  return visitInherited(decl->getInherited(), visitType);
}

ConstraintResult GenericSignatureBuilder::addRequirement(
                                                 const RequirementRepr *req,
                                                 ModuleDecl *inferForModule) {
  return addRequirement(req,
                        FloatingRequirementSource::forExplicit(req),
                        nullptr,
                        inferForModule);
}

ConstraintResult GenericSignatureBuilder::addRequirement(
                                             const RequirementRepr *Req,
                                             FloatingRequirementSource source,
                                             const SubstitutionMap *subMap,
                                             ModuleDecl *inferForModule) {
  auto subst = [&](Type t) {
    if (subMap)
      return t.subst(*subMap, SubstFlags::UseErrorType);

    return t;
  };

  auto getInferredTypeLoc = [=](Type type, TypeLoc existingTypeLoc) {
    if (subMap) return TypeLoc::withoutLoc(type);
    return existingTypeLoc;
  };

  switch (Req->getKind()) {
  case RequirementReprKind::LayoutConstraint: {
    auto subject = subst(Req->getSubject());
    if (inferForModule) {
      inferRequirements(*inferForModule,
                        getInferredTypeLoc(subject, Req->getSubjectLoc()),
                        source.asInferred(Req->getSubjectLoc().getTypeRepr()));
    }

    return addLayoutRequirement(subject,
                                Req->getLayoutConstraint(),
                                source,
                                UnresolvedHandlingKind::GenerateConstraints);
  }

  case RequirementReprKind::TypeConstraint: {
    auto subject = subst(Req->getSubject());
    auto constraint = subst(Req->getConstraint());
    if (inferForModule) {
      inferRequirements(*inferForModule,
                        getInferredTypeLoc(subject, Req->getSubjectLoc()),
                        source.asInferred(Req->getSubjectLoc().getTypeRepr()));
      inferRequirements(*inferForModule,
                        getInferredTypeLoc(constraint,
                                           Req->getConstraintLoc()),
                        source.asInferred(
                                      Req->getConstraintLoc().getTypeRepr()));
    }
    return addTypeRequirement(subject, constraint, source,
                              UnresolvedHandlingKind::GenerateConstraints,
                              inferForModule);
  }

  case RequirementReprKind::SameType: {
    // Warn if neither side of the requirement contains a type parameter.
    if (!Req->getFirstType()->hasTypeParameter() &&
        !Req->getSecondType()->hasTypeParameter() &&
        !Req->getFirstType()->hasError() &&
        !Req->getSecondType()->hasError()) {
      Diags.diagnose(Req->getEqualLoc(),
                     diag::requires_no_same_type_archetype,
                     Req->getFirstType(), Req->getSecondType())
        .highlight(Req->getFirstTypeLoc().getSourceRange())
        .highlight(Req->getSecondTypeLoc().getSourceRange());
    }

    auto firstType = subst(Req->getFirstType());
    auto secondType = subst(Req->getSecondType());
    if (inferForModule) {
      inferRequirements(*inferForModule,
                        getInferredTypeLoc(firstType, Req->getFirstTypeLoc()),
                        source.asInferred(
                                        Req->getFirstTypeLoc().getTypeRepr()));
      inferRequirements(*inferForModule,
                        getInferredTypeLoc(secondType,
                                           Req->getSecondTypeLoc()),
                        source.asInferred(
                                        Req->getSecondTypeLoc().getTypeRepr()));
    }
    return addRequirement(Requirement(RequirementKind::SameType,
                                      firstType, secondType),
                          source, nullptr);
  }
  }

  llvm_unreachable("Unhandled requirement?");
}

ConstraintResult
GenericSignatureBuilder::addRequirement(const Requirement &req,
                                        FloatingRequirementSource source,
                                        ModuleDecl *inferForModule) {
  auto firstType = req.getFirstType();

  switch (req.getKind()) {
  case RequirementKind::Superclass:
  case RequirementKind::Conformance: {
    auto secondType = req.getSecondType();

    if (inferForModule) {
      inferRequirements(*inferForModule, TypeLoc::withoutLoc(firstType),
                        FloatingRequirementSource::forInferred(
                            nullptr));
      inferRequirements(*inferForModule, TypeLoc::withoutLoc(secondType),
                        FloatingRequirementSource::forInferred(
                            nullptr));
    }

    return addTypeRequirement(firstType, secondType, source,
                              UnresolvedHandlingKind::GenerateConstraints,
                              inferForModule);
  }

  case RequirementKind::Layout: {
    if (inferForModule) {
      inferRequirements(*inferForModule, TypeLoc::withoutLoc(firstType),
                        FloatingRequirementSource::forInferred(nullptr));
    }

    return addLayoutRequirement(firstType, req.getLayoutConstraint(), source,
                                UnresolvedHandlingKind::GenerateConstraints);
  }

  case RequirementKind::SameType: {
    auto secondType = req.getSecondType();

    if (inferForModule) {
      inferRequirements(*inferForModule, TypeLoc::withoutLoc(firstType),
                        FloatingRequirementSource::forInferred(
                            nullptr));
      inferRequirements(*inferForModule, TypeLoc::withoutLoc(secondType),
                        FloatingRequirementSource::forInferred(
                            nullptr));
    }

    return addSameTypeRequirement(
        firstType, secondType, source,
        UnresolvedHandlingKind::GenerateConstraints,
        [&](Type type1, Type type2) {
          Impl->HadAnyError = true;
          if (source.getLoc().isValid()) {
            Diags.diagnose(source.getLoc(), diag::requires_same_concrete_type,
                           type1, type2);
          }
        });
  }
  }

  llvm_unreachable("Unhandled requirement?");
}

/// AST walker that infers requirements from type representations.
class GenericSignatureBuilder::InferRequirementsWalker : public TypeWalker {
  ModuleDecl &module;
  GenericSignatureBuilder &Builder;
  FloatingRequirementSource source;

public:
  InferRequirementsWalker(ModuleDecl &module,
                          GenericSignatureBuilder &builder,
                          FloatingRequirementSource source)
    : module(module), Builder(builder), source(source) { }

  Action walkToTypePre(Type ty) override {
    // Unbound generic types are the result of recovered-but-invalid code, and
    // don't have enough info to do any useful substitutions.
    if (ty->is<UnboundGenericType>())
      return Action::Stop;

    return Action::Continue;
  }

  Action walkToTypePost(Type ty) override {
    // Infer from generic typealiases.
    if (auto NameAlias = dyn_cast<NameAliasType>(ty.getPointer())) {
      auto decl = NameAlias->getDecl();
      auto genericSig = decl->getGenericSignature();
      if (!genericSig)
        return Action::Continue;

      auto subMap = NameAlias->getSubstitutionMap();
      for (const auto &rawReq : genericSig->getRequirements()) {
        if (auto req = rawReq.subst(subMap))
          Builder.addRequirement(*req, source, nullptr);
      }

      return Action::Continue;
    }

    // Infer from generic nominal types.
    auto decl = ty->getAnyNominal();
    if (!decl) return Action::Continue;

    auto genericSig = decl->getGenericSignature();
    if (!genericSig)
      return Action::Continue;

    /// Retrieve the substitution.
    auto subMap = ty->getContextSubstitutionMap(&module, decl,
                                                decl->getGenericEnvironment());

    // Handle the requirements.
    // FIXME: Inaccurate TypeReprs.
    for (const auto &rawReq : genericSig->getRequirements()) {
      if (auto req = rawReq.subst(subMap))
        Builder.addRequirement(*req, source, nullptr);
    }

    return Action::Continue;
  }
};

void GenericSignatureBuilder::inferRequirements(
                                          ModuleDecl &module,
                                          TypeLoc type,
                                          FloatingRequirementSource source) {
  if (!type.getType())
    return;
  // FIXME: Crummy source-location information.
  InferRequirementsWalker walker(module, *this, source);
  type.getType().walk(walker);
}

void GenericSignatureBuilder::inferRequirements(
                                          ModuleDecl &module,
                                          ParameterList *params,
                                          GenericParamList *genericParams) {
  if (genericParams == nullptr)
    return;

  for (auto P : *params) {
    inferRequirements(module, P->getTypeLoc(),
                      FloatingRequirementSource::forInferred(
                          P->getTypeLoc().getTypeRepr()));
  }
}

namespace swift {
  template<typename T>
  bool operator<(const Constraint<T> &lhs, const Constraint<T> &rhs) {
    // FIXME: Awful.
    auto lhsSource = lhs.getSubjectDependentType({ });
    auto rhsSource = rhs.getSubjectDependentType({ });
    if (int result = compareDependentTypes(lhsSource, rhsSource))
      return result < 0;

    if (int result = lhs.source->compare(rhs.source))
      return result < 0;

    return false;
  }

  template<typename T>
  bool operator==(const Constraint<T> &lhs, const Constraint<T> &rhs){
    return lhs.hasSameSubjectAs(rhs) &&
           lhs.value == rhs.value &&
           lhs.source == rhs.source;
  }

  template<>
  bool operator==(const Constraint<Type> &lhs, const Constraint<Type> &rhs){
    return lhs.hasSameSubjectAs(rhs) &&
           lhs.value->isEqual(rhs.value) &&
           lhs.source == rhs.source;
  }
} // namespace swift

namespace {
  /// Retrieve the representative constraint that will be used for diagnostics.
  template<typename T>
  Optional<Constraint<T>> findRepresentativeConstraint(
                            ArrayRef<Constraint<T>> constraints,
                            llvm::function_ref<bool(const Constraint<T> &)>
                                                   isSuitableRepresentative) {
    // Find a representative constraint.
    Optional<Constraint<T>> representativeConstraint;
    for (const auto &constraint : constraints) {
      // If this isn't a suitable representative constraint, ignore it.
      if (!isSuitableRepresentative(constraint))
        continue;

      // Check whether this constraint is better than the best we've seen so far
      // at being the representative constraint against which others will be
      // compared.
      if (!representativeConstraint) {
        representativeConstraint = constraint;
        continue;
      }

      // We prefer derived constraints to non-derived constraints.
      bool thisIsDerived = constraint.source->isDerivedRequirement();
      bool representativeIsDerived =
        representativeConstraint->source->isDerivedRequirement();
      if (thisIsDerived != representativeIsDerived) {
        if (thisIsDerived)
          representativeConstraint = constraint;

        continue;
      }

      // We prefer constraints that are explicit to inferred constraints.
      bool thisIsInferred = constraint.source->isInferredRequirement();
      bool representativeIsInferred =
        representativeConstraint->source->isInferredRequirement();
      if (thisIsInferred != representativeIsInferred) {
        if (thisIsInferred)
          representativeConstraint = constraint;

        continue;
      }

      // We prefer constraints with locations to constraints without locations.
      bool thisHasValidSourceLoc = constraint.source->getLoc().isValid();
      bool representativeHasValidSourceLoc =
        representativeConstraint->source->getLoc().isValid();
      if (thisHasValidSourceLoc != representativeHasValidSourceLoc) {
        if (thisHasValidSourceLoc)
          representativeConstraint = constraint;

        continue;
      }

      // Otherwise, order via the constraint itself.
      if (constraint < *representativeConstraint)
        representativeConstraint = constraint;
    }

    return representativeConstraint;
  }
} // end anonymous namespace

/// For each potential archetype within the given equivalence class that is
/// an associated type, expand the protocol requirements for the enclosing
/// protocol.
static void expandSameTypeConstraints(GenericSignatureBuilder &builder,
                                      EquivalenceClass *equivClass) {
  auto genericParams = builder.getGenericParams();
  auto existingMembers = equivClass->members;
  for (auto pa : existingMembers) {
    // Make sure that there are only associated types that chain up to the
    // parent.
    bool foundNonAssociatedType = false;
    for (auto currentPA = pa; auto parentPA = currentPA->getParent();
         currentPA = parentPA){
      if (!currentPA->getResolvedType()) {
        foundNonAssociatedType = true;
        break;
      }
    }
    if (foundNonAssociatedType) continue;

    auto dependentType = pa->getDependentType(genericParams);
    for (const auto &conforms : equivClass->conformsTo) {
      auto proto = conforms.first;

      // Check whether we already have a conformance constraint for this
      // potential archetype.
      bool alreadyFound = false;
      const RequirementSource *conformsSource = nullptr;
      for (const auto &constraint : conforms.second) {
        if (constraint.source->getAffectedType()->isEqual(dependentType)) {
          alreadyFound = true;
          break;
        }

        // Capture the source for later use, skipping
        if (!conformsSource &&
            constraint.source->kind
              != RequirementSource::RequirementSignatureSelf)
          conformsSource = constraint.source;
      }

      if (alreadyFound) continue;
      if (!conformsSource) continue;

      // Pick a source at random and reseat it on this potential archetype.
      auto source = conformsSource->viaEquivalentType(builder, dependentType);

      // Expand same-type constraints.
      builder.expandConformanceRequirement(pa, proto, source,
                                           /*onlySameTypeConstraints=*/true);
    }
  }
}

void
GenericSignatureBuilder::finalize(SourceLoc loc,
                              TypeArrayView<GenericTypeParamType> genericParams,
                              bool allowConcreteGenericParams) {
  // Process any delayed requirements that we can handle now.
  processDelayedRequirements();

  assert(!Impl->finalized && "Already finalized builder");
#ifndef NDEBUG
  Impl->finalized = true;
#endif

  // Local function (+ cache) describing the set of equivalence classes
  // directly referenced by the concrete same-type constraint of the given
  // equivalence class.
  llvm::DenseMap<EquivalenceClass *,
                 SmallPtrSet<EquivalenceClass *, 4>> concreteEquivClasses;
  auto getConcreteReferencedEquivClasses
      = [&](EquivalenceClass *equivClass)
          -> SmallPtrSet<EquivalenceClass *, 4> {
    auto known = concreteEquivClasses.find(equivClass);
    if (known != concreteEquivClasses.end())
      return known->second;

    SmallPtrSet<EquivalenceClass *, 4> referencedEquivClasses;
    if (!equivClass->concreteType ||
        !equivClass->concreteType->hasTypeParameter())
      return referencedEquivClasses;

    equivClass->concreteType.visit([&](Type type) {
      if (type->isTypeParameter()) {
        if (auto referencedEquivClass =
              resolveEquivalenceClass(type,
                                      ArchetypeResolutionKind::AlreadyKnown)) {
          referencedEquivClasses.insert(referencedEquivClass);
        }
      }
    });

    concreteEquivClasses[equivClass] = referencedEquivClasses;
    return referencedEquivClasses;
  };

  /// Check whether the given type references the archetype.
  auto isRecursiveConcreteType = [&](EquivalenceClass *equivClass,
                                     bool isSuperclass) {
    SmallPtrSet<EquivalenceClass *, 4> visited;
    SmallVector<EquivalenceClass *, 4> stack;
    stack.push_back(equivClass);
    visited.insert(equivClass);

    // Check whether the specific type introduces recursion.
    auto checkTypeRecursion = [&](Type type) {
      if (!type->hasTypeParameter()) return false;

      return type.findIf([&](Type type) {
        if (type->isTypeParameter()) {
          if (auto referencedEquivClass =
                resolveEquivalenceClass(
                                    type,
                                    ArchetypeResolutionKind::AlreadyKnown)) {
            if (referencedEquivClass == equivClass) return true;

            if (visited.insert(referencedEquivClass).second)
              stack.push_back(referencedEquivClass);
          }
        }

        return false;
      });
    };

    while (!stack.empty()) {
      auto currentEquivClass = stack.back();
      stack.pop_back();

      // If we're checking superclasses, do so now.
      if (isSuperclass && currentEquivClass->superclass &&
          checkTypeRecursion(currentEquivClass->superclass)) {
        return true;
      }

      // Otherwise, look for the equivalence classes referenced by
      // same-type constraints.
      for (auto referencedEquivClass :
             getConcreteReferencedEquivClasses(currentEquivClass)) {
        // If we found a reference to the original archetype, it's recursive.
        if (referencedEquivClass == equivClass) return true;

        if (visited.insert(referencedEquivClass).second)
          stack.push_back(referencedEquivClass);
      }
    }

    return false;
  };

  // Check for recursive or conflicting same-type bindings and superclass
  // constraints.
  for (auto &equivClass : Impl->EquivalenceClasses) {
    if (equivClass.concreteType) {
      // Check for recursive same-type bindings.
      if (isRecursiveConcreteType(&equivClass, /*isSuperclass=*/false)) {
        if (auto constraint =
              equivClass.findAnyConcreteConstraintAsWritten()) {
          Impl->HadAnyError = true;

          Diags.diagnose(constraint->source->getLoc(),
                         diag::recursive_same_type_constraint,
                         constraint->getSubjectDependentType(genericParams),
                         constraint->value);
        }

        equivClass.recursiveConcreteType = true;
      } else {
        checkConcreteTypeConstraints(genericParams, &equivClass);
      }
    }

    // Check for recursive superclass bindings.
    if (equivClass.superclass) {
      if (isRecursiveConcreteType(&equivClass, /*isSuperclass=*/true)) {
        if (auto source = equivClass.findAnySuperclassConstraintAsWritten()) {
          Impl->HadAnyError = true;

          Diags.diagnose(source->source->getLoc(),
                         diag::recursive_superclass_constraint,
                         source->getSubjectDependentType(genericParams),
                         equivClass.superclass);
        }

        equivClass.recursiveSuperclassType = true;
      } else {
        checkSuperclassConstraints(genericParams, &equivClass);
      }
    }

    checkConformanceConstraints(genericParams, &equivClass);
    checkLayoutConstraints(genericParams, &equivClass);
  };

  // FIXME: Expand all conformance requirements. This is expensive :(
  for (auto &equivClass : Impl->EquivalenceClasses) {
      expandSameTypeConstraints(*this, &equivClass);
  }

  // Check same-type constraints.
  for (auto &equivClass : Impl->EquivalenceClasses) {
    checkSameTypeConstraints(genericParams, &equivClass);
  }

  // Check for generic parameters which have been made concrete or equated
  // with each other.
  if (!allowConcreteGenericParams) {
    SmallPtrSet<PotentialArchetype *, 4> visited;
    
    unsigned depth = 0;
    for (const auto gp : getGenericParams())
      depth = std::max(depth, gp->getDepth());

    for (const auto pa : Impl->PotentialArchetypes) {
      auto rep = pa->getRepresentative();

      if (pa->getRootGenericParamKey().Depth < depth)
        continue;

      if (!visited.insert(rep).second)
        continue;

      // Don't allow a generic parameter to be equivalent to a concrete type,
      // because then we don't actually have a parameter.
      auto equivClass = rep->getOrCreateEquivalenceClass(*this);
      if (equivClass->concreteType) {
        if (auto constraint = equivClass->findAnyConcreteConstraintAsWritten()){
          Impl->HadAnyError = true;

          Diags.diagnose(constraint->source->getLoc(),
                         diag::requires_generic_param_made_equal_to_concrete,
                         rep->getDependentType(genericParams));
        }
        continue;
      }

      // Don't allow two generic parameters to be equivalent, because then we
      // don't actually have two parameters.
      for (auto other : rep->getEquivalenceClassMembers()) {
        // If it isn't a generic parameter, skip it.
        if (other == pa || !other->isGenericParam()) continue;

        // Try to find an exact constraint that matches 'other'.
        auto repConstraint =
          findRepresentativeConstraint<Type>(
            equivClass->sameTypeConstraints,
            [pa, other](const Constraint<Type> &constraint) {
              return (constraint.isSubjectEqualTo(pa) &&
                      constraint.value->isEqual(other->getDependentType({ }))) ||
                (constraint.isSubjectEqualTo(other) &&
                 constraint.value->isEqual(pa->getDependentType({ })));
            });


         // Otherwise, just take any old constraint.
        if (!repConstraint) {
          repConstraint =
            findRepresentativeConstraint<Type>(
              equivClass->sameTypeConstraints,
              [](const Constraint<Type> &constraint) {
                return true;
              });
        }

        if (repConstraint && repConstraint->source->getLoc().isValid()) {
          Impl->HadAnyError = true;

          Diags.diagnose(repConstraint->source->getLoc(),
                         diag::requires_generic_params_made_equal,
                         pa->getDependentType(genericParams),
                         other->getDependentType(genericParams));
        }
        break;
      }
    }
  }
}

/// Turn a requirement right-hand side into an unresolved type.
static GenericSignatureBuilder::UnresolvedType asUnresolvedType(
                        GenericSignatureBuilder::UnresolvedRequirementRHS rhs) {
  if (auto pa = rhs.dyn_cast<PotentialArchetype *>())
    return GenericSignatureBuilder::UnresolvedType(pa);

  return GenericSignatureBuilder::UnresolvedType(rhs.get<Type>());
}

void GenericSignatureBuilder::processDelayedRequirements() {
  // If we're already up-to-date, do nothing.
  if (Impl->Generation == Impl->LastProcessedGeneration) { return; }

  // If there are no delayed requirements, do nothing.
  if (Impl->DelayedRequirements.empty()) { return; }

  if (Impl->ProcessingDelayedRequirements) { return; }

  ++NumProcessDelayedRequirements;

  llvm::SaveAndRestore<bool> processing(Impl->ProcessingDelayedRequirements,
                                        true);
  bool anyChanges = false;
  SWIFT_DEFER {
    Impl->LastProcessedGeneration = Impl->Generation;
    if (!anyChanges)
      ++NumProcessDelayedRequirementsUnchanged;
  };

  bool anySolved;
  do {
    // Steal the delayed requirements so we can reprocess them.
    anySolved = false;
    auto delayed = std::move(Impl->DelayedRequirements);
    Impl->DelayedRequirements.clear();

    // Process delayed requirements.
    for (const auto &req : delayed) {
      // Reprocess the delayed requirement.
      ConstraintResult reqResult;
      switch (req.kind) {
      case DelayedRequirement::Type:
        reqResult =
            addTypeRequirement(req.lhs, asUnresolvedType(req.rhs), req.source,
                               UnresolvedHandlingKind::GenerateUnresolved,
                               /*inferForModule=*/nullptr);
        break;

      case DelayedRequirement::Layout:
        reqResult = addLayoutRequirement(
                           req.lhs, req.rhs.get<LayoutConstraint>(), req.source,
                           UnresolvedHandlingKind::GenerateUnresolved);
        break;

      case DelayedRequirement::SameType:
        reqResult = addSameTypeRequirement(
                               req.lhs, asUnresolvedType(req.rhs), req.source,
                               UnresolvedHandlingKind::GenerateUnresolved);
        break;
      }

      // Update our state based on what happened.
      switch (reqResult) {
      case ConstraintResult::Concrete:
        ++NumDelayedRequirementConcrete;
        anySolved = true;
        break;

      case ConstraintResult::Conflicting:
        anySolved = true;
        break;

      case ConstraintResult::Resolved:
        ++NumDelayedRequirementResolved;
        anySolved = true;
        break;

      case ConstraintResult::Unresolved:
        // Add the requirement back.
        ++NumDelayedRequirementUnresolved;
        break;
      }
    }

    if (anySolved) {
      anyChanges = true;
    }
  } while (anySolved);
}

template<typename T>
Constraint<T> GenericSignatureBuilder::checkConstraintList(
                           TypeArrayView<GenericTypeParamType> genericParams,
                           std::vector<Constraint<T>> &constraints,
                           llvm::function_ref<bool(const Constraint<T> &)>
                             isSuitableRepresentative,
                           llvm::function_ref<
                             ConstraintRelation(const Constraint<T>&)>
                               checkConstraint,
                           Optional<Diag<unsigned, Type, T, T>>
                             conflictingDiag,
                           Diag<Type, T> redundancyDiag,
                           Diag<unsigned, Type, T> otherNoteDiag) {
  return checkConstraintList<T, T>(genericParams, constraints,
                                   isSuitableRepresentative, checkConstraint,
                                   conflictingDiag, redundancyDiag,
                                   otherNoteDiag,
                                   [](const T& value) { return value; },
                                   /*removeSelfDerived=*/true);
}

namespace {
  /// Remove self-derived sources from the given vector of constraints.
  ///
  /// \returns true if any derived-via-concrete constraints were found.
  template<typename T>
  bool removeSelfDerived(GenericSignatureBuilder &builder,
                         std::vector<Constraint<T>> &constraints,
                         ProtocolDecl *proto,
                         bool dropDerivedViaConcrete = true,
                         bool allCanBeSelfDerived = false) {
    auto genericParams = builder.getGenericParams();
    bool anyDerivedViaConcrete = false;
    Optional<Constraint<T>> remainingConcrete;
    SmallVector<Constraint<T>, 4> minimalSources;
    constraints.erase(
      std::remove_if(constraints.begin(), constraints.end(),
        [&](const Constraint<T> &constraint) {
          bool derivedViaConcrete;
          auto minimalSource =
            constraint.source->getMinimalConformanceSource(
                         builder,
                         constraint.getSubjectDependentType(genericParams),
                         proto, derivedViaConcrete);
          if (minimalSource != constraint.source) {
            // The minimal source is smaller than the original source, so the
            // original source is self-derived.
            ++NumSelfDerived;

            if (minimalSource) {
              // Record a constraint with a minimized source.
              minimalSources.push_back(
                           {constraint.subject,
                             constraint.value,
                             minimalSource});
            }

            return true;
          }

           if (!derivedViaConcrete)
             return false;

           anyDerivedViaConcrete = true;

           if (!dropDerivedViaConcrete)
             return false;

           // Drop derived-via-concrete requirements.
           if (!remainingConcrete)
             remainingConcrete = constraint;

           ++NumSelfDerived;
           return true;
         }),
      constraints.end());

    // If we found any minimal sources, add them now, avoiding introducing any
    // redundant sources.
    if (!minimalSources.empty()) {
      // Collect the sources we already know about.
      SmallPtrSet<const RequirementSource *, 4> sources;
      for (const auto &constraint : constraints) {
        sources.insert(constraint.source);
      }

      // Add any minimal sources we didn't know about.
      for (const auto &minimalSource : minimalSources) {
        if (sources.insert(minimalSource.source).second) {
          constraints.push_back(minimalSource);
        }
      }
    }

    // If we only had concrete conformances, put one back.
    if (constraints.empty() && remainingConcrete)
      constraints.push_back(*remainingConcrete);

    assert((!constraints.empty() || allCanBeSelfDerived) &&
           "All constraints were self-derived!");
    return anyDerivedViaConcrete;
  }
} // end anonymous namespace

template<typename T, typename DiagT>
Constraint<T> GenericSignatureBuilder::checkConstraintList(
                           TypeArrayView<GenericTypeParamType> genericParams,
                           std::vector<Constraint<T>> &constraints,
                           llvm::function_ref<bool(const Constraint<T> &)>
                             isSuitableRepresentative,
                           llvm::function_ref<
                             ConstraintRelation(const Constraint<T>&)>
                               checkConstraint,
                           Optional<Diag<unsigned, Type, DiagT, DiagT>>
                             conflictingDiag,
                           Diag<Type, DiagT> redundancyDiag,
                           Diag<unsigned, Type, DiagT> otherNoteDiag,
                           llvm::function_ref<DiagT(const T&)> diagValue,
                           bool removeSelfDerived) {
  assert(!constraints.empty() && "No constraints?");
  if (removeSelfDerived) {
    ::removeSelfDerived(*this, constraints, /*proto=*/nullptr);
  }

  // Sort the constraints, so we get a deterministic ordering of diagnostics.
  llvm::array_pod_sort(constraints.begin(), constraints.end());

  // Find a representative constraint.
  auto representativeConstraint =
    findRepresentativeConstraint<T>(constraints, isSuitableRepresentative);

  // Local function to provide a note describing the representative constraint.
  auto noteRepresentativeConstraint = [&] {
    if (representativeConstraint->source->getLoc().isInvalid()) return;

    Diags.diagnose(representativeConstraint->source->getLoc(),
                   otherNoteDiag,
                   representativeConstraint->source->classifyDiagKind(),
                   representativeConstraint->getSubjectDependentType(
                                                               genericParams),
                   diagValue(representativeConstraint->value));
  };

  // Go through the concrete constraints looking for redundancies.
  bool diagnosedConflictingRepresentative = false;
  for (const auto &constraint : constraints) {
    // Leave the representative alone.
    if (representativeConstraint && constraint == *representativeConstraint)
      continue;

    switch (checkConstraint(constraint)) {
    case ConstraintRelation::Unrelated:
      continue;

    case ConstraintRelation::Conflicting: {
      // Figure out what kind of subject we have; it will affect the
      // diagnostic.
      auto getSubjectType =
        [&](Type subjectType) -> std::pair<unsigned, Type> {
          unsigned kind;
          if (auto gp = subjectType->getAs<GenericTypeParamType>()) {
            if (gp->getDecl() &&
                isa<ProtocolDecl>(gp->getDecl()->getDeclContext())) {
              kind = 1;
              subjectType = cast<ProtocolDecl>(gp->getDecl()->getDeclContext())
                              ->getDeclaredInterfaceType();
            } else {
              kind = 0;
            }
          } else {
            kind = 2;
          }

          return std::make_pair(kind, subjectType);
        };


      // The requirement conflicts. If this constraint has a location, complain
      // about it.
      if (constraint.source->getLoc().isValid()) {
        Impl->HadAnyError = true;

        auto subject =
          getSubjectType(constraint.getSubjectDependentType(genericParams));
        Diags.diagnose(constraint.source->getLoc(), *conflictingDiag,
                       subject.first, subject.second,
                       diagValue(constraint.value),
                       diagValue(representativeConstraint->value));

        noteRepresentativeConstraint();
        break;
      }

      // If the representative itself conflicts and we haven't diagnosed it yet,
      // do so now.
      if (!diagnosedConflictingRepresentative &&
          representativeConstraint->source->getLoc().isValid()) {
        Impl->HadAnyError = true;

        auto subject =
          getSubjectType(
            representativeConstraint->getSubjectDependentType(genericParams));
        Diags.diagnose(representativeConstraint->source->getLoc(),
                       *conflictingDiag,
                       subject.first, subject.second,
                       diagValue(representativeConstraint->value),
                       diagValue(constraint.value));

        diagnosedConflictingRepresentative = true;
        break;
      }
      break;
    }

    case ConstraintRelation::Redundant:
      // If this requirement is not derived or inferred (but has a useful
      // location) complain that it is redundant.
      Impl->HadAnyRedundantConstraints = true;
      if (constraint.source->shouldDiagnoseRedundancy(true) &&
          representativeConstraint &&
          representativeConstraint->source->shouldDiagnoseRedundancy(false)) {
        Diags.diagnose(constraint.source->getLoc(),
                       redundancyDiag,
                       constraint.getSubjectDependentType(genericParams),
                       diagValue(constraint.value));

        noteRepresentativeConstraint();
      }
      break;
    }
  }

  return *representativeConstraint;
}

/// Determine whether this is a redundantly inheritable Objective-C protocol.
///
/// A redundantly-inheritable Objective-C protocol is one where we will
/// silently accept a directly-stated redundant conformance to this protocol,
/// and emit this protocol in the list of "inherited" protocols. There are
/// two cases where we allow this:
///
//    1) For a protocol defined in Objective-C, so that we will match Clang's
///      behavior, and
///   2) For an @objc protocol defined in Swift that directly inherits from
///      JavaScriptCore's JSExport, which depends on this behavior.
static bool isRedundantlyInheritableObjCProtocol(
                                             ProtocolDecl *proto,
                                             const RequirementSource *source) {
  if (!proto->isObjC()) return false;

  // Only do this for the requirement signature computation.
  auto parentSource = source->parent;
  if (!parentSource ||
      parentSource->kind != RequirementSource::RequirementSignatureSelf)
    return false;

  // Check the two conditions in which we will suppress the diagnostic and
  // emit the redundant inheritance.
  auto inheritingProto = parentSource->getProtocolDecl();
  if (!inheritingProto->hasClangNode() && !proto->getName().is("JSExport"))
    return false;

  // If the inheriting protocol already has @_restatedObjCConformance with
  // this protocol, we're done.
  for (auto *attr : inheritingProto->getAttrs()
                      .getAttributes<RestatedObjCConformanceAttr>()) {
    if (attr->Proto == proto) return true;
  }

  // Otherwise, add @_restatedObjCConformance.
  auto &ctx = proto->getASTContext();
  inheritingProto->getAttrs().add(new (ctx) RestatedObjCConformanceAttr(proto));
  return true;
}

void GenericSignatureBuilder::checkConformanceConstraints(
                          TypeArrayView<GenericTypeParamType> genericParams,
                          EquivalenceClass *equivClass) {
  for (auto &entry : equivClass->conformsTo) {
    // Remove self-derived constraints.
    assert(!entry.second.empty() && "No constraints to work with?");

    // Remove any self-derived constraints.
    removeSelfDerived(*this, entry.second, entry.first);

    checkConstraintList<ProtocolDecl *, ProtocolDecl *>(
      genericParams, entry.second,
      [](const Constraint<ProtocolDecl *> &constraint) {
        return true;
      },
      [&](const Constraint<ProtocolDecl *> &constraint) {
        auto proto = constraint.value;
        assert(proto == entry.first && "Mixed up protocol constraints");

        // If this conformance requirement recursively makes a protocol
        // conform to itself, don't complain here.
        auto source = constraint.source;
        auto rootSource = source->getRoot();
        if (rootSource->kind == RequirementSource::RequirementSignatureSelf &&
            source != rootSource &&
            proto == rootSource->getProtocolDecl() &&
            areInSameEquivalenceClass(rootSource->getRootType(),
                                      source->getAffectedType())) {
          return ConstraintRelation::Unrelated;
        }

        // If this is a redundantly inherited Objective-C protocol, treat it
        // as "unrelated" to silence the warning about the redundant
        // conformance.
        if (isRedundantlyInheritableObjCProtocol(proto, constraint.source))
          return ConstraintRelation::Unrelated;

        return ConstraintRelation::Redundant;
      },
      None,
      diag::redundant_conformance_constraint,
      diag::redundant_conformance_here,
      [](ProtocolDecl *proto) { return proto; },
      /*removeSelfDerived=*/false);
  }
}

namespace swift {
  bool operator<(const DerivedSameTypeComponent &lhs,
                 const DerivedSameTypeComponent &rhs) {
    return compareDependentTypes(getUnresolvedType(lhs.anchor, {}),
                                 getUnresolvedType(rhs.anchor, {})) < 0;
  }
} // namespace swift

/// Find the representative in a simple union-find data structure of
/// integral values.
static unsigned findRepresentative(SmallVectorImpl<unsigned> &parents,
                                   unsigned index) {
  if (parents[index] == index) return index;

  return parents[index] = findRepresentative(parents, parents[index]);
}

/// Union the same-type components denoted by \c index1 and \c index2.
///
/// \param successThreshold Returns true when two sets have been joined
/// and both representatives are below the threshold. The default of 0
/// is equivalent to \c successThreshold == parents.size().
///
/// \returns \c true if the two components were separate and have now
/// been joined; \c false if they were already in the same set.
static bool unionSets(SmallVectorImpl<unsigned> &parents,
                      unsigned index1, unsigned index2,
                      unsigned successThreshold = 0) {
  // Find the representatives of each component class.
  unsigned rep1 = findRepresentative(parents, index1);
  unsigned rep2 = findRepresentative(parents, index2);
  if (rep1 == rep2) return false;

  // Point at the lowest-numbered representative.
  if (rep1 < rep2)
    parents[rep2] = rep1;
  else
    parents[rep1] = rep2;

  return (successThreshold == 0) ||
    (rep1 < successThreshold && rep2 < successThreshold);
}

/// Computes the ordered set of archetype anchors required to form a minimum
/// spanning tree among the connected components formed by only the derived
/// same-type requirements within the equivalence class \c equivClass.
///
/// The equivalence class contains all potential archetypes that are made
/// equivalent by the known set of same-type constraints, which includes both
/// directly-stated same-type constraints (e.g., \c T.A == T.B) as well as
/// same-type constraints that are implied either because the names coincide
/// (e.g., \c T[.P1].A == T[.P2].A) or due to a requirement in a protocol.
///
/// The equivalence class of the given representative potential archetype
/// (\c rep) is formed from a graph whose vertices are the potential archetypes
/// and whose edges are the same-type constraints. These edges include both
/// directly-stated same-type constraints (e.g., \c T.A == T.B) as well as
/// same-type constraints that are implied either because the names coincide
/// (e.g., \c T[.P1].A == T[.P2].A) or due to a requirement in a protocol.
/// The equivalence class forms a single connected component.
///
/// Within that graph is a subgraph that includes only those edges that are
/// implied (and, therefore, excluding those edges that were explicitly stated).
/// The connected components within that subgraph describe the potential
/// archetypes that would be equivalence even with all of the (explicit)
/// same-type constraints removed.
///
/// The entire equivalence class can be restored by introducing edges between
/// the connected components. This function computes a minimal, canonicalized
/// set of edges (same-type constraints) needed to describe the equivalence
/// class, which is suitable for the generation of the canonical generic
/// signature.
///
/// The resulting set of "edges" is returned as a set of vertices, one per
/// connected component (of the subgraph). Each is the anchor for that
/// connected component (as determined by \c compareDependentTypes()), and the
/// set itself is ordered by \c compareDependentTypes(). The actual set of
/// canonical edges connects vertex i to vertex i+1 for i in 0..<size-1.
static void computeDerivedSameTypeComponents(
              GenericSignatureBuilder &builder,
              EquivalenceClass *equivClass,
              llvm::SmallDenseMap<CanType, unsigned> &componentOf){
  // Set up the array of "parents" in the union-find data structure.
  llvm::SmallDenseMap<CanType, unsigned> parentIndices;
  SmallVector<unsigned, 4> parents;
  for (unsigned i : indices(equivClass->members)) {
    Type depType = equivClass->members[i]->getDependentType({ });
    parentIndices[depType->getCanonicalType()] = parents.size();
    parents.push_back(i);
  }

  // Walk all of the same-type constraints, performing a union-find operation.
  for (const auto &constraint : equivClass->sameTypeConstraints) {
    // Treat nested-type-name-match constraints specially.
    if (constraint.source->getRoot()->kind ==
          RequirementSource::NestedTypeNameMatch)
      continue;

    // Skip non-derived constraints.
    if (!constraint.source->isDerivedRequirement()) continue;

    CanType source =
      constraint.getSubjectDependentType({ })->getCanonicalType();
    CanType target = constraint.value->getCanonicalType();

    assert(parentIndices.count(source) == 1 && "Missing source");
    assert(parentIndices.count(target) == 1 && "Missing target");
    unionSets(parents, parentIndices[source], parentIndices[target]);
  }

  // Compute and record the components.
  auto &components = equivClass->derivedSameTypeComponents;
  for (unsigned i : indices(equivClass->members)) {
    auto pa = equivClass->members[i];
    CanType depType = pa->getDependentType({ })->getCanonicalType();

    // Find the representative of this set.
    assert(parentIndices.count(depType) == 1 && "Unknown member?");
    unsigned index = parentIndices[depType];
    unsigned representative = findRepresentative(parents, index);

    // If this is the representative, add a component for it.
    if (representative == index) {
      componentOf[depType] = components.size();
      components.push_back(DerivedSameTypeComponent{pa, nullptr});
      continue;
    }

    // This is not the representative; point at the component of the
    // representative.
    CanType representativeDepTy =
      equivClass->members[representative]->getDependentType({ })
        ->getCanonicalType();
    assert(componentOf.count(representativeDepTy) == 1 &&
           "Missing representative component?");
    unsigned componentIndex = componentOf[representativeDepTy];
    componentOf[depType] = componentIndex;

    // If this is a better anchor, record it.
    if (compareDependentTypes(
            depType, getUnresolvedType(components[componentIndex].anchor, {})) <
        0)
      components[componentIndex].anchor = pa;
  }

  // If there is a concrete type, figure out the best concrete type anchor
  // per component.
auto genericParams = builder.getGenericParams();
  for (const auto &concrete : equivClass->concreteTypeConstraints) {
    // Dig out the component associated with constraint.
    Type subjectType = concrete.getSubjectDependentType(genericParams);
    assert(componentOf.count(subjectType->getCanonicalType()) > 0);
    auto &component = components[componentOf[subjectType->getCanonicalType()]];

    // FIXME: Skip self-derived sources. This means our attempts to "stage"
    // construction of self-derived sources really don't work, because we
    // discover more information later, so we need a more on-line or
    // iterative approach.
    bool derivedViaConcrete;
    if (concrete.source->isSelfDerivedSource(builder, subjectType,
                                             derivedViaConcrete))
      continue;

    // If it has a better source than we'd seen before for this component,
    // keep it.
    auto &bestConcreteTypeSource = component.concreteTypeSource;
    if (!bestConcreteTypeSource ||
        concrete.source->compare(bestConcreteTypeSource) < 0)
      bestConcreteTypeSource = concrete.source;
  }
}

namespace {
  /// An edge in the same-type constraint graph that spans two different
  /// components.
  struct IntercomponentEdge {
    unsigned source;
    unsigned target;
    Constraint<Type> constraint;
    bool isSelfDerived = false;

    IntercomponentEdge(unsigned source, unsigned target,
                       const Constraint<Type> &constraint)
      : source(source), target(target), constraint(constraint)
    {
      assert(source != target && "Not an intercomponent edge");
      if (this->source > this->target) std::swap(this->source, this->target);
    }

    friend bool operator<(const IntercomponentEdge &lhs,
                          const IntercomponentEdge &rhs) {
      if (lhs.source != rhs.source)
        return lhs.source < rhs.source;
      if (lhs.target != rhs.target)
        return lhs.target < rhs.target;

      // Prefer non-inferred requirement sources.
      bool lhsIsInferred = lhs.constraint.source->isInferredRequirement();
      bool rhsIsInferred = rhs.constraint.source->isInferredRequirement();
      if (lhsIsInferred != rhsIsInferred)
        return rhsIsInferred;;

      return lhs.constraint < rhs.constraint;
    }

    LLVM_ATTRIBUTE_DEPRECATED(void dump() const LLVM_ATTRIBUTE_USED,
                              "only for use in the debugger");
  };
}

void IntercomponentEdge::dump() const {
  llvm::errs() << constraint.getSubjectDependentType({ }).getString() << " -- "
    << constraint.value << ": ";
  constraint.source->print(llvm::errs(), nullptr);
  llvm::errs() << "\n";
}

/// Determine whether the removal of the given edge will disconnect the
/// nodes \c from and \c to within the given equivalence class.
static bool removalDisconnectsEquivalenceClass(
               EquivalenceClass *equivClass,
               llvm::SmallDenseMap<CanType, unsigned> &componentOf,
               std::vector<IntercomponentEdge> &sameTypeEdges,
               unsigned edgeIndex,
               CanType fromDepType,
               CanType toDepType) {
  // Which component are "from" and "to" in within the intercomponent edges?
  assert(componentOf.count(fromDepType) > 0);
  auto fromComponentIndex = componentOf[fromDepType];

  assert(componentOf.count(toDepType) > 0);
  auto toComponentIndex = componentOf[toDepType];

  // If they're in the same component, they're always connected (due to
  // derived edges).
  if (fromComponentIndex == toComponentIndex) return false;

  /// Describes the parents in the equivalance classes we're forming.
  SmallVector<unsigned, 4> parents;
  for (unsigned i : range(equivClass->derivedSameTypeComponents.size())) {
    parents.push_back(i);
  }

  for (const auto existingEdgeIndex : indices(sameTypeEdges)) {
    if (existingEdgeIndex == edgeIndex) continue;

    const auto &edge = sameTypeEdges[existingEdgeIndex];
    if (edge.isSelfDerived) continue;

    if (unionSets(parents, edge.source, edge.target) &&
        findRepresentative(parents, fromComponentIndex) ==
          findRepresentative(parents, toComponentIndex))
      return false;
  }

  const auto &edge = sameTypeEdges[edgeIndex];

  return !unionSets(parents, edge.source, edge.target) ||
    findRepresentative(parents, fromComponentIndex) !=
      findRepresentative(parents, toComponentIndex);
}

static AssociatedTypeDecl *takeMemberOfDependentMemberType(Type &type) {
  if (auto depMemTy = type->getAs<DependentMemberType>()) {
    type = depMemTy->getBase();
    return depMemTy->getAssocType();
  }

  return nullptr;
}

static bool isSelfDerivedNestedTypeNameMatchEdge(
              GenericSignatureBuilder &builder,
              EquivalenceClass *equivClass,
              llvm::SmallDenseMap<CanType, unsigned> &componentOf,
              std::vector<IntercomponentEdge> &sameTypeEdges,
              unsigned edgeIndex) {
  const auto &edge = sameTypeEdges[edgeIndex];
  auto genericParams = builder.getGenericParams();
  Type sourceType = edge.constraint.getSubjectDependentType(genericParams);
  Type target = edge.constraint.value;

  DependentMemberType *sourceDepMemTy;
  while ((sourceDepMemTy = sourceType->getAs<DependentMemberType>()) &&
         sourceDepMemTy->getAssocType() ==
           takeMemberOfDependentMemberType(target)) {
    sourceType = sourceDepMemTy->getBase();

    auto targetEquivClass =
      builder.maybeResolveEquivalenceClass(target,
                                           ArchetypeResolutionKind::WellFormed,
                                           false)
        .getEquivalenceClassIfPresent();
    if (targetEquivClass == equivClass &&
        builder.maybeResolveEquivalenceClass(
                                     sourceType,
                                     ArchetypeResolutionKind::WellFormed,
                                     /*wantExactPotentialArchetype=*/false)
          .getEquivalenceClass(builder) == equivClass &&
        !removalDisconnectsEquivalenceClass(equivClass, componentOf,
                                            sameTypeEdges, edgeIndex,
                                            sourceType->getCanonicalType(),
                                            target->getCanonicalType()))
      return true;
  }

  return false;
}

/// Collapse same-type components using the "delayed" requirements of the
/// equivalence class.
///
/// This operation looks through the delayed requirements within the equivalence
/// class to find paths that connect existing potential archetypes.
static void collapseSameTypeComponentsThroughDelayedRequirements(
              GenericSignatureBuilder &builder,
              EquivalenceClass *equivClass,
              llvm::SmallDenseMap<CanType, unsigned> &componentOf,
              SmallVectorImpl<unsigned> &collapsedParents,
              unsigned &remainingComponents) {
  unsigned numCollapsedParents = collapsedParents.size();

  /// "Virtual" components for types that aren't resolve to potential
  /// archetypes.
  llvm::SmallDenseMap<CanType, unsigned> virtualComponents;

  /// Retrieve the component for a type representing a virtual component
  auto getTypeVirtualComponent = [&](Type type) {
    CanType canType = type->getCanonicalType();
    auto knownActual = componentOf.find(canType);
    if (knownActual != componentOf.end())
      return knownActual->second;

    auto knownVirtual = virtualComponents.find(canType);
    if (knownVirtual != virtualComponents.end())
      return knownVirtual->second;

    unsigned component = collapsedParents.size();
    collapsedParents.push_back(component);
    virtualComponents[canType] = component;
    return component;
  };

  /// Retrieve the component for the given potential archetype.
  auto genericParams = builder.getGenericParams();
  auto getPotentialArchetypeVirtualComponent = [&](PotentialArchetype *pa) {
    if (pa->getEquivalenceClassIfPresent() == equivClass)
      return getTypeVirtualComponent(pa->getDependentType(genericParams));

    // We found a potential archetype in another equivalence class. Treat it
    // as a "virtual" component representing that potential archetype's
    // equivalence class.
    return getTypeVirtualComponent(
             pa->getRepresentative()->getDependentType(genericParams));
  };

  for (const auto &delayedReq : equivClass->delayedRequirements) {
    // Only consider same-type requirements.
    if (delayedReq.kind != DelayedRequirement::SameType) continue;

    unsigned lhsComponent;
    if (auto lhsPA = delayedReq.lhs.dyn_cast<PotentialArchetype *>())
      lhsComponent = getPotentialArchetypeVirtualComponent(lhsPA);
    else
      lhsComponent = getTypeVirtualComponent(delayedReq.lhs.get<Type>());

    unsigned rhsComponent;
    if (auto rhsPA = delayedReq.rhs.dyn_cast<PotentialArchetype *>())
      rhsComponent = getPotentialArchetypeVirtualComponent(rhsPA);
    else
      rhsComponent = getTypeVirtualComponent(delayedReq.rhs.get<Type>());

    // Collapse the sets
    if (unionSets(collapsedParents, lhsComponent, rhsComponent,
                  numCollapsedParents) &&
        lhsComponent < numCollapsedParents &&
        rhsComponent < numCollapsedParents)
      --remainingComponents;
  }

  /// Remove any additional collapsed parents we added.
  collapsedParents.erase(collapsedParents.begin() + numCollapsedParents,
                         collapsedParents.end());
}

/// Collapse same-type components within an equivalence class, minimizing the
/// number of requirements required to express the equivalence class.
static void collapseSameTypeComponents(
              GenericSignatureBuilder &builder,
              EquivalenceClass *equivClass,
              llvm::SmallDenseMap<CanType, unsigned> &componentOf,
              std::vector<IntercomponentEdge> &sameTypeEdges) {
  SmallVector<unsigned, 4> collapsedParents;
  for (unsigned i : indices(equivClass->derivedSameTypeComponents)) {
    collapsedParents.push_back(i);
  }

  unsigned remainingComponents = equivClass->derivedSameTypeComponents.size();
  for (unsigned edgeIndex : indices(sameTypeEdges)) {
    auto &edge = sameTypeEdges[edgeIndex];

    // If this edge is self-derived, remove it.
    if (isSelfDerivedNestedTypeNameMatchEdge(builder, equivClass, componentOf,
                                             sameTypeEdges, edgeIndex)) {
      // Note that this edge is self-derived, so we don't consider it again.
      edge.isSelfDerived = true;

      auto &constraints = equivClass->sameTypeConstraints;
      auto known =
        std::find_if(constraints.begin(), constraints.end(),
                     [&](const Constraint<Type> &existing) {
                       // Check the requirement source, first.
                       if (existing.source != edge.constraint.source)
                         return false;

                       return
                         (existing.hasSameSubjectAs(edge.constraint) &&
                          existing.value->isEqual(edge.constraint.value)) ||
                         (existing.isSubjectEqualTo(edge.constraint.value) &&
                          edge.constraint.isSubjectEqualTo(existing.value));
                     });
      assert(known != constraints.end());
      constraints.erase(known);
      continue;
    }

    // Otherwise, collapse the derived same-type components along this edge,
    // because it's derived.
    if (unionSets(collapsedParents, edge.source, edge.target))
      --remainingComponents;
  }

  if (remainingComponents > 1) {
    // Collapse same-type components by looking at the delayed requirements.
    collapseSameTypeComponentsThroughDelayedRequirements(
      builder, equivClass, componentOf, collapsedParents, remainingComponents);
  }

  // If needed, collapse the same-type components merged by a derived
  // nested-type-name-match edge.
  unsigned maxComponents = equivClass->derivedSameTypeComponents.size();
  if (remainingComponents < maxComponents) {
    std::vector<DerivedSameTypeComponent> newComponents;
    std::vector<unsigned> newIndices(maxComponents, maxComponents);

    for (unsigned oldIndex : range(0, maxComponents)) {
      auto &oldComponent = equivClass->derivedSameTypeComponents[oldIndex];
      unsigned oldRepresentativeIndex =
        findRepresentative(collapsedParents, oldIndex);

      // If this is the representative, it's a new component; record it.
      if (oldRepresentativeIndex == oldIndex) {
        assert(newIndices[oldIndex] == maxComponents &&
               "Already saw this component?");
        unsigned newIndex = newComponents.size();
        newIndices[oldIndex] = newIndex;
        newComponents.push_back(
          {oldComponent.anchor, oldComponent.concreteTypeSource});
        continue;
      }

      // This is not the representative; merge it into the representative
      // component.
      auto newRepresentativeIndex = newIndices[oldRepresentativeIndex];
      assert(newRepresentativeIndex != maxComponents &&
             "Representative should have come earlier");
      auto &newComponent = newComponents[newRepresentativeIndex];

      // If the old component has a better anchor, keep it.
      if (compareDependentTypes(getUnresolvedType(oldComponent.anchor, {}),
                                getUnresolvedType(newComponent.anchor, {})) < 0)
        newComponent.anchor = oldComponent.anchor;

      // If the old component has a better concrete type source, keep it.
      if (!newComponent.concreteTypeSource ||
          (oldComponent.concreteTypeSource &&
           oldComponent.concreteTypeSource
             ->compare(newComponent.concreteTypeSource) < 0))
        newComponent.concreteTypeSource = oldComponent.concreteTypeSource;
    }

    // Move the new results into place.
    equivClass->derivedSameTypeComponents = std::move(newComponents);
  }

  // Sort the components.
  llvm::array_pod_sort(equivClass->derivedSameTypeComponents.begin(),
                       equivClass->derivedSameTypeComponents.end());
}

void GenericSignatureBuilder::checkSameTypeConstraints(
                          TypeArrayView<GenericTypeParamType> genericParams,
                          EquivalenceClass *equivClass) {
  if (!equivClass->derivedSameTypeComponents.empty())
    return;

  bool anyDerivedViaConcrete = false;
  // Remove self-derived constraints.
  if (removeSelfDerived(*this, equivClass->sameTypeConstraints,
                        /*proto=*/nullptr,
                        /*dropDerivedViaConcrete=*/false,
                        /*allCanBeSelfDerived=*/true))
    anyDerivedViaConcrete = true;

  // Sort the constraints, so we get a deterministic ordering of diagnostics.
  llvm::array_pod_sort(equivClass->sameTypeConstraints.begin(),
                       equivClass->sameTypeConstraints.end());

  // Compute the components in the subgraph of the same-type constraint graph
  // that includes only derived constraints.
  llvm::SmallDenseMap<CanType, unsigned> componentOf;
  computeDerivedSameTypeComponents(*this, equivClass, componentOf);

  // Go through all of the same-type constraints, collecting all of the
  // non-derived constraints to put them into bins: intra-component and
  // inter-component.

  // Intra-component edges are stored per-component, so we can perform
  // diagnostics within each component.
  unsigned numComponents = equivClass->derivedSameTypeComponents.size();
  std::vector<std::vector<Constraint<Type>>>
    intracomponentEdges(numComponents,
                        std::vector<Constraint<Type>>());

  // Intercomponent edges are stored as one big list, which tracks the
  // source/target components.
  std::vector<IntercomponentEdge> intercomponentEdges;
  std::vector<IntercomponentEdge> nestedTypeNameMatchEdges;
  for (const auto &constraint : equivClass->sameTypeConstraints) {
    // If the source/destination are identical, complain.
    if (constraint.isSubjectEqualTo(constraint.value)) {
      if (constraint.source->shouldDiagnoseRedundancy(true)) {
        Diags.diagnose(constraint.source->getLoc(),
                       diag::redundant_same_type_constraint,
                       constraint.getSubjectDependentType(genericParams),
                       constraint.value);
      }

      continue;
    }

    // Determine which component each of the source/destination fall into.
    CanType subjectType =
      constraint.getSubjectDependentType({ })->getCanonicalType();
    assert(componentOf.count(subjectType) > 0 &&
           "unknown potential archetype?");
    unsigned firstComponentIdx = componentOf[subjectType];
    assert(componentOf.count(constraint.value->getCanonicalType()) > 0 &&
           "unknown potential archetype?");
    unsigned secondComponentIdx =
      componentOf[constraint.value->getCanonicalType()];

    // Separately track nested-type-name-match constraints.
    if (constraint.source->getRoot()->kind ==
          RequirementSource::NestedTypeNameMatch) {
      // If this is an intercomponent edge, record it separately.
      if (firstComponentIdx != secondComponentIdx) {
        nestedTypeNameMatchEdges.push_back(
          IntercomponentEdge(firstComponentIdx, secondComponentIdx, constraint));
      }

      continue;
    }

    // If both vertices are within the same component, this is an
    // intra-component edge. Record it as such.
    if (firstComponentIdx == secondComponentIdx) {
      intracomponentEdges[firstComponentIdx].push_back(constraint);
      continue;
    }

    // Otherwise, it's an intercomponent edge, which is never derived.
    assert(!constraint.source->isDerivedRequirement() &&
           "Must not be derived");

    // Ignore inferred requirements; we don't want to diagnose them.
    intercomponentEdges.push_back(
      IntercomponentEdge(firstComponentIdx, secondComponentIdx, constraint));
  }

  // If there were any derived-via-concrete constraints, drop them now before
  // we emit other diagnostics.
  if (anyDerivedViaConcrete) {
    // Remove derived-via-concrete constraints.
    (void)removeSelfDerived(*this, equivClass->sameTypeConstraints,
                            /*proto=*/nullptr,
                            /*dropDerivedViaConcrete=*/true,
                            /*allCanBeSelfDerived=*/true);
  }

  // Walk through each of the components, checking the intracomponent edges.
  // This will diagnose any explicitly-specified requirements within a
  // component, all of which are redundant.
  for (auto &constraints : intracomponentEdges) {
    if (constraints.empty()) continue;

    checkConstraintList<Type, Type>(
      genericParams, constraints,
      [](const Constraint<Type> &) { return true; },
      [](const Constraint<Type> &constraint) {
        // Ignore nested-type-name-match constraints.
        if (constraint.source->getRoot()->kind ==
              RequirementSource::NestedTypeNameMatch)
          return ConstraintRelation::Unrelated;

        return ConstraintRelation::Redundant;
      },
      None,
      diag::redundant_same_type_constraint,
      diag::previous_same_type_constraint,
      [&](Type type) {
        return type;
      },
      /*removeSelfDerived=*/false);
  }

  // Diagnose redundant same-type constraints across components. First,
  // sort the edges so that edges that between the same component pairs
  // occur next to each other.
  llvm::array_pod_sort(intercomponentEdges.begin(), intercomponentEdges.end());

  // Diagnose and erase any redundant edges between the same two components.
  intercomponentEdges.erase(
    std::unique(
      intercomponentEdges.begin(), intercomponentEdges.end(),
      [&](const IntercomponentEdge &lhs,
          const IntercomponentEdge &rhs) {
        // If either the source or target is different, we have
        // different elements.
        if (lhs.source != rhs.source || lhs.target != rhs.target)
          return false;

        // Check whethe we should diagnose redundancy for both constraints.
        if (!lhs.constraint.source->shouldDiagnoseRedundancy(true) ||
            !rhs.constraint.source->shouldDiagnoseRedundancy(false))
          return true;

        Diags.diagnose(lhs.constraint.source->getLoc(),
                       diag::redundant_same_type_constraint,
                       lhs.constraint.getSubjectDependentType(genericParams),
                       lhs.constraint.value);
        Diags.diagnose(rhs.constraint.source->getLoc(),
                       diag::previous_same_type_constraint,
                       rhs.constraint.source->classifyDiagKind(),
                       rhs.constraint.getSubjectDependentType(genericParams),
                       rhs.constraint.value);
        return true;
      }),
    intercomponentEdges.end());

  // If we have more intercomponent edges than are needed to form a spanning
  // tree, complain about redundancies. Note that the edges we have must
  // connect all of the components, or else we wouldn't have an equivalence
  // class.
  if (intercomponentEdges.size() > numComponents - 1) {
    std::vector<bool> connected(numComponents, false);
    const auto &firstEdge = intercomponentEdges.front();
    for (const auto &edge : intercomponentEdges) {
      // If both the source and target are already connected, this edge is
      // not part of the spanning tree.
      if (connected[edge.source] && connected[edge.target]) {
        if (edge.constraint.source->shouldDiagnoseRedundancy(true) &&
            firstEdge.constraint.source->shouldDiagnoseRedundancy(false)) {
          Diags.diagnose(edge.constraint.source->getLoc(),
                         diag::redundant_same_type_constraint,
                         edge.constraint.getSubjectDependentType(
                                                          genericParams),
                         edge.constraint.value);

          Diags.diagnose(firstEdge.constraint.source->getLoc(),
                         diag::previous_same_type_constraint,
                         firstEdge.constraint.source->classifyDiagKind(),
                         firstEdge.constraint.getSubjectDependentType(
                                                          genericParams),
                         firstEdge.constraint.value);
        }

        continue;
      }

      // Put the source and target into the spanning tree.
      connected[edge.source] = true;
      connected[edge.target] = true;
    }
  }

  collapseSameTypeComponents(*this, equivClass, componentOf,
                             nestedTypeNameMatchEdges);
}

void GenericSignatureBuilder::checkConcreteTypeConstraints(
                              TypeArrayView<GenericTypeParamType> genericParams,
                              EquivalenceClass *equivClass) {
  // Resolve any thus-far-unresolved dependent types.
  Type resolvedConcreteType =
    resolveDependentMemberTypes(*this, equivClass->concreteType);

  checkConstraintList<Type>(
    genericParams, equivClass->concreteTypeConstraints,
    [&](const ConcreteConstraint &constraint) {
      if (constraint.value->isEqual(resolvedConcreteType))
        return true;

      auto resolvedType =
        resolveDependentMemberTypes(*this, constraint.value);
      return resolvedType->isEqual(resolvedConcreteType);
    },
    [&](const Constraint<Type> &constraint) {
      Type concreteType = constraint.value;

      // If the concrete type is equivalent, the constraint is redundant.
      if (concreteType->isEqual(equivClass->concreteType))
        return ConstraintRelation::Redundant;

      // If either has a type parameter, call them unrelated.
      if (concreteType->hasTypeParameter() ||
          equivClass->concreteType->hasTypeParameter())
        return ConstraintRelation::Unrelated;

      return ConstraintRelation::Conflicting;
    },
    diag::same_type_conflict,
    diag::redundant_same_type_to_concrete,
    diag::same_type_redundancy_here);

  equivClass->concreteType = resolvedConcreteType;
}

void GenericSignatureBuilder::checkSuperclassConstraints(
                              TypeArrayView<GenericTypeParamType> genericParams,
                              EquivalenceClass *equivClass) {
  assert(equivClass->superclass && "No superclass constraint?");

  // Resolve any this-far-unresolved dependent types.
  Type resolvedSuperclass =
    resolveDependentMemberTypes(*this, equivClass->superclass);

  auto representativeConstraint =
    checkConstraintList<Type>(
      genericParams, equivClass->superclassConstraints,
      [&](const ConcreteConstraint &constraint) {
        if (constraint.value->isEqual(resolvedSuperclass))
          return true;

        Type resolvedType =
          resolveDependentMemberTypes(*this, constraint.value);
        return resolvedType->isEqual(equivClass->superclass);
      },
      [&](const Constraint<Type> &constraint) {
        Type superclass = constraint.value;

        // If this class is a superclass of the "best"
        if (superclass->isExactSuperclassOf(equivClass->superclass))
          return ConstraintRelation::Redundant;

        // Otherwise, it conflicts.
        return ConstraintRelation::Conflicting;
      },
      diag::requires_superclass_conflict,
      diag::redundant_superclass_constraint,
      diag::superclass_redundancy_here);

  // Resolve any this-far-unresolved dependent types.
  equivClass->superclass = resolvedSuperclass;

  // If we have a concrete type, check it.
  // FIXME: Substitute into the concrete type.
  if (equivClass->concreteType) {
    Type resolvedConcreteType =
      resolveDependentMemberTypes(*this, equivClass->concreteType);
    auto existing = equivClass->findAnyConcreteConstraintAsWritten();
    // Make sure the concrete type fulfills the superclass requirement.
    if (!equivClass->superclass->isExactSuperclassOf(resolvedConcreteType)){
      Impl->HadAnyError = true;
      if (existing) {
        Diags.diagnose(existing->source->getLoc(), diag::type_does_not_inherit,
                       existing->getSubjectDependentType(getGenericParams()),
                       existing->value, equivClass->superclass);

        if (representativeConstraint.source->getLoc().isValid()) {
          Diags.diagnose(representativeConstraint.source->getLoc(),
                         diag::superclass_redundancy_here,
                         representativeConstraint.source->classifyDiagKind(),
                         representativeConstraint.getSubjectDependentType(
                                                              genericParams),
                         equivClass->superclass);
        }
      } else if (representativeConstraint.source->getLoc().isValid()) {
        Diags.diagnose(representativeConstraint.source->getLoc(),
                       diag::type_does_not_inherit,
                       representativeConstraint.getSubjectDependentType(
                                                              genericParams),
                       resolvedConcreteType, equivClass->superclass);
      }
    } else if (representativeConstraint.source->shouldDiagnoseRedundancy(true)
               && existing &&
               existing->source->shouldDiagnoseRedundancy(false)) {
      // It does fulfill the requirement; diagnose the redundancy.
      Diags.diagnose(representativeConstraint.source->getLoc(),
                     diag::redundant_superclass_constraint,
                     representativeConstraint.getSubjectDependentType(
                                                              genericParams),
                     representativeConstraint.value);

      Diags.diagnose(existing->source->getLoc(),
                     diag::same_type_redundancy_here,
                     existing->source->classifyDiagKind(),
                     existing->getSubjectDependentType(genericParams),
                     existing->value);
    }
  }
}

void GenericSignatureBuilder::checkLayoutConstraints(
                              TypeArrayView<GenericTypeParamType> genericParams,
                              EquivalenceClass *equivClass) {
  if (!equivClass->layout) return;

  checkConstraintList<LayoutConstraint>(
    genericParams, equivClass->layoutConstraints,
    [&](const Constraint<LayoutConstraint> &constraint) {
      return constraint.value == equivClass->layout;
    },
    [&](const Constraint<LayoutConstraint> &constraint) {
      auto layout = constraint.value;

      // If the layout constraints are mergable, i.e. compatible,
      // it is a redundancy.
      if (layout.merge(equivClass->layout)->isKnownLayout())
        return ConstraintRelation::Redundant;

      return ConstraintRelation::Conflicting;
    },
    diag::conflicting_layout_constraints,
    diag::redundant_layout_constraint,
    diag::previous_layout_constraint);
}

namespace {
  /// Retrieve the best requirement source from a set of constraints.
  template<typename T>
  Optional<const RequirementSource *>
  getBestConstraintSource(ArrayRef<Constraint<T>> constraints,
                          llvm::function_ref<bool(const T&)> matches) {
    Optional<const RequirementSource *> bestSource;
    for (const auto &constraint : constraints) {
      if (!matches(constraint.value)) continue;

      if (!bestSource || constraint.source->compare(*bestSource) < 0)
        bestSource = constraint.source;
    }

    return bestSource;
  }

  using SameTypeComponentRef = std::pair<EquivalenceClass *, unsigned>;

} // end anonymous namespace

static int compareSameTypeComponents(const SameTypeComponentRef *lhsPtr,
                                     const SameTypeComponentRef *rhsPtr){
  Type lhsType = getUnresolvedType(
      lhsPtr->first->derivedSameTypeComponents[lhsPtr->second].anchor,
      { });
  Type rhsType = getUnresolvedType(
      rhsPtr->first->derivedSameTypeComponents[rhsPtr->second].anchor,
      { });

  return compareDependentTypes(lhsType, rhsType);
}

void GenericSignatureBuilder::enumerateRequirements(
                   TypeArrayView<GenericTypeParamType> genericParams,
                   llvm::function_ref<
                     void (RequirementKind kind,
                           Type type,
                           RequirementRHS constraint,
                           const RequirementSource *source)> f) {
  // Collect all of the subject types that will be involved in constraints.
  SmallVector<SameTypeComponentRef, 8> subjects;
  for (auto &equivClass : Impl->EquivalenceClasses) {
    if (equivClass.derivedSameTypeComponents.empty()) {
      checkSameTypeConstraints(getGenericParams(), &equivClass);
    }

    for (unsigned i : indices(equivClass.derivedSameTypeComponents))
      subjects.push_back({&equivClass, i});
  }

  // Sort the subject types in canonical order.
  llvm::array_pod_sort(subjects.begin(), subjects.end(),
                       compareSameTypeComponents);

  for (const auto &subject : subjects) {
    // Dig out the subject type and its corresponding component.
    auto equivClass = subject.first;
    auto &component = equivClass->derivedSameTypeComponents[subject.second];
    Type subjectType = getUnresolvedType(component.anchor, genericParams);

    // If this equivalence class is bound to a concrete type, equate the
    // anchor with a concrete type.
    if (Type concreteType = equivClass->concreteType) {
      // If the parent of this anchor is also a concrete type, don't
      // create a requirement.
      if (!subjectType->is<GenericTypeParamType>() &&
          maybeResolveEquivalenceClass(
            subjectType->castTo<DependentMemberType>()->getBase(),
            ArchetypeResolutionKind::WellFormed,
            /*wantExactPotentialArchetype=*/false)
            .getEquivalenceClass(*this)->concreteType)
        continue;

      auto source =
        component.concreteTypeSource
          ? component.concreteTypeSource
          : RequirementSource::forAbstract(*this, subjectType);

      // Drop recursive and invalid concrete-type constraints.
      if (equivClass->recursiveConcreteType ||
          equivClass->invalidConcreteType)
        continue;

      f(RequirementKind::SameType, subjectType, concreteType, source);
      continue;
    }

    std::function<void()> deferredSameTypeRequirement;

    // If we're at the last anchor in the component, do nothing;
    if (subject.second + 1 != equivClass->derivedSameTypeComponents.size()) {
      // Form a same-type constraint from this anchor within the component
      // to the next.
      // FIXME: Distinguish between explicit and inferred here?
      auto &nextComponent =
        equivClass->derivedSameTypeComponents[subject.second + 1];
      Type otherSubjectType =
        getUnresolvedType(nextComponent.anchor, genericParams);
      deferredSameTypeRequirement =
        [&f, subjectType, otherSubjectType, this] {
          f(RequirementKind::SameType, subjectType, otherSubjectType,
            RequirementSource::forAbstract(*this, otherSubjectType));
        };
    }

    SWIFT_DEFER {
      if (deferredSameTypeRequirement) deferredSameTypeRequirement();
    };

    // If this is not the first component anchor in its equivalence class,
    // we're done.
    if (subject.second > 0)
      continue;

    // If we have a superclass, produce a superclass requirement
    if (equivClass->superclass && !equivClass->recursiveSuperclassType) {
      auto bestSource =
        getBestConstraintSource<Type>(equivClass->superclassConstraints,
           [&](const Type &type) {
             return type->isEqual(equivClass->superclass);
          });

      if (!bestSource)
        bestSource = RequirementSource::forAbstract(*this, subjectType);

      f(RequirementKind::Superclass, subjectType, equivClass->superclass,
        *bestSource);
    }

    // If we have a layout constraint, produce a layout requirement.
    if (equivClass->layout) {
      auto bestSource = getBestConstraintSource<LayoutConstraint>(
                          equivClass->layoutConstraints,
                          [&](const LayoutConstraint &layout) {
                            return layout == equivClass->layout;
                          });
      if (!bestSource)
        bestSource = RequirementSource::forAbstract(*this, subjectType);

      f(RequirementKind::Layout, subjectType, equivClass->layout, *bestSource);
    }

    // Enumerate conformance requirements.
    SmallVector<ProtocolDecl *, 4> protocols;
    DenseMap<ProtocolDecl *, const RequirementSource *> protocolSources;
    if (equivClass) {
      for (const auto &conforms : equivClass->conformsTo) {
        protocols.push_back(conforms.first);
        assert(protocolSources.count(conforms.first) == 0 && 
               "redundant protocol requirement?");

        protocolSources.insert(
          {conforms.first,
           *getBestConstraintSource<ProtocolDecl *>(conforms.second,
             [&](ProtocolDecl *proto) {
               return proto == conforms.first;
             })});
      }
    }

    // Sort the protocols in canonical order.
    llvm::array_pod_sort(protocols.begin(), protocols.end(), 
                         TypeDecl::compare);

    // Enumerate the conformance requirements.
    for (auto proto : protocols) {
      assert(protocolSources.count(proto) == 1 && "Missing conformance?");
      f(RequirementKind::Conformance, subjectType,
        proto->getDeclaredInterfaceType(),
        protocolSources.find(proto)->second);
    }
  };
}

void GenericSignatureBuilder::dump() {
  dump(llvm::errs());
}

void GenericSignatureBuilder::dump(llvm::raw_ostream &out) {
  out << "Requirements:";
  enumerateRequirements(getGenericParams(),
                        [&](RequirementKind kind,
                            Type type,
                            RequirementRHS constraint,
                            const RequirementSource *source) {
    switch (kind) {
    case RequirementKind::Conformance:
    case RequirementKind::Superclass:
      out << "\n  ";
      out << type.getString() << " : "
          << constraint.get<Type>().getString() << " [";
      source->print(out, &Context.SourceMgr);
      out << "]";
      break;
    case RequirementKind::Layout:
      out << "\n  ";
      out << type.getString() << " : "
          << constraint.get<LayoutConstraint>().getString() << " [";
      source->print(out, &Context.SourceMgr);
      out << "]";
      break;
    case RequirementKind::SameType:
      out << "\n  ";
      out << type.getString() << " == " ;
      if (auto secondType = constraint.dyn_cast<Type>()) {
        out << secondType.getString();
      } else {
        out << constraint.get<PotentialArchetype *>()->getDebugName();
      }
      out << " [";
      source->print(out, &Context.SourceMgr);
      out << "]";
      break;
    }
  });
  out << "\n";

  out << "Potential archetypes:\n";
  for (auto pa : Impl->PotentialArchetypes) {
    pa->dump(out, &Context.SourceMgr, 2);
  }
  out << "\n";
}

void GenericSignatureBuilder::addGenericSignature(GenericSignature *sig) {
  if (!sig) return;

  for (auto param : sig->getGenericParams())
    addGenericParameter(param);

  for (auto &reqt : sig->getRequirements())
    addRequirement(reqt, FloatingRequirementSource::forAbstract(), nullptr);
}

/// Collect the set of requirements placed on the given generic parameters and
/// their associated types.
static void collectRequirements(GenericSignatureBuilder &builder,
                                TypeArrayView<GenericTypeParamType> params,
                                SmallVectorImpl<Requirement> &requirements) {
  builder.enumerateRequirements(
      params,
      [&](RequirementKind kind,
          Type depTy,
          RequirementRHS type,
          const RequirementSource *source) {
    // Filter out derived requirements... except for concrete-type requirements
    // on generic parameters. The exception is due to the canonicalization of
    // generic signatures, which never eliminates generic parameters even when
    // they have been mapped to a concrete type.
    if (source->isDerivedRequirement() &&
        !(kind == RequirementKind::SameType &&
          depTy->is<GenericTypeParamType>() &&
          type.is<Type>()))
      return;

    if (depTy->hasError())
      return;

    assert(!depTy->findUnresolvedDependentMemberType() &&
           "Unresolved dependent member type in requirements");

    Type repTy;
    if (auto concreteTy = type.dyn_cast<Type>()) {
      // Maybe we were equated to a concrete or dependent type...
      repTy = concreteTy;

      // Drop requirements involving concrete types containing
      // unresolved associated types.
      if (repTy->findUnresolvedDependentMemberType())
        return;
    } else {
      auto layoutConstraint = type.get<LayoutConstraint>();
      requirements.push_back(Requirement(kind, depTy, layoutConstraint));
      return;
    }

    if (repTy->hasError())
      return;

    requirements.push_back(Requirement(kind, depTy, repTy));
  });
}

GenericSignature *GenericSignatureBuilder::computeGenericSignature(
                                          SourceLoc loc,
                                          bool allowConcreteGenericParams,
                                          bool allowBuilderToMove) && {
  // Finalize the builder, producing any necessary diagnostics.
  finalize(loc, getGenericParams(), allowConcreteGenericParams);

  // Collect the requirements placed on the generic parameter types.
  SmallVector<Requirement, 4> requirements;
  collectRequirements(*this, getGenericParams(), requirements);

  // Form the generic signature.
  auto sig = GenericSignature::get(getGenericParams(), requirements);

  // When we can, move this generic signature builder to make it the canonical
  // builder, rather than constructing a new generic signature builder that
  // will produce the same thing.
  //
  // We cannot do this when there were errors.
  // FIXME: The HadAnyRedundantConstraints bit is a hack because we are
  // over-minimizing.
  if (allowBuilderToMove && !Impl->HadAnyError &&
      !Impl->HadAnyRedundantConstraints) {
    // Register this generic signature builder as the canonical builder for the
    // given signature.
    Context.registerGenericSignatureBuilder(sig, std::move(*this));
  }

  // Wipe out the internal state, ensuring that nobody uses this builder for
  // anything more.
  Impl.reset();

  return sig;
}

GenericSignature *GenericSignatureBuilder::computeRequirementSignature(
                                                     ProtocolDecl *proto) {
  GenericSignatureBuilder builder(proto->getASTContext());

  // Add the 'self' parameter.
  auto selfType =
    proto->getSelfInterfaceType()->castTo<GenericTypeParamType>();
  builder.addGenericParameter(selfType);

  // Add the conformance of 'self' to the protocol.
  auto requirement =
    Requirement(RequirementKind::Conformance, selfType,
                proto->getDeclaredInterfaceType());

  builder.addRequirement(
                 requirement,
                 RequirementSource::forRequirementSignature(builder, selfType,
                                                            proto),
                 nullptr);

  return std::move(builder).computeGenericSignature(
           SourceLoc(),
           /*allowConcreteGenericPArams=*/false,
           /*allowBuilderToMove=*/false);
}

#pragma mark Generic signature verification

void GenericSignatureBuilder::verifyGenericSignature(ASTContext &context,
                                                     GenericSignature *sig) {
  llvm::errs() << "Validating generic signature: ";
  sig->print(llvm::errs());
  llvm::errs() << "\n";

  // Try removing each requirement in turn.
  auto genericParams = sig->getGenericParams();
  auto requirements = sig->getRequirements();
  for (unsigned victimIndex : indices(requirements)) {
    PrettyStackTraceGenericSignature debugStack("verifying", sig, victimIndex);

    // Form a new generic signature builder.
    GenericSignatureBuilder builder(context);

    // Add the generic parameters.
    for (auto gp : genericParams)
      builder.addGenericParameter(gp);

    // Add the requirements *except* the victim.
    auto source = FloatingRequirementSource::forAbstract();
    for (unsigned i : indices(requirements)) {
      if (i != victimIndex)
        builder.addRequirement(requirements[i], source, nullptr);
    }

    // Finalize the generic signature. If there were any errors, we formed
    // an invalid signature, so just continue.
    if (builder.Impl->HadAnyError) continue;

    // Form a generic signature from the result.
    auto newSig =
      std::move(builder).computeGenericSignature(
                                      SourceLoc(),
                                      /*allowConcreteGenericParams=*/true,
                                      /*allowBuilderToMove=*/true);

    // If the removed requirement is satisfied by the new generic signature,
    // it is redundant. Complain.
    if (newSig->isRequirementSatisfied(requirements[victimIndex])) {
      SmallString<32> reqString;
      {
        llvm::raw_svector_ostream out(reqString);
        requirements[victimIndex].print(out, PrintOptions());
      }
      context.Diags.diagnose(SourceLoc(), diag::generic_signature_not_minimal,
                             reqString, sig->getAsString());
    }

    // Canonicalize the signature to check that it is canonical.
    (void)newSig->getCanonicalSignature();
  }
}

void GenericSignatureBuilder::verifyGenericSignaturesInModule(
                                                        ModuleDecl *module) {
  LoadedFile *loadedFile = nullptr;
  for (auto fileUnit : module->getFiles()) {
    loadedFile = dyn_cast<LoadedFile>(fileUnit);
    if (loadedFile) break;
  }

  if (!loadedFile) return;

  // Check all of the (canonical) generic signatures.
  SmallVector<GenericSignature *, 8> allGenericSignatures;
  SmallPtrSet<CanGenericSignature, 4> knownGenericSignatures;
  (void)loadedFile->getAllGenericSignatures(allGenericSignatures);
  ASTContext &context = module->getASTContext();
  for (auto genericSig : allGenericSignatures) {
    // Check whether this is the first time we've checked this (canonical)
    // signature.
    auto canGenericSig = genericSig->getCanonicalSignature();
    if (!knownGenericSignatures.insert(canGenericSig).second) continue;

    verifyGenericSignature(context, canGenericSig);
  }
}


