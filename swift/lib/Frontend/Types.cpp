//===--- Types.cpp - Driver input & temporary type information ------------===//
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

#include "swift/Frontend/FileTypes.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/ErrorHandling.h"

using namespace swift;
using namespace swift::file_types;

struct TypeInfo {
  const char *Name;
  const char *Flags;
  const char *TempSuffix;
};

static const TypeInfo TypeInfos[] = {
#define TYPE(NAME, ID, TEMP_SUFFIX, FLAGS) \
  { NAME, FLAGS, TEMP_SUFFIX },
#include "swift/Frontend/Types.def"
};

static const TypeInfo &getInfo(unsigned Id) {
  assert(Id >= 0 && Id < TY_INVALID && "Invalid Type ID.");
  return TypeInfos[Id];
}

StringRef file_types::getTypeName(ID Id) { return getInfo(Id).Name; }

StringRef file_types::getTypeTempSuffix(ID Id) {
  return getInfo(Id).TempSuffix;
}

ID file_types::lookupTypeForExtension(StringRef Ext) {
  if (Ext.empty())
    return TY_INVALID;
  assert(Ext.front() == '.' && "not a file extension");
  return llvm::StringSwitch<file_types::ID>(Ext.drop_front())
#define TYPE(NAME, ID, SUFFIX, FLAGS) \
           .Case(SUFFIX, TY_##ID)
#include "swift/Frontend/Types.def"
      .Default(TY_INVALID);
}

ID file_types::lookupTypeForName(StringRef Name) {
  return llvm::StringSwitch<file_types::ID>(Name)
#define TYPE(NAME, ID, SUFFIX, FLAGS) \
           .Case(NAME, TY_##ID)
#include "swift/Frontend/Types.def"
      .Default(TY_INVALID);
}

bool file_types::isTextual(ID Id) {
  switch (Id) {
  case file_types::TY_Swift:
  case file_types::TY_SIL:
  case file_types::TY_Dependencies:
  case file_types::TY_Assembly:
  case file_types::TY_RawSIL:
  case file_types::TY_LLVM_IR:
  case file_types::TY_ObjCHeader:
  case file_types::TY_AutolinkFile:
  case file_types::TY_ImportedModules:
  case file_types::TY_TBD:
  case file_types::TY_ModuleTrace:
  case file_types::TY_OptRecord:
    return true;
  case file_types::TY_Image:
  case file_types::TY_Object:
  case file_types::TY_dSYM:
  case file_types::TY_PCH:
  case file_types::TY_SIB:
  case file_types::TY_RawSIB:
  case file_types::TY_SwiftModuleFile:
  case file_types::TY_SwiftModuleDocFile:
  case file_types::TY_LLVM_BC:
  case file_types::TY_SerializedDiagnostics:
  case file_types::TY_ClangModuleFile:
  case file_types::TY_SwiftDeps:
  case file_types::TY_Nothing:
  case file_types::TY_Remapping:
  case file_types::TY_IndexData:
    return false;
  case file_types::TY_INVALID:
    llvm_unreachable("Invalid type ID.");
  }

  // Work around MSVC warning: not all control paths return a value
  llvm_unreachable("All switch cases are covered");
}

bool file_types::isAfterLLVM(ID Id) {
  switch (Id) {
  case file_types::TY_Assembly:
  case file_types::TY_LLVM_IR:
  case file_types::TY_LLVM_BC:
  case file_types::TY_Object:
    return true;
  case file_types::TY_Swift:
  case file_types::TY_PCH:
  case file_types::TY_ImportedModules:
  case file_types::TY_TBD:
  case file_types::TY_SIL:
  case file_types::TY_Dependencies:
  case file_types::TY_RawSIL:
  case file_types::TY_ObjCHeader:
  case file_types::TY_AutolinkFile:
  case file_types::TY_Image:
  case file_types::TY_dSYM:
  case file_types::TY_SIB:
  case file_types::TY_RawSIB:
  case file_types::TY_SwiftModuleFile:
  case file_types::TY_SwiftModuleDocFile:
  case file_types::TY_SerializedDiagnostics:
  case file_types::TY_ClangModuleFile:
  case file_types::TY_SwiftDeps:
  case file_types::TY_Nothing:
  case file_types::TY_Remapping:
  case file_types::TY_IndexData:
  case file_types::TY_ModuleTrace:
  case file_types::TY_OptRecord:
    return false;
  case file_types::TY_INVALID:
    llvm_unreachable("Invalid type ID.");
  }

  // Work around MSVC warning: not all control paths return a value
  llvm_unreachable("All switch cases are covered");
}

bool file_types::isPartOfSwiftCompilation(ID Id) {
  switch (Id) {
  case file_types::TY_Swift:
  case file_types::TY_SIL:
  case file_types::TY_RawSIL:
  case file_types::TY_SIB:
  case file_types::TY_RawSIB:
    return true;
  case file_types::TY_Assembly:
  case file_types::TY_LLVM_IR:
  case file_types::TY_LLVM_BC:
  case file_types::TY_Object:
  case file_types::TY_Dependencies:
  case file_types::TY_ObjCHeader:
  case file_types::TY_AutolinkFile:
  case file_types::TY_PCH:
  case file_types::TY_ImportedModules:
  case file_types::TY_TBD:
  case file_types::TY_Image:
  case file_types::TY_dSYM:
  case file_types::TY_SwiftModuleFile:
  case file_types::TY_SwiftModuleDocFile:
  case file_types::TY_SerializedDiagnostics:
  case file_types::TY_ClangModuleFile:
  case file_types::TY_SwiftDeps:
  case file_types::TY_Nothing:
  case file_types::TY_Remapping:
  case file_types::TY_IndexData:
  case file_types::TY_ModuleTrace:
  case file_types::TY_OptRecord:
    return false;
  case file_types::TY_INVALID:
    llvm_unreachable("Invalid type ID.");
  }

  // Work around MSVC warning: not all control paths return a value
  llvm_unreachable("All switch cases are covered");
}
