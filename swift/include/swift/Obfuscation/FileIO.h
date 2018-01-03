#ifndef FileIO_h
#define FileIO_h

#include "swift/Frontend/Frontend.h"
#include "swift/Obfuscation/Obfuscation.h"

namespace swift {
  namespace obfuscation {
    
    llvm::ErrorOr<CompilerInvocationConfiguration> parseFilesJson(std::string PathToJson, std::string MainExecutablePath);
    
    int writeSymbolsToFile(SymbolsJson Symbols, std::string PathToOutput);
    
  } //namespace obfuscation
} //namespace swift

#endif /* FileIO_h */
