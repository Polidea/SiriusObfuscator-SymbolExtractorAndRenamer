//===----------------------------------------------------------------------===//
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

#include "swift/Runtime/Reflection.h"
#include "swift/Runtime/Casting.h"
#include "swift/Runtime/Config.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Runtime/Enum.h"
#include "swift/Runtime/Unreachable.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/Portability.h"
#include "Private.h"
#include "WeakReference.h"
#include "llvm/Support/Compiler.h"
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <tuple>

#if SWIFT_OBJC_INTEROP
#include "swift/Runtime/ObjCBridge.h"
#include "SwiftObject.h"
#include <Foundation/Foundation.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#endif

#if defined(_WIN32)
#include <stdarg.h>

namespace {
int asprintf(char **strp, const char *fmt, ...) {
  va_list argp0, argp1;

  va_start(argp0, fmt);
  va_copy(argp1, argp0);

  int length = _vscprintf(fmt, argp0);

  *strp = reinterpret_cast<char *>(malloc(length + 1));
  if (*strp == nullptr)
    return -1;

  length = _vsnprintf(*strp, length, fmt, argp1);

  va_end(argp0);
  va_end(argp1);

  return length;
}

char *strndup(const char *s, size_t n) {
  size_t length = std::min(strlen(s), n);

  char *buffer = reinterpret_cast<char *>(malloc(length + 1));
  if (buffer == nullptr)
    return buffer;

  strncpy(buffer, s, length);
  buffer[length] = '\0';
  return buffer;
}
}
#endif

using namespace swift;

#if SWIFT_OBJC_INTEROP
// Declare the debugQuickLookObject selector.
@interface DeclareSelectors
- (id)debugQuickLookObject;
@end
#endif

namespace {

/// The layout of Any.
using Any = OpaqueExistentialContainer;

// Swift assumes Any is returned in memory.
// Use AnyReturn to guarantee that even on architectures
// where Any would be returned in registers.
struct AnyReturn {
  Any any;
  AnyReturn(Any a) : any(a) { }
  operator Any() { return any; }
  ~AnyReturn() { }
};

static std::tuple<const Metadata *, OpaqueValue *>
unwrapExistential(const Metadata *T, OpaqueValue *Value) {
  // If the value is an existential container, look through it to reflect the
  // contained value.
  // TODO: Should look through existential metatypes too, but it doesn't
  // really matter yet since we don't have any special mirror behavior for
  // concrete metatypes yet.
  while (T->getKind() == MetadataKind::Existential) {
    auto *existential
      = static_cast<const ExistentialTypeMetadata *>(T);

    // Unwrap the existential container.
    T = existential->getDynamicType(Value);
    Value = existential->projectValue(Value);

    // Existential containers can end up nested in some cases due to generic
    // abstraction barriers.  Repeat in case we have a nested existential.
  }
  return std::make_tuple(T, Value);
}

static bool loadSpecialReferenceStorage(OpaqueValue *fieldData,
                                        const FieldType fieldType,
                                        Any *outValue) {
  // isWeak() implies a reference type via Sema.
  if (!fieldType.isWeak())
    return false;

  auto type = fieldType.getType();
  assert(type->getKind() == MetadataKind::Optional);

  auto *weakField = reinterpret_cast<WeakReference *>(fieldData);
  auto *strongValue = swift_unknownWeakLoadStrong(weakField);

  // Now that we have a strong reference, we need to create a temporary buffer
  // from which to copy the whole value, which might be a native class-bound
  // existential, which means we also need to copy n witness tables, for
  // however many protocols are in the protocol composition. For example, if we
  // are copying a:
  // weak var myWeakProperty : (Protocol1 & Protocol2)?
  // then we need to copy three values:
  // - the instance
  // - the witness table for Protocol1
  // - the witness table for Protocol2

  auto *weakContainer =
    reinterpret_cast<WeakClassExistentialContainer *>(fieldData);

  // Create a temporary existential where we can put the strong reference.
  // The allocateBuffer value witness requires a ValueBuffer to own the
  // allocated storage.
  ValueBuffer temporaryBuffer;

  auto *temporaryValue = reinterpret_cast<ClassExistentialContainer *>(
      type->allocateBufferIn(&temporaryBuffer));

  // Now copy the entire value out of the parent, which will include the
  // witness tables.
  temporaryValue->Value = strongValue;
  auto valueWitnessesSize = type->getValueWitnesses()->getSize() -
                            sizeof(WeakClassExistentialContainer);
  memcpy(temporaryValue->getWitnessTables(), weakContainer->getWitnessTables(),
         valueWitnessesSize);

  outValue->Type = type;
  auto *opaqueValueAddr = type->allocateBoxForExistentialIn(&outValue->Buffer);
  type->vw_initializeWithCopy(opaqueValueAddr,
                              reinterpret_cast<OpaqueValue *>(temporaryValue));

  type->deallocateBufferIn(&temporaryBuffer);
  
  return true;
}


// Abstract base class for reflection implementations.
struct ReflectionMirrorImpl {
  const Metadata *type;
  OpaqueValue *value;
  
