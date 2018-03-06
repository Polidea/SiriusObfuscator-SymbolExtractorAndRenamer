//XFAIL: *
//RUN: %target-prepare-obfuscation-for-file "UnsupportedDropnote" %target-run-full-obfuscation


import AppKit

class Test {
  func testFunc() {}
}

// debug blocks are omitted
final class DebugBlock {
  fileprivate init() {
    #if !DEBUG
      let testInDebug = Test()
    #endif
  }
}

// fun with protocols and extensions
struct TestStruct {}

protocol ParentProtocol {
  associatedtype Fuzz
  associatedtype Bazz
  associatedtype Gazz
  
  func foo(_ indexPath: Int) -> String
  func bar(_ fuzz: Fuzz, extBazz bazz: Bazz, extGazz gazz: Gazz, atIndexPath indexPath: Int)
}

protocol ChildProtocol: ParentProtocol {
  var items: [[Gazz]] { get }
}


protocol ChildProtocol2: ChildProtocol { }


final class TestClass {
  
  var items: [[Gazz]] = [[
    
    ]]
}


extension TestClass: ChildProtocol2 {
  
  func foo(_ indexPath: Int) -> String { return "" }
  
  func bar(_ fuzz: String, extBazz bazz: TestStruct, extGazz gazz: Test, atIndexPath indexPath: Int) {}
}

// non working if case cast

class SomeGenericClass<Param> {}

enum RandomEnum {
  case Foo
}

func someRandomFunc() -> RandomEnum { return RandomEnum.Foo }

final class TestController2: NSViewController {
  
  override func viewDidLoad() {
    super.viewDidLoad()
  }
  
  override func prepare(for segue: NSStoryboardSegue, sender: Any?) {
    if case .Foo = someRandomFunc() , sender is SomeGenericClass<String> {
      let casted = sender as! SomeGenericClass<String>
    }
  }
}

// for each stuff

final class ForEachController: NSViewController {
  
  var unitsSegmentedControl: NSSegmentedControl!
  
  var titles: [String] = []
  
  fileprivate func buggyFunc() {
    titles.enumerated().map {
      index, title in (title, index)
      }.forEach(unitsSegmentedControl.setLabel(_:forSegment:))
    
    unitsSegmentedControl.accessibilityHint = ""
  }
}

// mocking trick

protocol KeyValueStoreType {
  func object(forKey defaultName: String) -> Any?
  func set(_ value: Any?, forKey defaultName: String)
  func removeObject(forKey defaultName: String)
  func synchronize() -> Bool
}

extension UserDefaults: KeyValueStoreType {
  
}


