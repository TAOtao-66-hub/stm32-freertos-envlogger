"""Check FIFO code/.ioc export and rejection of unsupported or unsafe edits."""
from pathlib import Path
import argparse
import importlib.util
import shutil
import tempfile
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--work', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('project_sync', root / 'Tools/project_sync.py')
sync = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sync)
work = args.work.resolve()
work.mkdir(parents=True, exist_ok=True)

with tempfile.TemporaryDirectory(prefix='history_fifo_sync_', dir=work) as directory:
    probe = Path(directory).resolve()
    assert probe.is_relative_to(work)  # Temporary cleanup remains inside task work/.
    shutil.copytree(root / 'Core', probe / 'Core')
    project_relative = Path('MDK-ARM/F103_FreeRTOS_CubeMX.uvprojx')
    entries = ET.parse(root / project_relative).findall('./Targets/Target/Groups/Group/Files/File/FilePath')
    for entry in entries:
        source = (root / 'MDK-ARM' / entry.text.replace('\\', '/')).resolve()
        destination = probe / source.relative_to(root)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    (probe / 'MDK-ARM').mkdir(exist_ok=True)
    shutil.copy2(root / project_relative, probe / project_relative)
    ioc_path = probe / 'F103_FreeRTOS_CubeMX.ioc'
    shutil.copy2(root / ioc_path.name, ioc_path)
    sync.check(probe)
    print('PASS: actual FIFO length and Keil registration match the .ioc')

    config_path = probe / 'Core/Inc/project_config.h'
    config = sync.read(config_path)
    sync.write(config_path, sync.set_macro(config, 'HISTORY_RECORD_QUEUE_LENGTH', 4))
    sync.export(probe)
    assert sync.settings(probe)[1]['HISTORY_RECORD_QUEUE_LENGTH'] == 4
    sync.write(config_path, config)
    sync.export(probe)
    assert sync.settings(probe)[1]['HISTORY_RECORD_QUEUE_LENGTH'] == 8
    print('PASS: native FIFO length exports to .ioc and imports as the same parameter')

    ioc = sync.read(ioc_path)
    sync.write(ioc_path, ioc.replace('storageSampleQueue,8,', 'storageSampleQueue,17,'))
    try:
        sync.settings(probe)
    except ValueError as error:
        assert 'History FIFO exceeds' in str(error)
    else:
        raise AssertionError('Unbudgeted queue size was accepted')
    sync.write(ioc_path, ioc)
    print('PASS: .ioc FIFO lengths above the checked RAM bound are rejected')

    main_path = probe / 'Core/Src/main.c'
    main = sync.read(main_path)
    sync.write(main_path, main.replace('HistoryBuffer_Poll(HAL_GetTick(),',
                                      'xQueueOverwrite(s_storage_sample_queue, &sample); HistoryBuffer_Poll(HAL_GetTick(),'))
    try:
        sync.check(probe)
    except ValueError as error:
        assert 'must never use xQueueOverwrite' in str(error)
    else:
        raise AssertionError('FIFO overwrite was accepted')
    print('PASS: prebuild validation rejects overwrite use on the history FIFO')
    sync.write(main_path, main)

    # Diagnostic APIs are project requirements and must remain synchronized.
    kernel_path = probe / 'Core/Inc/FreeRTOSConfig.h'
    kernel_config = sync.read(kernel_path)
    sync.write(kernel_path, sync.set_macro(kernel_config, 'INCLUDE_uxTaskGetStackHighWaterMark', 0))
    try:
        sync.check(probe)
    except ValueError as error:
        assert '.ioc/kernel mismatch' in str(error)
    else:
        raise AssertionError('Diagnostic API drift was accepted')
    sync.write(kernel_path, kernel_config)
    print('PASS: diagnostic API flag drift between code and .ioc is rejected')

    sync.write(main_path, main.replace('RtosDiagnostics_Bind(', 'RemovedDiagnostics_Bind('))
    try:
        sync.check(probe)
    except ValueError as error:
        assert 'Diagnostic handles' in str(error)
    else:
        raise AssertionError('Missing task-handle binding was accepted')
    print('PASS: prebuild validation requires diagnostic handle binding after task creation')
