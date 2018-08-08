// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -emit-ir -primary-file %s -import-objc-header %S/Inputs/objc_bridged_generic_conformance.h | %FileCheck %s
// REQUIRES: objc_interop

// CHECK-NOT: _TMnCSo

// CHECK: @"$SSo6ThingyCyxG32objc_bridged_generic_conformance1PADMc" = hidden constant %swift.protocol_conformance_descriptor {{.*}} @"\01l_OBJC_CLASS_REF_$_Thingy"

// CHECK-NOT: _TMnCSo

protocol P { func test() }

extension Thingy: P {
  func test() {}
}
