//===--- GenDecl.cpp - IR Generation for Declarations ---------------------===//
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
//  This file implements IR generation for local and global
//  declarations in Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsIRGen.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeMemberVisitor.h"
#include "swift/AST/Types.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/IRGen/Linking.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/SIL/FormalLinkage.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILModule.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/GlobalDecl.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/TypeBuilder.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include "ConstantBuilder.h"
#include "Explosion.h"
#include "FixedTypeInfo.h"
#include "GenCall.h"
#include "GenClass.h"
#include "GenDecl.h"
#include "GenMeta.h"
#include "GenObjC.h"
#include "GenOpaque.h"
#include "GenProto.h"
#include "GenType.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenMangler.h"
#include "IRGenModule.h"
#include "LoadableTypeInfo.h"
#include "MetadataRequest.h"
#include "ProtocolInfo.h"
#include "Signature.h"
#include "StructLayout.h"

using namespace swift;
using namespace irgen;

bool IRGenerator::tryEnableLazyTypeMetadata(NominalTypeDecl *Nominal) {
  // When compiling with -Onone keep all metadata for the debugger. Even if it
  // is not used by the program itself.
  if (!Opts.shouldOptimize())
    return false;
  if (Opts.UseJIT)
    return false;

  switch (Nominal->getKind()) {
    case DeclKind::Enum:
    case DeclKind::Struct:
      break;
    default:
      // Keep all metadata for classes, because a class can be instantiated by
      // using the library function _typeByName or NSClassFromString.
      return false;
  }

  switch (getDeclLinkage(Nominal)) {
    case FormalLinkage::PublicUnique:
    case FormalLinkage::PublicNonUnique:
      // We can't remove metadata for externally visible types.
      return false;
    case FormalLinkage::HiddenUnique:
      // In non-whole-module mode, also internal types are visible externally.
      if (!SIL.isWholeModule())
        return false;
      break;
    case FormalLinkage::Private:
      break;
  }

  auto insertResult = LazyTypeGlobals.try_emplace(Nominal);
  auto &entry = insertResult.first->second;
  assert(!entry.IsLazy);
  entry.IsLazy = true;
  if (entry.IsMetadataUsed)
    LazyTypeMetadata.push_back(Nominal);
  if (entry.IsDescriptorUsed)
    LazyTypeContextDescriptors.push_back(Nominal);

  return true;
}

namespace {
  
/// Add methods, properties, and protocol conformances from a JITed extension
/// to an ObjC class using the ObjC runtime.
///
/// This must happen after ObjCProtocolInitializerVisitor if any @objc protocols
/// were defined in the TU.
class CategoryInitializerVisitor
  : public ClassMemberVisitor<CategoryInitializerVisitor>
{
  IRGenFunction &IGF;
  IRGenModule &IGM = IGF.IGM;
  IRBuilder &Builder = IGF.Builder;
  
  llvm::Constant *class_replaceMethod;
  llvm::Constant *class_addProtocol;
  
  llvm::Constant *classMetadata;
  llvm::Constant *metaclassMetadata;
  
public:
  CategoryInitializerVisitor(IRGenFunction &IGF, ExtensionDecl *ext)
    : IGF(IGF)
  {
    class_replaceMethod = IGM.getClassReplaceMethodFn();
    class_addProtocol = IGM.getClassAddProtocolFn();

    CanType origTy = ext->getAsNominalTypeOrNominalTypeExtensionContext()
        ->getDeclaredType()->getCanonicalType();
    classMetadata =
      tryEmitConstantHeapMetadataRef(IGM, origTy, /*allowUninit*/ true);
    assert(classMetadata &&
           "extended objc class doesn't have constant metadata?!");
    classMetadata = llvm::ConstantExpr::getBitCast(classMetadata,
                                                   IGM.ObjCClassPtrTy);
    metaclassMetadata = IGM.getAddrOfMetaclassObject(
                                       origTy.getClassOrBoundGenericClass(),
                                                         NotForDefinition);
    metaclassMetadata = llvm::ConstantExpr::getBitCast(metaclassMetadata,
                                                   IGM.ObjCClassPtrTy);

    // We need to make sure the Objective-C runtime has initialized our
    // class. If you try to add or replace a method to a class that isn't
    // initialized yet, the Objective-C runtime will crash in the calls
    // to class_replaceMethod or class_addProtocol.
    Builder.CreateCall(IGM.getGetInitializedObjCClassFn(), classMetadata);

    // Register ObjC protocol conformances.
    for (auto *p : ext->getLocalProtocols()) {
      if (!p->isObjC())
        continue;
      
      llvm::Value *protoRef = IGM.getAddrOfObjCProtocolRef(p, NotForDefinition);
      auto proto = Builder.CreateLoad(protoRef, IGM.getPointerAlignment());
      Builder.CreateCall(class_addProtocol, {classMetadata, proto});
    }
  }
  
  void visitMembers(ExtensionDecl *ext) {
    for (Decl *member : ext->getMembers())
      visit(member);
  }

  void visitTypeDecl(TypeDecl *type) {
    // We'll visit nested types separately if necessary.
  }

  void visitMissingMemberDecl(MissingMemberDecl *placeholder) {}
  
  void visitFuncDecl(FuncDecl *method) {
    if (!requiresObjCMethodDescriptor(method)) return;

    // Don't emit getters/setters for @NSManaged methods.
    if (method->getAttrs().hasAttribute<NSManagedAttr>())
      return;

    llvm::Constant *name, *imp, *types;
    emitObjCMethodDescriptorParts(IGM, method,
                                  /*extended*/false,
                                  /*concrete*/true,
                                  name, types, imp);
    
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(),
                                          name);
    
    llvm::Value *args[] = {
      method->isStatic() ? metaclassMetadata : classMetadata,
      sel,
      imp,
      types
    };
    
    Builder.CreateCall(class_replaceMethod, args);
  }

  // Can't be added in an extension.
  void visitDestructorDecl(DestructorDecl *dtor) {}

  void visitConstructorDecl(ConstructorDecl *constructor) {
    if (!requiresObjCMethodDescriptor(constructor)) return;
    llvm::Constant *name, *imp, *types;
    emitObjCMethodDescriptorParts(IGM, constructor, /*extended*/false,
                                  /*concrete*/true,
                                  name, types, imp);

    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(),
                                          name);

    llvm::Value *args[] = {
      classMetadata,
      sel,
      imp,
      types
    };

    Builder.CreateCall(class_replaceMethod, args);
  }

  void visitPatternBindingDecl(PatternBindingDecl *binding) {
    // Ignore the PBD and just handle the individual vars.
  }
  
  void visitVarDecl(VarDecl *prop) {
    if (!requiresObjCPropertyDescriptor(IGM, prop)) return;

    // FIXME: register property metadata in addition to the methods.
    // ObjC doesn't have a notion of class properties, so we'd only do this
    // for instance properties.

    // Don't emit getters/setters for @NSManaged properties.
    if (prop->getAttrs().hasAttribute<NSManagedAttr>())
      return;

    llvm::Constant *name, *imp, *types;
    emitObjCGetterDescriptorParts(IGM, prop,
                                  name, types, imp);
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(),
                                          name);
    auto theClass = prop->isStatic() ? metaclassMetadata : classMetadata;
    llvm::Value *getterArgs[] = {theClass, sel, imp, types};
    Builder.CreateCall(class_replaceMethod, getterArgs);

    if (prop->isSettable(prop->getDeclContext())) {
      emitObjCSetterDescriptorParts(IGM, prop,
                                    name, types, imp);
      sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(),
                               name);
      llvm::Value *setterArgs[] = {theClass, sel, imp, types};
      
      Builder.CreateCall(class_replaceMethod, setterArgs);
    }
  }

  void visitSubscriptDecl(SubscriptDecl *subscript) {
    assert(!subscript->isStatic() && "objc doesn't support class subscripts");
    if (!requiresObjCSubscriptDescriptor(IGM, subscript)) return;
    
    llvm::Constant *name, *imp, *types;
    emitObjCGetterDescriptorParts(IGM, subscript,
                                  name, types, imp);
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(),
                                          name);
    llvm::Value *getterArgs[] = {classMetadata, sel, imp, types};
    Builder.CreateCall(class_replaceMethod, getterArgs);

    if (subscript->isSettable()) {
      emitObjCSetterDescriptorParts(IGM, subscript,
                                    name, types, imp);
      sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(),
                               name);
      llvm::Value *setterArgs[] = {classMetadata, sel, imp, types};
      
      Builder.CreateCall(class_replaceMethod, setterArgs);
    }
  }
};

/// Create a descriptor for JITed @objc protocol using the ObjC runtime.
class ObjCProtocolInitializerVisitor
  : public ClassMemberVisitor<ObjCProtocolInitializerVisitor>
{
  IRGenFunction &IGF;
  IRGenModule &IGM = IGF.IGM;
  IRBuilder &Builder = IGF.Builder;

  llvm::Constant *objc_getProtocol,
                 *objc_allocateProtocol,
                 *objc_registerProtocol,
                 *protocol_addMethodDescription,
                 *protocol_addProtocol;
  
  llvm::Value *NewProto = nullptr;
  
public:
  ObjCProtocolInitializerVisitor(IRGenFunction &IGF)
    : IGF(IGF)
  {
    objc_getProtocol = IGM.getGetObjCProtocolFn();
    objc_allocateProtocol = IGM.getAllocateObjCProtocolFn();
    objc_registerProtocol = IGM.getRegisterObjCProtocolFn();
    protocol_addMethodDescription = IGM.getProtocolAddMethodDescriptionFn();
    protocol_addProtocol = IGM.getProtocolAddProtocolFn();
  }
  
  void visitMembers(ProtocolDecl *proto) {
    // Check if the ObjC runtime already has a descriptor for this
    // protocol. If so, use it.
    SmallString<32> buf;
    auto protocolName
      = IGM.getAddrOfGlobalString(proto->getObjCRuntimeName(buf));
    
    auto existing = Builder.CreateCall(objc_getProtocol, protocolName);
    auto isNull = Builder.CreateICmpEQ(existing,
                   llvm::ConstantPointerNull::get(IGM.ProtocolDescriptorPtrTy));

    auto existingBB = IGF.createBasicBlock("existing_protocol");
    auto newBB = IGF.createBasicBlock("new_protocol");
    auto contBB = IGF.createBasicBlock("cont");
    Builder.CreateCondBr(isNull, newBB, existingBB);
    
    // Nothing to do if there's already a descriptor.
    Builder.emitBlock(existingBB);
    Builder.CreateBr(contBB);
    
    Builder.emitBlock(newBB);
    
    // Allocate the protocol descriptor.
    NewProto = Builder.CreateCall(objc_allocateProtocol, protocolName);
    
    // Add the parent protocols.
    for (auto parentProto : proto->getInheritedProtocols()) {
      if (!parentProto->isObjC())
        continue;
      llvm::Value *parentRef = IGM.getAddrOfObjCProtocolRef(parentProto,
                                                            NotForDefinition);
      parentRef = IGF.Builder.CreateBitCast(parentRef,
                                            IGM.ProtocolDescriptorPtrTy
                                              ->getPointerTo());
      auto parent = Builder.CreateLoad(parentRef,
                                       IGM.getPointerAlignment());
      Builder.CreateCall(protocol_addProtocol, {NewProto, parent});
    }
    
    // Add the members.
    for (Decl *member : proto->getMembers())
      visit(member);
    
    // Register it.
    Builder.CreateCall(objc_registerProtocol, NewProto);
    Builder.CreateBr(contBB);
    
    // Store the reference to the runtime's idea of the protocol descriptor.
    Builder.emitBlock(contBB);
    auto result = Builder.CreatePHI(IGM.ProtocolDescriptorPtrTy, 2);
    result->addIncoming(existing, existingBB);
    result->addIncoming(NewProto, newBB);
    
    llvm::Value *ref = IGM.getAddrOfObjCProtocolRef(proto, NotForDefinition);
    ref = IGF.Builder.CreateBitCast(ref,
                                  IGM.ProtocolDescriptorPtrTy->getPointerTo());

    Builder.CreateStore(result, ref, IGM.getPointerAlignment());
  }

  void visitTypeDecl(TypeDecl *type) {
    // We'll visit nested types separately if necessary.
  }

  void visitMissingMemberDecl(MissingMemberDecl *placeholder) {}

  void visitAbstractFunctionDecl(AbstractFunctionDecl *method) {
    llvm::Constant *name, *imp, *types;
    emitObjCMethodDescriptorParts(IGM, method, /*extended*/true,
                                  /*concrete*/false,
                                  name, types, imp);
    
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(), name);

    llvm::Value *args[] = {
      NewProto, sel, types,
      // required?
      llvm::ConstantInt::get(IGM.ObjCBoolTy,
                             !method->getAttrs().hasAttribute<OptionalAttr>()),
      // instance?
      llvm::ConstantInt::get(IGM.ObjCBoolTy,
                   isa<ConstructorDecl>(method) || method->isInstanceMember()),
    };
    
    Builder.CreateCall(protocol_addMethodDescription, args);
  }
  
  void visitPatternBindingDecl(PatternBindingDecl *binding) {
    // Ignore the PBD and just handle the individual vars.
  }
  
  void visitAbstractStorageDecl(AbstractStorageDecl *prop) {
    // TODO: Add properties to protocol.
    
    llvm::Constant *name, *imp, *types;
    emitObjCGetterDescriptorParts(IGM, prop,
                                  name, types, imp);
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    llvm::Value *sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(), name);
    llvm::Value *getterArgs[] = {
      NewProto, sel, types,
      // required?
      llvm::ConstantInt::get(IGM.ObjCBoolTy,
                             !prop->getAttrs().hasAttribute<OptionalAttr>()),
      // instance?
      llvm::ConstantInt::get(IGM.ObjCBoolTy,
                             prop->isInstanceMember()),
    };
    Builder.CreateCall(protocol_addMethodDescription, getterArgs);
    
    if (prop->isSettable(nullptr)) {
      emitObjCSetterDescriptorParts(IGM, prop, name, types, imp);
      sel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(), name);
      llvm::Value *setterArgs[] = {
        NewProto, sel, types,
        // required?
        llvm::ConstantInt::get(IGM.ObjCBoolTy,
                               !prop->getAttrs().hasAttribute<OptionalAttr>()),
        // instance?
        llvm::ConstantInt::get(IGM.ObjCBoolTy,
                               prop->isInstanceMember()),
      };
      Builder.CreateCall(protocol_addMethodDescription, setterArgs);
    }
  }
};

} // end anonymous namespace

namespace {
class PrettySourceFileEmission : public llvm::PrettyStackTraceEntry {
  const SourceFile &SF;
public:
  explicit PrettySourceFileEmission(const SourceFile &SF) : SF(SF) {}

  void print(raw_ostream &os) const override {
    os << "While emitting IR for source file " << SF.getFilename() << '\n';
  }
};
} // end anonymous namespace

/// Emit all the top-level code in the source file.
void IRGenModule::emitSourceFile(SourceFile &SF, unsigned StartElem) {
  PrettySourceFileEmission StackEntry(SF);
  llvm::SaveAndRestore<SourceFile *> SetCurSourceFile(CurSourceFile, &SF);

  // Emit types and other global decls.
  for (unsigned i = StartElem, e = SF.Decls.size(); i != e; ++i)
    emitGlobalDecl(SF.Decls[i]);
  for (auto *localDecl : SF.LocalTypeDecls)
    emitGlobalDecl(localDecl);

  SF.collectLinkLibraries([this](LinkLibrary linkLib) {
      this->addLinkLibrary(linkLib);
  });

  if (ObjCInterop)
    this->addLinkLibrary(LinkLibrary("objc", LibraryKind::Library));
}

/// Collect elements of an already-existing global list with the given
/// \c name into \c list.
///
/// We use this when Clang code generation might populate the list.
static void collectGlobalList(IRGenModule &IGM,
                              SmallVectorImpl<llvm::WeakTrackingVH> &list,
                              StringRef name) {
  if (auto *existing = IGM.Module.getGlobalVariable(name)) {
    auto *globals = cast<llvm::ConstantArray>(existing->getInitializer());
    for (auto &use : globals->operands()) {
      auto *global = use.get();
      list.push_back(global);
    }
    existing->eraseFromParent();
  }

  std::for_each(list.begin(), list.end(),
                [](const llvm::WeakTrackingVH &global) {
    assert(!isa<llvm::GlobalValue>(global) ||
           !cast<llvm::GlobalValue>(global)->isDeclaration() &&
           "all globals in the 'used' list must be definitions");
  });
}

/// Emit a global list, i.e. a global constant array holding all of a
/// list of values.  Generally these lists are for various LLVM
/// metadata or runtime purposes.
static llvm::GlobalVariable *
emitGlobalList(IRGenModule &IGM, ArrayRef<llvm::WeakTrackingVH> handles,
               StringRef name, StringRef section,
               llvm::GlobalValue::LinkageTypes linkage,
               llvm::Type *eltTy,
               bool isConstant) {
  // Do nothing if the list is empty.
  if (handles.empty()) return nullptr;

  // For global lists that actually get linked (as opposed to notional
  // ones like @llvm.used), it's important to set an explicit alignment
  // so that the linker doesn't accidentally put padding in the list.
  Alignment alignment = IGM.getPointerAlignment();

  // We have an array of value handles, but we need an array of constants.
  SmallVector<llvm::Constant*, 8> elts;
  elts.reserve(handles.size());
  for (auto &handle : handles) {
    auto elt = cast<llvm::Constant>(&*handle);
    if (elt->getType() != eltTy)
      elt = llvm::ConstantExpr::getBitCast(elt, eltTy);
    elts.push_back(elt);
  }

  auto varTy = llvm::ArrayType::get(eltTy, elts.size());
  auto init = llvm::ConstantArray::get(varTy, elts);
  auto var = new llvm::GlobalVariable(IGM.Module, varTy, isConstant, linkage,
                                      init, name);
  var->setSection(section);
  var->setAlignment(alignment.getValue());
  disableAddressSanitizer(IGM, var);

  // Mark the variable as used if doesn't have external linkage.
  // (Note that we'd specifically like to not put @llvm.used in itself.)
  if (llvm::GlobalValue::isLocalLinkage(linkage))
    IGM.addUsedGlobal(var);
  return var;
}

