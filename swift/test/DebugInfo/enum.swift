// RUN: %target-swift-frontend -primary-file %s -emit-ir -g -o - | %FileCheck %s
// RUN: %target-swift-frontend -primary-file %s -emit-ir -gdwarf-types -o - | %FileCheck %s --check-prefix=DWARF

// UNSUPPORTED: OS=watchos

protocol P {}

enum Either {
  case First(Int64), Second(P), Neither
// CHECK: !DICompositeType({{.*}}name: "Either",
// CHECK-SAME:             line: [[@LINE-3]],
// CHECK-SAME:             size: {{328|168}},
}
// CHECK: ![[EMPTY:.*]] = !{}
// DWARF: ![[INT:.*]] = !DICompositeType({{.*}}identifier: "$SSiD"
let E : Either = .Neither;

// CHECK: !DICompositeType({{.*}}name: "Color",
// CHECK-SAME:             line: [[@LINE+3]]
// CHECK-SAME:             size: 8,
// CHECK-SAME:             identifier: "$S4enum5ColorOD"
enum Color : UInt64 {
// This is effectively a 2-bit bitfield:
// DWARF: !DIDerivedType(tag: DW_TAG_member, name: "Red"
// DWARF-SAME:           baseType: ![[UINT64:[0-9]+]]
// DWARF-SAME:           size: 8{{[,)]}}
// DWARF: ![[UINT64]] = !DICompositeType({{.*}}identifier: "$Ss6UInt64VD"
  case Red, Green, Blue
}

// CHECK: !DICompositeType({{.*}}name: "MaybeIntPair",
// CHECK-SAME:             line: [[@LINE+3]],
// CHECK-SAME:             size: 136{{[,)]}}
// CHECK-SAME:             identifier: "$S4enum12MaybeIntPairOD"
enum MaybeIntPair {
// DWARF: !DIDerivedType(tag: DW_TAG_member, name: "none"
// DWARF-SAME:           baseType: ![[INT]]{{[,)]}}
  case none
// DWARF: !DIDerivedType(tag: DW_TAG_member, name: "just"
// DWARF-SAME:           baseType: ![[INTTUP:[0-9]+]]
// DWARF-SAME:           size: 128{{[,)]}}
// DWARF: ![[INTTUP]] = !DICompositeType({{.*}}identifier: "$Ss5Int64V_ABtD"
  case just(Int64, Int64)
}

enum Maybe<T> {
  case none
  case just(T)
}

let r = Color.Red
let c = MaybeIntPair.just(74, 75)
// CHECK: !DICompositeType({{.*}}name: "Maybe",
// CHECK-SAME:             line: [[@LINE-8]],
// CHECK-SAME:             size: 8{{[,)]}}
// CHECK-SAME:             identifier: "$S4enum5MaybeOyAA5ColorOGD"
let movie : Maybe<Color> = .none

public enum Nothing { }
public func foo(_ empty : Nothing) { }
// CHECK: !DICompositeType({{.*}}name: "Nothing", {{.*}}elements: ![[EMPTY]]

// CHECK: !DICompositeType({{.*}}name: "Rose", {{.*}}elements: ![[ELTS:[0-9]+]],
// CHECK-SAME:             {{.*}}identifier: "$S4enum4RoseOyxG{{z?}}D")
enum Rose<A> {
	case MkRose(() -> A, () -> [Rose<A>])
  // DWARF: !DICompositeType({{.*}}name: "Rose",{{.*}}identifier: "$S4enum4RoseOyAA3fooyACyxGAElFQq_GD")
	case IORose(() -> Rose<A>)
}

func foo<T>(_ x : Rose<T>) -> Rose<T> { return x }

// CHECK: !DICompositeType({{.*}}name: "Tuple", {{.*}}elements: ![[ELTS:[0-9]+]], {{.*}}identifier: "$S4enum5TupleOyAA3baryACyxGAElFQq_GD")
// DWARF: !DICompositeType({{.*}}name: "Tuple", {{.*}}elements: ![[ELTS:[0-9]+]],
// DWARF-SAME:             {{.*}}identifier: "$S4enum5TupleOyxG{{z?}}D")
public enum Tuple<P> {
  // DWARF: !DICompositeType({{.*}}name: "Tuple",{{.*}}identifier: "$S4enum5TupleOyAA3baryACyxGAElFQq_GD")
	case C(P, () -> Tuple)
}

func bar<T>(_ x : Tuple<T>) -> Tuple<T> { return x }

// CHECK: !DILocalVariable(name: "self", arg: 1, {{.*}} line: [[@LINE+5]], type: ![[LIST:.*]], flags: DIFlagArtificial)
// CHECK: ![[LIST]] = !DICompositeType({{.*}}identifier: "$S4enum4ListOyACQq_GD"
public enum List<T> {
       indirect case Tail(List, T)
       case End
       func fooMyList() {}
}
