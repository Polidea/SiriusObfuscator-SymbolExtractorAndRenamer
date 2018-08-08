//===----------- SyntaxParsingContext.h -==============----------*- C++ -*-===//
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

#ifndef SWIFT_PARSE_SYNTAXPARSINGCONTEXT_H
#define SWIFT_PARSE_SYNTAXPARSINGCONTEXT_H

#include "llvm/ADT/PointerUnion.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/Syntax/Syntax.h"
#include "swift/Syntax/TokenSyntax.h"

namespace swift {
class SourceFile;
class Token;
class DiagnosticEngine;

namespace syntax {
class RawSyntax;
enum class SyntaxKind;
}

using namespace swift::syntax;

enum class SyntaxContextKind {
  Decl,
  Stmt,
  Expr,
  Type,
  Pattern,
  Syntax,
};

constexpr size_t SyntaxAlignInBits = 3;

/// RAII object which receive RawSyntax parts. On destruction, this constructs
/// a specified syntax node from received parts and propagate it to the parent
/// context.
///
/// e.g.
///   parseExprParen() {
///     SyntaxParsingContext LocalCtxt(SyntaxKind::ParenExpr, SyntaxContext);
///     consumeToken(tok::l_paren) // In consumeToken(), a RawTokenSyntax is
///                                // added to the context.
///     parseExpr(); // On returning from parseExpr(), a Expr Syntax node is
///                  // created and added to the context.
///     consumeToken(tok::r_paren)
///     // Now the context holds { '(' Expr ')' }.
///     // From these parts, it creates ParenExpr node and add it to the parent.
///   }
class alignas(1 << SyntaxAlignInBits) SyntaxParsingContext {
public:
  /// The shared data for all syntax parsing contexts with the same root.
  /// This should be accessible from the root context only.
  struct alignas(1 << SyntaxAlignInBits) RootContextData {
    // The source file under parsing.
    SourceFile &SF;

    // Where to issue diagnostics.
    DiagnosticEngine &Diags;

    SourceManager &SourceMgr;

    unsigned BufferID;

    // Storage for Collected parts.
    std::vector<RC<RawSyntax>> Storage;

    RootContextData(SourceFile &SF, DiagnosticEngine &Diags,
                    SourceManager &SourceMgr, unsigned BufferID)
        : SF(SF), Diags(Diags), SourceMgr(SourceMgr), BufferID(BufferID) {}
  };

private:
  /// Indicates what action should be performed on the destruction of
  ///  SyntaxParsingContext
  enum class AccumulationMode {
    // Coerece the result to one of ContextKind.
    // E.g. for ContextKind::Expr, passthroug if the result is CallExpr, whereas
    // <UnknownExpr><SomeDecl /></UnknownDecl> for non Exprs.
    CoerceKind,

    // Construct a result Syntax with specified SyntaxKind.
    CreateSyntax,

    // Pass through all parts to the parent context.
    Transparent,

    // Discard all parts in the context.
    Discard,

    // Construct SourceFile syntax to the specified SF.
    Root,

    // Invalid.
    NotSet,
  };

  // When this context is a root, this points to an instance of RootContextData;
  // When this context isn't a root, this points to the parent context.
  const llvm::PointerUnion<RootContextData *, SyntaxParsingContext *>
      RootDataOrParent;

  // Reference to the
  SyntaxParsingContext *&CtxtHolder;

  SyntaxArena &Arena;

  std::vector<RC<RawSyntax>> &Storage;

  // Offet for 'Storage' this context owns from.
  const size_t Offset;

  // Operation on destruction.
  AccumulationMode Mode = AccumulationMode::NotSet;

  // Additional info depending on \c Mode.
  union {
    // For AccumulationMode::CreateSyntax; desired syntax node kind.
    SyntaxKind SynKind;
    // For AccumulationMode::CoerceKind; desired syntax node category.
    SyntaxContextKind CtxtKind;
  };

  // If false, context does nothing.
  bool Enabled;

  /// Create a syntax node using the tail \c N elements of collected parts and
  /// replace those parts with the single result.
  void createNodeInPlace(SyntaxKind Kind, size_t N);

  ArrayRef<RC<RawSyntax>> getParts() const {
    return makeArrayRef(Storage).drop_front(Offset);
  }

