"""Compile actual SPI/ADC drivers and service commands against transport models."""
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
for relative in ['Tests/test_external_peripherals.c','Core/Src/w25qxx.c','Core/Src/potentiometer.c',
                 'Core/Src/project_commands.c','Core/Src/time_calendar.c']:
    source = root / relative
    obj = build / ('peripheral_' + source.stem + '.o')
    run([cc,'--cpu=Cortex-M3','--thumb','--c99','-O2','--library_type=microlib','--split_sections',
         '-I' + str(root / 'Tests/external_stubs'),'-I' + str(root / 'Core/Inc'),'-c',source,'-o',obj])
    objects.append(obj)
target = build / 'external_peripheral_tests.axf'
run([cc.with_name('armlink.exe'), *objects, '--cpu=Cortex-M3','--library_type=microlib',
     '--libpath=' + str(cc.parent.parent / 'lib'),'--ro_base=0x10000','--rw_base=0x20000000',
     '--entry=RunExternalPeripheralTests','-o',target])
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
names = ['SPI init and JEDEC identification', 'cross-page split, SPI receive and CS release', 'bounds/null/erase alignment', 'transport failure and BUSY timeout', 'unknown JEDEC disables all writes', 'ADC and potentiometer low endpoint', 'filtered high endpoint and mode selection', 'unchanged knob and ADC failure/off mode', 'time/pot command formatting and unknown command', 'bounded log requests and busy rejection', 'request matching and command timeout', 'queue saturation and retry']
names += ['transient read timeout resets SPI and preserves its diagnosis',
          'partial multi-chunk receive restarts the entire read buffer',
          'persistent transport failure stops at the configured read limit',
          'read command timeout recovers without modifying Flash',
          'status transfer timeout recovers with a fresh CS transaction',
          'SPI recovery failure stops further attempts and releases CS',
          'persistently BUSY Flash is not retried as a transport failure',
          'program and erase failures are never automatically replayed',
          'OLED-length preemption fits the new transfer timeout budget',
          'invalid read requests do not reach the SPI transport']
for index, name in enumerate(names): print(('FAIL' if failures & (1 << index) else 'PASS') + ': ' + name)
print('External peripheral/command failure mask:', hex(failures))
raise SystemExit(1 if failures else 0)
