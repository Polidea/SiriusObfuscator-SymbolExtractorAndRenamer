// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-sil -o - -emit-module-path %t/Lib.swiftmodule -module-name Lib -I %S/Inputs/custom-modules -disable-objc-attr-requires-foundation-module %s | %FileCheck -check-prefix CHECK-VTABLE %s

// RUN: %target-swift-ide-test -source-filename=x -print-module -module-to-print Lib -I %t -I %S/Inputs/custom-modules | %FileCheck %s

// RUN: %target-swift-ide-test -source-filename=x -print-module -module-to-print Lib -I %t -I %S/Inputs/custom-modules -Xcc -DBAD > %t.txt
// RUN: %FileCheck -check-prefix CHECK-RECOVERY %s < %t.txt
// RUN: %FileCheck -check-prefix CHECK-RECOVERY-NEGATIVE %s < %t.txt

// RUN: %target-swift-frontend -typecheck -I %t -I %S/Inputs/custom-modules -Xcc -DBAD -DTEST -DVERIFY %s -verify
// RUN: %target-swift-frontend -emit-silgen -I %t -I %S/Inputs/custom-modules -Xcc -DBAD -DTEST %s | %FileCheck -check-prefix CHECK-SIL %s

// RUN: %target-swift-frontend -emit-ir -I %t -I %S/Inputs/custom-modules -DTEST %s | %FileCheck -check-prefix CHECK-IR %s
// RUN: %target-swift-frontend -emit-ir -I %t -I %S/Inputs/custom-modules -Xcc -DBAD -DTEST %s | %FileCheck -check-prefix CHECK-IR %s

#if TEST

import Typedefs
import Lib

// CHECK-SIL-LABEL: sil hidden @_T08typedefs11testSymbolsyyF
func testSymbols() {
  // Check that the symbols are not using 'Bool'.
  // CHECK-SIL: function_ref @_T03Lib1xs5Int32Vfau
  _ = Lib.x
  // CHECK-SIL: function_ref @_T03Lib9usesAssocs5Int32VSgfau
  _ = Lib.usesAssoc
} // CHECK-SIL: end sil function '_T08typedefs11testSymbolsyyF'

// CHECK-IR-LABEL: define{{.*}} void @_T08typedefs18testVTableBuildingy3Lib4UserC4user_tF
public func testVTableBuilding(user: User) {
  // The important thing in this CHECK line is the "i64 24", which is the offset
  // for the vtable slot for 'lastMethod()'. If the layout here
  // changes, please check that offset 24 is still correct.
  // CHECK-IR-NOT: ret
  // CHECK-IR: getelementptr inbounds void (%T3Lib4UserC*)*, void (%T3Lib4UserC*)** %{{[0-9]+}}, {{i64 24|i32 27}}
  _ = user.lastMethod()
} // CHECK-IR: ret void

#if VERIFY
let _: String = useAssoc(ImportedType.self) // expected-error {{cannot convert value of type 'Int32?' to specified type 'String'}}
let _: Bool? = useAssoc(ImportedType.self) // expected-error {{cannot convert value of type 'Int32?' to specified type 'Bool?'}}
let _: Int32? = useAssoc(ImportedType.self)

let _: String = useAssoc(AnotherType.self) // expected-error {{cannot convert value of type 'AnotherType.Assoc?' (aka 'Optional<Int32>') to specified type 'String'}}
let _: Bool? = useAssoc(AnotherType.self) // expected-error {{cannot convert value of type 'AnotherType.Assoc?' (aka 'Optional<Int32>') to specified type 'Bool?'}}
let _: Int32? = useAssoc(AnotherType.self)

let _ = wrapped // expected-error {{use of unresolved identifier 'wrapped'}}
let _ = unwrapped // okay

_ = usesWrapped(nil) // expected-error {{use of unresolved identifier 'usesWrapped'}}
_ = usesUnwrapped(nil) // expected-error {{nil is not compatible with expected argument type 'Int32'}}

