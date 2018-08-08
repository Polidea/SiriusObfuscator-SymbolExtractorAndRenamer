// RUN: %target-swift-frontend -typecheck -verify -primary-file %s %S/Inputs/enum_conformance_synthesis_other.swift -verify-ignore-unknown -swift-version 4

var hasher = Hasher()

enum Foo: CaseIterable {
  case A, B
}

func foo() {
  if Foo.A == .B { }
  var _: Int = Foo.A.hashValue
  Foo.A.hash(into: &hasher)
  _ = Foo.allCases

  Foo.A == Foo.B // expected-warning {{result of operator '==' is unused}}
}

enum Generic<T>: CaseIterable {
  case A, B

  static func method() -> Int {
    // Test synthesis of == without any member lookup being done
    if A == B { }
    return Generic.A.hashValue
  }
}

func generic() {
  if Generic<Foo>.A == .B { }
  var _: Int = Generic<Foo>.A.hashValue
  Generic<Foo>.A.hash(into: &hasher)
  _ = Generic<Foo>.allCases
}

func localEnum() -> Bool {
  enum Local {
    case A, B
  }

  return Local.A == .B
}

enum CustomHashable {
  case A, B

  var hashValue: Int { return 0 }
}
func ==(x: CustomHashable, y: CustomHashable) -> Bool { // expected-note 4 {{non-matching type}}
  return true
}

func customHashable() {
  if CustomHashable.A == .B { }
  var _: Int = CustomHashable.A.hashValue
  CustomHashable.A.hash(into: &hasher)
}

// We still synthesize conforming overloads of '==' and 'hashValue' if
// explicit definitions don't satisfy the protocol requirements. Probably
// not what we actually want.
enum InvalidCustomHashable {
  case A, B

  var hashValue: String { return "" } // expected-note{{previously declared here}}
}
func ==(x: InvalidCustomHashable, y: InvalidCustomHashable) -> String { // expected-note 4 {{non-matching type}}
  return ""
}
func invalidCustomHashable() {
  if InvalidCustomHashable.A == .B { }
  var s: String = InvalidCustomHashable.A == .B
  s = InvalidCustomHashable.A.hashValue
  _ = s
  var _: Int = InvalidCustomHashable.A.hashValue
  InvalidCustomHashable.A.hash(into: &hasher)
}

// Check use of an enum's synthesized members before the enum is actually declared.
struct UseEnumBeforeDeclaration {
  let eqValue = EnumToUseBeforeDeclaration.A == .A
  let hashValue = EnumToUseBeforeDeclaration.A.hashValue
}
enum EnumToUseBeforeDeclaration {
  case A
}

func getFromOtherFile() -> AlsoFromOtherFile { return .A }
func overloadFromOtherFile() -> YetAnotherFromOtherFile { return .A }
func overloadFromOtherFile() -> Bool { return false }

func useEnumBeforeDeclaration() {
  // Check enums from another file in the same module.
  if FromOtherFile.A == .A {}
  let _: Int = FromOtherFile.A.hashValue

  if .A == getFromOtherFile() {}

  if .A == overloadFromOtherFile() {}
}

// Complex enums are not automatically Equatable, Hashable, or CaseIterable.
enum Complex {
  case A(Int)
  case B
}

func complex() {
  if Complex.A(1) == .B { } // expected-error{{binary operator '==' cannot be applied to operands of type 'Complex' and '_'}}
  // expected-note @-1 {{overloads for '==' exist with these partially matching parameter lists: }}
}

// Enums with equatable payloads are equatable if they explicitly conform.
enum EnumWithEquatablePayload: Equatable {
  case A(Int)
  case B(String, Int)
  case C
}

func enumWithEquatablePayload() {
  if EnumWithEquatablePayload.A(1) == .B("x", 1) { }
  if EnumWithEquatablePayload.A(1) == .C { }
  if EnumWithEquatablePayload.B("x", 1) == .C { }
}

// Enums with hashable payloads are hashable if they explicitly conform.
enum EnumWithHashablePayload: Hashable {
  case A(Int)
  case B(String, Int)
  case C
}

