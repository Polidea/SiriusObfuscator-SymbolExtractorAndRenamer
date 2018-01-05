#ifndef NameMapping_h
#define NameMapping_h

#include "swift/Obfuscation/DataStructures.h"

namespace swift {
  namespace obfuscation {
    
    llvm::ErrorOr<RenamesJson> proposeRenamings(SymbolsJson symbolsJson);

  } //namespace obfuscation
  
} //namespace swift

#endif /* NameMapping_h */
