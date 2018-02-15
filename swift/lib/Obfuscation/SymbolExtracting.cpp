#include "swift/Obfuscation/SymbolExtracting.h"
#include "swift/Obfuscation/DataStructures.h"
#include "swift/Obfuscation/CompilerInfrastructure.h"
#include "swift/Obfuscation/SourceFileWalker.h"
#include "swift/Obfuscation/Utils.h"

#include <vector>
#include <set>

namespace swift {
namespace obfuscation {

llvm::Expected<SymbolsJson>
extractSymbols(const FilesJson &FilesJson,
               std::string MainExecutablePath,
               llvm::raw_ostream &DiagnosticStream) {
  
  CompilerInstance CompilerInstance;
  auto Error = setupCompilerInstance(CompilerInstance,
                                     FilesJson,
                                     MainExecutablePath,
                                     DiagnosticStream);
  if (Error) {
    return std::move(Error);
  }
  
  SymbolsJson Json;

  using FileWithName = std::pair<std::string, SourceFile *>;
  std::vector<FileWithName> Files;
  for (auto* Unit : CompilerInstance.getMainModule()->getFiles()) {
    if (auto* Current = dyn_cast<SourceFile>(Unit)) {
      Files.push_back(std::make_pair(Current->getFilename().str(), Current));
    }
  }
  std::sort(Files.begin(),
            Files.end(),
            [](const FileWithName &Left, const FileWithName &Right) {
              return Left.first < Right.first;
            });

  std::set<IndexedSymbolWithRange,
           IndexedSymbolWithRange::SymbolCompare> Symbols;

  for (auto &Unit : Files) {
    auto CurrentSymbols = walkAndCollectSymbols(*Unit.second);
    std::vector<IndexedSymbolWithRange> SortedSymbols;
    copyToVector(CurrentSymbols, SortedSymbols);
    std::sort(SortedSymbols.begin(),
              SortedSymbols.end(),
              [](const IndexedSymbolWithRange &Left,
                 const IndexedSymbolWithRange &Right) {
                return Left.Index < Right.Index;
              });
    copyToSet(SortedSymbols, Symbols);
  }

  std::vector<IndexedSymbolWithRange> Result;
  copyToVector(Symbols, Result);

  std::sort(Result.begin(),
            Result.end(),
            [](const IndexedSymbolWithRange &Left,
               const IndexedSymbolWithRange &Right) {
              return Left.Index < Right.Index;
            });
  std::transform(Result.cbegin(),
                 Result.cend(),
                 std::back_inserter(Json.Symbols),
                 [](const IndexedSymbolWithRange &Symbol) -> struct Symbol {
                   return Symbol.SymbolWithRange.Symbol;
                 });
  return Json;
}

} //namespace obfuscation
} //namespace swift
