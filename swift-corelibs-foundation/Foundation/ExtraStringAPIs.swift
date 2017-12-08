//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

// Random access for String.UTF16View, only when Foundation is
// imported.  Making this API dependent on Foundation decouples the
// Swift core from a UTF16 representation.
extension String.UTF16View.Index {
    public func advanced(by n: Int) -> String.UTF16View.Index {
        return String.UTF16View.Index(encodedOffset: encodedOffset.advanced(by: n))
    }
}


