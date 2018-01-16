#include "swift/Obfuscation/DeclarationParser.h"
#include "swift/Obfuscation/Utils.h"

#include <sstream>
#include <string>
#include <vector>

namespace swift {
namespace obfuscation {

std::string identifierFromParts(std::vector<std::string> &Parts) {
  if (Parts.empty()) {
    return "";
  } else if (Parts.size() == 1) {
    return Parts[0];
  } else {
    //TODO: can we rewrite it to use llvm:raw_string_ostream?
    std::stringstream Result;
    std::copy(Parts.cbegin(),
              Parts.cend(),
              std::ostream_iterator<std::string>(Result, "."));
    return Result.str();
  }
}

llvm::Expected<Symbol> parse(const swift::NominalTypeDecl* Declaration) {
  std::vector<std::string> Parts;
  std::string SymbolName;
  std::string ModuleName = Declaration->getModuleContext()->getBaseName().getIdentifier().get();
  Parts.push_back("module");
  Parts.push_back(ModuleName);
  if (auto *EnumDeclaration = dyn_cast<EnumDecl>(Declaration)) {
    SymbolName = EnumDeclaration->getDeclaredInterfaceType()->getString();
    Parts.push_back("enum." + SymbolName);
  } else if (auto *ClassDeclaration = dyn_cast<ClassDecl>(Declaration)) {
    SymbolName = ClassDeclaration->getDeclaredInterfaceType()->getString();
    Parts.push_back("class." + SymbolName);
  } else if (auto *ProtocolDeclaration = dyn_cast<ProtocolDecl>(Declaration)) {
    SymbolName = ProtocolDeclaration->getDeclaredInterfaceType()->getString();
    Parts.push_back("protocol." + SymbolName);
  } else if (auto *StructDeclaration = dyn_cast<StructDecl>(Declaration)) {
    SymbolName = StructDeclaration->getDeclaredInterfaceType()->getString();
    Parts.push_back("struct." + SymbolName);
  } else {
    return stringError("found unsupported declaration type");
  }
  
  return Symbol(identifierFromParts(Parts), SymbolName, ModuleName);
}

llvm::Expected<Symbol> parse(const swift::ValueDecl* Declaration) {
  return stringError("found unsupported declaration type");
}

llvm::Expected<Symbol> extractSymbol(Decl* Declaration) {
  
  std::unique_ptr<llvm::Expected<Symbol>> SymbolOrError(nullptr);
  if (const auto *NominalTypeDeclaration = dyn_cast<NominalTypeDecl>(Declaration)) {
    SymbolOrError = llvm::make_unique<llvm::Expected<Symbol>>(parse(NominalTypeDeclaration));
  } else if (const auto *ValueDeclaration = dyn_cast<ValueDecl>(Declaration)) {
    SymbolOrError = llvm::make_unique<llvm::Expected<Symbol>>(parse(ValueDeclaration));
  } else {
    return stringError("unsupported declaration type");
  }
  
  if (auto Error = SymbolOrError->takeError()) {
    return std::move(Error);
  }
  return SymbolOrError->get();
}
  
} //namespace obfuscation
} //namespace swift
