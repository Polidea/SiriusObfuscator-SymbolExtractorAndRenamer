
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -module-name objc_imported_generic -emit-silgen %s -enable-sil-ownership | %FileCheck %s
// For integration testing, ensure we get through IRGen too.
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -module-name objc_imported_generic -emit-ir -verify -DIRGEN_INTEGRATION_TEST %s

// REQUIRES: objc_interop

import objc_generics

func callInitializer() {
  _ = GenericClass(thing: NSObject())
}

// CHECK-LABEL: sil shared [serializable] @$SSo12GenericClassC5thingAByxGSgxSg_tcfC
// CHECK:         thick_to_objc_metatype {{%.*}} : $@thick GenericClass<T>.Type to $@objc_metatype GenericClass<T>.Type

public func genericMethodOnAnyObject(o: AnyObject, b: Bool) -> AnyObject {
  return o.thing!()!
}

// CHECK-LABEL: sil @$S21objc_imported_generic0C17MethodOnAnyObject{{[_0-9a-zA-Z]*}}F
// CHECK:         objc_method {{%.*}} : $@opened([[TAG:.*]]) AnyObject, #GenericClass.thing!1.foreign : <T where T : AnyObject> (GenericClass<T>) -> () -> T?, $@convention(objc_method) @pseudogeneric (@opened([[TAG]]) AnyObject) -> @autoreleased Optional<AnyObject>

public func genericMethodOnAnyObjectChained(o: AnyObject, b: Bool) -> AnyObject? {
  return o.thing?()
}

// CHECK-LABEL: sil @$S21objc_imported_generic0C24MethodOnAnyObjectChained1o1byXlSgyXl_SbtF
// CHECK: bb0([[ANY:%.*]] : @guaranteed $AnyObject, [[BOOL:%.*]] : @trivial $Bool):
// CHECK:   [[OPENED_ANY:%.*]] = open_existential_ref [[ANY]]
// CHECK:   [[OPENED_ANY_COPY:%.*]] = copy_value [[OPENED_ANY]]
// CHECK:   dynamic_method_br [[OPENED_ANY_COPY]] : $@opened([[TAG:.*]]) AnyObject, #GenericClass.thing!1.foreign, bb1
// CHECK:   bb1({{%.*}} : @trivial $@convention(objc_method) @pseudogeneric (@opened([[TAG]]) AnyObject) -> @autoreleased Optional<AnyObject>):
// CHECK: } // end sil function '$S21objc_imported_generic0C24MethodOnAnyObjectChained1o1byXlSgyXl_SbtF'

public func genericSubscriptOnAnyObject(o: AnyObject, b: Bool) -> AnyObject? {
  return o[0 as UInt16]
}

// CHECK-LABEL: sil @$S21objc_imported_generic0C20SubscriptOnAnyObject1o1byXlSgyXl_SbtF
// CHECK: bb0([[ANY:%.*]]
// CHCEK:   [[OPENED_ANY:%.*]] = open_existential_ref [[ANY]]
// CHECK:   [[OPENED_ANY_COPY:%.*]] = copy_value [[OPENED_ANY]]
// CHECK:   dynamic_method_br [[OPENED_ANY_COPY]] : $@opened([[TAG:.*]]) AnyObject, #GenericClass.subscript!getter.1.foreign, bb1
// CHECK:   bb1({{%.*}} : @trivial $@convention(objc_method) @pseudogeneric (UInt16, @opened([[TAG]]) AnyObject) -> @autoreleased AnyObject):
// CHECK: } // end sil function '$S21objc_imported_generic0C20SubscriptOnAnyObject1o1byXlSgyXl_SbtF'

public func genericPropertyOnAnyObject(o: AnyObject, b: Bool) -> AnyObject?? {
  return o.propertyThing
}

// CHECK-LABEL: sil @$S21objc_imported_generic0C19PropertyOnAnyObject1o1byXlSgSgyXl_SbtF
// CHECK: bb0([[ANY:%.*]] : @guaranteed $AnyObject, [[BOOL:%.*]] : @trivial $Bool):
// CHECK:   [[OPENED_ANY:%.*]] = open_existential_ref [[ANY]]
// CHECK:   [[OPENED_ANY_COPY:%.*]] = copy_value [[OPENED_ANY]]
// CHECK:   dynamic_method_br [[OPENED_ANY_COPY]] : $@opened([[TAG:.*]]) AnyObject, #GenericClass.propertyThing!getter.1.foreign, bb1
// CHECK:   bb1({{%.*}} : @trivial $@convention(objc_method) @pseudogeneric (@opened([[TAG]]) AnyObject) -> @autoreleased Optional<AnyObject>):
// CHECK: } // end sil function '$S21objc_imported_generic0C19PropertyOnAnyObject1o1byXlSgSgyXl_SbtF'

public protocol ThingHolder {
  associatedtype Thing

  init!(thing: Thing!)
  func thing() -> Thing?
  func arrayOfThings() -> [Thing]
  func setArrayOfThings(_: [Thing])
  static func classThing() -> Thing?

  var propertyThing: Thing? { get set }
  var propertyArrayOfThings: [Thing]? { get set }
}

public protocol Ansible: class {
  associatedtype Anser: ThingHolder
}

public func genericBlockBridging<T: Ansible>(x: GenericClass<T>) {
  let block = x.blockForPerformingOnThings()
  x.performBlock(onThings: block)
}