public class UserDynamicSub: UserDynamic {
  override init() {}
}
// FIXME: Bad error message; really it's that the convenience init hasn't been
// inherited.
_ = UserDynamicSub(conveniently: 0) // expected-error {{argument passed to call that takes no arguments}}

public class UserDynamicConvenienceSub: UserDynamicConvenience {
  override init() {}
}
_ = UserDynamicConvenienceSub(conveniently: 0)

public class UserSub : User {} // expected-error {{cannot inherit from class 'User' because it has overridable members that could not be loaded}}

#endif // VERIFY

#else // TEST

import Typedefs

// CHECK-LABEL: class User {
// CHECK-RECOVERY-LABEL: class User {
open class User {
  // CHECK: var unwrappedProp: UnwrappedInt?
  // CHECK-RECOVERY: var unwrappedProp: Int32?
  public var unwrappedProp: UnwrappedInt?
  // CHECK: var wrappedProp: WrappedInt?
  // CHECK-RECOVERY: /* placeholder for _ */
  // CHECK-RECOVERY: /* placeholder for _ */
  // CHECK-RECOVERY: /* placeholder for _ */
  public var wrappedProp: WrappedInt?

  // CHECK: func returnsUnwrappedMethod() -> UnwrappedInt
  // CHECK-RECOVERY: func returnsUnwrappedMethod() -> Int32
  public func returnsUnwrappedMethod() -> UnwrappedInt { fatalError() }
  // CHECK: func returnsWrappedMethod() -> WrappedInt
  // CHECK-RECOVERY: /* placeholder for returnsWrappedMethod() */
  public func returnsWrappedMethod() -> WrappedInt { fatalError() }

  // CHECK: subscript(_: WrappedInt) -> () { get }
  // CHECK-RECOVERY: /* placeholder for _ */
  public subscript(_: WrappedInt) -> () { return () }

  // CHECK: init()
  // CHECK-RECOVERY: init()
  public init() {}

  // CHECK: init(wrapped: WrappedInt)
  // CHECK-RECOVERY: /* placeholder for init(wrapped:) */
  public init(wrapped: WrappedInt) {}

  // CHECK: convenience init(conveniently: Int)
  // CHECK-RECOVERY: convenience init(conveniently: Int)
  public convenience init(conveniently: Int) { self.init() }

  // CHECK: required init(wrappedRequired: WrappedInt)
  // CHECK-RECOVERY: /* placeholder for init(wrappedRequired:) */
  public required init(wrappedRequired: WrappedInt) {}

  public func lastMethod() {}
}
// CHECK: {{^}$}}
// CHECK-RECOVERY: {{^}$}}

// This is mostly to check when changes are necessary for the CHECK-IR lines
// above.
// CHECK-VTABLE-LABEL: sil_vtable User {
// (10 words of normal class metadata on 64-bit platforms, 13 on 32-bit)
// 10 CHECK-VTABLE-NEXT: #User.unwrappedProp!getter.1:
// 11 CHECK-VTABLE-NEXT: #User.unwrappedProp!setter.1:
// 12 CHECK-VTABLE-NEXT: #User.unwrappedProp!materializeForSet.1:
// 13 CHECK-VTABLE-NEXT: #User.wrappedProp!getter.1:
// 14 CHECK-VTABLE-NEXT: #User.wrappedProp!setter.1:
// 15 CHECK-VTABLE-NEXT: #User.wrappedProp!materializeForSet.1:
// 16 CHECK-VTABLE-NEXT: #User.returnsUnwrappedMethod!1:
// 17 CHECK-VTABLE-NEXT: #User.returnsWrappedMethod!1:
// 18 CHECK-VTABLE-NEXT: #User.subscript!getter.1:
// 19 CHECK-VTABLE-NEXT: #User.init!initializer.1:
// 20 CHECK-VTABLE-NEXT: #User.init!initializer.1:
// 21 CHECK-VTABLE-NEXT: #User.init!initializer.1:
// 22 CHECK-VTABLE-NEXT: #User.init!allocator.1:
// 23 CHECK-VTABLE-NEXT: #User.init!initializer.1:
// 24 CHECK-VTABLE-NEXT: #User.lastMethod!1:
// CHECK-VTABLE: }


