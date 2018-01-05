#ifndef FileIO_h
#define FileIO_h

#include "swift/Frontend/Frontend.h"
#include "swift/Obfuscation/Obfuscation.h"

namespace swift {
  namespace obfuscation {
    
    template<class T>
    llvm::ErrorOr<T> parseJson(std::string PathToJson);

    template<class T>
    int writeSymbolsToFile(T Symbols, std::string PathToOutput);
    
  } //namespace obfuscation
} //namespace swift

#endif /* FileIO_h */
