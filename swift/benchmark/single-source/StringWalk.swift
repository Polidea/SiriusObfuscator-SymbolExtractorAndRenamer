//===--- StringWalk.swift -------------------------------------*- swift -*-===//
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

////////////////////////////////////////////////////////////////////////////////
// WARNING: This file is manually generated from .gyb template and should not
// be directly modified. Instead, make changes to StringWalk.swift.gyb and run
// scripts/generate_harness/generate_harness.py to regenerate this file.
////////////////////////////////////////////////////////////////////////////////


// Test String subscript performance.
//
// Subscript has a slow path that initializes a global variable:
// Swift._cocoaStringSubscript.addressor.  Global optimization would
// normally hoist the initializer outside the inner loop (over
// unicodeScalars), forcing the initializer to be called on each
// lap. However, no that the cocoa code is properly marked "slowPath",
// no hoisting should occur.
import TestsUtils

var count: Int = 0

//
// Helper functionality
//

@inline(never) func count_unicodeScalars(_ s: String.UnicodeScalarView) {
  for _ in s {
    count += 1
  }
}
@inline(never) func count_characters(_ s: String.CharacterView) {
  for _ in s {
    count += 1
  }
}
@inline(never) func count_unicodeScalars_rev(
  _ s: ReversedCollection<String.UnicodeScalarView>
) {
  for _ in s {
    count += 1
  }
}
@inline(never) func count_characters_rev(
  _ s: ReversedCollection<String.CharacterView>
) {
  for _ in s {
    count += 1
  }
}

//
// Workloads
//
let ascii =
  "siebenhundertsiebenundsiebzigtausendsiebenhundertsiebenundsiebzig"
let emoji = "👍👩‍👩‍👧‍👧👨‍👨‍👦‍👦🇺🇸🇨🇦🇲🇽👍🏻👍🏼👍🏽👍🏾👍🏿"
let utf16 = emoji + "the quick brown fox" + String(emoji.reversed() as Array<Character>)

let japanese = "今回のアップデートでSwiftに大幅な改良が施され、安定していてしかも直感的に使うことができるAppleプラットフォーム向けプログラミング言語になりました。"
let chinese = "Swift 是面向 Apple 平台的编程语言，功能强大且直观易用，而本次更新对其进行了全面优化。"
let korean = "이번 업데이트에서는 강력하면서도 직관적인 Apple 플랫폼용 프로그래밍 언어인 Swift를 완벽히 개선하였습니다."
let russian = "в чащах юга жил-был цитрус? да, но фальшивый экземпляр"

// A workload that's mostly Latin characters, with occasional emoji
// interspersed. Common for tweets.
let tweet = "Worst thing about working on String is that it breaks *everything*. Asserts, debuggers, and *especially* printf-style debugging 😭"

//
// Benchmarks
//

// Pre-commit benchmark: simple scalar walk
@inline(never)
public func run_StringWalk(_ N: Int) {
  return run_StringWalk_ascii_unicodeScalars(N)
}

// Extended String benchmarks:
let baseMultiplier = 10_000
let unicodeScalarsMultiplier = baseMultiplier
let charactersMultiplier = baseMultiplier / 5


