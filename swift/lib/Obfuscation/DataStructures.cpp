#include "swift/Obfuscation/DataStructures.h"
#include <stdio.h>
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace swift::obfuscation;

namespace swift {
  namespace obfuscation {
    
    bool Symbol::operator< (const Symbol &right) const {
      return (symbol < right.symbol);
    }
    
  } //namespace obfuscation
} //namespace swift

namespace llvm {
  namespace yaml {
    
    void MappingTraits<FilesJson>::mapping(IO &io, FilesJson &info) {
      io.mapRequired("module", info.module);
      io.mapRequired("sdk", info.sdk);
      io.mapRequired("filenames", info.filenames);
      io.mapRequired("systemLinkedFrameworks", info.systemLinkedFrameworks);
      io.mapRequired("explicitelyLinkedFrameworks", info.explicitelyLinkedFrameworks);
    }
   
    void MappingTraits<swift::obfuscation::Module>::mapping(IO &io, swift::obfuscation::Module &info) {
      io.mapRequired("name", info.name);
    }
    
    void MappingTraits<Sdk>::mapping(IO &io, Sdk &info) {
      io.mapRequired("name", info.name);
      io.mapRequired("path", info.path);
    }
    
    void MappingTraits<ExplicitelyLinkedFrameworks>::mapping(IO &io, ExplicitelyLinkedFrameworks &info) {
      io.mapRequired("name", info.name);
      io.mapRequired("path", info.path);
    }
    
    void MappingTraits<SymbolsJson>::mapping(IO &io, SymbolsJson &info) {
      io.mapRequired("symbols", info.symbols);
    }
    
    void MappingTraits<Symbol>::mapping(IO &io, Symbol &info) {
      io.mapRequired("symbol", info.symbol);
      io.mapRequired("name", info.name);
    }
    
    template <typename U> size_t SequenceTraits<std::vector<U>>::size(IO &Io, std::vector<U> &Vec) {
      return Vec.size();
    }
    
    template <typename U> U& SequenceTraits<std::vector<U>>::element(IO &Io, std::vector<U> &Vec, size_t Index) {
      if (Vec.size() <= Index) {
        Vec.resize(Index + 1);
      }
      return Vec[Index];
    }
    
  } // namespace yaml
} // namespace llvm

namespace swift {
  namespace json {
    
    void ObjectTraits<SymbolsJson>::mapping(Output &out, SymbolsJson &s) {
      out.mapRequired("symbols", s.symbols);
    }
    
    void ObjectTraits<Symbol>::mapping(Output &out, Symbol &s) {
      out.mapRequired("name", s.name);
      out.mapRequired("symbol", s.symbol);
    }
    
  } // namespace json
} // namespace swift

