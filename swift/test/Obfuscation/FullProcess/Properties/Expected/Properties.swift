import AppKit

struct T1_DummyStruct {}

// stored let and var properties of different types
class T1_StoredClass {
  let V1_letInt: Int = 0
  var V1_varInt: Int
  let V1_letString: String = "0"
  var V1_varString: String
  let V1_letStruct: T1_DummyStruct = T1_DummyStruct()
  var V1_varStruct: T1_DummyStruct
}

struct T1_StoredStruct {
  let V2_letInt: Int = 0
  var V2_varInt: Int
  let V2_letString: String = "0"
  var V2_varString: String
  let V2_letStruct: T1_DummyStruct = T1_DummyStruct()
  var V2_varStruct: T1_DummyStruct
}

// computed properties of different types
protocol T1_ComputedProtocol {
  var V3_varInt: Int { get }
  var V3_varString: String { get set }
  var V3_varStruct: T1_DummyStruct { get set }
}

class T1_ComputedClass: T1_ComputedProtocol {
  var V3_varInt: Int { return 42 }
  var V3_varString: String { get { return "42" } set {  } }
  var V3_varStruct = T1_DummyStruct()
}

struct T1_ComputedStruct {
  var V4_varInt: Int { return 42 }
  var V4_varString: String { get { return "42" } set {  } }
  var V4_varStruct: T1_DummyStruct = T1_DummyStruct()
}

//computed properties required by other modules
class T1_OtherStruct: NSValidatedUserInterfaceItem {
  var action: Selector? { return nil }
  var tag: Int
}

// stored properties required by other modules
class T1_ViewClass: NSView {
  override var subviews: [NSView] { get { return [] } set { } }
  override var window: NSWindow? { return nil }
}