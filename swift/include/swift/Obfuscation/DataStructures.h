#ifndef DataStructures_h
#define DataStructures_h

#include "swift/Frontend/Frontend.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/YAMLParser.h"
#include "swift/Basic/JSONSerialization.h"

namespace swift {
  namespace obfuscation {

    struct Module {
      std::string name;
    };
    
    struct Sdk {
      std::string name;
      std::string path;
    };
    
    struct ExplicitelyLinkedFrameworks {
      std::string name;
      std::string path;
    };
    
    struct FilesJson {
      Module module;
      Sdk sdk;
      std::vector<std::string> filenames;
      std::vector<std::string> systemLinkedFrameworks;
      std::vector<ExplicitelyLinkedFrameworks> explicitelyLinkedFrameworks;
    };
    
    struct Symbol {
      std::string identifier;
      std::string name;
      
      bool operator< (const Symbol &right) const;
    };
    
    struct SymbolsJson {
      std::vector<Symbol> symbols;
    };
    
    struct SymbolRenaming {
      std::string identifier;
      std::string originalName;
      std::string obfuscatedName;
      
      bool operator< (const SymbolRenaming &right) const;
    };
    
    struct RenamesJson {
      std::vector<SymbolRenaming> symbols;
    };
    
  } //namespace obfuscation
} //namespace swift

using namespace swift::obfuscation;

// MARK: - Deserialization

namespace llvm {
  namespace yaml {
    
    template <>
    struct MappingTraits<FilesJson> {
      static void mapping(IO &io, FilesJson &info);
    };
    
    template <>
    struct MappingTraits<swift::obfuscation::Module> {
      static void mapping(IO &io, swift::obfuscation::Module &info);
    };
    
    template <>
    struct MappingTraits<Sdk> {
      static void mapping(IO &io, Sdk &info);
    };
    
    template <>
    struct MappingTraits<ExplicitelyLinkedFrameworks> {
      static void mapping(IO &io, ExplicitelyLinkedFrameworks &info);
    };
    
    template <>
    struct MappingTraits<SymbolsJson> {
      static void mapping(IO &io, SymbolsJson &info);
    };
    
    template <>
    struct MappingTraits<Symbol> {
      static void mapping(IO &io, Symbol &info);
    };
    
    template <>
    struct MappingTraits<RenamesJson> {
      static void mapping(IO &io, RenamesJson &info);
    };
    
    template <>
    struct MappingTraits<SymbolRenaming> {
      static void mapping(IO &io, SymbolRenaming &info);
    };
    
    template <typename U>
    struct SequenceTraits<std::vector<U>> {
      static size_t size(IO &Io, std::vector<U> &Vec);
      static U& element(IO &Io, std::vector<U> &Vec, size_t Index);
    };
    
  } // namespace yaml
} // namespace llvm

// MARK: - Serialization

namespace swift {
  namespace json  {
    
    template <>
    struct ObjectTraits<SymbolsJson> {
      static void mapping(Output &out, SymbolsJson &s);
    };
    
    template <>
    struct ObjectTraits<Symbol> {
      static void mapping(Output &out, Symbol &s);
    };
    
    template <>
    struct ObjectTraits<RenamesJson> {
      static void mapping(Output &out, RenamesJson &s);
    };
    
    template <>
    struct ObjectTraits<SymbolRenaming> {
      static void mapping(Output &out, SymbolRenaming &s);
    };
    
  } // namespace json
} // namespace swift

#endif /* DataStructures_h */
