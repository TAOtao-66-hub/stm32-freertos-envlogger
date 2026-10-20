"""Compile the real command parser/report C code and run it on an emulated Cortex-M3."""
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


for relative in ('Tests/test_serial_command.c', 'Core/Src/serial_command.c', 'Core/Src/serial_report.c', 'Core/Src/env_alarm.c'):
    source = project / relative
    obj = base / ('command_test_' + source.stem + '.o')
    run([str(compiler), '--cpu=Cortex-M3', '--thumb', '--c99', '-O2',
         '--library_type=microlib', '--split_sections',
         '-I' + str(project / 'Tests/serial_stubs'),
         '-I' + str(project / 'Core/Inc'), '-c', str(source), '-o', str(obj)])
    objects.append(str(obj))

target = base / 'serial_command_tests.axf'
run([str(compiler.with_name('armlink.exe')), *objects, '--cpu=Cortex-M3',
     '--library_type=microlib', '--libpath=' + str(compiler.parent.parent / 'lib'),
     '--ro_base=0x10000', '--rw_base=0x20000000', '--entry=RunSerialCommandTests',
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
names = ('line completion and GET before the first acquisition',
         'GET prefix, coherent value copy and invalid-reading placeholders',
         'HELP with CR, LF, CRLF and ignored empty lines',
         'edge whitespace, case-insensitive matching and unknown suffixes',
         'overlong line discarded through terminator and clean next command',
         'exact line-size boundary and whitespace-only line',
         'embedded NUL/non-ASCII rejection without suffix execution',
         'RX loss resynchronization keeps cached data and rejects partial lines',
         'sample freshness boundary and 32-bit millisecond rollover',
         'TX timeout/busy propagation and parser recovery for the next command',
         'consecutive commands, null sample update and initialization reset',
         'plain get at idle boundary and single-response behavior',
         'plain help, mixed case and delayed CRLF without duplicate replies',
         'fragmented command extends the idle deadline',
         'whole-command matching and separate idle-framed requests',
         'idle framing recovers from overflow, invalid bytes and RX loss',
         'idle timer handles 32-bit millisecond rollover',
         'idle-triggered TX timeout does not poison the next command',
         'SET idle completion, real owner application before ACK and no duplicate ACK',
         'malformed SET values never enter the request queue',
         'range rejection uses actual alarm response and leaves defaults unchanged',
         'one outstanding SET, matching response IDs/kinds and HELP remains usable',
         'missing/full queues and failed ACK allow recovery',
         'mixed-case humidity SET, integer scaling and uint16 numeric boundary',
         'compound SET creates one request and replies once after both fields applied',
         'reverse field order, mixed case and combined numeric boundaries',
         'duplicate, incomplete and malformed compound SET never enters queue',
         'one invalid compound value rejects all changes and preserves mute/flags',
         'compound pending/result matching and later single setting isolation',
         'SAVE queues one request and waits for stored/unchanged owner results',
         'SAVE errors, extra arguments and failed reply allow retry')
names += ('pot mode request/owner ACK and remote-control conflict replies',)
for index, name in enumerate(names):
    print(('FAIL' if failures & (1 << index) else 'PASS') + ': ' + name)
print('Cortex-M3 serial command test failure mask:', hex(failures))
if failures:
    raise SystemExit(1)
