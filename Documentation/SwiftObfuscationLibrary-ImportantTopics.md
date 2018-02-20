# Important topics for Swift Obfuscation

The goal of this document is to be a bag of important ideas, decisions, concepts and discoveries in the `swiftObfuscation` library. Currently these include:

* [What is `swiftObfuscation` library?](#what)
* [Why do we write the templates implementations in *-template.h files?](#templates)
* [Why do we use llvm::yaml for json deserialization and swift::json for json serialization?](#json)

# <a name="what"></a> What is `swiftObfuscation` library?

The `swiftObfuscation` library is the underlying common codebase for all the compiler-based obfuscation subtools: `SymbolExtractor`, `NameMapper` and `Renamer`. It's purpose is to maximally reduce the amount of code duplicated in the command line tools and to strip them out of all the  job-performing logic, leaving them with the sole responsibility of defining and maintaining the actual command-line interface. It let's us easily share the model (including the data structures and their serialization and deserialization) and the AST parsing logic.

# <a name="templates"></a> Why do we write the templates implementations in `*-template.h` files?

To enable the compiler to generate the required implementations of the template methods one may use either the implicit or explicit instantiation. The implicit one means that there's no requirement on the programmer's side to specify that are the template parameters used in the program; compiler will deduce them looking and the usages. The explicit one means that the programmer writes all the possible use cases.

We want to use the implicit instantiation because we want to be able to test the code in the unit test module. Testing requires instantiation of our template code with the mock types, so it's impossible to explicitly state what are the possible types for template without including the mock types in the production code. We want our production code to not know nor see them, so the only option is the implicit instantiation.

To leverage the implicit instantiation the compiler must have the template methods implementation available in the header. To somehow indicate that some part of the header contains declaration and the other part contains implementation we artifically split the header into two, one with traditional `.h` extension (declaration) and the second one with `-template.h` suffix (implementation).

For more context please see [this StackOverflow answer](https://stackoverflow.com/a/495056) and [further explanation](https://isocpp.org/wiki/faq/templates#templates-defn-vs-decl).

# <a name="json"></a> Why do we use `llvm::yaml` for JSON deserialization and `swift::json` for JSON serialization?

LLVM contains the YAML parser with the serialization and deserialization support. At the same time:

> "YAML can therefore be viewed as a natural superset of JSON, offering improved human readability and a more complete information model. This is also the case in practice; every JSON file is also a valid YAML file."
- http://yaml.org/spec/1.2/spec.html#id2759572

Therefore to deserialize JSON (read JSON from file) the already existing `llvm::yaml` parser is perfectly fine. However, the serialization (write JSON to file) cannot be done with `llvm::yaml` because it prints YAML, not JSON. Thankfully, the Swift compiler community has provided the tool for JSON serialization (not deserialization) in the `swift::json` namespace. It keeps almost the same API as the LLVM YAML parser and therefore is easy to use. That's why we're using it.
