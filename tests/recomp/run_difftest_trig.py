#!/usr/bin/env python3
"""Run the unmodified differential tester with the opt-in x87 trig emitter."""
import os, sys
sys.path.insert(0, os.path.abspath('tools'))
import difftest
import recomp_x87_fixed
recomp_x87_fixed.activate()
sys.exit(difftest.main())
