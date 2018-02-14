
import Foundation

class T1_SampleClass {}

extension T1_SampleClass {}

fileprivate class T1_FilePrivateSampleClass {}

protocol T1_SampleProtocol {}

struct T1_SampleStruct {}

class T1_Outer {
  class T1_Outer.Inner {
    struct T1_Outer.Inner.InnerStruct: T1_SampleProtocol{
      func NF1_foo() {
        class T1_InsideFunc: Array<T1_SampleClass?> {}
        
        extension T1_InsideFunc {}
      }
    }
  }
}

class T1_DerivedClass: T1_SampleClass, T1_SampleProtocol {}

extension T1_SampleClass: T1_SampleProtocol {}

class T1_CustomNSString : NSString {}

extension NSBoolean {}

struct T1_CustomCFLocaleKey: CFLocaleKey {
  class T1_CustomCFLocaleKey.CustomGenericNSString: Array<NSString> {}
}
