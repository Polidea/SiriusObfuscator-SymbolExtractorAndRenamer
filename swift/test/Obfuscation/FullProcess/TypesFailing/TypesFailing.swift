//XFAIL: *

//RUN: %target-prepare-obfuscation-for-file "TypesFailing" %target-run-full-obfuscation

protocol SampleProtocol { }

struct Generic<GenericParam> {
  class InsideGeneric: Generic<SampleProtocol> {}
}

class `BackticksName` {}

class `BackticksNameGeneric`<`BackticksGenericParam`> {
  class `BackticksInsideBackticksGeneric`: `BackticksNameGeneric`<`BackticksNameGeneric`<`BackticksName`>> {}
}

class SampleClass {}

extension SampleProtocol where Self: SampleClass {}

extension SampleProtocol where Self == SampleClass {}
