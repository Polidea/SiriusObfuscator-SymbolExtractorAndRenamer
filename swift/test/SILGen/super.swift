// RUN: rm -rf %t
// RUN: mkdir -p %t
// RUN: %target-swift-frontend -I %t -emit-module -emit-module-path=%t/resilient_struct.swiftmodule -module-name resilient_struct %S/../Inputs/resilient_struct.swift
// RUN: %target-swift-frontend -I %t -emit-module -emit-module-path=%t/resilient_class.swiftmodule -module-name resilient_class %S/../Inputs/resilient_class.swift
// RUN: %target-swift-frontend -emit-silgen -parse-as-library -I %t %s | %FileCheck %s

import resilient_class

public class Parent {
  public final var finalProperty: String {
    return "Parent.finalProperty"
  }

  public var property: String {
    return "Parent.property"
  }

  public final class var finalClassProperty: String {
    return "Parent.finalProperty"
  }

  public class var classProperty: String {
    return "Parent.property"
  }

  public func methodOnlyInParent() {}
  public final func finalMethodOnlyInParent() {}
  public func method() {}

  public final class func finalClassMethodOnlyInParent() {}
  public class func classMethod() {}
}

public class Child : Parent {
  // CHECK-LABEL: sil @_T05super5ChildC8propertySSfg : $@convention(method) (@guaranteed Child) -> @owned String {
  // CHECK:       bb0([[SELF:%.*]] : $Child):
  // CHECK:         [[SELF_COPY:%.*]] = copy_value [[SELF]]
  // CHECK:         [[CASTED_SELF_COPY:%[0-9]+]] = upcast [[SELF_COPY]] : $Child to $Parent
  // CHECK:         [[SUPER_METHOD:%[0-9]+]] = function_ref @_T05super6ParentC8propertySSfg : $@convention(method) (@guaranteed Parent) -> @owned String
  // CHECK:         [[RESULT:%.*]] = apply [[SUPER_METHOD]]([[CASTED_SELF_COPY]])
  // CHECK:         destroy_value [[CASTED_SELF_COPY]]
  // CHECK:         return [[RESULT]]
  public override var property: String {
    return super.property
  }

  // CHECK-LABEL: sil @_T05super5ChildC13otherPropertySSfg : $@convention(method) (@guaranteed Child) -> @owned String {
  // CHECK:       bb0([[SELF:%.*]] : $Child):
  // CHECK:         [[COPIED_SELF:%.*]] = copy_value [[SELF]]
  // CHECK:         [[CASTED_SELF_COPY:%[0-9]+]] = upcast [[COPIED_SELF]] : $Child to $Parent
  // CHECK:         [[SUPER_METHOD:%[0-9]+]] = function_ref @_T05super6ParentC13finalPropertySSfg
  // CHECK:         [[RESULT:%.*]] = apply [[SUPER_METHOD]]([[CASTED_SELF_COPY]])
  // CHECK:         destroy_value [[CASTED_SELF_COPY]]
  // CHECK:         return [[RESULT]]
  public var otherProperty: String {
    return super.finalProperty
  }
}

public class Grandchild : Child {
  // CHECK-LABEL: sil @_T05super10GrandchildC06onlyInB0yyF
  public func onlyInGrandchild() {
    // CHECK: function_ref @_T05super6ParentC012methodOnlyInB0yyF : $@convention(method) (@guaranteed Parent) -> ()
    super.methodOnlyInParent()
    // CHECK: function_ref @_T05super6ParentC017finalMethodOnlyInB0yyF
    super.finalMethodOnlyInParent()
  }

  // CHECK-LABEL: sil @_T05super10GrandchildC6methodyyF
  public override func method() {
    // CHECK: function_ref @_T05super6ParentC6methodyyF : $@convention(method) (@guaranteed Parent) -> ()
    super.method()
  }
}

public class GreatGrandchild : Grandchild {
  // CHECK-LABEL: sil @_T05super15GreatGrandchildC6methodyyF
  public override func method() {
    // CHECK: function_ref @_T05super10GrandchildC6methodyyF : $@convention(method) (@guaranteed Grandchild) -> ()
    super.method()
  }
}

