/*
 This source file is part of the Swift.org open source project

 Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
 Licensed under Apache License v2.0 with Runtime Library Exception

 See http://swift.org/LICENSE.txt for license information
 See http://swift.org/CONTRIBUTORS.txt for Swift project authors
*/

import XCTest

import Basic
@testable import PackageGraph
import PackageDescription
import PackageDescription4
import PackageModel
import TestSupport
import enum PackageLoading.ModuleError

class PackageGraphTests: XCTestCase {

    func testBasic() throws {
        let fs = InMemoryFileSystem(emptyFiles:
            "/Foo/Sources/Foo/source.swift",
            "/Foo/Sources/FooDep/source.swift",
            "/Foo/Tests/FooTests/source.swift",
            "/Bar/source.swift",
            "/Baz/source.swift",
            "/Baz/Tests/BazTests/source.swift"
        )

        let diagnostics = DiagnosticsEngine()
        let g = loadMockPackageGraph([
            "/Foo": Package(name: "Foo", targets: [Target(name: "Foo", dependencies: ["FooDep"])]),
            "/Bar": Package(name: "Bar", dependencies: [.Package(url: "/Foo", majorVersion: 1)]),
            "/Baz": Package(name: "Baz", dependencies: [.Package(url: "/Bar", majorVersion: 1)]),
        ], root: "/Baz", diagnostics: diagnostics, in: fs)

        PackageGraphTester(g) { result in
            result.check(packages: "Bar", "Foo", "Baz")
            result.check(targets: "Bar", "Foo", "Baz", "FooDep")
            result.check(testModules: "BazTests")
            result.check(dependencies: "FooDep", target: "Foo")
            result.check(dependencies: "Foo", target: "Bar")
            result.check(dependencies: "Bar", target: "Baz")
        }
    }

    func testProductDependencies() throws {
        typealias Package = PackageDescription4.Package

        let fs = InMemoryFileSystem(emptyFiles:
            "/Foo/Sources/Foo/source.swift",
            "/Bar/Source/Bar/source.swift",
            "/Bar/Source/CBar/module.modulemap"
        )

        let diagnostics = DiagnosticsEngine()
        let g = loadMockPackageGraph4([
            "/Bar": Package(
                name: "Bar",
                products: [
                    .library(name: "Bar", targets: ["Bar"]),
                    .library(name: "CBar", targets: ["CBar"]),
                ],
                targets: [
                    .target(name: "Bar", dependencies: ["CBar"]),
                    .systemLibrary(name: "CBar"),
                ]),
            "/Foo": .init(
                name: "Foo",
                dependencies: [
                    .package(url: "/Bar", from: "1.0.0"),
                ],
                targets: [
                    .target(name: "Foo", dependencies: ["Bar", "CBar"]),
                ]),
        ], root: "/Foo", diagnostics: diagnostics, in: fs)

        XCTAssertNoDiagnostics(diagnostics)
        PackageGraphTester(g) { result in
            result.check(packages: "Bar", "Foo")
            result.check(targets: "Bar", "CBar", "Foo")
            result.check(dependencies: "Bar", "CBar", target: "Foo")
            result.check(dependencies: "CBar", target: "Bar")
        }
    }

    func testCycle() throws {
        let fs = InMemoryFileSystem(emptyFiles:
            "/Foo/source.swift",
            "/Bar/source.swift",
            "/Baz/source.swift"
        )

        let diagnostics = DiagnosticsEngine()
        _ = loadMockPackageGraph([
            "/Foo": Package(name: "Foo", dependencies: [.Package(url: "/Bar", majorVersion: 1)]),
            "/Bar": Package(name: "Bar", dependencies: [.Package(url: "/Baz", majorVersion: 1)]),
            "/Baz": Package(name: "Baz", dependencies: [.Package(url: "/Bar", majorVersion: 1)]),
        ], root: "/Foo", diagnostics: diagnostics, in: fs)

        XCTAssertEqual(diagnostics.diagnostics[0].localizedDescription, "cyclic dependency declaration found: Foo -> Bar -> Baz -> Bar")
    }

