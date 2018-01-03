#include "swift/Obfuscation/Obfuscation.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include <sstream>

namespace swift {
  
  namespace obfuscation {
    
    struct SymbolsProvider {
      
    public:
      
      static Symbol symbol(Decl* De) {
        std::vector<std::string> Parts;
        std::string SymbolName;
        if (auto *D = dyn_cast<NominalTypeDecl>(De)) {
          Parts.push_back("module");
          Parts.push_back(D->getModuleContext()->getBaseName().getIdentifier().get());
          if (auto *decl = dyn_cast<EnumDecl>(D)) {
            SymbolName = decl->getDeclaredInterfaceType()->getString();
            Parts.push_back("enum." + SymbolName);
          } else if (auto *decl = dyn_cast<ClassDecl>(D)) {
            SymbolName = decl->getDeclaredInterfaceType()->getString();
            Parts.push_back("class." + SymbolName);
          } else if (auto *decl = dyn_cast<ProtocolDecl>(D)) {
            SymbolName = decl->getDeclaredInterfaceType()->getString();
            Parts.push_back("protocol." + SymbolName);
          } else if (auto *decl = dyn_cast<StructDecl>(D)) {
            SymbolName = decl->getDeclaredInterfaceType()->getString();
            Parts.push_back("struct." + SymbolName);
          }
        }
        
        std::string StringParts;
        if (Parts.size() == 1) {
          StringParts = Parts[0];
        } else {
          std::stringstream Result;
          copy(Parts.begin(), Parts.end(), std::ostream_iterator<std::string>(Result, "."));
          StringParts = Result.str();
          StringParts.pop_back();
        }
        
        auto SymbolStruct = Symbol();
        SymbolStruct.symbol = StringParts;
        SymbolStruct.name = SymbolName;
        return SymbolStruct;
      }
    };
    
    CompilerInvocation createInvocation(CompilerInvocationConfiguration Configuration) {
      CompilerInvocation Invocation;
      Invocation.setModuleName(Configuration.ModuleName);
      Invocation.setMainExecutablePath(Configuration.MainExecutablePath);
      for (auto InputFilename : Configuration.InputFilenames) {
        Invocation.addInputFilename(InputFilename);
      }
      Invocation.getLangOptions().AttachCommentsToDecls = true;
      Invocation.setFrameworkSearchPaths(Configuration.Paths);
      Invocation.setSDKPath(Configuration.SdkPath);
      return Invocation;
    }
    
    std::set<Symbol> findSymbolsToObfuscate(SourceFile &SF) {
      struct RenamesCollector: public SourceEntityWalker {
        std::set<Symbol> Bucket;
        RenamesCollector() {}
        
        void handleSymbol(Symbol &Symbol) {
          Bucket.insert(Symbol);
        }
        
        bool walkToDeclPre(Decl *D, CharSourceRange Range) override {
          if (D->isImplicit())
            return false;
          auto Symbol = SymbolsProvider::symbol(D);
          handleSymbol(Symbol);
          return true;
        }
        
        bool visitDeclReference(ValueDecl *D, CharSourceRange Range,
                                TypeDecl *CtorTyRef, ExtensionDecl *ExtTyRef,
                                Type T, ReferenceMetaData Data) override {
          auto Symbol = SymbolsProvider::symbol(D);
          handleSymbol(Symbol);
          return true;
        }
      } Collector;
      
      Collector.walk(SF);
      
      return Collector.Bucket;
    }
    
    llvm::ErrorOr<SymbolsJson> extractSymbols(CompilerInvocationConfiguration Configuration) {
      auto Invocation = createInvocation(Configuration);
      CompilerInstance CI;
      PrintingDiagnosticConsumer PrintDiags;
      CI.addDiagnosticConsumer(&PrintDiags);
      if (CI.setup(Invocation)) {
        return llvm::ErrorOr<std::vector<Symbol>>(std::error_code(1, std::generic_category()));
      }
      CI.performSema();
      
      std::vector<Symbol> Symbols;
      std::vector<SourceFile *> SF;
      for (auto Unit : CI.getMainModule()->getFiles()) {
        if (auto Current = dyn_cast<SourceFile>(Unit)) {
          SF.push_back(Current);
          auto CurrentSymbols = findSymbolsToObfuscate(*Current);
          std::copy(CurrentSymbols.begin(), CurrentSymbols.end(), std::back_inserter(Symbols));
        }
      }
      SymbolsJson Json = SymbolsJson(Symbols);
      return llvm::ErrorOr<SymbolsJson>(Json);
    }
    
  } //namespace obfuscation
  
} //namespace swift
