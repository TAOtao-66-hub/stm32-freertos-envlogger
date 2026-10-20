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
for relative in ['Tests/test_history_clear.c','Core/Src/storage_codec.c','Core/Src/settings_store.c',
                 'Core/Src/history_store.c','Core/Src/time_calendar.c']:
    source = root / relative
    obj = build / ('history_clear_' + source.stem + '.o')
    run([cc,'--cpu=Cortex-M3','--thumb','--c99','-O2','--library_type=microlib','--split_sections',
         '-I' + str(root / 'Core/Inc'),'-c',source,'-o',obj])
    objects.append(obj)
target = build / 'history_clear.axf'
run([cc.with_name('armlink.exe'), *objects, '--cpu=Cortex-M3','--library_type=microlib',
     '--libpath=' + str(cc.parent.parent / 'lib'),'--ro_base=0x10000','--rw_base=0x20000000',
     '--entry=RunHistoryClearTests','-o',target])
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
names = ['empty media can commit an empty logical start', 'one history erase preserves both settings sectors', 'old bytes remain outside the visible history boundary', 'a clear invalidates an existing snapshot reader', 'committed clear survives restart with monotonic sequence', 'new records are readable without reviving pre-clear data', 'clear boundary carries through rotation and restart', 'repeated clear remains durable and settings-safe', 'all 32 header cut points reject uncommitted clear', 'complete committed header remains cleared after restart', 'erase failure stops writes and reports the correct sector', 'lost commit verification returns error but restart recognizes clear', 'unready history cannot be silently formatted', 'clear boundary handles generation wrap including zero', 'empty iterator completion remains stable on repeated reads', 'full ring stays usable after logical clear']
for index,name in enumerate(names):
    print(('FAIL' if failures & (1 << index) else 'PASS') + ': ' + name)
print('Failure mask:', hex(failures))
raise SystemExit(1 if failures else 0)
