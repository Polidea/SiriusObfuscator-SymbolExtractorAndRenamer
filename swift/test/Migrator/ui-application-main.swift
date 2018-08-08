// RUN: %empty-directory(%t) && %target-swift-frontend -c -update-code -primary-file %s -emit-migrated-file-path %t/ui-application-main.swift.result -emit-remap-file-path %t/ui-application-main.swift.remap -o /dev/null -swift-version 3
// RUN: diff -u %S/ui-application-main.swift.expected %t/ui-application-main.swift.result
// RUN: %empty-directory(%t) && %target-swift-frontend -c -update-code -primary-file %s -emit-migrated-file-path %t/ui-application-main.swift.result -emit-remap-file-path %t/ui-application-main.swift.remap -o /dev/null -swift-version 4
// RUN: diff -u %S/ui-application-main.swift.expected %t/ui-application-main.swift.result

// REQUIRES: OS=ios
// REQUIRES: objc_interop

import UIKit

func foo(pointer: UnsafeMutablePointer<UnsafeMutablePointer<Int8>>) {
	UIApplicationMain(CommandLine.argc, pointer, "", "")
	UIApplicationMain(2, pointer, "", "")
}
