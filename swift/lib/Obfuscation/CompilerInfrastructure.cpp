#include "swift/Obfuscation/CompilerInfrastructure.h"

#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <vector>

namespace swift {
namespace obfuscation {

struct CompilerInvocationConfiguration {
  std::string ModuleName;
  std::string MainExecutablePath;
  std::string SdkPath;
  std::vector<std::string> InputFilenames;
  std::vector<SearchPathOptions::FrameworkSearchPath> Paths;
  
  CompilerInvocationConfiguration(const FilesJson &FilesJson,
                                  std::string MainExecutablePath)
  : ModuleName(FilesJson.Module.Name),
  MainExecutablePath(MainExecutablePath),
  SdkPath(FilesJson.Sdk.Path),
  InputFilenames(FilesJson.Filenames) {
    std::vector<SearchPathOptions::FrameworkSearchPath> Paths;
    for (const auto &Framework : FilesJson.ExplicitelyLinkedFrameworks) {
      auto Path = SearchPathOptions::FrameworkSearchPath(Framework.Path,
                                                         false);
      Paths.push_back(Path);
    }
    this->Paths = Paths;
  }
};

CompilerInvocation
  createInvocation(const CompilerInvocationConfiguration &Configuration) {
    CompilerInvocation Invocation;
    Invocation.setModuleName(Configuration.ModuleName);
    Invocation.setMainExecutablePath(Configuration.MainExecutablePath);
    for (const auto &InputFilename : Configuration.InputFilenames) {
      Invocation.addInputFilename(InputFilename);
    }
    Invocation.getLangOptions().AttachCommentsToDecls = true;
    Invocation.setFrameworkSearchPaths(Configuration.Paths);
    Invocation.setSDKPath(Configuration.SdkPath);
    return Invocation;
}

llvm::Error
setupCompilerInstance(CompilerInstance &CompilerInstance,
                      const FilesJson &FilesJson,
                      std::string MainExecutablePath) {
  auto Configuration = CompilerInvocationConfiguration(FilesJson,
                                                       MainExecutablePath);
  auto Invocation = createInvocation(Configuration);
  PrintingDiagnosticConsumer PrintDiags;
  CompilerInstance.addDiagnosticConsumer(&PrintDiags);
  if (CompilerInstance.setup(Invocation)) {
    return llvm::make_error<llvm::StringError>("Error during JSON file read",
                                               std::error_code(1, std::generic_category()));
  }
  CompilerInstance.performSema();
  return llvm::Error::success();
}

} //namespace obfuscation
} //namespace swift
