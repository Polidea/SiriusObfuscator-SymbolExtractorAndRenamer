#include "swift/Basic/LLVMInitialize.h"
#include "llvm/Support/FileSystem.h"
#include "swift/AST/Decl.h"
#include "swift/Obfuscation/Obfuscation.h"
#include "swift/Obfuscation/FileIO.h"

#include <iostream>

using namespace swift;
using namespace swift::obfuscation;

namespace options {
  static llvm::cl::opt<std::string>
  FilesJsonPath("filejson", llvm::cl::desc("Name of the file containing File Extractor data"));

  static llvm::cl::opt<std::string>
  SymbolJsonPath("symbolsjson", llvm::cl::desc("Name of the file to write extracted Symbols"));
}

void printSymbols(std::vector<Symbol> Symbols) {
  for (auto Symbol : Symbols) {
    std::cout << "symbol: " << Symbol.symbol << "\n" << "name: " << Symbol.name << "\n";
  }
}

// This function isn't referenced outside its translation unit, but it
// can't use the "static" keyword because its address is used for
// getMainExecutable (since some platforms don't support taking the
// address of main, and some platforms can't implement getMainExecutable
// without being given the address of a function in the main executable).
void anchorForGetMainExecutable() {}

int main(int argc, char *argv[]) {
  INITIALIZE_LLVM(argc, argv);
  std::cout << "Swift obfuscator symbol extractor tool\n";
  
  llvm::cl::ParseCommandLineOptions(argc, argv, "obfuscator-symbol-extractor\n");
  
  if (options::FilesJsonPath.empty()) {
    llvm::errs() << "cannot find Files Extractor json file\n";
    return 1;
  }

  std::string PathToJson = options::FilesJsonPath;
  std::string MainExecutablePath = llvm::sys::fs::getMainExecutable(argv[0],
                                                                    reinterpret_cast<void *>(&anchorForGetMainExecutable));
  llvm::ErrorOr<CompilerInvocationConfiguration> ConfigurationOrError = parseFilesJson(PathToJson, MainExecutablePath);
  if (std::error_code ec = ConfigurationOrError.getError()) {
    return ec.value();
  }
  llvm::ErrorOr<SymbolsJson> SymbolsOrError = extractSymbols(ConfigurationOrError.get());
  if (std::error_code ec = SymbolsOrError.getError()) {
    return ec.value();
  }
  
  printSymbols(SymbolsOrError.get().symbols);
  if (options::SymbolJsonPath.empty()) {
    llvm::errs() << "there is no path to write extracted symbols to\n";
    return 1;
  }
  std::string PathToOutput = options::SymbolJsonPath;
  writeSymbolsToFile(SymbolsOrError.get(), PathToOutput);
  return 0;
}

