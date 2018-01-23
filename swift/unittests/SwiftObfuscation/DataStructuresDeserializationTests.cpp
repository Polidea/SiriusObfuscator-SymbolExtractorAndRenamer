#include "swift/Obfuscation/DataStructures.h"
#include "gtest/gtest.h"

using namespace swift::obfuscation;
using namespace llvm::yaml;

template<class T>
bool vectorContains(const std::vector<T> &Vector, const T &Element) {
  return std::end(Vector) != std::find(Vector.begin(), Vector.end(), Element);
}

TEST(DataStructuresDeserialization, DeserializeProject) {
  std::string JsonString = "{\n\"rootPath\": \"samplePath\"\n}";
  
  auto DeserializedOrError = deserialize<Project>(JsonString);
  
  auto Expected = Project();
  Expected.RootPath = "samplePath";
  
  if (auto ErrorCode = DeserializedOrError.takeError()) {
    llvm::consumeError(std::move(ErrorCode));
    FAIL() << "Error during json parsing";
  }
  auto Deserialized = DeserializedOrError.get();
  
  EXPECT_EQ(Deserialized.RootPath, Expected.RootPath);
}

TEST(DataStructuresDeserialization, DeserializeModule) {
  std::string JsonString = "{\n\"name\": \"sampleName\"\n}";

  auto DeserializedOrError = deserialize<Module>(JsonString);

  auto Expected = Module();
  Expected.Name = "sampleName";
  
  if (auto ErrorCode = DeserializedOrError.takeError()) {
    llvm::consumeError(std::move(ErrorCode));
    FAIL() << "Error during json parsing";
  }
  auto Deserialized = DeserializedOrError.get();

  EXPECT_EQ(Deserialized.Name, Expected.Name);
}

TEST(DataStructuresDeserialization, DeserializeSdk) {
  std::string JsonString = "{\n"
  "\"name\": \"sampleName\"\n,"
  "\"path\": \"samplePath\",\n}";

  auto DeserializedOrError = deserialize<Sdk>(JsonString);

  auto Expected = Sdk();
  Expected.Name = "sampleName";
  Expected.Path = "samplePath";

  if (auto ErrorCode = DeserializedOrError.takeError()) {
    llvm::consumeError(std::move(ErrorCode));
    FAIL() << "Error during json parsing";
  }
  auto Deserialized = DeserializedOrError.get();
  
  EXPECT_EQ(Deserialized.Name, Expected.Name);
}

TEST(DataStructuresDeserialization, DeserializeExplicitlyLinkedFramework) {
  std::string JsonString = "{\n"
  "\"name\": \"sampleName\"\n,"
  "\"path\": \"samplePath\",\n}";

  auto DeserializedOrError = deserialize<ExplicitelyLinkedFrameworks>(JsonString);

  auto Expected = ExplicitelyLinkedFrameworks();
  Expected.Name = "sampleName";
  Expected.Path = "samplePath";

  if (auto ErrorCode = DeserializedOrError.takeError()) {
    llvm::consumeError(std::move(ErrorCode));
    FAIL() << "Error during json parsing";
  }
  auto Deserialized = DeserializedOrError.get();
  
  EXPECT_EQ(Deserialized.Name, Expected.Name);
}

TEST(DataStructuresDeserialization, DeserializeSymbolsJson) {
  std::string JsonString =
  "{\n"
  "symbols: [\n"
  "{\n"
  "\"name\": \"sampleName0\",\n"
  "\"identifier\": \"sampleIdentifier0\",\n"
  "\"module\": \"sampleModule0\"\n"
  "},\n"
  "{\n"
  "\"name\": \"sampleName1\",\n"
  "\"identifier\": \"sampleIdentifier1\",\n"
  "\"module\": \"sampleModule1\"\n"
  "}\n"
  "]\n"
  "}";

  auto DeserializedOrError = deserialize<SymbolsJson>(JsonString);

  if (auto ErrorCode = DeserializedOrError.takeError()) {
    llvm::consumeError(std::move(ErrorCode));
    FAIL() << "Error during json parsing";
  }
  auto Deserialized = DeserializedOrError.get();
  
  size_t ExpectedSize = 2;
  EXPECT_EQ(Deserialized.Symbols.size(), ExpectedSize);

  Symbol Expected0("sampleIdentifier0", "sampleName0", "sampleModule0");
  Symbol Expected1("sampleIdentifier1", "sampleName1", "sampleModule1");

  EXPECT_TRUE(vectorContains<Symbol>(Deserialized.Symbols, Expected0));
  EXPECT_TRUE(vectorContains<Symbol>(Deserialized.Symbols, Expected1));
}