void IRGenModule::emitRuntimeRegistration() {
  // Duck out early if we have nothing to register.
  if (SwiftProtocols.empty() && ProtocolConformances.empty() &&
      RuntimeResolvableTypes.empty() &&
      (!ObjCInterop || (ObjCProtocols.empty() && ObjCClasses.empty() &&
                        ObjCCategoryDecls.empty())) &&
      FieldDescriptors.empty())
    return;
  
  // Find the entry point.
  SILFunction *EntryPoint =
    getSILModule().lookUpFunction(SWIFT_ENTRY_POINT_FUNCTION);
  
  // If we're debugging (and not in the REPL), we don't have a
  // main. Find a function marked with the LLDBDebuggerFunction
  // attribute instead.
  if (!EntryPoint && Context.LangOpts.DebuggerSupport) {
    for (SILFunction &SF : getSILModule()) {
      if (SF.hasLocation()) {
        if (Decl* D = SF.getLocation().getAsASTNode<Decl>()) {
          if (auto *FD = dyn_cast<FuncDecl>(D)) {
            if (FD->getAttrs().hasAttribute<LLDBDebuggerFunctionAttr>()) {
              EntryPoint = &SF;
              break;
            }
          }
        }
      }
    }
  }
  
  if (!EntryPoint)
    return;
    
  llvm::Function *EntryFunction = Module.getFunction(EntryPoint->getName());
  if (!EntryFunction)
    return;
  
  // Create a new function to contain our logic.
  auto fnTy = llvm::FunctionType::get(VoidTy, /*varArg*/ false);
  auto RegistrationFunction = llvm::Function::Create(fnTy,
                                           llvm::GlobalValue::PrivateLinkage,
                                           "runtime_registration",
                                           getModule());
  RegistrationFunction->setAttributes(constructInitialAttributes());
  
  // Insert a call into the entry function.
  {
    llvm::BasicBlock *EntryBB = &EntryFunction->getEntryBlock();
    llvm::BasicBlock::iterator IP = EntryBB->getFirstInsertionPt();
    IRBuilder Builder(getLLVMContext(),
                      DebugInfo && !Context.LangOpts.DebuggerSupport);
    Builder.llvm::IRBuilderBase::SetInsertPoint(EntryBB, IP);
    if (DebugInfo && !Context.LangOpts.DebuggerSupport)
      DebugInfo->setEntryPointLoc(Builder);
    Builder.CreateCall(RegistrationFunction, {});
  }
  
  IRGenFunction RegIGF(*this, RegistrationFunction);
  if (DebugInfo && !Context.LangOpts.DebuggerSupport)
    DebugInfo->emitArtificialFunction(RegIGF, RegistrationFunction);
  
  // Register ObjC protocols, classes, and extensions we added.
  if (ObjCInterop) {
    if (!ObjCProtocols.empty()) {
      // We need to initialize ObjC protocols in inheritance order, parents
      // first.
      
      llvm::DenseSet<ProtocolDecl*> protos;
      for (auto &proto : ObjCProtocols)
        protos.insert(proto.first);
      
      llvm::SmallVector<ProtocolDecl*, 4> protoInitOrder;

      std::function<void(ProtocolDecl*)> orderProtocol
        = [&](ProtocolDecl *proto) {
          // Recursively put parents first.
          for (auto parent : proto->getInheritedProtocols())
            orderProtocol(parent);

          // Skip if we don't need to reify this protocol.
          auto found = protos.find(proto);
          if (found == protos.end())
            return;
          protos.erase(found);
          protoInitOrder.push_back(proto);
        };
      
      while (!protos.empty()) {
        orderProtocol(*protos.begin());
      }

      // Visit the protocols in the order we established.
      for (auto *proto : protoInitOrder) {
        ObjCProtocolInitializerVisitor(RegIGF)
          .visitMembers(proto);
      }
    }
    
    for (llvm::WeakTrackingVH &ObjCClass : ObjCClasses) {
      RegIGF.Builder.CreateCall(getInstantiateObjCClassFn(), {ObjCClass});
    }
      
    for (ExtensionDecl *ext : ObjCCategoryDecls) {
      CategoryInitializerVisitor(RegIGF, ext).visitMembers(ext);
    }
  }

  // Register Swift protocols if we added any.
  if (!SwiftProtocols.empty()) {
    llvm::Constant *protocols = emitSwiftProtocols();

    llvm::Constant *beginIndices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, 0),
    };
    auto begin = llvm::ConstantExpr::getGetElementPtr(
        /*Ty=*/nullptr, protocols, beginIndices);
    llvm::Constant *endIndices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, SwiftProtocols.size()),
    };
    auto end = llvm::ConstantExpr::getGetElementPtr(
        /*Ty=*/nullptr, protocols, endIndices);

    RegIGF.Builder.CreateCall(getRegisterProtocolsFn(), {begin, end});
  }

  // Register Swift protocol conformances if we added any.
  if (!ProtocolConformances.empty()) {

    llvm::Constant *conformances = emitProtocolConformances();

    llvm::Constant *beginIndices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, 0),
    };
    auto begin = llvm::ConstantExpr::getGetElementPtr(
        /*Ty=*/nullptr, conformances, beginIndices);
    llvm::Constant *endIndices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, ProtocolConformances.size()),
    };
    auto end = llvm::ConstantExpr::getGetElementPtr(
        /*Ty=*/nullptr, conformances, endIndices);
    
    RegIGF.Builder.CreateCall(getRegisterProtocolConformancesFn(), {begin, end});
  }

  if (!RuntimeResolvableTypes.empty()) {
    llvm::Constant *records = emitTypeMetadataRecords();

    llvm::Constant *beginIndices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, 0),
    };
    auto begin = llvm::ConstantExpr::getGetElementPtr(
        /*Ty=*/nullptr, records, beginIndices);
    llvm::Constant *endIndices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, RuntimeResolvableTypes.size()),
    };
    auto end = llvm::ConstantExpr::getGetElementPtr(
        /*Ty=*/nullptr, records, endIndices);

    RegIGF.Builder.CreateCall(getRegisterTypeMetadataRecordsFn(), {begin, end});
  }

  if (!FieldDescriptors.empty()) {
    auto *records = emitFieldDescriptors();
    auto *begin =
        llvm::ConstantExpr::getBitCast(records, FieldDescriptorPtrPtrTy);
    auto *size = llvm::ConstantInt::get(SizeTy, FieldDescriptors.size());
    RegIGF.Builder.CreateCall(getRegisterFieldDescriptorsFn(), {begin, size});
  }

  RegIGF.Builder.CreateRetVoid();
}

/// Return the address of the context descriptor representing the given
/// decl context.
///
/// For a nominal type context, this returns the address of the nominal type
/// descriptor.
/// For an extension context, this returns the address of the extension
/// context descriptor.
/// For a module or file unit context, this returns the address of the module
/// context descriptor.
/// For any other kind of context, this returns an anonymous context descriptor
/// for the context.
ConstantReference
IRGenModule::getAddrOfParentContextDescriptor(DeclContext *from) {
  // Some types get special treatment.
  if (auto Type = dyn_cast<NominalTypeDecl>(from)) {
    // Use a special module context if we have one.
    if (auto context = Mangle::ASTMangler::getSpecialManglingContext(Type)) {
      switch (*context) {
      case Mangle::ASTMangler::ObjCContext:
        return {getAddrOfObjCModuleContextDescriptor(),
                ConstantReference::Direct};
      case Mangle::ASTMangler::ClangImporterContext:
        return {getAddrOfClangImporterModuleContextDescriptor(),
                ConstantReference::Direct};
      }
    }

    // Wrap up private types in an anonymous context for the containing file
    // unit so that the runtime knows they have unstable identity.
    if (Type->isOutermostPrivateOrFilePrivateScope())
      return {getAddrOfAnonymousContextDescriptor(Type),
              ConstantReference::Direct};
  }
  
  auto parent = from->getParent();
  
  switch (parent->getContextKind()) {
  case DeclContextKind::AbstractClosureExpr:
  case DeclContextKind::AbstractFunctionDecl:
  case DeclContextKind::SubscriptDecl:
  case DeclContextKind::TopLevelCodeDecl:
  case DeclContextKind::Initializer:
  case DeclContextKind::SerializedLocal:
    return {getAddrOfAnonymousContextDescriptor(from),
            ConstantReference::Direct};

  case DeclContextKind::GenericTypeDecl:
    if (auto nomTy = dyn_cast<NominalTypeDecl>(parent)) {
      return {getAddrOfTypeContextDescriptor(nomTy, DontRequireMetadata),
              ConstantReference::Direct};
    }
    return {getAddrOfAnonymousContextDescriptor(from),
            ConstantReference::Direct};

  case DeclContextKind::ExtensionDecl: {
    auto ext = cast<ExtensionDecl>(parent);
    // If the extension is equivalent to its extended context (that is, it's
    // in the same module as the original non-protocol type and
    // has no constraints), then we can use the original nominal type context
    // (assuming there is one).
    if (ext->isEquivalentToExtendedContext()) {
      auto nominal = ext->getExtendedType()->getAnyNominal();
      // If the extended type is an ObjC class, it won't have a nominal type
      // descriptor, so we'll just emit an extension context.
      auto clas = dyn_cast<ClassDecl>(nominal);
      if (!clas || clas->isForeign() || hasKnownSwiftMetadata(*this, clas)) {
        // Some targets don't support relative references to undefined symbols.
        // If the extension is in a different file from the original type
        // declaration, it may not get emitted in this TU. Use an indirect
        // reference to work around the object format limitation.
        auto shouldBeIndirect =
            parent->getModuleScopeContext() != from->getModuleScopeContext()
          ? ConstantReference::Indirect
          : ConstantReference::Direct;
        
        return getAddrOfLLVMVariableOrGOTEquivalent(
                                LinkEntity::forNominalTypeDescriptor(nominal),
                                Alignment(4),
                                TypeContextDescriptorTy,
                                shouldBeIndirect);
      }
    }
    return {getAddrOfExtensionContextDescriptor(ext),
            ConstantReference::Direct};
  }
      
  case DeclContextKind::FileUnit:
    parent = parent->getParentModule();
    LLVM_FALLTHROUGH;
      
  case DeclContextKind::Module:
    return {getAddrOfModuleContextDescriptor(cast<ModuleDecl>(parent)),
            ConstantReference::Direct};
  }
}

/// Add the given global value to @llvm.used.
///
/// This value must have a definition by the time the module is finalized.
void IRGenModule::addUsedGlobal(llvm::GlobalValue *global) {
  LLVMUsed.push_back(global);
}

/// Add the given global value to @llvm.compiler.used.
///
/// This value must have a definition by the time the module is finalized.
void IRGenModule::addCompilerUsedGlobal(llvm::GlobalValue *global) {
  LLVMCompilerUsed.push_back(global);
}

/// Add the given global value to the Objective-C class list.
void IRGenModule::addObjCClass(llvm::Constant *classPtr, bool nonlazy) {
  ObjCClasses.push_back(classPtr);
  if (nonlazy)
    ObjCNonLazyClasses.push_back(classPtr);
}

void IRGenModule::addRuntimeResolvableType(NominalTypeDecl *nominal) {
  // Collect the nominal type records we emit into a special section.
  RuntimeResolvableTypes.push_back(nominal);

  // As soon as the type metadata is available, all the type's conformances
  // must be available, too. The reason is that a type (with the help of its
  // metadata) can be checked at runtime if it conforms to a protocol.
  addLazyConformances(nominal);
}

ConstantReference
IRGenModule::getConstantReferenceForProtocolDescriptor(ProtocolDecl *proto) {
  if (proto->isObjC()) {
    // ObjC protocol descriptors don't have a unique address, but get uniqued
    // by the Objective-C runtime at load time.
    // Get the indirected address of the protocol descriptor reference variable
    // that the ObjC runtime uniques.
    auto refVar = getAddrOfObjCProtocolRef(proto, NotForDefinition);
    return ConstantReference(refVar, ConstantReference::Indirect);
  }
  
  // Try to form a direct reference to the nominal type descriptor if it's in
  // the same binary, or use the GOT entry if it's from another binary.
  return getAddrOfLLVMVariableOrGOTEquivalent(
                                     LinkEntity::forProtocolDescriptor(proto),
                                     getPointerAlignment(),
                                     ProtocolDescriptorStructTy);
}

llvm::Constant *IRGenModule::getAddrOfAssociatedTypeGenericParamRef(
                                                   GenericSignature *sig,
                                                   CanDependentMemberType dmt) {
  llvm::SmallVector<AssociatedTypeDecl *, 4> assocTypePath;

  // Get the base ordinal.
  auto baseDMT = dmt;
  CanType base;
  do {
    base = baseDMT.getBase();
    assocTypePath.push_back(baseDMT->getAssocType());
    baseDMT = dyn_cast<DependentMemberType>(base);
  } while (baseDMT);
  auto ordinal = sig->getGenericParamOrdinal(cast<GenericTypeParamType>(base));
  
  // Generate a symbol name for the descriptor. This is a private symbol, so
  // isn't ABI, but is useful for ODR coalescing within the same binary.
  IRGenMangler Mangler;
  auto symbolName = Mangler
    .mangleAssociatedTypeGenericParamRef(ordinal, dmt);
  
  // Use an existing definition if we have one.
  if (auto existingVar = Module.getGlobalVariable(symbolName))
    return existingVar;
  
  // Otherwise, build the reference path.
  ConstantInitBuilder builder(*this);
  auto B = builder.beginStruct();
  B.addInt32(ordinal << 1);

  for (auto *assocType : reversed(assocTypePath)) {
    auto proto = getConstantReferenceForProtocolDescriptor(
                                                      assocType->getProtocol());
    B.addRelativeAddress(proto);
    
    // Find the offset of the associated type entry in witness tables of this
    // protocol.
    auto &protoInfo = getProtocolInfo(assocType->getProtocol());
    auto index = protoInfo.getAssociatedTypeIndex(AssociatedType(assocType))
      .getValue();
    
    B.addInt32(index);
  }
  
  // Null terminator.
  B.addInt32(0);
  
  auto var = B.finishAndCreateGlobal(symbolName, Alignment(4),
                                     /*constant*/ true);
  var->setLinkage(llvm::GlobalValue::LinkOnceODRLinkage);
  var->setVisibility(llvm::GlobalValue::HiddenVisibility);
  setTrueConstGlobal(var);
  return var;
}

void IRGenModule::addLazyConformances(NominalTypeDecl *Nominal) {
  for (const ProtocolConformance *Conf : Nominal->getAllConformances()) {
    IRGen.addLazyWitnessTable(Conf);
  }
}

std::string IRGenModule::GetObjCSectionName(StringRef Section,
                                            StringRef MachOAttributes) {
  assert(Section.substr(0, 2) == "__" && "expected the name to begin with __");

  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("must know the object file format");
  case llvm::Triple::MachO:
    return MachOAttributes.empty()
               ? ("__DATA," + Section).str()
               : ("__DATA," + Section + "," + MachOAttributes).str();
  case llvm::Triple::ELF:
    return Section.substr(2).str();
  case llvm::Triple::COFF:
    return ("." + Section.substr(2) + "$B").str();
  case llvm::Triple::Wasm:
    error(SourceLoc(), "wasm is not a supported object file format");
  }

  llvm_unreachable("unexpected object file format");
}

void IRGenModule::SetCStringLiteralSection(llvm::GlobalVariable *GV,
                                           ObjCLabelType Type) {
  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("must know the object file format");
  case llvm::Triple::MachO:
    switch (Type) {
    case ObjCLabelType::ClassName:
      GV->setSection("__TEXT,__objc_classname,cstring_literals");
      return;
    case ObjCLabelType::MethodVarName:
      GV->setSection("__TEXT,__objc_methname,cstring_literals");
      return;
    case ObjCLabelType::MethodVarType:
      GV->setSection("__TEXT,__objc_methtype,cstring_literals");
      return;
    case ObjCLabelType::PropertyName:
      GV->setSection("__TEXT,__cstring,cstring_literals");
      return;
    }
  case llvm::Triple::ELF:
    return;
  case llvm::Triple::COFF:
    return;
  case llvm::Triple::Wasm:
    error(SourceLoc(), "wasm is not a supported object file format");
    return;
  }

  llvm_unreachable("unexpected object file format");
}

void IRGenModule::emitGlobalLists() {
  if (ObjCInterop) {
    // Objective-C class references go in a variable with a meaningless
    // name but a magic section.
    emitGlobalList(*this, ObjCClasses, "objc_classes",
                   GetObjCSectionName("__objc_classlist",
                                      "regular,no_dead_strip"),
                   llvm::GlobalValue::InternalLinkage, Int8PtrTy, false);

    // So do categories.
    emitGlobalList(*this, ObjCCategories, "objc_categories",
                   GetObjCSectionName("__objc_catlist",
                                      "regular,no_dead_strip"),
                   llvm::GlobalValue::InternalLinkage, Int8PtrTy, false);

    // Emit nonlazily realized class references in a second magic section to make
    // sure they are realized by the Objective-C runtime before any instances
    // are allocated.
    emitGlobalList(*this, ObjCNonLazyClasses, "objc_non_lazy_classes",
                   GetObjCSectionName("__objc_nlclslist",
                                      "regular,no_dead_strip"),
                   llvm::GlobalValue::InternalLinkage, Int8PtrTy, false);
  }

  // @llvm.used

  // Collect llvm.used globals already in the module (coming from ClangCodeGen).
  collectGlobalList(*this, LLVMUsed, "llvm.used");
  emitGlobalList(*this, LLVMUsed, "llvm.used", "llvm.metadata",
                 llvm::GlobalValue::AppendingLinkage,
                 Int8PtrTy,
                 false);

  // Collect llvm.compiler.used globals already in the module (coming
  // from ClangCodeGen).
  collectGlobalList(*this, LLVMCompilerUsed, "llvm.compiler.used");
  emitGlobalList(*this, LLVMCompilerUsed, "llvm.compiler.used", "llvm.metadata",
                 llvm::GlobalValue::AppendingLinkage,
                 Int8PtrTy,
                 false);
}

static bool hasCodeCoverageInstrumentation(SILFunction &f, SILModule &m) {
  return f.getProfiler() && m.getOptions().EmitProfileCoverageMapping;
}

void IRGenerator::emitGlobalTopLevel(bool emitForParallelEmission) {
  // Generate order numbers for the functions in the SIL module that
  // correspond to definitions in the LLVM module.
  unsigned nextOrderNumber = 0;
  for (auto &silFn : PrimaryIGM->getSILModule().getFunctions()) {
    // Don't bother adding external declarations to the function order.
    if (!silFn.isDefinition()) continue;
    FunctionOrder.insert(std::make_pair(&silFn, nextOrderNumber++));
  }

  // Ensure that relative symbols are collocated in the same LLVM module.
  for (SILWitnessTable &wt : PrimaryIGM->getSILModule().getWitnessTableList()) {
    CurrentIGMPtr IGM = getGenModule(wt.getConformance()->getDeclContext());
    if (emitForParallelEmission)
      IGM->ensureRelativeSymbolCollocation(wt);
  }

  for (SILGlobalVariable &v : PrimaryIGM->getSILModule().getSILGlobals()) {
    Decl *decl = v.getDecl();
    CurrentIGMPtr IGM = getGenModule(decl ? decl->getDeclContext() : nullptr);
    IGM->emitSILGlobalVariable(&v);
  }
  
  // Emit SIL functions.
  for (SILFunction &f : PrimaryIGM->getSILModule()) {
    // Eagerly emit functions that are externally visible. Functions with code
    // coverage instrumentation must also be eagerly emitted.
    if (!f.isPossiblyUsedExternally() &&
        !hasCodeCoverageInstrumentation(f, PrimaryIGM->getSILModule()))
      continue;

    CurrentIGMPtr IGM = getGenModule(&f);
    IGM->emitSILFunction(&f);
  }

  // Emit static initializers.
  for (auto Iter : *this) {
    IRGenModule *IGM = Iter.second;
    IGM->emitSILStaticInitializers();
  }

  // Emit witness tables.
  for (SILWitnessTable &wt : PrimaryIGM->getSILModule().getWitnessTableList()) {
    CurrentIGMPtr IGM = getGenModule(wt.getConformance()->getDeclContext());
    if (!canEmitWitnessTableLazily(&wt)) {
      IGM->emitSILWitnessTable(&wt);
    }
  }
  
  // Emit property descriptors.
  for (auto &prop : PrimaryIGM->getSILModule().getPropertyList()) {
    CurrentIGMPtr IGM = getGenModule(prop.getDecl()->getInnermostDeclContext());
    IGM->emitSILProperty(&prop);
  }
  
  // Emit code coverage mapping data.
  PrimaryIGM->emitCoverageMapping();

  for (auto Iter : *this) {
    IRGenModule *IGM = Iter.second;
    IGM->finishEmitAfterTopLevel();
  }
}

void IRGenModule::finishEmitAfterTopLevel() {
  // Emit the implicit import of the swift standard library.
  if (DebugInfo) {
    std::pair<swift::Identifier, swift::SourceLoc> AccessPath[] = {
      { Context.StdlibModuleName, swift::SourceLoc() }
    };

    auto Imp = ImportDecl::create(Context,
                                  getSwiftModule(),
                                  SourceLoc(),
                                  ImportKind::Module, SourceLoc(),
                                  AccessPath);
    DebugInfo->emitImport(Imp);
  }
}

void IRGenerator::emitSwiftProtocols() {
  for (auto &m : *this) {
    m.second->emitSwiftProtocols();
  }
}

void IRGenerator::emitProtocolConformances() {
  for (auto &m : *this) {
    m.second->emitProtocolConformances();
  }
}

void IRGenerator::emitTypeMetadataRecords() {
  for (auto &m : *this) {
    m.second->emitTypeMetadataRecords();
  }
}

