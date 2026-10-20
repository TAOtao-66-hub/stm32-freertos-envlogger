from pathlib import Path
import subprocess
import sys
import argparse
parser=argparse.ArgumentParser()
parser.add_argument('--deps',type=Path)
args=parser.parse_args()
if args.deps: sys.path.insert(0,str(args.deps))
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_R0,UC_ARM_REG_PC
root=Path(__file__).resolve().parent.parent
cc=Path('D:/Keil/core/ARM/ARMCC/ARMCC/bin/armcc.exe')
kernel=root/'Middlewares/Third_Party/FreeRTOS/Source'
paths=[root/'Tests/test_rtos_allocation.c',root/'Core/Src/history_buffer.c',root/'Core/Src/rtos_diagnostics.c',kernel/'tasks.c',kernel/'queue.c',kernel/'list.c',kernel/'portable/MemMang/heap_4.c',kernel/'portable/RVDS/ARM_CM3/port.c']
includes=[root/'Core/Inc',kernel/'include',kernel/'portable/RVDS/ARM_CM3',root/'Drivers/STM32F1xx_HAL_Driver/Inc',root/'Drivers/CMSIS/Include',root/'Drivers/CMSIS/Device/ST/STM32F1xx/Include']
def run(command):
    result=subprocess.run([str(x) for x in command],capture_output=True,text=True,errors='replace')
    if result.returncode: print(result.stdout,result.stderr);result.check_returncode()
objects=[]
for source in paths:
    obj=root/'Tests/build'/('allocation_'+source.stem+'.o')
    run([cc,'--cpu=Cortex-M3','--thumb','--c99','--split_sections','--library_type=microlib','-O2','-DSTM32F103xB','-DUSE_HAL_DRIVER',*['-I'+str(x) for x in includes],'-c',source,'-o',obj]);objects.append(obj)
target=root/'Tests/build/allocation_tests.axf'
run([cc.with_name('armlink.exe'),*objects,'--cpu=Cortex-M3','--library_type=microlib','--libpath='+str(cc.parent.parent/'lib'),'--ro_base=0x10000','--rw_base=0x20000000','--entry=RunAllocationTests','-o',target])
machine=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS)
machine.mem_map(0x10000,0x30000);machine.mem_map(0x20000000,0x20000);machine.mem_map(0xE000E000,0x2000)
with target.open('rb') as stream:
    elf=ELFFile(stream);entry=elf.header.e_entry
    for segment in elf.iter_segments():
        if segment.header.p_type=='PT_LOAD' and segment.header.p_filesz: machine.mem_write(segment.header.p_vaddr,segment.data())
machine.reg_write(UC_ARM_REG_SP,0x2001F000);machine.reg_write(UC_ARM_REG_LR,0x3FFF1)
machine.emu_start(entry|1,0x3FFF0,count=10000000)
assert machine.reg_read(UC_ARM_REG_PC)==0x3FFF0,'Allocation probe did not return'
value=machine.reg_read(UC_ARM_REG_R0)
assert value>256,f'Allocation/diagnostic check failed (stage={value}) or heap margin too small'
print('FreeRTOS actual heap remaining after 11 queues + 4 tasks:',value,'bytes')
print('PASS: actual producer and real FreeRTOS FIFO preserve order, copies and full-queue counts')
print('PASS: actual kernel high-water marks, heap minimum, diagnostic units and TX-failure propagation')