TEST(DataStructuresDeserialization, DeserializeSymbol) {
  std::string JsonString = "{\n"
  "\"name\": \"sampleName\"\n,"
  "\"identifier\": \"sampleIdentifier\",\n"
  "\"module\": \"sampleModule\"\n}";

  auto DeserializedOrError = deserialize<Symbol>(JsonString);

  Symbol Expected("sampleIdentifier", "sampleName", "sampleModule");

  if (auto ErrorCode = DeserializedOrError.takeError()) {
    llvm::consumeError(std::move(ErrorCode));
    FAIL() << "Error during json parsing";
  }
  auto Deserialized = DeserializedOrError.get();
  
  EXPECT_EQ(Deserialized, Expected);
}

TEST(DataStructuresDeserialization, DeserializeRenamesJson) {
  std::string JsonString =
  "{\n"
  "symbols: [\n"
  "{\n"
  "\"identifier\": \"sampleIdentifier0\",\n"
  "\"originalName\": \"sampleName0\",\n"
  "\"obfuscatedName\": \"sampleObfuscatedName0\",\n"
  "\"module\": \"sampleModule0\"\n"
  "},\n"
  "{\n"
  "\"identifier\": \"sampleIdentifier1\",\n"
  "\"originalName\": \"sampleName1\",\n"
  "\"obfuscatedName\": \"sampleObfuscatedName1\",\n"
  "\"module\": \"sampleModule1\"\n"
  "}\n"
  "]\n"
  "}";

  auto DeserializedOrError = deserialize<RenamesJson>(JsonString);

  if (auto ErrorCode = DeserializedOrError.takeError()) {
    llvm::consumeError(std::move(ErrorCode));
    FAIL() << "Error during json parsing";
  }
  auto Deserialized = DeserializedOrError.get();
  
  size_t ExpectedSize = 2;
  EXPECT_EQ(Deserialized.Symbols.size(), ExpectedSize);

  SymbolRenaming Expected0("sampleIdentifier0", "sampleName0", "sampleObfuscatedName0", "sampleModule0");
  SymbolRenaming Expected1("sampleIdentifier1", "sampleName1", "sampleObfuscatedName1", "sampleModule1");

  EXPECT_TRUE(vectorContains<SymbolRenaming>(Deserialized.Symbols, Expected0));
  EXPECT_TRUE(vectorContains<SymbolRenaming>(Deserialized.Symbols, Expected1));
}

TEST(DataStructuresDeserialization, DeserializeSymbolRenaming) {
  std::string JsonString = "{\n"
  "\"originalName\": \"sampleName\"\n,"
  "\"identifier\": \"sampleIdentifier\",\n"
  "\"obfuscatedName\": \"sampleObfuscatedName\",\n"
  "\"module\": \"sampleModule\"\n}";

  auto DeserializedOrError = deserialize<SymbolRenaming>(JsonString);

  auto Expected = SymbolRenaming();
  Expected.Identifier = "sampleIdentifier";
  Expected.OriginalName = "sampleName";
  Expected.ObfuscatedName = "sampleObfuscatedName";
  Expected.Module = "sampleModule";
  
  if (auto ErrorCode = DeserializedOrError.takeError()) {
    llvm::consumeError(std::move(ErrorCode));
    FAIL() << "Error during json parsing";
  }
  auto Deserialized = DeserializedOrError.get();
  
  EXPECT_EQ(Deserialized, Expected);
}
