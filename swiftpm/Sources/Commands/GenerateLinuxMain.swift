/*
This source file is part of the Swift.org open source project

Copyright 2015 - 2018 Apple Inc. and the Swift project authors
Licensed under Apache License v2.0 with Runtime Library Exception

See http://swift.org/LICENSE.txt for license information
See http://swift.org/CONTRIBUTORS.txt for Swift project authors
*/

import Basic
import PackageGraph
import PackageModel

/// A utility for generating test entries on linux.
///
/// This uses input from macOS's test discovery and generates
/// corelibs-xctest compatible test manifests.
final class LinuxMainGenerator {

    enum Error: Swift.Error {
        case noTestTargets
    }

    /// The package graph we're working on.
    let graph: PackageGraph

    /// The test suites that we need to write.
    let testSuites: [TestSuite]

    init(graph: PackageGraph, testSuites: [TestSuite]) {
        self.graph = graph
        self.testSuites = testSuites
    }

    /// Generate the XCTestManifests.swift and LinuxMain.swift for the package.
    func generate() throws {
        // Create the module struct from input.
        //
        // This converts the input test suite into a structure that
        // is more suitable for generating linux test entries.
        let modulesBuilder = ModulesBuilder()
        for suite in testSuites {
            modulesBuilder.add(suite.tests)
        }
        let modules = modulesBuilder.build()

        // Generate manifest file for each test module we got from XCTest discovery.
        for module in modules.lazy.sorted(by: { $0.name < $1.name }) {
            guard let target = graph.reachableTargets.first(where: { $0.c99name == module.name }) else {
                print("warning: did not file target '\(module.name)'")
                continue
            }
            assert(target.type == .test, "Unexpected target type \(target.type) for \(target)")

            // Write the manifest file for this module.
            let testManifest = target.sources.root.appending(component: "XCTestManifests.swift")
            let stream = try LocalFileOutputByteStream(testManifest)

            stream <<< "import XCTest" <<< "\n"
            for klass in module.classes.lazy.sorted(by: { $0.name < $1.name }) {
                stream <<< "\n"
                stream <<< "extension " <<< klass.name <<< " {" <<< "\n"
                stream <<< indent(4) <<< "static let __allTests = [" <<< "\n"
                for method in klass.methods {
                    stream <<< indent(8) <<< "(\"\(method)\", \(method))," <<< "\n"
                }
                stream <<< indent(4) <<< "]" <<< "\n"
                stream <<< "}" <<< "\n"
            }

            stream <<<
            """

            #if !os(macOS)
            public func __allTests() -> [XCTestCaseEntry] {
                return [

            """

            for klass in module.classes {
                stream <<< indent(8) <<< "testCase(" <<< klass.name <<< ".__allTests)," <<< "\n"
            }

            stream <<< """
                ]
            }
            #endif

            """
            stream.flush()
        }

        /// Write LinuxMain.swift file.
        guard let testTarget = graph.reachableProducts.first(where: { $0.type == .test })?.targets.first else {
            throw Error.noTestTargets
        }
        let linuxMain = testTarget.sources.root.parentDirectory.appending(components: SwiftTarget.linuxMainBasename)

        let stream = try LocalFileOutputByteStream(linuxMain)
        stream <<< "import XCTest" <<< "\n\n"
        for module in modules {
            stream <<< "import " <<< module.name <<< "\n"
        }
        stream <<< "\n"
        stream <<< "var tests = [XCTestCaseEntry]()" <<< "\n"
        for module in modules {
            stream <<< "tests += \(module.name).__allTests()" <<< "\n"
        }
        stream <<< "\n"
        stream <<< "XCTMain(tests)" <<< "\n"
        stream.flush()
    }

    private func indent(_ spaces: Int) -> ByteStreamable {
        return Format.asRepeating(string: " ", count: spaces)
    }
}

// MARK: - Internal data structure for LinuxMainGenerator.

private struct Module {
    struct Class {
        let name: String
        let methods: [String]
    }
    let name: String
    let classes: [Class]
}

private final class ModulesBuilder {

    final class ModuleBuilder {
        let name: String
        var classes: [ClassBuilder]

        init(_ name: String) {
            self.name = name
            self.classes = []
        }

        func build() -> Module {
            return Module(name: name, classes: classes.map({ $0.build() }))
        }
    }

    final class ClassBuilder {
        let name: String
        var methods: [String]

        init(_ name: String) {
            self.name = name
            self.methods = []
        }

        func build() -> Module.Class {
            return .init(name: name, methods: methods)
        }
    }

    /// The built modules.
    private var modules: [ModuleBuilder] = []

    func add(_ cases: [TestSuite.TestCase]) {
        for testCase in cases {
            let (module, theKlass) = testCase.name.split(around: ".")
            guard let klass = theKlass else {
                fatalError("Unsupported test case name \(testCase.name)")
            }
            for method in testCase.tests {
                add(module, klass, method)
            }
        }
    }

    private func add(_ moduleName: String, _ klassName: String, _ methodName: String) {
        // Find or create the module.
        let module: ModuleBuilder
        if let theModule = modules.first(where: { $0.name == moduleName }) {
            module = theModule
        } else {
            module = ModuleBuilder(moduleName)
            modules.append(module)
        }

        // Find or create the class.
        let klass: ClassBuilder
        if let theKlass = module.classes.first(where: { $0.name == klassName }) {
            klass = theKlass
        } else {
            klass = ClassBuilder(klassName)
            module.classes.append(klass)
        }

        // Finally, append the method to the class.
        klass.methods.append(methodName)
    }

    func build() -> [Module] {
        return modules.map({ $0.build() })
    }
}
