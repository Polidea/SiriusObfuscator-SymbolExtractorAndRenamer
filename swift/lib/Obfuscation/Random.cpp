#include "swift/Obfuscation/Random.h"
#include <stdio.h>
#include <random>

namespace swift {
  
  namespace obfuscation {
    
    size_t RandomIntegerGenerator::rand() {
      return distribution(engine);
    }
    
    template<typename T>
    T RandomElementChooser<T>::rand() {
      return list.at(generator->rand());
    }
    
    template<typename T>
    std::vector<T> RandomVectorGenerator<T>::rand(size_t length) {
      std::vector<T> result;
      for (size_t i = 0; i < length; i++) {
        result.push_back(chooser->rand());
      }
      return result;
    }
    
    std::string RandomStringGenerator::rand(size_t length) {
      auto vector = generator->rand(length);
      std::string result;
      for (auto const& elem : vector) {
        result += elem;
      }
      return result;
    }
    
  } //namespace obfuscation
  
} //namespace swift
