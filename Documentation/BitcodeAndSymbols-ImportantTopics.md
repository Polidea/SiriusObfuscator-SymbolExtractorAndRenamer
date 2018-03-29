# Important topics for Bitcode, Symbols and other stuff

The goal of this document is to be a bag of important ideas, concepts and discoveries related to system and other low level things. Currently these include:

1. [What is Bitcode and how does it affect obfuscation?](#bitcode)
2. [How can someone disassemble my app?](#jailbreak)
3. [What does Xcode's "Strip Swift Symbols" option do?](#strip)
4. [What are dSYM files?](#dsym)
5. [How can I symbolicate crash logs from an obfuscated app?](#symbolicate)
6. [Resources](#resources)

# <a name="bitcode"></a> Bitcode

TODO

# <a name="jailbreak"></a> Disassembling an app

TODO

# <a name="strip"></a> Stripping Swift symbols

TODO

# <a name="dsym"></a> dSYM files

dSYM files contain debug symbols of an app and allow for [symbolicating crash logs](#symbolicate).

That means that debug symbols can be removed from a release build of an app reducing its binary size and making it harder to reverse engineer.

Under the hood dSYM files are using [DWARF](http://dwarfstd.org/) format. It allows the compiler to tell the debugger how the original source code relates to the binary.

# <a name="symbolicate"></a> Symbolicating crash logs

Symbolication is the process of replacement of addresses in crash logs with human readable values. Without first symbolicating a crash report it is difficult to determine where the crash occurred.

In order to perform symbolication a dSYM file is needed. The process can be performed in Xcode or it can be done by services like Crashlytics ([you have to provide them with a proper dSYM file for your app first](https://docs.fabric.io/apple/crashlytics/missing-dsyms.html#upload-symbols)).

Unfortunately current version of the Sirius Swift Obfuscator doesn't support generating non-obfuscated dSYM files and doesn't provide a tool to deobfuscate crash logs. We're aware that this is an important feature and will provide such tool in the future. It's technically challenging because of the complexity of the [DWARF](http://dwarfstd.org/) format used by dSYM files.

# <a name="resources"></a> Resources

You can find more detailed information about Bitcode, dSYM files and crash log symbolication in an [Apple technical note TN2151](#https://developer.apple.com/library/content/technotes/tn2151/_index.html).
