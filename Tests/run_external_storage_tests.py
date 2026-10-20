"""Compile actual persistence, ring log and calendar C with a power-cut NOR model."""
from pathlib import Path
import argparse
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--deps', type=Path)
args = parser.parse_args()
if args.deps: sys.path.insert(0, str(args.deps))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_PC

root = Path(__file__).resolve().parent.parent
build = root / 'Tests/build'
build.mkdir(exist_ok=True)
cc = Path('D:/Keil/core/ARM/ARMCC/ARMCC/bin/armcc.exe')
objects = []
def run(command):
    result = subprocess.run([str(value) for value in command], capture_output=True, text=True, errors='replace')
    if result.returncode:
        print(result.stdout, result.stderr)
        result.check_returncode()
for relative in ['Tests/test_external_storage.c','Core/Src/storage_codec.c','Core/Src/settings_store.c',
                 'Core/Src/history_store.c','Core/Src/time_calendar.c']:
    source = root / relative
    obj = build / ('external_' + source.stem + '.o')
    run([cc,'--cpu=Cortex-M3','--thumb','--c99','-O2','--library_type=microlib','--split_sections',
         '-I' + str(root / 'Core/Inc'),'-c',source,'-o',obj])
    objects.append(obj)
target = build / 'external_storage_tests.axf'
run([cc.with_name('armlink.exe'), *objects, '--cpu=Cortex-M3','--library_type=microlib',
     '--libpath=' + str(cc.parent.parent / 'lib'),'--ro_base=0x10000','--rw_base=0x20000000',
     '--entry=RunExternalStorageTests','-o',target])
machine = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
machine.mem_map(0x10000,0x30000)
machine.mem_map(0x20000000,0x20000)
with target.open('rb') as stream:
    elf = ELFFile(stream)
    entry = elf.header.e_entry
    for segment in elf.iter_segments():
        if segment.header.p_type == 'PT_LOAD' and segment.header.p_filesz:
            machine.mem_write(segment.header.p_vaddr, segment.data())
machine.reg_write(UC_ARM_REG_SP,0x2001F000)
machine.reg_write(UC_ARM_REG_LR,0x3FFF1)
machine.emu_start(entry | 1, 0x3FFF0, count=200000000)
assert machine.reg_read(UC_ARM_REG_PC) == 0x3FFF0, 'Test entry did not return'
failures = machine.reg_read(UC_ARM_REG_R0)
names = ['CRC golden fixture and little endian', 'empty/default/save/load/idempotent settings',
         'CRC corrupt newest settings falls back', '32 parameter program cut points preserve old copy',
         'erase failure and invalid ranges never replace settings', 'first log/header and packed fields',
         'restart session and uncalibrated/invalid sample flags', '32 log program cut points and append recovery',
         'sector rotation, oldest-sector overwrite and parameter isolation', 'restart scan and corrupted record skip',
         'foreign header rejected without erase', 'UTC+8 fixture/leap-day conversion', 'invalid dates and year limit']
names += ['read I/O failure is distinct from empty history']
names += ['same-session inference preserves raw records and hides anchor rows',
          'persisted anchor survives restart and reserves session identity',
          'separate boots never share calibration anchors',
          'nearest later calibration and original RTC timestamps',
          'invalid anchors, uptime ordering and calendar lower bound',
          '32 anchor write cut points never manufacture calibrated dates',
          'legacy RTC sample supplies an inference point',
          'anchor and samples across a sector boundary after restart',
          'CRC-corrupt anchor leaves samples uncalibrated',
          'anchor-only session has no fake sample and survives restart']
names += ['scan read failure exposes exact address without erase',
          'active-sector slot read failure exposes exact address',
          'erase failure is distinct from header program failure',
          'incomplete header write reports its sector address',
          'incomplete sample write reports slot address and stops writes',
          'missing or undersized media has an explicit failure reason',
          'valid chain remains usable beside an invalid unrelated header',
          '32 first-header cut points preserve no-auto-format policy']
for index, name in enumerate(names): print(('FAIL' if failures & (1 << index) else 'PASS') + ': ' + name)
print('External storage/calendar failure mask:', hex(failures))
raise SystemExit(1 if failures else 0)
