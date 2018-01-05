#ifndef Random_h
#define Random_h

#include <stdio.h>
#include <random>

namespace swift {
  
  namespace obfuscation {
    
    class RandomIntegerGenerator {
      
    private:
      
      std::random_device rd;
      std::mt19937 engine;
      std::uniform_int_distribution<size_t> distribution;
      
    public:
      
      RandomIntegerGenerator(size_t min, size_t max) :
      rd(),
      engine(rd()),
      distribution(std::uniform_int_distribution<size_t>(min, max)) {}
      
      size_t rand();

    };
    
    template<typename T>
    class RandomElementChooser {
      
    private:
      
      RandomIntegerGenerator* generator;
      std::vector<T> list;
      
    public:
      
      RandomElementChooser(std::vector<T> listToChooseFrom) :
      generator(new RandomIntegerGenerator(0, listToChooseFrom.size() - 1)),
      list(listToChooseFrom) {}
      
      T rand();

    };
    
    template<typename T>
    class RandomVectorGenerator {
      
    private:
      
      RandomElementChooser<T>* chooser;
      
    public:
      
      RandomVectorGenerator(std::vector<T> listToChooseFrom) :
      chooser(new RandomElementChooser<T>(listToChooseFrom)) {}
      
      std::vector<T> rand(size_t length);

    };
    
    class RandomStringGenerator {
      
    private:
      
      RandomVectorGenerator<std::string>* generator;
      
    public:
      
      RandomStringGenerator(std::vector<std::string> listToChooseFrom) :
      generator(new RandomVectorGenerator<std::string>(listToChooseFrom)) {}
      
      std::string rand(size_t length);

    };
    
  } //namespace obfuscation
  
} //namespace swift

#endif /* Random_h */
