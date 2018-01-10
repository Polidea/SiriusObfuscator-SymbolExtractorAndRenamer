#include "swift/Obfuscation/Renaming.h"
#include "swift/Obfuscation/CompilerInfrastructure.h"
#include "swift/Obfuscation/SymbolProvider.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "swift/IDE/Utils.h"

namespace swift {
namespace obfuscation {
  
llvm::Error copyProject(std::string OriginalPath, std::string ObfuscatedPath) {
  // TODO: copy original project so that we have a place to write obfuscated project to
  return llvm::Error::success();
}
  
llvm::Expected<std::string>
computeObfuscatedSourceFilePath(const SourceFile* Current,
                                const FilesJson &FilesJson,
                                std::string ObfuscatedProjectPath) {
  std::string Filename = llvm::sys::path::filename(Current->getFilename());
  // TODO: compute a path for any directory structure, not just this sample one
  std::string Path = ObfuscatedProjectPath + FilesJson.Module.Name + "/" + Filename;
  return Path;
}
  
bool performActualRenaming(swift::SourceFile* Current,
                           const FilesJson &FilesJson,
                           const RenamesJson &RenamesJson,
                           swift::SourceManager &SourceManager,
                           swift::ide::SourceEditOutputConsumer& Editor) {
  
  bool performedRenaming = false;
  
  auto SymbolsWithRanges = findSymbolsWithRanges(*Current);
  
  //TODO: would be way better to have a map here instead of iterating through symbols
  for (const auto &SymbolWithRange : SymbolsWithRanges) {
    for (const auto &Symbol : RenamesJson.Symbols) {
      
      if (SymbolWithRange.Symbol.Identifier == Symbol.Identifier
          && SymbolWithRange.Symbol.Name == Symbol.OriginalName
          && std::string::npos != SymbolWithRange.Symbol.Identifier.find(FilesJson.Module.Name)) {
        
        Editor.ide::SourceEditConsumer::accept(SourceManager,
                                               SymbolWithRange.Range,
                                               StringRef(Symbol.ObfuscatedName));
        performedRenaming = true;
        break;
      }
    }
  }
  return performedRenaming;
}
  
llvm::Expected<FilesList>
performRenaming(std::string MainExecutablePath,
                const FilesJson &FilesJson,
                const RenamesJson &RenamesJson,
                std::string ObfuscatedProjectPath) {
  
  CompilerInstance CI;
  if (auto Error = setupCompilerInstance(CI, FilesJson, MainExecutablePath)) {
    return std::move(Error);
  }
  
  FilesList Files;
  for (auto* Unit : CI.getMainModule()->getFiles()) {
    if (auto* Current = dyn_cast<SourceFile>(Unit)) {
      
      if (auto Error = copyProject("???", ObfuscatedProjectPath)) {
        return std::move(Error);
      }
      
      auto PathOrError = computeObfuscatedSourceFilePath(Current,
                                                         FilesJson,
                                                         ObfuscatedProjectPath);
      if (auto Error = PathOrError.takeError()) {
        return std::move(Error);
      }
      
      auto Path = PathOrError.get();
      auto &SourceManager = Current->getASTContext().SourceMgr;
      std::error_code Error;
      llvm::raw_fd_ostream DescriptorStream(Path, Error, llvm::sys::fs::F_None);
      if (DescriptorStream.has_error() || Error) {
        return llvm::make_error<llvm::StringError>("Cannot open output file: " + Path, Error);
      }
      
      auto BufferId = Current->getBufferID().getValue();
      auto Editor = swift::ide::SourceEditOutputConsumer(SourceManager,
                                                         BufferId,
                                                         DescriptorStream);
      if (performActualRenaming(Current, FilesJson, RenamesJson, SourceManager, Editor)) {
        auto Filename = llvm::sys::path::filename(Path).str();
        Files.push_back(std::pair<std::string, std::string>(Filename, Path));
      }
    }
  }
  
  return Files;
}

} //namespace obfuscation
} //namespace swift
