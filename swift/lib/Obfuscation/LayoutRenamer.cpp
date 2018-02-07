#include "swift/Obfuscation/LayoutRenamer.h"

namespace swift {
namespace obfuscation {

LayoutRenamer::LayoutRenamer(std::string FileName) {
  this->FileName = FileName;
  XmlDocument = xmlReadFile(FileName.c_str(), /* encoding */ "UTF-8", /* options */ 0);
}

LayoutRenamer::~LayoutRenamer() {
  if(XmlDocument != nullptr) {
    xmlFreeDoc(XmlDocument);
  }
  xmlCleanupParser();
}

bool LayoutRenamer::shouldRename(const SymbolRenaming &Symbol, const std::string &CustomClass, const std::string &CustomModule) {
  return CustomModule.empty() || CustomModule == Symbol.Module;
}
  
void LayoutRenamer::performActualRenaming(xmlNode *Node, const std::unordered_map<std::string, SymbolRenaming> &RenamedSymbols, bool &performedRenaming) {
  const auto *CustomClassAttributeName = reinterpret_cast<const xmlChar *>("customClass");
  const auto *CustomModuleAttributeName = reinterpret_cast<const xmlChar *>("customModule");

  for (xmlNode *CurrentNode = Node; CurrentNode != nullptr; CurrentNode = CurrentNode->next) {

    if (CurrentNode->type == XML_ELEMENT_NODE) {
      
      std::string CustomClass;
      std::string CustomModule;
      
      for (xmlAttr *CurrentAttribute = CurrentNode->properties; CurrentAttribute != nullptr; CurrentAttribute = CurrentAttribute->next) {
       
        if(CurrentAttribute->type == XML_ATTRIBUTE_NODE) {
          
          if ((!xmlStrcmp(CurrentAttribute->name, CustomClassAttributeName))) {
            CustomClass = std::string(reinterpret_cast<const char *>(xmlGetProp(CurrentNode, CurrentAttribute->name)));
          }
          
          if ((!xmlStrcmp(CurrentAttribute->name, CustomModuleAttributeName))) {
            CustomModule = std::string(reinterpret_cast<const char *>(xmlGetProp(CurrentNode, CurrentAttribute->name)));
          }
        }
        
        if(!CustomClass.empty()) {
          
          auto SymbolIterator = RenamedSymbols.find(CustomClass);
          
          if ( SymbolIterator != RenamedSymbols.end() ) {
            
            auto Symbol = SymbolIterator->second;
            
            if(shouldRename(Symbol, CustomClass, CustomModule)) {
              xmlSetProp(CurrentNode, CustomClassAttributeName, reinterpret_cast<const xmlChar *>(Symbol.ObfuscatedName.c_str()));
              performedRenaming = true;
            }
          }
          
          CustomClass.clear();
          CustomModule.clear();
        }
      }
    }

    performActualRenaming(CurrentNode->children, RenamedSymbols, performedRenaming);
  }
}

llvm::Expected<bool> LayoutRenamer::performRenaming(std::unordered_map<std::string, SymbolRenaming> RenamedSymbols, std::string OutputPath) {
  //TODO check xib/storyboard version and pick renaming strategy based on that.

  if (XmlDocument == nullptr) {
    return stringError("Could not parse file: " + FileName);
  }
  
  xmlNode *RootNode = xmlDocGetRootElement(XmlDocument);
  
  bool performedRenaming = false;
  performActualRenaming(RootNode, RenamedSymbols, performedRenaming);
  
  xmlSaveFileEnc(static_cast<const char *>(OutputPath.c_str()), XmlDocument, reinterpret_cast<const char *>(XmlDocument->encoding));
  
  return performedRenaming;
}
        
} //namespace obfuscation
} //namespace swift