/// Emit any lazy definitions (of globals or functions or whatever
/// else) that we require.
void IRGenerator::emitLazyDefinitions() {
  while (!LazyTypeMetadata.empty() ||
         !LazyTypeContextDescriptors.empty() ||
         !LazyFunctionDefinitions.empty() ||
         !LazyFieldTypes.empty() ||
         !LazyWitnessTables.empty()) {

    while (!LazyFieldTypes.empty()) {
      auto info = LazyFieldTypes.pop_back_val();
      auto &IGM = *info.IGM;

      for (auto fieldType : info.fieldTypes) {
        if (fieldType->hasArchetype())
          continue;

        // All of the required attributes are going to be preserved
        // by field reflection metadata in the mangled name, so
        // there is no need to worry about ownership semantics here.
        if (auto refStorTy = dyn_cast<ReferenceStorageType>(fieldType))
          fieldType = refStorTy.getReferentType();

        // Make sure that all of the field type metadata is forced,
        // otherwise there might be a problem when fields are accessed
        // through reflection.
        (void)irgen::getOrCreateTypeMetadataAccessFunction(IGM, fieldType);
      }
    }

    // Emit any lazy type metadata we require.
    while (!LazyTypeMetadata.empty()) {
      NominalTypeDecl *type = LazyTypeMetadata.pop_back_val();
      auto &entry = LazyTypeGlobals.find(type)->second;
      assert(entry.IsLazy && entry.IsMetadataUsed && !entry.IsMetadataEmitted);
      entry.IsMetadataEmitted = true;
      CurrentIGMPtr IGM = getGenModule(type->getDeclContext());
      emitLazyTypeMetadata(*IGM.get(), type);
    }
    while (!LazyTypeContextDescriptors.empty()) {
      NominalTypeDecl *type = LazyTypeContextDescriptors.pop_back_val();
      auto &entry = LazyTypeGlobals.find(type)->second;
      assert(entry.IsLazy && entry.IsDescriptorUsed &&
             !entry.IsDescriptorEmitted);
      entry.IsDescriptorEmitted = true;
      CurrentIGMPtr IGM = getGenModule(type->getDeclContext());
      emitLazyTypeContextDescriptor(*IGM.get(), type,
                                    RequireMetadata_t(entry.IsMetadataUsed));
    }
    while (!LazyWitnessTables.empty()) {
      SILWitnessTable *wt = LazyWitnessTables.pop_back_val();
      CurrentIGMPtr IGM = getGenModule(wt->getConformance()->getDeclContext());
      IGM->emitSILWitnessTable(wt);
    }

    // Emit any lazy function definitions we require.
    while (!LazyFunctionDefinitions.empty()) {
      SILFunction *f = LazyFunctionDefinitions.pop_back_val();
      CurrentIGMPtr IGM = getGenModule(f);
      assert(!f->isPossiblyUsedExternally()
             && "function with externally-visible linkage emitted lazily?");
      IGM->emitSILFunction(f);
    }
  }
}

void IRGenerator::addLazyFunction(SILFunction *f) {
  // Add it to the queue if it hasn't already been put there.
  if (!LazilyEmittedFunctions.insert(f).second)
    return;

  LazyFunctionDefinitions.push_back(f);

  if (auto *dc = f->getDeclContext())
    if (dc->getParentSourceFile())
      return;

  if (CurrentIGM == nullptr)
    return;

  // Don't update the map if we already have an entry.
  DefaultIGMForFunction.insert(std::make_pair(f, CurrentIGM));
}

void IRGenerator::noteUseOfTypeGlobals(NominalTypeDecl *type,
                                       bool isUseOfMetadata,
                                       RequireMetadata_t requireMetadata) {
  // Try to create a new record of the fact that we used this type.
  auto insertResult = LazyTypeGlobals.try_emplace(type);
  auto &entry = insertResult.first->second;

  bool metadataWasUsed = entry.IsMetadataUsed;
  bool descriptorWasUsed = entry.IsDescriptorUsed;

  bool isNovelUseOfMetadata = false;
  bool isNovelUseOfDescriptor = false;

  // Flag that we have a use of the metadata if
  //   - the reference was directly to the metadata
  //   - the reference was to the descriptor, but it requested the emission
  //     of metadata
  if (!metadataWasUsed && (isUseOfMetadata || requireMetadata)) {
    if (metadataWasUsed) return;
    entry.IsMetadataUsed = true;
    isNovelUseOfMetadata = true;
  }

  if (!descriptorWasUsed && !isUseOfMetadata) {
    if (descriptorWasUsed) return;
    entry.IsDescriptorUsed = true;
    isNovelUseOfDescriptor = true;
  }

  // If the type isn't known to be lazy, don't mess around with the queues.
  if (!entry.IsLazy)
    return;

  // Enqueue metadata emission if we have a novel use of it.
  if (isNovelUseOfMetadata)
    LazyTypeMetadata.push_back(type);

  // Enqueue descriptor emission if we have a novel use of it or if we
  // need to re-emit it because we're suddenly using metadata for it.
  if (isNovelUseOfDescriptor ||
      (isNovelUseOfMetadata && entry.IsDescriptorEmitted)) {
    entry.IsDescriptorEmitted = false; // clear this in case it was true
    LazyTypeContextDescriptors.push_back(type);
  }
}

void IRGenerator::emitEagerClassInitialization() {
  if (ClassesForEagerInitialization.empty())
    return;

  // Emit the register function in the primary module.
  IRGenModule *IGM = getPrimaryIGM();

  llvm::Function *RegisterFn = llvm::Function::Create(
                                llvm::FunctionType::get(IGM->VoidTy, false),
                                llvm::GlobalValue::PrivateLinkage,
                                "_swift_eager_class_initialization");
  IGM->Module.getFunctionList().push_back(RegisterFn);
  IRGenFunction RegisterIGF(*IGM, RegisterFn);
  RegisterFn->setAttributes(IGM->constructInitialAttributes());
  RegisterFn->setCallingConv(IGM->DefaultCC);

  for (ClassDecl *CD : ClassesForEagerInitialization) {
    Type Ty = CD->getDeclaredType();
    llvm::Value *MetaData = RegisterIGF.emitTypeMetadataRef(getAsCanType(Ty));
    assert(CD->getAttrs().hasAttribute<StaticInitializeObjCMetadataAttr>());

    // Get the metadata to make sure that the class is registered. We need to 
    // add a use (empty inline asm instruction) for the metadata. Otherwise
    // llvm would optimize the metadata accessor call away because it's
    // defined as "readnone".
    llvm::FunctionType *asmFnTy =
      llvm::FunctionType::get(IGM->VoidTy, {MetaData->getType()},
                              false /* = isVarArg */);
    llvm::InlineAsm *inlineAsm =
      llvm::InlineAsm::get(asmFnTy, "", "r", true /* = SideEffects */);
    RegisterIGF.Builder.CreateAsmCall(inlineAsm, MetaData);
  }
  RegisterIGF.Builder.CreateRetVoid();

  // Add the registration function as a static initializer. We use a priority
  // slightly lower than used for C++ global constructors, so that the code is
  // executed before C++ global constructors (in case someone uses archives
  // from a C++ global constructor).
  llvm::appendToGlobalCtors(IGM->Module, RegisterFn, 60000, nullptr);
}

/// Emit symbols for eliminated dead methods, which can still be referenced
/// from other modules. This happens e.g. if a public class contains a (dead)
/// private method.
void IRGenModule::emitVTableStubs() {
  llvm::Function *stub = nullptr;
  for (auto I = getSILModule().zombies_begin();
           I != getSILModule().zombies_end(); ++I) {
    const SILFunction &F = *I;
    if (! F.isExternallyUsedSymbol())
      continue;
    
    if (!stub) {
      // Create a single stub function which calls swift_deletedMethodError().
      stub = llvm::Function::Create(llvm::FunctionType::get(VoidTy, false),
                                    llvm::GlobalValue::InternalLinkage,
                                    "_swift_dead_method_stub");
      stub->setAttributes(constructInitialAttributes());
      Module.getFunctionList().push_back(stub);
      stub->setCallingConv(DefaultCC);
      auto *entry = llvm::BasicBlock::Create(getLLVMContext(), "entry", stub);
      auto *errorFunc = getDeletedMethodErrorFn();
      llvm::CallInst::Create(errorFunc, ArrayRef<llvm::Value *>(), "", entry);
      new llvm::UnreachableInst(getLLVMContext(), entry);
    }

    // For each eliminated method symbol create an alias to the stub.
    auto *alias = llvm::GlobalAlias::create(llvm::GlobalValue::ExternalLinkage,
                                            F.getName(), stub);

    if (F.getEffectiveSymbolLinkage() == SILLinkage::Hidden)
      alias->setVisibility(llvm::GlobalValue::HiddenVisibility);

    if (useDllStorage())
      alias->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
  }
}

void IRGenModule::emitTypeVerifier() {
  // Look up the types to verify.
  
  SmallVector<CanType, 4> TypesToVerify;
  for (auto name : IRGen.Opts.VerifyTypeLayoutNames) {
    // Look up the name in the module.
    SmallVector<ValueDecl*, 1> lookup;
    swift::ModuleDecl *M = getSwiftModule();
    M->lookupMember(lookup, M, DeclName(Context.getIdentifier(name)),
                    Identifier());
    if (lookup.empty()) {
      Context.Diags.diagnose(SourceLoc(), diag::type_to_verify_not_found,
                             name);
      continue;
    }
    
    TypeDecl *typeDecl = nullptr;
    for (auto decl : lookup) {
      if (auto td = dyn_cast<TypeDecl>(decl)) {
        if (typeDecl) {
          Context.Diags.diagnose(SourceLoc(), diag::type_to_verify_ambiguous,
                                 name);
          goto next;
        }
        typeDecl = td;
        break;
      }
    }
    if (!typeDecl) {
      Context.Diags.diagnose(SourceLoc(), diag::type_to_verify_not_found, name);
      continue;
    }
    
    {
      auto type = typeDecl->getDeclaredInterfaceType();
      if (type->hasTypeParameter()) {
        Context.Diags.diagnose(SourceLoc(), diag::type_to_verify_dependent,
                               name);
        continue;
      }
      
      TypesToVerify.push_back(type->getCanonicalType());
    }
  next:;
  }
  if (TypesToVerify.empty())
    return;

  // Find the entry point.
  SILFunction *EntryPoint =
    getSILModule().lookUpFunction(SWIFT_ENTRY_POINT_FUNCTION);

  if (!EntryPoint)
    return;
  
  llvm::Function *EntryFunction = Module.getFunction(EntryPoint->getName());
  if (!EntryFunction)
    return;
  
  // Create a new function to contain our logic.
  auto fnTy = llvm::FunctionType::get(VoidTy, /*varArg*/ false);
  auto VerifierFunction = llvm::Function::Create(fnTy,
                                             llvm::GlobalValue::PrivateLinkage,
                                             "type_verifier",
                                             getModule());
  VerifierFunction->setAttributes(constructInitialAttributes());
  
  // Insert a call into the entry function.
  {
    llvm::BasicBlock *EntryBB = &EntryFunction->getEntryBlock();
    llvm::BasicBlock::iterator IP = EntryBB->getFirstInsertionPt();
    IRBuilder Builder(getLLVMContext(), DebugInfo);
    Builder.llvm::IRBuilderBase::SetInsertPoint(EntryBB, IP);
    if (DebugInfo)
      DebugInfo->setEntryPointLoc(Builder);
    Builder.CreateCall(VerifierFunction, {});
  }

  IRGenTypeVerifierFunction VerifierIGF(*this, VerifierFunction);
  VerifierIGF.emit(TypesToVerify);
}

/// Get SIL-linkage for something that's not required to be visible
/// and doesn't actually need to be uniqued.
static SILLinkage getNonUniqueSILLinkage(FormalLinkage linkage,
                                         ForDefinition_t forDefinition) {
  switch (linkage) {
  case FormalLinkage::PublicUnique:
  case FormalLinkage::PublicNonUnique:
    return (forDefinition ? SILLinkage::Shared : SILLinkage::PublicExternal);

  case FormalLinkage::HiddenUnique:
    return (forDefinition ? SILLinkage::Shared : SILLinkage::HiddenExternal);

  case FormalLinkage::Private:
    return SILLinkage::Private;
  }
  llvm_unreachable("bad formal linkage");
}

SILLinkage LinkEntity::getLinkage(ForDefinition_t forDefinition) const {
  // For when `this` is a protocol conformance of some kind.
  auto getLinkageAsConformance = [&] {
    return getLinkageForProtocolConformance(
        getProtocolConformance()->getRootNormalConformance(), forDefinition);
  };

  switch (getKind()) {
  case Kind::DispatchThunk:
  case Kind::DispatchThunkInitializer:
  case Kind::DispatchThunkAllocator: {
    auto *decl = getDecl();

    // Protocol requirements don't have their own access control
    if (auto *proto = dyn_cast<ProtocolDecl>(decl->getDeclContext()))
      decl = proto;

    return getSILLinkage(getDeclLinkage(decl), forDefinition);
  }

  // Most type metadata depend on the formal linkage of their type.
  case Kind::ValueWitnessTable: {
    auto type = getType();

    // Builtin types, (), () -> () and so on are in the runtime.
    if (!type.getAnyNominal())
      return getSILLinkage(FormalLinkage::PublicUnique, forDefinition);

    // Imported types.
    if (getTypeMetadataAccessStrategy(type) ==
          MetadataAccessStrategy::NonUniqueAccessor)
      return SILLinkage::Shared;

    // Everything else is only referenced inside its module.
    return SILLinkage::Private;
  }

  case Kind::TypeMetadataInstantiationCache:
  case Kind::TypeMetadataInstantiationFunction:
  case Kind::TypeMetadataCompletionFunction:
  case Kind::TypeMetadataPattern:
    return SILLinkage::Private;

  case Kind::TypeMetadataLazyCacheVariable: {
    auto type = getType();

    // Imported types, non-primitive structural types.
    if (getTypeMetadataAccessStrategy(type) ==
          MetadataAccessStrategy::NonUniqueAccessor)
      return SILLinkage::Shared;

    // Everything else is only referenced inside its module.
    return SILLinkage::Private;
  }

  case Kind::TypeMetadata:
    switch (getMetadataAddress()) {
    case TypeMetadataAddress::FullMetadata:
      // The full metadata object is private to the containing module.
      return SILLinkage::Private;
    case TypeMetadataAddress::AddressPoint: {
      auto *nominal = getType().getAnyNominal();
      return getSILLinkage(nominal
                           ? getDeclLinkage(nominal)
                           : FormalLinkage::PublicUnique,
                           forDefinition);
    }
    }
    llvm_unreachable("bad kind");

  // ...but we don't actually expose individual value witnesses (right now).
  case Kind::ValueWitness:
    return getNonUniqueSILLinkage(getDeclLinkage(getType().getAnyNominal()),
                                  forDefinition);

  // Foreign type metadata candidates are always shared; the runtime
  // does the uniquing.
  case Kind::ForeignTypeMetadataCandidate:
    return SILLinkage::Shared;

  case Kind::TypeMetadataAccessFunction:
    switch (getTypeMetadataAccessStrategy(getType())) {
    case MetadataAccessStrategy::PublicUniqueAccessor:
      return getSILLinkage(FormalLinkage::PublicUnique, forDefinition);
    case MetadataAccessStrategy::HiddenUniqueAccessor:
      return getSILLinkage(FormalLinkage::HiddenUnique, forDefinition);
    case MetadataAccessStrategy::PrivateAccessor:
      return getSILLinkage(FormalLinkage::Private, forDefinition);
    case MetadataAccessStrategy::NonUniqueAccessor:
      return SILLinkage::Shared;
    }
    llvm_unreachable("bad metadata access kind");

  case Kind::ObjCClassRef:
    return SILLinkage::Private;

  // Continuation prototypes need to be external or else LLVM will fret.
  case Kind::CoroutineContinuationPrototype:
    return SILLinkage::PublicExternal;


  case Kind::EnumCase: {
    auto *elementDecl = cast<EnumElementDecl>(getDecl());
    return getSILLinkage(getDeclLinkage(elementDecl), forDefinition);
  }

  case Kind::FieldOffset: {
    auto *varDecl = cast<VarDecl>(getDecl());

    auto linkage = getDeclLinkage(varDecl);

    // Resilient classes don't expose field offset symbols.
    if (cast<ClassDecl>(varDecl->getDeclContext())->isResilient()) {
      assert(linkage != FormalLinkage::PublicNonUnique &&
            "Cannot have a resilient class with non-unique linkage");

      if (linkage == FormalLinkage::PublicUnique)
        linkage = FormalLinkage::HiddenUnique;
    }

    return getSILLinkage(linkage, forDefinition);
  }

  case Kind::ObjCClass:
  case Kind::ObjCMetaclass:
  case Kind::SwiftMetaclassStub:
  case Kind::NominalTypeDescriptor:
  case Kind::PropertyDescriptor:
  case Kind::ClassMetadataBaseOffset:
  case Kind::ProtocolDescriptor:
    return getSILLinkage(getDeclLinkage(getDecl()), forDefinition);

  case Kind::ProtocolRequirementArray:
    return SILLinkage::Private;

  case Kind::DirectProtocolWitnessTable:
  case Kind::ProtocolWitnessTableAccessFunction:
  case Kind::ProtocolConformanceDescriptor:
    return getLinkageAsConformance();

  case Kind::ProtocolWitnessTablePattern:
  case Kind::ResilientProtocolWitnessTable:
    if (getLinkageAsConformance() == SILLinkage::Shared)
      return SILLinkage::Shared;
    return SILLinkage::Private;

  case Kind::ProtocolWitnessTableLazyAccessFunction:
  case Kind::ProtocolWitnessTableLazyCacheVariable: {
    auto *nominal = getType().getAnyNominal();
    assert(nominal);
    if (getDeclLinkage(nominal) == FormalLinkage::Private ||
        getLinkageAsConformance() == SILLinkage::Private) {
      return SILLinkage::Private;
    } else {
      return SILLinkage::Shared;
    }
  }

  case Kind::AssociatedTypeMetadataAccessFunction:
  case Kind::AssociatedTypeWitnessTableAccessFunction:
  case Kind::GenericProtocolWitnessTableCache:
  case Kind::GenericProtocolWitnessTableInstantiationFunction:
    return SILLinkage::Private;
  
  case Kind::SILFunction:
    return getSILFunction()->getEffectiveSymbolLinkage();
      
  case Kind::SILGlobalVariable:
    return getSILGlobalVariable()->getLinkage();

  case Kind::ReflectionBuiltinDescriptor:
  case Kind::ReflectionFieldDescriptor: {
    // Reflection descriptors for imported types have shared linkage,
    // since we may emit them in other TUs in the same module.
    if (auto *nominal = getType().getAnyNominal())
      if (getDeclLinkage(nominal) == FormalLinkage::PublicNonUnique)
        return SILLinkage::Shared;
    return SILLinkage::Private;
  }
  case Kind::ReflectionAssociatedTypeDescriptor:
    if (getLinkageAsConformance() == SILLinkage::Shared)
      return SILLinkage::Shared;
    return SILLinkage::Private;
      
  case Kind::ModuleDescriptor:
  case Kind::ExtensionDescriptor:
  case Kind::AnonymousDescriptor:
    return SILLinkage::Shared;
  }
  llvm_unreachable("bad link entity kind");
}

static bool isAvailableExternally(IRGenModule &IGM, const DeclContext *dc) {
  dc = dc->getModuleScopeContext();
  if (isa<ClangModuleUnit>(dc) ||
      dc == IGM.getSILModule().getAssociatedContext())
    return false;
  return true;
}

static bool isAvailableExternally(IRGenModule &IGM, const Decl *decl) {
  return isAvailableExternally(IGM, decl->getDeclContext());
}

static bool isAvailableExternally(IRGenModule &IGM, Type type) {
  if (auto decl = type->getAnyNominal())
    return isAvailableExternally(IGM, decl->getDeclContext());
  return true;
}

