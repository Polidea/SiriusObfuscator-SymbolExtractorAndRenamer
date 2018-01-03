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
    
    llvm::ErrorOr<CompilerInvocationConfiguration> parseFilesJson(std::string PathToJson, std::string MainExecutablePath) {
      auto Buffer = llvm::MemoryBuffer::getFile(PathToJson);
      if (auto ErrorCode = Buffer.getError()) {
        std::cout << "Error during JSON file read: " << ErrorCode.message();
        return llvm::ErrorOr<CompilerInvocationConfiguration>(std::error_code(1, std::generic_category()));
      }
      
      llvm::yaml::Input Input(std::move(Buffer.get())->getBuffer());
      FilesJson filesJson;
      Input >> filesJson;
      std::error_code ParsingError = Input.error();
      if (ParsingError) {
        std::cout << "Error during JSON parse: " << ParsingError.message();
        return llvm::ErrorOr<CompilerInvocationConfiguration>(std::error_code(1, std::generic_category()));
      }
      
      StringRef ModuleName = filesJson.module.name;
      
      StringRef SdkPath = filesJson.sdk.path;
      
      std::vector<std::string> InputFilenames = filesJson.filenames;
      std::vector<SearchPathOptions::FrameworkSearchPath> Paths;
      for (auto Framework : filesJson.explicitelyLinkedFrameworks) {
        Paths.push_back(SearchPathOptions::FrameworkSearchPath(Framework.path, false));
      }
      
      return llvm::ErrorOr<CompilerInvocationConfiguration>(CompilerInvocationConfiguration(ModuleName, MainExecutablePath, SdkPath, InputFilenames, Paths));
    }
    
    
    int writeSymbolsToFile(SymbolsJson Symbols, std::string PathToOutput) {
      
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
    
  } //namespace obfuscation
} //namespace swift
