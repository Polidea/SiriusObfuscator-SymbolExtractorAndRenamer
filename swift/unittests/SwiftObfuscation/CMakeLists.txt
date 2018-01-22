add_swift_unittest(SwiftObfuscationTests
    RandomTests.cpp
    SwiftObfuscationTests.cpp
    FileIOTests.cpp
)

target_link_libraries(SwiftObfuscationTests
    swiftObfuscation
)
