//===--- ASTMangler.cpp - Swift AST symbol mangling -----------------------===//
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
//  This file implements declaration name mangling in Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTMangler.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/Demangling/ManglingUtils.h"
#include "swift/Demangling/Demangler.h"
#include "swift/Strings.h"
#include "clang/Basic/CharInfo.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"

using namespace swift;
using namespace swift::Mangle;

std::string ASTMangler::mangleClosureEntity(const AbstractClosureExpr *closure,
                                            SymbolKind SKind) {
  beginMangling();
  appendClosureEntity(closure);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleEntity(const ValueDecl *decl, bool isCurried,
                                     SymbolKind SKind) {
  beginMangling();
  appendEntity(decl);
  if (isCurried)
    appendOperator("Tc");
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleDestructorEntity(const DestructorDecl *decl,
                                               bool isDeallocating,
                                               SymbolKind SKind) {
  beginMangling();
  appendDestructorEntity(decl, isDeallocating);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleConstructorEntity(const ConstructorDecl *ctor,
                                                bool isAllocating,
                                                bool isCurried,
                                                SymbolKind SKind) {
  beginMangling();
  appendConstructorEntity(ctor, isAllocating);
  if (isCurried)
    appendOperator("Tc");
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleIVarInitDestroyEntity(const ClassDecl *decl,
                                                    bool isDestroyer,
                                                    SymbolKind SKind) {
  beginMangling();
  appendContext(decl);
  appendOperator(isDestroyer ? "fE" : "fe");
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleAccessorEntity(AccessorKind kind,
                                             AddressorKind addressorKind,
                                             const ValueDecl *decl,
                                             bool isStatic,
                                             SymbolKind SKind) {
  beginMangling();
  appendAccessorEntity(kind, addressorKind, decl, isStatic);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleGlobalGetterEntity(const ValueDecl *decl,
                                                 SymbolKind SKind) {
  beginMangling();
  appendEntity(decl, "fG", false);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleDefaultArgumentEntity(const DeclContext *func,
                                                    unsigned index,
                                                    SymbolKind SKind) {
  beginMangling();
  appendDefaultArgumentEntity(func, index);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleInitializerEntity(const VarDecl *var,
                                                SymbolKind SKind) {
  beginMangling();
  appendInitializerEntity(var);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleNominalType(const NominalTypeDecl *decl) {
  beginMangling();
  appendAnyGenericType(decl);
  return finalize();
}

std::string ASTMangler::mangleVTableThunk(const FuncDecl *Base,
                                          const FuncDecl *Derived) {
  beginMangling();

  appendEntity(Derived);
  appendEntity(Base);
  appendOperator("TV");

  return finalize();
}

std::string ASTMangler::mangleConstructorVTableThunk(
                                               const ConstructorDecl *Base,
                                               const ConstructorDecl *Derived,
                                               bool isAllocating) {
  beginMangling();

  appendConstructorEntity(Derived, isAllocating);
  appendConstructorEntity(Base, isAllocating);
  appendOperator("TV");

  return finalize();
}

std::string ASTMangler::mangleWitnessTable(const NormalProtocolConformance *C) {
  beginMangling();
  appendProtocolConformance(C);
  appendOperator("WP");
  return finalize();
}

std::string ASTMangler::mangleWitnessThunk(const ProtocolConformance *Conformance,
                                           const ValueDecl *Requirement) {
  beginMangling();
  // Concrete witness thunks get a special mangling.
  if (Conformance)
    appendProtocolConformance(Conformance);

  if (auto ctor = dyn_cast<ConstructorDecl>(Requirement)) {
    appendConstructorEntity(ctor, /*isAllocating=*/true);
  } else {
    assert(isa<FuncDecl>(Requirement) && "expected function");
    appendEntity(cast<FuncDecl>(Requirement));
  }

  if (Conformance)
    appendOperator("TW");
  return finalize();
}

std::string ASTMangler::mangleClosureWitnessThunk(
                                         const ProtocolConformance *Conformance,
                                         const AbstractClosureExpr *Closure) {
  beginMangling();
  appendProtocolConformance(Conformance);
  appendClosureEntity(Closure);
  appendOperator("TW");
  return finalize();
}

std::string ASTMangler::mangleBehaviorInitThunk(const VarDecl *decl) {
  auto topLevelContext = decl->getDeclContext()->getModuleScopeContext();
  auto fileUnit = cast<FileUnit>(topLevelContext);
  Identifier discriminator = fileUnit->getDiscriminatorForPrivateValue(decl);
  assert(!discriminator.empty());
  assert(!isNonAscii(discriminator.str()) &&
         "discriminator contains non-ASCII characters");
  assert(!clang::isDigit(discriminator.str().front()) &&
         "not a valid identifier");

  appendContextOf(decl);
  appendIdentifier(decl->getName().str());
  appendIdentifier(discriminator.str());
  appendOperator("TB");
  return finalize();
}

std::string ASTMangler::mangleGlobalVariableFull(const VarDecl *decl) {
  // As a special case, Clang functions and globals don't get mangled at all.
  // FIXME: When we can import C++, use Clang's mangler.
  if (auto clangDecl =
      dyn_cast_or_null<clang::DeclaratorDecl>(decl->getClangDecl())) {
    if (auto asmLabel = clangDecl->getAttr<clang::AsmLabelAttr>()) {
      Buffer << '\01' << asmLabel->getLabel();
    } else {
      Buffer << clangDecl->getName();
    }
    return finalize();
  }
  beginMangling();
  appendEntity(decl);
  return finalize();
}

std::string ASTMangler::mangleKeyPathGetterThunkHelper(
                                            const AbstractStorageDecl *property,
                                            GenericSignature *signature,
                                            CanType baseType,
                                            ArrayRef<CanType> subs) {
  beginMangling();
  appendEntity(property);
  if (signature)
    appendGenericSignature(signature);
  appendType(baseType);
  if (isa<SubscriptDecl>(property)) {
    // Subscripts can be generic, and different key paths could capture the same
    // subscript at different generic arguments.
    for (auto &sub : subs) {
      appendType(sub);
    }
  }
  appendOperator("TK");
  return finalize();
}

std::string ASTMangler::mangleKeyPathSetterThunkHelper(
                                          const AbstractStorageDecl *property,
                                          GenericSignature *signature,
                                          CanType baseType,
                                          ArrayRef<CanType> subs) {
  beginMangling();
  appendEntity(property);
  if (signature)
    appendGenericSignature(signature);
  appendType(baseType);
  if (isa<SubscriptDecl>(property)) {
    // Subscripts can be generic, and different key paths could capture the same
    // subscript at different generic arguments.
    for (auto &sub : subs) {
      appendType(sub);
    }
  }
  appendOperator("Tk");
  return finalize();
}

std::string ASTMangler::mangleKeyPathEqualsHelper(ArrayRef<CanType> indices,
                                                  GenericSignature *signature) {
  beginMangling();
  for (auto &index : indices)
    appendType(index);
  if (signature)
    appendGenericSignature(signature);
  appendOperator("TH");
  return finalize();
}

std::string ASTMangler::mangleKeyPathHashHelper(ArrayRef<CanType> indices,
                                                GenericSignature *signature) {
  beginMangling();
  for (auto &index : indices)
    appendType(index);
  if (signature)
    appendGenericSignature(signature);
  appendOperator("Th");
  return finalize();
}

std::string ASTMangler::mangleGlobalInit(const VarDecl *decl, int counter,
                                         bool isInitFunc) {
  auto topLevelContext = decl->getDeclContext()->getModuleScopeContext();
  auto fileUnit = cast<FileUnit>(topLevelContext);
  Identifier discriminator = fileUnit->getDiscriminatorForPrivateValue(decl);
  assert(!discriminator.empty());
  assert(!isNonAscii(discriminator.str()) &&
         "discriminator contains non-ASCII characters");
  assert(!clang::isDigit(discriminator.str().front()) &&
         "not a valid identifier");

  Buffer << "globalinit_";
  appendIdentifier(discriminator.str());
  Buffer << (isInitFunc ? "_func" : "_token");
  Buffer << counter;
  return finalize();
}

std::string ASTMangler::mangleReabstractionThunkHelper(
                                            CanSILFunctionType ThunkType,
                                            Type FromType,
                                            Type ToType,
                                            ModuleDecl *Module) {
  Mod = Module;
  GenericSignature *GenSig = ThunkType->getGenericSignature();
  if (GenSig)
    CurGenericSignature = GenSig->getCanonicalSignature();

  beginMangling();
  appendType(FromType);
  appendType(ToType);
  if (GenSig)
    appendGenericSignature(GenSig);
  // TODO: mangle ThunkType->isPseudogeneric()
  appendOperator("TR");
  return finalize();
}

std::string ASTMangler::mangleTypeForDebugger(Type Ty, const DeclContext *DC,
                                              GenericEnvironment *GE) {
  GenericEnv = GE;
  DWARFMangling = true;
  beginMangling();
  
  if (DC)
    bindGenericParameters(DC);
  DeclCtx = DC;

  appendType(Ty);
  appendOperator("D");
  return finalize();
}

std::string ASTMangler::mangleDeclType(const ValueDecl *decl) {
  DWARFMangling = true;
  beginMangling();
  
  appendDeclType(decl);
  appendOperator("D");
  return finalize();
}

#ifdef USE_NEW_MANGLING_FOR_OBJC_RUNTIME_NAMES
static bool isPrivate(const NominalTypeDecl *Nominal) {
  return Nominal->hasAccessibility() &&
         Nominal->getFormalAccess() <= Accessibility::FilePrivate;
}
#endif

std::string ASTMangler::mangleObjCRuntimeName(const NominalTypeDecl *Nominal) {
#ifdef USE_NEW_MANGLING_FOR_OBJC_RUNTIME_NAMES
  // Using the new mangling for ObjC runtime names (except for top-level
  // classes). This is currently disabled to support old archives.
  // TODO: re-enable this as we switch to the new mangling for ObjC names.
  DeclContext *Ctx = Nominal->getDeclContext();

  if (Ctx->isModuleScopeContext() && !isPrivate(Nominal)) {
    // Use the old mangling for non-private top-level classes and protocols.
    // This is what the ObjC runtime needs to demangle.
    // TODO: Use new mangling scheme as soon as the ObjC runtime
    // can demangle it.
    //
    // Don't use word-substitutions and punycode encoding.
    MaxNumWords = 0;
    UsePunycode = false;
    UseSubstitutions = false;
    Buffer << "_Tt";
    bool isProto = false;
    if (isa<ClassDecl>(Nominal)) {
      Buffer << 'C';
    } else {
      isProto = true;
      assert(isa<ProtocolDecl>(Nominal));
      Buffer << 'P';
    }
    appendModule(Ctx->getParentModule());
    appendIdentifier(Nominal->getName().str());
    if (isProto)
      Buffer << '_';
    return finalize();
  }
  // For all other cases, we can use the new mangling.
  beginMangling();
  appendAnyGenericType(Nominal);
  return finalize();
#else
  // Use the old mangling for ObjC runtime names.
  beginMangling();
  appendAnyGenericType(Nominal);
  std::string NewName = finalize();
  Demangle::Demangler Dem;
  Demangle::Node *Root = Dem.demangleSymbol(NewName);
  assert(Root->getKind() == Node::Kind::Global);
  Node *NomTy = Root->getFirstChild();
  if (NomTy->getKind() == Node::Kind::Protocol) {
    // Protocols are actually mangled as protocol lists.
    Node *PTy = Dem.createNode(Node::Kind::Type);
    PTy->addChild(NomTy, Dem);
    Node *TList = Dem.createNode(Node::Kind::TypeList);
    TList->addChild(PTy, Dem);
    NomTy = Dem.createNode(Node::Kind::ProtocolList);
    NomTy->addChild(TList, Dem);
  }
  // Add a TypeMangling node at the top
  Node *Ty = Dem.createNode(Node::Kind::Type);
  Ty->addChild(NomTy, Dem);
  Node *TyMangling = Dem.createNode(Node::Kind::TypeMangling);
  TyMangling->addChild(Ty, Dem);
  Node *NewGlobal = Dem.createNode(Node::Kind::Global);
  NewGlobal->addChild(TyMangling, Dem);
  std::string OldName = mangleNodeOld(NewGlobal);
  return OldName;
#endif
}

std::string ASTMangler::mangleTypeAsContextUSR(const NominalTypeDecl *type) {
  beginManglingWithoutPrefix();
  llvm::SaveAndRestore<bool> allowUnnamedRAII(AllowNamelessEntities, true);
  appendContext(type);
  return finalize();
}

std::string ASTMangler::mangleDeclAsUSR(const ValueDecl *Decl,
                                        StringRef USRPrefix) {
  beginManglingWithoutPrefix();
  llvm::SaveAndRestore<bool> allowUnnamedRAII(AllowNamelessEntities, true);
  Buffer << USRPrefix;
  bindGenericParameters(Decl->getDeclContext());

  if (auto Ctor = dyn_cast<ConstructorDecl>(Decl)) {
    appendConstructorEntity(Ctor, /*isAllocating=*/false);
  } else if (auto Dtor = dyn_cast<DestructorDecl>(Decl)) {
    appendDestructorEntity(Dtor, /*isDeallocating=*/false);
  } else if (auto GTD = dyn_cast<GenericTypeDecl>(Decl)) {
    appendAnyGenericType(GTD);
  } else if (isa<AssociatedTypeDecl>(Decl)) {
    appendContextOf(Decl);
    appendDeclName(Decl);
  } else {
    appendEntity(Decl);
  }

  // We have a custom prefix, so finalize() won't verify for us. Do it manually.
  verify(Storage.str().drop_front(USRPrefix.size()));
  return finalize();
}

std::string ASTMangler::mangleAccessorEntityAsUSR(AccessorKind kind,
                                                  AddressorKind addressorKind,
                                                  const ValueDecl *decl,
                                                  StringRef USRPrefix) {
  beginManglingWithoutPrefix();
  llvm::SaveAndRestore<bool> allowUnnamedRAII(AllowNamelessEntities, true);
  Buffer << USRPrefix;
  appendAccessorEntity(kind, addressorKind, decl, /*isStatic*/ false);
  // We have a custom prefix, so finalize() won't verify for us. Do it manually.
  verify(Storage.str().drop_front(USRPrefix.size()));
  return finalize();
}


void ASTMangler::appendSymbolKind(SymbolKind SKind) {
  switch (SKind) {
    case SymbolKind::Default: return;
    case SymbolKind::DynamicThunk: return appendOperator("TD");
    case SymbolKind::SwiftAsObjCThunk: return appendOperator("To");
    case SymbolKind::ObjCAsSwiftThunk: return appendOperator("TO");
    case SymbolKind::DirectMethodReferenceThunk: return appendOperator("Td");
  }
}

/// Returns true if one of the ancestor DeclContexts of \p D is either marked
/// private or is a local context.
static bool isInPrivateOrLocalContext(const ValueDecl *D) {
  const DeclContext *DC = D->getDeclContext();
  if (!DC->isTypeContext()) {
    assert((DC->isModuleScopeContext() || DC->isLocalContext()) &&
           "unexpected context kind");
    return DC->isLocalContext();
  }

  auto declaredType = DC->getDeclaredTypeOfContext();
  if (!declaredType || declaredType->hasError())
    return false;

  auto *nominal = declaredType->getAnyNominal();
  if (nominal->getFormalAccess() <= Accessibility::FilePrivate)
    return true;
  return isInPrivateOrLocalContext(nominal);
}

static bool getUnnamedParamIndex(const ParameterList *ParamList,
                                 const ParamDecl *D,
                                 unsigned &UnnamedIndex) {
  for (auto Param : *ParamList) {
    if (!Param->hasName()) {
      if (Param == D)
        return true;
      ++UnnamedIndex;
    }
  }
  return false;
}

static unsigned getUnnamedParamIndex(const ParamDecl *D) {
  if (auto SD = dyn_cast<SubscriptDecl>(D->getDeclContext())) {
    unsigned UnnamedIndex = 0;
    auto *ParamList = SD->getIndices();
    if (getUnnamedParamIndex(ParamList, D, UnnamedIndex))
      return UnnamedIndex;
    llvm_unreachable("param not found");
  }

  ArrayRef<ParameterList *> ParamLists;

  if (auto AFD = dyn_cast<AbstractFunctionDecl>(D->getDeclContext())) {
    ParamLists = AFD->getParameterLists();
  } else {
    auto ACE = cast<AbstractClosureExpr>(D->getDeclContext());
    ParamLists = ACE->getParameterLists();
  }

  unsigned UnnamedIndex = 0;
  for (auto ParamList : ParamLists) {
    if (getUnnamedParamIndex(ParamList, D, UnnamedIndex))
      return UnnamedIndex;
  }

  llvm_unreachable("param not found");
}

static StringRef getPrivateDiscriminatorIfNecessary(const ValueDecl *decl) {
  if (!decl->hasAccessibility() ||
      decl->getFormalAccess() > Accessibility::FilePrivate ||
      isInPrivateOrLocalContext(decl)) {
    return StringRef();
  }

  // Mangle non-local private declarations with a textual discriminator
  // based on their enclosing file.
  auto topLevelContext = decl->getDeclContext()->getModuleScopeContext();
  auto fileUnit = cast<FileUnit>(topLevelContext);

  Identifier discriminator =
      fileUnit->getDiscriminatorForPrivateValue(decl);
  assert(!discriminator.empty());
  assert(!isNonAscii(discriminator.str()) &&
         "discriminator contains non-ASCII characters");
  (void)&isNonAscii;
  assert(!clang::isDigit(discriminator.str().front()) &&
         "not a valid identifier");
  return discriminator.str();
}

void ASTMangler::appendDeclName(const ValueDecl *decl) {
  if (decl->isOperator()) {
    auto name = decl->getBaseName().getIdentifier().str();
    appendIdentifier(translateOperator(name));
    switch (decl->getAttrs().getUnaryOperatorKind()) {
      case UnaryOperatorKind::Prefix:
        appendOperator("op");
        break;
      case UnaryOperatorKind::Postfix:
        appendOperator("oP");
        break;
      case UnaryOperatorKind::None:
        appendOperator("oi");
        break;
    }
  } else if (decl->hasName()) {
    // TODO: Handle special names
    appendIdentifier(decl->getBaseName().getIdentifier().str());
  } else {
    assert(AllowNamelessEntities && "attempt to mangle unnamed decl");
    // Fall back to an unlikely name, so that we still generate a valid
    // mangled name.
    appendIdentifier("_");
  }

  if (decl->getDeclContext()->isLocalContext()) {
    if (auto *paramDecl = dyn_cast<ParamDecl>(decl)) {
      if (!decl->hasName()) {
        // Mangle unnamed params with their ordering.
        return appendOperator("L", Index(getUnnamedParamIndex(paramDecl)));
      }
    }
    // Mangle local declarations with a numeric discriminator.
    return appendOperator("L", Index(decl->getLocalDiscriminator()));
  }

  StringRef privateDiscriminator = getPrivateDiscriminatorIfNecessary(decl);
  if (!privateDiscriminator.empty()) {
    appendIdentifier(privateDiscriminator.str());
    return appendOperator("LL");
  }
}

static const char *getMetatypeRepresentationOp(MetatypeRepresentation Rep) {
  switch (Rep) {
    case MetatypeRepresentation::Thin:
      return "t";
    case MetatypeRepresentation::Thick:
      return "T";
    case MetatypeRepresentation::ObjC:
      return "o";
  }

  llvm_unreachable("Unhandled MetatypeRepresentation in switch.");
}

static bool isStdlibType(const TypeDecl *decl) {
  DeclContext *dc = decl->getDeclContext();
  return dc->isModuleScopeContext() && dc->getParentModule()->isStdlibModule();
}

/// Mangle a type into the buffer.
///
void ASTMangler::appendType(Type type) {
  assert((DWARFMangling || type->isCanonical()) &&
         "expecting canonical types when not mangling for the debugger");
  TypeBase *tybase = type.getPointer();
  switch (type->getKind()) {
    case TypeKind::TypeVariable:
      llvm_unreachable("mangling type variable");

    case TypeKind::Module:
      llvm_unreachable("Cannot mangle module type yet");

    case TypeKind::Error:
    case TypeKind::Unresolved:
      appendOperator("Xe");
      return;

      // We don't care about these types being a bit verbose because we
      // don't expect them to come up that often in API names.
    case TypeKind::BuiltinFloat:
      switch (cast<BuiltinFloatType>(tybase)->getFPKind()) {
        case BuiltinFloatType::IEEE16: appendOperator("Bf16_"); return;
        case BuiltinFloatType::IEEE32: appendOperator("Bf32_"); return;
        case BuiltinFloatType::IEEE64: appendOperator("Bf64_"); return;
        case BuiltinFloatType::IEEE80: appendOperator("Bf80_"); return;
        case BuiltinFloatType::IEEE128: appendOperator("Bf128_"); return;
        case BuiltinFloatType::PPC128: llvm_unreachable("ppc128 not supported");
      }
      llvm_unreachable("bad floating-point kind");
    case TypeKind::BuiltinInteger: {
      auto width = cast<BuiltinIntegerType>(tybase)->getWidth();
      if (width.isFixedWidth())
        appendOperator("Bi", Index(width.getFixedWidth() + 1));
      else if (width.isPointerWidth())
        appendOperator("Bw");
      else
        llvm_unreachable("impossible width value");
      return;
    }
    case TypeKind::BuiltinRawPointer:
      return appendOperator("Bp");
    case TypeKind::BuiltinNativeObject:
      return appendOperator("Bo");
    case TypeKind::BuiltinBridgeObject:
      return appendOperator("Bb");
    case TypeKind::BuiltinUnknownObject:
      return appendOperator("BO");
    case TypeKind::BuiltinUnsafeValueBuffer:
      return appendOperator("BB");
    case TypeKind::BuiltinVector:
      appendType(cast<BuiltinVectorType>(tybase)->getElementType());
      return appendOperator("Bv",
                            cast<BuiltinVectorType>(tybase)->getNumElements());
    case TypeKind::NameAlias: {
      assert(DWARFMangling && "sugared types are only legal for the debugger");
      auto NameAliasTy = cast<NameAliasType>(tybase);
      TypeAliasDecl *decl = NameAliasTy->getDecl();
      if (decl->getModuleContext() == decl->getASTContext().TheBuiltinModule) {
        // It's not possible to mangle the context of the builtin module.
        return appendType(NameAliasTy->getSinglyDesugaredType());
      }

      // For the DWARF output we want to mangle the type alias + context,
      // unless the type alias references a builtin type.
      return appendAnyGenericType(decl);
    }

    case TypeKind::Paren:
      return appendSugaredType<ParenType>(type);
    case TypeKind::ArraySlice: /* fallthrough */
    case TypeKind::Optional:
      return appendSugaredType<SyntaxSugarType>(type);
    case TypeKind::Dictionary:
      return appendSugaredType<DictionaryType>(type);

    case TypeKind::ImplicitlyUnwrappedOptional: {
      assert(DWARFMangling && "sugared types are only legal for the debugger");
      auto *IUO = cast<ImplicitlyUnwrappedOptionalType>(tybase);
      auto implDecl = tybase->getASTContext().getImplicitlyUnwrappedOptionalDecl();
      auto GenTy = BoundGenericType::get(implDecl, Type(), IUO->getBaseType());
      return appendType(GenTy);
    }

    case TypeKind::ExistentialMetatype: {
      ExistentialMetatypeType *EMT = cast<ExistentialMetatypeType>(tybase);
      appendType(EMT->getInstanceType());
      if (EMT->hasRepresentation()) {
        appendOperator("Xm",
                       getMetatypeRepresentationOp(EMT->getRepresentation()));
      } else {
        appendOperator("Xp");
      }
      return;
    }
    case TypeKind::Metatype: {
      MetatypeType *MT = cast<MetatypeType>(tybase);
      appendType(MT->getInstanceType());
      if (MT->hasRepresentation()) {
        appendOperator("XM",
                       getMetatypeRepresentationOp(MT->getRepresentation()));
      } else {
        appendOperator("m");
      }
      return;
    }
    case TypeKind::LValue:
      llvm_unreachable("@lvalue types should not occur in function interfaces");

    case TypeKind::InOut:
      appendType(cast<InOutType>(tybase)->getObjectType());
      return appendOperator("z");

    case TypeKind::UnmanagedStorage:
      appendType(cast<UnmanagedStorageType>(tybase)->getReferentType());
      return appendOperator("Xu");

    case TypeKind::UnownedStorage:
      appendType(cast<UnownedStorageType>(tybase)->getReferentType());
      return appendOperator("Xo");

    case TypeKind::WeakStorage:
      appendType(cast<WeakStorageType>(tybase)->getReferentType());
      return appendOperator("Xw");

    case TypeKind::Tuple:
      appendTypeList(type);
      return appendOperator("t");

    case TypeKind::Protocol: {
      bool First = true;
      appendProtocolName(cast<ProtocolType>(tybase)->getDecl());
      appendListSeparator(First);
      return appendOperator("p");
    }

    case TypeKind::ProtocolComposition: {
      // We mangle ProtocolType and ProtocolCompositionType using the
      // same production:
      bool First = true;
      auto layout = type->getExistentialLayout();
      for (Type protoTy : layout.getProtocols()) {
        appendProtocolName(protoTy->castTo<ProtocolType>()->getDecl());
        appendListSeparator(First);
      }
      if (First)
        appendOperator("y");

      if (layout.superclass) {
        appendType(layout.superclass);
        return appendOperator("Xc");
      } else if (layout.hasExplicitAnyObject) {
        return appendOperator("Xl");
      }
      return appendOperator("p");
    }

    case TypeKind::UnboundGeneric:
    case TypeKind::Class:
    case TypeKind::Enum:
    case TypeKind::Struct:
    case TypeKind::BoundGenericClass:
    case TypeKind::BoundGenericEnum:
    case TypeKind::BoundGenericStruct:
      if (type->isSpecialized()) {
        // Try to mangle the entire name as a substitution.
        if (tryMangleSubstitution(type.getPointer()))
          return;

        NominalTypeDecl *NDecl = type->getAnyNominal();
        if (isStdlibType(NDecl) && NDecl->getName().str() == "Optional") {
          auto GenArgs = type->castTo<BoundGenericType>()->getGenericArgs();
          assert(GenArgs.size() == 1);
          appendType(GenArgs[0]);
          appendOperator("Sg");
        } else {
          appendAnyGenericType(NDecl);
          bool isFirstArgList = true;
          appendBoundGenericArgs(type, isFirstArgList);
          appendOperator("G");
        }
        addSubstitution(type.getPointer());
        return;
      }
      appendAnyGenericType(tybase->getAnyNominal());
      return;

    case TypeKind::SILFunction:
      return appendImplFunctionType(cast<SILFunctionType>(tybase));

      // type ::= archetype
    case TypeKind::Archetype: {
      auto *archetype = cast<ArchetypeType>(tybase);

      assert(DWARFMangling && "Cannot mangle free-standing archetypes");

      // Mangle the associated type of a parent archetype.
      if (auto parent = archetype->getParent()) {
        assert(archetype->getAssocType()
               && "child archetype has no associated type?!");

        if (tryMangleSubstitution(archetype))
          return;
        appendType(parent);
        appendIdentifier(archetype->getName().str());
        appendOperator("Qa");
        addSubstitution(archetype);
        return;
      }

      // archetype ::= 'Q' <index>             # archetype with depth=0, index=N
      // archetype ::= 'Qd' <index> <index>    # archetype with depth=M+1, index=N
      // Mangle generic parameter archetypes.

      // Find the archetype information.
      const DeclContext *DC = DeclCtx;
      auto GTPT = GenericEnvironment::mapTypeOutOfContext(GenericEnv, archetype)
                      ->castTo<GenericTypeParamType>();

      // The DWARF output created by Swift is intentionally flat,
      // therefore archetypes are emitted with their DeclContext if
      // they appear at the top level of a type.
      DWARFMangling = false;
      while (DC && DC->isGenericContext()) {
        if (DC->isInnermostContextGeneric() &&
            DC->getGenericParamsOfContext()->getDepth() == GTPT->getDepth())
          break;
        DC = DC->getParent();
      }
      assert(DC && "no decl context for archetype found");
      if (!DC) return;
      appendContext(DC);
      DWARFMangling = true;
      return appendOperator("Qq", Index(GTPT->getIndex()));
    }

    case TypeKind::DynamicSelf: {
      auto dynamicSelf = cast<DynamicSelfType>(tybase);
      if (dynamicSelf->getSelfType()->getAnyNominal()) {
        appendType(dynamicSelf->getSelfType());
        return appendOperator("XD");
      }
      return appendType(dynamicSelf->getSelfType());
    }

    case TypeKind::GenericFunction: {
      auto genFunc = cast<GenericFunctionType>(tybase);
      appendFunctionType(genFunc, /*forceSingleParam*/ false);
      appendGenericSignature(genFunc->getGenericSignature());
      appendOperator("u");
      return;
    }

    case TypeKind::GenericTypeParam: {
      auto paramTy = cast<GenericTypeParamType>(tybase);
      // A special mangling for the very first generic parameter. This shows up
      // frequently because it corresponds to 'Self' in protocol requirement
      // generic signatures.
      if (paramTy->getDepth() == 0 && paramTy->getIndex() == 0)
        return appendOperator("x");

      return appendOpWithGenericParamIndex("q", paramTy);
    }

    case TypeKind::DependentMember: {
      auto *DepTy = cast<DependentMemberType>(tybase);
      if (tryMangleSubstitution(DepTy))
        return;

      bool isAssocTypeAtDepth = false;
      if (GenericTypeParamType *gpBase = appendAssocType(DepTy,
                                                         isAssocTypeAtDepth)) {
        if (gpBase->getDepth() == 0 && gpBase->getIndex() == 0) {
          appendOperator(isAssocTypeAtDepth ? "QZ" : "Qz");
        } else {
          appendOpWithGenericParamIndex(isAssocTypeAtDepth ? "QY" : "Qy",
                                        gpBase);
        }
      } else {
        // Dependent members of non-generic-param types are not canonical, but
        // we may still want to mangle them for debugging or indexing purposes.
        appendType(DepTy->getBase());
        appendAssociatedTypeName(DepTy);
        appendOperator("qa");
      }
      addSubstitution(DepTy);
      return;
    }
      
    case TypeKind::Function:
      appendFunctionType(cast<FunctionType>(tybase), /*forceSingleParam*/ false);
      return;
      
    case TypeKind::SILBox: {
      auto box = cast<SILBoxType>(tybase);
      auto layout = box->getLayout();
      SmallVector<TupleTypeElt, 4> fieldsList;
      for (auto &field : layout->getFields()) {
        auto fieldTy = field.getLoweredType();
        // Use the `inout` mangling to represent a mutable field.
        if (field.isMutable())
          fieldTy = CanInOutType::get(fieldTy);
        fieldsList.push_back(TupleTypeElt(fieldTy));
      }
      appendTypeList(TupleType::get(fieldsList, tybase->getASTContext())
                       ->getCanonicalType());

      if (auto sig = layout->getGenericSignature()) {
        fieldsList.clear();
        for (auto &arg : box->getGenericArgs()) {
          fieldsList.push_back(TupleTypeElt(arg.getReplacement()));
        }
        appendTypeList(TupleType::get(fieldsList, tybase->getASTContext())
                         ->getCanonicalType());
        appendGenericSignature(sig);
        appendOperator("XX");
      } else {
        appendOperator("Xx");
      }

      return;
    }

    case TypeKind::SILBlockStorage:
      llvm_unreachable("should never be mangled");
  }
  llvm_unreachable("bad type kind");
}

GenericTypeParamType *ASTMangler::appendAssocType(DependentMemberType *DepTy,
                                                  bool &isAssocTypeAtDepth) {
  auto base = DepTy->getBase()->getCanonicalType();
  // 't_0_0.Member'
  if (auto gpBase = dyn_cast<GenericTypeParamType>(base)) {
    appendAssociatedTypeName(DepTy);
    isAssocTypeAtDepth = false;
    return gpBase;
  }

  // 't_0_0.Member.Member...'
  SmallVector<DependentMemberType*, 2> path;
  path.push_back(DepTy);
  while (auto dmBase = dyn_cast<DependentMemberType>(base)) {
    path.push_back(dmBase);
    base = dmBase.getBase();
  }
  if (auto gpRoot = dyn_cast<GenericTypeParamType>(base)) {
    bool first = true;
    for (auto *member : reversed(path)) {
      appendAssociatedTypeName(member);
      appendListSeparator(first);
    }
    isAssocTypeAtDepth = true;
    return gpRoot;
  }
  return nullptr;
}

void ASTMangler::appendOpWithGenericParamIndex(StringRef Op,
                                          const GenericTypeParamType *paramTy) {
  llvm::SmallVector<char, 8> OpBuf(Op.begin(), Op.end());
  if (paramTy->getDepth() > 0) {
    OpBuf.push_back('d');
    return appendOperator(StringRef(OpBuf.data(), OpBuf.size()),
                          Index(paramTy->getDepth() - 1),
                          Index(paramTy->getIndex()));
  }
  if (paramTy->getIndex() == 0) {
    OpBuf.push_back('z');
    return appendOperator(StringRef(OpBuf.data(), OpBuf.size()));
  }
  appendOperator(Op, Index(paramTy->getIndex() - 1));
}


/// Bind the generic parameters from the given signature.
void ASTMangler::bindGenericParameters(CanGenericSignature sig) {
  if (sig)
    CurGenericSignature = sig;
}

/// Bind the generic parameters from the given context and its parents.
void ASTMangler::bindGenericParameters(const DeclContext *DC) {
  if (auto sig = DC->getGenericSignatureOfContext())
    bindGenericParameters(sig->getCanonicalSignature());
}

void ASTMangler::appendBoundGenericArgs(Type type, bool &isFirstArgList) {
  BoundGenericType *boundType = nullptr;
  if (auto *unboundType = type->getAs<UnboundGenericType>()) {
    if (Type parent = unboundType->getParent())
      appendBoundGenericArgs(parent, isFirstArgList);
  } else if (auto *nominalType = type->getAs<NominalType>()) {
    if (Type parent = nominalType->getParent())
      appendBoundGenericArgs(parent, isFirstArgList);
  } else {
    boundType = type->castTo<BoundGenericType>();
    if (Type parent = boundType->getParent())
      appendBoundGenericArgs(parent, isFirstArgList);
  }
  if (isFirstArgList) {
    appendOperator("y");
    isFirstArgList = false;
  } else {
    appendOperator("_");
  }
  if (boundType) {
    for (Type arg : boundType->getGenericArgs()) {
      appendType(arg);
    }
  }
}

static char getParamConvention(ParameterConvention conv) {
  // @in and @out are mangled the same because they're put in
  // different places.
  switch (conv) {
    case ParameterConvention::Indirect_In: return 'i';
    case ParameterConvention::Indirect_In_Constant:
      return 'c';
    case ParameterConvention::Indirect_Inout: return 'l';
    case ParameterConvention::Indirect_InoutAliasable: return 'b';
    case ParameterConvention::Indirect_In_Guaranteed: return 'n';
    case ParameterConvention::Direct_Owned: return 'x';
    case ParameterConvention::Direct_Unowned: return 'y';
    case ParameterConvention::Direct_Guaranteed: return 'g';
  }
  llvm_unreachable("bad parameter convention");
};

static char getResultConvention(ResultConvention conv) {
  switch (conv) {
    case ResultConvention::Indirect: return 'r';
    case ResultConvention::Owned: return 'o';
    case ResultConvention::Unowned: return 'd';
    case ResultConvention::UnownedInnerPointer: return 'u';
    case ResultConvention::Autoreleased: return 'a';
  }
  llvm_unreachable("bad result convention");
};

void ASTMangler::appendImplFunctionType(SILFunctionType *fn) {

  llvm::SmallVector<char, 32> OpArgs;

  if (fn->isPolymorphic() && fn->isPseudogeneric())
    OpArgs.push_back('P');

  // <impl-callee-convention>
  if (fn->getExtInfo().hasContext()) {
    OpArgs.push_back(getParamConvention(fn->getCalleeConvention()));
  } else {
    OpArgs.push_back('t');
  }

  switch (fn->getRepresentation()) {
    case SILFunctionTypeRepresentation::Thick:
    case SILFunctionTypeRepresentation::Thin:
      break;
    case SILFunctionTypeRepresentation::Block:
      OpArgs.push_back('B');
      break;
    case SILFunctionTypeRepresentation::CFunctionPointer:
      OpArgs.push_back('C');
      break;
    case SILFunctionTypeRepresentation::ObjCMethod:
      OpArgs.push_back('O');
      break;
    case SILFunctionTypeRepresentation::Method:
      OpArgs.push_back('M');
      break;
    case SILFunctionTypeRepresentation::Closure:
      OpArgs.push_back('K');
      break;
    case SILFunctionTypeRepresentation::WitnessMethod:
      OpArgs.push_back('W');
      break;
  }

  // Mangle the parameters.
  for (auto param : fn->getParameters()) {
    OpArgs.push_back(getParamConvention(param.getConvention()));
    appendType(param.getType());
  }

  // Mangle the results.
  for (auto result : fn->getResults()) {
    OpArgs.push_back(getResultConvention(result.getConvention()));
    appendType(result.getType());
  }

  // Mangle the error result if present.
  if (fn->hasErrorResult()) {
    auto error = fn->getErrorResult();
    OpArgs.push_back('z');
    OpArgs.push_back(getResultConvention(error.getConvention()));
    appendType(error.getType());
  }
  if (fn->isPolymorphic())
    appendGenericSignature(fn->getGenericSignature());

  OpArgs.push_back('_');

  appendOperator("I", StringRef(OpArgs.data(), OpArgs.size()));
}

/// Mangle the context of the given declaration as a <context.
/// This is the top-level entrypoint for mangling <context>.
void ASTMangler::appendContextOf(const ValueDecl *decl) {
  auto clangDecl = decl->getClangDecl();

  // Classes and protocols implemented in Objective-C have a special context
  // mangling.
  //   known-context ::= 'So'
  if (isa<ClassDecl>(decl) && clangDecl) {
    assert(isa<clang::ObjCInterfaceDecl>(clangDecl) ||
           isa<clang::TypedefDecl>(clangDecl));
    return appendOperator("So");
  }

  if (isa<ProtocolDecl>(decl) && clangDecl) {
    assert(isa<clang::ObjCProtocolDecl>(clangDecl));
    return appendOperator("So");
  }

  // Declarations provided by a C module have a special context mangling.
  //   known-context ::= 'SC'
  // Do a dance to avoid a layering dependency.
  if (auto file = dyn_cast<FileUnit>(decl->getDeclContext())) {
    if (file->getKind() == FileUnitKind::ClangModule)
      return appendOperator("SC");
  }

  // Just mangle the decl's DC.
  appendContext(decl->getDeclContext());
}

namespace {
  class FindFirstVariable :
    public PatternVisitor<FindFirstVariable, VarDecl *> {
  public:
    VarDecl *visitNamedPattern(NamedPattern *P) {
      return P->getDecl();
    }

    VarDecl *visitTuplePattern(TuplePattern *P) {
      for (auto &elt : P->getElements()) {
        VarDecl *var = visit(elt.getPattern());
        if (var) return var;
      }
      return nullptr;
    }

    VarDecl *visitParenPattern(ParenPattern *P) {
      return visit(P->getSubPattern());
    }
    VarDecl *visitVarPattern(VarPattern *P) {
      return visit(P->getSubPattern());
    }
    VarDecl *visitTypedPattern(TypedPattern *P) {
      return visit(P->getSubPattern());
    }
    VarDecl *visitAnyPattern(AnyPattern *P) {
      return nullptr;
    }

    // Refutable patterns shouldn't ever come up.
#define REFUTABLE_PATTERN(ID, BASE)                                        \
    VarDecl *visit##ID##Pattern(ID##Pattern *P) {                          \
      llvm_unreachable("shouldn't be visiting a refutable pattern here!"); \
    }
#define PATTERN(ID, BASE)
#include "swift/AST/PatternNodes.def"
  };
} // end anonymous namespace

/// Find the first identifier bound by the given binding.  This
/// assumes that field and global-variable bindings always bind at
/// least one name, which is probably a reasonable assumption but may
/// not be adequately enforced.
static Optional<VarDecl*> findFirstVariable(PatternBindingDecl *binding) {
  for (auto entry : binding->getPatternList()) {
    auto var = FindFirstVariable().visit(entry.getPattern());
    if (var) return var;
  }
  // Pattern-binding bound without variables exists in erroneous code, e.g.
  // during code completion.
  return None;
}

void ASTMangler::appendContext(const DeclContext *ctx) {
  switch (ctx->getContextKind()) {
  case DeclContextKind::Module:
    return appendModule(cast<ModuleDecl>(ctx));

  case DeclContextKind::FileUnit:
    assert(!isa<BuiltinUnit>(ctx) && "mangling member of builtin module!");
    appendContext(ctx->getParent());
    return;

  case DeclContextKind::SerializedLocal: {
    auto local = cast<SerializedLocalDeclContext>(ctx);
    switch (local->getLocalDeclContextKind()) {
    case LocalDeclContextKind::AbstractClosure:
      appendClosureEntity(cast<SerializedAbstractClosureExpr>(local));
      return;
    case LocalDeclContextKind::DefaultArgumentInitializer: {
      auto argInit = cast<SerializedDefaultArgumentInitializer>(local);
      appendDefaultArgumentEntity(ctx->getParent(), argInit->getIndex());
      return;
    }
    case LocalDeclContextKind::PatternBindingInitializer: {
      auto patternInit = cast<SerializedPatternBindingInitializer>(local);
      if (auto var = findFirstVariable(patternInit->getBinding())) {
        appendInitializerEntity(var.getValue());
      } else {
        // This is incorrect in that it does not produce a /unique/ mangling,
        // but it will at least produce a /valid/ mangling.
        appendContext(ctx->getParent());
      }
      return;
    }
    case LocalDeclContextKind::TopLevelCodeDecl:
      return appendContext(local->getParent());
    }
  }

  case DeclContextKind::GenericTypeDecl:
    appendAnyGenericType(cast<GenericTypeDecl>(ctx));
    return;

  case DeclContextKind::ExtensionDecl: {
    auto ExtD = cast<ExtensionDecl>(ctx);
    auto ExtTy = ExtD->getExtendedType();
    // Recover from erroneous extension.
    if (ExtTy.isNull() || ExtTy->hasError())
      return appendContext(ExtD->getDeclContext());

    auto decl = ExtTy->getAnyNominal();
    assert(decl && "extension of non-nominal type?");
    // Mangle the module name if:
    // - the extension is defined in a different module from the actual nominal
    //   type decl,
    // - the extension is constrained, or
    // - the extension is to a protocol.
    // FIXME: In a world where protocol extensions are dynamically dispatched,
    // "extension is to a protocol" would no longer be a reason to use the
    // extension mangling, because an extension method implementation could be
    // resiliently moved into the original protocol itself.
    if (ExtD->getParentModule() != decl->getParentModule()
        || ExtD->isConstrainedExtension()
        || ExtD->getDeclaredInterfaceType()->isExistentialType()) {
      auto sig = ExtD->getGenericSignature();
      // If the extension is constrained, mangle the generic signature that
      // constrains it.
      appendAnyGenericType(decl);
      appendModule(ExtD->getParentModule());
      if (sig && ExtD->isConstrainedExtension()) {
        Mod = ExtD->getModuleContext();
        appendGenericSignature(sig);
      }
      return appendOperator("E");
    }
    return appendAnyGenericType(decl);
  }

  case DeclContextKind::AbstractClosureExpr:
    return appendClosureEntity(cast<AbstractClosureExpr>(ctx));

  case DeclContextKind::AbstractFunctionDecl: {
    auto fn = cast<AbstractFunctionDecl>(ctx);

    // Constructors and destructors as contexts are always mangled
    // using the non-(de)allocating variants.
    if (auto ctor = dyn_cast<ConstructorDecl>(fn)) {
      return appendConstructorEntity(ctor, /*allocating*/ false);
    }
    
    if (auto dtor = dyn_cast<DestructorDecl>(fn))
      return appendDestructorEntity(dtor, /*deallocating*/ false);
    
    return appendEntity(fn);
  }

  case DeclContextKind::SubscriptDecl:
    // FIXME: We may need to do something here if subscripts contain any symbols
    // exposed with linkage names, or if/when they get generic parameters.
    return appendContext(ctx->getParent());
      
  case DeclContextKind::Initializer:
    switch (cast<Initializer>(ctx)->getInitializerKind()) {
    case InitializerKind::DefaultArgument: {
      auto argInit = cast<DefaultArgumentInitializer>(ctx);
      return appendDefaultArgumentEntity(ctx->getParent(), argInit->getIndex());
    }

    case InitializerKind::PatternBinding: {
      auto patternInit = cast<PatternBindingInitializer>(ctx);
      if (auto var = findFirstVariable(patternInit->getBinding())) {
        appendInitializerEntity(var.getValue());
      } else {
        // This is incorrect in that it does not produce a /unique/ mangling,
        // but it will at least produce a /valid/ mangling.
        appendContext(ctx->getParent());
      }
      return;
    }
    }
    llvm_unreachable("bad initializer kind");

  case DeclContextKind::TopLevelCodeDecl:
    // Mangle the containing module context.
    return appendContext(ctx->getParent());
  }

  llvm_unreachable("bad decl context");
}

void ASTMangler::appendModule(const ModuleDecl *module) {
  assert(!module->getParent() && "cannot mangle nested modules!");

  // Try the special 'swift' substitution.
  if (module->isStdlibModule())
    return appendOperator("s");

  StringRef ModName = module->getName().str();
  if (ModName == MANGLING_MODULE_OBJC)
    return appendOperator("So");
  if (ModName == MANGLING_MODULE_C)
    return appendOperator("SC");

  appendIdentifier(ModName);
}

/// Mangle the name of a protocol as a substitution candidate.
void ASTMangler::appendProtocolName(const ProtocolDecl *protocol) {
  appendContextOf(protocol);
  appendDeclName(protocol);
}

void ASTMangler::appendAnyGenericType(const GenericTypeDecl *decl) {
  // Check for certain standard types.
  if (tryAppendStandardSubstitution(decl))
    return;

  // For generic types, this uses the unbound type.
  Type key;
  if (auto *alias = dyn_cast<TypeAliasDecl>(decl)) {
    if (alias->isGeneric())
      key = alias->getUnboundGenericType();
    else
      key = alias->getDeclaredInterfaceType();
  } else {
    key = cast<NominalTypeDecl>(decl)->getDeclaredType();
  }

  // Try to mangle the entire name as a substitution.
  if (tryMangleSubstitution(key.getPointer()))
    return;

  appendContextOf(decl);
  appendDeclName(decl);

  switch (decl->getKind()) {
  default:
    llvm_unreachable("not a nominal type");

  case DeclKind::TypeAlias:
    appendOperator("a");
    break;
  case DeclKind::Protocol:
    appendOperator("P");
    break;
  case DeclKind::Class:
    appendOperator("C");
    break;
  case DeclKind::Enum:
    appendOperator("O");
    break;
  case DeclKind::Struct:
    appendOperator("V");
    break;
  }
  addSubstitution(key.getPointer());
}

void ASTMangler::appendFunctionType(AnyFunctionType *fn,
                                    bool forceSingleParam) {
  assert((DWARFMangling || fn->isCanonical()) &&
         "expecting canonical types when not mangling for the debugger");

  appendFunctionSignature(fn, forceSingleParam);

  // Note that we do not currently use thin representations in the AST
  // for the types of function decls.  This may need to change at some
  // point, in which case the uncurry logic can probably migrate to that
  // case.
  //
  // It would have been cleverer if we'd used 'f' for thin functions
  // and something else for uncurried functions, but oh well.
  //
  // Or maybe we can change the mangling at the same time we make
  // changes to better support thin functions.
  switch (fn->getRepresentation()) {
  case AnyFunctionType::Representation::Block:
    return appendOperator("XB");
  case AnyFunctionType::Representation::Thin:
    return appendOperator("Xf");
  case AnyFunctionType::Representation::Swift:
    if (fn->isAutoClosure())
      return appendOperator("XK");
    return appendOperator("c");

  case AnyFunctionType::Representation::CFunctionPointer:
    return appendOperator("XC");
  }
}

void ASTMangler::appendFunctionSignature(AnyFunctionType *fn,
                                         bool forceSingleParam) {
  appendParams(fn->getResult(), /*forceSingleParam*/ false);
  appendParams(fn->getInput(), forceSingleParam);
  if (fn->throws())
    appendOperator("K");
}

void ASTMangler::appendParams(Type ParamsTy, bool forceSingleParam) {
  if (TupleType *Tuple = ParamsTy->getAs<TupleType>()) {
    if (Tuple->getNumElements() == 0) {
      if (forceSingleParam) {
        // A tuple containing a single empty tuple.
        appendOperator("y");
        appendOperator("t");
        appendListSeparator();
        appendOperator("t");
      } else {
        appendOperator("y");
      }
      return;
    }
    if (forceSingleParam && Tuple->getNumElements() > 1) {
      appendType(ParamsTy);
      appendListSeparator();
      appendOperator("t");
      return;
    }
  }
  appendType(ParamsTy);
}

void ASTMangler::appendTypeList(Type listTy) {
  if (TupleType *tuple = listTy->getAs<TupleType>()) {
    if (tuple->getNumElements() == 0)
      return appendOperator("y");
    bool firstField = true;
    for (auto &field : tuple->getElements()) {
      appendType(field.getType());
      if (field.hasName())
        appendIdentifier(field.getName().str());
      if (field.isVararg())
        appendOperator("d");
      appendListSeparator(firstField);
    }
  } else {
    appendType(listTy);
    appendListSeparator();
  }
}

void ASTMangler::appendGenericSignature(const GenericSignature *sig) {
  auto canSig = sig->getCanonicalSignature();
  CurGenericSignature = canSig;
  appendGenericSignatureParts(canSig->getGenericParams(), 0,
                              canSig->getRequirements());
}

void ASTMangler::appendRequirement(const Requirement &reqt) {

  Type FirstTy = reqt.getFirstType()->getCanonicalType();

  switch (reqt.getKind()) {
  case RequirementKind::Layout: {
  } break;
  case RequirementKind::Conformance: {
    Type SecondTy = reqt.getSecondType();
    appendProtocolName(SecondTy->castTo<ProtocolType>()->getDecl());
  } break;
  case RequirementKind::Superclass:
  case RequirementKind::SameType: {
    Type SecondTy = reqt.getSecondType();
    appendType(SecondTy->getCanonicalType());
  } break;
  }

  if (auto *DT = FirstTy->getAs<DependentMemberType>()) {
    bool isAssocTypeAtDepth = false;
    if (tryMangleSubstitution(DT)) {
      switch (reqt.getKind()) {
        case RequirementKind::Conformance:
          return appendOperator("RQ");
        case RequirementKind::Layout:
          appendOperator("RL");
          appendOpParamForLayoutConstraint(reqt.getLayoutConstraint());
          return;
        case RequirementKind::Superclass:
          return appendOperator("RB");
        case RequirementKind::SameType:
          return appendOperator("RS");
      }
      llvm_unreachable("bad requirement type");
    }
    GenericTypeParamType *gpBase = appendAssocType(DT, isAssocTypeAtDepth);
    addSubstitution(DT);
    assert(gpBase);
    switch (reqt.getKind()) {
      case RequirementKind::Conformance:
        return appendOpWithGenericParamIndex(isAssocTypeAtDepth ? "RP" : "Rp",
                                             gpBase);
      case RequirementKind::Layout:
        appendOpWithGenericParamIndex(isAssocTypeAtDepth ? "RM" : "Rm", gpBase);
        appendOpParamForLayoutConstraint(reqt.getLayoutConstraint());
        return;
      case RequirementKind::Superclass:
        return appendOpWithGenericParamIndex(isAssocTypeAtDepth ? "RC" : "Rc",
                                             gpBase);
      case RequirementKind::SameType:
        return appendOpWithGenericParamIndex(isAssocTypeAtDepth ? "RT" : "Rt",
                                             gpBase);
    }
    llvm_unreachable("bad requirement type");
  }
  GenericTypeParamType *gpBase = FirstTy->castTo<GenericTypeParamType>();
  switch (reqt.getKind()) {
    case RequirementKind::Conformance:
      return appendOpWithGenericParamIndex("R", gpBase);
    case RequirementKind::Layout:
      appendOpWithGenericParamIndex("Rl", gpBase);
      appendOpParamForLayoutConstraint(reqt.getLayoutConstraint());
      return;
    case RequirementKind::Superclass:
      return appendOpWithGenericParamIndex("Rb", gpBase);
    case RequirementKind::SameType:
      return appendOpWithGenericParamIndex("Rs", gpBase);
  }
  llvm_unreachable("bad requirement type");
}

void ASTMangler::appendGenericSignatureParts(
                                        ArrayRef<GenericTypeParamType*> params,
                                        unsigned initialParamDepth,
                                        ArrayRef<Requirement> requirements) {
  // Mangle the requirements.
  for (const Requirement &reqt : requirements) {
    appendRequirement(reqt);
  }

  if (params.size() == 1 && params[0]->getDepth() == initialParamDepth)
    return appendOperator("l");

  llvm::SmallVector<char, 16> OpStorage;
  llvm::raw_svector_ostream OpBuffer(OpStorage);

  // Mangle the number of parameters.
  unsigned depth = 0;
  unsigned count = 0;
  
  // Since it's unlikely (but not impossible) to have zero generic parameters
  // at a depth, encode indexes starting from 1, and use a special mangling
  // for zero.
  auto mangleGenericParamCount = [&](unsigned depth, unsigned count) {
    if (depth < initialParamDepth)
      return;
    if (count == 0)
      OpBuffer << 'z';
    else
      OpBuffer << Index(count - 1);
  };
  
  // As a special case, mangle nothing if there's a single generic parameter
  // at the initial depth.
  for (auto param : params) {
    if (param->getDepth() != depth) {
      assert(param->getDepth() > depth && "generic params not ordered");
      while (depth < param->getDepth()) {
        mangleGenericParamCount(depth, count);
        ++depth;
        count = 0;
      }
    }
    assert(param->getIndex() == count && "generic params not ordered");
    ++count;
  }
  mangleGenericParamCount(depth, count);
  OpBuffer << 'l';

  appendOperator("r", StringRef(OpStorage.data(), OpStorage.size()));
}

void ASTMangler::appendAssociatedTypeName(DependentMemberType *dmt) {
  auto assocTy = dmt->getAssocType();

  // If the base type is known to have a single protocol conformance
  // in the current generic context, then we don't need to disambiguate the
  // associated type name by protocol.
  // This can result in getting the same mangled string for different
  // DependentMemberTypes. This is not a problem but re-mangling might do more
  // aggressive substitutions, which means that the re-mangled name may differ
  // from the original mangled name.
  // FIXME: We ought to be able to get to the generic signature from a
  // dependent type, but can't yet. Shouldn't need this side channel.

  appendIdentifier(assocTy->getName().str());
  if (!OptimizeProtocolNames || !CurGenericSignature || !Mod
      || CurGenericSignature->getConformsTo(dmt->getBase(), *Mod).size() > 1) {
    appendAnyGenericType(assocTy->getProtocol());
  }
}

void ASTMangler::appendClosureEntity(
                              const SerializedAbstractClosureExpr *closure) {
  appendClosureComponents(closure->getType(), closure->getDiscriminator(),
                          closure->isImplicit(), closure->getParent(),
                          closure->getLocalContext());
}

void ASTMangler::appendClosureEntity(const AbstractClosureExpr *closure) {
  appendClosureComponents(closure->getType(), closure->getDiscriminator(),
                          isa<AutoClosureExpr>(closure), closure->getParent(),
                          closure->getLocalContext());
}

void ASTMangler::appendClosureComponents(Type Ty, unsigned discriminator,
                                      bool isImplicit,
                                      const DeclContext *parentContext,
                                      const DeclContext *localContext) {
  if (!DeclCtx) DeclCtx = localContext;

  assert(discriminator != AbstractClosureExpr::InvalidDiscriminator
         && "closure must be marked correctly with discriminator");

  appendContext(parentContext);

  if (!Ty)
    Ty = ErrorType::get(localContext->getASTContext());

  Ty = parentContext->mapTypeOutOfContext(Ty);
  appendType(Ty->getCanonicalType());
  appendOperator(isImplicit ? "fu" : "fU", Index(discriminator));
}

void ASTMangler::appendDefaultArgumentEntity(const DeclContext *func,
                                             unsigned index) {
  appendContext(func);
  appendOperator("fA", Index(index));
}

void ASTMangler::appendInitializerEntity(const VarDecl *var) {
  appendEntity(var, "v", var->isStatic());
  appendOperator("fi");
}

/// Is this declaration a method for mangling purposes? If so, we'll leave the
/// Self type out of its mangling.
static bool isMethodDecl(const Decl *decl) {
  return isa<AbstractFunctionDecl>(decl)
    && decl->getDeclContext()->isTypeContext();
}

static bool genericParamIsBelowDepth(Type type, unsigned methodDepth) {
  if (!type->hasTypeParameter())
    return true;

  return !type.findIf([methodDepth](Type t) -> bool {
    if (auto *gp = t->getAs<GenericTypeParamType>())
      return gp->getDepth() >= methodDepth;
    return false;
  });
}

CanType ASTMangler::getDeclTypeForMangling(
    const ValueDecl *decl,
    ArrayRef<GenericTypeParamType *> &genericParams,
    unsigned &initialParamDepth,
    ArrayRef<Requirement> &requirements,
    SmallVectorImpl<Requirement> &requirementsBuf) {
  auto &C = decl->getASTContext();
  if (!decl->hasInterfaceType() || decl->getInterfaceType()->is<ErrorType>()) {
    if (isa<AbstractFunctionDecl>(decl))
      return CanFunctionType::get(C.TheErrorType, C.TheErrorType);
    return C.TheErrorType;
  }

  auto type = decl->getInterfaceType()->getCanonicalType();

  initialParamDepth = 0;
  CanGenericSignature sig;
  if (auto gft = dyn_cast<GenericFunctionType>(type)) {
    sig = gft.getGenericSignature();
    CurGenericSignature = sig;
    genericParams = sig->getGenericParams();
    requirements = sig->getRequirements();

    type = CanFunctionType::get(gft.getInput(), gft.getResult(),
                                gft->getExtInfo());
  } else {
    genericParams = {};
    requirements = {};
  }

  if (!type->hasError()) {
    // Shed the 'self' type and generic requirements from method manglings.
    if (isMethodDecl(decl)) {
      // Drop the Self argument clause from the type.
      type = cast<AnyFunctionType>(type).getResult();
    }

    if (isMethodDecl(decl) || isa<SubscriptDecl>(decl)) {
      // Drop generic parameters and requirements from the method's context.
      auto parentGenericSig =
        decl->getDeclContext()->getGenericSignatureOfContext();
      if (parentGenericSig && sig) {
        // The method's depth starts above the depth of the context.
        if (!parentGenericSig->getGenericParams().empty())
          initialParamDepth =
            parentGenericSig->getGenericParams().back()->getDepth() + 1;

        while (!genericParams.empty()) {
          if (genericParams.front()->getDepth() >= initialParamDepth)
            break;
          genericParams = genericParams.slice(1);
        }

        requirementsBuf.clear();
        for (auto &reqt : sig->getRequirements()) {
          switch (reqt.getKind()) {
        case RequirementKind::Conformance:
        case RequirementKind::Layout:
        case RequirementKind::Superclass:
          // We don't need the requirement if the constrained type is below the
          // method depth.
          if (genericParamIsBelowDepth(reqt.getFirstType(), initialParamDepth))
            continue;
          break;
        case RequirementKind::SameType:
          // We don't need the requirement if both types are below the method
          // depth, or non-dependent.
          if (genericParamIsBelowDepth(reqt.getFirstType(), initialParamDepth) &&
              genericParamIsBelowDepth(reqt.getSecondType(), initialParamDepth))
            continue;
          break;
          }

          // If we fell through the switch, mangle the requirement.
          requirementsBuf.push_back(reqt);
        }
        requirements = requirementsBuf;
      }
    }
  }
  return type->getCanonicalType();
}

void ASTMangler::appendDeclType(const ValueDecl *decl, bool isFunctionMangling) {
  ArrayRef<GenericTypeParamType *> genericParams;
  unsigned initialParamDepth;
  ArrayRef<Requirement> requirements;
  SmallVector<Requirement, 4> requirementsBuf;
  Mod = decl->getModuleContext();
  auto type = getDeclTypeForMangling(decl,
                                     genericParams, initialParamDepth,
                                     requirements, requirementsBuf);
 
  if (AnyFunctionType *FuncTy = type->getAs<AnyFunctionType>()) {

    const ParameterList *Params = nullptr;
    if (const auto *FDecl = dyn_cast<AbstractFunctionDecl>(decl)) {
      unsigned PListIdx = isMethodDecl(decl) ? 1 : 0;
      if (PListIdx < FDecl->getNumParameterLists()) {
        Params = FDecl->getParameterList(PListIdx);
      }
    } else if (const auto *SDecl = dyn_cast<SubscriptDecl>(decl)) {
        Params = SDecl->getIndices();
    }
    bool forceSingleParam = Params && (Params->size() == 1);
          

    if (isFunctionMangling) {
      appendFunctionSignature(FuncTy, forceSingleParam);
    } else {
      appendFunctionType(FuncTy, forceSingleParam);
    }
  } else {
    appendType(type);
  }

  // Mangle the generic signature, if any.
  if (!genericParams.empty() || !requirements.empty()) {
    appendGenericSignatureParts(genericParams, initialParamDepth,
                                requirements);
    // The 'F' function mangling doesn't need a 'u' for its generic signature.
    if (!isFunctionMangling)
      appendOperator("u");
  }
}

bool ASTMangler::tryAppendStandardSubstitution(const GenericTypeDecl *decl) {
  // Bail out if our parent isn't the swift standard library.
  if (!isStdlibType(decl))
    return false;

  if (char Subst = getStandardTypeSubst(decl->getName().str())) {
    if (!SubstMerging.tryMergeSubst(*this, Subst, /*isStandardSubst*/ true)) {
      appendOperator("S", StringRef(&Subst, 1));
    }
    return true;
  }
  return false;
}

void ASTMangler::appendConstructorEntity(const ConstructorDecl *ctor,
                                         bool isAllocating) {
  appendContextOf(ctor);
  appendDeclType(ctor);
  StringRef privateDiscriminator = getPrivateDiscriminatorIfNecessary(ctor);
  if (!privateDiscriminator.empty()) {
    appendIdentifier(privateDiscriminator);
    appendOperator("Ll");
  }
  appendOperator(isAllocating ? "fC" : "fc");
}

void ASTMangler::appendDestructorEntity(const DestructorDecl *dtor,
                                     bool isDeallocating) {
  appendContextOf(dtor);
  appendOperator(isDeallocating ? "fD" : "fd");
}

static StringRef getCodeForAccessorKind(AccessorKind kind,
                                        AddressorKind addressorKind) {
  switch (kind) {
  case AccessorKind::NotAccessor: llvm_unreachable("bad accessor kind!");
  case AccessorKind::IsGetter:    return "g";
  case AccessorKind::IsSetter:    return "s";
  case AccessorKind::IsWillSet:   return "w";
  case AccessorKind::IsDidSet:    return "W";
  case AccessorKind::IsAddressor:
    // 'l' is for location. 'A' was taken.
    switch (addressorKind) {
    case AddressorKind::NotAddressor: llvm_unreachable("bad combo");
    case AddressorKind::Unsafe: return "lu";
    case AddressorKind::Owning: return "lO";
    case AddressorKind::NativeOwning: return "lo";
    case AddressorKind::NativePinning: return "lp";
    }
    llvm_unreachable("bad addressor kind");
  case AccessorKind::IsMutableAddressor:
    switch (addressorKind) {
    case AddressorKind::NotAddressor: llvm_unreachable("bad combo");
    case AddressorKind::Unsafe: return "au";
    case AddressorKind::Owning: return "aO";
    case AddressorKind::NativeOwning: return "ao";
    case AddressorKind::NativePinning: return "aP";
    }
    llvm_unreachable("bad addressor kind");
  case AccessorKind::IsMaterializeForSet: return "m";
  }
  llvm_unreachable("bad accessor kind");
}

void ASTMangler::appendAccessorEntity(AccessorKind kind,
                                      AddressorKind addressorKind,
                                      const ValueDecl *decl,
                                      bool isStatic) {
  assert(kind != AccessorKind::NotAccessor);
  appendContextOf(decl);
  bindGenericParameters(decl->getDeclContext());
  appendDeclName(decl);
  appendDeclType(decl);
  appendOperator("f", getCodeForAccessorKind(kind, addressorKind));
  if (isStatic)
    appendOperator("Z");
}

void ASTMangler::appendEntity(const ValueDecl *decl, StringRef EntityOp,
                              bool isStatic) {
  if (!DeclCtx) DeclCtx = decl->getInnermostDeclContext();
  appendContextOf(decl);
  appendDeclName(decl);
  appendDeclType(decl);
  appendOperator(EntityOp);
  if (isStatic)
    appendOperator("Z");
}

void ASTMangler::appendEntity(const ValueDecl *decl) {
  if (!DeclCtx) DeclCtx = decl->getInnermostDeclContext();
  assert(!isa<ConstructorDecl>(decl));
  assert(!isa<DestructorDecl>(decl));
  
  // Handle accessors specially, they are mangled as modifiers on the accessed
  // declaration.
  if (auto func = dyn_cast<FuncDecl>(decl)) {
    auto accessorKind = func->getAccessorKind();
    if (accessorKind != AccessorKind::NotAccessor)
      return appendAccessorEntity(accessorKind, func->getAddressorKind(),
                                  func->getAccessorStorageDecl(),
                                  decl->isStatic());
  }

  if (isa<VarDecl>(decl))
    return appendEntity(decl, "v", decl->isStatic());
  if (isa<SubscriptDecl>(decl))
    return appendEntity(decl, "i", decl->isStatic());
  if (isa<GenericTypeParamDecl>(decl))
    return appendEntity(decl, "fp", decl->isStatic());

  assert(isa<AbstractFunctionDecl>(decl) || isa<EnumElementDecl>(decl));

  appendContextOf(decl);
  appendDeclName(decl);
  appendDeclType(decl, /*isFunctionMangling*/ true);
  appendOperator("F");
  if (decl->isStatic())
    appendOperator("Z");
}

void ASTMangler::appendProtocolConformance(const ProtocolConformance *conformance){
  Mod = conformance->getDeclContext()->getParentModule();
  if (auto behaviorStorage = conformance->getBehaviorDecl()) {
    auto topLevelContext =
      conformance->getDeclContext()->getModuleScopeContext();
    appendContextOf(behaviorStorage);
    FileUnit *fileUnit = cast<FileUnit>(topLevelContext);
    appendIdentifier(
              fileUnit->getDiscriminatorForPrivateValue(behaviorStorage).str());
    appendProtocolName(conformance->getProtocol());
    appendIdentifier(behaviorStorage->getBaseName().getIdentifier().str());
  } else {
    auto conformanceDC = conformance->getDeclContext();
    auto conformingType =
      conformanceDC->mapTypeOutOfContext(conformance->getType());
    appendType(conformingType->getCanonicalType());
    appendProtocolName(conformance->getProtocol());
    appendModule(conformance->getDeclContext()->getParentModule());
  }
  if (GenericSignature *Sig = conformance->getGenericSignature()) {
    appendGenericSignature(Sig);
  }
}

void ASTMangler::appendOpParamForLayoutConstraint(LayoutConstraint layout) {
  assert(layout);
  switch (layout->getKind()) {
  case LayoutConstraintKind::UnknownLayout:
    appendOperatorParam("U");
    break;
  case LayoutConstraintKind::RefCountedObject:
    appendOperatorParam("R");
    break;
  case LayoutConstraintKind::NativeRefCountedObject:
    appendOperatorParam("N");
    break;
  case LayoutConstraintKind::Class:
    appendOperatorParam("C");
    break;
  case LayoutConstraintKind::NativeClass:
    appendOperatorParam("D");
    break;
  case LayoutConstraintKind::Trivial:
    appendOperatorParam("T");
    break;
  case LayoutConstraintKind::TrivialOfExactSize:
    if (!layout->getAlignment())
      appendOperatorParam("e", Index(layout->getTrivialSizeInBits()));
    else
      appendOperatorParam("E", Index(layout->getTrivialSizeInBits()),
                     Index(layout->getAlignment()));
    break;
  case LayoutConstraintKind::TrivialOfAtMostSize:
    if (!layout->getAlignment())
      appendOperatorParam("m", Index(layout->getTrivialSizeInBits()));
    else
      appendOperatorParam("M", Index(layout->getTrivialSizeInBits()),
                     Index(layout->getAlignment()));
    break;
  }
}