bool LinkEntity::isAvailableExternally(IRGenModule &IGM) const {
  switch (getKind()) {
  case Kind::DispatchThunk:
  case Kind::DispatchThunkInitializer:
  case Kind::DispatchThunkAllocator: {
    auto *decl = getDecl();

    // Protocol requirements don't have their own access control
    if (auto *proto = dyn_cast<ProtocolDecl>(decl->getDeclContext()))
      decl = proto;

    return ::isAvailableExternally(IGM, getDecl());
  }

  case Kind::ValueWitnessTable:
  case Kind::TypeMetadata:
    return ::isAvailableExternally(IGM, getType());

  case Kind::ForeignTypeMetadataCandidate:
    assert(!::isAvailableExternally(IGM, getType()));
    return false;

  case Kind::ObjCClass:
  case Kind::ObjCMetaclass:
    // FIXME: Removing this triggers a linker bug
    return true;

  case Kind::SwiftMetaclassStub:
  case Kind::ClassMetadataBaseOffset:
  case Kind::PropertyDescriptor:
  case Kind::NominalTypeDescriptor:
  case Kind::ProtocolDescriptor:
    return ::isAvailableExternally(IGM, getDecl());

  case Kind::EnumCase:
    return ::isAvailableExternally(IGM, getDecl());

  case Kind::DirectProtocolWitnessTable:
  case Kind::ProtocolConformanceDescriptor:
    return ::isAvailableExternally(IGM, getProtocolConformance()->getDeclContext());

  case Kind::ProtocolWitnessTablePattern:
  case Kind::ResilientProtocolWitnessTable:
  case Kind::ProtocolRequirementArray:
  case Kind::ObjCClassRef:
  case Kind::ModuleDescriptor:
  case Kind::ExtensionDescriptor:
  case Kind::AnonymousDescriptor:
  case Kind::TypeMetadataInstantiationCache:
  case Kind::TypeMetadataInstantiationFunction:
  case Kind::TypeMetadataCompletionFunction:
  case Kind::TypeMetadataPattern:
    return false;

  case Kind::ValueWitness:
  case Kind::TypeMetadataAccessFunction:
  case Kind::TypeMetadataLazyCacheVariable:
  case Kind::FieldOffset:
  case Kind::ProtocolWitnessTableAccessFunction:
  case Kind::ProtocolWitnessTableLazyAccessFunction:
  case Kind::ProtocolWitnessTableLazyCacheVariable:
  case Kind::AssociatedTypeMetadataAccessFunction:
  case Kind::AssociatedTypeWitnessTableAccessFunction:
  case Kind::GenericProtocolWitnessTableCache:
  case Kind::GenericProtocolWitnessTableInstantiationFunction:
  case Kind::SILFunction:
  case Kind::SILGlobalVariable:
  case Kind::ReflectionBuiltinDescriptor:
  case Kind::ReflectionFieldDescriptor:
  case Kind::ReflectionAssociatedTypeDescriptor:
  case Kind::CoroutineContinuationPrototype:
    llvm_unreachable("Relative reference to unsupported link entity");
  }
  llvm_unreachable("bad link entity kind");
}

static std::tuple<llvm::GlobalValue::LinkageTypes,
                  llvm::GlobalValue::VisibilityTypes,
                  llvm::GlobalValue::DLLStorageClassTypes>
getIRLinkage(const UniversalLinkageInfo &info, SILLinkage linkage,
             ForDefinition_t isDefinition,
             bool isWeakImported) {
#define RESULT(LINKAGE, VISIBILITY, DLL_STORAGE)                               \
  std::make_tuple(llvm::GlobalValue::LINKAGE##Linkage,                         \
                  llvm::GlobalValue::VISIBILITY##Visibility,                   \
                  llvm::GlobalValue::DLL_STORAGE##StorageClass)

  // Use protected visibility for public symbols we define on ELF.  ld.so
  // doesn't support relative relocations at load time, which interferes with
  // our metadata formats.  Default visibility should suffice for other object
  // formats.
  llvm::GlobalValue::VisibilityTypes PublicDefinitionVisibility =
      info.IsELFObject ? llvm::GlobalValue::ProtectedVisibility
                       : llvm::GlobalValue::DefaultVisibility;
  llvm::GlobalValue::DLLStorageClassTypes ExportedStorage =
      info.UseDLLStorage ? llvm::GlobalValue::DLLExportStorageClass
                         : llvm::GlobalValue::DefaultStorageClass;
  llvm::GlobalValue::DLLStorageClassTypes ImportedStorage =
      info.UseDLLStorage ? llvm::GlobalValue::DLLImportStorageClass
                         : llvm::GlobalValue::DefaultStorageClass;

  switch (linkage) {
  case SILLinkage::Public:
    return std::make_tuple(llvm::GlobalValue::ExternalLinkage,
                           PublicDefinitionVisibility, ExportedStorage);

  case SILLinkage::Shared:
  case SILLinkage::SharedExternal:
    return isDefinition ? RESULT(LinkOnceODR, Hidden, Default)
                        : RESULT(External, Hidden, Default);

  case SILLinkage::Hidden:
  case SILLinkage::PublicNonABI:
    return RESULT(External, Hidden, Default);

  case SILLinkage::Private: {
    auto linkage = info.needLinkerToMergeDuplicateSymbols()
                       ? llvm::GlobalValue::LinkOnceODRLinkage
                       : llvm::GlobalValue::InternalLinkage;
    auto visibility = info.shouldAllPrivateDeclsBeVisibleFromOtherFiles()
                          ? llvm::GlobalValue::HiddenVisibility
                          : llvm::GlobalValue::DefaultVisibility;
    return std::make_tuple(linkage, visibility,
                           llvm::GlobalValue::DefaultStorageClass);
  }

  case SILLinkage::PublicExternal: {
    if (isDefinition) {
      return std::make_tuple(llvm::GlobalValue::AvailableExternallyLinkage,
                             llvm::GlobalValue::DefaultVisibility,
                             ExportedStorage);
    }

    auto linkage = isWeakImported ? llvm::GlobalValue::ExternalWeakLinkage
                                  : llvm::GlobalValue::ExternalLinkage;
    return std::make_tuple(linkage, llvm::GlobalValue::DefaultVisibility,
                           ImportedStorage);
  }

  case SILLinkage::HiddenExternal:
  case SILLinkage::PrivateExternal:
    return std::make_tuple(isDefinition
                               ? llvm::GlobalValue::AvailableExternallyLinkage
                               : llvm::GlobalValue::ExternalLinkage,
                           llvm::GlobalValue::HiddenVisibility,
                           ImportedStorage);

  }

  llvm_unreachable("bad SIL linkage");
}

/// Given that we're going to define a global value but already have a
/// forward-declaration of it, update its linkage.
void irgen::updateLinkageForDefinition(IRGenModule &IGM,
                                       llvm::GlobalValue *global,
                                       const LinkEntity &entity) {
  // TODO: there are probably cases where we can avoid redoing the
  // entire linkage computation.
  UniversalLinkageInfo linkInfo(IGM);
  auto linkage =
      getIRLinkage(linkInfo, entity.getLinkage(ForDefinition),
                   ForDefinition, entity.isWeakImported(IGM.getSwiftModule()));
  global->setLinkage(std::get<0>(linkage));
  global->setVisibility(std::get<1>(linkage));
  global->setDLLStorageClass(std::get<2>(linkage));

  // Everything externally visible is considered used in Swift.
  // That mostly means we need to be good at not marking things external.
  //
  // Exclude "main", because it should naturally be used, and because adding it
  // to llvm.used leaves a dangling use when the REPL attempts to discard
  // intermediate mains.
  if (LinkInfo::isUsed(std::get<0>(linkage), std::get<1>(linkage),
                       std::get<2>(linkage)) &&
      global->getName() != SWIFT_ENTRY_POINT_FUNCTION)
    IGM.addUsedGlobal(global);
}

LinkInfo LinkInfo::get(IRGenModule &IGM, const LinkEntity &entity,
                       ForDefinition_t isDefinition) {
  return LinkInfo::get(IGM, IGM.getSwiftModule(), entity, isDefinition);
}

LinkInfo LinkInfo::get(const UniversalLinkageInfo &linkInfo,
                       ModuleDecl *swiftModule, const LinkEntity &entity,
                       ForDefinition_t isDefinition) {
  LinkInfo result;

  entity.mangle(result.Name);

  std::tie(result.Linkage, result.Visibility, result.DLLStorageClass) =
      getIRLinkage(linkInfo, entity.getLinkage(isDefinition),
                   isDefinition, entity.isWeakImported(swiftModule));

  result.ForDefinition = isDefinition;

  return result;
}

LinkInfo LinkInfo::get(const UniversalLinkageInfo &linkInfo,
                       StringRef name,
                       SILLinkage linkage,
                       ForDefinition_t isDefinition,
                       bool isWeakImported) {
  LinkInfo result;

  result.Name += name;
  std::tie(result.Linkage, result.Visibility, result.DLLStorageClass) =
    getIRLinkage(linkInfo, linkage,
                 isDefinition, isWeakImported);
  result.ForDefinition = isDefinition;
  return result;
}

static bool isPointerTo(llvm::Type *ptrTy, llvm::Type *objTy) {
  return cast<llvm::PointerType>(ptrTy)->getElementType() == objTy;
}

/// Get or create an LLVM function with these linkage rules.
llvm::Function *irgen::createFunction(IRGenModule &IGM,
                                      LinkInfo &linkInfo,
                                      const Signature &signature,
                                      llvm::Function *insertBefore,
                                      OptimizationMode FuncOptMode) {
  auto name = linkInfo.getName();

  llvm::Function *existing = IGM.Module.getFunction(name);
  if (existing) {
    if (isPointerTo(existing->getType(), signature.getType()))
      return cast<llvm::Function>(existing);

    IGM.error(SourceLoc(),
              "program too clever: function collides with existing symbol " +
                  name);

    // Note that this will implicitly unique if the .unique name is also taken.
    existing->setName(name + ".unique");
  }

  llvm::Function *fn =
    llvm::Function::Create(signature.getType(), linkInfo.getLinkage(), name);
  fn->setVisibility(linkInfo.getVisibility());
  fn->setDLLStorageClass(linkInfo.getDLLStorage());
  fn->setCallingConv(signature.getCallingConv());

  if (insertBefore) {
    IGM.Module.getFunctionList().insert(insertBefore->getIterator(), fn);
  } else {
    IGM.Module.getFunctionList().push_back(fn);
 }

  llvm::AttrBuilder initialAttrs;
  IGM.constructInitialFnAttributes(initialAttrs, FuncOptMode);
  // Merge initialAttrs with attrs.
  auto updatedAttrs =
    signature.getAttributes().addAttributes(IGM.getLLVMContext(),
                                      llvm::AttributeList::FunctionIndex,
                                            initialAttrs);
  if (!updatedAttrs.isEmpty())
    fn->setAttributes(updatedAttrs);

  // Everything externally visible is considered used in Swift.
  // That mostly means we need to be good at not marking things external.
  //
  // Exclude "main", because it should naturally be used, and because adding it
  // to llvm.used leaves a dangling use when the REPL attempts to discard
  // intermediate mains.
  if (linkInfo.isUsed() && name != SWIFT_ENTRY_POINT_FUNCTION) {
    IGM.addUsedGlobal(fn);
  }

  return fn;
}

bool LinkInfo::isUsed(llvm::GlobalValue::LinkageTypes Linkage,
                      llvm::GlobalValue::VisibilityTypes Visibility,
                      llvm::GlobalValue::DLLStorageClassTypes DLLStorage) {
  // Everything externally visible is considered used in Swift.
  // That mostly means we need to be good at not marking things external.
  return Linkage == llvm::GlobalValue::ExternalLinkage &&
         (Visibility == llvm::GlobalValue::DefaultVisibility ||
          Visibility == llvm::GlobalValue::ProtectedVisibility) &&
         (DLLStorage == llvm::GlobalValue::DefaultStorageClass ||
          DLLStorage == llvm::GlobalValue::DLLExportStorageClass);
}

/// Get or create an LLVM global variable with these linkage rules.
llvm::GlobalVariable *swift::irgen::createVariable(
    IRGenModule &IGM, LinkInfo &linkInfo, llvm::Type *storageType,
    Alignment alignment, DebugTypeInfo DbgTy, Optional<SILLocation> DebugLoc,
    StringRef DebugName, bool inFixedBuffer, bool indirectForDebugInfo) {
  auto name = linkInfo.getName();
  llvm::GlobalValue *existingValue = IGM.Module.getNamedGlobal(name);
  if (existingValue) {
    auto existingVar = dyn_cast<llvm::GlobalVariable>(existingValue);
    if (existingVar && isPointerTo(existingVar->getType(), storageType))
      return existingVar;

    IGM.error(SourceLoc(),
              "program too clever: variable collides with existing symbol " +
                  name);

    // Note that this will implicitly unique if the .unique name is also taken.
    existingValue->setName(name + ".unique");
  }

  auto var = new llvm::GlobalVariable(IGM.Module, storageType,
                                      /*constant*/ false, linkInfo.getLinkage(),
                                      /*initializer*/ nullptr, name);
  var->setVisibility(linkInfo.getVisibility());
  var->setDLLStorageClass(linkInfo.getDLLStorage());
  var->setAlignment(alignment.getValue());

  // Everything externally visible is considered used in Swift.
  // That mostly means we need to be good at not marking things external.
  if (linkInfo.isUsed()) {
    IGM.addUsedGlobal(var);
  }

  if (IGM.DebugInfo && !DbgTy.isNull() && linkInfo.isForDefinition())
    IGM.DebugInfo->emitGlobalVariableDeclaration(
        var, DebugName.empty() ? name : DebugName, name, DbgTy,
        var->hasInternalLinkage(), indirectForDebugInfo, DebugLoc);

  return var;
}

void swift::irgen::disableAddressSanitizer(IRGenModule &IGM, llvm::GlobalVariable *var) {
  // Add an operand to llvm.asan.globals blacklisting this global variable.
  llvm::Metadata *metadata[] = {
    // The global variable to blacklist.
    llvm::ConstantAsMetadata::get(var),
    
    // Source location. Optional, unnecessary here.
    nullptr,
    
    // Name. Optional, unnecessary here.
    nullptr,
    
    // Whether the global is dynamically initialized.
    llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
      llvm::Type::getInt1Ty(IGM.Module.getContext()), false)),
    
    // Whether the global is blacklisted.
    llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
      llvm::Type::getInt1Ty(IGM.Module.getContext()), true))};
  
  auto *globalNode = llvm::MDNode::get(IGM.Module.getContext(), metadata);
  auto *asanMetadata = IGM.Module.getOrInsertNamedMetadata("llvm.asan.globals");
  asanMetadata->addOperand(globalNode);
}

/// Emit a global declaration.
void IRGenModule::emitGlobalDecl(Decl *D) {
  switch (D->getKind()) {
  case DeclKind::Extension:
    return emitExtension(cast<ExtensionDecl>(D));

  case DeclKind::Protocol:
    return emitProtocolDecl(cast<ProtocolDecl>(D));

  case DeclKind::PatternBinding:
    // The global initializations are in SIL.
    return;

  case DeclKind::Param:
    llvm_unreachable("there are no global function parameters");

  case DeclKind::Subscript:
    llvm_unreachable("there are no global subscript operations");
      
  case DeclKind::EnumCase:
  case DeclKind::EnumElement:
    llvm_unreachable("there are no global enum elements");

  case DeclKind::Constructor:
    llvm_unreachable("there are no global constructor");

  case DeclKind::Destructor:
    llvm_unreachable("there are no global destructor");

  case DeclKind::MissingMember:
    llvm_unreachable("there are no global member placeholders");

  case DeclKind::TypeAlias:
  case DeclKind::GenericTypeParam:
  case DeclKind::AssociatedType:
  case DeclKind::IfConfig: 
  case DeclKind::PoundDiagnostic:
    return;

  case DeclKind::Enum:
    return emitEnumDecl(cast<EnumDecl>(D));

  case DeclKind::Struct:
    return emitStructDecl(cast<StructDecl>(D));

  case DeclKind::Class:
    return emitClassDecl(cast<ClassDecl>(D));

  // These declarations are only included in the debug info.
  case DeclKind::Import:
    if (DebugInfo)
      DebugInfo->emitImport(cast<ImportDecl>(D));
    return;

  // We emit these as part of the PatternBindingDecl.
  case DeclKind::Var:
    return;

  case DeclKind::Func:
  case DeclKind::Accessor:
    // Handled in SIL.
    return;

  case DeclKind::TopLevelCode:
    // All the top-level code will be lowered separately.
    return;
      
  // Operator decls aren't needed for IRGen.
  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
  case DeclKind::PrecedenceGroup:
    return;

  case DeclKind::Module:
    return;
  }

  llvm_unreachable("bad decl kind!");
}

Address IRGenModule::getAddrOfSILGlobalVariable(SILGlobalVariable *var,
                                                const TypeInfo &ti,
                                                ForDefinition_t forDefinition) {
  if (auto clangDecl = var->getClangDecl()) {
    auto addr = getAddrOfClangGlobalDecl(cast<clang::VarDecl>(clangDecl),
                                         forDefinition);

    // If we're not emitting this to define it, make sure we cast it to the
    // right type.
    if (!forDefinition) {
      auto ptrTy = ti.getStorageType()->getPointerTo();
      addr = llvm::ConstantExpr::getBitCast(addr, ptrTy);
    }

    auto alignment =
      Alignment(getClangASTContext().getDeclAlign(clangDecl).getQuantity());
    return Address(addr, alignment);
  }

  LinkEntity entity = LinkEntity::forSILGlobalVariable(var);
  ResilienceExpansion expansion = getResilienceExpansionForLayout(var);

  llvm::Type *storageType;
  Size fixedSize;
  Alignment fixedAlignment;
  bool inFixedBuffer = false;
  bool indirectForDebugInfo = false;

  // FIXME: Remove this once LLDB has proper support for resilience.
  bool isREPLVar = false;
  if (auto *decl = var->getDecl())
    if (decl->isREPLVar())
      isREPLVar = true;

  if (var->isInitializedObject()) {
    assert(ti.isFixedSize(expansion));
    StructLayout *Layout = StaticObjectLayouts[var].get();
    if (!Layout) {
      // Create the layout (includes the llvm type) for the statically
      // initialized object and store it for later.
      ObjectInst *OI = cast<ObjectInst>(var->getStaticInitializerValue());
      llvm::SmallVector<SILType, 16> TailTypes;
      for (SILValue TailOp : OI->getTailElements()) {
        TailTypes.push_back(TailOp->getType());
      }
      Layout = getClassLayoutWithTailElems(*this,
                                           var->getLoweredType(), TailTypes);
      StaticObjectLayouts[var] = std::unique_ptr<StructLayout>(Layout);
    }
    storageType = Layout->getType();
    fixedSize = Layout->getSize();
    fixedAlignment = Layout->getAlignment();
    assert(fixedAlignment >= TargetInfo.HeapObjectAlignment);
  } else if (isREPLVar || ti.isFixedSize(expansion)) {
    // Allocate static storage.
    auto &fixedTI = cast<FixedTypeInfo>(ti);
    storageType = fixedTI.getStorageType();
    fixedSize = fixedTI.getFixedSize();
    fixedAlignment = fixedTI.getFixedAlignment();
  } else {
    // Allocate a fixed-size buffer and possibly heap-allocate a payload at
    // runtime if the runtime size of the type does not fit in the buffer.
    inFixedBuffer = true;
    storageType = getFixedBufferTy();
    fixedSize = Size(DataLayout.getTypeAllocSize(storageType));
    fixedAlignment = Alignment(DataLayout.getABITypeAlignment(storageType));

    // DebugInfo is not resilient for now, so disable resilience to figure out
    // if lldb needs to dereference the global variable or not.
    //
    // FIXME: Once lldb can make use of remote mirrors to calculate layouts
    // at runtime, this should be removed.
    {
      CompletelyFragileScope Scope(*this);

      SILType loweredTy = var->getLoweredType();
      auto &nonResilientTI = cast<FixedTypeInfo>(getTypeInfo(loweredTy));
      auto packing = nonResilientTI.getFixedPacking(*this);
      switch (packing) {
      case FixedPacking::OffsetZero:
        break;
      case FixedPacking::Allocate:
        indirectForDebugInfo = true;
        break;
      default:
        llvm_unreachable("Bad packing");
      }
    }
  }

  // Check whether we've created the global variable already.
  // FIXME: We should integrate this into the LinkEntity cache more cleanly.
  auto gvar = Module.getGlobalVariable(var->getName(), /*allowInternal*/ true);
  if (gvar) {
    if (forDefinition)
      updateLinkageForDefinition(*this, gvar, entity);
  } else {
    LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
    llvm::Type *storageTypeWithContainer = storageType;
    if (var->isInitializedObject()) {
      // A statically initialized object must be placed into a container struct
      // because the swift_initStaticObject needs a swift_once_t at offset -1:
      //     struct Container {
      //       swift_once_t token[fixedAlignment / sizeof(swift_once_t)];
      //       HeapObject object;
      //     };
      std::string typeName = storageType->getStructName().str() + 'c';
      assert(fixedAlignment >= getPointerAlignment());
      unsigned numTokens = fixedAlignment.getValue() /
        getPointerAlignment().getValue();
      storageTypeWithContainer = llvm::StructType::create(getLLVMContext(),
              {llvm::ArrayType::get(OnceTy, numTokens), storageType}, typeName);
      gvar = createVariable(*this, link, storageTypeWithContainer,
                            fixedAlignment);
    } else {
      StringRef name;
      Optional<SILLocation> loc;
      if (var->getDecl()) {
        // Use the VarDecl for more accurate debugging information.
        loc = var->getDecl();
        name = var->getDecl()->getName().str();
      } else {
        if (var->hasLocation())
          loc = var->getLocation();
        name = var->getName();
      }
      auto DbgTy = DebugTypeInfo::getGlobal(var, storageTypeWithContainer,
                                            fixedSize, fixedAlignment);
      gvar = createVariable(*this, link, storageTypeWithContainer,
                            fixedAlignment, DbgTy, loc, name, inFixedBuffer,
                            indirectForDebugInfo);
    }
    /// Add a zero initializer.
    if (forDefinition)
      gvar->setInitializer(llvm::Constant::getNullValue(storageTypeWithContainer));
  }
  llvm::Constant *addr = gvar;
  if (var->isInitializedObject()) {
    // Project out the object from the container.
    llvm::Constant *Indices[2] = {
      llvm::ConstantExpr::getIntegerValue(Int32Ty, APInt(32, 0)),
      llvm::ConstantExpr::getIntegerValue(Int32Ty, APInt(32, 1))
    };
    // Return the address of the initialized object itself (and not the address
    // to a reference to it).
    addr = llvm::ConstantExpr::getGetElementPtr(nullptr, gvar, Indices);
  }
  addr = llvm::ConstantExpr::getBitCast(addr, storageType->getPointerTo());
  return Address(addr, Alignment(gvar->getAlignment()));
}

