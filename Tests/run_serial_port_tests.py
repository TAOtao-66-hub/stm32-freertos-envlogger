"""Compile the real USART1 driver C code and run it on an emulated Cortex-M3."""
from pathlib import Path
import argparse
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--deps', type=Path,
                    help='Optional folder containing unicorn and pyelftools.')
parser.add_argument('--armcc', type=Path,
                    default=Path('D:/Keil/core/ARM/ARMCC/ARMCC/bin/armcc.exe'))
args = parser.parse_args()
if args.deps is not None:
    sys.path.insert(0, str(args.deps))

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_PC

project = Path(__file__).resolve().parent.parent
base = project / 'Tests/build'
base.mkdir(parents=True, exist_ok=True)
compiler = args.armcc
objects = []


def run(command):
    result = subprocess.run(command, capture_output=True, text=True, errors='replace')
    if result.returncode:
        print(result.stdout, result.stderr)
        result.check_returncode()


for relative in ('Tests/test_serial_port.c', 'Core/Src/serial_port.c', 'Core/Src/serial_report.c'):
    source = project / relative
    obj = base / ('serial_test_' + source.stem + '.o')
    run([str(compiler), '--cpu=Cortex-M3', '--thumb', '--c99', '-O2',
         '--library_type=microlib', '--split_sections',
         '-I' + str(project / 'Tests/serial_stubs'),
         '-I' + str(project / 'Core/Inc'), '-c', str(source), '-o', str(obj)])
    objects.append(str(obj))

target = base / 'serial_port_tests.axf'
run([str(compiler.with_name('armlink.exe')), *objects, '--cpu=Cortex-M3',
     '--library_type=microlib', '--libpath=' + str(compiler.parent.parent / 'lib'),
     '--ro_base=0x10000', '--rw_base=0x20000000', '--entry=RunSerialPortTests',
     '-o', str(target)])

machine = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
machine.mem_map(0x10000, 0x20000)
machine.mem_map(0x20000000, 0x10000)
with target.open('rb') as stream:
    elf = ELFFile(stream)
    entry = elf.header.e_entry
    for segment in elf.iter_segments():
        if segment.header.p_type == 'PT_LOAD' and segment.header.p_filesz:
            machine.mem_write(segment.header.p_vaddr, segment.data())
machine.reg_write(UC_ARM_REG_SP, 0x2000F000)
machine.reg_write(UC_ARM_REG_LR, 0x2FFF1)
machine.emu_start(entry | 1, 0x2FFF0, count=5000000)
assert machine.reg_read(UC_ARM_REG_PC) == 0x2FFF0, 'Test entry did not return.'
failures = machine.reg_read(UC_ARM_REG_R0)
names = ('initialization order, retry, UART settings and idempotence',
         'bounded TX, input validation and HAL timeout/busy propagation',
         'RX startup retry, byte copies, queue-full accounting and ISR wakeup',
         'failed rearm and failed recovery retry without losing RX permanently',
         'UART error recovery and unrelated UART callback isolation',
         'IRQ dispatch and coherent diagnostic snapshot',
         'sample report decimal formatting and CRLF without a NUL byte',
         'failed sample hides retained previous good readings',
         'report null input, zero, tenths and uint16 boundaries',
         'report TX timeout propagation and next fresh sample recovery')
for index, name in enumerate(names):
    print(('FAIL' if failures & (1 << index) else 'PASS') + ': ' + name)
print('Cortex-M3 serial port test failure mask:', hex(failures))
if failures:
    raise SystemExit(1)
