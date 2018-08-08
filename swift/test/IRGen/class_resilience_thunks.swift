// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -emit-module -enable-resilience -emit-module-path=%t/resilient_class_thunks.swiftmodule -module-name=resilient_class_thunks %S/../Inputs/resilient_class_thunks.swift
// RUN: %target-swift-frontend -I %t -emit-ir -enable-resilience %s | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize
// RUN: %target-swift-frontend -I %t -emit-ir -enable-resilience -O %s

// CHECK: %swift.type = type { [[INT:i32|i64]] }

import resilient_class_thunks

// CHECK-LABEL: define{{( protected)?}} swiftcc void @"$S23class_resilience_thunks21testDispatchThunkBase1b1ty010resilient_a1_C00G0CyxG_xtlF"(%T22resilient_class_thunks4BaseC*, %swift.opaque* noalias nocapture)
public func testDispatchThunkBase<T>(b: Base<T>, t: T) {

  // CHECK: call swiftcc void @"$S22resilient_class_thunks4BaseC6takesTyyxFTj"(%swift.opaque* noalias nocapture {{%.*}}, %T22resilient_class_thunks4BaseC* swiftself %0)
  b.takesT(t)

  // CHECK: call swiftcc void @"$S22resilient_class_thunks4BaseC8takesIntyySiFTj"([[INT]] 0, %T22resilient_class_thunks4BaseC* swiftself %0)
  b.takesInt(0)

  // CHECK: call swiftcc void @"$S22resilient_class_thunks4BaseC14takesReferenceyyAA6ObjectCFTj"(%T22resilient_class_thunks6ObjectC* {{%.*}}, %T22resilient_class_thunks4BaseC* swiftself %0)
  b.takesReference(Object())

  // CHECK: ret void
}

// CHECK-LABEL: define{{( protected)?}} swiftcc void @"$S23class_resilience_thunks24testDispatchThunkDerived1dy010resilient_a1_C00G0C_tF"(%T22resilient_class_thunks7DerivedC*)
public func testDispatchThunkDerived(d: Derived) {

  // CHECK: call swiftcc void @"$S22resilient_class_thunks4BaseC6takesTyyxFTj"(%swift.opaque* noalias nocapture dereferenceable({{4|8}}) {{%.*}}, %T22resilient_class_thunks4BaseC* swiftself {{%.*}})
  d.takesT(0)

  // CHECK: call swiftcc void @"$S22resilient_class_thunks7DerivedC8takesIntyySiSgFTj"([[INT]] 0, i8 1, %T22resilient_class_thunks7DerivedC* swiftself %0)
  d.takesInt(nil)

  // CHECK: call swiftcc void @"$S22resilient_class_thunks4BaseC14takesReferenceyyAA6ObjectCFTj"(%T22resilient_class_thunks6ObjectC* null, %T22resilient_class_thunks4BaseC* swiftself {{%.*}})
  d.takesReference(nil)

  // CHECK: ret void
}
