// RUN: %target-swift-frontend -primary-file %s -emit-ir -o - | %FileCheck %s

func f() -> Bool? { return nil }

({
  guard var b = f() else { return }
  let c = { b = true }
  _ = (b, c)
})()

// CHECK-LABEL: @"$S9alloc_boxyyXEfU_"
// CHECK: <label>:8:
// CHECK-NOT: call void @swift_setDeallocating
// CHECK: call void @swift_deallocUninitializedObject