  virtual char displayStyle() = 0;
  virtual intptr_t count() = 0;
  virtual AnyReturn subscript(intptr_t index, const char **outName,
                              void (**outFreeFunc)(const char *)) = 0;
  virtual const char *enumCaseName() { return nullptr; }

#if SWIFT_OBJC_INTEROP
  virtual id quickLookObject() { return nil; }
#endif
  
  virtual ~ReflectionMirrorImpl() {}
};


// Implementation for tuples.
struct TupleImpl : ReflectionMirrorImpl {
  char displayStyle() {
    return 't';
  }
  
  intptr_t count() {
    auto *Tuple = static_cast<const TupleTypeMetadata *>(type);
    return Tuple->NumElements;
  }
  
  AnyReturn subscript(intptr_t i, const char **outName,
                      void (**outFreeFunc)(const char *)) {
    auto *Tuple = static_cast<const TupleTypeMetadata *>(type);

    if (i < 0 || (size_t)i > Tuple->NumElements)
      swift::crash("Swift mirror subscript bounds check failure");

    // Determine whether there is a label.
    bool hasLabel = false;
    if (const char *labels = Tuple->Labels) {
      const char *space = strchr(labels, ' ');
      for (intptr_t j = 0; j != i && space; ++j) {
        labels = space + 1;
        space = strchr(labels, ' ');
      }

      // If we have a label, create it.
      if (labels && space && labels != space) {
        *outName = strndup(labels, space - labels);
        hasLabel = true;
      }
    }

    if (!hasLabel) {
      // The name is the stringized element number '.0'.
      char *str;
      asprintf(&str, ".%" PRIdPTR, i);
      *outName = str;
    }
    
    *outFreeFunc = [](const char *str) { free(const_cast<char *>(str)); };

    // Get the nth element.
    auto &elt = Tuple->getElement(i);
    auto *bytes = reinterpret_cast<const char *>(value);
    auto *eltData = reinterpret_cast<const OpaqueValue *>(bytes + elt.Offset);

    Any result;

    result.Type = elt.Type;
    auto *opaqueValueAddr = result.Type->allocateBoxForExistentialIn(&result.Buffer);
    result.Type->vw_initializeWithCopy(opaqueValueAddr,
                                       const_cast<OpaqueValue *>(eltData));

    return AnyReturn(result);
  }
};


// Implementation for structs.
struct StructImpl : ReflectionMirrorImpl {
  bool isReflectable() {
    const auto *Struct = static_cast<const StructMetadata *>(type);
    const auto &Description = Struct->getDescription();
    return Description->getTypeContextDescriptorFlags().isReflectable();
  }

  char displayStyle() {
    return 's';
  }
  