func enumWithHashablePayload() {
  _ = EnumWithHashablePayload.A(1).hashValue
  _ = EnumWithHashablePayload.B("x", 1).hashValue
  _ = EnumWithHashablePayload.C.hashValue

  EnumWithHashablePayload.A(1).hash(into: &hasher)
  EnumWithHashablePayload.B("x", 1).hash(into: &hasher)
  EnumWithHashablePayload.C.hash(into: &hasher)

  // ...and they should also inherit equatability from Hashable.
  if EnumWithHashablePayload.A(1) == .B("x", 1) { }
  if EnumWithHashablePayload.A(1) == .C { }
  if EnumWithHashablePayload.B("x", 1) == .C { }
}

// Enums with non-hashable payloads don't derive conformance.
struct NotHashable {}
enum EnumWithNonHashablePayload: Hashable { // expected-error 2 {{does not conform}}
  case A(NotHashable)
}

// Enums should be able to derive conformances based on the conformances of
// their generic arguments.
enum GenericHashable<T: Hashable>: Hashable {
  case A(T)
  case B
}
func genericHashable() {
  if GenericHashable<String>.A("a") == .B { }
  var _: Int = GenericHashable<String>.A("a").hashValue
}

// But it should be an error if the generic argument doesn't have the necessary
// constraints to satisfy the conditions for derivation.
enum GenericNotHashable<T: Equatable>: Hashable { // expected-error 2 {{does not conform to protocol 'Hashable'}}
  case A(T)
  case B
}
func genericNotHashable() {
  if GenericNotHashable<String>.A("a") == .B { }
  let _: Int = GenericNotHashable<String>.A("a").hashValue // No error. hashValue is always synthesized, even if Hashable derivation fails
  GenericNotHashable<String>.A("a").hash(into: &hasher) // expected-error {{value of type 'GenericNotHashable<String>' has no member 'hash'}}
}

// An enum with no cases should also derive conformance.
enum NoCases: Hashable {}

// rdar://19773050
private enum Bar<T> {
  case E(Unknown<T>)  // expected-error {{use of undeclared type 'Unknown'}}

  mutating func value() -> T {
    switch self {
    // FIXME: Should diagnose here that '.' needs to be inserted, but E has an ErrorType at this point
    case E(let x):
      return x.value
    }
  }
}

// Equatable extension -- rdar://20981254
enum Instrument {
  case Piano
  case Violin
  case Guitar
}

extension Instrument : Equatable {}

extension Instrument : CaseIterable {}

enum UnusedGeneric<T> {
    case a, b, c
}
extension UnusedGeneric : CaseIterable {}

// Explicit conformance should work too
public enum Medicine {
  case Antibiotic
  case Antihistamine
}

extension Medicine : Equatable {}

public func ==(lhs: Medicine, rhs: Medicine) -> Bool { // expected-note 4 {{non-matching type}}
  return true
}

// No explicit conformance; but it can be derived, for the same-file cases.
enum Complex2 {
  case A(Int)
  case B
}
extension Complex2 : Hashable {}
extension Complex2 : CaseIterable {}  // expected-error {{type 'Complex2' does not conform to protocol 'CaseIterable'}}
extension FromOtherFile: CaseIterable {} // expected-error {{cannot be automatically synthesized in an extension in a different file to the type}} expected-error {{does not conform to protocol 'CaseIterable'}}

// No explicit conformance and it cannot be derived.
enum NotExplicitlyHashableAndCannotDerive {
  case A(NotHashable)
}
extension NotExplicitlyHashableAndCannotDerive : Hashable {} // expected-error 2 {{does not conform}}
extension NotExplicitlyHashableAndCannotDerive : CaseIterable {} // expected-error {{does not conform}}

