// Please keep this file in alphabetical order!

// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -I %S/Inputs/custom-modules/ -F %S/Inputs/ -emit-module -o %t %s -disable-objc-attr-requires-foundation-module
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -I %S/Inputs/custom-modules/ -F %S/Inputs/ -parse-as-library %t/imports.swiftmodule -typecheck -emit-objc-header-path %t/imports.h -import-objc-header %S/../Inputs/empty.h -disable-objc-attr-requires-foundation-module
// RUN: %FileCheck %s < %t/imports.h
// RUN: %FileCheck -check-prefix=NEGATIVE %s < %t/imports.h
// RUN: %check-in-clang %t/imports.h -I %S/Inputs/custom-modules/ -F %S/Inputs/

// REQUIRES: objc_interop

// CHECK: @import Base;
// CHECK-NEXT: @import Base.ExplicitSub;
// CHECK-NEXT: @import Base.ExplicitSub.ExSub;
// CHECK-NEXT: @import Base.ImplicitSub.ExSub;
// CHECK-NEXT: @import Foundation;
// CHECK-NEXT: @import MostlyPrivate1;
// CHECK-NEXT: @import MostlyPrivate1_Private;
// CHECK-NEXT: @import MostlyPrivate2_Private;
// CHECK-NEXT: @import ctypes.bits;

// NEGATIVE-NOT: ctypes;
// NEGATIVE-NOT: ImSub;
// NEGATIVE-NOT: ImplicitSub;
// NEGATIVE-NOT: MostlyPrivate2;

import ctypes.bits
import Foundation

import Base
import Base.ImplicitSub
import Base.ImplicitSub.ImSub
import Base.ImplicitSub.ExSub
import Base.ExplicitSub
import Base.ExplicitSub.ImSub
import Base.ExplicitSub.ExSub

import MostlyPrivate1
import MostlyPrivate1_Private
// Deliberately not importing MostlyPrivate2
import MostlyPrivate2_Private

@objc class Test {
  let word: DWORD = 0
  let number: TimeInterval = 0.0

  let baseI: BaseI = 0
  let baseII: BaseII = 0
  let baseIE: BaseIE = 0
  let baseE: BaseE = 0
  let baseEI: BaseEI = 0
  let baseEE: BaseEE = 0

  // Deliberately use the private type before the public type.
  let mp1priv: MP1PrivateType = 0
  let mp1pub: MP1PublicType = 0

  let mp2priv: MP2PrivateType = 0
}
