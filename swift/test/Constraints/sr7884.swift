// RUN: %target-typecheck-verify-swift

// SR-7884
func f<T>(_ x: T) -> T {
  return x
}

func f<T>(_ x: T?) -> T? {
  return x
}

let r = f(1) // calls optional variant
let _ = r! // expected-error {{cannot force unwrap value of non-optional type 'Int'}}

// SR-7899
let optSeq: LazySequence<[Int]>? = nil
let lazySequence = optSeq?.lazy
let value = lazySequence?.compactMap({ $0 as? Int }).first // expected-warning {{conditional cast from 'Int' to 'Int' always succeeds}}
let _: Int = value!
