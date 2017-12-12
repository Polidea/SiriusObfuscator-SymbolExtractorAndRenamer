add_swift_host_tool(obfuscator-renamer
  obfuscator-renamer.cpp
  LINK_LIBRARIES 
  swiftDriver 
  swiftFrontend 
  swiftObfuscation
  SWIFT_COMPONENT 
  tools
)

