//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

extension String : StringProtocol, RangeReplaceableCollection {
  /// A type that represents the number of steps between two `String.Index`
  /// values, where one value is reachable from the other.
  ///
  /// In Swift, *reachability* refers to the ability to produce one value from
  /// the other through zero or more applications of `index(after:)`.
  public typealias IndexDistance = Int

  public typealias SubSequence = Substring

  /// Creates a string representing the given character repeated the specified
  /// number of times.
  ///
  /// For example, use this initializer to create a string with ten `"0"`
  /// characters in a row.
  ///
  ///     let zeroes = String(repeating: "0" as Character, count: 10)
  ///     print(zeroes)
  ///     // Prints "0000000000"
  ///
  /// - Parameters:
  ///   - repeatedValue: The character to repeat.
  ///   - count: The number of times to repeat `repeatedValue` in the
  ///     resulting string.
  @inlinable // FIXME(sil-serialize-all)
  public init(repeating repeatedValue: Character, count: Int) {
    self.init(repeating: String(repeatedValue), count: count)
  }

  // This initializer disambiguates between the following intitializers, now
  // that String conforms to Collection:
  // - init<T>(_ value: T) where T : LosslessStringConvertible
  // - init<S>(_ characters: S) where S : Sequence, S.Element == Character

  /// Creates a new string containing the characters in the given sequence.
  ///
  /// You can use this initializer to create a new string from the result of
  /// one or more collection operations on a string's characters. For example:
  ///
  ///     let str = "The rain in Spain stays mainly in the plain."
  ///
  ///     let vowels: Set<Character> = ["a", "e", "i", "o", "u"]
  ///     let disemvoweled = String(str.lazy.filter { !vowels.contains($0) })
  ///
  ///     print(disemvoweled)
  ///     // Prints "Th rn n Spn stys mnly n th pln."
  ///
  /// - Parameter other: A string instance or another sequence of
  ///   characters.
  @inlinable // FIXME(sil-serialize-all)
  public init<S : Sequence & LosslessStringConvertible>(_ other: S)
  where S.Element == Character {
    self = other.description
  }

  // The defaulted argument prevents this initializer from satisfies the
  // LosslessStringConvertible conformance.  You can satisfy a protocol
  // requirement with something that's not yet available, but not with
  // something that has become unavailable. Without this, the code won't
  // compile as Swift 4.
  @inlinable // FIXME(sil-serialize-all)
  @available(swift, obsoleted: 4, message: "String.init(_:String) is no longer failable")
  public init?(_ other: String, obsoletedInSwift4: () = ()) {
    self.init(other._guts)
  }

  /// The position of the first character in a nonempty string.
  ///
  /// In an empty string, `startIndex` is equal to `endIndex`.
  @inlinable // FIXME(sil-serialize-all)
  public var startIndex: Index { return Index(encodedOffset: 0) }

  /// A string's "past the end" position---that is, the position one greater
  /// than the last valid subscript argument.
  ///
  /// In an empty string, `endIndex` is equal to `startIndex`.
  @inlinable // FIXME(sil-serialize-all)
  public var endIndex: Index { return Index(encodedOffset: _guts.count) }

  /// The number of characters in a string.
  public var count: Int {
    return distance(from: startIndex, to: endIndex)
  }

  @inlinable
  @inline(__always)
  internal func _boundsCheck(_ index: Index) {
    _precondition(index.encodedOffset >= 0 && index.encodedOffset < _guts.count,
      "String index is out of bounds")
  }

  @inlinable
  @inline(__always)
  internal func _boundsCheck(_ range: Range<Index>) {
    _precondition(
      range.lowerBound.encodedOffset >= 0 &&
      range.upperBound.encodedOffset <= _guts.count,
      "String index range is out of bounds")
  }

  @inlinable
  @inline(__always)
  internal func _boundsCheck(_ range: ClosedRange<Index>) {
    _precondition(
      range.lowerBound.encodedOffset >= 0 &&
      range.upperBound.encodedOffset < _guts.count,
      "String index range is out of bounds")
  }

  internal func _index(atEncodedOffset offset: Int) -> Index {
    return _visitGuts(_guts, args: offset,
      ascii: { ascii, offset in return ascii.characterIndex(atOffset: offset) },
      utf16: { utf16, offset in return utf16.characterIndex(atOffset: offset) },
      opaque: { opaque, offset in
        return opaque.characterIndex(atOffset: offset) })
  }

