// RUN: %target-typecheck-verify-swift -swift-version 4.2
// RUN: %target-typecheck-verify-swift -swift-version 4.2 -enable-testing
// RUN: %target-typecheck-verify-swift -swift-version 4.2 -enable-resilience
// RUN: %target-typecheck-verify-swift -swift-version 4.2 -enable-resilience -enable-testing
@inlinable struct TestInlinableStruct {}
// expected-error@-1 {{'@inlinable' attribute cannot be applied to this declaration}}

@inlinable @usableFromInline func redundantAttribute() {}
// expected-warning@-1 {{'@inlinable' declaration is already '@usableFromInline'}}

private func privateFunction() {}
// expected-note@-1{{global function 'privateFunction()' is not '@usableFromInline' or public}}
fileprivate func fileprivateFunction() {}
// expected-note@-1{{global function 'fileprivateFunction()' is not '@usableFromInline' or public}}
func internalFunction() {}
// expected-note@-1{{global function 'internalFunction()' is not '@usableFromInline' or public}}
@usableFromInline func versionedFunction() {}
public func publicFunction() {}

private struct PrivateStruct {}
// expected-note@-1 3{{struct 'PrivateStruct' is not '@usableFromInline' or public}}
struct InternalStruct {}
// expected-note@-1 4{{struct 'InternalStruct' is not '@usableFromInline' or public}}
@usableFromInline struct VersionedStruct {
  @usableFromInline init() {}
}
public struct PublicStruct {
  public init() {}

  @inlinable public var storedProperty: Int
  // expected-error@-1 {{'@inlinable' attribute cannot be applied to stored properties}}

  @inlinable public lazy var lazyProperty: Int = 0
  // expected-error@-1 {{'@inlinable' attribute cannot be applied to stored properties}}
}

public struct Struct {
  @_transparent
  public func publicTransparentMethod() {
    struct Nested {}
    // expected-error@-1 {{type 'Nested' cannot be nested inside a '@_transparent' function}}

    publicFunction()
    // OK
    versionedFunction()
    // OK
    internalFunction()
    // expected-error@-1 {{global function 'internalFunction()' is internal and cannot be referenced from a '@_transparent' function}}
    fileprivateFunction()
    // expected-error@-1 {{global function 'fileprivateFunction()' is fileprivate and cannot be referenced from a '@_transparent' function}}
    privateFunction()
    // expected-error@-1 {{global function 'privateFunction()' is private and cannot be referenced from a '@_transparent' function}}
  }

  @inlinable
  public func publicInlinableMethod() {
    struct Nested {}
    // expected-error@-1 {{type 'Nested' cannot be nested inside an '@inlinable' function}}

    let _: PublicStruct
    let _: VersionedStruct
    let _: InternalStruct
    // expected-error@-1 {{struct 'InternalStruct' is internal and cannot be referenced from an '@inlinable' function}}
    let _: PrivateStruct
    // expected-error@-1 {{struct 'PrivateStruct' is private and cannot be referenced from an '@inlinable' function}}

    let _ = PublicStruct.self
    let _ = VersionedStruct.self
    let _ = InternalStruct.self
    // expected-error@-1 {{struct 'InternalStruct' is internal and cannot be referenced from an '@inlinable' function}}
    let _ = PrivateStruct.self
    // expected-error@-1 {{struct 'PrivateStruct' is private and cannot be referenced from an '@inlinable' function}}

    let _ = PublicStruct()
    let _ = VersionedStruct()
    let _ = InternalStruct()
    // expected-error@-1 {{struct 'InternalStruct' is internal and cannot be referenced from an '@inlinable' function}}
    let _ = PrivateStruct()
    // expected-error@-1 {{struct 'PrivateStruct' is private and cannot be referenced from an '@inlinable' function}}
  }

  @inline(__always)
  public func publicInlineAlwaysMethod(x: Any) {
    struct Nested {}
    // expected-error@-1 {{type 'Nested' cannot be nested inside an '@inline(__always)' function}}

    switch x {
      case is InternalStruct:
      // expected-error@-1 {{struct 'InternalStruct' is internal and cannot be referenced from an '@inline(__always)' function}}
        _ = ()
    }
  }

  private func privateMethod() {}
  // expected-note@-1 {{instance method 'privateMethod()' is not '@usableFromInline' or public}}

