// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -assume-parsing-unqualified-ownership-sil -emit-module %S/Inputs/sil_witness_tables_external_input.swift -o %t/Swift.swiftmodule -parse-stdlib -parse-as-library -module-name Swift -module-link-name swiftCore
// RUN: %target-swift-frontend -assume-parsing-unqualified-ownership-sil -I %t -primary-file %s -emit-ir | %FileCheck %s

import Swift

// CHECK: @"$Ss1XVN" = external global %swift.type
// CHECK: @"$Ss1XVs1PsWP" = external global i8*

func doSomething<T : P>(_ t : T) -> Y {
  return t.doSomething()
}

func done() -> Y {
  let x = X()
  return doSomething(x)
}
