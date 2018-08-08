//===--- Join.swift - Protocol and Algorithm for concatenation ------------===//
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

/// A sequence that presents the elements of a base sequence of sequences
/// concatenated using a given separator.
@_fixed_layout // FIXME(sil-serialize-all)
public struct JoinedSequence<Base : Sequence> where Base.Element : Sequence {

  public typealias Element = Base.Element.Element
  
  @usableFromInline // FIXME(sil-serialize-all)
  internal var _base: Base
  @usableFromInline // FIXME(sil-serialize-all)
  internal var _separator: ContiguousArray<Element>

  /// Creates an iterator that presents the elements of the sequences
  /// traversed by `base`, concatenated using `separator`.
  ///
  /// - Complexity: O(`separator.count`).
  @inlinable // FIXME(sil-serialize-all)
  public init<Separator : Sequence>(base: Base, separator: Separator)
    where Separator.Element == Element {
    self._base = base
    self._separator = ContiguousArray(separator)
  }
}

extension JoinedSequence {
  /// An iterator that presents the elements of the sequences traversed
  /// by a base iterator, concatenated using a given separator.
  @_fixed_layout // FIXME(sil-serialize-all)
  public struct Iterator {
    @usableFromInline // FIXME(sil-serialize-all)
    internal var _base: Base.Iterator
    @usableFromInline // FIXME(sil-serialize-all)
    internal var _inner: Base.Element.Iterator?
    @usableFromInline // FIXME(sil-serialize-all)
    internal var _separatorData: ContiguousArray<Element>
    @usableFromInline // FIXME(sil-serialize-all)
    internal var _separator: ContiguousArray<Element>.Iterator?
    
    @_frozen // FIXME(sil-serialize-all)
    @usableFromInline // FIXME(sil-serialize-all)
    internal enum JoinIteratorState {
      case start
      case generatingElements
      case generatingSeparator
      case end
    }
    @usableFromInline // FIXME(sil-serialize-all)
    internal var _state: JoinIteratorState = .start

    /// Creates a sequence that presents the elements of `base` sequences
    /// concatenated using `separator`.
    ///
    /// - Complexity: O(`separator.count`).
    @inlinable // FIXME(sil-serialize-all)
    public init<Separator: Sequence>(base: Base.Iterator, separator: Separator)
      where Separator.Element == Element {
      self._base = base
      self._separatorData = ContiguousArray(separator)
    }
  }  
}

extension JoinedSequence.Iterator: IteratorProtocol {
  public typealias Element = Base.Element.Element

  /// Advances to the next element and returns it, or `nil` if no next element
  /// exists.
  ///
  /// Once `nil` has been returned, all subsequent calls return `nil`.
  @inlinable // FIXME(sil-serialize-all)
  public mutating func next() -> Element? {
    while true {
      switch _state {
      case .start:
        if let nextSubSequence = _base.next() {
          _inner = nextSubSequence.makeIterator()
          _state = .generatingElements
        } else {
          _state = .end
          return nil
        }

      case .generatingElements:
        let result = _inner!.next()
        if _fastPath(result != nil) {
          return result
        }
        _inner = _base.next()?.makeIterator()
        if _inner == nil {
          _state = .end
          return nil
        }
        if !_separatorData.isEmpty {
          _separator = _separatorData.makeIterator()
          _state = .generatingSeparator
        }

      case .generatingSeparator:
        let result = _separator!.next()
        if _fastPath(result != nil) {
          return result
        }
        _state = .generatingElements

      case .end:
        return nil
      }
    }
  }
}

extension JoinedSequence: Sequence {
  /// Return an iterator over the elements of this sequence.
  ///
  /// - Complexity: O(1).
  @inlinable // FIXME(sil-serialize-all)
  public func makeIterator() -> Iterator {
    return Iterator(base: _base.makeIterator(), separator: _separator)
  }

  @inlinable // FIXME(sil-serialize-all)
  public func _copyToContiguousArray() -> ContiguousArray<Element> {
    var result = ContiguousArray<Element>()
    let separatorSize: Int = numericCast(_separator.count)

    let reservation = _base._preprocessingPass {
      () -> Int in
      var r = 0
      for chunk in _base {
        r += separatorSize + chunk.underestimatedCount
      }
      return r - separatorSize
    }

    if let n = reservation {
      result.reserveCapacity(numericCast(n))
    }

    if separatorSize == 0 {
      for x in _base {
        result.append(contentsOf: x)
      }
      return result
    }

    var iter = _base.makeIterator()
    if let first = iter.next() {
      result.append(contentsOf: first)
      while let next = iter.next() {
        result.append(contentsOf: _separator)
        result.append(contentsOf: next)
      }
    }

    return result
  }
}
  
extension Sequence where Element : Sequence {
  /// Returns the concatenated elements of this sequence of sequences,
  /// inserting the given separator between each element.
  ///
  /// This example shows how an array of `[Int]` instances can be joined, using
  /// another `[Int]` instance as the separator:
  ///
  ///     let nestedNumbers = [[1, 2, 3], [4, 5, 6], [7, 8, 9]]
  ///     let joined = nestedNumbers.joined(separator: [-1, -2])
  ///     print(Array(joined))
  ///     // Prints "[1, 2, 3, -1, -2, 4, 5, 6, -1, -2, 7, 8, 9]"
  ///
  /// - Parameter separator: A sequence to insert between each of this
  ///   sequence's elements.
  /// - Returns: The joined sequence of elements.
  @inlinable // FIXME(sil-serialize-all)
  public func joined<Separator : Sequence>(
    separator: Separator
  ) -> JoinedSequence<Self>
    where Separator.Element == Element.Element {
    return JoinedSequence(base: self, separator: separator)
  }
}

// @available(*, deprecated, renamed: "JoinedSequence.Iterator")
public typealias JoinedIterator<T: Sequence> = JoinedSequence<T>.Iterator where T.Element: Sequence
