# Important topics for Renamer

The goal of this document is to be a bag of important ideas, decisions, concepts and discoveries in the `Renamer` project. Currently these include:

1. [How does renaming process work?](#renaming)
2. [Where does `Renamer` take new (obfuscated) names from?](#names)
3. [Do you support layout files like `.xib` and `.storyboard`?](#layouts)
4. [Is it possible to exclude parts of the code from being obfuscated?](#exclude)
5. [How can I verify if the code was renamed correctly?](#verification)

# <a name="renaming"></a> How does it work?

???

# <a name="names"></a> Where are new names coming from?

The obfuscated names for symbols come from [`NameMapper` tool](./NameMapper-ImportantTopics.md) which output is `Renames.json`. This file contains all information needed to perform renaming such as `originalName` and `obfuscatedName` for each symbol and is later consumed by `Renamer` tool which performs actual renaming.

# <a name="layouts"></a> Are layout files renamed?

Yes, layout files are being renamed.

First [`FileExtractor` tool](./ImportantTopics.md) extracts information about all layout (`.xib` and `.storyboard`) files that are present in the project.
Then when `Renamer` is renaming symbols, it also stores each successfully renamed symbol in a set. After that it iterates over gathered layout files and renames them one by one. Both `.xib` and `.storyboard` are `.xml` files. `Renamer` parses them using `libxml` and then traverses the tree looking for types and functions that can be renamed. It decides if a found name should be renamed by checking if such symbol was renamed during symbol renaming. Such check is possible because it has access to the set containing all renamed symbols. After the file is finally processed `Renamer` saves the result in obfuscated project directory.

Currently only class names are being obfuscated. Support for outlets and actions is in the future plans.

# <a name="exclude"></a> Excluding parts of code from obfuscation

It's not possible currently. The feature is in the future plans.

# <a name="verification"></a> Verify if the code was correctly renamed

???
