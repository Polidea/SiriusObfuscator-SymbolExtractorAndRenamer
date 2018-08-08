// RUN: %target-swift-frontend -emit-ir  -module-name outcopyaddr -primary-file %s | %FileCheck %s

public protocol BaseProt {
}

public protocol ChildProt: BaseProt {
}

public struct BaseStruct<T: BaseProt> {
    public typealias Element = T
    public var elem1: Element
    public var elem2: Element
}

public struct StructWithBaseStruct<T: BaseProt> {
    public typealias Element = T
    var elem1: Element
    var elem2: BaseStruct<Element>
}

// CHECK-LABEL: define hidden swiftcc void @"$S11outcopyaddr010StructWithbc4BaseB0V4elemAA0bcdB0VyxGvg"(%T11outcopyaddr014StructWithBaseB0V.5* noalias nocapture sret, %swift.type* %"StructWithStructWithBaseStruct<T>", %T11outcopyaddr010StructWithbc4BaseB0V* noalias nocapture swiftself)
// CHECK: call %T11outcopyaddr014StructWithBaseB0V.5* @"$S11outcopyaddr014StructWithBaseB0VyxGAA9ChildProtRzlWOc"
public struct StructWithStructWithBaseStruct<T: ChildProt> {
    public typealias Element = T
    let elem: StructWithBaseStruct<Element>
}

protocol P { }

class OtherPrivate<T> { }

struct OtherInternal<T> {
  var myPrivate: OtherPrivate<T>? = nil
}

struct MyPrivate<T: P> {
  var otherHelper: OtherInternal<T>? = nil

  // CHECK-LABEL: define hidden swiftcc {{i32|i64}} @"$S11outcopyaddr9MyPrivateVyACyxGxcfC"(%swift.opaque* noalias nocapture, %swift.type* %T, i8** %T.P) {{.*}} {
  // CHECK: call %T11outcopyaddr9MyPrivateV* @"$S11outcopyaddr9MyPrivateVyxGAA1PRzlWOh"(%T11outcopyaddr9MyPrivateV* {{%.*}})
  // CHECK: ret
  init(_: T) { }
}

extension P {
  func foo(data: Any) {
    _ = MyPrivate(data as! Self)
  }
}

enum GenericError<T: BaseProt> {
  case payload(T)
}

func testIt<P: BaseProt>(_ f: GenericError<P>?) {
}

func dontCrash<P: BaseProt>(_ f: GenericError<P>) {
  testIt(f)
}

protocol Baz : class {
}
extension Baz {
  static func crash(setup: ((Self) -> ())?){
  }
}
class Foobar {
  public static func dontCrash() -> Baz? {
     let cls : Baz.Type = Foobar1.self
     // This used to crash because we tried to outline the optional consume with
     // an opened payload existential type.
     cls.crash(setup: { (arg:  Baz) -> () in  })
     return nil
  }
}
class Foobar1 : Foobar, Baz { }
