import AppKit

// Backtick named function
func NF1_`function`() -> Int { return 1 }

// Generic class methods
class T1_GenericClass<T> {
  func NF1_method() -> T { return T() }
}

let V1_gcii = T1_GenericClass<Int>()
_ = V1_gcii.NF1_method()

let V1_gcsi = T1_GenericClass<T1_SampleClass>()
_ = V1_gcsi.NF1_method()
