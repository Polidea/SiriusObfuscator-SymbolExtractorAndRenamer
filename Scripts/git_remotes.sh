#!/bin/bash

names=( 
    "llvm"
    "clang"
    "swift"
    "lldb"
    "cmark"
    "llbuild"
    "swiftpm"
    "compiler-rt"
    "swift-corelibs-xctest"
    "swift-corelibs-foundation"
    "swift-corelibs-libdispatch"
    "swift-integration-tests"
    "swift-xcode-playground-support"
    "ninja"
)

paths=(
    "apple/swift-llvm"
    "apple/swift-clang"
    "apple/swift"
    "apple/swift-lldb"
    "apple/swift-cmark"
    "apple/swift-llbuild"
    "apple/swift-package-manager"
    "apple/swift-compiler-rt"
    "apple/swift-corelibs-xctest"
    "apple/swift-corelibs-foundation"
    "apple/swift-corelibs-libdispatch"
    "apple/swift-integration-tests"
    "apple/swift-xcode-playground-support"
    "ninja-build/ninja"
)

branches=(
    "swift-4.0-branch"
    "swift-4.0-branch"
    "swift-4.0-branch"
    "swift-4.0-branch"
    "swift-4.0-branch"
    "swift-4.0-branch"
    "swift-4.0-branch"
    "swift-4.0-branch"
    "swift-4.0-branch"
    "swift-4.0-branch"
    "swift-4.0-branch"
    "swift-4.0-branch"
    "swift-4.0-branch"
    "release"
)

for i in ${!names[@]}; do
  echo "DATA: ${names[$i]} -> ${paths[$i]} -> ${branches[$i]}"
  echo "REMOTE: git remote add ${names[$i]} https://github.com/${paths[$i]}.git"
  git remote add ${names[$i]} https://github.com/${paths[$i]}.git
  echo "FETCH: git fetch ${names[$i]}"
  git fetch ${names[$i]}
  echo "READ-TREE: git read-tree --prefix=${names[$i]} -u ${names[$i]}/${branches[$i]}"
  git read-tree --prefix=${names[$i]} -u ${names[$i]}/${branches[$i]}
done
