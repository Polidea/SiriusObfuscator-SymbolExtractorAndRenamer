# TestREPLIntVars.py
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ------------------------------------------------------------------------------
"""Test that basic integer arithmetic works in the REPL."""

import os
import time
import unittest2
import lldb
import lldbsuite.test.decorators as decorators
import lldbsuite.test.lldbrepl as lldbrepl


class REPLIntVarsTestCase (lldbrepl.REPLTest):

    mydir = lldbrepl.REPLTest.compute_mydir(__file__)

    @decorators.swiftTest
    @decorators.no_debug_info_test
    @decorators.expectedFailureAll(
        oslist=["linux"],
        bugnumber="rdar://23081322")
    def testREPL(self):
        lldbrepl.REPLTest.testREPL(self)

    def doTest(self):
        self.command('3 + 2', patterns=['\\$R0: Int = 5'])
        self.command('$R0 + 5', patterns=['\\$R1: Int = 10'])
