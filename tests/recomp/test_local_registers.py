#!/usr/bin/env python3
"""Structural guards for opt-in straight-line guest-register caching.
The differential test additionally compares executed programs against Unicorn.
"""
import os, sys, unittest
sys.path.insert(0, os.path.abspath('tools'))
import difftest, recomp_local as recomp

class LocalRegisters(unittest.TestCase):
    def emitted(self, source, unsupported=None):
        pe = difftest.FakePE(difftest.asm(source))
        rc = recomp.LocalRecompiler(pe, 't'); rc.use_eh = False
        rc.local_register_opt = True; rc.discover()
        if unsupported:
            old = rc.tr
            def tr(i, f, body):
                if i.mnemonic == unsupported: raise recomp.Unsupported('test')
                return old(i, f, body)
            rc.tr = tr
        return '\n'.join(rc.emit_function(pe.base))

    def test_groups_commit_before_memory_and_division(self):
        c = self.emitted(['mov eax, 2','add eax, 3','mov dword ptr [esi], eax',
                          'mov ebx, 7','add ebx, 1','idiv ecx','ret'])
        store = c.index('wro32('); divide = c.index('rt_raise_hw(')
        self.assertLess(c.index('c->eax = lr_eax;'), store)
        self.assertLess(c.index('c->ebx = lr_ebx;'), divide)
        self.assertNotIn('/*@LOCAL_', c)

    def test_control_flow_target_flushes(self):
        c = self.emitted(['mov eax, 2','add eax, 1','jz next','mov ebx, 2',
                          'add ebx, 1','next:','mov ecx, 3','add ecx, 4','ret'])
        jump = c.index('if (zf) goto'); self.assertLess(c.index('c->eax = lr_eax;'), jump)
        label = c.index('\nL_', jump); self.assertLess(c.index('c->ebx = lr_ebx;'), label)
        self.assertNotIn('/*@LOCAL_', c)

    def test_unsupported_after_earlier_group(self):
        c = self.emitted(['mov eax, 1','add eax, 2','mov dword ptr [esi], eax',
                          'mov ebx, 2','add ebx, 1','not ecx','ret'], 'not')
        self.assertNotIn('/*@LOCAL_', c)
        self.assertEqual(c.count('experimental straight-line guest registers'), 2)
        self.assertLess(c.index('c->ebx = lr_ebx;'), c.index('UNIMPL('))

if __name__ == '__main__': unittest.main()
