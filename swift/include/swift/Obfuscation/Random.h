#ifndef Random_h
#define Random_h

#include <cassert>
#include <vector>
#include <string>
#include <random>

namespace swift {
namespace obfuscation {

class RandomIntegerGenerator {
  
private:
  
  std::mt19937 Engine;
  std::uniform_int_distribution<int> Distribution;
  
public:
  
  RandomIntegerGenerator(int Min, int Max)
  : Engine(std::random_device()()),
  Distribution(std::uniform_int_distribution<int>(Min, Max)) {}
  
  int rand();

};

template<typename T>
class RandomElementChooser {
  
private:
  
  RandomIntegerGenerator Generator;
  std::vector<T> List;
  
public:
  
  RandomElementChooser(const std::vector<T> &ListToChooseFrom)
  : Generator(0, ListToChooseFrom.empty() ? 0 : ListToChooseFrom.size() - 1),
  List(ListToChooseFrom) {
    assert(!ListToChooseFrom.empty() && "list of elements to choose from "
                                        "must not be empty");
  };
  
  T rand() {
    return List.at(Generator.rand());
  }

};

template<typename T>
using length_type = typename std::vector<T>::size_type;

template<typename T>
class RandomVectorGenerator {
  
private:
  
  RandomElementChooser<T> Chooser;
  
public:
  
  RandomVectorGenerator(const std::vector<T> &ListToChooseFrom)
  : Chooser(RandomElementChooser<T>(ListToChooseFrom)) {}
  
  std::vector<T> rand(length_type<T> Length);

};

class RandomStringGenerator {
  
private:
  
  RandomVectorGenerator<std::string> Generator;
  
public:
  
  RandomStringGenerator(const std::vector<std::string> &ListToChooseFrom)
  : Generator(ListToChooseFrom) {}
  
  std::string rand(length_type<std::string> Length);

};

} //namespace obfuscation
} //namespace swift

#endif /* Random_h */