// Verify that conformance (albeit manually implemented) can still be added to
// a type in a different file.
extension OtherFileNonconforming: Hashable {
  static func ==(lhs: OtherFileNonconforming, rhs: OtherFileNonconforming) -> Bool { // expected-note 4 {{non-matching type}}
    return true
  }
  var hashValue: Int { return 0 }
}
// ...but synthesis in a type defined in another file doesn't work yet.
extension YetOtherFileNonconforming: Equatable {} // expected-error {{cannot be automatically synthesized in an extension in a different file to the type}}
extension YetOtherFileNonconforming: CaseIterable {} // expected-error {{does not conform}}

// Verify that an indirect enum doesn't emit any errors as long as its "leaves"
// are conformant.
enum StringBinaryTree: Hashable {
  indirect case tree(StringBinaryTree, StringBinaryTree)
  case leaf(String)
}

// Add some generics to make it more complex.
enum BinaryTree<Element: Hashable>: Hashable {
  indirect case tree(BinaryTree, BinaryTree)
  case leaf(Element)
}

// Verify mutually indirect enums.
enum MutuallyIndirectA: Hashable {
  indirect case b(MutuallyIndirectB)
  case data(Int)
}
enum MutuallyIndirectB: Hashable {
  indirect case a(MutuallyIndirectA)
  case data(Int)
}

// Verify that it works if the enum itself is indirect, rather than the cases.
indirect enum TotallyIndirect: Hashable {
  case another(TotallyIndirect)
  case end(Int)
}

// Check the use of conditional conformances.
enum ArrayOfEquatables : Equatable {
case only([Int])
}

struct NotEquatable { }

enum ArrayOfNotEquatables : Equatable { // expected-error{{type 'ArrayOfNotEquatables' does not conform to protocol 'Equatable'}}
case only([NotEquatable])
}

// Conditional conformances should be able to be synthesized
enum GenericDeriveExtension<T> {
    case A(T)
}
extension GenericDeriveExtension: Equatable where T: Equatable {}
extension GenericDeriveExtension: Hashable where T: Hashable {}

// Incorrectly/insufficiently conditional shouldn't work
enum BadGenericDeriveExtension<T> {
    case A(T)
}
extension BadGenericDeriveExtension: Equatable {}
// expected-error@-1 {{type 'BadGenericDeriveExtension<T>' does not conform to protocol 'Equatable'}}
extension BadGenericDeriveExtension: Hashable where T: Equatable {}
// expected-error@-1 {{type 'BadGenericDeriveExtension' does not conform to protocol 'Hashable'}}

// But some cases don't need to be conditional, even if they look similar to the
// above
struct AlwaysHashable<T>: Hashable {}
enum UnusedGenericDeriveExtension<T> {
    case A(AlwaysHashable<T>)
}
extension UnusedGenericDeriveExtension: Hashable {}

// Cross-file synthesis is disallowed for conditional cases just as it is for
// non-conditional ones.
extension GenericOtherFileNonconforming: Equatable where T: Equatable {}
// expected-error@-1{{implementation of 'Equatable' cannot be automatically synthesized in an extension in a different file to the type}}

// rdar://problem/41852654

// There is a conformance to Equatable (or at least, one that implies Equatable)
// in the same file as the type, so the synthesis is okay. Both orderings are
// tested, to catch choosing extensions based on the order of the files, etc.
protocol ImplierMain: Equatable {}
enum ImpliedMain: ImplierMain {
  case a(Int)
}
extension ImpliedOther: ImplierMain {}


// FIXME: Remove -verify-ignore-unknown.
// <unknown>:0: error: unexpected error produced: invalid redeclaration of 'hashValue'
// <unknown>:0: error: unexpected note produced: candidate has non-matching type '(Foo, Foo) -> Bool'
// <unknown>:0: error: unexpected note produced: candidate has non-matching type '<T> (Generic<T>, Generic<T>) -> Bool'
// <unknown>:0: error: unexpected note produced: candidate has non-matching type '(InvalidCustomHashable, InvalidCustomHashable) -> Bool'
// <unknown>:0: error: unexpected note produced: candidate has non-matching type '(EnumToUseBeforeDeclaration, EnumToUseBeforeDeclaration) -> Bool'
