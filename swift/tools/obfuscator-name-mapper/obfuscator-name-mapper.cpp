#include "swift/Basic/LLVMInitialize.h"
#include "llvm/Support/CommandLine.h"

#include "swift/Obfuscation/Obfuscation.h"
#include "swift/Obfuscation/NameMapping.h"
#include "swift/Obfuscation/FileIO.h"
#include <iostream>

using namespace swift;
using namespace swift::obfuscation;

namespace options {
  static llvm::cl::opt<std::string>
  SymbolsJsonPath("symbolsjson", llvm::cl::desc("Name of the file containing extracted symbols"));
  
  static llvm::cl::opt<std::string>
  RenamesJsonPath("renamesjson", llvm::cl::desc("Name of the output file to write extracted symbols with proposed renamings"));
}

void printRenamings(std::vector<SymbolRenaming> Renamings) {
  for (auto Renaming : Renamings) {
    std::cout << "identifier: " << Renaming.identifier << "\n" << "originalName: " << Renaming.originalName << "\n" << "obfuscatedName: " << Renaming.obfuscatedName << "\n";
  }
}

int main(int argc, char *argv[]) {
  INITIALIZE_LLVM(argc, argv);
  std::cout << "Swift obfuscator name mapper tool\n";
  
  llvm::cl::ParseCommandLineOptions(argc, argv, "obfuscator-name-mapper\n");
  
  if (options::SymbolsJsonPath.empty()) {
    llvm::errs() << "cannot find Symbols json file\n";
    return 1;
  }
  
  std::string PathToJson = options::SymbolsJsonPath;
  llvm::ErrorOr<SymbolsJson> SymbolsJsonOrErr = parseJson<SymbolsJson>(PathToJson);
  if (std::error_code ec = SymbolsJsonOrErr.getError()) {
    return ec.value();
  }
  
  auto RenamingsOrError = proposeRenamings(SymbolsJsonOrErr.get());
  if (std::error_code ec = RenamingsOrError.getError()) {
    return ec.value();
  }
  auto Renamings = RenamingsOrError.get();
  
  printRenamings(Renamings.symbols);
  
  std::string PathToOutput = options::RenamesJsonPath;
  writeSymbolsToFile(Renamings, PathToOutput);
  return 0;
}

