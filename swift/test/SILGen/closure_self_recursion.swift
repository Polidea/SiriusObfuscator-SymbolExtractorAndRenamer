// RUN: %target-swift-frontend -module-name foo -enable-sil-ownership -emit-silgen %s | %FileCheck %s
// RUN: %target-swift-frontend -module-name foo -enable-sil-ownership -emit-sil -verify %s

// CHECK-LABEL: sil @main

// CHECK-LABEL: sil private @$S3foo5recuryycvgyycfU_
var recur : () -> () {
  // CHECK-LABEL: function_ref @$S3foo5recuryycvg
  return { recur() } // expected-warning {{attempting to access 'recur' within its own getter}}
}

// CHECK-LABEL: sil private @$S3foo12recur_harderyyycyyXEcvgyycyyXEcfU_
var recur_harder : (() -> ()) -> (() -> ()) {
  // CHECK-LABEL: function_ref @$S3foo12recur_harderyyycyyXEcvg
  return { f in recur_harder(f) } // expected-warning {{attempting to access 'recur_harder' within its own getter}}
}
