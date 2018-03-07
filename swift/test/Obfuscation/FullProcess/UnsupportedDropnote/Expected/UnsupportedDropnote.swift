

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


