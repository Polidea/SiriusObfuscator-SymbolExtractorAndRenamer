

import AppKit


class T1_Test {
  func NF1_testFunc() {}
}

// debug blocks are omitted
final class T1_DebugBlock {
  fileprivate init() {
    #if !DEBUG
      let V1_testInDebug = T1_Test()
    #endif
  }
}

// fun with protocols and extensions
struct T1_TestStruct {}

protocol T1_ParentProtocol {
  associatedtype Fuzz
  associatedtype Bazz
  associatedtype Gazz
  
  func NF1_foo(_ IP1_indexPath: Int) -> String
  func NF1_bar(_ IP1_fuzz: Fuzz, EP1_extBazz IP1_bazz: Bazz, EP1_extGazz IP1_gazz: Gazz, EP1_atIndexPath IP2_indexPath: Int)
}

protocol T1_ChildProtocol: T1_ParentProtocol {
  var V1_items: [[Gazz]] { get }
}


protocol T1_ChildProtocol2: T1_ChildProtocol { }


final class T1_TestClass {
  
  var V1_items: [[Gazz]] = [[
    
    ]]
}


extension T1_TestClass: T1_ChildProtocol2 {
  
  func NF1_foo(_ IP1_indexPath: Int) -> String { return "" }
  
  func NF1_bar(_ IP1_fuzz: String, EP1_extBazz IP1_bazz: T1_TestStruct, EP1_extGazz IP1_gazz: T1_Test, EP1_atIndexPath IP2_indexPath: Int) {}
}

// overridden method parameters

final class T1_TestController: NSViewController {
  
  override func viewDidLoad() {
    super.viewDidLoad()
  }
  
  override func prepare(for segue: NSStoryboardSegue, sender: Any?) {
    super.prepare(for: segue, sender)
  }
}

// protocol vars in extensions

class T1_TestWithBool {
  var V1_isFoo = false
}

func NF1_foo(SP1_boolParam: Bool) {}

protocol T1_Activable {
  var V1_active: Bool { get set }
}

extension T1_Activable where Self: T1_TestWithBool {
  var V1_active: Bool {
    get {
      return V1_isFoo
    }
    set(activeValue) {
      NF1_foo(activeValue)
    }
  }
}

// non working if case cast

class T1_SomeGenericClass<Param> {}

enum T1_RandomEnum {
  case Foo
}

func NF1_someRandomFunc() -> T1_RandomEnum { return T1_RandomEnum.Foo }

final class T1_TestController2: NSViewController {
  
  override func viewDidLoad() {
    super.viewDidLoad()
  }
  
  override func prepare(for segue: NSStoryboardSegue, sender: Any?) {
    if case .Foo = NF1_someRandomFunc() , sender is T1_SomeGenericClass<String> {
      let V1_casted = sender as! T1_SomeGenericClass<String>
    }
  }
}

// for each stuff

final class T1_ForEachController: NSViewController {
  
  var V1_unitsSegmentedControl: NSSegmentedControl!
  
  var V1_titles: [String] = []
  
  fileprivate func NF1_buggyFunc() {
    V1_titles.enumerated().map {
      index, title in (title, index)
      }.forEach(V1_unitsSegmentedControl.setLabel(_:forSegment:))
    
    V1_unitsSegmentedControl.accessibilityHint = ""
  }
}

// protocol functions strikes back

class T1_NotWorkingParent {
  func NF1_addSearchItem() {
  }
}

final class T1_NextNotWorking: T1_NotWorkingParent {
  
  override func NF1_addSearchItem() {
    let inserter = T1_ItemInserter()
    do {
      let coffee = try inserter.NF1_insertEntityWithName("")
      
    } catch {
      
    }
  }
}

protocol T1_ItemInserterType {
  associatedtype Entity
  func NF1_insertEntityWithName(_ IP1_name: String) throws -> Entity
}

struct T1_ItemInserter: T1_ItemInserterType {
  
  func NF1_insertEntityWithName(_ IP1_name: String) throws -> String {
    return ""
  }
}

// nested funcs stuff

class T1_NestedFuncs {
  
  fileprivate func NF1_broken() -> [Int] {
    
    func NF1_makeInt(EP1_withIdentifier IP1_identifier: String, SP1_model: Int) -> Int {
      return 42
    }
    
    var ints = [Int]()
    ints.append(NF1_makeInt(EP1_withIdentifier: "", SP1_model: 42))
    ints.append(NF1_makeInt(EP1_withIdentifier: "", SP1_model: 42))
    return ints
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


