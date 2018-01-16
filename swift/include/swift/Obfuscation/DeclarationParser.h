#ifndef DeclarationParser_h
#define DeclarationParser_h

#include "swift/Frontend/Frontend.h"
#include "swift/Obfuscation/DataStructures.h"

namespace swift {
namespace obfuscation {

llvm::Expected<Symbol> extractSymbol(Decl* Declaration);
    
} //namespace obfuscation
} //namespace swift

#endif /* DeclarationParser_h */
