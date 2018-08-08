// RUN: %target-typecheck-verify-swift

protocol P {
  associatedtype T = Int

  func hasDefault()
  func returnsSelf() -> Self
  func hasDefaultTakesT(_: T)
  func returnsSelfTakesT(_: T) -> Self
}

extension P {
  func hasDefault() {}

  func returnsSelf() -> Self {
    return self
  }

  func hasDefaultTakesT(_: T) {}

  func returnsSelfTakesT(_: T) -> Self { // expected-note {{'returnsSelfTakesT' declared here}}
    return self
  }
}

// This fails
class Class : P {} // expected-error {{method 'returnsSelfTakesT' in non-final class 'Class' cannot be implemented in a protocol extension because it returns 'Self' and has associated type requirements}}

// This succeeds, because the class is final
final class FinalClass : P {}

// This succeeds, because we're not using the default implementation
class NonFinalClass : P {
  func returnsSelfTakesT(_: T) -> Self {
    return self
  }
}

// Test for default implementation that comes from a constrained extension
// - https://bugs.swift.org/browse/SR-7422

// FIXME: Better error message here?

class SillyClass {}

protocol HasDefault {
  func foo()
  // expected-note@-1 {{protocol requires function 'foo()' with type '() -> ()'; do you want to add a stub?}}
}

extension HasDefault where Self == SillyClass {
  func foo() {}
  // expected-note@-1 {{candidate has non-matching type '<Self> () -> ()'}}
}

extension SillyClass : HasDefault {}
// expected-error@-1 {{type 'SillyClass' does not conform to protocol 'HasDefault'}}

// This is OK, though
class SeriousClass {}

extension HasDefault where Self : SeriousClass {
  func foo() {}
  // expected-note@-1 {{candidate has non-matching type '<Self> () -> ()'}}

  // FIXME: the above diangostic is from trying to check conformance for
  // 'SillyClass' and not 'SeriousClass'. Evidently name lookup finds members
  // from all constrained extensions, and then if any don't have a matching
  // generic signature, diagnostics doesn't really know what to do about it.
}

extension SeriousClass : HasDefault {}

// https://bugs.swift.org/browse/SR-7428

protocol Node {
  associatedtype ValueType = Int

  func addChild<ChildType>(_ child: ChildType)
    where ChildType: Node, ChildType.ValueType == Self.ValueType
}

extension Node {
  func addChild<ChildType>(_ child: ChildType)
    where ChildType: Node, ChildType.ValueType == Self.ValueType {}
}

class IntNode: Node {}
