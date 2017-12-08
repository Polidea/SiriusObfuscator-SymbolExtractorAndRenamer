//===-- SwiftMangledNameVisitor.cpp -----------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "lldb/Symbol/SwiftMangledNameVisitor.h"

using namespace lldb_private;

void SwiftMangledNameVisitor::accept(swift::Demangle::NodePointer pointer) {
#define NODE(e)                                                                \
  case swift::Demangle::Node::Kind::e: {                                       \
    accept##e(pointer);                                                        \
    break;                                                                     \
  }
  const swift::Demangle::Node::Kind node_kind = pointer->getKind();
  switch (node_kind) {
#include "swift/Demangling/DemangleNodes.def"
  }
}

#define NODE(e)                                                                \
  void SwiftMangledNameVisitor::visit##e(                                      \
      swift::Demangle::NodePointer pointer) {}                                 \
  void SwiftMangledNameVisitor::accept##e(                                     \
      swift::Demangle::NodePointer cur_node) {                                 \
    swift::Demangle::Node::iterator end = cur_node->end();                     \
    for (swift::Demangle::Node::iterator pos = cur_node->begin(); pos != end;  \
         ++pos) {                                                              \
      accept(*pos);                                                            \
    };                                                                         \
    visit##e(cur_node);                                                        \
  }
#include "swift/Demangling/DemangleNodes.def"

void SwiftMangledNameVisitor::visit(const char *mangled_name) {
  if (mangled_name && mangled_name[0]) {
    swift::Demangle::Context demangle_ctx;
    accept(demangle_ctx.demangleSymbolAsNode(llvm::StringRef(mangled_name)));
  }
}
