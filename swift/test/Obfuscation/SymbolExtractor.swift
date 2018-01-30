//RUN: echo "{\"project\": {\"rootPath\": \"TetsRootPath\"}, \"module\": {\"name\": \"TestModuleName\"}, \"sdk\": {\"name\": \"%target-sdk-name\", \"path\": \"%sdk\"}, \"filenames\": [\"%S/Inputs\/ViewController.swift\", \"%S/Inputs\/AppDelegate.swift\"], \"explicitelyLinkedFrameworks\": [], \"systemLinkedFrameworks\": []}" > %T/files.json
//RUN: obfuscator-symbol-extractor -filesjson %T/files.json -symbolsjson %t
//RUN: diff -w %S/Inputs/expectedSymbols.json %t