public class ChildToResilientParent : ResilientOutsideParent {
  // CHECK-LABEL: sil @_T05super22ChildToResilientParentC6methodyyF : $@convention(method) (@guaranteed ChildToResilientParent) -> ()
  public override func method() {
    // CHECK: [[COPY:%.*]] = copy_value %0
    // CHECK: super_method [[COPY]] : $ChildToResilientParent, #ResilientOutsideParent.method!1 : (ResilientOutsideParent) -> () -> (), $@convention(method) (@guaranteed ResilientOutsideParent) -> ()
    // CHECK: return
    super.method()
  }

  // CHECK-LABEL: sil @_T05super22ChildToResilientParentC11classMethodyyFZ : $@convention(method) (@thick ChildToResilientParent.Type) -> ()
  public override class func classMethod() {
    // CHECK: super_method %0 : $@thick ChildToResilientParent.Type, #ResilientOutsideParent.classMethod!1 : (ResilientOutsideParent.Type) -> () -> (), $@convention(method) (@thick ResilientOutsideParent.Type) -> ()
    // CHECK: return
    super.classMethod()
  }

  // CHECK-LABEL: sil @_T05super22ChildToResilientParentC11returnsSelfACXDyFZ : $@convention(method) (@thick ChildToResilientParent.Type) -> @owned ChildToResilientParent
  public class func returnsSelf() -> Self {
    // CHECK: super_method %0 : $@thick ChildToResilientParent.Type, #ResilientOutsideParent.classMethod!1 : (ResilientOutsideParent.Type) -> () -> ()
    // CHECK: unreachable
    super.classMethod()
  }
}

public class ChildToFixedParent : OutsideParent {
  // CHECK-LABEL: sil @_T05super18ChildToFixedParentC6methodyyF : $@convention(method) (@guaranteed ChildToFixedParent) -> ()
  public override func method() {
    // CHECK: [[COPY:%.*]] = copy_value %0
    // CHECK: super_method [[COPY]] : $ChildToFixedParent, #OutsideParent.method!1 : (OutsideParent) -> () -> (), $@convention(method) (@guaranteed OutsideParent) -> ()
    // CHECK: return
    super.method()
  }

  // CHECK-LABEL: sil @_T05super18ChildToFixedParentC11classMethodyyFZ : $@convention(method) (@thick ChildToFixedParent.Type) -> ()
  public override class func classMethod() {
    // CHECK: super_method %0 : $@thick ChildToFixedParent.Type, #OutsideParent.classMethod!1 : (OutsideParent.Type) -> () -> (), $@convention(method) (@thick OutsideParent.Type) -> ()
    // CHECK: return
    super.classMethod()
  }

  // CHECK-LABEL: sil @_T05super18ChildToFixedParentC11returnsSelfACXDyFZ : $@convention(method) (@thick ChildToFixedParent.Type) -> @owned ChildToFixedParent
  public class func returnsSelf() -> Self {
    // CHECK: super_method %0 : $@thick ChildToFixedParent.Type, #OutsideParent.classMethod!1 : (OutsideParent.Type) -> () -> ()
    // CHECK: unreachable
    super.classMethod()
  }
}

public extension ResilientOutsideChild {
  public func callSuperMethod() {
    super.method()
  }

  public class func callSuperClassMethod() {
    super.classMethod()
  }
}

public class GenericBase<T> {
  public func method() {}
}

public class GenericDerived<T> : GenericBase<T> {
  public override func method() {
    // CHECK-LABEL: sil private @_T05super14GenericDerivedC6methodyyFyycfU_ : $@convention(thin) <T> (@owned GenericDerived<T>) -> ()
    // CHECK: upcast {{.*}} : $GenericDerived<T> to $GenericBase<T>
    // CHECK: return
    {
      super.method()
    }()

    // CHECK-LABEL: sil private @_T05super14GenericDerivedC6methodyyF13localFunctionL_yylF : $@convention(thin) <T> (@owned GenericDerived<T>) -> ()
    // CHECK: upcast {{.*}} : $GenericDerived<T> to $GenericBase<T>
    // CHECK: return

    func localFunction() {
      super.method()
    }
    localFunction()

    // CHECK-LABEL: sil private @_T05super14GenericDerivedC6methodyyF15genericFunctionL_yqd__r__lF : $@convention(thin) <T><U> (@in U, @owned GenericDerived<T>) -> ()
    // CHECK: upcast {{.*}} : $GenericDerived<T> to $GenericBase<T>
    // CHECK: return
    func genericFunction<U>(_: U) {
      super.method()
    }
    genericFunction(0)
  }
}