/// Return True if the function \p f is a 'readonly' function. Checking
/// for the SIL @effects(readonly) attribute is not enough because this
/// definition does not match the definition of the LLVM readonly function
/// attribute. In this function we do the actual check.
static bool isReadOnlyFunction(SILFunction *f) {
  // Check if the function has any 'owned' parameters. Owned parameters may
  // call the destructor of the object which could violate the readonly-ness
  // of the function.
  if (f->hasOwnedParameters() || f->hasIndirectFormalResults())
    return false;

  auto Eff = f->getEffectsKind();

  // Swift's readonly does not automatically match LLVM's readonly.
  // Swift SIL optimizer relies on @effects(readonly) to remove e.g.
  // dead code remaining from initializers of strings or dictionaries
  // of variables that are not used. But those initializers are often
  // not really readonly in terms of LLVM IR. For example, the
  // Dictionary.init() is marked as @effects(readonly) in Swift, but
  // it does invoke reference-counting operations.
  if (Eff == EffectsKind::ReadOnly || Eff == EffectsKind::ReadNone) {
    // TODO: Analyze the body of function f and return true if it is
    // really readonly.
    return false;
  }

  return false;
}

static clang::GlobalDecl getClangGlobalDeclForFunction(const clang::Decl *decl) {
  if (auto ctor = dyn_cast<clang::CXXConstructorDecl>(decl))
    return clang::GlobalDecl(ctor, clang::Ctor_Complete);
  if (auto dtor = dyn_cast<clang::CXXDestructorDecl>(decl))
    return clang::GlobalDecl(dtor, clang::Dtor_Complete);
  return clang::GlobalDecl(cast<clang::FunctionDecl>(decl));
}

/// Find the entry point for a SIL function.
llvm::Function *IRGenModule::getAddrOfSILFunction(SILFunction *f,
                                                  ForDefinition_t forDefinition) {
  LinkEntity entity = LinkEntity::forSILFunction(f);

  // Check whether we've created the function already.
  // FIXME: We should integrate this into the LinkEntity cache more cleanly.
  llvm::Function *fn = Module.getFunction(f->getName());  
  if (fn) {
    if (forDefinition) updateLinkageForDefinition(*this, fn, entity);
    return fn;
  }

  // If it's a Clang declaration, ask Clang to generate the IR declaration.
  // This might generate new functions, so we should do it before computing
  // the insert-before point.
  llvm::Constant *clangAddr = nullptr;
  if (auto clangDecl = f->getClangDecl()) {
    auto globalDecl = getClangGlobalDeclForFunction(clangDecl);
    clangAddr = getAddrOfClangGlobalDecl(globalDecl, forDefinition);
  }

  bool isDefinition = f->isDefinition();
  bool hasOrderNumber = isDefinition;
  unsigned orderNumber = ~0U;
  llvm::Function *insertBefore = nullptr;

  // If the SIL function has a definition, we should have an order
  // number for it; make sure to insert it in that position relative
  // to other ordered functions.
  if (hasOrderNumber) {
    orderNumber = IRGen.getFunctionOrder(f);
    if (auto emittedFunctionIterator
          = EmittedFunctionsByOrder.findLeastUpperBound(orderNumber))
      insertBefore = *emittedFunctionIterator;
  }

  // If it's a Clang declaration, check whether Clang gave us a declaration.
  if (clangAddr) {
    fn = dyn_cast<llvm::Function>(clangAddr->stripPointerCasts());

    // If we have a function, move it to the appropriate position.
    if (fn) {
      if (hasOrderNumber) {
        auto &fnList = Module.getFunctionList();
        fnList.remove(fn);
        fnList.insert(llvm::Module::iterator(insertBefore), fn);

        EmittedFunctionsByOrder.insert(orderNumber, fn);
      }
      return fn;
    }

  // Otherwise, if we have a lazy definition for it, be sure to queue that up.
  } else if (isDefinition && !forDefinition && !f->isPossiblyUsedExternally() &&
             !hasCodeCoverageInstrumentation(*f, getSILModule())) {
    IRGen.addLazyFunction(f);
  }

  Signature signature = getSignature(f->getLoweredFunctionType());
  auto &attrs = signature.getMutableAttributes();

  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);

  switch (f->getInlineStrategy()) {
  case NoInline:
    attrs = attrs.addAttribute(signature.getType()->getContext(),
                               llvm::AttributeList::FunctionIndex,
                               llvm::Attribute::NoInline);
    break;
  case AlwaysInline:
    // FIXME: We do not currently transfer AlwaysInline since doing so results
    // in test failures, which must be investigated first.
  case InlineDefault:
    break;
  }

  if (isReadOnlyFunction(f)) {
    attrs = attrs.addAttribute(signature.getType()->getContext(),
                               llvm::AttributeList::FunctionIndex,
                               llvm::Attribute::ReadOnly);
  }
  fn = createFunction(*this, link, signature, insertBefore,
                      f->getOptimizationMode());

  // If we have an order number for this function, set it up as appropriate.
  if (hasOrderNumber) {
    EmittedFunctionsByOrder.insert(orderNumber, fn);
  }
  return fn;
}

static llvm::GlobalVariable *createGOTEquivalent(IRGenModule &IGM,
                                                 llvm::Constant *global,
                                                 StringRef globalName) {
  if (IGM.Triple.getObjectFormat() == llvm::Triple::COFF) {
    if (cast<llvm::GlobalValue>(global)->hasDLLImportStorageClass()) {
      llvm::GlobalVariable *GV =
          new llvm::GlobalVariable(IGM.Module, global->getType(),
                                   /*Constant=*/true,
                                   llvm::GlobalValue::ExternalLinkage,
                                   nullptr, llvm::Twine("__imp_") + globalName);
      GV->setExternallyInitialized(true);
      return GV;
    }
  }

  auto gotEquivalent = new llvm::GlobalVariable(IGM.Module,
                                      global->getType(),
                                      /*constant*/ true,
                                      llvm::GlobalValue::PrivateLinkage,
                                      global,
                                      llvm::Twine("got.") + globalName);
  gotEquivalent->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  return gotEquivalent;
}

static llvm::Constant *getElementBitCast(llvm::Constant *ptr,
                                         llvm::Type *newEltType) {
  auto ptrType = cast<llvm::PointerType>(ptr->getType());
  if (ptrType->getElementType() == newEltType) {
    return ptr;
  } else {
    auto newPtrType = newEltType->getPointerTo(ptrType->getAddressSpace());
    return llvm::ConstantExpr::getBitCast(ptr, newPtrType);
  }
}

/// Return a reference to an object that's suitable for being used for
/// the given kind of reference.
///
/// Note that, if the requested reference kind is a relative reference.
/// the returned constant will not actually be a relative reference.
/// To form the actual relative reference, you must pass the returned
/// result to emitRelativeReference, passing the correct base-address
/// information.
ConstantReference
IRGenModule::getAddrOfLLVMVariable(LinkEntity entity, Alignment alignment,
                                   ConstantInit definition,
                                   llvm::Type *defaultType,
                                   DebugTypeInfo debugType,
                                   SymbolReferenceKind refKind) {
  switch (refKind) {
  case SymbolReferenceKind::Relative_Direct:
  case SymbolReferenceKind::Far_Relative_Direct:
    assert(!definition);
    // FIXME: don't just fall through; force the creation of a weak
    // definition so that we can emit a relative reference.
    LLVM_FALLTHROUGH;

  case SymbolReferenceKind::Absolute:
    return { getAddrOfLLVMVariable(entity, alignment, definition,
                                   defaultType, debugType),
             ConstantReference::Direct };


  case SymbolReferenceKind::Relative_Indirectable:
  case SymbolReferenceKind::Far_Relative_Indirectable:
    assert(!definition);
    return getAddrOfLLVMVariableOrGOTEquivalent(entity, alignment, defaultType);
  }
  llvm_unreachable("bad reference kind");
}

/// A convenient wrapper around getAddrOfLLVMVariable which uses the
/// default type as the definition type.
llvm::Constant *
IRGenModule::getAddrOfLLVMVariable(LinkEntity entity, Alignment alignment,
                                   ForDefinition_t forDefinition,
                                   llvm::Type *defaultType,
                                   DebugTypeInfo debugType) {
  auto definition =
    (forDefinition ? ConstantInit::getDelayed(defaultType) : ConstantInit());
  return getAddrOfLLVMVariable(entity, alignment, definition,
                               defaultType, debugType);
}

/// Get or create an llvm::GlobalVariable.
///
/// If a definition type is given, the result will always be an
/// llvm::GlobalVariable of that type.  Otherwise, the result will
/// have type pointerToDefaultType and may involve bitcasts.
llvm::Constant *
IRGenModule::getAddrOfLLVMVariable(LinkEntity entity, Alignment alignment,
                                   ConstantInit definition,
                                   llvm::Type *defaultType,
                                   DebugTypeInfo DbgTy) {
  // This function assumes that 'globals' only contains GlobalValue
  // values for the entities that it will look up.

  llvm::Type *definitionType = (definition ? definition.getType() : nullptr);

  auto &entry = GlobalVars[entity];
  if (entry) {
    auto existing = cast<llvm::GlobalValue>(entry);

    // If we're looking to define something, we may need to replace a
    // forward declaration.
    if (definitionType) {
      assert(existing->isDeclaration() && "already defined");
      assert(entry->getType()->getPointerElementType() == defaultType
          || entry->getType()->getPointerElementType() == definition.getType());
      updateLinkageForDefinition(*this, existing, entity);

      // If the existing entry is a variable of the right type,
      // set the initializer on it and return.
      if (auto var = dyn_cast<llvm::GlobalVariable>(existing)) {
        if (definitionType == var->getValueType()) {
          if (definition.hasInit())
            definition.getInit().installInGlobal(var);
          return var;
        }
      }

      // Fall out to the case below, clearing the name so that
      // createVariable doesn't detect a collision.
      entry->setName("");

    // Otherwise, we have a previous declaration or definition which
    // we need to ensure has the right type.
    } else {
      return getElementBitCast(entry, defaultType);
    }
  }

  ForDefinition_t forDefinition = (ForDefinition_t) (definitionType != nullptr);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);

  // Clang may have defined the variable already.
  if (auto existing = Module.getNamedGlobal(link.getName()))
    return getElementBitCast(existing, defaultType);

  // If we're not defining the object now, forward declare it with the default
  // type.
  if (!definitionType) definitionType = defaultType;

  // Create the variable.
  auto var = createVariable(*this, link, definitionType, alignment, DbgTy);

  // Install the concrete definition if we have one.
  if (definition && definition.hasInit()) {
    definition.getInit().installInGlobal(var);
  }

  // If we have an existing entry, destroy it, replacing it with the
  // new variable.
  if (entry) {
    auto existing = cast<llvm::GlobalValue>(entry);
    auto castVar = getElementBitCast(var, defaultType);
    existing->replaceAllUsesWith(castVar);
    existing->eraseFromParent();
  }

  // If there's also an existing GOT-equivalent entry, rewrite it too, since
  // LLVM won't recognize a global with bitcasts in its initializers as GOT-
  // equivalent. rdar://problem/22388190
  auto foundGOTEntry = GlobalGOTEquivalents.find(entity);
  if (foundGOTEntry != GlobalGOTEquivalents.end() && foundGOTEntry->second) {
    auto existingGOTEquiv = cast<llvm::GlobalVariable>(foundGOTEntry->second);

    // Make a new GOT equivalent referring to the new variable with its
    // definition type.
    auto newGOTEquiv = createGOTEquivalent(*this, var, var->getName());
    auto castGOTEquiv = llvm::ConstantExpr::getBitCast(newGOTEquiv,
                                                   existingGOTEquiv->getType());
    existingGOTEquiv->replaceAllUsesWith(castGOTEquiv);
    existingGOTEquiv->eraseFromParent();
    GlobalGOTEquivalents[entity] = newGOTEquiv;
  }

  // Cache and return.
  entry = var;
  return var;
}

/// Get or create a "GOT equivalent" llvm::GlobalVariable, if applicable.
///
/// Creates a private, unnamed constant containing the address of another
/// global variable. LLVM can replace relative references to this variable with
/// relative references to the GOT entry for the variable in the object file.
ConstantReference
IRGenModule::getAddrOfLLVMVariableOrGOTEquivalent(LinkEntity entity,
                              Alignment alignment,
                              llvm::Type *defaultType,
                              ConstantReference::Directness forceIndirectness) {
  // ObjC class references can always be directly referenced, even in
  // the weird cases where we don't see a definition.
  if (entity.isObjCClassRef()) {
    auto value = getAddrOfObjCClassRef(
      const_cast<ClassDecl *>(cast<ClassDecl>(entity.getDecl())));
    return { cast<llvm::Constant>(value.getAddress()),
             ConstantReference::Direct };
  }

  // Ensure the variable is at least forward-declared.
  if (entity.isForeignTypeMetadataCandidate()) {
    auto foreignCandidate
      = getAddrOfForeignTypeMetadataCandidate(entity.getType());
    (void)foreignCandidate;
  } else {
    getAddrOfLLVMVariable(entity, alignment, ConstantInit(),
                          defaultType, DebugTypeInfo());
  }

  // Guess whether a global entry is a definition from this TU. This isn't
  // bulletproof, but at the point we emit conformance tables, we're far enough
  // along that we should have emitted any metadata objects we were going to.
  auto isDefinition = [&](llvm::Constant *global) -> bool {
    // We only emit aliases for definitions. (An extern alias would be an
    // extern global.)
    if (isa<llvm::GlobalAlias>(global))
      return true;
    // Global vars are definitions if they have an initializer.
    if (auto var = dyn_cast<llvm::GlobalVariable>(global))
      return var->hasInitializer();
    // Assume anything else isn't a definition.
    return false;
  };

  // If the variable has already been defined in this TU,
  // then it definitely doesn't need a GOT entry, and we can
  // relative-reference it directly.
  //
  // TODO: If we know the target entry is going to be linked into the same
  // binary, then we ought to be able to directly relative-reference the
  // symbol. However, some platforms don't have the necessary relocations to
  // represent a relative reference to an undefined symbol, so conservatively
  // produce an indirect reference in this case. Also, the integrated REPL
  // incrementally adds new definitions that refer back to existing ones
  // relatively, so always use indirect references in this situation.
  auto entry = GlobalVars[entity];
  if (forceIndirectness == ConstantReference::Direct &&
      !IRGen.Opts.IntegratedREPL &&
      (!entity.isAvailableExternally(*this) || isDefinition(entry))) {
    // FIXME: Relative references to aliases break MC on 32-bit Mach-O
    // platforms (rdar://problem/22450593 ), so substitute an alias with its
    // aliasee to work around that.
    if (auto alias = dyn_cast<llvm::GlobalAlias>(entry))
      entry = alias->getAliasee();
    return {entry, ConstantReference::Direct};
  }

  auto &gotEntry = GlobalGOTEquivalents[entity];
  if (gotEntry) {
    return {gotEntry, ConstantReference::Indirect};
  }

  // Look up the global variable.
  auto global = cast<llvm::GlobalValue>(entry);
  // Use it as the initializer for an anonymous constant. LLVM can treat this as
  // equivalent to the global's GOT entry.
  llvm::SmallString<64> name;
  entity.mangle(name);
  auto gotEquivalent = createGOTEquivalent(*this, global, name);
  gotEntry = gotEquivalent;
  return {gotEquivalent, ConstantReference::Indirect};
}

/// Get or create a "GOT equivalent" llvm::GlobalVariable, if applicable.
///
/// Creates a private, unnamed constant containing the address of another
/// function. LLVM can replace relative references to this variable with
/// relative references to the GOT entry for the function in the object file.
ConstantReference
IRGenModule::getFunctionGOTEquivalent(LinkEntity entity,
                                      llvm::Function *func) {
  auto &gotEntry = GlobalGOTEquivalents[entity];
  if (gotEntry) {
    return {gotEntry, ConstantReference::Indirect};
  }

  // Use it as the initializer for an anonymous constant. LLVM can treat this as
  // equivalent to the global's GOT entry.
  llvm::SmallString<64> name;
  entity.mangle(name);
  auto gotEquivalent = createGOTEquivalent(*this, func, name);
  gotEntry = gotEquivalent;
  return {gotEquivalent, ConstantReference::Indirect};
}

TypeEntityReference
IRGenModule::getTypeEntityReference(NominalTypeDecl *decl) {
  TypeMetadataRecordKind kind;
  Optional<LinkEntity> entity;
  llvm::Type *defaultTy;

  auto clas = dyn_cast<ClassDecl>(decl);
  if (clas && !clas->isForeign() && !hasKnownSwiftMetadata(*this, clas)) {
    // A reference to an Objective-C class object.
    assert(clas->isObjC() && "Must have an Objective-C class here");

    kind = TypeMetadataRecordKind::IndirectObjCClass;
    defaultTy = TypeMetadataPtrTy;
    entity = LinkEntity::forObjCClassRef(clas);
  } else {
    // A reference to a concrete type.
    // TODO: consider using a symbolic reference (i.e. a symbol string
    // to be looked up dynamically) for types defined outside the module.
    kind = TypeMetadataRecordKind::DirectNominalTypeDescriptor;
    entity = LinkEntity::forNominalTypeDescriptor(decl);
    defaultTy = TypeContextDescriptorTy;
  }

  auto ref = getAddrOfLLVMVariableOrGOTEquivalent(
      *entity, getPointerAlignment(), defaultTy);

  // Adjust the flags now that we know whether the reference to this
  // entity will be indirect.
  if (ref.isIndirect()) {
    assert(kind == TypeMetadataRecordKind::DirectNominalTypeDescriptor);
    kind = TypeMetadataRecordKind::IndirectNominalTypeDescriptor;
  }

  return TypeEntityReference(kind, ref.getValue());
}

