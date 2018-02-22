
//RUN: %target-prepare-obfuscation-for-file "UnsupportedDropnote" %target-run-full-obfuscation

class Test {}

// capture list is not properly renamed - should be?
class ClosureTest {
  let test = Test()
  
  lazy var someClosure = {
    [unowned self, weak test = self.test] in
  }
}

// init param FieldA is not renamed if there is a second param with default value
class Foo {
  var FieldA: String
  var FieldB: String?

  init(FieldA: String, FieldB: String? = nil) {}
}
let FooObj = Foo(FieldA: "test")