  intptr_t count() {
    if (!isReflectable()) {
      return 0;
    }

    auto *Struct = static_cast<const StructMetadata *>(type);
    return Struct->getDescription()->NumFields;
  }
  
  AnyReturn subscript(intptr_t i, const char **outName,
                      void (**outFreeFunc)(const char *)) {
    auto *Struct = static_cast<const StructMetadata *>(type);

    if (i < 0 || (size_t)i > Struct->getDescription()->NumFields)
      swift::crash("Swift mirror subscript bounds check failure");

    // Load the offset from its respective vector.
    auto fieldOffset = Struct->getFieldOffsets()[i];

    Any result;
    
    _swift_getFieldAt(type, i, [&](llvm::StringRef name, FieldType fieldInfo) {
      assert(!fieldInfo.isIndirect() && "indirect struct fields not implemented");
      
      *outName = name.data();
      *outFreeFunc = nullptr;
      
      auto *bytes = reinterpret_cast<char*>(value);
      auto *fieldData = reinterpret_cast<OpaqueValue *>(bytes + fieldOffset);
      
      bool didLoad = loadSpecialReferenceStorage(fieldData, fieldInfo, &result);
      if (!didLoad) {
        result.Type = fieldInfo.getType();
        auto *opaqueValueAddr = result.Type->allocateBoxForExistentialIn(&result.Buffer);
        result.Type->vw_initializeWithCopy(opaqueValueAddr,
                                           const_cast<OpaqueValue *>(fieldData));
      }
    });

    return AnyReturn(result);
  }
};


// Implementation for enums.
struct EnumImpl : ReflectionMirrorImpl {
  bool isReflectable() {
    const auto *Enum = static_cast<const EnumMetadata *>(type);
    const auto &Description = Enum->getDescription();
    return Description->getTypeContextDescriptorFlags().isReflectable();
  }
  
  const char *getInfo(unsigned *tagPtr = nullptr,
                      const Metadata **payloadTypePtr = nullptr,
                      bool *indirectPtr = nullptr) {
    // 'tag' is in the range [0..NumElements-1].
    unsigned tag = type->vw_getEnumTag(value);

    const Metadata *payloadType = nullptr;
    bool indirect = false;
    
    const char *caseName = nullptr;
    _swift_getFieldAt(type, tag, [&](llvm::StringRef name, FieldType info) {
      caseName = name.data();
      payloadType = info.getType();
      indirect = info.isIndirect();
    });

    if (tagPtr)
      *tagPtr = tag;
    if (payloadTypePtr)
      *payloadTypePtr = payloadType;
    if (indirectPtr)
      *indirectPtr = indirect;
    
    return caseName;
  }

  char displayStyle() {
    return 'e';
  }
  
  intptr_t count() {
    if (!isReflectable()) {
      return 0;
    }
    
    const Metadata *payloadType;
    getInfo(nullptr, &payloadType, nullptr);
    return (payloadType != nullptr) ? 1 : 0;
  }

  AnyReturn subscript(intptr_t i, const char **outName,
                      void (**outFreeFunc)(const char *)) {
    unsigned tag;
    const Metadata *payloadType;
    bool indirect;

    auto *caseName = getInfo(&tag, &payloadType, &indirect);

    // Copy the enum payload into a box
    const Metadata *boxType = (indirect ? &METADATA_SYM(Bo).base : payloadType);
    BoxPair pair = swift_allocBox(boxType);

    type->vw_destructiveProjectEnumData(const_cast<OpaqueValue *>(value));
    boxType->vw_initializeWithCopy(pair.buffer, const_cast<OpaqueValue *>(value));
    type->vw_destructiveInjectEnumTag(const_cast<OpaqueValue *>(value), tag);

    value = pair.buffer;

    // If the payload is indirect, we need to jump through the box to get it.
    if (indirect) {
      const HeapObject *owner = *reinterpret_cast<HeapObject * const *>(value);
      value = swift_projectBox(const_cast<HeapObject *>(owner));
    }
    
    *outName = caseName;
    *outFreeFunc = nullptr;
    
    Any result;

    result.Type = payloadType;
    auto *opaqueValueAddr = result.Type->allocateBoxForExistentialIn(&result.Buffer);
    result.Type->vw_initializeWithCopy(opaqueValueAddr,
                                       const_cast<OpaqueValue *>(value));

    swift_release(pair.object);
    return AnyReturn(result);
  }
  