/// Form an LLVM constant for the relative distance between a reference
/// (appearing at gep (0, indices) of `base`) and `target`.
llvm::Constant *
IRGenModule::emitRelativeReference(ConstantReference target,
                                   llvm::Constant *base,
                                   ArrayRef<unsigned> baseIndices) {
  llvm::Constant *relativeAddr =
    emitDirectRelativeReference(target.getValue(), base, baseIndices);

  // If the reference is indirect, flag it by setting the low bit.
  // (All of the base, direct target, and GOT entry need to be pointer-aligned
  // for this to be OK.)
  if (target.isIndirect()) {
    relativeAddr = llvm::ConstantExpr::getAdd(relativeAddr,
                             llvm::ConstantInt::get(RelativeAddressTy, 1));
  }

  return relativeAddr;
}

/// Form an LLVM constant for the relative distance between a reference
/// (appearing at gep (0, indices...) of `base`) and `target`.  For now,
/// for this to succeed portably, both need to be globals defined in the
/// current translation unit.
llvm::Constant *
IRGenModule::emitDirectRelativeReference(llvm::Constant *target,
                                         llvm::Constant *base,
                                         ArrayRef<unsigned> baseIndices) {
  // Convert the target to an integer.
  auto targetAddr = llvm::ConstantExpr::getPtrToInt(target, SizeTy);

  SmallVector<llvm::Constant*, 4> indices;
  indices.push_back(llvm::ConstantInt::get(Int32Ty, 0));
  for (unsigned baseIndex : baseIndices) {
    indices.push_back(llvm::ConstantInt::get(Int32Ty, baseIndex));
  };

  // Drill down to the appropriate address in the base, then convert
  // that to an integer.
  auto baseElt = llvm::ConstantExpr::getInBoundsGetElementPtr(
                       base->getType()->getPointerElementType(), base, indices);
  auto baseAddr = llvm::ConstantExpr::getPtrToInt(baseElt, SizeTy);

  // The relative address is the difference between those.
  auto relativeAddr = llvm::ConstantExpr::getSub(targetAddr, baseAddr);

  // Relative addresses can be 32-bit even on 64-bit platforms.
  if (SizeTy != RelativeAddressTy)
    relativeAddr = llvm::ConstantExpr::getTrunc(relativeAddr,
                                                RelativeAddressTy);

  return relativeAddr;
}

/// Emit the protocol descriptors list and return it.
llvm::Constant *IRGenModule::emitSwiftProtocols() {
  if (SwiftProtocols.empty())
    return nullptr;

  // Define the global variable for the protocol list.
  ConstantInitBuilder builder(*this);
  auto recordsArray = builder.beginArray(ProtocolRecordTy);

  for (auto *protocol : SwiftProtocols) {
    auto record = recordsArray.beginStruct(ProtocolRecordTy);

    // Relative reference to the protocol descriptor.
    auto descriptorRef = getAddrOfLLVMVariableOrGOTEquivalent(
                  LinkEntity::forProtocolDescriptor(protocol),
                  getPointerAlignment(), ProtocolDescriptorStructTy);
    record.addRelativeAddress(descriptorRef);

    record.finishAndAddTo(recordsArray);
  }

  // FIXME: This needs to be a linker-local symbol in order for Darwin ld to
  // resolve relocations relative to it.

  auto var = recordsArray.finishAndCreateGlobal(
                                            "\x01l_protocols",
                                            Alignment(4),
                                            /*isConstant*/ true,
                                            llvm::GlobalValue::PrivateLinkage);

  StringRef sectionName;
  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::MachO:
    sectionName = "__TEXT, __swift4_protos, regular, no_dead_strip";
    break;
  case llvm::Triple::ELF:
    sectionName = "swift4_protocols";
    break;
  case llvm::Triple::COFF:
    sectionName = ".sw5prt$B";
    break;
  default:
    llvm_unreachable("Don't know how to emit protocols for "
                     "the selected object format.");
  }

  var->setSection(sectionName);
  
  disableAddressSanitizer(*this, var);
  
  addUsedGlobal(var);
  return var;
}

namespace {
  /// Builds a protocol conformance descriptor.
  class ProtocolConformanceDescriptorBuilder {
    IRGenModule &IGM;
    ConstantStructBuilder &B;
    const NormalProtocolConformance *Conformance;
    ConformanceFlags Flags;

  public:
    ProtocolConformanceDescriptorBuilder(
                                 IRGenModule &IGM,
                                 ConstantStructBuilder &B,
                                 const NormalProtocolConformance *conformance)
    : IGM(IGM), B(B), Conformance(conformance) { }

    void layout() {
      addProtocol();
      addConformingType();
      addWitnessTable();
      addFlags();
      addContext();
      addConditionalRequirements();

      B.suggestType(IGM.ProtocolConformanceDescriptorTy);
    }

    void addProtocol() {
      // Relative reference to the protocol descriptor.
      auto protocol = Conformance->getProtocol();
      auto descriptorRef = IGM.getAddrOfLLVMVariableOrGOTEquivalent(
                    LinkEntity::forProtocolDescriptor(protocol),
                    IGM.getPointerAlignment(), IGM.ProtocolDescriptorStructTy);
      B.addRelativeAddress(descriptorRef);
    }

    void addConformingType() {
      // Add a relative reference to the type, with the type reference
      // kind stored in the flags.
      auto ref =
        IGM.getTypeEntityReference(Conformance->getType()->getAnyNominal());
      B.addRelativeAddress(ref.getValue());
      Flags = Flags.withTypeReferenceKind(ref.getKind());
    }

    void addWitnessTable() {
      using ConformanceKind = ConformanceFlags::ConformanceKind;

      // Figure out what kind of witness table we have.
      llvm::Constant *witnessTableVar;

      if (Conformance->getConditionalRequirements().empty()) {
        if (!isDependentConformance(Conformance)) {
          Flags = Flags.withConformanceKind(ConformanceKind::WitnessTable);
          witnessTableVar = IGM.getAddrOfWitnessTable(Conformance);
        } else {
          Flags = Flags.withConformanceKind(
                                        ConformanceKind::WitnessTableAccessor);
          witnessTableVar = IGM.getAddrOfWitnessTableAccessFunction(
              Conformance, ForDefinition);
        }
      } else {
        Flags =
          Flags.withConformanceKind(
              ConformanceKind::ConditionalWitnessTableAccessor)
            .withNumConditionalRequirements(
              Conformance->getConditionalRequirements().size());

        witnessTableVar = IGM.getAddrOfWitnessTableAccessFunction(
          Conformance, ForDefinition);
      }

      // Relative reference to the witness table.
      auto witnessTableRef =
        ConstantReference(witnessTableVar, ConstantReference::Direct);
      B.addRelativeAddress(witnessTableRef);
    }

    void addFlags() {
      // Miscellaneous flags.
      Flags = Flags.withIsRetroactive(Conformance->isRetroactive());
      Flags =
        Flags.withIsSynthesizedNonUnique(Conformance->isSynthesizedNonUnique());

      // Add the flags.
      B.addInt32(Flags.getIntValue());
    }

    void addContext() {
      if (!Conformance->isRetroactive())
        return;

      auto moduleContext =
        Conformance->getDeclContext()->getModuleScopeContext();
      ConstantReference moduleContextRef =
        IGM.getAddrOfParentContextDescriptor(moduleContext);
      B.addRelativeAddress(moduleContextRef);
    }

    void addConditionalRequirements() {
      if (Conformance->getConditionalRequirements().empty())
        return;

      auto nominal = Conformance->getType()->getAnyNominal();
      irgen::addGenericRequirements(IGM, B,
        nominal->getGenericSignatureOfContext(),
        Conformance->getConditionalRequirements());
    }
  };
}

void IRGenModule::emitProtocolConformance(
                                const NormalProtocolConformance *conformance) {
  // Emit additional metadata to be used by reflection.
  emitAssociatedTypeMetadataRecord(conformance);

  // Form the protocol conformance descriptor.
  ConstantInitBuilder initBuilder(*this);
  auto init = initBuilder.beginStruct();
  ProtocolConformanceDescriptorBuilder builder(*this, init, conformance);
  builder.layout();

  auto var =
    cast<llvm::GlobalVariable>(
          getAddrOfProtocolConformanceDescriptor(conformance,
                                                 init.finishAndCreateFuture()));
  var->setConstant(true);
}

void IRGenModule::addProtocolConformance(
                                const NormalProtocolConformance *conformance) {
  // Add this protocol conformance.
  ProtocolConformances.push_back(conformance);
}

/// Emit the protocol conformance list and return it.
llvm::Constant *IRGenModule::emitProtocolConformances() {
  // Do nothing if the list is empty.
  if (ProtocolConformances.empty())
    return nullptr;

  // Define the global variable for the conformance list.

  ConstantInitBuilder builder(*this);
  auto descriptorArray = builder.beginArray(RelativeAddressTy);

  for (auto *conformance : ProtocolConformances) {
    // Emit the protocol conformance now.
    emitProtocolConformance(conformance);

    auto entity = LinkEntity::forProtocolConformanceDescriptor(conformance);
    auto descriptor =
      getAddrOfLLVMVariable(entity, getPointerAlignment(), ConstantInit(),
                            ProtocolConformanceDescriptorTy, DebugTypeInfo());
    descriptorArray.addRelativeAddress(descriptor);
  }

  // FIXME: This needs to be a linker-local symbol in order for Darwin ld to
  // resolve relocations relative to it.

  auto var = descriptorArray.finishAndCreateGlobal(
                                          "\x01l_protocol_conformances",
                                          Alignment(4),
                                          /*isConstant*/ true,
                                          llvm::GlobalValue::PrivateLinkage);

  StringRef sectionName;
  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::MachO:
    sectionName = "__TEXT, __swift4_proto, regular, no_dead_strip";
    break;
  case llvm::Triple::ELF:
    sectionName = "swift4_protocol_conformances";
    break;
  case llvm::Triple::COFF:
    sectionName = ".sw5prtc$B";
    break;
  default:
    llvm_unreachable("Don't know how to emit protocol conformances for "
                     "the selected object format.");
  }

  var->setSection(sectionName);

  disableAddressSanitizer(*this, var);
  
  addUsedGlobal(var);
  return var;
}


/// Emit type metadata for types that might not have explicit protocol conformances.
llvm::Constant *IRGenModule::emitTypeMetadataRecords() {
  std::string sectionName;
  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::MachO:
    sectionName = "__TEXT, __swift4_types, regular, no_dead_strip";
    break;
  case llvm::Triple::ELF:
    sectionName = "swift4_type_metadata";
    break;
  case llvm::Triple::COFF:
    sectionName = ".sw5tymd$B";
    break;
  default:
    llvm_unreachable("Don't know how to emit type metadata table for "
                     "the selected object format.");
  }

  // Do nothing if the list is empty.
  if (RuntimeResolvableTypes.empty())
    return nullptr;

  // Define the global variable for the conformance list.
  // We have to do this before defining the initializer since the entries will
  // contain offsets relative to themselves.
  auto arrayTy = llvm::ArrayType::get(TypeMetadataRecordTy,
                                      RuntimeResolvableTypes.size());

  // FIXME: This needs to be a linker-local symbol in order for Darwin ld to
  // resolve relocations relative to it.
  auto var = new llvm::GlobalVariable(Module, arrayTy,
                                      /*isConstant*/ true,
                                      llvm::GlobalValue::PrivateLinkage,
                                      /*initializer*/ nullptr,
                                      "\x01l_type_metadata_table");

  SmallVector<llvm::Constant *, 8> elts;
  for (auto type : RuntimeResolvableTypes) {
    auto ref = getTypeEntityReference(type);

    // Form the relative address, with the type refernce kind in the low bits.
    unsigned arrayIdx = elts.size();
    llvm::Constant *relativeAddr =
      emitDirectRelativeReference(ref.getValue(), var, { arrayIdx, 0 });
    unsigned lowBits = static_cast<unsigned>(ref.getKind());
    if (lowBits != 0) {
      relativeAddr = llvm::ConstantExpr::getAdd(relativeAddr,
                       llvm::ConstantInt::get(RelativeAddressTy, lowBits));
    }

    llvm::Constant *recordFields[] = { relativeAddr };
    auto record = llvm::ConstantStruct::get(TypeMetadataRecordTy,
                                            recordFields);
    elts.push_back(record);
  }

  auto initializer = llvm::ConstantArray::get(arrayTy, elts);

  var->setInitializer(initializer);
  var->setSection(sectionName);
  var->setAlignment(4);

  disableAddressSanitizer(*this, var);
  
  addUsedGlobal(var);
  return var;
}

llvm::Constant *IRGenModule::emitFieldDescriptors() {
  std::string sectionName;
  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::MachO:
    sectionName = "__TEXT, __swift4_fieldmd, regular, no_dead_strip";
    break;
  case llvm::Triple::ELF:
    sectionName = "swift4_fieldmd";
    break;
  case llvm::Triple::COFF:
    sectionName = ".swift4_fieldmd";
    break;
  default:
    llvm_unreachable("Don't know how to emit field records table for "
                     "the selected object format.");
  }

  // Do nothing if the list is empty.
  if (FieldDescriptors.empty())
    return nullptr;

  // Define the global variable for the field record list.
  // We have to do this before defining the initializer since the entries will
  // contain offsets relative to themselves.
  auto arrayTy =
      llvm::ArrayType::get(FieldDescriptorPtrTy, FieldDescriptors.size());

  // FIXME: This needs to be a linker-local symbol in order for Darwin ld to
  // resolve relocations relative to it.
  auto var = new llvm::GlobalVariable(
      Module, arrayTy,
      /*isConstant*/ true, llvm::GlobalValue::PrivateLinkage,
      /*initializer*/ nullptr, "\x01l_type_metadata_table");

  SmallVector<llvm::Constant *, 8> elts;
  for (auto *descriptor : FieldDescriptors)
    elts.push_back(
        llvm::ConstantExpr::getBitCast(descriptor, FieldDescriptorPtrTy));

  var->setInitializer(llvm::ConstantArray::get(arrayTy, elts));
  var->setSection(sectionName);
  var->setAlignment(4);
  
  disableAddressSanitizer(*this, var);
  
  addUsedGlobal(var);
  return var;
}

/// Fetch a global reference to a reference to the given Objective-C class.
/// The result is of type ObjCClassPtrTy->getPointerTo().
Address IRGenModule::getAddrOfObjCClassRef(ClassDecl *theClass) {
  assert(ObjCInterop && "getting address of ObjC class ref in no-interop mode");

  Alignment alignment = getPointerAlignment();

  LinkEntity entity = LinkEntity::forObjCClassRef(theClass);
  auto DbgTy = DebugTypeInfo::getObjCClass(
      theClass, ObjCClassPtrTy, getPointerSize(), getPointerAlignment());
  auto addr = getAddrOfLLVMVariable(entity, alignment, ConstantInit(),
                                    ObjCClassPtrTy, DbgTy);

  // Define it lazily.
  if (auto global = dyn_cast<llvm::GlobalVariable>(addr)) {
    if (global->isDeclaration()) {
      global->setSection(GetObjCSectionName("__objc_classrefs",
                                            "regular,no_dead_strip"));
      global->setLinkage(llvm::GlobalVariable::PrivateLinkage);
      global->setExternallyInitialized(true);
      global->setInitializer(getAddrOfObjCClass(theClass, NotForDefinition));
      addCompilerUsedGlobal(global);
    }
  }

  return Address(addr, alignment);
}

/// Fetch a global reference to the given Objective-C class.  The
/// result is of type ObjCClassPtrTy.
llvm::Constant *IRGenModule::getAddrOfObjCClass(ClassDecl *theClass,
                                                ForDefinition_t forDefinition) {
  assert(ObjCInterop && "getting address of ObjC class in no-interop mode");
  assert(!theClass->isForeign());
  LinkEntity entity = LinkEntity::forObjCClass(theClass);
  auto DbgTy = DebugTypeInfo::getObjCClass(
      theClass, ObjCClassPtrTy, getPointerSize(), getPointerAlignment());
  auto addr = getAddrOfLLVMVariable(entity, getPointerAlignment(),
                                    forDefinition, ObjCClassStructTy, DbgTy);
  return addr;
}

/// Fetch the declaration of a metaclass object. The result is always a
/// GlobalValue of ObjCClassPtrTy, and is either the Objective-C metaclass or
/// the Swift metaclass stub, depending on whether the class is published as an
/// ObjC class.
llvm::Constant *
IRGenModule::getAddrOfMetaclassObject(ClassDecl *decl,
                                      ForDefinition_t forDefinition) {
  assert((!decl->isGenericContext() || decl->hasClangNode()) &&
         "generic classes do not have a static metaclass object");

  auto entity = decl->getMetaclassKind() == ClassDecl::MetaclassKind::ObjC
                    ? LinkEntity::forObjCMetaclass(decl)
                    : LinkEntity::forSwiftMetaclassStub(decl);

  auto DbgTy = DebugTypeInfo::getObjCClass(
      decl, ObjCClassPtrTy, getPointerSize(), getPointerAlignment());
  auto addr = getAddrOfLLVMVariable(entity, getPointerAlignment(),
                                    forDefinition, ObjCClassStructTy, DbgTy);
  return addr;
}

