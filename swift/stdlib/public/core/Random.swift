//===--- Random.swift -----------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SwiftShims

/// A type that provides uniformly distributed random data.
///
/// When you call methods that use random data, such as creating new random
/// values or shuffling a collection, you can pass a `RandomNumberGenerator`
/// type to be used as the source for randomness. When you don't pass a
/// generator, the default `Random` type is used.
///
/// When providing new APIs that use randomness, provide a version that accepts
/// a generator conforming to the `RandomNumberGenerator` protocol as well as a
/// version that uses the default system generator. For example, this `Weekday`
/// enumeration provides static methods that return a random day of the week:
///
///     enum Weekday: CaseIterable {
///         case sunday, monday, tuesday, wednesday, thursday, friday, saturday
///
///         static func random<G: RandomNumberGenerator>(using generator: inout G) -> Weekday {
///             return Weekday.allCases.randomElement(using: &generator)!
///         }
///
///         static func random() -> Weekday {
///             var g = SystemRandomGenerator()
///             return Weekday.random(using: &g)
///         }
///     }
///
/// Conforming to the RandomNumberGenerator Protocol
/// ================================================
///
/// A custom `RandomNumberGenerator` type can have different characteristics
/// than the default `Random` type. For example, a seedable generator can be
/// used to generate the same sequence of random values for testing purposes.
///
/// To make a custom type conform to the `RandomNumberGenerator` protocol,
/// implement the required `next()` method. Each call to `next()` must produce
/// a uniform and independent random value.
///
/// Types that conform to `RandomNumberGenerator` should specifically document
/// the thread safety and quality of the generator.
public protocol RandomNumberGenerator {
  /// Returns a value from a uniform, independent distribution of binary data.
  ///
  /// - Returns: An unsigned 64-bit random value.
  mutating func next() -> UInt64

  // FIXME: De-underscore after swift-evolution amendment
  mutating func _fill(bytes buffer: UnsafeMutableRawBufferPointer)
}

extension RandomNumberGenerator {
  @inlinable
  public mutating func _fill(bytes buffer: UnsafeMutableRawBufferPointer) {
    // FIXME: Optimize
    var chunk: UInt64 = 0
    var chunkBytes = 0
    for i in 0..<buffer.count {
      if chunkBytes == 0 {
        chunk = next()
        chunkBytes = UInt64.bitWidth / 8
      }
      buffer[i] = UInt8(truncatingIfNeeded: chunk)
      chunk >>= UInt8.bitWidth
      chunkBytes -= 1
    }
  }
}

extension RandomNumberGenerator {
  /// Returns a value from a uniform, independent distribution of binary data.
  ///
  /// - Returns: A random value of `T`. Bits are randomly distributed so that
  ///   every value of `T` is equally likely to be returned.
  @inlinable
  public mutating func next<T: FixedWidthInteger & UnsignedInteger>() -> T {
    return T._random(using: &self)
  }

  /// Returns a random value that is less than the given upper bound.
  ///
  /// - Parameter upperBound: The upper bound for the randomly generated value.
  ///   Must be non-zero.
  /// - Returns: A random value of `T` in the range `0..<upperBound`. Every
  ///   value in the range `0..<upperBound` is equally likely to be returned.
  @inlinable
  public mutating func next<T: FixedWidthInteger & UnsignedInteger>(
    upperBound: T
  ) -> T {
    _precondition(upperBound != 0, "upperBound cannot be zero.")
    let tmp = (T.max % upperBound) + 1
    let range = tmp == upperBound ? 0 : tmp
    var random: T = 0

    repeat {
      random = next()
    } while random < range

    return random % upperBound
  }
}

/// The system's default source of random data.
///
/// When you generate random values, shuffle a collection, or perform another
/// operation that depends on random data, this type is the generator used by
/// default. For example, the two method calls in this example are equivalent:
///
///     let x = Int.random(in: 1...100)
///     var g = SystemRandomNumberGenerator()
///     let y = Int.random(in: 1...100, using: &g)
///
/// `SystemRandomNumberGenerator` is automatically seeded, is safe to use in
/// multiple threads, and uses a cryptographically secure algorithm whenever
/// possible.
///
/// Platform Implementation of `SystemRandomNumberGenerator`
/// ========================================================
///
/// While the system generator is automatically seeded and thread-safe on every
/// platform, the cryptographic quality of the stream of random data produced by
/// the generator may vary. For more detail, see the documentation for the APIs
/// used by each platform.
///
/// - Apple platforms use `arc4random_buf(3)`.
/// - Linux platforms use `getrandom(2)` when available; otherwise, they read
///   from `/dev/urandom`.
@_fixed_layout
public struct SystemRandomNumberGenerator : RandomNumberGenerator {
  @inlinable
  public init() { }

  /// Returns a value from a uniform, independent distribution of binary data.
  ///
  /// - Returns: An unsigned 64-bit random value.
  @inlinable
  public mutating func next() -> UInt64 {
    var random: UInt64 = 0
    _stdlib_random(&random, MemoryLayout<UInt64>.size)
    return random
  }

  @inlinable
  public mutating func _fill(bytes buffer: UnsafeMutableRawBufferPointer) {
    if !buffer.isEmpty {
      _stdlib_random(buffer.baseAddress!, buffer.count)
    }
  }
}
