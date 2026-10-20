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
for relative in ['Tests/test_time_history_service.c', 'Core/Src/project_commands.c',
                 'Core/Src/storage_service.c', 'Core/Src/history_transfer.c', 'Core/Src/history_buffer.c', 'Core/Src/history_store.c',
                 'Core/Src/storage_codec.c', 'Core/Src/settings_store.c',
                 'Core/Src/time_calendar.c', 'Core/Src/env_alarm.c']:
    source = root / relative
    obj = build / ('time_history_' + source.stem + '.o')
    run([cc, '--cpu=Cortex-M3', '--thumb', '--c99', '-O2', '--library_type=microlib', '--split_sections',
         '-I' + str(root / 'Tests/external_stubs'), '-I' + str(root / 'Core/Inc'), '-c', source, '-o', obj])
    objects.append(obj)

target = build / 'time_history_service_tests.axf'
run([cc.with_name('armlink.exe'), *objects, '--cpu=Cortex-M3', '--library_type=microlib',
     '--libpath=' + str(cc.parent.parent / 'lib'), '--ro_base=0x10000', '--rw_base=0x20000000',
     '--entry=RunTimeHistoryServiceTests', '-o', target])
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
names = ['deferred calibration ACK follows actual committed anchor',
         'serial history prints the inferred Beijing date and source',
         'reset leaves RTC uncalibrated but preserves old inferred dates',
         'new-session samples remain unknown beside old calibrated sessions',
         'RTC failure never commits an anchor or sends success',
         'invalid calendar date never enters the storage queue',
         'anchor write failure reports clock-set/not-saved and survives restart',
         'post-calibration sample retains its RTC date/source']
names += ['healthy status includes FIFO counters with correct final marker',
          'foreign header diagnostic reaches serial without truncation or erase',
          'read failure serial diagnosis differs from foreign content']
names += ['log failure includes the last SPI result and duration',
          'flash info exposes recovered transport failures',
          'maximum diagnostic counters fit the existing reply buffer']
names += ['paused storage preserves FIFO order and original record times',
          'full FIFO drops new records without overwriting or blocking',
          'serial FIFO counters distinguish accepted dropped and written records',
          'RTC timestamp is captured before delayed Flash write',
          'queued uncalibrated records precede their persisted time anchor',
          'uncertain write stops dequeue and counts the unconfirmed record',
          'unready history preserves the remaining queued records',
          'delayed producer counts missed periods without fabricated backfill',
          'record scheduling survives millisecond tick wrap',
          'missing or invalid sensor produces an explicit fault record',
          'failed backlog flush cannot report a stored calibration anchor']
for index, name in enumerate(names):
    print(('FAIL' if failures & (1 << index) else 'PASS') + ': ' + name)
print('Time/history service failure mask:', hex(failures))
raise SystemExit(1 if failures else 0)
