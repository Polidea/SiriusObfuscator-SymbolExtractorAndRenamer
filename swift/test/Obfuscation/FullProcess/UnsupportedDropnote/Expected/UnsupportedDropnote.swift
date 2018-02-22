
class T1_Test {}

// capture list is not properly renamed - should be?
class T1_ClosureTest {
  let V1_test = T1_Test()
  
  lazy var someClosure = {
    [unowned self, weak V2_test = self.V1_test] in
  }
}

// init param FieldA is not renamed if there is a second param with default value
class T1_Foo {
  var V1_FieldA: String
  var V1_FieldB: String?
  
  init(SP1_FieldA: String, SP1_FieldB: String? = nil) {}
}
let V1_FooObj = T1_Foo(SP1_FieldA: "test")

