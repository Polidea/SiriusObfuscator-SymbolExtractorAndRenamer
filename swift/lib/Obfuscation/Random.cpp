#include "swift/Obfuscation/Random.h"

namespace swift {
namespace obfuscation {

int RandomIntegerGenerator::rand() {
  return Distribution(Engine);
}
  
template<typename T>
std::vector<T> RandomVectorGenerator<T>::rand(length_type<T> Length) {
  std::vector<T> Result;
  for (length_type<T> i = 0; i < Length; i++) {
    Result.push_back(Chooser.rand());
  }
  return Result;
}

std::string RandomStringGenerator::rand(length_type<std::string> Length) {
  auto Vector = Generator.rand(Length);
  std::string Result;
  for (const auto &Elem : Vector) {
    Result += Elem;
  }
  return Result;
}

} //namespace obfuscation
} //namespace swift
