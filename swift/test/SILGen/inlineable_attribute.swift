// RUN: %target-swift-frontend -emit-silgen -emit-verbose-sil %s | %FileCheck %s

// CHECK-LABEL: sil [serialized] @_T020inlineable_attribute15fragileFunctionyyF : $@convention(thin) () -> ()
@_inlineable public func fragileFunction() {

}

public struct MySt {
  // CHECK-LABEL: sil [serialized] @_T020inlineable_attribute4MyStV6methodyyF : $@convention(method) (MySt) -> ()
  @_inlineable public func method() {}

  // CHECK-LABEL: sil [serialized] @_T020inlineable_attribute4MyStV8propertySifg : $@convention(method) (MySt) -> Int
  @_inlineable public var property: Int {
    return 5
  }

  // CHECK-LABEL: sil [serialized] @_T020inlineable_attribute4MyStV9subscriptS2icfg : $@convention(method) (Int, MySt) -> Int
  @_inlineable public subscript(x: Int) -> Int {
    return x
  }
}

public class MyCls {
  // CHECK-LABEL: sil [serialized] @_T020inlineable_attribute5MyClsCfD : $@convention(method) (@owned MyCls) -> ()
  @_inlineable deinit {}

  // Allocating entry point is [serialized]

  // CHECK-LABEL: sil [serialized] @_T020inlineable_attribute5MyClsCACyt14designatedInit_tcfC : $@convention(method) (@thick MyCls.Type) -> @owned MyCls
  public init(designatedInit: ()) {}

  // Note -- convenience init is intentionally not [serialized]

  // CHECK-LABEL: sil @_T020inlineable_attribute5MyClsCACyt15convenienceInit_tcfC : $@convention(method) (@thick MyCls.Type) -> @owned MyCls
  public convenience init(convenienceInit: ()) {
    self.init(designatedInit: ())
  }
}

// Make sure enum case constructors for public and versioned enums are
// [serialized].
@_versioned enum MyEnum {
  case c(MySt)
}

// CHECK-LABEL: sil shared [transparent] [serialized] [thunk] @_T020inlineable_attribute6MyEnumO1cAcA0C2StVcACmFTc : $@convention(thin) (@thin MyEnum.Type) -> @owned @callee_owned (MySt) -> MyEnum

@_inlineable public func referencesMyEnum() {
  _ = MyEnum.c
}

// CHECK-LABEL: sil [transparent] @_T020inlineable_attribute15HasInitializersV1xSivfi : $@convention(thin) () -> Int

public struct HasInitializers {
  public let x = 1234

  @_inlineable public init() {}
}

public class Horse {
  public func gallop() {}
}

// CHECK-LABEL: sil [serialized] @_T020inlineable_attribute15talkAboutAHorseyAA5HorseC1h_tF : $@convention(thin) (@owned Horse) -> () {
// CHECK: function_ref @_T020inlineable_attribute5HorseC6gallopyyFTc
// CHECK: return
// CHECK: }

// CHECK-LABEL: sil shared [serializable] [thunk] @_T020inlineable_attribute5HorseC6gallopyyFTc : $@convention(thin) (@owned Horse) -> @owned @callee_owned () -> () {
// CHECK: class_method
// CHECK: return
// CHECK: }

@_inlineable public func talkAboutAHorse(h: Horse) {
  _ = h.gallop
}
