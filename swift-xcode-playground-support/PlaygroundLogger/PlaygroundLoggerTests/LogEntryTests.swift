//===--- LogEntryTests.swift ----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import XCTest
@testable import PlaygroundLogger

import Foundation

#if os(macOS)
    import AppKit
#elseif os(iOS) || os(tvOS)
    import UIKit
#endif

class LogEntryTests: XCTestCase {
    func testNilIUO() throws {
        let nilIUO: Int! = nil

        let logEntry = try LogEntry(describing: nilIUO as Any, name: "nilIUO", policy: .default)

        guard case let .structured(name, _, _, totalChildrenCount, children, _) = logEntry else {
            XCTFail("Expected a structured log entry")
            return
        }

        XCTAssertEqual(name, "nilIUO")
        XCTAssertEqual(totalChildrenCount, 0)
        XCTAssert(children.isEmpty)
    }

    func testEmptyView() throws {
        #if os(macOS)
            let emptyView = NSView()
        #elseif os(iOS) || os(tvOS)
            let emptyView = UIView()
        #endif

        let logEntry = try LogEntry(describing: emptyView, name: "emptyView", policy: .default)

        guard case let .opaque(name, _, _, _, representation) = logEntry else {
            XCTFail("Expected an opaque log entry")
            return
        }

        XCTAssertEqual(name, "emptyView")
        XCTAssert(representation is ImageOpaqueRepresentation)

        // Try to encode the log entry. This operation shouldn't throw; if it does, it will fail the test.
        let encoder = LogEncoder()
        try logEntry.encode(with: encoder, format: .current)
    }

    func testEmptyImage() throws {
        #if os(macOS)
            let emptyImage = NSImage()
        #elseif os(iOS) || os(tvOS)
            let emptyImage = UIImage()
        #endif

        let logEntry = try LogEntry(describing: emptyImage, name: "emptyImage", policy: .default)

        guard case let .opaque(name, _, _, _, representation) = logEntry else {
            XCTFail("Expected an opaque log entry")
            return
        }

        XCTAssertEqual(name, "emptyImage")
        XCTAssert(representation is ImageOpaqueRepresentation)

        // Try to encode the log entry. This operation shouldn't throw; if it does, it will fail the test.
        let encoder = LogEncoder()
        try logEntry.encode(with: encoder, format: .current)
    }

    func testLargeSet() throws {
        let set = Set(1...1000)

        let logEntry = try LogEntry(describing: set, name: "set", policy: .default)

        guard case let .structured(name, _, _, totalChildrenCount, children, disposition) = logEntry else {
            XCTFail("Expected a structured log entry")
            return
        }

        XCTAssertEqual(name, "set")

        // We expect `totalChildrenCount` to be 1000 because `set` has 1000 elements.
        XCTAssertEqual(totalChildrenCount, 1000)

        // We expect `children.count` to be 101 due to the default logging policy, which encodes the first 80 and the last 20 children when there's more than 100 children, plus a gap in between to indicate what was elided.
        XCTAssertEqual(children.count, 101)

        for (index, childEntry) in children.enumerated() {
            if index == 80 {
                // We expect the 81st child to be a gap based on the default logging policy for containers.
                guard case .gap = childEntry else {
                    XCTFail("Expected this entry to be a gap entry!")
                    return
                }
            }
            else {
                // We expect all other children to be opaque entries representing the Ints in the set.
                guard case let .opaque(_, _, _, _, representation) = childEntry else {
                    XCTFail("Expected this entry to be an opaque entry!")
                    return
                }

                // We don't know the precise value, due to hashing in the set.
                // But we *do* know that the value should be an Int64, so check that at least.
                XCTAssert(representation is Int64)
            }
        }

        XCTAssertEqual(disposition, .membershipContainer)
    }
}
