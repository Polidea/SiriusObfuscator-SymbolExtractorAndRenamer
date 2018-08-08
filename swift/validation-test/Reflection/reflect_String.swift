// RUN: %empty-directory(%t)
// RUN: %target-build-swift -lswiftSwiftReflectionTest %s -o %t/reflect_String
// RUN: %target-run %target-swift-reflection-test %t/reflect_String 2>&1 | %FileCheck %s --check-prefix=CHECK-%target-ptrsize
// REQUIRES: objc_interop
// REQUIRES: executable_test

import SwiftReflectionTest

class TestClass {
    var t: String
    init(t: String) {
        self.t = t
    }
}

var obj = TestClass(t: "Hello, Reflection!")

reflect(object: obj)

// CHECK-64: Reflecting an object.
// CHECK-64: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-64: Type reference:
// CHECK-64: (class reflect_String.TestClass)

// CHECK-64: Type info:
// CHECK-64-NEXT: (class_instance size=32 alignment=8 stride=32
// CHECK-64-NEXT:   (field name=t offset=16
// CHECK-64-NEXT:     (struct size=16 alignment=8 stride=16 num_extra_inhabitants=1
// (unstable implementation details omitted)

// CHECK-32: Reflecting an object.
// CHECK-32: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-32: Type reference:
// CHECK-32: (class reflect_String.TestClass)

// CHECK-32: Type info:
// CHECK-32-NEXT: (class_instance size=20 alignment=4 stride=20
// CHECK-32-NEXT:   (field name=t offset=8
// CHECK-32-NEXT:     (struct size=12 alignment=4 stride=12 num_extra_inhabitants=4092
// (unstable implementation details omitted)

doneReflecting()

// CHECK-64: Done.

// CHECK-32: Done.