  /// Returns the position immediately after the given index.
  ///
  /// - Parameter i: A valid index of the collection. `i` must be less than
  ///   `endIndex`.
  /// - Returns: The index value immediately after `i`.
  public func index(after i: Index) -> Index {
    return _visitGuts(_guts, args: i,
      ascii: { ascii, i in ascii.characterIndex(after: i) },
      utf16: { utf16, i in utf16.characterIndex(after: i) },
      opaque: { opaque, i in opaque.characterIndex(after: i) })
  }

  /// Returns the position immediately before the given index.
  ///
  /// - Parameter i: A valid index of the collection. `i` must be greater than
  ///   `startIndex`.
  /// - Returns: The index value immediately before `i`.
  public func index(before i: Index) -> Index {
    return _visitGuts(_guts, args: i,
      ascii: { ascii, i in ascii.characterIndex(before: i) },
      utf16: { utf16, i in utf16.characterIndex(before: i) },
      opaque: { opaque, i in opaque.characterIndex(before: i) })
  }

  /// Returns an index that is the specified distance from the given index.
  ///
  /// The following example obtains an index advanced four positions from a
  /// string's starting index and then prints the character at that position.
  ///
  ///     let s = "Swift"
  ///     let i = s.index(s.startIndex, offsetBy: 4)
  ///     print(s[i])
  ///     // Prints "t"
  ///
  /// The value passed as `distance` must not offset `i` beyond the bounds of
  /// the collection.
  ///
  /// - Parameters:
  ///   - i: A valid index of the collection.
  ///   - distance: The distance to offset `i`.
  /// - Returns: An index offset by `distance` from the index `i`. If
  ///   `distance` is positive, this is the same value as the result of
  ///   `distance` calls to `index(after:)`. If `distance` is negative, this
  ///   is the same value as the result of `abs(distance)` calls to
  ///   `index(before:)`.
  ///
  /// - Complexity: O(*k*), where *k* is the absolute value of `distance`.
  public func index(_ i: Index, offsetBy distance: IndexDistance) -> Index {
    return _visitGuts(_guts, args: (i, distance),
      ascii: { ascii, args in let (i, n) = args
        return ascii.characterIndex(i, offsetBy: n) },
      utf16: { utf16, args in let (i, n) = args
        return utf16.characterIndex(i, offsetBy: n) },
      opaque: { opaque, args in let (i, n) = args
        return opaque.characterIndex(i, offsetBy: n) })
  }

  /// Returns an index that is the specified distance from the given index,
  /// unless that distance is beyond a given limiting index.
  ///
  /// The following example obtains an index advanced four positions from a
  /// string's starting index and then prints the character at that position.
  /// The operation doesn't require going beyond the limiting `s.endIndex`
  /// value, so it succeeds.
  ///
  ///     let s = "Swift"
  ///     if let i = s.index(s.startIndex, offsetBy: 4, limitedBy: s.endIndex) {
  ///         print(s[i])
  ///     }
  ///     // Prints "t"
  ///
  /// The next example attempts to retrieve an index six positions from
  /// `s.startIndex` but fails, because that distance is beyond the index
  /// passed as `limit`.
  ///
  ///     let j = s.index(s.startIndex, offsetBy: 6, limitedBy: s.endIndex)
  ///     print(j)
  ///     // Prints "nil"
  ///
  /// The value passed as `distance` must not offset `i` beyond the bounds of the
  /// collection, unless the index passed as `limit` prevents offsetting
  /// beyond those bounds.
  ///
  /// - Parameters:
  ///   - i: A valid index of the collection.
  ///   - distance: The distance to offset `i`.
  ///   - limit: A valid index of the collection to use as a limit. If `distance > 0`,
  ///     a limit that is less than `i` has no effect. Likewise, if `distance < 0`, a
  ///     limit that is greater than `i` has no effect.
  /// - Returns: An index offset by `distance` from the index `i`, unless that index
  ///   would be beyond `limit` in the direction of movement. In that case,
  ///   the method returns `nil`.
  ///
  /// - Complexity: O(*k*), where *k* is the absolute value of `distance`.
  public func index(
    _ i: Index, offsetBy distance: IndexDistance, limitedBy limit: Index
  ) -> Index? {
    return _visitGuts(_guts, args: (i, distance, limit),
      ascii: { ascii, args in let (i, n, limit) = args
        return ascii.characterIndex(i, offsetBy: n, limitedBy: limit) },
      utf16: { utf16, args in let (i, n, limit) = args
        return utf16.characterIndex(i, offsetBy: n, limitedBy: limit) },
      opaque: { opaque, args in let (i, n, limit) = args
        return opaque.characterIndex(i, offsetBy: n, limitedBy: limit) })
  }