  const char *enumCaseName() {
    if (!isReflectable()) {
      return nullptr;
    }
    
    return getInfo();
  }
};


// Implementation for classes.
struct ClassImpl : ReflectionMirrorImpl {
  bool isReflectable() {
    const auto *Class = static_cast<const ClassMetadata *>(type);
    const auto &Description = Class->getDescription();
    return Description->getTypeContextDescriptorFlags().isReflectable();
  }

  char displayStyle() {
    return 'c';
  }
  
  intptr_t count() {
    if (!isReflectable())
      return 0;

    auto *Clas = static_cast<const ClassMetadata*>(type);
    auto count = Clas->getDescription()->NumFields;

    return count;
  }
  
  AnyReturn subscript(intptr_t i, const char **outName,
                      void (**outFreeFunc)(const char *)) {
    auto *Clas = static_cast<const ClassMetadata*>(type);

    if (i < 0 || (size_t)i > Clas->getDescription()->NumFields)
      swift::crash("Swift mirror subscript bounds check failure");

    // FIXME: If the class has ObjC heritage, get the field offset using the ObjC
    // metadata, because we don't update the field offsets in the face of
    // resilient base classes.
    uintptr_t fieldOffset;
    if (usesNativeSwiftReferenceCounting(Clas)) {
      fieldOffset = Clas->getFieldOffsets()[i];
    } else {
  #if SWIFT_OBJC_INTEROP
      Ivar *ivars = class_copyIvarList((Class)Clas, nullptr);
      fieldOffset = ivar_getOffset(ivars[i]);
      free(ivars);
  #else
      swift::crash("Object appears to be Objective-C, but no runtime.");
  #endif
    }

    Any result;
    
    _swift_getFieldAt(type, i, [&](llvm::StringRef name, FieldType fieldInfo) {
      assert(!fieldInfo.isIndirect() && "class indirect properties not implemented");
      
      auto *bytes = *reinterpret_cast<char * const *>(value);
      auto *fieldData = reinterpret_cast<OpaqueValue *>(bytes + fieldOffset);

      *outName = name.data();
      *outFreeFunc = nullptr;
    
      bool didLoad = loadSpecialReferenceStorage(fieldData, fieldInfo, &result);
      if (!didLoad) {
        result.Type = fieldInfo.getType();
        auto *opaqueValueAddr = result.Type->allocateBoxForExistentialIn(&result.Buffer);
        result.Type->vw_initializeWithCopy(opaqueValueAddr,
                                           const_cast<OpaqueValue *>(fieldData));
      }
    });
    
    return AnyReturn(result);
  }

#if SWIFT_OBJC_INTEROP
  id quickLookObject() {
    id object = [*reinterpret_cast<const id *>(value) retain];
    if ([object respondsToSelector:@selector(debugQuickLookObject)]) {
      id quickLookObject = [object debugQuickLookObject];
      [quickLookObject retain];
      [object release];
      return quickLookObject;
    }

    return object;
  }
#endif
};


#if SWIFT_OBJC_INTEROP
// Implementation for ObjC classes.
struct ObjCClassImpl : ClassImpl {
  intptr_t count() {
    // ObjC makes no guarantees about the state of ivars, so we can't safely
    // introspect them in the general case.
    return 0;
  }
  
  AnyReturn subscript(intptr_t i, const char **outName,
                      void (**outFreeFunc)(const char *)) {
    swift::crash("Cannot get children of Objective-C objects.");
  }
};
#endif


// Implementation for metatypes.
struct MetatypeImpl : ReflectionMirrorImpl {
  char displayStyle() {
    return '\0';
  }
  