  @_transparent
  @usableFromInline
  func versionedTransparentMethod() {
    struct Nested {}
    // expected-error@-1 {{type 'Nested' cannot be nested inside a '@_transparent' function}}
    privateMethod()
    // expected-error@-1 {{instance method 'privateMethod()' is private and cannot be referenced from a '@_transparent' function}}
  }

  @inlinable
  func internalInlinableMethod() {
    struct Nested {}
    // expected-error@-1 {{type 'Nested' cannot be nested inside an '@inlinable' function}}
  }

  @inline(__always)
  @usableFromInline
  func versionedInlineAlwaysMethod() {
    struct Nested {}
    // expected-error@-1 {{type 'Nested' cannot be nested inside an '@inline(__always)' function}}
  }

  @_transparent
  func internalTransparentMethod() {
    struct Nested {}
    // OK
  }

  @inlinable
  private func privateInlinableMethod() {
  // expected-error@-2 {{'@inlinable' attribute can only be applied to public declarations, but 'privateInlinableMethod' is private}}
    struct Nested {}
    // OK
  }

  @inline(__always)
  func internalInlineAlwaysMethod() {
    struct Nested {}
    // OK
  }
}

// Make sure protocol extension members can reference protocol requirements
// (which do not inherit the @usableFromInline attribute).
@usableFromInline
protocol VersionedProtocol {
  associatedtype T

  func requirement() -> T
}

extension VersionedProtocol {
  func internalMethod() {}
  // expected-note@-1 {{instance method 'internalMethod()' is not '@usableFromInline' or public}}

  @inlinable
  func versionedMethod() -> T {
    internalMethod()
    // expected-error@-1 {{instance method 'internalMethod()' is internal and cannot be referenced from an '@inlinable' function}}

    return requirement()
  }
}

enum InternalEnum {
// expected-note@-1 2{{enum 'InternalEnum' is not '@usableFromInline' or public}}
  case apple
  case orange
}

@inlinable public func usesInternalEnum() {
  _ = InternalEnum.apple
  // expected-error@-1 {{enum 'InternalEnum' is internal and cannot be referenced from an '@inlinable' function}}
  let _: InternalEnum = .orange
  // expected-error@-1 {{enum 'InternalEnum' is internal and cannot be referenced from an '@inlinable' function}}
}

@usableFromInline enum VersionedEnum {
  case apple
  case orange
  // FIXME: Should this be banned?
  case pear(InternalEnum)
  case persimmon(String)
}

@inlinable public func usesVersionedEnum() {
  _ = VersionedEnum.apple
  let _: VersionedEnum = .orange
  _ = VersionedEnum.persimmon
}

// Inherited initializers - <rdar://problem/34398148>
@usableFromInline
@_fixed_layout
class Base {
  @usableFromInline
  init(x: Int) {}
}

@usableFromInline
@_fixed_layout
class Middle : Base {}

@usableFromInline
@_fixed_layout
class Derived : Middle {
  @inlinable
  init(y: Int) {
    super.init(x: y)
  }
}

// More inherited initializers
@_fixed_layout
public class Base2 {
  @inlinable
  public init(x: Int) {}
}

@_fixed_layout
@usableFromInline
class Middle2 : Base2 {}

@_fixed_layout
@usableFromInline
class Derived2 : Middle2 {
  @inlinable
  init(y: Int) {
    super.init(x: y)
  }
}

// Stored property initializer expressions.
//
// Note the behavior here does not depend on the state of the -enable-resilience
// flag; the test runs with both the flag on and off. Only the explicit
// presence of a '@_fixed_layout' attribute determines the behavior here.

let internalGlobal = 0
// expected-note@-1 {{let 'internalGlobal' is not '@usableFromInline' or public}}
public let publicGlobal = 0

struct InternalStructWithInit {
  var x = internalGlobal // OK
  var y = publicGlobal // OK
}

public struct PublicResilientStructWithInit {
  var x = internalGlobal // OK
  var y = publicGlobal // OK
}

@_fixed_layout
public struct PublicFixedStructWithInit {
  var x = internalGlobal // expected-error {{let 'internalGlobal' is internal and cannot be referenced from a property initializer in a '@_fixed_layout' type}}
  var y = publicGlobal // OK
}

public struct KeypathStruct {
  var x: Int
  // expected-note@-1 {{var 'x' is not '@usableFromInline' or public}}

  @inlinable public func usesKeypath() {
    _ = \KeypathStruct.x
    // expected-error@-1 {{var 'x' is internal and cannot be referenced from an '@inlinable' function}}
  }
}
