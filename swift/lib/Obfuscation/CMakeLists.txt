add_swift_library(swiftObfuscation STATIC
  Obfuscation.cpp
  DataStructures.cpp
  FileIO.cpp
  LINK_LIBRARIES
  swiftParse
  swiftAST
  swiftSema
)