  intptr_t count() {
    return 0;
  }
  
  AnyReturn subscript(intptr_t i, const char **outName,
                    void (**outFreeFunc)(const char *)) {
    swift::crash("Metatypes have no children.");
  }
};


// Implementation for opaque types.
struct OpaqueImpl : ReflectionMirrorImpl {
  char displayStyle() {
    return '\0';
  }
  
  intptr_t count() {
    return 0;
  }
  
  AnyReturn subscript(intptr_t i, const char **outName,
                    void (**outFreeFunc)(const char *)) {
    swift::crash("Opaque types have no children.");
  }
};


template<typename F>
auto call(OpaqueValue *passedValue, const Metadata *T, const Metadata *passedType,
          const F &f) -> decltype(f(nullptr))
{
  const Metadata *type;
  OpaqueValue *value;
  std::tie(type, value) = unwrapExistential(T, passedValue);
  
  if (passedType != nullptr) {
    type = passedType;
  }
  
  auto call = [&](ReflectionMirrorImpl *impl) {
    impl->type = type;
    impl->value = value;
    auto result = f(impl);
    SWIFT_CC_PLUSONE_GUARD(T->vw_destroy(passedValue));
    return result;
  };
  
  auto callClass = [&] {
    if (passedType == nullptr) {
      // Get the runtime type of the object.
      const void *obj = *reinterpret_cast<const void * const *>(value);
      auto isa = _swift_getClass(obj);

      // Look through artificial subclasses.
      while (isa->isTypeMetadata() && isa->isArtificialSubclass()) {
        isa = isa->Superclass;
      }
      passedType = isa;
    }

  #if SWIFT_OBJC_INTEROP
    // If this is a pure ObjC class, reflect it using ObjC's runtime facilities.
    // ForeignClass (e.g. CF classes) manifests as a NULL class object.
    auto *classObject = passedType->getClassObject();
    if (classObject == nullptr || !classObject->isTypeMetadata()) {
      ObjCClassImpl impl;
      return call(&impl);
    }
  #endif

    // Otherwise, use the native Swift facilities.
    ClassImpl impl;
    return call(&impl);
  };
  
  switch (type->getKind()) {
    case MetadataKind::Tuple: {
      TupleImpl impl;
      return call(&impl);
    }

    case MetadataKind::Struct: {
      StructImpl impl;
      return call(&impl);
    }
    

    case MetadataKind::Enum:
    case MetadataKind::Optional: {
      EnumImpl impl;
      return call(&impl);
    }
      
    case MetadataKind::ObjCClassWrapper:
    case MetadataKind::ForeignClass:
    case MetadataKind::Class: {
      return callClass();
    }

    case MetadataKind::Metatype:
    case MetadataKind::ExistentialMetatype: {
      MetatypeImpl impl;
      return call(&impl);
    }

    case MetadataKind::Opaque: {
#if SWIFT_OBJC_INTEROP
      // If this is the Builtin.UnknownObject type, use the dynamic type of the
      // object reference.
      if (type == &METADATA_SYM(BO).base) {
        return callClass();
      }
#endif
      // If this is the Builtin.NativeObject type, and the heap object is a
      // class instance, use the dynamic type of the object reference.
      if (type == &METADATA_SYM(Bo).base) {
        const HeapObject *obj
          = *reinterpret_cast<const HeapObject * const*>(value);
        if (obj->metadata->getKind() == MetadataKind::Class) {
          return callClass();
        }
      }
      LLVM_FALLTHROUGH;
    }

    /// TODO: Implement specialized mirror witnesses for all kinds.
    case MetadataKind::Function:
    case MetadataKind::Existential:
      break;

    // Types can't have these kinds.
    case MetadataKind::HeapLocalVariable:
    case MetadataKind::HeapGenericLocalVariable:
    case MetadataKind::ErrorObject:
      swift::crash("Swift mirror lookup failure");
    }

    // If we have an unknown kind of type, or a type without special handling,
    // treat it as opaque.
    OpaqueImpl impl;
    return call(&impl);
}

} // end anonymous namespace