  RC<RawSyntax> makeUnknownSyntax(SyntaxKind Kind,
                                  ArrayRef<RC<RawSyntax>> Parts);
  RC<RawSyntax> createSyntaxAs(SyntaxKind Kind, ArrayRef<RC<RawSyntax>> Parts);
  RC<RawSyntax> bridgeAs(SyntaxContextKind Kind, ArrayRef<RC<RawSyntax>> Parts);

public:
  /// Construct root context.
  SyntaxParsingContext(SyntaxParsingContext *&CtxtHolder, SourceFile &SF,
                       unsigned BufferID);

  /// Designated constructor for child context.
  SyntaxParsingContext(SyntaxParsingContext *&CtxtHolder)
      : RootDataOrParent(CtxtHolder), CtxtHolder(CtxtHolder),
        Arena(CtxtHolder->Arena),
        Storage(CtxtHolder->Storage), Offset(Storage.size()),
        Enabled(CtxtHolder->isEnabled()) {
    assert(CtxtHolder->isTopOfContextStack() &&
           "SyntaxParsingContext cannot have multiple children");
    CtxtHolder = this;
  }

  SyntaxParsingContext(SyntaxParsingContext *&CtxtHolder, SyntaxContextKind Kind)
      : SyntaxParsingContext(CtxtHolder) {
    setCoerceKind(Kind);
  }

  SyntaxParsingContext(SyntaxParsingContext *&CtxtHolder, SyntaxKind Kind)
      : SyntaxParsingContext(CtxtHolder) {
    setCreateSyntax(Kind);
  }

  ~SyntaxParsingContext();

  void disable() { Enabled = false; }
  bool isEnabled() const { return Enabled; }
  bool isRoot() const { return RootDataOrParent.is<RootContextData*>(); }
  bool isTopOfContextStack() const { return this == CtxtHolder; }

  SyntaxParsingContext *getParent() {
    return RootDataOrParent.get<SyntaxParsingContext*>();
  }

  RootContextData &getRootData() {
    return *getRoot()->RootDataOrParent.get<RootContextData*>();
  }

  SyntaxParsingContext *getRoot();

  /// Add RawSyntax to the parts.
  void addRawSyntax(RC<RawSyntax> Raw);

  /// Add Token with Trivia to the parts.
  void addToken(Token &Tok, Trivia &LeadingTrivia,
                Trivia &TrailingTrivia);

  /// Add Syntax to the parts.
  void addSyntax(Syntax Node);

  template<typename SyntaxNode>
  llvm::Optional<SyntaxNode> popIf() {
    assert(Storage.size() > Offset);
    if (auto Node = make<Syntax>(Storage.back()).getAs<SyntaxNode>()) {
      Storage.pop_back();
      return Node;
    }
    return None;
  }

  TokenSyntax popToken() {
    assert(Storage.size() > Offset);
    assert(Storage.back()->getKind() == SyntaxKind::Token);
    auto Node = make<TokenSyntax>(std::move(Storage.back()));
    Storage.pop_back();
    return Node;
  }

  /// Create a node using the tail of the collected parts. The number of parts
  /// is automatically determined from \c Kind. Node: limited number of \c Kind
  /// are supported. See the implementation.
  void createNodeInPlace(SyntaxKind Kind);

  /// Squshing nodes from the back of the pending syntax list to a given syntax
  /// collection kind. If there're no nodes can fit into the collection kind,
  /// this function does nothing. Otherwise, it creates a collection node in place
  /// to contain all sequential suitable nodes from back.
  void collectNodesInPlace(SyntaxKind ColletionKind);

  /// On destruction, construct a specified kind of RawSyntax node consuming the
  /// collected parts, then append it to the parent context.
  void setCreateSyntax(SyntaxKind Kind) {
    Mode = AccumulationMode::CreateSyntax;
    SynKind = Kind;
  }

  /// On destruction, if the parts size is 1 and it's kind of \c Kind, just
  /// append it to the parent context. Otherwise, create Unknown{Kind} node from
  /// the collected parts.
  void setCoerceKind(SyntaxContextKind Kind) {
    Mode = AccumulationMode::CoerceKind;
    CtxtKind = Kind;
  }

  /// Move the collected parts to the tail of parent context.
  void setTransparent() { Mode = AccumulationMode::Transparent; }

  /// Discard collected parts on this context.
  void setDiscard() { Mode = AccumulationMode::Discard; }

  /// Explicitly finalizing syntax tree creation.
  /// This function will be called during the destroying of a root syntax
  /// parsing context. However, we can explicitly call this function to get
  /// the syntax tree before closing the root context.
  void finalizeRoot();

};

} // namespace swift
#endif // SWIFT_SYNTAX_PARSING_CONTEXT_H
