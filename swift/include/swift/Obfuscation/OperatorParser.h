#ifndef OperatorParser_h
#define OperatorParser_h

#include "swift/Obfuscation/DeclarationParsingUtils.h"

namespace swift {
namespace obfuscation {

SingleSymbolOrError parse(const OperatorDecl* Declaration);
SymbolsOrError parseOperator(const FuncDecl* Declaration, CharSourceRange Range);

} //namespace obfuscation
} //namespace swift

#endif /* OperatorParser_h */