// func _getNormalizedType<T>(_: T, type: Any.Type) -> Any.Type
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERFACE
const Metadata *swift_reflectionMirror_normalizedType(OpaqueValue *value,
                                                      const Metadata *type,
                                                      const Metadata *T) {
  return call(value, T, type, [](ReflectionMirrorImpl *impl) { return impl->type; });
}

// func _getChildCount<T>(_: T, type: Any.Type) -> Int
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERFACE
intptr_t swift_reflectionMirror_count(OpaqueValue *value,
                                      const Metadata *type,
                                      const Metadata *T) {
  return call(value, T, type, [](ReflectionMirrorImpl *impl) {
    return impl->count();
  });
}

// We intentionally use a non-POD return type with this entry point to give
// it an indirect return ABI for compatibility with Swift.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
// func _getChild<T>(
//   of: T,
//   type: Any.Type,
//   index: Int,
//   outName: UnsafeMutablePointer<UnsafePointer<CChar>?>,
//   outFreeFunc: UnsafeMutablePointer<NameFreeFunc?>
// ) -> Any
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERFACE
AnyReturn swift_reflectionMirror_subscript(OpaqueValue *value, const Metadata *type,
                                           intptr_t index,
                                           const char **outName,
                                           void (**outFreeFunc)(const char *),
                                           const Metadata *T) {
  return call(value, T, type, [&](ReflectionMirrorImpl *impl) {
    return impl->subscript(index, outName, outFreeFunc);
  });
}
#pragma clang diagnostic pop

// func _getDisplayStyle<T>(_: T) -> CChar
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERFACE
char swift_reflectionMirror_displayStyle(OpaqueValue *value, const Metadata *T) {
  return call(value, T, nullptr, [](ReflectionMirrorImpl *impl) { return impl->displayStyle(); });
}

// func _getEnumCaseName<T>(_ value: T) -> UnsafePointer<CChar>?
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERFACE
const char *swift_EnumCaseName(OpaqueValue *value, const Metadata *T) {
  return call(value, T, nullptr, [](ReflectionMirrorImpl *impl) { return impl->enumCaseName(); });
}

// func _opaqueSummary(_ metadata: Any.Type) -> UnsafePointer<CChar>?
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERFACE
const char *swift_OpaqueSummary(const Metadata *T) {
  switch (T->getKind()) {
    case MetadataKind::Class:
    case MetadataKind::Struct:
    case MetadataKind::Enum:
    case MetadataKind::Optional:
    case MetadataKind::Metatype:
      return nullptr;
    case MetadataKind::Opaque:
      return "(Opaque Value)";
    case MetadataKind::Tuple:
      return "(Tuple)";
    case MetadataKind::Function:
      return "(Function)";
    case MetadataKind::Existential:
      return "(Existential)";
    case MetadataKind::ObjCClassWrapper:
      return "(Objective-C Class Wrapper)";
    case MetadataKind::ExistentialMetatype:
      return "(Existential Metatype)";
    case MetadataKind::ForeignClass:
      return "(Foreign Class)";
    case MetadataKind::HeapLocalVariable:
      return "(Heap Local Variable)";
    case MetadataKind::HeapGenericLocalVariable:
      return "(Heap Generic Local Variable)";
    case MetadataKind::ErrorObject:
      return "(ErrorType Object)";
  }

  return "(Unknown)";
}

#if SWIFT_OBJC_INTEROP
// func _getQuickLookObject<T>(_: T) -> AnyObject?
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERFACE
id swift_reflectionMirror_quickLookObject(OpaqueValue *value, const Metadata *T) {
  return call(value, T, nullptr, [](ReflectionMirrorImpl *impl) { return impl->quickLookObject(); });
}
#endif