/// Fetch the type metadata access function for a non-generic type.
llvm::Function *
IRGenModule::getAddrOfTypeMetadataAccessFunction(CanType type,
                                              ForDefinition_t forDefinition) {
  assert(!type->hasArchetype() && !type->hasTypeParameter());
  NominalTypeDecl *Nominal = type->getNominalOrBoundGenericNominal();
  IRGen.noteUseOfTypeMetadata(Nominal);

  LinkEntity entity = LinkEntity::forTypeMetadataAccessFunction(type);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  llvm::Type *params[] = { SizeTy }; // MetadataRequest
  auto fnType = llvm::FunctionType::get(TypeMetadataResponseTy, params, false);
  Signature signature(fnType, llvm::AttributeList(), SwiftCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Fetch the type metadata access function for the given generic type.
llvm::Function *
IRGenModule::getAddrOfGenericTypeMetadataAccessFunction(
                                           NominalTypeDecl *nominal,
                                           ArrayRef<llvm::Type *> genericArgs,
                                           ForDefinition_t forDefinition) {
  assert(nominal->isGenericContext());
  assert(!genericArgs.empty() ||
         nominal->getGenericSignature()->areAllParamsConcrete());
  IRGen.noteUseOfTypeMetadata(nominal);

  auto type = nominal->getDeclaredType()->getCanonicalType();
  assert(type->hasUnboundGenericType());
  LinkEntity entity = LinkEntity::forTypeMetadataAccessFunction(type);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  // If we have more arguments than can be passed directly, all of the
  // generic arguments are passed as an array.
  llvm::Type *paramTypesArray[NumDirectGenericTypeMetadataAccessFunctionArgs+1];

  paramTypesArray[0] = SizeTy; // MetadataRequest
  size_t numParams = 1;

  size_t numGenericArgs = genericArgs.size();
  if (numGenericArgs > NumDirectGenericTypeMetadataAccessFunctionArgs) {
    paramTypesArray[1] = Int8PtrPtrTy;
    numParams++;
  } else {
    for (size_t i : indices(genericArgs))
      paramTypesArray[i + 1] = genericArgs[i];
    numParams += numGenericArgs;
  }

  auto paramTypes = llvm::makeArrayRef(paramTypesArray, numParams);
  auto fnType = llvm::FunctionType::get(TypeMetadataResponseTy,
                                        paramTypes, false);
  Signature signature(fnType, llvm::AttributeList(), SwiftCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Get or create a type metadata cache variable.  These are an
/// implementation detail of type metadata access functions.
llvm::Constant *
IRGenModule::getAddrOfTypeMetadataLazyCacheVariable(CanType type,
                                              ForDefinition_t forDefinition) {
  assert(!type->hasArchetype() && !type->hasTypeParameter());
  LinkEntity entity = LinkEntity::forTypeMetadataLazyCacheVariable(type);
  return getAddrOfLLVMVariable(entity, getPointerAlignment(), forDefinition,
                               TypeMetadataPtrTy, DebugTypeInfo());
}

/// Define the metadata for a type.
///
/// Some type metadata has information before the address point that the
/// public symbol for the metadata references. This function will rewrite any
/// existing external declaration to the address point as an alias into the
/// full metadata object.
llvm::GlobalValue *IRGenModule::defineTypeMetadata(CanType concreteType,
                                 bool isIndirect,
                                 bool isPattern,
                                 bool isConstant,
                                 ConstantInitFuture init,
                                 llvm::StringRef section) {
  assert(init);
  assert(!isIndirect && "indirect type metadata not used yet");

  if (isPattern) {
    auto addr = getAddrOfTypeMetadataPattern(concreteType->getAnyNominal(),
                                             isConstant, init, section);

    return cast<llvm::GlobalValue>(addr);
  }

  /// For concrete metadata, we want to use the initializer on the
  /// "full metadata", and define the "direct" address point as an alias.
  TypeMetadataAddress addrKind;
  llvm::Type *defaultVarTy;
  unsigned adjustmentIndex;
  Alignment alignment = getPointerAlignment();

  if (concreteType->getClassOrBoundGenericClass()) {
    addrKind = TypeMetadataAddress::FullMetadata;
    defaultVarTy = FullHeapMetadataStructTy;
    adjustmentIndex = MetadataAdjustmentIndex::Class;
  } else {
    addrKind = TypeMetadataAddress::FullMetadata;
    defaultVarTy = FullTypeMetadataStructTy;
    adjustmentIndex = MetadataAdjustmentIndex::ValueType;
  }

  auto entity = LinkEntity::forTypeMetadata(concreteType, addrKind);

  auto DbgTy = DebugTypeInfo::getMetadata(MetatypeType::get(concreteType),
                                          defaultVarTy->getPointerTo(), Size(0),
                                          Alignment(1));

  // Define the variable.
  llvm::GlobalVariable *var = cast<llvm::GlobalVariable>(
      getAddrOfLLVMVariable(entity, alignment, init, defaultVarTy, DbgTy));

  var->setConstant(isConstant);
  if (!section.empty())
    var->setSection(section);

  // Keep type metadata around for all types.
  if (auto nominal = concreteType->getAnyNominal())
    addRuntimeResolvableType(nominal);

  // For concrete metadata, declare the alias to its address point.
  auto directEntity = LinkEntity::forTypeMetadata(concreteType,
                                              TypeMetadataAddress::AddressPoint);

  llvm::Constant *addr = var;
  // Do an adjustment if necessary.
  if (adjustmentIndex) {
    llvm::Constant *indices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, adjustmentIndex)
    };
    addr = llvm::ConstantExpr::getInBoundsGetElementPtr(/*Ty=*/nullptr,
                                                        addr, indices);
  }
  addr = llvm::ConstantExpr::getBitCast(addr, TypeMetadataPtrTy);

  // Check for an existing forward declaration of the address point.
  auto &directEntry = GlobalVars[directEntity];
  llvm::GlobalValue *existingVal = nullptr;
  if (directEntry) {
    existingVal = cast<llvm::GlobalValue>(directEntry);
    // Clear the existing value's name so we can steal it.
    existingVal->setName("");
  }

  LinkInfo link = LinkInfo::get(*this, directEntity, ForDefinition);
  auto *ptrTy = cast<llvm::PointerType>(addr->getType());
  auto *alias = llvm::GlobalAlias::create(
      ptrTy->getElementType(), ptrTy->getAddressSpace(), link.getLinkage(),
      link.getName(), addr, &Module);
  alias->setVisibility(link.getVisibility());
  alias->setDLLStorageClass(link.getDLLStorage());

  // The full metadata is used based on the visibility of the address point,
  // not the metadata itself.
  if (link.isUsed()) {
    addUsedGlobal(var);
    addUsedGlobal(alias);
  }

  // Replace an existing external declaration for the address point.
  if (directEntry) {
    auto existingVal = cast<llvm::GlobalValue>(directEntry);

    // FIXME: MC breaks when emitting alias references on some platforms
    // (rdar://problem/22450593 ). Work around this by referring to the aliasee
    // instead.
    llvm::Constant *aliasCast = alias->getAliasee();
    aliasCast = llvm::ConstantExpr::getBitCast(aliasCast,
                                               directEntry->getType());
    existingVal->replaceAllUsesWith(aliasCast);
    existingVal->eraseFromParent();
  }
  directEntry = alias;

  return alias;
}

/// Fetch the declaration of the metadata (or metadata template) for a
/// type.
///
/// If the definition type is specified, the result will always be a
/// GlobalValue of the given type, which may not be at the
/// canonical address point for a type metadata.
///
/// If the definition type is not specified, then:
///   - if the metadata is indirect, then the result will not be adjusted
///     and it will have the type pointer-to-T, where T is the type
///     of a direct metadata;
///   - if the metadata is a pattern, then the result will not be
///     adjusted and it will have FullTypeMetadataPtrTy;
///   - otherwise it will be adjusted to the canonical address point
///     for a type metadata and it will have type TypeMetadataPtrTy.
llvm::Constant *IRGenModule::getAddrOfTypeMetadata(CanType concreteType) {
  return getAddrOfTypeMetadata(concreteType,
                               SymbolReferenceKind::Absolute).getDirectValue();
}

ConstantReference IRGenModule::getAddrOfTypeMetadata(CanType concreteType,
                                               SymbolReferenceKind refKind) {
  assert(!isa<UnboundGenericType>(concreteType));

  llvm::Type *defaultVarTy;
  unsigned adjustmentIndex;
  Alignment alignment = getPointerAlignment();

  ClassDecl *ObjCClass = nullptr;
  
  // Objective-C classes use the ObjC class object.
  if (isa<ClassType>(concreteType) &&
      !hasKnownSwiftMetadata(*this, cast<ClassType>(concreteType)->getDecl())) {
    defaultVarTy = TypeMetadataStructTy;
    adjustmentIndex = 0;
    ObjCClass = cast<ClassType>(concreteType)->getDecl();
  // The symbol for other nominal type metadata is generated at the address
  // point.
  } else if (isa<ClassType>(concreteType) ||
             isa<BoundGenericClassType>(concreteType)) {
    assert(!concreteType->getClassOrBoundGenericClass()->isForeign()
           && "metadata for foreign classes should be emitted as "
              "foreign candidate");
    defaultVarTy = TypeMetadataStructTy;
    adjustmentIndex = 0;
  } else if (auto nom = concreteType->getAnyNominal()) {
    assert(!isa<ClangModuleUnit>(nom->getModuleScopeContext())
           && "metadata for foreign type should be emitted as "
              "foreign candidate");
    (void)nom;
    
    defaultVarTy = TypeMetadataStructTy;
    adjustmentIndex = 0;
  } else {
    // FIXME: Non-nominal metadata provided by the C++ runtime is exported
    // with the address of the start of the full metadata object, since
    // Clang doesn't provide an easy way to emit symbols aliasing into the
    // middle of an object.
    defaultVarTy = FullTypeMetadataStructTy;
    adjustmentIndex = MetadataAdjustmentIndex::ValueType;
  }

  // If this is a use, and the type metadata is emitted lazily,
  // trigger lazy emission of the metadata.
  if (NominalTypeDecl *nominal = concreteType->getAnyNominal()) {
    IRGen.noteUseOfTypeMetadata(nominal);
  }

  LinkEntity entity
    = ObjCClass ? LinkEntity::forObjCClass(ObjCClass)
                : LinkEntity::forTypeMetadata(concreteType,
                                     TypeMetadataAddress::AddressPoint);

  auto DbgTy =
      ObjCClass
          ? DebugTypeInfo::getObjCClass(ObjCClass, ObjCClassPtrTy,
                                        getPointerSize(), getPointerAlignment())
          : DebugTypeInfo::getMetadata(MetatypeType::get(concreteType),
                                       defaultVarTy->getPointerTo(), Size(0),
                                       Alignment(1));

  auto addr = getAddrOfLLVMVariable(entity, alignment, ConstantInit(),
                                    defaultVarTy, DbgTy, refKind);

  // FIXME: MC breaks when emitting alias references on some platforms
  // (rdar://problem/22450593 ). Work around this by referring to the aliasee
  // instead.
  if (auto alias = dyn_cast<llvm::GlobalAlias>(addr.getValue()))
    addr = ConstantReference(alias->getAliasee(), addr.isIndirect());

  // Adjust if necessary.
  if (adjustmentIndex) {
    llvm::Constant *indices[] = {
      llvm::ConstantInt::get(Int32Ty, 0),
      llvm::ConstantInt::get(Int32Ty, adjustmentIndex)
    };
    addr = ConstantReference(
             llvm::ConstantExpr::getInBoundsGetElementPtr(
                                    /*Ty=*/nullptr, addr.getValue(), indices),
                             addr.isIndirect());
  }
  
  return addr;
}

llvm::Constant *
IRGenModule::getAddrOfTypeMetadataPattern(NominalTypeDecl *D) {
  return getAddrOfTypeMetadataPattern(D, false, ConstantInit(), "");
}

llvm::Constant *
IRGenModule::getAddrOfTypeMetadataPattern(NominalTypeDecl *D,
                                          bool isConstant,
                                          ConstantInit init,
                                          StringRef section) {
  if (!init)
    IRGen.noteUseOfTypeMetadata(D);

  auto alignment = getPointerAlignment();
  LinkEntity entity = LinkEntity::forTypeMetadataPattern(D);
  auto addr = getAddrOfLLVMVariable(entity, alignment, init,
                                   Int8PtrTy, DebugTypeInfo());

  if (init) {
    auto var = cast<llvm::GlobalVariable>(addr);
    var->setConstant(true);
    if (!section.empty())
      var->setSection(section);

    // Keep type metadata around for all types.
    addRuntimeResolvableType(D);
  }

  return addr;
}

/// Returns the address of a class metadata base offset.
llvm::Constant *
IRGenModule::getAddrOfClassMetadataBounds(ClassDecl *D,
                                          ForDefinition_t forDefinition) {
  // StoredClassMetadataBounds
  auto layoutTy = llvm::StructType::get(getLLVMContext(), {
    SizeTy,  // Immediate members offset
    Int32Ty, // Negative size in words
    Int32Ty  // Positive size in words
  });

  LinkEntity entity = LinkEntity::forClassMetadataBaseOffset(D);
  return getAddrOfLLVMVariable(entity, getPointerAlignment(), forDefinition,
                               layoutTy, DebugTypeInfo());
}

/// Return the address of a generic type's metadata instantiation cache.
llvm::Constant *
IRGenModule::getAddrOfTypeMetadataInstantiationCache(NominalTypeDecl *D,
                                                ForDefinition_t forDefinition) {
  auto entity = LinkEntity::forTypeMetadataInstantiationCache(D);
  auto ty = llvm::ArrayType::get(Int8PtrTy, NumGenericMetadataPrivateDataWords);
  return getAddrOfLLVMVariable(entity, getPointerAlignment(),
                               forDefinition, ty, DebugTypeInfo());
}

llvm::Function *
IRGenModule::getAddrOfTypeMetadataInstantiationFunction(NominalTypeDecl *D,
                                              ForDefinition_t forDefinition) {
  LinkEntity entity = LinkEntity::forTypeMetadataInstantiationFunction(D);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  llvm::Type *argTys[] = {
    /// Type descriptor.
    TypeContextDescriptorPtrTy,
    /// Generic arguments.
    Int8PtrPtrTy,
    /// Generic metadata pattern.
    Int8PtrPtrTy
  };
  auto fnType = llvm::FunctionType::get(TypeMetadataPtrTy,
                                        argTys, /*isVarArg*/ false);
  Signature signature(fnType, llvm::AttributeList(), DefaultCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

llvm::Function *
IRGenModule::getAddrOfTypeMetadataCompletionFunction(NominalTypeDecl *D,
                                              ForDefinition_t forDefinition) {
  LinkEntity entity = LinkEntity::forTypeMetadataCompletionFunction(D);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  llvm::Type *argTys[] = {
    /// Type metadata.
    TypeMetadataPtrTy,
    /// Metadata completion context.
    Int8PtrTy,
    /// Generic metadata pattern.
    Int8PtrPtrTy
  };
  auto fnType = llvm::FunctionType::get(TypeMetadataResponseTy,
                                        argTys, /*isVarArg*/ false);
  Signature signature(fnType, llvm::AttributeList(), SwiftCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Return the address of a nominal type descriptor.
llvm::Constant *IRGenModule::getAddrOfTypeContextDescriptor(NominalTypeDecl *D,
                                              RequireMetadata_t requireMetadata,
                                              ConstantInit definition) {
  IRGen.noteUseOfTypeContextDescriptor(D, requireMetadata);

  auto entity = LinkEntity::forNominalTypeDescriptor(D);
  return getAddrOfLLVMVariable(entity, Alignment(4),
                               definition,
                               TypeContextDescriptorTy,
                               DebugTypeInfo());
}

/// Return the address of a property descriptor.
llvm::Constant *IRGenModule::getAddrOfPropertyDescriptor(AbstractStorageDecl *D,
                                                      ConstantInit definition) {
  auto entity = LinkEntity::forPropertyDescriptor(D);
  return getAddrOfLLVMVariable(entity, Alignment(4),
                               definition,
                               TypeContextDescriptorTy,
                               DebugTypeInfo());
}

llvm::Constant *IRGenModule::getAddrOfProtocolDescriptor(ProtocolDecl *D,
                                                      ConstantInit definition) {
  if (D->isObjC()) {
    assert(!definition &&
           "cannot define an @objc protocol descriptor this way");
    return getAddrOfObjCProtocolRecord(D, NotForDefinition);
  }

  auto entity = LinkEntity::forProtocolDescriptor(D);
  return getAddrOfLLVMVariable(entity, getPointerAlignment(), definition,
                               ProtocolDescriptorStructTy, DebugTypeInfo());
}

llvm::Constant *IRGenModule::getAddrOfProtocolRequirementArray(ProtocolDecl *D,
                                                               ConstantInit definition) {
  auto entity = LinkEntity::forProtocolRequirementArray(D);
  return getAddrOfLLVMVariable(entity, Alignment(4), definition,
                               definition.getType(),
                               DebugTypeInfo());
}

llvm::Constant *IRGenModule::getAddrOfProtocolConformanceDescriptor(
                                const NormalProtocolConformance *conformance,
                                ConstantInit definition) {
  auto entity = LinkEntity::forProtocolConformanceDescriptor(conformance);
  return getAddrOfLLVMVariable(entity, getPointerAlignment(), definition,
                               ProtocolConformanceDescriptorTy,
                               DebugTypeInfo());
}

/// Fetch the declaration of the ivar initializer for the given class.
Optional<llvm::Function*> IRGenModule::getAddrOfIVarInitDestroy(
                            ClassDecl *cd,
                            bool isDestroyer,
                            bool isForeign,
                            ForDefinition_t forDefinition) {
  auto silRef = SILDeclRef(cd,
                           isDestroyer
                           ? SILDeclRef::Kind::IVarDestroyer
                           : SILDeclRef::Kind::IVarInitializer)
    .asForeign(isForeign);

  // Find the SILFunction for the ivar initializer or destroyer.
  if (auto silFn = getSILModule().lookUpFunction(silRef)) {
    return getAddrOfSILFunction(silFn, forDefinition);
  }

  return None;
}

/// Returns the address of a value-witness function.
llvm::Function *IRGenModule::getAddrOfValueWitness(CanType abstractType,
                                                   ValueWitness index,
                                                ForDefinition_t forDefinition) {
  // We shouldn't emit value witness symbols for generic type instances.
  assert(!isa<BoundGenericType>(abstractType) &&
         "emitting value witness for generic type instance?!");
  
  LinkEntity entity = LinkEntity::forValueWitness(abstractType, index);

  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  auto signature = getValueWitnessSignature(index);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Returns the address of a value-witness table.  If a definition
/// type is provided, the table is created with that type; the return
/// value will be an llvm::GlobalValue.  Otherwise, the result will
/// have type WitnessTablePtrTy.
llvm::Constant *
IRGenModule::getAddrOfValueWitnessTable(CanType concreteType,
                                        ConstantInit definition) {
  LinkEntity entity = LinkEntity::forValueWitnessTable(concreteType);
  return getAddrOfLLVMVariable(entity, getPointerAlignment(), definition,
                               WitnessTableTy, DebugTypeInfo());
}

static Address getAddrOfSimpleVariable(IRGenModule &IGM,
                            llvm::DenseMap<LinkEntity, llvm::Constant*> &cache,
                                       LinkEntity entity,
                                       llvm::Type *type,
                                       Alignment alignment,
                                       ForDefinition_t forDefinition) {
  // Check whether it's already cached.
  llvm::Constant *&entry = cache[entity];
  if (entry) {
    auto existing = cast<llvm::GlobalValue>(entry);
    assert(alignment == Alignment(existing->getAlignment()));
    if (forDefinition) updateLinkageForDefinition(IGM, existing, entity);
    return Address(entry, alignment);
  }

  // Otherwise, we need to create it.
  LinkInfo link = LinkInfo::get(IGM, entity, forDefinition);
  auto addr = createVariable(IGM, link, type, alignment);
  addr->setConstant(true);

  entry = addr;
  return Address(addr, alignment);
}

/// getAddrOfFieldOffset - Get the address of the global variable
/// which contains an offset to apply to either an object (if direct)
/// or a metadata object in order to find an offset to apply to an
/// object (if indirect).
///
/// The result is always a GlobalValue.
Address IRGenModule::getAddrOfFieldOffset(VarDecl *var,
                                          ForDefinition_t forDefinition) {
  LinkEntity entity = LinkEntity::forFieldOffset(var);
  return getAddrOfSimpleVariable(*this, GlobalVars, entity,
                                 SizeTy, getPointerAlignment(),
                                 forDefinition);
}

Address IRGenModule::getAddrOfEnumCase(EnumElementDecl *Case,
                                       ForDefinition_t forDefinition) {
  LinkEntity entity = LinkEntity::forEnumCase(Case);
  return getAddrOfSimpleVariable(*this, GlobalVars, entity, Int32Ty,
                                 getPointerAlignment(), forDefinition);
}

void IRGenModule::emitNestedTypeDecls(DeclRange members) {
  for (Decl *member : members) {
    switch (member->getKind()) {
    case DeclKind::Import:
    case DeclKind::TopLevelCode:
    case DeclKind::Protocol:
    case DeclKind::Extension:
    case DeclKind::InfixOperator:
    case DeclKind::PrefixOperator:
    case DeclKind::PostfixOperator:
    case DeclKind::Param:
    case DeclKind::Module:
    case DeclKind::PrecedenceGroup:
      llvm_unreachable("decl not allowed in type context");

    case DeclKind::IfConfig:
    case DeclKind::PoundDiagnostic:
      continue;

    case DeclKind::Var:
    case DeclKind::Subscript:
    case DeclKind::PatternBinding:
    case DeclKind::Func:
    case DeclKind::Accessor:
    case DeclKind::Constructor:
    case DeclKind::Destructor:
    case DeclKind::EnumCase:
    case DeclKind::EnumElement:
    case DeclKind::MissingMember:
      // Skip non-type members.
      continue;

    case DeclKind::AssociatedType:
    case DeclKind::GenericTypeParam:
      // Do nothing.
      continue;

    case DeclKind::TypeAlias:
      // Do nothing.
      continue;

    case DeclKind::Enum:
      emitEnumDecl(cast<EnumDecl>(member));
      continue;
    case DeclKind::Struct:
      emitStructDecl(cast<StructDecl>(member));
      continue;
    case DeclKind::Class:
      emitClassDecl(cast<ClassDecl>(member));
      continue;
    }
  }
}

static bool shouldEmitCategory(IRGenModule &IGM, ExtensionDecl *ext) {
  for (auto conformance : ext->getLocalConformances()) {
    if (conformance->getProtocol()->isObjC())
      return true;
  }

  for (auto member : ext->getMembers()) {
    if (auto func = dyn_cast<FuncDecl>(member)) {
      if (requiresObjCMethodDescriptor(func))
        return true;
    } else if (auto constructor = dyn_cast<ConstructorDecl>(member)) {
      if (requiresObjCMethodDescriptor(constructor))
        return true;
    } else if (auto var = dyn_cast<VarDecl>(member)) {
      if (requiresObjCPropertyDescriptor(IGM, var))
        return true;
    } else if (auto subscript = dyn_cast<SubscriptDecl>(member)) {
      if (requiresObjCSubscriptDescriptor(IGM, subscript))
        return true;
    }
  }

  return false;
}

void IRGenModule::emitExtension(ExtensionDecl *ext) {
  emitNestedTypeDecls(ext->getMembers());

  // Generate a category if the extension either introduces a
  // conformance to an ObjC protocol or introduces a method
  // that requires an Objective-C entry point.
  ClassDecl *origClass = ext->getExtendedType()->getClassOrBoundGenericClass();
  if (!origClass)
    return;

  if (shouldEmitCategory(*this, ext)) {
    assert(origClass && !origClass->isForeign() &&
           "foreign types cannot have categories emitted");
    llvm::Constant *category = emitCategoryData(*this, ext);
    category = llvm::ConstantExpr::getBitCast(category, Int8PtrTy);
    ObjCCategories.push_back(category);
    ObjCCategoryDecls.push_back(ext);
  }
}


/// Create an allocation on the stack.
Address IRGenFunction::createAlloca(llvm::Type *type,
                                    Alignment alignment,
                                    const llvm::Twine &name) {
  llvm::AllocaInst *alloca =
      new llvm::AllocaInst(type, IGM.DataLayout.getAllocaAddrSpace(), name,
                           AllocaIP);
  alloca->setAlignment(alignment.getValue());
  return Address(alloca, alignment);
}

/// Create an allocation of an array on the stack.
Address IRGenFunction::createAlloca(llvm::Type *type,
                                    llvm::Value *ArraySize,
                                    Alignment alignment,
                                    const llvm::Twine &name) {
  llvm::AllocaInst *alloca =
      new llvm::AllocaInst(type, IGM.DataLayout.getAllocaAddrSpace(), ArraySize,
                           alignment.getValue(), name, AllocaIP);
  return Address(alloca, alignment);
}

/// Allocate a fixed-size buffer on the stack.
Address IRGenFunction::createFixedSizeBufferAlloca(const llvm::Twine &name) {
  return createAlloca(IGM.getFixedBufferTy(),
                      getFixedBufferAlignment(IGM),
                      name);
}

/// Get or create a global string constant.
///
/// \returns an i8* with a null terminator; note that embedded nulls
///   are okay
///
/// FIXME: willBeRelativelyAddressed is only needed to work around an ld64 bug
/// resolving relative references to coalesceable symbols.
/// It should be removed when fixed. rdar://problem/22674524
llvm::Constant *IRGenModule::getAddrOfGlobalString(StringRef data,
                                               bool willBeRelativelyAddressed) {
  // Check whether this string already exists.
  auto &entry = GlobalStrings[data];
  if (entry.second) {
    // FIXME: Clear unnamed_addr if the global will be relative referenced
    // to work around an ld64 bug. rdar://problem/22674524
    if (willBeRelativelyAddressed)
      entry.first->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::None);
    return entry.second;
  }

  entry = createStringConstant(data, willBeRelativelyAddressed);
  return entry.second;
}

/// Get or create a global UTF-16 string constant.
///
/// \returns an i16* with a null terminator; note that embedded nulls
///   are okay
llvm::Constant *IRGenModule::getAddrOfGlobalUTF16String(StringRef utf8) {
  // Check whether this string already exists.
  auto &entry = GlobalUTF16Strings[utf8];
  if (entry) return entry;

  // If not, first transcode it to UTF16.
  SmallVector<llvm::UTF16, 128> buffer(utf8.size() + 1); // +1 for ending nulls.
  const llvm::UTF8 *fromPtr = (const llvm::UTF8 *) utf8.data();
  llvm::UTF16 *toPtr = &buffer[0];
  (void) ConvertUTF8toUTF16(&fromPtr, fromPtr + utf8.size(),
                            &toPtr, toPtr + utf8.size(),
                            llvm::strictConversion);

  // The length of the transcoded string in UTF-8 code points.
  size_t utf16Length = toPtr - &buffer[0];

  // Null-terminate the UTF-16 string.
  *toPtr = 0;
  ArrayRef<llvm::UTF16> utf16(&buffer[0], utf16Length + 1);

  auto init = llvm::ConstantDataArray::get(LLVMContext, utf16);
  auto global = new llvm::GlobalVariable(Module, init->getType(), true,
                                         llvm::GlobalValue::PrivateLinkage,
                                         init);
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

  // Drill down to make an i16*.
  auto zero = llvm::ConstantInt::get(SizeTy, 0);
  llvm::Constant *indices[] = { zero, zero };
  auto address = llvm::ConstantExpr::getInBoundsGetElementPtr(
      global->getValueType(), global, indices);

  // Cache and return.
  entry = address;
  return address;
}

static llvm::Constant *getMetatypeDeclarationFor(IRGenModule &IGM,
                                                 StringRef name) {
  auto *storageType = IGM.ObjCClassStructTy;

  // We may have defined the variable already.
  if (auto existing = IGM.Module.getNamedGlobal(name))
    return getElementBitCast(existing, storageType);

  auto linkage = llvm::GlobalValue::ExternalLinkage;
  auto visibility = llvm::GlobalValue::DefaultVisibility;
  auto storageClass = llvm::GlobalValue::DefaultStorageClass;

  auto var = new llvm::GlobalVariable(IGM.Module, storageType,
                                      /*constant*/ false, linkage,
                                      /*initializer*/ nullptr, name);
  var->setVisibility(visibility);
  var->setDLLStorageClass(storageClass);
  var->setAlignment(IGM.getPointerAlignment().getValue());

  return var;
}
#define STRINGIFY_IMPL(x) #x
#define REALLY_STRINGIFY( x) STRINGIFY_IMPL(x)

llvm::Constant *
IRGenModule::getAddrOfGlobalConstantString(StringRef utf8) {
  auto &entry = GlobalConstantStrings[utf8];
  if (entry)
    return entry;

  // If not, create it.  This implicitly adds a trailing null.
  auto data = llvm::ConstantDataArray::getString(LLVMContext, utf8);
  auto *dataTy = data->getType();

  llvm::Type *constantStringTy[] = {
      RefCountedStructTy,
      Int32Ty,
      Int32Ty,
      Int8Ty,
      dataTy
  };
  auto *ConstantStringTy =
      llvm::StructType::get(getLLVMContext(), constantStringTy,
                            /*packed*/ false);

  auto metaclass = getMetatypeDeclarationFor(
      *this, REALLY_STRINGIFY(CLASS_METADATA_SYM(s20_Latin1StringStorage)));

  metaclass = llvm::ConstantExpr::getBitCast(metaclass, TypeMetadataPtrTy);

  // Get a reference count of two.
  auto *refCountInit = llvm::ConstantInt::get(
      IntPtrTy,
      InlineRefCountBits(1 /* "extra" strong ref count*/, 1 /* unowned count */)
          .getBitsValue());

  auto *count = llvm::ConstantInt::get(Int32Ty, utf8.size());
  // Capacity is length plus one because of the implicitly added '\0'
  // character.
  auto *capacity = llvm::ConstantInt::get(Int32Ty, utf8.size() + 1);
  auto *flags = llvm::ConstantInt::get(Int8Ty, 0);

  llvm::Constant *heapObjectHeaderFields[] = {
      metaclass, refCountInit
  };

  auto *initRefCountStruct = llvm::ConstantStruct::get(
      RefCountedStructTy, makeArrayRef(heapObjectHeaderFields));

  llvm::Constant *fields[] = {
      initRefCountStruct, count, capacity, flags, data};
  auto *init =
      llvm::ConstantStruct::get(ConstantStringTy, makeArrayRef(fields));

  auto global = new llvm::GlobalVariable(Module, init->getType(), true,
                                         llvm::GlobalValue::PrivateLinkage,
                                         init);
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

  // Cache string entry.
  entry = global;

  return global;
}

llvm::Constant *
IRGenModule::getAddrOfGlobalUTF16ConstantString(StringRef utf8) {
  auto &entry = GlobalConstantUTF16Strings[utf8];
  if (entry)
    return entry;

  // If not, first transcode it to UTF16.
  SmallVector<llvm::UTF16, 128> buffer(utf8.size() + 1); // +1 for ending nulls.
  const llvm::UTF8 *fromPtr = (const llvm::UTF8 *) utf8.data();
  llvm::UTF16 *toPtr = &buffer[0];
  (void) ConvertUTF8toUTF16(&fromPtr, fromPtr + utf8.size(),
                            &toPtr, toPtr + utf8.size(),
                            llvm::strictConversion);

  // The length of the transcoded string in UTF-8 code points.
  size_t utf16Length = toPtr - &buffer[0];

  // Null-terminate the UTF-16 string.
  *toPtr = 0;
  ArrayRef<llvm::UTF16> utf16(&buffer[0], utf16Length + 1);

  auto *data = llvm::ConstantDataArray::get(LLVMContext, utf16);
  auto *dataTy = data->getType();

  llvm::Type *constantUTFStringTy[] = {
      RefCountedStructTy,
      Int32Ty,
      Int32Ty,
      Int8Ty,
      Int8Ty, // For 16-byte alignment.
      dataTy
  };
  auto *ConstantUTFStringTy =
      llvm::StructType::get(getLLVMContext(), constantUTFStringTy,
                            /*packed*/ false);

  auto metaclass = getMetatypeDeclarationFor(
      *this, REALLY_STRINGIFY(CLASS_METADATA_SYM(s19_UTF16StringStorage)));

  metaclass = llvm::ConstantExpr::getBitCast(metaclass, TypeMetadataPtrTy);

  // Get a reference count of two.
  auto *refCountInit = llvm::ConstantInt::get(
      IntPtrTy,
      InlineRefCountBits(1 /* "extra" strong ref count*/, 1 /* unowned count */)
          .getBitsValue());

  auto *count = llvm::ConstantInt::get(Int32Ty, utf16Length);
  auto *capacity = llvm::ConstantInt::get(Int32Ty, utf16Length + 1);
  auto *flags = llvm::ConstantInt::get(Int8Ty, 0);
  auto *padding = llvm::ConstantInt::get(Int8Ty, 0);

  llvm::Constant *heapObjectHeaderFields[] = {
      metaclass, refCountInit
  };

  auto *initRefCountStruct = llvm::ConstantStruct::get(
      RefCountedStructTy, makeArrayRef(heapObjectHeaderFields));

  llvm::Constant *fields[] = {
      initRefCountStruct, count, capacity, flags, padding, data};
  auto *init =
      llvm::ConstantStruct::get(ConstantUTFStringTy, makeArrayRef(fields));

  auto global = new llvm::GlobalVariable(Module, init->getType(), true,
                                         llvm::GlobalValue::PrivateLinkage,
                                         init);
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

  // Cache string entry.
  entry = global;

  return global;
}

/// Do we have to use resilient access patterns when working with this
/// declaration?
///
/// IRGen is primarily concerned with resilient handling of the following:
/// - For structs, a struct's size might change
/// - For enums, new cases can be added
/// - For classes, the superclass might change the size or number
///   of stored properties
bool IRGenModule::isResilient(NominalTypeDecl *D, ResilienceExpansion expansion) {
  if (expansion == ResilienceExpansion::Maximal &&
      Types.isCompletelyFragile()) {
    return false;
  }
  return D->isResilient(getSwiftModule(), expansion);
}

// The most general resilience expansion where the given declaration is visible.
ResilienceExpansion
IRGenModule::getResilienceExpansionForAccess(NominalTypeDecl *decl) {
  if (decl->getModuleContext() == getSwiftModule() &&
      decl->getEffectiveAccess() < AccessLevel::Public)
    return ResilienceExpansion::Maximal;
  return ResilienceExpansion::Minimal;
}

// The most general resilience expansion which has knowledge of the declaration's
// layout. Calling isResilient() with this scope will always return false.
ResilienceExpansion
IRGenModule::getResilienceExpansionForLayout(NominalTypeDecl *decl) {
  if (Types.isCompletelyFragile())
    return ResilienceExpansion::Minimal;

  if (isResilient(decl, ResilienceExpansion::Minimal))
    return ResilienceExpansion::Maximal;

  return getResilienceExpansionForAccess(decl);
}

// The most general resilience expansion which has knowledge of the global
// variable's layout.
ResilienceExpansion
IRGenModule::getResilienceExpansionForLayout(SILGlobalVariable *global) {
  if (hasPublicVisibility(global->getLinkage()))
    return ResilienceExpansion::Minimal;
  return ResilienceExpansion::Maximal;
}

llvm::Constant *IRGenModule::
getAddrOfGenericWitnessTableCache(const NormalProtocolConformance *conf,
                                  ForDefinition_t forDefinition) {
  auto entity = LinkEntity::forGenericProtocolWitnessTableCache(conf);
  auto expectedTy = getGenericWitnessTableCacheTy();
  return getAddrOfLLVMVariable(entity, getPointerAlignment(), forDefinition,
                               expectedTy, DebugTypeInfo());
}

llvm::Constant *IRGenModule::
getAddrOfResilientWitnessTable(const NormalProtocolConformance *conf,
                               ConstantInit definition) {
  auto entity = LinkEntity::forResilientProtocolWitnessTable(conf);
  return getAddrOfLLVMVariable(entity, getPointerAlignment(), definition,
                               definition.getType(), DebugTypeInfo());
}

llvm::Function *
IRGenModule::getAddrOfGenericWitnessTableInstantiationFunction(
                                      const NormalProtocolConformance *conf) {
  auto forDefinition = ForDefinition;

  LinkEntity entity =
    LinkEntity::forGenericProtocolWitnessTableInstantiationFunction(conf);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  auto fnType = llvm::FunctionType::get(
      VoidTy, {WitnessTablePtrTy, TypeMetadataPtrTy, Int8PtrPtrTy},
      /*varargs*/ false);
  Signature signature(fnType, llvm::AttributeList(), DefaultCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

llvm::StructType *IRGenModule::getGenericWitnessTableCacheTy() {
  if (auto ty = GenericWitnessTableCacheTy) return ty;

  GenericWitnessTableCacheTy = llvm::StructType::create(getLLVMContext(),
    {
      // WitnessTableSizeInWords
      Int16Ty,
      // WitnessTablePrivateSizeInWords
      Int16Ty,
      // Protocol
      RelativeAddressTy,
      // Pattern
      RelativeAddressTy,
      // ResilientWitnesses
      RelativeAddressTy,
      // Instantiator
      RelativeAddressTy,
      // PrivateData
      RelativeAddressTy
    }, "swift.generic_witness_table_cache");
  return GenericWitnessTableCacheTy;
}

/// Fetch the witness table access function for a protocol conformance.
llvm::Function *
IRGenModule::getAddrOfWitnessTableAccessFunction(
                                      const NormalProtocolConformance *conf,
                                              ForDefinition_t forDefinition) {
  IRGen.addLazyWitnessTable(conf);

  LinkEntity entity = LinkEntity::forProtocolWitnessTableAccessFunction(conf);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  llvm::FunctionType *fnType;
  if (conf->witnessTableAccessorRequiresArguments()) {
    // conditional requirements are passed indirectly, as an array of witness
    // tables.
    fnType = llvm::FunctionType::get(
        WitnessTablePtrTy, {TypeMetadataPtrTy, WitnessTablePtrPtrTy}, false);
  } else {
    fnType = llvm::FunctionType::get(WitnessTablePtrTy, false);
  }

  Signature signature(fnType, llvm::AttributeList(), DefaultCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Fetch the lazy witness table access function for a protocol conformance.
llvm::Function *
IRGenModule::getAddrOfWitnessTableLazyAccessFunction(
                                      const NormalProtocolConformance *conf,
                                              CanType conformingType,
                                              ForDefinition_t forDefinition) {
  LinkEntity entity =
    LinkEntity::forProtocolWitnessTableLazyAccessFunction(conf, conformingType);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  llvm::FunctionType *fnType
    = llvm::FunctionType::get(WitnessTablePtrTy, false);

  Signature signature(fnType, llvm::AttributeList(), DefaultCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Get or create a witness table cache variable.  These are an
/// implementation detail of witness table lazy access functions.
llvm::Constant *
IRGenModule::getAddrOfWitnessTableLazyCacheVariable(
                                      const NormalProtocolConformance *conf,
                                              CanType conformingType,
                                              ForDefinition_t forDefinition) {
  assert(!conformingType->hasArchetype());
  LinkEntity entity =
    LinkEntity::forProtocolWitnessTableLazyCacheVariable(conf, conformingType);
  return getAddrOfLLVMVariable(entity, getPointerAlignment(),
                               forDefinition, WitnessTablePtrTy,
                               DebugTypeInfo());
}

/// Look up the address of a witness table.
///
/// This can only be used with non-dependent conformances.
llvm::Constant*
IRGenModule::getAddrOfWitnessTable(const NormalProtocolConformance *conf,
                                   ConstantInit definition) {
  IRGen.addLazyWitnessTable(conf);

  auto entity = LinkEntity::forDirectProtocolWitnessTable(conf);
  return getAddrOfLLVMVariable(entity, getPointerAlignment(), definition,
                               WitnessTableTy, DebugTypeInfo());
}

/// Look up the address of a witness table pattern.
///
/// This can only be used with dependent conformances from inside the
/// defining module.
llvm::Constant*
IRGenModule::getAddrOfWitnessTablePattern(const NormalProtocolConformance *conf,
                                          ConstantInit definition) {
  IRGen.addLazyWitnessTable(conf);

  auto entity = LinkEntity::forProtocolWitnessTablePattern(conf);
  return getAddrOfLLVMVariable(entity, getPointerAlignment(), definition,
                               WitnessTableTy, DebugTypeInfo());
}

llvm::Function *
IRGenModule::getAddrOfAssociatedTypeMetadataAccessFunction(
                                  const NormalProtocolConformance *conformance,
                                  AssociatedType association) {
  auto forDefinition = ForDefinition;

  LinkEntity entity =
    LinkEntity::forAssociatedTypeMetadataAccessFunction(conformance,
                                                        association);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  auto signature = getAssociatedTypeMetadataAccessFunctionSignature();
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

llvm::Function *
IRGenModule::getAddrOfAssociatedTypeWitnessTableAccessFunction(
                                  const NormalProtocolConformance *conformance,
                                  const AssociatedConformance &association) {
  auto forDefinition = ForDefinition;

  LinkEntity entity =
    LinkEntity::forAssociatedTypeWitnessTableAccessFunction(conformance,
                                                            association);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  auto signature = getAssociatedTypeWitnessTableAccessFunctionSignature();
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

llvm::Function *
IRGenModule::getAddrOfContinuationPrototype(CanSILFunctionType fnType) {
  LinkEntity entity = LinkEntity::forCoroutineContinuationPrototype(fnType);

  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) return entry;

  auto signature = Signature::forCoroutineContinuation(*this, fnType);
  LinkInfo link = LinkInfo::get(*this, entity, NotForDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

/// Should we be defining the given helper function?
static llvm::Function *shouldDefineHelper(IRGenModule &IGM,
                                          llvm::Constant *fn,
                                          bool setIsNoInline) {
  auto *def = dyn_cast<llvm::Function>(fn);
  if (!def) return nullptr;
  if (!def->empty()) return nullptr;

  def->setLinkage(llvm::Function::LinkOnceODRLinkage);
  def->setVisibility(llvm::Function::HiddenVisibility);
  def->setDLLStorageClass(llvm::GlobalVariable::DefaultStorageClass);
  def->setDoesNotThrow();
  def->setCallingConv(IGM.DefaultCC);
  if (setIsNoInline)
    def->addFnAttr(llvm::Attribute::NoInline);
  return def;
}

/// Get or create a helper function with the given name and type, lazily
/// using the given generation function to fill in its body.
///
/// The helper function will be shared between translation units within the
/// current linkage unit, so choose the name carefully to ensure that it
/// does not collide with any other helper function.  In general, it should
/// be a Swift-specific C reserved name; that is, it should start with
//  "__swift".
llvm::Constant *
IRGenModule::getOrCreateHelperFunction(StringRef fnName, llvm::Type *resultTy,
                                       ArrayRef<llvm::Type*> paramTys,
                        llvm::function_ref<void(IRGenFunction &IGF)> generate,
                        bool setIsNoInline) {
  llvm::FunctionType *fnTy =
    llvm::FunctionType::get(resultTy, paramTys, false);

  llvm::Constant *fn = Module.getOrInsertFunction(fnName, fnTy);

  if (llvm::Function *def = shouldDefineHelper(*this, fn, setIsNoInline)) {
    IRGenFunction IGF(*this, def);
    if (DebugInfo)
      DebugInfo->emitArtificialFunction(IGF, def);
    generate(IGF);
  }

  return fn;
}