  /// Returns the distance between two indices.
  ///
  /// - Parameters:
  ///   - start: A valid index of the collection.
  ///   - end: Another valid index of the collection. If `end` is equal to
  ///     `start`, the result is zero.
  /// - Returns: The distance between `start` and `end`.
  ///
  /// - Complexity: O(*k*), where *k* is the resulting distance.
  public func distance(from start: Index, to end: Index) -> IndexDistance {
    return _visitGuts(_guts, args: (start, end),
      ascii: { ascii, args in let (start, end) = args
        return ascii.characterDistance(from: start, to: end) },
      utf16: { utf16, args in let (start, end) = args
        return utf16.characterDistance(from: start, to: end) },
      opaque: { opaque, args in let (start, end) = args
        return opaque.characterDistance(from: start, to: end) })
  }

  /// Accesses the character at the given position.
  ///
  /// You can use the same indices for subscripting a string and its substring.
  /// For example, this code finds the first letter after the first space:
  ///
  ///     let str = "Greetings, friend! How are you?"
  ///     let firstSpace = str.firstIndex(of: " ") ?? str.endIndex
  ///     let substr = str[firstSpace...]
  ///     if let nextCapital = substr.firstIndex(where: { $0 >= "A" && $0 <= "Z" }) {
  ///         print("Capital after a space: \(str[nextCapital])")
  ///     }
  ///     // Prints "Capital after a space: H"
  ///
  /// - Parameter i: A valid index of the string. `i` must be less than the
  ///   string's end index.
  public subscript(i: Index) -> Character {
    return _visitGuts(_guts, args: i,
      ascii: { ascii, i in return ascii.character(at: i) },
      utf16: { utf16, i in return utf16.character(at: i) },
      opaque: { opaque, i in return opaque.character(at: i) })
  }
}

extension String {
  /// Creates a new string containing the characters in the given sequence.
  ///
  /// You can use this initializer to create a new string from the result of
  /// one or more collection operations on a string's characters. For example:
  ///
  ///     let str = "The rain in Spain stays mainly in the plain."
  ///
  ///     let vowels: Set<Character> = ["a", "e", "i", "o", "u"]
  ///     let disemvoweled = String(str.lazy.filter { !vowels.contains($0) })
  ///
  ///     print(disemvoweled)
  ///     // Prints "Th rn n Spn stys mnly n th pln."
  ///
  /// - Parameter characters: A string instance or another sequence of
  ///   characters.
  @inlinable // FIXME(sil-serialize-all)
  public init<S : Sequence>(_ characters: S)
    where S.Iterator.Element == Character {
    self = ""
    self.append(contentsOf: characters)
  }

  /// Reserves enough space in the string's underlying storage to store the
  /// specified number of ASCII characters.
  ///
  /// Because each character in a string can require more than a single ASCII
  /// character's worth of storage, additional allocation may be necessary
  /// when adding characters to a string after a call to
  /// `reserveCapacity(_:)`.
  ///
  /// - Parameter n: The minimum number of ASCII character's worth of storage
  ///   to allocate.
  ///
  /// - Complexity: O(*n*)
  public mutating func reserveCapacity(_ n: Int) {
    _guts.reserveCapacity(n)
  }

  /// Appends the given character to the string.
  ///
  /// The following example adds an emoji globe to the end of a string.
  ///
  ///     var globe = "Globe "
  ///     globe.append("🌍")
  ///     print(globe)
  ///     // Prints "Globe 🌍"
  ///
  /// - Parameter c: The character to append to the string.
  public mutating func append(_ c: Character) {
    if let small = c._smallUTF16 {
      _guts.append(contentsOf: small)
    } else {
      _guts.append(c._largeUTF16!.unmanagedView)
      _fixLifetime(c)
    }
  }

