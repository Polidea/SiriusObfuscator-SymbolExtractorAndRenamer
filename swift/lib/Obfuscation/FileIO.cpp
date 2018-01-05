#include "swift/Obfuscation/FileIO.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/MemoryBuffer.h"
#include "swift/Basic/JSONSerialization.h"
#include <stdio.h>
#include <iostream>
#include <fstream>

namespace swift {
  namespace obfuscation {
        
    template<class T>
    llvm::ErrorOr<T> parseJson(std::string PathToJson) {
      auto Buffer = llvm::MemoryBuffer::getFile(PathToJson);
      if (auto ErrorCode = Buffer.getError()) {
        std::cout << "Error during JSON file read: " << ErrorCode.message();
        return llvm::ErrorOr<T>(std::error_code(1, std::generic_category()));
      }
      
      llvm::yaml::Input Input(std::move(Buffer.get())->getBuffer());
      T json;
      Input >> json;
      std::error_code ParsingError = Input.error();
      if (ParsingError) {
        std::cout << "Error during JSON parse: " << ParsingError.message();
        return llvm::ErrorOr<T>(std::error_code(1, std::generic_category()));
      }
      
      return llvm::ErrorOr<T>(json);
    }
    
    template llvm::ErrorOr<FilesJson> parseJson(std::string);
    
    template llvm::ErrorOr<SymbolsJson> parseJson(std::string);
    
    template llvm::ErrorOr<RenamesJson> parseJson(std::string);

    template<class T>
    int writeSymbolsToFile(T Symbols, std::string PathToOutput) {
      
      std::string outputStr;
      llvm::raw_string_ostream rso(outputStr);
      swift::json::Output output(rso);
      output << Symbols;
      // TODO: add serialization error handling
      
      std::ofstream file;
      file.open(PathToOutput);
      if (file.fail()) {
        file.close();
        std::cout << "Failed to open file: " << PathToOutput << "\n";
        return 1;
      }
      file << rso.str();
      file.close();
      
      std::cout << "symbols.json:\n" << rso.str();
      
      return 0;
    }
    
    template int writeSymbolsToFile(SymbolsJson Symbols, std::string PathToOutput);
    
    template int writeSymbolsToFile(RenamesJson Symbols, std::string PathToOutput);
    
  } //namespace obfuscation
} //namespace swift
