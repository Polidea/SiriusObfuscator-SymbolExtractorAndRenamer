#include "swift/Obfuscation/NameMapping.h"
#include "swift/Obfuscation/Random.h"
#include <stdio.h>

namespace swift {
  
  namespace obfuscation {
    
    class UniqueTypeNameGenerator {
      
    private:
      std::set<std::string> generatedSymbols;
      const std::vector<std::string> headSymbols = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
      std::vector<std::string> tailSymbols;
      RandomElementChooser<std::string>* headGenerator;
      RandomStringGenerator* tailGenerator;
      const size_t identifierLength = 32;
      
      llvm::ErrorOr<std::string> generateName(int numbersOfTriesLeft) {
        if (numbersOfTriesLeft <= 0) {
          return llvm::ErrorOr<std::string>(std::error_code(1, std::generic_category()));
        }
        auto head = headGenerator->rand();
        auto tail = tailGenerator->rand(identifierLength - 1);
        auto name = head + tail;
        
        if (generatedSymbols.insert(name).second) {
          return name;
        } else {
          return this->generateName(numbersOfTriesLeft - 1);
        }
      }
      
    public:
      
      UniqueTypeNameGenerator() {
        tailSymbols = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
        tailSymbols.insert(tailSymbols.end(), headSymbols.begin(), headSymbols.end());
        headGenerator = new RandomElementChooser<std::string>(headSymbols);
        tailGenerator = new RandomStringGenerator(tailSymbols);
      }
      
      
      llvm::ErrorOr<std::string> generateName() {
        return generateName(100);
      }
      
    };
    
    llvm::ErrorOr<RenamesJson> proposeRenamings(SymbolsJson symbolsJson) {
      
      auto typeNameGenerator = new UniqueTypeNameGenerator();
      
      std::vector<SymbolRenaming> Symbols;
      
      for (std::size_t i = 0; i < symbolsJson.symbols.size(); ++i) {
        auto Symbol = symbolsJson.symbols.at(i);
        auto Renaming = SymbolRenaming();
        Renaming.identifier = Symbol.identifier;
        Renaming.originalName = Symbol.name;
        auto NameOrError = typeNameGenerator->generateName();
        if (std::error_code ec = NameOrError.getError()) {
          return llvm::ErrorOr<RenamesJson>(ec);
        }
        Renaming.obfuscatedName = NameOrError.get();
        Symbols.push_back(Renaming);
      }
      
      RenamesJson renamesJson = RenamesJson();
      renamesJson.symbols = Symbols;
      return renamesJson;
    }
    
  } //namespace obfuscation
  
} //namespace swift