@inline(never)
public func run_StringWalk_ascii_unicodeScalars(_ N: Int) {
  for _ in 1...unicodeScalarsMultiplier*N {
    count_unicodeScalars(ascii.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_ascii_unicodeScalars_Backwards(_ N: Int) {
  for _ in 1...unicodeScalarsMultiplier*N {
    count_unicodeScalars_rev(ascii.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_ascii_characters(_ N: Int) {
  for _ in 1...charactersMultiplier*N {
    count_characters(ascii.characters)
  }
}

@inline(never)
public func run_StringWalk_ascii_characters_Backwards(_ N: Int) {
  for _ in 1...charactersMultiplier*N {
    count_characters_rev(ascii.characters.reversed())
  }
}


@inline(never)
public func run_StringWalk_utf16_unicodeScalars(_ N: Int) {
  for _ in 1...unicodeScalarsMultiplier*N {
    count_unicodeScalars(utf16.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_utf16_unicodeScalars_Backwards(_ N: Int) {
  for _ in 1...unicodeScalarsMultiplier*N {
    count_unicodeScalars_rev(utf16.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_utf16_characters(_ N: Int) {
  for _ in 1...charactersMultiplier*N {
    count_characters(utf16.characters)
  }
}

@inline(never)
public func run_StringWalk_utf16_characters_Backwards(_ N: Int) {
  for _ in 1...charactersMultiplier*N {
    count_characters_rev(utf16.characters.reversed())
  }
}


@inline(never)
public func run_StringWalk_tweet_unicodeScalars(_ N: Int) {
  for _ in 1...unicodeScalarsMultiplier*N {
    count_unicodeScalars(tweet.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_tweet_unicodeScalars_Backwards(_ N: Int) {
  for _ in 1...unicodeScalarsMultiplier*N {
    count_unicodeScalars_rev(tweet.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_tweet_characters(_ N: Int) {
  for _ in 1...charactersMultiplier*N {
    count_characters(tweet.characters)
  }
}

@inline(never)
public func run_StringWalk_tweet_characters_Backwards(_ N: Int) {
  for _ in 1...charactersMultiplier*N {
    count_characters_rev(tweet.characters.reversed())
  }
}


@inline(never)
public func run_StringWalk_japanese_unicodeScalars(_ N: Int) {
  for _ in 1...unicodeScalarsMultiplier*N {
    count_unicodeScalars(japanese.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_japanese_unicodeScalars_Backwards(_ N: Int) {
  for _ in 1...unicodeScalarsMultiplier*N {
    count_unicodeScalars_rev(japanese.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_japanese_characters(_ N: Int) {
  for _ in 1...charactersMultiplier*N {
    count_characters(japanese.characters)
  }
}

@inline(never)
public func run_StringWalk_japanese_characters_Backwards(_ N: Int) {
  for _ in 1...charactersMultiplier*N {
    count_characters_rev(japanese.characters.reversed())
  }
}


@inline(never)
public func run_StringWalk_chinese_unicodeScalars(_ N: Int) {
  for _ in 1...unicodeScalarsMultiplier*N {
    count_unicodeScalars(chinese.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_chinese_unicodeScalars_Backwards(_ N: Int) {
  for _ in 1...unicodeScalarsMultiplier*N {
    count_unicodeScalars_rev(chinese.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_chinese_characters(_ N: Int) {
  for _ in 1...charactersMultiplier*N {
    count_characters(chinese.characters)
  }
}

@inline(never)
public func run_StringWalk_chinese_characters_Backwards(_ N: Int) {
  for _ in 1...charactersMultiplier*N {
    count_characters_rev(chinese.characters.reversed())
  }
}


@inline(never)
public func run_StringWalk_korean_unicodeScalars(_ N: Int) {
  for _ in 1...unicodeScalarsMultiplier*N {
    count_unicodeScalars(korean.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_korean_unicodeScalars_Backwards(_ N: Int) {
  for _ in 1...unicodeScalarsMultiplier*N {
    count_unicodeScalars_rev(korean.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_korean_characters(_ N: Int) {
  for _ in 1...charactersMultiplier*N {
    count_characters(korean.characters)
  }
}

@inline(never)
public func run_StringWalk_korean_characters_Backwards(_ N: Int) {
  for _ in 1...charactersMultiplier*N {
    count_characters_rev(korean.characters.reversed())
  }
}


@inline(never)
public func run_StringWalk_russian_unicodeScalars(_ N: Int) {
  for _ in 1...unicodeScalarsMultiplier*N {
    count_unicodeScalars(russian.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_russian_unicodeScalars_Backwards(_ N: Int) {
  for _ in 1...unicodeScalarsMultiplier*N {
    count_unicodeScalars_rev(russian.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_russian_characters(_ N: Int) {
  for _ in 1...charactersMultiplier*N {
    count_characters(russian.characters)
  }
}

@inline(never)
public func run_StringWalk_russian_characters_Backwards(_ N: Int) {
  for _ in 1...charactersMultiplier*N {
    count_characters_rev(russian.characters.reversed())
  }
}

