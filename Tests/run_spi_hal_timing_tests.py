"""Exercise the vendor HAL SPI timeout checks with injected task preemption."""
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
objects = []


def run(command):
    result = subprocess.run([str(value) for value in command], capture_output=True, text=True, errors='replace')
    if result.returncode:
        print(result.stdout, result.stderr)
        result.check_returncode()


for relative in ['Tests/test_spi_hal_timing.c', 'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_spi.c']:
    obj = build / ('spi_timing_' + Path(relative).stem + '.o')
    includes = ['Core/Inc', 'Drivers/STM32F1xx_HAL_Driver/Inc',
                'Drivers/CMSIS/Device/ST/STM32F1xx/Include', 'Drivers/CMSIS/Include']
    run([cc, '--cpu=Cortex-M3', '--thumb', '--c99', '-O2', '--library_type=microlib', '--split_sections',
         '-DSTM32F103xB', '-DUSE_HAL_DRIVER', *['-I' + str(root / path) for path in includes],
         '-c', root / relative, '-o', obj])
    objects.append(obj)
target = build / 'spi_hal_timing_tests.axf'
run([cc.with_name('armlink.exe'), *objects, '--cpu=Cortex-M3', '--library_type=microlib',
     '--libpath=' + str(cc.parent.parent / 'lib'), '--ro_base=0x10000', '--rw_base=0x20000000',
     '--entry=RunSpiHalTimingTests', '-o', target])
machine = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
machine.mem_map(0x10000, 0x30000)
machine.mem_map(0x20000000, 0x20000)
machine.mem_map(0x40013000, 0x1000)
with target.open('rb') as stream:
    elf = ELFFile(stream)
    entry = elf.header.e_entry
    for segment in elf.iter_segments():
        if segment.header.p_type == 'PT_LOAD' and segment.header.p_filesz:
            machine.mem_write(segment.header.p_vaddr, segment.data())
machine.reg_write(UC_ARM_REG_SP, 0x2001F000)
machine.reg_write(UC_ARM_REG_LR, 0x3FFF1)
machine.emu_start(entry | 1, 0x3FFF0, count=2000000)
assert machine.reg_read(UC_ARM_REG_PC) == 0x3FFF0, 'Test entry did not return'
failures = machine.reg_read(UC_ARM_REG_R0)
names = ['real HAL reproduces the old 20ms timeout after 180ms preemption',
         'real HAL completes the same receive with the new timeout',
         'preemption timeout arithmetic survives HAL tick wrap',
         'stuck SPI flags still produce a bounded timeout']
for index, name in enumerate(names):
    print(('FAIL' if failures & (1 << index) else 'PASS') + ': ' + name)
print('Vendor HAL SPI timing failure mask:', hex(failures))
raise SystemExit(1 if failures else 0)
