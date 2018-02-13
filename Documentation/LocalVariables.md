# Why Local Variables are not obfuscated?


Local variables are not obfuscated because they won't be visible in the compiled binary. Unlike other constructs, local variables are not included in the [symbol table](https://en.wikipedia.org/wiki/Symbol_table). They are kept on [stack](https://en.wikipedia.org/wiki/Call_stack) or in [registers](https://en.wikipedia.org/wiki/Processor_register) depending on how compiler optimized the code. You may be now wondering - how the debugger know the names of local variables? It uses special debug informations that are included in the compiled binary when it's compiled in "debug mode". In release builds these special debug informations are stripped off.


References:
  - ["Advanced Apple Debugging & Reverse Engineering" by Derek Selander (pages 124, 159)](https://store.raywenderlich.com/products/advanced-apple-debugging-and-reverse-engineering)
  - [Compiler, Assembler, Linker and Loader:
 a brief story](http://www.tenouk.com/ModuleW.html)
