// RUN: %clang_cc1 -std=c++11 -triple x86_64-apple-darwin10 -emit-llvm -o - %s -fsanitize=alignment | FileCheck %s -check-prefixes=ALIGN,COMMON
// RUN: %clang_cc1 -std=c++11 -triple x86_64-apple-darwin10 -emit-llvm -o - %s -fsanitize=null | FileCheck %s -check-prefixes=NULL,COMMON
// RUN: %clang_cc1 -std=c++11 -triple x86_64-apple-darwin10 -emit-llvm -o - %s -fsanitize=object-size | FileCheck %s -check-prefixes=OBJSIZE,COMMON
// RUN: %clang_cc1 -std=c++11 -triple x86_64-apple-darwin10 -emit-llvm -o - %s -fsanitize=null,vptr | FileCheck %s -check-prefixes=VPTR
// RUN: %clang_cc1 -std=c++11 -triple x86_64-apple-darwin10 -emit-llvm -o - %s -fsanitize=vptr | FileCheck %s -check-prefixes=NOVPTR

struct A {
  // COMMON-LABEL: define linkonce_odr void @_ZN1A10do_nothingEv
  void do_nothing() {
    // ALIGN-NOT: ptrtoint %struct.A* %{{.*}} to i64, !nosanitize
 
    // NULL: icmp ne %struct.A* %{{.*}}, null, !nosanitize
 
    // OBJSIZE-NOT: call i64 @llvm.objectsize
  }
};

struct B {
  int x;

  // COMMON-LABEL: define linkonce_odr void @_ZN1B10do_nothingEv
  void do_nothing() {
    // ALIGN: ptrtoint %struct.B* %{{.*}} to i64, !nosanitize
    // ALIGN: and i64 %{{.*}}, 3, !nosanitize

    // NULL: icmp ne %struct.B* %{{.*}}, null, !nosanitize

    // OBJSIZE-NOT: call i64 @llvm.objectsize
  }
};

struct Animal {
  virtual const char *speak() = 0;
};

struct Cat : Animal {
  const char *speak() override { return "meow"; }
};

struct Dog : Animal {
  const char *speak() override { return "woof"; }
};

// VPTR-LABEL: define void @_Z12invalid_castP3Cat
void invalid_cast(Cat *cat = nullptr) {
  // First, null check the pointer:
  //
  // VPTR: [[ICMP:%.*]] = icmp ne %struct.Dog* {{.*}}, null
  // VPTR-NEXT: br i1 [[ICMP]]
  // VPTR: call void @__ubsan_handle_type_mismatch
  //
  // Once we're done emitting the null check, reuse the check to see if we can
  // proceed to the vptr check:
  //
  // VPTR: br i1 [[ICMP]]
  // VPTR: call void @__ubsan_handle_dynamic_type_cache_miss
  auto *badDog = reinterpret_cast<Dog *>(cat);
  badDog->speak();
}

// NOVPTR-NOT: __ubsan_handle_dynamic_type_cache_miss
int main() {
  A a;
  a.do_nothing();

  B b;
  b.do_nothing();

  invalid_cast();
  return 0;
}