// CHECK-LABEL: class UserConvenience
// CHECK-RECOVERY-LABEL: class UserConvenience
open class UserConvenience {
  // CHECK: init()
  // CHECK-RECOVERY: init()
  public init() {}

  // CHECK: convenience init(wrapped: WrappedInt)
  // CHECK-RECOVERY: /* placeholder for init(wrapped:) */
  public convenience init(wrapped: WrappedInt) { self.init() }

  // CHECK: convenience init(conveniently: Int)
  // CHECK-RECOVERY: convenience init(conveniently: Int)
  public convenience init(conveniently: Int) { self.init() }
}
// CHECK: {{^}$}}
// CHECK-RECOVERY: {{^}$}}

// CHECK-LABEL: class UserDynamic
// CHECK-RECOVERY-LABEL: class UserDynamic
open class UserDynamic {
  // CHECK: init()
  // CHECK-RECOVERY: init()
  @objc public dynamic init() {}

  // CHECK: init(wrapped: WrappedInt)
  // CHECK-RECOVERY: /* placeholder for init(wrapped:) */
  @objc public dynamic init(wrapped: WrappedInt) {}

  // CHECK: convenience init(conveniently: Int)
  // CHECK-RECOVERY: convenience init(conveniently: Int)
  @objc public dynamic convenience init(conveniently: Int) { self.init() }
}
// CHECK: {{^}$}}
// CHECK-RECOVERY: {{^}$}}

// CHECK-LABEL: class UserDynamicConvenience
// CHECK-RECOVERY-LABEL: class UserDynamicConvenience
open class UserDynamicConvenience {
  // CHECK: init()
  // CHECK-RECOVERY: init()
  @objc public dynamic init() {}

  // CHECK: convenience init(wrapped: WrappedInt)
  // CHECK-RECOVERY: /* placeholder for init(wrapped:) */
  @objc public dynamic convenience init(wrapped: WrappedInt) { self.init() }

  // CHECK: convenience init(conveniently: Int)
  // CHECK-RECOVERY: convenience init(conveniently: Int)
  @objc public dynamic convenience init(conveniently: Int) { self.init() }
}
// CHECK: {{^}$}}
// CHECK-RECOVERY: {{^}$}}


// CHECK-LABEL: class UserSub
// CHECK-RECOVERY-LABEL: class UserSub
open class UserSub : User {
  // CHECK: init(wrapped: WrappedInt?)
  // CHECK-RECOVERY: /* placeholder for init(wrapped:) */
  public override init(wrapped: WrappedInt?) { super.init() }

  // CHECK: required init(wrappedRequired: WrappedInt?)
  // CHECK-RECOVERY: /* placeholder for init(wrappedRequired:) */
  public required init(wrappedRequired: WrappedInt?) { super.init() }
}
// CHECK: {{^}$}}
// CHECK-RECOVERY: {{^}$}}


// CHECK-DAG: let x: MysteryTypedef
// CHECK-RECOVERY-DAG: let x: Int32
public let x: MysteryTypedef = 0

public protocol HasAssoc {
  associatedtype Assoc
}

extension ImportedType: HasAssoc {}

public struct AnotherType: HasAssoc {
  public typealias Assoc = MysteryTypedef
}

public func useAssoc<T: HasAssoc>(_: T.Type) -> T.Assoc? { return nil }

// CHECK-DAG: let usesAssoc: ImportedType.Assoc?
// CHECK-RECOVERY-DAG: let usesAssoc: Int32?
public let usesAssoc = useAssoc(ImportedType.self)
// CHECK-DAG: let usesAssoc2: AnotherType.Assoc?
// CHECK-RECOVERY-DAG: let usesAssoc2: AnotherType.Assoc?
public let usesAssoc2 = useAssoc(AnotherType.self)


// CHECK-DAG: let wrapped: WrappedInt
// CHECK-RECOVERY-NEGATIVE-NOT: let wrapped:
public let wrapped = WrappedInt(0)
// CHECK-DAG: let unwrapped: UnwrappedInt
// CHECK-RECOVERY-DAG: let unwrapped: Int32
public let unwrapped: UnwrappedInt = 0

