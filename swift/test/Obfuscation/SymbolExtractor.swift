//RUN: echo "{\"project\": {\"rootPath\": \"TetsRootPath\"}, \"module\": {\"name\": \"TestModuleName\", \"triple\": \"x86_64-apple-macosx10.13\"}, \"sdk\": {\"name\": \"%target-sdk-name\", \"path\": \"%sdk\"}, \"filenames\": [\"%S/Inputs\/ViewController.swift\", \"%S/Inputs\/AppDelegate.swift\"], \"explicitelyLinkedFrameworks\": [], \"systemLinkedFrameworks\": []}" > %T/files.json
//RUN: obfuscator-symbol-extractor -filesjson %T/files.json -symbolsjson %t -printdiagnostics
//RUN: diff -w %S/Inputs/expectedSymbols.json %t

