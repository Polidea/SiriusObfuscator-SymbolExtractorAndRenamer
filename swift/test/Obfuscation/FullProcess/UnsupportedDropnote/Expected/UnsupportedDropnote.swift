import Foundation

class T1_Test {
  func NF1_testFunc() {}
}

// init param FieldA is not renamed if there is a second param with default value
class T1_Foo {
  var V1_FieldA: String
  var V1_FieldB: String?
  
  init(SP1_FieldA: String, SP1_FieldB: String? = nil) {}
}
let V1_FooObj = T1_Foo(SP1_FieldA: "test")

// type is not renamed in if case
let V1_num = 42
if case 0...225 = V1_num, V1_num is T1_Test {
}

// debug blocks are omitted
final class T1_DebugBlock {
  fileprivate init() {
    #if !DEBUG
      let V1_testInDebug = T1_Test()
    #endif
  }
}

//protocol stuff
protocol T1_Proto {
  func NF1_hello()
}
extension NSString: T1_Proto {}
extension T1_Proto where Self: NSString {
  func NF1_hello() {}
}
