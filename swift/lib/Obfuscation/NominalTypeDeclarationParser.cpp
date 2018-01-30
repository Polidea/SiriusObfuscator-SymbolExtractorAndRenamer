#include "swift/Obfuscation/NominalTypeDeclarationParser.h"
#include "swift/Obfuscation/Utils.h"

namespace swift {
namespace obfuscation {
    
llvm::Expected<ModuleNameAndParts>
nominalTypeIdentifierParts(const NominalTypeDecl *Declaration,
                           const std::string &ModuleName,
                           const std::string &SymbolName) {
  
  std::vector<std::string> Parts;
  
  if (auto *EnumDeclaration = dyn_cast<EnumDecl>(Declaration)) {
    Parts.push_back("enum." + SymbolName);
  } else if (auto *ClassDeclaration = dyn_cast<ClassDecl>(Declaration)) {
    Parts.push_back("class." + SymbolName);
  } else if (auto *ProtocolDeclaration = dyn_cast<ProtocolDecl>(Declaration)) {
    Parts.push_back("protocol." + SymbolName);
  } else if (auto *StructDeclaration = dyn_cast<StructDecl>(Declaration)) {
    Parts.push_back("struct." + SymbolName);
  } else {
    return stringError("found unsupported declaration type");
  }
  return std::make_pair(ModuleName, Parts);
}

SingleSymbolOrError parse(const NominalTypeDecl* Declaration) {
  
  auto ModuleNameAndParts = moduleNameAndParts(Declaration);
  std::string ModuleName = ModuleNameAndParts.first;
  std::vector<std::string> Parts = ModuleNameAndParts.second;
  std::string SymbolName = typeName(Declaration);
  
  auto NominalTypeParts = nominalTypeIdentifierParts(Declaration,
                                                     ModuleName,
                                                     SymbolName);
  if (auto Error = NominalTypeParts.takeError()) {
    return std::move(Error);
  }
  
  return Symbol(combineIdentifier(NominalTypeParts.get().second),
                SymbolName,
                NominalTypeParts.get().first,
                SymbolType::Type);
}

} //namespace obfuscation
} //namespace swift