    // Make sure there is no error when we reference Test targets in a package and then
    // use it as a dependency to another package. SR-2353
    func testTestTargetDeclInExternalPackage() throws {
        let fs = InMemoryFileSystem(emptyFiles:
            "/Foo/source.swift",
            "/Foo/Tests/SomeTests/source.swift",
            "/Bar/source.swift",
            "/Bar/Tests/BarTests/source.swift"
        )

        let g = loadMockPackageGraph([
            "/Foo": Package(name: "Foo", targets: [Target(name: "SomeTests", dependencies: ["Foo"])]),
            "/Bar": Package(name: "Bar", dependencies: [.Package(url: "/Foo", majorVersion: 1)]),
        ], root: "/Bar", in: fs)

        PackageGraphTester(g) { result in
            result.check(packages: "Bar", "Foo")
            result.check(targets: "Bar", "Foo")
            result.check(testModules: "BarTests")
        }
    }

    func testDuplicateModules() throws {
        let fs = InMemoryFileSystem(emptyFiles:
            "/Foo/Sources/Bar/source.swift",
            "/Bar/source.swift"
        )

        let diagnostics = DiagnosticsEngine()
        _ = loadMockPackageGraph([
            "/Foo": Package(name: "Foo"),
            "/Bar": Package(name: "Bar", dependencies: [.Package(url: "/Foo", majorVersion: 1)]),
        ], root: "/Bar", diagnostics: diagnostics, in: fs)

        XCTAssertEqual(diagnostics.diagnostics[0].localizedDescription, "multiple targets named 'Bar' in: Bar, Foo")
    }

    func testMultipleDuplicateModules() throws {
        let fs = InMemoryFileSystem(emptyFiles:
            "/Fourth/Sources/First/source.swift",
            "/Third/Sources/First/source.swift",
            "/Second/Sources/First/source.swift",
            "/First/Sources/First/source.swift"
        )

        let diagnostics = DiagnosticsEngine()
        _ = loadMockPackageGraph4([
            "/Fourth": Package(
                name: "Fourth",
                products: [
                    .library(name: "Fourth", targets: ["First"]),
                ],
                targets: [
                    .target(name: "First"),
                ]),
            "/Third": Package(
                name: "Third",
                products: [
                    .library(name: "Third", targets: ["First"]),
                ],
                targets: [
                    .target(name: "First"),
                ]),
            "/Second": Package(
                name: "Second",
                products: [
                    .library(name: "Second", targets: ["First"]),
                ],
                targets: [
                    .target(name: "First"),
                ]),
            "/First": Package(
                name: "First",
                products: [
                    .library(name: "First", targets: ["First"]),
                ],
                dependencies: [
                    .package(url: "/Second",  from: "1.0.0"),
                    .package(url: "/Third",  from: "1.0.0"),
                    .package(url: "/Fourth",  from: "1.0.0"),
                ],
                targets: [
                    .target(name: "First", dependencies: ["Second", "Third", "Fourth"])
                ]),
            ], root: "/First", diagnostics: diagnostics, in: fs)


        DiagnosticsEngineTester(diagnostics) { result in
            result.check(diagnostic: "multiple targets named 'First' in: First, Fourth, Second, Third", behavior: .error)
        }
    }

    func testSeveralDuplicateModules() throws {
        let fs = InMemoryFileSystem(emptyFiles:
            "/Fourth/Sources/Bar/source.swift",
            "/Third/Sources/Bar/source.swift",
            "/Second/Sources/Foo/source.swift",
            "/First/Sources/Foo/source.swift"
        )

        let diagnostics = DiagnosticsEngine()
        _ = loadMockPackageGraph4([
            "/Fourth": Package(
                name: "Fourth",
                products: [
                    .library(name: "Fourth", targets: ["Bar"]),
                ],
                targets: [
                    .target(name: "Bar"),
                ]),
            "/Third": Package(
                name: "Third",
                products: [
                    .library(name: "Third", targets: ["Bar"]),
                ],
                targets: [
                    .target(name: "Bar"),
                ]),
            "/Second": Package(
                name: "Second",
                products: [
                    .library(name: "Second", targets: ["Foo"]),
                ],
                targets: [
                    .target(name: "Foo"),
                ]),
            "/First": Package(
                name: "First",
                products: [
                    .library(name: "First", targets: ["Foo"]),
                ],
                dependencies: [
                    .package(url: "/Second",  from: "1.0.0"),
                    .package(url: "/Third",  from: "1.0.0"),
                    .package(url: "/Fourth",  from: "1.0.0"),
                ],
                targets: [
                    .target(name: "Foo", dependencies: ["Second", "Third", "Fourth"])
                ]),
            ], root: "/First", diagnostics: diagnostics, in: fs)


        DiagnosticsEngineTester(diagnostics) { result in
            result.check(diagnostic: "multiple targets named 'Bar' in: Fourth, Third", behavior: .error)
            result.check(diagnostic: "multiple targets named 'Foo' in: First, Second", behavior: .error)
        }
    }

