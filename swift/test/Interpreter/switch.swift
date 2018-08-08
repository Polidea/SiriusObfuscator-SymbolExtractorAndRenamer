// RUN: %target-run-simple-swift
// REQUIRES: executable_test

import StdlibUnittest

var SwitchTestSuite = TestSuite("Switch")

public enum Phase<Value> {
  case possible
  case active(Value)
  case paused(Value)
  case ended(Value)
  case failed
}

extension Phase {
  public var valueLet: Value? {
	  switch self {
    case .possible, .failed:
      return nil
    case let .active(value), let .paused(value), let .ended(value):
      return value
    }
  }

  public var valueVar: Value? {
	  switch self {
    case .possible, .failed:
      return nil
    case var .active(value), var .paused(value), var .ended(value):
      return value
    }
  }
}

enum K {
  case A, B
}

enum A<K> {
  case left(a: K, b: K)
  case right(a: K, b: K)
  
  var valueLet: [K] {
    switch self {
    case let .left(a, b), let .right(a, b):
      return [a, b]
    }
  }
  
  var valueVar: [K] {
    switch self {
    case var .left(a, b), var .right(a, b):
      return [a, b]
    }
  }
}

SwitchTestSuite.test("GenericLet") {
  do {
    expectEqual(1.0, Phase.active(1.0).valueLet)
    expectEqual(2.0, Phase.paused(2.0).valueLet)
    expectEqual(3.0, Phase.ended(3.0).valueLet)
  }

  do {
    let l = LifetimeTracked(0)
    expectTrue(l === Phase.active(l).valueLet)
    expectTrue(l === Phase.paused(l).valueLet)
    expectTrue(l === Phase.ended(l).valueLet)
  }

  do {
    expectEqual([K.A, K.B], A.left(a: K.A, b: K.B).valueLet)
    expectEqual([K.A, K.B], A.right(a: K.A, b: K.B).valueLet)
  }

  do {
    let l = LifetimeTracked(0)
    let r = LifetimeTracked(0)
    let arr = A.left(a: l, b: r).valueLet
    expectTrue(arr[0] === l)
    expectTrue(arr[1] === r)
  }

  do {
    let l = LifetimeTracked(0)
    let r = LifetimeTracked(0)
    let arr = A.right(a: l, b: r).valueLet
    expectTrue(arr[0] === l)
    expectTrue(arr[1] === r)
  }
}

SwitchTestSuite.test("GenericVar") {
  do {
    expectEqual(1.0, Phase.active(1.0).valueVar)
    expectEqual(2.0, Phase.paused(2.0).valueVar)
    expectEqual(3.0, Phase.ended(3.0).valueVar)
  }

  do {
    let l = LifetimeTracked(0)
    expectTrue(l === Phase.active(l).valueVar)
    expectTrue(l === Phase.paused(l).valueVar)
    expectTrue(l === Phase.ended(l).valueVar)
  }

  do {
    expectEqual([K.A, K.B], A.left(a: K.A, b: K.B).valueVar)
    expectEqual([K.A, K.B], A.right(a: K.A, b: K.B).valueVar)
  }

  do {
    let l = LifetimeTracked(0)
    let r = LifetimeTracked(0)
    let arr = A.left(a: l, b: r).valueVar
    expectTrue(arr[0] === l)
    expectTrue(arr[1] === r)
  }

  do {
    let l = LifetimeTracked(0)
    let r = LifetimeTracked(0)
    let arr = A.right(a: l, b: r).valueVar
    expectTrue(arr[0] === l)
    expectTrue(arr[1] === r)
  }
}

enum Gesture {
  case pan(Any)
  case pinch(Any)
}

extension Gesture {
  var valueLet: Any {
    switch self {
    case .pan(let data),
         .pinch(let data):
      return data
    }
  }
  var valueVar: Any {
    switch self {
    case .pan(var data),
         .pinch(var data):
      return data
    }
  }
}

SwitchTestSuite.test("GenericLet") {
  expectEqual(1, Gesture.pan(1).valueLet as! Int)
  expectEqual(2, Gesture.pinch(2).valueLet as! Int)

  let l = LifetimeTracked(0)
  expectTrue(l === Gesture.pan(l).valueLet as! LifetimeTracked)
  expectTrue(l === Gesture.pinch(l).valueLet as! LifetimeTracked)
}

SwitchTestSuite.test("GenericVar") {
  expectEqual(1, Gesture.pan(1).valueVar as! Int)
  expectEqual(2, Gesture.pinch(2).valueVar as! Int)

  let l = LifetimeTracked(0)
  expectTrue(l === Gesture.pan(l).valueVar as! LifetimeTracked)
  expectTrue(l === Gesture.pinch(l).valueVar as! LifetimeTracked)
}

runAllTests()
