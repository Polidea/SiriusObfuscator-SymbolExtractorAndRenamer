// RUN: %target-swift-frontend -emit-silgen %s | %FileCheck %s

// CHECK-LABEL: sil private @_T026closure_inline_initializer3FooV3fooSivfiSiycfU_

struct Foo {
  var foo: Int = { 2 }()

  init(x: Int) {}
  init(y: Int) {}
}