    func testNestedDuplicateModules() throws {
        let fs = InMemoryFileSystem(emptyFiles:
            "/Fourth/Sources/First/source.swift",
            "/Third/Sources/Third/source.swift",
            "/Second/Sources/Second/source.swift",
            "/First/Sources/First/source.swift"
        )

        let diagnostics = DiagnosticsEngine()
        _ = loadMockPackageGraph4([
            "/Fourth": Package(
                name: "Fourth",
                products: [
                    .library(name: "Fourth", targets: ["First"]),
                ],
                targets: [
                    .target(name: "First"),
                ]),
            "/Third": Package(
                name: "Third",
                products: [
                    .library(name: "Third", targets: ["Third"]),
                ],
                dependencies: [
                    .package(url: "/Fourth", from: "1.0.0"),
                ],
                targets: [
                    .target(name: "Third", dependencies: ["Fourth"]),
                ]),
            "/Second": Package(
                name: "Second",
                products: [
                    .library(name: "Second", targets: ["Second"]),
                ],
                dependencies: [
                    .package(url: "/Third",  from: "1.0.0"),
                ],
                targets: [
                    .target(name: "Second", dependencies: ["Third"]),
                ]),
            "/First": Package(
                name: "First",
                products: [
                    .library(name: "First", targets: ["First"]),
                ],
                dependencies: [
                    .package(url: "/Second",  from: "1.0.0"),
                ],
                targets: [
                    .target(name: "First", dependencies: ["Second"]),
                ]),
            ], root: "/First", diagnostics: diagnostics, in: fs)


        DiagnosticsEngineTester(diagnostics) { result in
            result.check(diagnostic: "multiple targets named 'First' in: First, Fourth", behavior: .error)
        }
    }

    func testEmptyDependency() throws {
        let fs = InMemoryFileSystem(emptyFiles:
            "/Foo/Sources/Foo/foo.swift",
            "/Bar/Sources/Bar/source.txt"
        )

        let diagnostics = DiagnosticsEngine()
        _ = loadMockPackageGraph4([
            "/Bar": Package(
                name: "Bar",
                products: [
                    .library(name: "Bar", targets: ["Bar"]),
                ],
                targets: [
                    .target(name: "Bar"),
                ]),
            "/Foo": .init(
                name: "Foo",
                dependencies: [
                    .package(url: "/Bar", from: "1.0.0"),
                ],
                targets: [
                    .target(name: "Foo", dependencies: ["Bar"]),
                ]),
            ], root: "/Foo", diagnostics: diagnostics, in: fs)

        DiagnosticsEngineTester(diagnostics) { result in
            result.check(diagnostic: "target 'Bar' in package 'Bar' contains no valid source files", behavior: .warning)
            result.check(diagnostic: "target 'Bar' referenced in product 'Bar' could not be found", behavior: .error, location: "'Bar' /Bar")
            result.check(diagnostic: "product dependency 'Bar' not found", behavior: .error, location: "'Foo' /Foo")

        }
    }

