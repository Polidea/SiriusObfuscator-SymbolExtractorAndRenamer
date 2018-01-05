#ifndef Obfuscation_h
#define Obfuscation_h

#include "swift/Frontend/Frontend.h"
#include "swift/Obfuscation/DataStructures.h"

namespace swift {
  
  namespace obfuscation {
    
    struct CompilerInvocationConfiguration {
      std::string ModuleName;
      std::string MainExecutablePath;
      std::string SdkPath;
      std::vector<std::string> InputFilenames;
      std::vector<SearchPathOptions::FrameworkSearchPath> Paths;
      
      CompilerInvocationConfiguration(std::string ModuleName, std::string MainExecutablePath, std::string SdkPath, std::vector<std::string> InputFilenames, std::vector<SearchPathOptions::FrameworkSearchPath> Paths)
      : ModuleName(ModuleName), MainExecutablePath(MainExecutablePath), SdkPath(SdkPath), InputFilenames(InputFilenames), Paths(Paths) { }
    };
    
    CompilerInvocationConfiguration createCompilerInvocationConfiguration(FilesJson filesJson, std::string MainExecutablePath);
    
    CompilerInvocation createInvocation(CompilerInvocationConfiguration Configuration);
    
    llvm::ErrorOr<SymbolsJson> extractSymbols(CompilerInvocationConfiguration Configuration);
    
  } //namespace obfuscation
  
} //namespace swift

#endif /* Obfuscation_h */
