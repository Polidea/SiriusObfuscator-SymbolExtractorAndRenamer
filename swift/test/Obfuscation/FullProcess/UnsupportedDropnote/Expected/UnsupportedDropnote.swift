import Foundation

class T1_Test {
  func NF1_testFunc() {}
}

// debug blocks are omitted
final class T1_DebugBlock {
  fileprivate init() {
    #if !DEBUG
      let V1_testInDebug = T1_Test()
    #endif
  }
}

//error name in catch block should not be renamed
func NF1_canThrowErrors() throws {}
func NF1_a() {
  do {
    try NF1_canThrowErrors()
  } catch {
    error
  }
}

//override init
class T1_Parent{
  init(SP1_p1: String, SP1_p2: Int) {}
}
class T1_Child: T1_Parent {
  override init(SP1_p1: String, SP1_p2: Int){}
}
let V1_c = T1_Child(SP1_p1: "p1", SP1_p2:42)

//protocol stuff
protocol T1_Proto {
  func NF1_hello()
}
extension NSString: T1_Proto {}
extension T1_Proto where Self: NSString {
  func NF1_hello() {}
}