    func testUnusedDependency() throws {
        let fs = InMemoryFileSystem(emptyFiles:
            "/Foo/Sources/Foo/foo.swift",
            "/Bar/Sources/Bar/bar.swift",
            "/Baz/Sources/Baz/baz.swift",
            "/Biz/Sources/Biz/main.swift"
        )

        let diagnostics = DiagnosticsEngine()
        _ = loadMockPackageGraph4([
            "/Bar": Package(
                name: "Bar",
                products: [
                    .library(name: "BarLibrary", targets: ["Bar"]),
                ],
                targets: [
                    .target(name: "Bar"),
                ]),
            "/Baz": Package(
                name: "Baz",
                products: [
                    .library(name: "BazLibrary", targets: ["Baz"]),
                ],
                targets: [
                    .target(name: "Baz"),
                ]),
            "/Biz": Package(
                name: "Biz",
                products: [
                    .executable(name: "biz", targets: ["Biz"]),
                ],
                targets: [
                    .target(name: "Biz"),
                ]),
            "/Foo": .init(
                name: "Foo",
                dependencies: [
                    .package(url: "/Bar", from: "1.0.0"),
                    .package(url: "/Baz", from: "1.0.0"),
                    .package(url: "/Biz", from: "1.0.0"),
                ],
                targets: [
                    .target(name: "Foo", dependencies: ["BarLibrary"]),
                ]),
            ], root: "/Foo", diagnostics: diagnostics, in: fs)

        DiagnosticsEngineTester(diagnostics) { result in
            result.check(diagnostic: "dependency 'Baz' is not used by any target", behavior: .warning)
        }
    }

    func testUnusedDependency2() throws {
        typealias Package = PackageDescription4.Package
        let fs = InMemoryFileSystem(emptyFiles:
            "/Foo/module.modulemap",
            "/Bar/Sources/Bar/main.swift"
        )

        let diagnostics = DiagnosticsEngine()
        _ = loadMockPackageGraph4([
            "/Foo": Package(name: "Foo"),
            "/Bar": Package(
                name: "Bar",
                dependencies: [
                    .package(url: "/Foo", from: "1.0.0"),
                    ],
                targets: [
                    .target(name: "Bar"),
                    ]),
            ], root: "/Bar", diagnostics: diagnostics, in: fs)

        // We don't expect any unused dependency diagnostics from a system module package.
        DiagnosticsEngineTester(diagnostics) { _ in }
    }

    func testDuplicateInterPackageTargetNames() throws {
        let fs = InMemoryFileSystem(emptyFiles:
            "/Start/Sources/Foo/foo.swift",
            "/Start/Sources/Bar/bar.swift",
            "/Dep1/Sources/Baz/baz.swift",
            "/Dep2/Sources/Foo/foo.swift",
            "/Dep2/Sources/Bam/bam.swift"
        )

        let diagnostics = DiagnosticsEngine()
        _ = loadMockPackageGraph4([
            "/Start": Package(
                name: "Start",
                products: [
                    .library(name: "FooLibrary", targets: ["Foo"]),
                    .library(name: "BarLibrary", targets: ["Bar"]),
                ],
                dependencies: [
                    .package(url: "/Dep1", from: "1.0.0"),
                ],
                targets: [
                    .target(name: "Foo", dependencies: ["BazLibrary"]),
                    .target(name: "Bar"),
                ]),
            "/Dep1": Package(
                name: "Dep1",
                products: [
                    .library(name: "BazLibrary", targets: ["Baz"]),
                ],
                dependencies: [
                    .package(url: "/Dep2", from: "1.0.0"),
                ],
                targets: [
                    .target(name: "Baz", dependencies: ["FooLibrary"]),
                ]),
            "/Dep2": Package(
                name: "Dep2",
                products: [
                    .library(name: "FooLibrary", targets: ["Foo"]),
                    .library(name: "BamLibrary", targets: ["Bam"]),
                ],
                targets: [
                    .target(name: "Foo"),
                    .target(name: "Bam"),
                ]),
            ], root: "/Start", diagnostics: diagnostics, in: fs)

        DiagnosticsEngineTester(diagnostics) { result in
            result.check(diagnostic: "multiple targets named 'Foo' in: Dep2, Start", behavior: .error)
        }
    }

    static var allTests = [
        ("testBasic", testBasic),
        ("testDuplicateModules", testDuplicateModules),
        ("testCycle", testCycle),
        ("testProductDependencies", testProductDependencies),
        ("testTestTargetDeclInExternalPackage", testTestTargetDeclInExternalPackage),
        ("testEmptyDependency", testEmptyDependency),
        ("testUnusedDependency", testUnusedDependency),
        ("testUnusedDependency2", testUnusedDependency2),
        ("testDuplicateInterPackageTargetNames", testDuplicateInterPackageTargetNames),
    ]
}
