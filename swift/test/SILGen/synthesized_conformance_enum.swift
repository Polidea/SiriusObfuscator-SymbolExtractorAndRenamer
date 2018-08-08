// RUN: %target-swift-frontend -assume-parsing-unqualified-ownership-sil -emit-silgen %s -swift-version 4 | %FileCheck %s

enum Enum<T> {
    case a(T), b(T)
}
// CHECK-LABEL: enum Enum<T> {
// CHECK:   case a(T), b(T)
// CHECK: }

enum NoValues {
    case a, b
}
// CHECK-LABEL: enum NoValues {
// CHECK:   case a, b
// CHECK:   @_implements(Equatable, ==(_:_:)) static func __derived_enum_equals(_ a: NoValues, _ b: NoValues) -> Bool
// CHECK:   var hashValue: Int { get }
// CHECK:   func hash(into hasher: inout Hasher)
// CHECK: }

// CHECK-LABEL: extension Enum : Equatable where T : Equatable {
// CHECK:   @_implements(Equatable, ==(_:_:)) static func __derived_enum_equals(_ a: Enum<T>, _ b: Enum<T>) -> Bool
// CHECK: }
// CHECK-LABEL: extension Enum : Hashable where T : Hashable {
// CHECK:   var hashValue: Int { get }
// CHECK:   func hash(into hasher: inout Hasher)
// CHECK: }

// CHECK-LABEL: extension NoValues : CaseIterable {
// CHECK:   typealias AllCases = [NoValues]
// CHECK:   static var allCases: [NoValues] { get }
// CHECK: }


extension Enum: Equatable where T: Equatable {}
// CHECK-LABEL: // static Enum<A>.__derived_enum_equals(_:_:)
// CHECK-NEXT: sil hidden @$S28synthesized_conformance_enum4EnumOAASQRzlE010__derived_C7_equalsySbACyxG_AEtFZ : $@convention(method) <T where T : Equatable> (@in_guaranteed Enum<T>, @in_guaranteed Enum<T>, @thin Enum<T>.Type) -> Bool {

extension Enum: Hashable where T: Hashable {}
// CHECK-LABEL: // Enum<A>.hashValue.getter
// CHECK-NEXT: sil hidden @$S28synthesized_conformance_enum4EnumOAASHRzlE9hashValueSivg : $@convention(method) <T where T : Hashable> (@in_guaranteed Enum<T>) -> Int {

// CHECK-LABEL: // Enum<A>.hash(into:)
// CHECK-NEXT: sil hidden @$S28synthesized_conformance_enum4EnumOAASHRzlE4hash4intoys6HasherVz_tF : $@convention(method) <T where T : Hashable> (@inout Hasher, @in_guaranteed Enum<T>) -> () {

extension NoValues: CaseIterable {}
// CHECK-LABEL: // static NoValues.allCases.getter
// CHECK-NEXT: sil hidden @$S28synthesized_conformance_enum8NoValuesO8allCasesSayACGvgZ : $@convention(method) (@thin NoValues.Type) -> @owned Array<NoValues> {


// Witness tables for Enum

// CHECK-LABEL: sil_witness_table hidden <T where T : Equatable> Enum<T>: Equatable module synthesized_conformance_enum {
// CHECK-NEXT:   method #Equatable."=="!1: <Self where Self : Equatable> (Self.Type) -> (Self, Self) -> Bool : @$S28synthesized_conformance_enum4EnumOyxGSQAASQRzlSQ2eeoiySbx_xtFZTW	// protocol witness for static Equatable.== infix(_:_:) in conformance <A> Enum<A>
// CHECK-NEXT:   conditional_conformance (T: Equatable): dependent
// CHECK-NEXT: }

// CHECK-LABEL: sil_witness_table hidden <T where T : Hashable> Enum<T>: Hashable module synthesized_conformance_enum {
// CHECK-NEXT:   base_protocol Equatable: <T where T : Equatable> Enum<T>: Equatable module synthesized_conformance_enum
// CHECK-NEXT:   method #Hashable.hashValue!getter.1: <Self where Self : Hashable> (Self) -> () -> Int : @$S28synthesized_conformance_enum4EnumOyxGSHAASHRzlSH9hashValueSivgTW	// protocol witness for Hashable.hashValue.getter in conformance <A> Enum<A>
// CHECK-NEXT:   method #Hashable.hash!1: <Self where Self : Hashable> (Self) -> (inout Hasher) -> () : @$S28synthesized_conformance_enum4EnumOyxGSHAASHRzlSH4hash4intoys6HasherVz_tFTW	// protocol witness for Hashable.hash(into:) in conformance <A> Enum<A>
// CHECK-NEXT:   conditional_conformance (T: Hashable): dependent
// CHECK-NEXT: }

// Witness tables for NoValues

// CHECK-LABEL: sil_witness_table hidden NoValues: CaseIterable module synthesized_conformance_enum {
// CHECK-NEXT:   associated_type AllCases: Array<NoValues>
// CHECK-NEXT:   associated_type_protocol (AllCases: Collection): [NoValues]: specialize <NoValues> (<Element> Array<Element>: Collection module Swift)
// CHECK-NEXT:   method #CaseIterable.allCases!getter.1: <Self where Self : CaseIterable> (Self.Type) -> () -> Self.AllCases : @$S28synthesized_conformance_enum8NoValuesOs12CaseIterableAAsADP8allCases03AllI0QzvgZTW // protocol witness for static CaseIterable.allCases.getter in conformance NoValues
// CHECK-NEXT: }