  public mutating func append(contentsOf newElements: String) {
    append(newElements)
  }

  public mutating func append(contentsOf newElements: Substring) {
    _guts.append(
      newElements._wholeString._guts,
      range: newElements._encodedOffsetRange)
  }

  /// Appends the characters in the given sequence to the string.
  ///
  /// - Parameter newElements: A sequence of characters.
  @inlinable // FIXME(sil-serialize-all)
  public mutating func append<S : Sequence>(contentsOf newElements: S)
    where S.Iterator.Element == Character {
    if _fastPath(newElements is _SwiftStringView) {
      let v = newElements as! _SwiftStringView
      _guts.append(v._wholeString._guts, range: v._encodedOffsetRange)
      return
    }
    _guts.reserveUnusedCapacity(
      newElements.underestimatedCount,
      ascii: _guts.isASCII)
    for c in newElements { self.append(c) }
  }

  /// Replaces the text within the specified bounds with the given characters.
  ///
  /// Calling this method invalidates any existing indices for use with this
  /// string.
  ///
  /// - Parameters:
  ///   - bounds: The range of text to replace. The bounds of the range must be
  ///     valid indices of the string.
  ///   - newElements: The new characters to add to the string.
  ///
  /// - Complexity: O(*m*), where *m* is the combined length of the string and
  ///   `newElements`. If the call to `replaceSubrange(_:with:)` simply
  ///   removes text at the end of the string, the complexity is O(*n*), where
  ///   *n* is equal to `bounds.count`.
  @inlinable // FIXME(sil-serialize-all)
  public mutating func replaceSubrange<C>(
    _ bounds: Range<Index>,
    with newElements: C
  ) where C : Collection, C.Iterator.Element == Character {
    let offsetRange: Range<Int> =
      bounds.lowerBound.encodedOffset ..< bounds.upperBound.encodedOffset
    let lazyUTF16 = newElements.lazy.flatMap { $0.utf16 }
    _guts.replaceSubrange(offsetRange, with: lazyUTF16)
  }

  /// Inserts a new character at the specified position.
  ///
  /// Calling this method invalidates any existing indices for use with this
  /// string.
  ///
  /// - Parameters:
  ///   - newElement: The new character to insert into the string.
  ///   - i: A valid index of the string. If `i` is equal to the string's end
  ///     index, this methods appends `newElement` to the string.
  ///
  /// - Complexity: O(*n*), where *n* is the length of the string.
  @inlinable // FIXME(sil-serialize-all)
  public mutating func insert(_ newElement: Character, at i: Index) {
    let offset = i.encodedOffset
    _guts.replaceSubrange(offset..<offset, with: newElement.utf16)
  }

  /// Inserts a collection of characters at the specified position.
  ///
  /// Calling this method invalidates any existing indices for use with this
  /// string.
  ///
  /// - Parameters:
  ///   - newElements: A collection of `Character` elements to insert into the
  ///     string.
  ///   - i: A valid index of the string. If `i` is equal to the string's end
  ///     index, this methods appends the contents of `newElements` to the
  ///     string.
  ///
  /// - Complexity: O(*n*), where *n* is the combined length of the string and
  ///   `newElements`.
  @inlinable // FIXME(sil-serialize-all)
  public mutating func insert<S : Collection>(
    contentsOf newElements: S, at i: Index
  ) where S.Iterator.Element == Character {
    let offset = i.encodedOffset
    let utf16 = newElements.lazy.flatMap { $0.utf16 }
    _guts.replaceSubrange(offset..<offset, with: utf16)
  }

  /// Removes and returns the character at the specified position.
  ///
  /// All the elements following `i` are moved to close the gap. This example
  /// removes the hyphen from the middle of a string.
  ///
  ///     var nonempty = "non-empty"
  ///     if let i = nonempty.firstIndex(of: "-") {
  ///         nonempty.remove(at: i)
  ///     }
  ///     print(nonempty)
  ///     // Prints "nonempty"
  ///
  /// Calling this method invalidates any existing indices for use with this
  /// string.
  ///
  /// - Parameter i: The position of the character to remove. `i` must be a
  ///   valid index of the string that is not equal to the string's end index.
  /// - Returns: The character that was removed.
  @inlinable // FIXME(sil-serialize-all)
  @discardableResult
  public mutating func remove(at i: Index) -> Character {
    let offset = i.encodedOffset
    let stride = _stride(of: i)
    let range: Range<Int> = offset ..< offset + stride
    let old = Character(_unverified: _guts, range: range)
    _guts.replaceSubrange(range, with: EmptyCollection())
    return old
  }

