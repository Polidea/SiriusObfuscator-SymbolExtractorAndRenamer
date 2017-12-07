//===--- StringEdits.swift ------------------------------------------------===//
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

import TestsUtils
#if os(Linux)
import Glibc
#else
import Darwin
#endif

var editWords: [String] = [
  "woodshed",
  "lakism",
  "gastroperiodynia",
]

let alphabet = "abcdefghijklmnopqrstuvwxyz"
/// All edits that are one edit away from `word`
func edits(_ word: String) -> Set<String> {
  // create right/left splits as CharacterViews instead
  let splits = word.characters.indices.map {
    (word.characters[word.characters.startIndex..<$0],word.characters[$0..<word.characters.endIndex])
  }
  
  // though it should be, CharacterView isn't hashable
  // so using an array for now, ignore that aspect...
  var result: [String.CharacterView] = []
  
  for (left,right) in splits {
    // drop a character
    result.append(left + right.dropFirst())
    
    // transpose two characters
    if let fst = right.first {
      let drop1 = right.dropFirst()
      if let snd = drop1.first {
        result.append(left + [snd,fst] + drop1.dropFirst())
      }
    }
    
    // replace each character with another
    for letter in alphabet {
      result.append(left + [letter] + right.dropFirst())
    }
    
    // insert rogue characters
    for letter in alphabet {
      result.append(left + [letter] + right)
    }
  }
  
  // have to map back to strings right at the end
  return Set(result.lazy.map(String.init))
}

@inline(never)
public func run_StringEdits(_ N: Int) {
  for _ in 1...N*100 {
    for word in editWords {
      _ = edits(word)      
    }
  }
}

