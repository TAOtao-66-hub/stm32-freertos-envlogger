from pathlib import Path
import subprocess
import sys

import argparse

parser = argparse.ArgumentParser(description='Run alarm/driver C tests on an emulated Cortex-M3.')
parser.add_argument('--deps', type=Path, help='Optional folder containing unicorn and pyelftools.')
parser.add_argument('--armcc', type=Path,
                    default=Path('D:/Keil/core/ARM/ARMCC/ARMCC/bin/armcc.exe'))
args = parser.parse_args()
project = Path(__file__).resolve().parent.parent
base = project / 'Tests/build'
base.mkdir(parents=True, exist_ok=True)
if args.deps is not None:
    sys.path.insert(0, str(args.deps))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_PC

compiler = args.armcc
objects = []

def run(command):
    result = subprocess.run(command, capture_output=True, text=True, errors='replace')
    if result.returncode:
        print(result.stdout, result.stderr)
        result.check_returncode()

for relative in ('Tests/test_alarm_drivers.c', 'Core/Src/env_alarm.c',
                 'Core/Src/button.c', 'Core/Src/buzzer.c', 'Core/Src/led.c'):
    source = project / relative
    obj = base / ('alarm_test_' + source.stem + '.o')
    run([str(compiler), '--cpu=Cortex-M3', '--thumb', '--c99', '-O2',
         '--split_sections', '-I' + str(project / 'Tests/stubs'),
         '-I' + str(project / 'Core/Inc'), '-c', str(source), '-o', str(obj)])
    objects.append(str(obj))

target = base / 'alarm_driver_tests.axf'
run([str(compiler.with_name('armlink.exe')), *objects, '--ro_base=0x10000',
     '--rw_base=0x20000000', '--entry=RunAlarmDriverTests', '--no_scanlib', '-o', str(target)])

machine = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
machine.mem_map(0x10000, 0x10000)
machine.mem_map(0x20000000, 0x10000)
with target.open('rb') as stream:
    elf = ELFFile(stream)
    entry = elf.header.e_entry
    for segment in elf.iter_segments():
        if segment.header.p_type == 'PT_LOAD' and segment.header.p_filesz:
            machine.mem_write(segment.header.p_vaddr, segment.data())
machine.reg_write(UC_ARM_REG_SP, 0x2000F000)
machine.reg_write(UC_ARM_REG_LR, 0x1FFF1)
machine.emu_start(entry | 1, 0x1FFF0, count=5000000)
assert machine.reg_read(UC_ARM_REG_PC) == 0x1FFF0, 'Test entry did not return.'
failures = machine.reg_read(UC_ARM_REG_R0)
names = ('threshold boundaries and independent hysteresis',
         'mute persistence, recovery and repeat alarm',
         'new cause re-arms sound; repeated fault stays muted',
         'invalid readings and display status text',
         'press/release bounce and no repeat while held',
         'button held during startup', '32-bit millisecond tick rollover',
         'active-high LED and active-low buzzer polarity',
         'runtime temperature re-evaluation, hysteresis and mute recovery',
         'runtime temperature/humidity inclusive boundaries and minimum clear value',
         'rejected requests preserve configuration, alarm flags and mute',
         'setting without sample, invalid sample retention and default restoration',
         'compound commit and rejection of either field preserves entire state',
         'combined configuration reevaluates once with hysteresis and mute intact',
         'combined inclusive boundaries, no sample and invalid sample rules')
for index, name in enumerate(names):
    print(('FAIL' if failures & (1 << index) else 'PASS') + ': ' + name)
print('Cortex-M3 alarm/driver test failure mask:', hex(failures))
if failures:
    raise SystemExit(1)
