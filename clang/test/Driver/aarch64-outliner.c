// REQUIRES: aarch64-registered-target

// RUN: %clang -target aarch64 -moutline -S %s -### 2>&1 | FileCheck %s
// CHECK: "-mllvm" "-enable-machine-outliner"
