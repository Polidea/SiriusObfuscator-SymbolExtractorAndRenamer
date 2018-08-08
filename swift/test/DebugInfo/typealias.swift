// RUN: %target-swift-frontend %s -emit-ir -g -o - | %FileCheck %s

func markUsed<T>(_ t: T) {}

class DWARF {
// CHECK-DAG: ![[BASE:.*]] = !DICompositeType({{.*}}identifier: "$Ss6UInt32VD"
// CHECK-DAG: ![[DIEOFFSET:.*]] = !DIDerivedType(tag: DW_TAG_typedef, name: "$S9typealias5DWARFC9DIEOffsetaD",{{.*}} line: [[@LINE+1]], baseType: ![[BASE]])
  typealias DIEOffset = UInt32
  // CHECK-DAG: ![[VOID:.*]] = !DICompositeType({{.*}}identifier: "$SytD"
  // CHECK-DAG: ![[PRIVATETYPE:.*]] = !DIDerivedType(tag: DW_TAG_typedef, name: "$S9typealias5DWARFC11PrivateType{{.+}}aD",{{.*}} line: [[@LINE+1]], baseType: ![[VOID]])
  fileprivate typealias PrivateType = ()
  fileprivate static func usePrivateType() -> PrivateType { return () }
}

func main () {
  // CHECK-DAG: !DILocalVariable(name: "a",{{.*}} type: ![[DIEOFFSET]]
  let a : DWARF.DIEOffset = 123
  markUsed(a)
  // CHECK-DAG: !DILocalVariable(name: "b",{{.*}} type: ![[DIEOFFSET]]
  let b = DWARF.DIEOffset(456) as DWARF.DIEOffset
  markUsed(b)

  // CHECK-DAG: !DILocalVariable(name: "c",{{.*}} type: ![[PRIVATETYPE]]
  let c = DWARF.usePrivateType()
  markUsed(c);
}

main();
