#include "swift/Obfuscation/DataStructures.h"
#include "swift/Obfuscation/Utils.h"

using namespace swift::obfuscation;

namespace swift {
namespace obfuscation {

Symbol::Symbol(const std::string &Identifier,
       const std::string &Name,
       const std::string &Module)
: Identifier(Identifier), Name(Name), Module(Module) {};

bool Symbol::operator< (const Symbol &Right) const {
  return Identifier < Right.Identifier;
}
  
bool Symbol::operator== (const Symbol &Right) const {
  return Identifier == Right.Identifier
  && Name == Right.Name
  && Module == Right.Module;
}
  
SymbolRenaming::SymbolRenaming(const std::string &Identifier,
                               const std::string &OriginalName,
                               const std::string &ObfuscatedName,
                               const std::string &Module)
: Identifier(Identifier), OriginalName(OriginalName), ObfuscatedName(ObfuscatedName), Module(Module) {};

bool SymbolRenaming::operator== (const SymbolRenaming &Right) const {
  return Identifier == Right.Identifier
  && ObfuscatedName == Right.ObfuscatedName
  && OriginalName == Right.OriginalName
  && Module == Right.Module;
}

const char* pointerToRangeValue(const SymbolWithRange &Symbol) {
  auto Pointer = Symbol.Range.getStart().getOpaquePointerValue();
  return static_cast<const char *>(Pointer);
}

bool SymbolWithRange::operator< (const SymbolWithRange &Right) const {
  auto less = std::less<const char *>();
  if (const auto* RangeValuePointer = pointerToRangeValue(*this)) {
    if (const auto* RightRangeValuePointer = pointerToRangeValue(Right)) {
      auto isRangeLess = less(RangeValuePointer, RightRangeValuePointer);
      return Symbol < Right.Symbol || isRangeLess;
    }
  }
  assert(false && "Comparing Symbols with Ranges requires Ranges Start "
         "Location Values Pointers to be of const char type");
}
  
} //namespace obfuscation
} //namespace swift

namespace llvm {
namespace yaml {

void MappingTraits<FilesJson>::mapping(IO &Io, FilesJson &Object) {
  Io.mapRequired("project", Object.Project);
  Io.mapRequired("module", Object.Module);
  Io.mapRequired("sdk", Object.Sdk);
  Io.mapRequired("filenames", Object.Filenames);
  Io.mapRequired("systemLinkedFrameworks", Object.SystemLinkedFrameworks);
  Io.mapRequired("explicitelyLinkedFrameworks",
                 Object.ExplicitelyLinkedFrameworks);
}
  
void MappingTraits<Project>::mapping(IO &Io, Project &Object) {
  Io.mapRequired("rootPath", Object.RootPath);
}

using ObfuscationModule = swift::obfuscation::Module;
void MappingTraits<ObfuscationModule>::mapping(IO &Io,
                                               ObfuscationModule &Object) {
  Io.mapRequired("name", Object.Name);
}

void MappingTraits<Sdk>::mapping(IO &Io, Sdk &Object) {
  Io.mapRequired("name", Object.Name);
  Io.mapRequired("path", Object.Path);
}

using ELF = ExplicitelyLinkedFrameworks;
void MappingTraits<ELF>::mapping(IO &Io, ELF &Object) {
  Io.mapRequired("name", Object.Name);
  Io.mapRequired("path", Object.Path);
}

void MappingTraits<SymbolsJson>::mapping(IO &Io, SymbolsJson &Object) {
  Io.mapRequired("symbols", Object.Symbols);
}

void MappingTraits<Symbol>::mapping(IO &Io, Symbol &Object) {
  Io.mapRequired("identifier", Object.Identifier);
  Io.mapRequired("name", Object.Name);
  Io.mapRequired("module", Object.Module);
}

void MappingTraits<RenamesJson>::mapping(IO &Io, RenamesJson &Object) {
  Io.mapRequired("symbols", Object.Symbols);
}

void MappingTraits<SymbolRenaming>::mapping(IO &Io, SymbolRenaming &Object) {
  Io.mapRequired("identifier", Object.Identifier);
  Io.mapRequired("originalName", Object.OriginalName);
  Io.mapRequired("obfuscatedName", Object.ObfuscatedName);
  Io.mapRequired("module", Object.Module);
}

template <typename U>
size_t SequenceTraits<std::vector<U>>::size(IO &Io, std::vector<U> &Vec) {
  return Vec.size();
}

template <typename U>
U& SequenceTraits<std::vector<U>>::element(IO &Io,
                                           std::vector<U> &Vec,
                                           size_t Index) {
  if (Vec.size() <= Index) {
    Vec.resize(Index + 1);
  }
  return Vec[Index];
}
  
template<class T>
Expected<T> deserialize(StringRef Json) {
  Input Input(Json);
  T Deserialized;
  Input >> Deserialized;
  if (auto ErrorCode = Input.error()) {
    return stringError("Error during JSON parse", ErrorCode);
  }
  return Deserialized;
}
  
template Expected<FilesJson> deserialize(StringRef Json);
template Expected<Project> deserialize(StringRef Json);
template Expected<ObfuscationModule> deserialize(StringRef Json);
template Expected<Sdk> deserialize(StringRef Json);
template Expected<ELF> deserialize(StringRef Json);
template Expected<SymbolsJson> deserialize(StringRef Json);
template Expected<Symbol> deserialize(StringRef Json);
template Expected<RenamesJson> deserialize(StringRef Json);
template Expected<SymbolRenaming> deserialize(StringRef Json);

} // namespace yaml
} // namespace llvm

namespace swift {
namespace json {

void ObjectTraits<SymbolsJson>::mapping(Output &Out, SymbolsJson &Object) {
  Out.mapRequired("symbols", Object.Symbols);
}

void ObjectTraits<Symbol>::mapping(Output &Out, Symbol &Object) {
  Out.mapRequired("name", Object.Name);
  Out.mapRequired("identifier", Object.Identifier);
  Out.mapRequired("module", Object.Module);
}

void ObjectTraits<RenamesJson>::mapping(Output &Out, RenamesJson &Object) {
  Out.mapRequired("symbols", Object.Symbols);
}

void ObjectTraits<SymbolRenaming>::mapping(Output &Out,
                                           SymbolRenaming &Object) {
  Out.mapRequired("identifier", Object.Identifier);
  Out.mapRequired("originalName", Object.OriginalName);
  Out.mapRequired("obfuscatedName", Object.ObfuscatedName);
  Out.mapRequired("module", Object.Module);
}

template<class T>
std::string serialize(T &Object) {
    std::string OutputString;
    llvm::raw_string_ostream OutputStringStream(OutputString);
    swift::json::Output Output(OutputStringStream);
    Output << Object;
    return OutputStringStream.str();
}

template std::string serialize(SymbolsJson &Object);
template std::string serialize(Symbol &Object);
template std::string serialize(RenamesJson &Object);
template std::string serialize(SymbolRenaming &Object);

} // namespace json
} // namespace swift

