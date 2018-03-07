

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

// mocking trick

protocol KeyValueStoreType {
  func object(forKey defaultName: String) -> Any?
  func set(_ value: Any?, forKey defaultName: String)
  func removeObject(forKey defaultName: String)
  func synchronize() -> Bool
}

extension UserDefaults: KeyValueStoreType {
  
}


