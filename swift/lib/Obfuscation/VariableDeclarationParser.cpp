#include "swift/Obfuscation/VariableDeclarationParser.h"
#include "swift/Obfuscation/FunctionDeclarationParser.h"
#include "swift/Obfuscation/NominalTypeDeclarationParser.h"
#include "swift/Obfuscation/DeclarationParsingUtils.h"
#include "swift/Obfuscation/Utils.h"

namespace swift {
namespace obfuscation {

SingleSymbolOrError
parseOverridenDeclaration(const VarDecl *Declaration,
                          const std::string &ModuleName) {
  std::set<std::string> Modules;
  auto Base = baseOverridenDeclarationWithModules(Declaration, Modules);
  if (Modules.size() == 1 && Modules.count(ModuleName) == 1) {
    return parse(Base);
  } else {
    return stringError("only overriding properties from the same module "
                       "might be safely obfuscated");
  }
}

llvm::Error appendContextToParts(const VarDecl *Declaration,
                                    std::string &ModuleName,
                                    std::vector<std::string> &Parts) {
  auto ProtocolRequirements = Declaration->getSatisfiedProtocolRequirements();
  auto *ProtocolDeclaration =
  dyn_cast<ProtocolDecl>(Declaration->getDeclContext());
  // TODO: for now, we're renaming all protocol properties with the same name
  // to the same obfuscated name. this should be improved in the future.
  if (!ProtocolRequirements.empty() || ProtocolDeclaration != nullptr) {
    auto UpdatedModuleName = ProtocolRequirements.empty() ?
    moduleName(ProtocolDeclaration) : moduleName(ProtocolRequirements.front());
    ModuleName = UpdatedModuleName;
    Parts.push_back("protocol");
    
  } else if (auto *FunctionDeclaration =
             dyn_cast<FuncDecl>(Declaration->getDeclContext())) {
    std::string FunctionName = functionName(FunctionDeclaration);
    auto ModuleAndParts = functionIdentifierParts(FunctionDeclaration,
                                                  ModuleName,
                                                  FunctionName);
    copyToVector(ModuleAndParts.second, Parts);
    
  } else if (auto *NominalTypeDeclaration =
             dyn_cast<NominalTypeDecl>(Declaration->getDeclContext())) {
    std::string TypeName = typeName(NominalTypeDeclaration);
    auto ModuleAndParts = nominalTypeIdentifierParts(NominalTypeDeclaration,
                                                     ModuleName,
                                                     TypeName);
    if (auto Error = ModuleAndParts.takeError()) {
      return Error;
    }
    copyToVector(ModuleAndParts.get().second, Parts);
  }
  
  return llvm::Error::success();
}

SingleSymbolOrError parse(const VarDecl* Declaration) {
  
  auto ModuleNameAndParts = moduleNameAndParts(Declaration);
  std::string ModuleName = ModuleNameAndParts.first;
  std::vector<std::string> Parts = ModuleNameAndParts.second;
  std::string SymbolName = Declaration->getName().str().str();
  
  if (Declaration->getOverriddenDecl() != nullptr) {
    return parseOverridenDeclaration(Declaration, ModuleName);
  }
  
  if (auto Error = appendContextToParts(Declaration, ModuleName, Parts)) {
    return std::move(Error);
  }
  
  if (Declaration->isStatic()) {
    Parts.push_back("static");
  }
  
  Parts.push_back("variable." + SymbolName);
  
  return Symbol(combineIdentifier(Parts),
                SymbolName,
                ModuleName,
                SymbolType::Variable);
}
  
} //namespace obfuscation
} //namespace swift
