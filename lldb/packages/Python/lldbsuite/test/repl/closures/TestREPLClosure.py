# TestREPLClosure.py
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
"""Test that we can define and use closures in the REPL"""

import os
import time
import unittest2
import lldb
import lldbsuite.test.lldbrepl as lldbrepl


class REPLClosureTestCase (lldbrepl.REPLTest):

    mydir = lldbrepl.REPLTest.compute_mydir(__file__)

    def doTest(self):
        self.command(
            'var names = [ "Chris", "Alex", "Ewa", "Barry", "Daniella" ]',
            patterns=[
                'names: \\[String\\] = 5 values {',
                '\\[0\\] = "Chris"',
                '\\[1\\] = "Alex"',
                '\\[2\\] = "Ewa"',
                '\\[3\\] = "Barry"',
                '\\[4\\] = "Daniella"',
                '}'])
        self.command('names.sort {$0 < $1}')
        self.command(
            'names',
            patterns=[
                '\\$R0: \\[String\\] = 5 values {',
                '\\[0\\] = "Alex"',
                '\\[1\\] = "Barry"',
                '\\[2\\] = "Chris"',
                '\\[3\\] = "Daniella"',
                '\\[4\\] = "Ewa"',
                '}'])
