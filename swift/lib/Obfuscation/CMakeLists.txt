add_swift_library(swiftObfuscation STATIC
  Obfuscation.cpp
  DataStructures.cpp
  FileIO.cpp
  Random.cpp
  NameMapping.cpp
  LINK_LIBRARIES
  swiftParse
  swiftAST
  swiftSema
  swiftDriver
  swiftFrontend
)

