// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -assume-parsing-unqualified-ownership-sil -emit-module -o %t %S/sil_witness_tables_external_conformance.swift
// RUN: %target-swift-frontend -assume-parsing-unqualified-ownership-sil -I %t -primary-file %s -emit-ir | %FileCheck %s

// REQUIRES: CPU=x86_64

import sil_witness_tables_external_conformance

// FIXME: This should be a SIL test, but we can't parse sil_witness_tables
// yet.

protocol A {}

protocol P {
  associatedtype Assoc: A

  static func staticMethod()
  func instanceMethod()
}

protocol Q : P {
  func qMethod()
}

protocol QQ {
  func qMethod()
}

struct AssocConformer: A {}

struct Conformer: Q, QQ {
  typealias Assoc = AssocConformer

  static func staticMethod() {}
  func instanceMethod() {}
  func qMethod() {}
}

// CHECK: [[EXTERNAL_CONFORMER_EXTERNAL_P_WITNESS_TABLE:@"\$S39sil_witness_tables_external_conformance17ExternalConformerVAA0F1PAAWP"]] = external global i8*, align 8
// CHECK: [[CONFORMER_Q_WITNESS_TABLE:@"\$S18sil_witness_tables9ConformerVAA1QAAWP"]] = hidden constant [3 x i8*] [
// CHECK:   i8* bitcast ([5 x i8*]* [[CONFORMER_P_WITNESS_TABLE:@"\$S18sil_witness_tables9ConformerVAA1PAAWP"]] to i8*),
// CHECK:   i8* bitcast (void (%T18sil_witness_tables9ConformerV*, %swift.type*, i8**)* @"$S18sil_witness_tables9ConformerVAA1QA2aDP7qMethod{{[_0-9a-zA-Z]*}}FTW" to i8*)
// CHECK: ]
// CHECK: [[CONFORMER_P_WITNESS_TABLE]] = hidden constant [5 x i8*] [
// CHECK:   i8* bitcast (%swift.metadata_response (i64)* @"$S18sil_witness_tables14AssocConformerVMa" to i8*),
// CHECK:   i8* bitcast (i8** ()* @"$S18sil_witness_tables14AssocConformerVAA1AAAWa" to i8*)
// CHECK:   i8* bitcast (void (%swift.type*, %swift.type*, i8**)* @"$S18sil_witness_tables9ConformerVAA1PA2aDP12staticMethod{{[_0-9a-zA-Z]*}}FZTW" to i8*),
// CHECK:   i8* bitcast (void (%T18sil_witness_tables9ConformerV*, %swift.type*, i8**)* @"$S18sil_witness_tables9ConformerVAA1PA2aDP14instanceMethod{{[_0-9a-zA-Z]*}}FTW" to i8*)
// CHECK: ]
// CHECK: [[CONFORMER2_P_WITNESS_TABLE:@"\$S18sil_witness_tables10Conformer2VAA1PAAWP"]] = hidden constant [5 x i8*]

struct Conformer2: Q {
  typealias Assoc = AssocConformer

  static func staticMethod() {}
  func instanceMethod() {}
  func qMethod() {}
}

// CHECK-LABEL: define hidden swiftcc void @"$S18sil_witness_tables7erasure1cAA2QQ_pAA9ConformerV_tF"(%T18sil_witness_tables2QQP* noalias nocapture sret)
// CHECK:         [[WITNESS_TABLE_ADDR:%.*]] = getelementptr inbounds %T18sil_witness_tables2QQP, %T18sil_witness_tables2QQP* %0, i32 0, i32 2
// CHECK-NEXT:    store i8** getelementptr inbounds ([2 x i8*], [2 x i8*]* [[CONFORMER_QQ_WITNESS_TABLE:@"\$S.*WP"]], i32 0, i32 0), i8*** [[WITNESS_TABLE_ADDR]], align 8
func erasure(c: Conformer) -> QQ {
  return c
}

// CHECK-LABEL: define hidden swiftcc void @"$S18sil_witness_tables15externalErasure1c0a1_b1_c1_D12_conformance9ExternalP_pAD0G9ConformerV_tF"(%T39sil_witness_tables_external_conformance9ExternalPP* noalias nocapture sret)
// CHECK:         [[WITNESS_TABLE_ADDR:%.*]] = getelementptr inbounds %T39sil_witness_tables_external_conformance9ExternalPP, %T39sil_witness_tables_external_conformance9ExternalPP* %0, i32 0, i32 2
// CHECK-NEXT:    store i8** [[EXTERNAL_CONFORMER_EXTERNAL_P_WITNESS_TABLE]], i8*** %2, align 8
func externalErasure(c: ExternalConformer) -> ExternalP {
  return c
}

// FIXME: why do these have different linkages?

// CHECK-LABEL: define hidden swiftcc %swift.metadata_response @"$S18sil_witness_tables14AssocConformerVMa"(i64)
// CHECK:         ret %swift.metadata_response { %swift.type* bitcast (i64* getelementptr inbounds {{.*}} @"$S18sil_witness_tables14AssocConformerVMf", i32 0, i32 1) to %swift.type*), i64 0 }

// CHECK-LABEL: define hidden i8** @"$S18sil_witness_tables9ConformerVAA1PAAWa"()
// CHECK:         ret i8** getelementptr inbounds ([5 x i8*], [5 x i8*]* @"$S18sil_witness_tables9ConformerVAA1PAAWP", i32 0, i32 0)