// CHECK-LABEL: sil @$S21objc_imported_generic0C13BlockBridging{{[_0-9a-zA-Z]*}}F
// CHECK:         [[BLOCK_TO_FUNC:%.*]] = function_ref @$SxxIeyBya_xxIeggo_21objc_imported_generic7AnsibleRzlTR
// CHECK:         partial_apply [callee_guaranteed] [[BLOCK_TO_FUNC]]<T>
// CHECK:         [[FUNC_TO_BLOCK:%.*]] = function_ref @$SxxIeggo_xxIeyBya_21objc_imported_generic7AnsibleRzlTR
// CHECK:         init_block_storage_header {{.*}} invoke [[FUNC_TO_BLOCK]]<T>

// CHECK-LABEL: sil @$S21objc_imported_generic20arraysOfGenericParam{{[_0-9a-zA-Z]*}}F
public func arraysOfGenericParam<T: AnyObject>(y: Array<T>) {
  // CHECK:         function_ref @$SSo12GenericClassC13arrayOfThingsAByxGSgSayxG_tcfC : $@convention(method) <τ_0_0 where τ_0_0 : AnyObject> (@owned Array<τ_0_0>, @thick GenericClass<τ_0_0>.Type) -> @owned Optional<GenericClass<τ_0_0>>
  let x = GenericClass<T>(arrayOfThings: y)!
  // CHECK:         objc_method {{%.*}} : $GenericClass<T>, #GenericClass.setArrayOfThings!1.foreign {{.*}}, $@convention(objc_method) @pseudogeneric <τ_0_0 where τ_0_0 : AnyObject> (NSArray, GenericClass<τ_0_0>) -> ()
  x.setArrayOfThings(y)
  // CHECK:         objc_method {{%.*}} : $GenericClass<T>, #GenericClass.propertyArrayOfThings!getter.1.foreign {{.*}}, $@convention(objc_method) @pseudogeneric <τ_0_0 where τ_0_0 : AnyObject> (GenericClass<τ_0_0>) -> @autoreleased Optional<NSArray>
  _ = x.propertyArrayOfThings
  // CHECK:         objc_method {{%.*}} : $GenericClass<T>, #GenericClass.propertyArrayOfThings!setter.1.foreign {{.*}}, $@convention(objc_method) @pseudogeneric <τ_0_0 where τ_0_0 : AnyObject> (Optional<NSArray>, GenericClass<τ_0_0>) -> ()
  x.propertyArrayOfThings = y
}

// CHECK-LABEL: sil private @$S21objc_imported_generic0C4FuncyyxmRlzClFyycfU_ : $@convention(thin) <V where V : AnyObject> () -> () {
// CHECK:  [[META:%.*]] = metatype $@thick GenericClass<V>.Type
// CHECK:  [[INIT:%.*]] = function_ref @$SSo12GenericClassCAByxGycfC : $@convention(method) <τ_0_0 where τ_0_0 : AnyObject> (@thick GenericClass<τ_0_0>.Type) -> @owned GenericClass<τ_0_0>
// CHECK:  apply [[INIT]]<V>([[META]])
// CHECK:  return
func genericFunc<V: AnyObject>(_ v: V.Type) {
  let _ = {
    var _ = GenericClass<V>()
  }
}

// CHECK-LABEL: sil hidden @$S21objc_imported_generic23configureWithoutOptionsyyF : $@convention(thin) () -> ()
// CHECK: enum $Optional<Dictionary<GenericOption, Any>>, #Optional.none!enumelt
// CHECK: return
func configureWithoutOptions() {
  _ = GenericClass<NSObject>(options: nil)
}

// This gets emitted down here for some reason

// CHECK-LABEL: sil shared [serializable] [thunk] @$SSo12GenericClassC13arrayOfThingsAByxGSgSayxG_tcfcTO
// CHECK:         objc_method {{%.*}} : $GenericClass<T>, #GenericClass.init!initializer.1.foreign {{.*}}, $@convention(objc_method) @pseudogeneric <τ_0_0 where τ_0_0 : AnyObject> (NSArray, @owned GenericClass<τ_0_0>) -> @owned Optional<GenericClass<τ_0_0>>

// foreign to native thunk for init(options:), uses GenericOption : Hashable
// conformance

// CHECK-LABEL: sil shared [serializable] [thunk] @$SSo12GenericClassC7optionsAByxGSgSDySo0A6OptionaypGSg_tcfcTO : $@convention(method) <T where T : AnyObject> (@owned Optional<Dictionary<GenericOption, Any>>, @owned GenericClass<T>) -> @owned Optional<GenericClass<T>>
// CHECK: [[FN:%.*]] = function_ref @$SSD10FoundationE19_bridgeToObjectiveCSo12NSDictionaryCyF : $@convention(method) <τ_0_0, τ_0_1 where τ_0_0 : Hashable> (@guaranteed Dictionary<τ_0_0, τ_0_1>) -> @owned NSDictionary
// CHECK: apply [[FN]]<GenericOption, Any>({{.*}}) : $@convention(method) <τ_0_0, τ_0_1 where τ_0_0 : Hashable> (@guaranteed Dictionary<τ_0_0, τ_0_1>) -> @owned NSDictionary
// CHECK: return

// Make sure we emitted the witness table for the above conformance

// CHECK-LABEL: sil_witness_table shared [serialized] GenericOption: Hashable module objc_generics {
// CHECK: method #Hashable.hashValue!getter.1: {{.*}}: @$SSo13GenericOptionaSHSCSH9hashValueSivgTW
// CHECK: }
