#!/bin/bash

names=( 
    "llvm"
    "clang"
    "swift"
    "lldb"
    "cmark"
    "llbuild"
    "swiftpm"
    "compiler-rt"
    "swift-corelibs-xctest"
    "swift-corelibs-foundation"
    "swift-corelibs-libdispatch"
    "swift-integration-tests"
    "swift-xcode-playground-support"
)

paths=(
    "apple/swift-llvm"
    "apple/swift-clang"
    "apple/swift"
    "apple/swift-lldb"
    "apple/swift-cmark"
    "apple/swift-llbuild"
    "apple/swift-package-manager"
    "apple/swift-compiler-rt"
    "apple/swift-corelibs-xctest"
    "apple/swift-corelibs-foundation"
    "apple/swift-corelibs-libdispatch"
    "apple/swift-integration-tests"
    "apple/swift-xcode-playground-support"
)

swift_3_0_tag="swift-3.0-RELEASE"
swift_3_0_1_tag="swift-3.0.1-RELEASE"
swift_3_0_2_tag="swift-3.0.2-RELEASE"
swift_3_1_tag="swift-3.1-RELEASE"
swift_3_1_1_tag="swift-3.1.1-RELEASE"
swift_4_0_tag="swift-4.0-RELEASE"
swift_4_0_2_tag="swift-4.0.2-RELEASE"
swift_4_0_3_tag="swift-4.0.3-RELEASE"
swift_4_1_tag="swift-4.1-RELEASE"
swift_4_1_1_tag="swift-4.1.1-RELEASE"
swift_4_1_2_tag="swift-4.1.2-RELEASE"
swift_4_1_3_tag="swift-4.1.3-RELEASE"

if [ $# -eq 0 ]
then
    echo "Using default: Swift 4.1.2"
    tag=$swift_4_1_2_tag # default to swift 4.1.2

else
    tag_parameter="$1"
    case $tag_parameter in
        -3.0|--swift-3.0)
	tag=$swift_3_0_tag
        ;;
        -3.0.1|--swift-3.0.1)
	tag=$swift_3_0_1_tag
        ;;
        -3.0.2|--swift-3.0.2)
	tag=$swift_3_0_2_tag
        ;;
        -3.1|--swift-3.1)
	tag=$swift_3_1_tag
        ;;
        -3.1.1|--swift-3.1.1)
	tag=$swift_3_1_1_tag
        ;;
        -4.0|--swift-4.0)
	tag=$swift_4_0_tag
        ;;
        -4.0.2|--swift-4.0.2)
	tag=$swift_4_0_2_tag
        ;;
        -4.0.3|--swift-4.0.3)
	tag=$swift_4_0_3_tag
        ;;
        -4.1|--swift-4.1)
	tag=$swift_4_1_tag
        ;;
        -4.1.1|--swift-4.1.1)
	tag=$swift_4_1_1_tag
        ;;
        -4.1.2|--swift-4.1.2)
	tag=$swift_4_1_2_tag
        ;;
        -4.1.3|--swift-4.1.3)
	tag=$swift_4_1_3_tag
        ;;
        -h|--help)
        echo "Available parameters for checing dependencies in given version:"
        echo ""
        echo "-3.0   | --swift-3.0   -> Swift 3.0   (release)"
        echo "-3.0.1 | --swift-3.0.1 -> Swift 3.0.1 (release)"
        echo "-3.0.2 | --swift-3.0.2 -> Swift 3.0.2 (release)"
        echo "-3.1   | --swift-3.1   -> Swift 3.1   (release)"
        echo "-3.1.1 | --swift-3.1.1 -> Swift 3.1.1 (release)"
        echo "-4.0   | --swift-4.0   -> Swift 4.0   (release)"
        echo "-4.0.2 | --swift-4.0.2 -> Swift 4.0.2 (release)"
        echo "-4.0.3 | --swift-4.0.3 -> Swift 4.0.3 (release)"
        echo "-4.1   | --swift-4.1   -> Swift 4.1   (release)"
        echo "-4.1.1 | --swift-4.1.1 -> Swift 4.1.1 (release)"
        echo "-4.1.2 | --swift-4.1.2 -> Swift 4.1.2 (release)"
        echo "-4.1.3 | --swift-4.1.3 -> Swift 4.1.3 (release)"
        exit 0
        ;;
        *)
        echo "Invalid parameter ${tag_parameter}"
        echo "Use --help or -h to discover valid parameters"
        exit 1
        ;;
    esac
fi

diff_start=3cb38854e963b84873a7b7769b6c0b3f28c86015 # the starting commit
diff_end=$(git log -1 --format="%H")

echo "GENERATE PATCH: git checkout ${diff_end} && git format-patch ${diff_start} --stdout > obfuscator.patch"
git checkout ${diff_end} && git format-patch ${diff_start} --stdout > obfuscator.patch

echo "RESET TO START: git reset --hard ${diff_start}"
git reset --hard ${diff_start}

for i in ${!names[@]}; do
  echo "DATA: ${names[$i]} -> ${paths[$i]} -> ${tag_parameter}"
  echo "GIT REMOVE: git rm -r ${names[$i]}"
  git rm -r ${names[$i]}
  echo "REMOTE: git remote add ${names[$i]} https://github.com/${paths[$i]}.git"
  git remote add ${names[$i]} https://github.com/${paths[$i]}.git
  echo "FETCH: git fetch --tags ${names[$i]} \"+refs/tags/*:refs/rtags/${names[$i]}/*\""
  git fetch --tags ${names[$i]} "+refs/tags/*:refs/rtags/${names[$i]}/*"
  echo "READ-TREE: git read-tree refs/rtags/${names[$i]}/${tag} -u --prefix=${names[$i]}"
  git read-tree refs/rtags/${names[$i]}/${tag} -u --prefix=${names[$i]}
done

git rm -r ninja
git remote add ninja https://github.com/ninja-build/ninja.git
git fetch ninja
git read-tree --prefix=ninja -u ninja/release

git add . -A
git commit -m "Temporary commit for changing dependencies to ${tag}"

echo "APPLY PATCH: git am < obfuscator.patch"
git am --3way < obfuscator.patch

echo "REMOVE PATCH: rm obfuscator.patch"
rm obfuscator.patch

