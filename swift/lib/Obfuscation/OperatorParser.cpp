#include "swift/Obfuscation/OperatorParser.h"
#include "swift/Obfuscation/FunctionDeclarationParser.h"
#include "swift/Obfuscation/ParameterDeclarationParser.h"
#include "swift/Obfuscation/Utils.h"

namespace swift {
namespace obfuscation {

using SingleSymbolOrError = llvm::Expected<Symbol>;

SingleSymbolOrError parse(const OperatorDecl* Declaration) {

  auto ModuleNameAndParts = moduleNameAndParts(Declaration);
  std::string ModuleName = ModuleNameAndParts.first;
  std::vector<std::string> Parts = ModuleNameAndParts.second;
  std::string SymbolName = Declaration->getName().str().str();
  Parts.push_back("operator." + SymbolName);

  return Symbol(combineIdentifier(Parts),
                SymbolName,
                ModuleName,
                SymbolType::Operator);
}

SymbolsOrError parseOperator(const FuncDecl* Declaration, CharSourceRange Range) {

    std::vector<SymbolWithRange> Symbols;

    auto ParametersSymbolsOrError =
    parseSeparateFunctionDeclarationForParameters(Declaration);
    if (auto Error = ParametersSymbolsOrError.takeError()) {
        return std::move(Error);
    }
    copyToVector(ParametersSymbolsOrError.get(), Symbols);

    auto ModuleNameAndParts = moduleNameAndParts(Declaration);
    std::string ModuleName = ModuleNameAndParts.first;

    if (auto OperatorDecl = Declaration->getOperatorDecl()) {
        auto OperatorModuleName = moduleName(OperatorDecl);
        if (ModuleName != OperatorModuleName) {
            return Symbols;
        }
    }

    std::vector<std::string> Parts = ModuleNameAndParts.second;
    std::string SymbolName = declarationName(Declaration);

    auto IdentifierParts = functionIdentifierParts(Declaration,
                                                   ModuleName,
                                                   SymbolName);
    ModuleName = IdentifierParts.first;
    Parts.push_back("operator." + SymbolName);

    Symbol Symbol(combineIdentifier(Parts),
                  SymbolName,
                  ModuleName,
                  SymbolType::Operator);

    Symbols.push_back(SymbolWithRange(Symbol, Range));
    return Symbols;
}

} //namespace obfuscation
} //namespace swift
