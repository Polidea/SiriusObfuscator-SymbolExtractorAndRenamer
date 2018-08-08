// RUN: %target-swift-frontend -assume-parsing-unqualified-ownership-sil -emit-silgen %s -swift-version 4 | %FileCheck %s

struct Struct<T> {
    var x: T
}

// CHECK-LABEL: struct Struct<T> {
// CHECK:   @sil_stored var x: T { get set }
// CHECK:   init(x: T)
// CHECK:   enum CodingKeys : CodingKey {
// CHECK:     case x
// CHECK:     var stringValue: String { get }
// CHECK:     init?(stringValue: String)
// CHECK:     var intValue: Int? { get }
// CHECK:     init?(intValue: Int)
// CHECK:     @_implements(Equatable, ==(_:_:)) static func __derived_enum_equals(_ a: Struct<T>.CodingKeys, _ b: Struct<T>.CodingKeys) -> Bool
// CHECK:     var hashValue: Int { get }
// CHECK:     func hash(into hasher: inout Hasher)
// CHECK:   }
// CHECK: }
// CHECK-LABEL: extension Struct : Equatable where T : Equatable {
// CHECK:   @_implements(Equatable, ==(_:_:)) static func __derived_struct_equals(_ a: Struct<T>, _ b: Struct<T>) -> Bool
// CHECK: }
// CHECK-LABEL: extension Struct : Hashable where T : Hashable {
// CHECK:   var hashValue: Int { get }
// CHECK:   func hash(into hasher: inout Hasher)
// CHECK: }
// CHECK-LABEL: extension Struct : Decodable & Encodable where T : Decodable, T : Encodable {
// CHECK:   init(from decoder: Decoder) throws
// CHECK:   func encode(to encoder: Encoder) throws
// CHECK: }

extension Struct: Equatable where T: Equatable {}
// CHECK-LABEL: // static Struct<A>.__derived_struct_equals(_:_:)
// CHECK-NEXT: sil hidden @$S30synthesized_conformance_struct6StructVAASQRzlE010__derived_C7_equalsySbACyxG_AEtFZ : $@convention(method) <T where T : Equatable> (@in_guaranteed Struct<T>, @in_guaranteed Struct<T>, @thin Struct<T>.Type) -> Bool {

extension Struct: Hashable where T: Hashable {}
// CHECK-LABEL: // Struct<A>.hashValue.getter
// CHECK-NEXT: sil hidden @$S30synthesized_conformance_struct6StructVAASHRzlE9hashValueSivg : $@convention(method) <T where T : Hashable> (@in_guaranteed Struct<T>) -> Int {

// CHECK-LABEL: // Struct<A>.hash(into:)
// CHECK-NEXT: sil hidden @$S30synthesized_conformance_struct6StructVAASHRzlE4hash4intoys6HasherVz_tF : $@convention(method) <T where T : Hashable> (@inout Hasher, @in_guaranteed Struct<T>) -> () {

extension Struct: Codable where T: Codable {}
// CHECK-LABEL: // Struct<A>.init(from:)
// CHECK-NEXT: sil hidden @$S30synthesized_conformance_struct6StructVAASeRzSERzlE4fromACyxGs7Decoder_p_tKcfC : $@convention(method) <T where T : Decodable, T : Encodable> (@in Decoder, @thin Struct<T>.Type) -> (@out Struct<T>, @error Error)

// CHECK-LABEL: // Struct<A>.encode(to:)
// CHECK-NEXT: sil hidden @$S30synthesized_conformance_struct6StructVAASeRzSERzlE6encode2toys7Encoder_p_tKF : $@convention(method) <T where T : Decodable, T : Encodable> (@in_guaranteed Encoder, @in_guaranteed Struct<T>) -> @error Error {


// Witness tables

// CHECK-LABEL: sil_witness_table hidden <T where T : Equatable> Struct<T>: Equatable module synthesized_conformance_struct {
// CHECK-NEXT:   method #Equatable."=="!1: <Self where Self : Equatable> (Self.Type) -> (Self, Self) -> Bool : @$S30synthesized_conformance_struct6StructVyxGSQAASQRzlSQ2eeoiySbx_xtFZTW	// protocol witness for static Equatable.== infix(_:_:) in conformance <A> Struct<A>
// CHECK-NEXT:   conditional_conformance (T: Equatable): dependent
// CHECK-NEXT: }

// CHECK-LABEL: sil_witness_table hidden <T where T : Hashable> Struct<T>: Hashable module synthesized_conformance_struct {
// CHECK-NEXT:   base_protocol Equatable: <T where T : Equatable> Struct<T>: Equatable module synthesized_conformance_struct
// CHECK-NEXT:   method #Hashable.hashValue!getter.1: <Self where Self : Hashable> (Self) -> () -> Int : @$S30synthesized_conformance_struct6StructVyxGSHAASHRzlSH9hashValueSivgTW	// protocol witness for Hashable.hashValue.getter in conformance <A> Struct<A>
// CHECK-NEXT:   method #Hashable.hash!1: <Self where Self : Hashable> (Self) -> (inout Hasher) -> () : @$S30synthesized_conformance_struct6StructVyxGSHAASHRzlSH4hash4intoys6HasherVz_tFTW	// protocol witness for Hashable.hash(into:) in conformance <A> Struct<A>
// CHECK-NEXT:   conditional_conformance (T: Hashable): dependent
// CHECK-NEXT: }

// CHECK-LABEL: sil_witness_table hidden <T where T : Decodable, T : Encodable> Struct<T>: Decodable module synthesized_conformance_struct {
// CHECK-NEXT:   method #Decodable.init!allocator.1: <Self where Self : Decodable> (Self.Type) -> (Decoder) throws -> Self : @$S30synthesized_conformance_struct6StructVyxGSeAASeRzSERzlSe4fromxs7Decoder_p_tKcfCTW	// protocol witness for Decodable.init(from:) in conformance <A> Struct<A>
// CHECK-NEXT:   conditional_conformance (T: Decodable): dependent
// CHECK-NEXT:   conditional_conformance (T: Encodable): dependent
// CHECK-NEXT: }

// CHECK-LABEL: sil_witness_table hidden <T where T : Decodable, T : Encodable> Struct<T>: Encodable module synthesized_conformance_struct {
// CHECK-NEXT:   method #Encodable.encode!1: <Self where Self : Encodable> (Self) -> (Encoder) throws -> () : @$S30synthesized_conformance_struct6StructVyxGSEAASeRzSERzlSE6encode2toys7Encoder_p_tKFTW	// protocol witness for Encodable.encode(to:) in conformance <A> Struct<A>
// CHECK-NEXT:   conditional_conformance (T: Decodable): dependent
// CHECK-NEXT:   conditional_conformance (T: Encodable): dependent
// CHECK-NEXT: }
