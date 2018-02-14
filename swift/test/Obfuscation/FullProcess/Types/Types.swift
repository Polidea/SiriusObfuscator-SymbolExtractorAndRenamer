//RUN: %target-prepare-obfuscation-for-file "Types" %target-run-full-obfuscation

import Foundation

class SampleClass {}

extension SampleClass {}

fileprivate class FilePrivateSampleClass {}

protocol SampleProtocol {}

struct SampleStruct {}

class Outer {
  class Inner {
    struct InnerStruct: SampleProtocol{
      func foo() {
        class InsideFunc: Array<SampleClass?> {}
        
        extension InsideFunc {}
      }
    }
  }
}

class DerivedClass: SampleClass, SampleProtocol {}

extension SampleClass: SampleProtocol {}

class CustomNSString : NSString {}

extension NSBoolean {}

struct CustomCFLocaleKey: CFLocaleKey {
  class CustomGenericNSString: Array<NSString> {}
}

