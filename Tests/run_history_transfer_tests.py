"""Exercise actual command/storage/history C together with NOR and queue models."""
from pathlib import Path
import argparse
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--deps', type=Path)
args = parser.parse_args()
if args.deps:
    sys.path.insert(0, str(args.deps))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_PC

root = Path(__file__).resolve().parent.parent
build = root / 'Tests/build'
build.mkdir(exist_ok=True)
cc = Path('D:/Keil/core/ARM/ARMCC/ARMCC/bin/armcc.exe')


def run(command):
    result = subprocess.run([str(value) for value in command], capture_output=True, text=True, errors='replace')
    if result.returncode:
        print(result.stdout, result.stderr)
        result.check_returncode()


objects = []
for relative in ['Tests/test_history_transfer.c', 'Core/Src/project_commands.c',
                 'Core/Src/storage_service.c', 'Core/Src/history_transfer.c', 'Core/Src/history_buffer.c', 'Core/Src/history_store.c',
                 'Core/Src/serial_command.c', 'Core/Src/serial_report.c',
                 'Core/Src/storage_codec.c', 'Core/Src/settings_store.c',
                 'Core/Src/time_calendar.c', 'Core/Src/env_alarm.c']:
    source = root / relative
    obj = build / ('history_transfer_' + source.stem + '.o')
    run([cc, '--cpu=Cortex-M3', '--thumb', '--c99', '-O2', '--library_type=microlib', '--split_sections',
         '-I' + str(root / 'Tests/external_stubs'), '-I' + str(root / 'Core/Inc'), '-c', source, '-o', obj])
    objects.append(obj)

target = build / 'history_transfer_service_tests.axf'
run([cc.with_name('armlink.exe'), *objects, '--cpu=Cortex-M3', '--library_type=microlib',
     '--libpath=' + str(cc.parent.parent / 'lib'), '--ro_base=0x10000', '--rw_base=0x20000000',
     '--entry=RunHistoryTransferTests', '-o', target])
machine = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
machine.mem_map(0x10000, 0x30000)
machine.mem_map(0x20000000, 0x20000)
with target.open('rb') as stream:
    elf = ELFFile(stream)
    entry = elf.header.e_entry
    for segment in elf.iter_segments():
        if segment.header.p_type == 'PT_LOAD' and segment.header.p_filesz:
            machine.mem_write(segment.header.p_vaddr, segment.data())
machine.reg_write(UC_ARM_REG_SP, 0x2001F000)
machine.reg_write(UC_ARM_REG_LR, 0x3FFF1)
machine.emu_start(entry | 1, 0x3FFF0, count=200000000)
assert machine.reg_read(UC_ARM_REG_PC) == 0x3FFF0, 'Test entry did not return'
failures = machine.reg_read(UC_ARM_REG_R0)
names = [
    'empty export has header and successful terminator',
    'full export exceeds old 20-record limit',
    'CSV framing and queue sends are nonblocking',
    'first page returns exclusive next cursor',
    'new writes between pages cause no duplicate rows',
    'last page and empty cursor finish with next=0',
    'cursor/count parser rejects overflow and malformed values',
    'FIFO writes continue when export output queue is full',
    'snapshot excludes records committed after export began',
    'other commands cannot interleave with CSV rows',
    'cancel terminates and deferred notices follow CSV_END',
    'cancel works after output queue saturation',
    'corrupt slots are skipped and counted',
    'read I/O failure cannot report successful completion',
    'unready history emits an explicit failure terminator',
    'sector reuse invalidates a snapshot rather than mixing records',
    'each iterator step respects its SPI read budget',
    'CSV preserves inferred Beijing dates and hides anchor metadata',
    'internal heartbeat extends timeout without serial output',
    'idle timeout cancels backend work',
    'TX failure cancels transfer and reports incomplete output',
    'duplicate backend begin cannot replace an active snapshot',
    'exactly full last page reports end of history',
    'foreign reply cannot close or contaminate a transfer',
    'pending control ACK prevents opening a CSV stream',
    'real serial parser gates get/set during mixed-case export',
    'real parser defers overflow/RX-loss errors until transfer end',
    'real parser accepts cancellation without interleaved error text',
]
for index, name in enumerate(names):
    print(('FAIL' if failures & (1 << index) else 'PASS') + ': ' + name)
print('History transfer failure mask:', hex(failures))
raise SystemExit(1 if failures else 0)
