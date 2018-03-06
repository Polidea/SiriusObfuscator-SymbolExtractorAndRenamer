

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

// overridden method parameters

final class T1_TestController: NSViewController {
  
  override func viewDidLoad() {
    super.viewDidLoad()
  }
  
  override func prepare(for segue: NSStoryboardSegue, sender: Any?) {
    super.prepare(for: segue, sender)
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

// mocking trick

protocol KeyValueStoreType {
  func object(forKey defaultName: String) -> Any?
  func set(_ value: Any?, forKey defaultName: String)
  func removeObject(forKey defaultName: String)
  func synchronize() -> Bool
}

extension UserDefaults: KeyValueStoreType {
  
}


