// RUN: %target-typecheck-verify-swift

// -----

protocol Foo {
  associatedtype Bar : Foo // expected-error{{type may not reference itself as a requirement}}
}

struct Oroborous : Foo {
  typealias Bar = Oroborous
}

// -----

protocol P {
 associatedtype A : P // expected-error{{type may not reference itself as a requirement}}
}

struct X<T: P> {
}

func f<T : P>(_ z: T) {
 _ = X<T.A>()
}

// -----

protocol PP2 {
  associatedtype A : P2 = Self // expected-error{{type may not reference itself as a requirement}}
}

protocol P2 : PP2 {
  associatedtype A = Self
}

struct X2<T: P2> {
}

struct Y2 : P2 {
  typealias A = Y2
}

func f<T : P2>(_ z: T) {
 _ = X2<T.A>() // expected-error{{type 'T.A' does not conform to protocol 'P2'}}
}

// -----

protocol P3 {
 associatedtype A: P4 = Self // expected-error{{type may not reference itself as a requirement}}
}

protocol P4 : P3 {}

protocol DeclaredP : P3, // expected-warning{{redundant conformance constraint 'Self': 'P3'}}
P4 {} // expected-note{{conformance constraint 'Self': 'P3' implied here}}

struct Y3 : DeclaredP {
}

struct X3<T:P4> {}

func f2<T:P4>(_ a: T) {
 _ = X3<T.A>()
}

f2(Y3())

// -----

protocol Alpha {
  associatedtype Beta: Gamma
}

protocol Gamma {
  associatedtype Delta: Alpha
}

// FIXME: Redundancy diagnostics are odd here.
struct Epsilon<T: Alpha, // expected-note{{conformance constraint 'U': 'Gamma' implied here}}
// expected-warning@-1{{redundant conformance constraint 'T': 'Alpha'}}
               U: Gamma> // expected-warning{{redundant conformance constraint 'U': 'Gamma'}}
// expected-note@-1{{conformance constraint 'T': 'Alpha' implied here}}
  where T.Beta == U,
        U.Delta == T {}

// -----

protocol AsExistentialA {
  var delegate : AsExistentialB? { get }
}
protocol AsExistentialB {
  func aMethod(_ object : AsExistentialA)
}

protocol AsExistentialAssocTypeA {
  var delegate : AsExistentialAssocTypeB? { get } // expected-error {{protocol 'AsExistentialAssocTypeB' can only be used as a generic constraint because it has Self or associated type requirements}}
}
protocol AsExistentialAssocTypeB {
  func aMethod(_ object : AsExistentialAssocTypeA)
  associatedtype Bar
}

protocol AsExistentialAssocTypeAgainA {
  var delegate : AsExistentialAssocTypeAgainB? { get }
  associatedtype Bar
}
protocol AsExistentialAssocTypeAgainB {
  func aMethod(_ object : AsExistentialAssocTypeAgainA) // expected-error {{protocol 'AsExistentialAssocTypeAgainA' can only be used as a generic constraint because it has Self or associated type requirements}}
}

// SR-547
protocol A {
    associatedtype B1: B
    associatedtype C1: C
    
    mutating func addObserver(_ observer: B1, forProperty: C1)
}

protocol C {
    
}

protocol B {
    associatedtype BA: A
    associatedtype BC: C
    
    func observeChangeOfProperty(_ property: BC, observable: BA)
}