// CHECK-DAG: let wrappedMetatype: WrappedInt.Type
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedMetatype:
public let wrappedMetatype = WrappedInt.self
// CHECK-DAG: let wrappedOptional: WrappedInt?
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedOptional:
public let wrappedOptional: WrappedInt? = nil
// CHECK-DAG: let wrappedIUO: WrappedInt!
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedIUO:
public let wrappedIUO: WrappedInt! = nil
// CHECK-DAG: let wrappedArray: [WrappedInt]
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedArray:
public let wrappedArray: [WrappedInt] = []
// CHECK-DAG: let wrappedDictionary: [Int : WrappedInt]
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedDictionary:
public let wrappedDictionary: [Int: WrappedInt] = [:]
// CHECK-DAG: let wrappedTuple: (WrappedInt, Int)?
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedTuple:
public let wrappedTuple: (WrappedInt, Int)? = nil
// CHECK-DAG: let wrappedTuple2: (Int, WrappedInt)?
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedTuple2:
public let wrappedTuple2: (Int, WrappedInt)? = nil
// CHECK-DAG: let wrappedClosure: ((WrappedInt) -> Void)?
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedClosure:
public let wrappedClosure: ((WrappedInt) -> Void)? = nil
// CHECK-DAG: let wrappedClosure2: (() -> WrappedInt)?
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedClosure2:
public let wrappedClosure2: (() -> WrappedInt)? = nil
// CHECK-DAG: let wrappedClosure3: ((Int, WrappedInt) -> Void)?
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedClosure3:
public let wrappedClosure3: ((Int, WrappedInt) -> Void)? = nil
// CHECK-DAG: let wrappedClosureInout: ((inout WrappedInt) -> Void)?
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedClosureInout:
public let wrappedClosureInout: ((inout WrappedInt) -> Void)? = nil


// CHECK-DAG: var wrappedFirst: WrappedInt?
// CHECK-DAG: var normalSecond: Int?
// CHECK-RECOVERY-NEGATIVE-NOT: var wrappedFirst:
// CHECK-RECOVERY-DAG: var normalSecond: Int?
public var wrappedFirst: WrappedInt?, normalSecond: Int?
// CHECK-DAG: var normalFirst: Int?
// CHECK-DAG: var wrappedSecond: WrappedInt?
// CHECK-RECOVERY-DAG: var normalFirst: Int?
// CHECK-RECOVERY-NEGATIVE-NOT: var wrappedSecond:
public var normalFirst: Int?, wrappedSecond: WrappedInt?
// CHECK-DAG: var wrappedThird: WrappedInt?
// CHECK-DAG: var wrappedFourth: WrappedInt?
// CHECK-RECOVERY-NEGATIVE-NOT: var wrappedThird:
// CHECK-RECOVERY-NEGATIVE-NOT: var wrappedFourth:
public var wrappedThird, wrappedFourth: WrappedInt?

// CHECK-DAG: func usesWrapped(_ wrapped: WrappedInt)
// CHECK-RECOVERY-NEGATIVE-NOT: func usesWrapped(
public func usesWrapped(_ wrapped: WrappedInt) {}
// CHECK-DAG: func usesUnwrapped(_ unwrapped: UnwrappedInt)
// CHECK-RECOVERY-DAG: func usesUnwrapped(_ unwrapped: Int32)
public func usesUnwrapped(_ unwrapped: UnwrappedInt) {}

// CHECK-DAG: func returnsWrapped() -> WrappedInt
// CHECK-RECOVERY-NEGATIVE-NOT: func returnsWrapped(
public func returnsWrapped() -> WrappedInt { fatalError() }

// CHECK-DAG: func returnsWrappedGeneric<T>(_: T.Type) -> WrappedInt
// CHECK-RECOVERY-NEGATIVE-NOT: func returnsWrappedGeneric<
public func returnsWrappedGeneric<T>(_: T.Type) -> WrappedInt { fatalError() }

#endif // TEST
