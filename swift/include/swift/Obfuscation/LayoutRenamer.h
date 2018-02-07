#ifndef LayoutRenamer_h
#define LayoutRenamer_h

#include "swift/Obfuscation/DataStructures.h"
#include "swift/Obfuscation/Utils.h"
#include <vector>
#include <unordered_map>
#include <libxml2/libxml/parser.h>

namespace swift {
namespace obfuscation {
        
class LayoutRenamer {

private:
  
  std::string FileName;
  
  xmlDoc *XmlDocument;
  
  bool shouldRename(const SymbolRenaming &Symbol, const std::string &CustomClass, const std::string &CustomModule);
  
  void performActualRenaming(xmlNode *Node, const std::unordered_map<std::string, SymbolRenaming> &RenamedSymbols, bool &performedRenaming);

public:

  LayoutRenamer(std::string FileName);

  
  /// Performs renaming of layout (.xib and .storyboard) files from FilesJson in the following steps:
  ///
  /// 1. Gathers all renamed symbols (see Renaming.h) and stores them in RenamedSymbols map.
  /// 2. Iterates through FilesJson.LayoutFiles list and performs renames if needed based
  /// on RenamedSymbols map. Layout files are xmls, it looks for a specific attributes
  /// such as "customClass" and retrieves their values. These values are then used to
  /// look up RenamedSymbols map. If a "customClass" value is present inside RenamedSymbols, then
  /// it means that this symbol was renamed in the source code in previous step and it should be
  /// renamed in layout file as well. "customModule" attribute is also taken into account - if it's
  /// present then it's value is compared with symbol's module value (the one found in RenamedSymbols) and
  /// if it's not present then we assume that it's inherited from target project.
  /// 3. Performs actual renaming if all conditions are met.
  /// 4. Saves renamed layout files in OutputPath.
  ///
  /// Typical usage:
  /// \code
  /// LayoutRenamer LayoutRenamer(LayoutFile); // param is a path to layout file
  ///
  /// auto PerformedRenamingOrError = LayoutRenamer.performRenaming(RenamedSymbols, Path);
  ///
  /// if (auto Error = PerformedRenamingOrError.takeError()) {
  ///   return std::move(Error);
  /// }
  ///
  /// auto PerformedRenaming = PerformedRenamingOrError.get();
  ///
  /// if (PerformedRenaming) {
  ///   ...
  /// }
  /// \endcode
  ///
  /// \param RenamedSymbols a map containing all renamed symbols in the source code.
  /// \param OutputPath Path where layout files will be saved after renaming.
  ///
  /// \returns true if file was renamed and false if it wasn't.
  llvm::Expected<bool> performRenaming(std::unordered_map<std::string, SymbolRenaming> RenamedSymbols, std::string OutputPath);

  ~LayoutRenamer();
};
        
} //namespace obfuscation
} //namespace swift

#endif /* LayoutRenamer_h */