  @inlinable // FIXME(sil-serialize-all)
  internal func _stride(of i: Index) -> Int {
    if let stride = i.characterStride { return stride }

    let offset = i.encodedOffset
    return _visitGuts(_guts, args: offset,
      ascii: { ascii, offset in
        return ascii.characterStride(atOffset: offset) },
      utf16: { utf16, offset in
        return utf16.characterStride(atOffset: offset) },
      opaque: { opaque, offset in
        return opaque.characterStride(atOffset: offset) })
  }

  /// Removes the characters in the given range.
  ///
  /// Calling this method invalidates any existing indices for use with this
  /// string.
  ///
  /// - Parameter bounds: The range of the elements to remove. The upper and
  ///   lower bounds of `bounds` must be valid indices of the string and not
  ///   equal to the string's end index.
  /// - Parameter bounds: The range of the elements to remove. The upper and
  ///   lower bounds of `bounds` must be valid indices of the string.
  @inlinable // FIXME(sil-serialize-all)
  public mutating func removeSubrange(_ bounds: Range<Index>) {
    let start = bounds.lowerBound.encodedOffset
    let end = bounds.upperBound.encodedOffset
    _guts.replaceSubrange(start..<end, with: EmptyCollection())
  }

  /// Replaces this string with the empty string.
  ///
  /// Calling this method invalidates any existing indices for use with this
  /// string.
  ///
  /// - Parameter keepCapacity: Pass `true` to prevent the release of the
  ///   string's allocated storage. Retaining the storage can be a useful
  ///   optimization when you're planning to grow the string again. The
  ///   default value is `false`.
  @inlinable // FIXME(sil-serialize-all)
  public mutating func removeAll(keepingCapacity keepCapacity: Bool = false) {
    if keepCapacity {
      _guts.replaceSubrange(0..<_guts.count, with: EmptyCollection())
    } else {
      _guts = _StringGuts()
    }
  }
}

extension String {
  // This is needed because of the issue described in SR-4660 which causes
  // source compatibility issues when String becomes a collection
  @inlinable // FIXME(sil-serialize-all)
  @_transparent
  public func max<T : Comparable>(_ x: T, _ y: T) -> T {
    return Swift.max(x,y)
  }

  // This is needed because of the issue described in SR-4660 which causes
  // source compatibility issues when String becomes a collection
  @inlinable // FIXME(sil-serialize-all)
  @_transparent
  public func min<T : Comparable>(_ x: T, _ y: T) -> T {
    return Swift.min(x,y)
  }
}

//===----------------------------------------------------------------------===//
// The following overloads of flatMap are carefully crafted to allow the code
// like the following:
//   ["hello"].flatMap { $0 }
// return an array of strings without any type context in Swift 3 mode, at the
// same time allowing the following code snippet to compile:
//   [0, 1].flatMap { x in
//     if String(x) == "foo" { return "bar" } else { return nil }
//   }
// Note that the second overload is declared on a more specific protocol.
// See: test/stdlib/StringFlatMap.swift for tests.
extension Sequence {
  @inlinable // FIXME(sil-serialize-all)
  @available(swift, obsoleted: 4)
  public func flatMap(
    _ transform: (Element) throws -> String
  ) rethrows -> [String] {
    return try map(transform)
  }
}

extension Collection {
  @available(swift, deprecated: 4.1, renamed: "compactMap(_:)",
    message: "Please use compactMap(_:) for the case where closure returns an optional value")
  @inline(__always)
  public func flatMap(
    _ transform: (Element) throws -> String?
  ) rethrows -> [String] {
    return try _compactMap(transform)
  }
}
//===----------------------------------------------------------------------===//

extension Sequence where Element == String {
  @available(*, unavailable, message: "Operator '+' cannot be used to append a String to a sequence of strings")
  public static func + (lhs: Self, rhs: String) -> Never {
    fatalError()
  }

  @available(*, unavailable, message: "Operator '+' cannot be used to append a String to a sequence of strings")
  public static func + (lhs: String, rhs: Self) -> Never {
    fatalError()
  }
}
