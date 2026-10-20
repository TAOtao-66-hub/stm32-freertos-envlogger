"""Keep this project's CubeMX settings and native FreeRTOS layout consistent.

CubeMX 6.11.1 generates CMSIS-V1 scaffolding for this MCU. The before/after
hooks preserve the application, import the generated clock configuration and
apply the supported .ioc settings to the native implementation. Unsupported
peripheral or pin changes fail explicitly; they are never silently discarded.
"""
from __future__ import annotations

import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import shutil
import sys
import xml.etree.ElementTree as ET

PROJECT_NAME = "F103_FreeRTOS_CubeMX"
TASKS = {
    "envMonitorTask": ("StartEnvMonitorTask", "ENV_MONITOR_TASK"),
    "alarmTask": ("StartAlarmTask", "ALARM_TASK"),
    "serialTask": ("StartSerialTask", "SERIAL_TASK"),
    "storageTask": ("StartStorageTask", "STORAGE_TASK"),
}
QUEUES = {
    "envSampleQueue": (1, "EnvAlarm_Sample"),
    "alarmStateQueue": (1, "EnvAlarm_State"),
    "serialRxQueue": (None, "uint8_t"),
    "serialSampleQueue": (1, "EnvAlarm_Sample"),
    "alarmRequestQueue": (1, "EnvAlarm_Request"),
    "alarmResponseQueue": (1, "EnvAlarm_Response"),
    "storageRequestQueue": (None, "Storage_Request"),
    "storageSampleQueue": (None, "History_Sample"),
    "storageBootQueue": (1, "Storage_Boot"),
    "storageSavedQueue": (1, "Storage_SaveReply"),
    "storageTextQueue": (None, "Storage_TextReply"),
}
SIGNALS = {
    "PA0": "ADCx_IN0", "PA5": "SPI1_SCK", "PA6": "SPI1_MISO",
    "PA7": "SPI1_MOSI", "PB0": "GPIO_Output",
    "PA2": "GPIO_Output", "PA3": "GPIO_Input", "PA4": "GPIO_Output",
    "PA9": "USART1_TX", "PA10": "USART1_RX", "PB6": "GPIO_Output",
    "PB7": "GPIO_Output", "PB10": "GPIO_Input",
    "PA13": "SYS_JTMS-SWDIO", "PA14": "SYS_JTCK-SWCLK",
}
KERNEL_KEYS = (
    "configUSE_PREEMPTION", "configTOTAL_HEAP_SIZE", "configUSE_MUTEXES",
    "configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY",
    "configLIBRARY_LOWEST_INTERRUPT_PRIORITY", "configCHECK_FOR_STACK_OVERFLOW",
    "configMAX_PRIORITIES", "configTICK_RATE_HZ",
    "configSUPPORT_STATIC_ALLOCATION", "configSUPPORT_DYNAMIC_ALLOCATION",
    "INCLUDE_uxTaskGetStackHighWaterMark", "INCLUDE_xTaskGetIdleTaskHandle",
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read(path):
    return path.read_text(encoding="utf-8-sig")


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8"))


def properties(text):
    return dict(line.split("=", 1) for line in text.splitlines()
                if "=" in line and not line.startswith("#"))


def rows(value):
    return {row.split(",")[0]: row.split(",") for row in value.split(";") if row}


def macro_number(text, name):
    match = re.search(r"^#define\s+" + re.escape(name) + r"\s+(.+)$", text, re.M)
    require(match is not None, "Missing macro: " + name)
    numbers = re.findall(r"(?<![A-Za-z_])\b(?:0[xX][0-9A-Fa-f]+|[0-9]+)[UuLl]*\b", match[1].split("/*")[0])
    require(len(numbers) == 1, "Expected one numeric value: " + name)
    token = numbers[0].rstrip("UuLl")
    return int(token, 16 if token.lower().startswith("0x") else 10)


def set_macro(text, name, value, suffix="U"):
    # Keep the original comment and alignment; normalize only the numeric token.
    pattern = r"(^#define\s+" + re.escape(name) + r"\s+)([^\n/]+)"
    match = re.search(pattern, text, re.M)
    require(match is not None, "Missing macro to update: " + name)
    old = match[2]
    updated = re.sub(r"(?<![A-Za-z_])\b\d+[UuLl]*\b", str(value) + suffix, old, count=1)
    return text[:match.start(2)] + updated + text[match.end(2):]


def function(text, name):
    match = re.search(r"\bvoid\s+" + name + r"\s*\([^;]*?\)\s*\{", text)
    require(match is not None, "Missing function definition: " + name)
    depth = 1
    pos = match.end()
    while depth and pos < len(text):
        depth += (text[pos] == "{") - (text[pos] == "}")
        pos += 1
    require(depth == 0, "Unbalanced function: " + name)
    return match.start(), pos, text[match.start():pos]


def check_gpio(root):
    """Check the actual initialization statements, including the staged TX mode."""
    gpio = read(root / "Core/Src/gpio.c")
    header = read(root / "Core/Inc/gpio.h")
    aliases = dict(re.findall(r"^#define\s+(\w+)\s+(\w+)\s*$", header, re.M))

    def resolve(token):
        return aliases.get(token, token)

    def pins(expression):
        result = []
        for token in expression.split("|"):
            name = resolve(token.strip())
            require(re.fullmatch(r"GPIO_PIN_\d+", name), "Unsupported GPIO pin expression: " + expression)
            result.append(int(name.rsplit("_", 1)[1]))
        return result

    configurations = {}
    levels = {}
    bodies = [function(gpio, name)[2] for name in ("MX_GPIO_Init", "MX_GPIO_USART1_TX_Init")]
    for body in bodies:
        fields = {"Pull": "GPIO_NOPULL"}
        events = re.finditer(
            r"GPIO_InitStruct\.(Pin|Mode|Pull|Speed)\s*=\s*([^;]+);"
            r"|HAL_GPIO_Init\(\s*(\w+)\s*,\s*&GPIO_InitStruct\s*\);", body)
        for event in events:
            if event[1]:
                fields[event[1]] = event[2].strip()
            else:
                port = resolve(event[3]).replace("GPIO", "P")
                for number in pins(fields["Pin"]):
                    configurations[port + str(number)] = dict(fields)
        for event in re.finditer(r"HAL_GPIO_WritePin\(\s*(\w+)\s*,\s*([^,]+),\s*(GPIO_PIN_\w+)\s*\);", body):
            port = resolve(event[1]).replace("GPIO", "P")
            for number in pins(event[2]):
                levels[port + str(number)] = event[3]
    expected = {
        "PA0": ("GPIO_MODE_ANALOG", "GPIO_NOPULL", None),
        "PA5": ("GPIO_MODE_AF_PP", "GPIO_NOPULL", "GPIO_SPEED_FREQ_HIGH"),
        "PA6": ("GPIO_MODE_INPUT", "GPIO_NOPULL", None),
        "PA7": ("GPIO_MODE_AF_PP", "GPIO_NOPULL", "GPIO_SPEED_FREQ_HIGH"),
        "PB0": ("GPIO_MODE_OUTPUT_PP", "GPIO_NOPULL", "GPIO_SPEED_FREQ_LOW"),
        "PA2": ("GPIO_MODE_OUTPUT_PP", "GPIO_NOPULL", "GPIO_SPEED_FREQ_LOW"),
        "PA3": ("GPIO_MODE_INPUT", "GPIO_PULLUP", None),
        "PA4": ("GPIO_MODE_OUTPUT_PP", "GPIO_NOPULL", "GPIO_SPEED_FREQ_LOW"),
        "PA9": ("GPIO_MODE_AF_PP", "GPIO_NOPULL", "GPIO_SPEED_FREQ_HIGH"),
        "PA10": ("GPIO_MODE_INPUT", "GPIO_PULLUP", None),
        "PB6": ("GPIO_MODE_OUTPUT_OD", "GPIO_NOPULL", "GPIO_SPEED_FREQ_LOW"),
        "PB7": ("GPIO_MODE_OUTPUT_OD", "GPIO_NOPULL", "GPIO_SPEED_FREQ_LOW"),
        "PB10": ("GPIO_MODE_INPUT", "GPIO_PULLUP", None),
    }
    require(configurations.keys() == expected.keys(), "Unexpected/missing GPIO initialization")
    for pin, (mode, pull, speed) in expected.items():
        actual = configurations[pin]
        require(actual["Mode"] == mode and actual["Pull"] == pull
                and (speed is None or actual["Speed"] == speed), "GPIO code/.ioc mismatch: " + pin)
    for pin, state in (("PB0", "GPIO_PIN_SET"), ("PA2", "GPIO_PIN_SET"), ("PA4", "GPIO_PIN_RESET"),
                       ("PA9", "GPIO_PIN_SET"), ("PB6", "GPIO_PIN_SET"), ("PB7", "GPIO_PIN_SET")):
        require(levels.get(pin) == state, "GPIO initial level mismatch: " + pin)


def settings(root):
    ioc = properties(read(root / (PROJECT_NAME + ".ioc")))
    require(ioc.get("Mcu.CPN") == "STM32F103C8T6", "Unsupported MCU")
    ips = {value for key, value in ioc.items() if re.fullmatch(r"Mcu.IP\d+", key)}
    require(ips == {"ADC1", "FREERTOS", "NVIC", "RCC", "RTC", "SPI1", "SYS", "USART1"},
            "Peripheral set changed. Extend the native adapter before regeneration.")
    actual_signals = {key.split(".")[0].split("-")[0]: value for key, value in ioc.items()
                      if re.fullmatch(r"P[A-G]\d+(?:-[A-Za-z0-9_]+)?\.Signal", key)}
    require(actual_signals == SIGNALS, "Pin assignment changed; update drivers and the adapter first.")
    for key, value in {
        "SPI1.VirtualType": "VM_MASTER", "SPI1.Direction": "SPI_DIRECTION_2LINES",
        "SPI1.BaudRatePrescaler": "SPI_BAUDRATEPRESCALER_64", "SPI1.NSS": "SPI_NSS_SOFT",
        "SPI1.CLKPolarity": "SPI_POLARITY_LOW", "SPI1.CLKPhase": "SPI_PHASE_1EDGE",
        "SPI1.DataSize": "SPI_DATASIZE_8BIT", "SPI1.FirstBit": "SPI_FIRSTBIT_MSB",
        r"ADC1.SamplingTime-0\#ChannelRegularConversion": "ADC_SAMPLETIME_239CYCLES_5",
        "RCC.ADCPresc": "RCC_ADCPCLK2_DIV8", "RCC.RTCClockSelection": "RCC_RTCCLKSOURCE_LSI",
        "RTC.AsynchPrediv": "RTC_AUTO_1_SECOND",
        r"ADC1.Channel-0\#ChannelRegularConversion": "ADC_CHANNEL_0",
        "ADC1.NbrOfConversion": "1", "ADC1.ContinuousConvMode": "DISABLE",
        "ADC1.ExternalTrigConv": "ADC_SOFTWARE_START", "ADC1.DataAlign": "ADC_DATAALIGN_RIGHT",
        "RCC.ADCFreqValue": "8000000",
        "SH.ADCx_IN0.0": "ADC1_IN0,IN0",
    }.items():
        require(ioc.get(key) == value, "Native peripheral setting mismatch: " + key)
    for pin in ("PA3", "PA10", "PB10"):
        require(ioc.get(pin + ".GPIO_PuPd") == "GPIO_PULLUP", pin + " requires pull-up")
    for pin, state in (("PB0", "GPIO_PIN_SET"), ("PA2", "GPIO_PIN_SET"), ("PA4", "GPIO_PIN_RESET"),
                       ("PB6", "GPIO_PIN_SET"), ("PB7", "GPIO_PIN_SET")):
        require(ioc.get(pin + ".PinState", "GPIO_PIN_RESET") == state,
                pin + " startup level conflicts with the current circuit")
    for pin in ("PB6", "PB7"):
        require(ioc.get(pin + ".GPIO_ModeDefaultOutputPP") == "GPIO_MODE_OUTPUT_OD",
                pin + " must remain open-drain")
        require(ioc.get(pin + ".GPIO_PuPd", "GPIO_NOPULL") == "GPIO_NOPULL", pin + " pull mismatch")
        require(ioc.get(pin + ".GPIO_Speed", "GPIO_SPEED_FREQ_LOW") == "GPIO_SPEED_FREQ_LOW",
                pin + " speed changed; extend adapter")
    for pin in ("PA2", "PA4"):
        require(ioc.get(pin + ".GPIO_ModeDefaultOutputPP", "GPIO_MODE_OUTPUT_PP") == "GPIO_MODE_OUTPUT_PP",
                pin + " must remain push-pull")
        require(ioc.get(pin + ".GPIO_PuPd", "GPIO_NOPULL") == "GPIO_NOPULL", pin + " pull mismatch")
        require(ioc.get(pin + ".GPIO_Speed", "GPIO_SPEED_FREQ_LOW") == "GPIO_SPEED_FREQ_LOW",
                pin + " speed changed; extend adapter")
    require(ioc.get("PA9.GPIO_Speed") == "GPIO_SPEED_FREQ_HIGH", "PA9 TX speed mismatch")
    require(ioc.get("USART1.VirtualMode") == "VM_ASYNC", "USART1 must use asynchronous UART")
    for key, default in (("WordLength", "WORDLENGTH_8B"), ("StopBits", "STOPBITS_1"),
                         ("Parity", "PARITY_NONE"), ("HwFlowCtl", "UART_HWCONTROL_NONE"),
                         ("Mode", "MODE_TX_RX"), ("OverSampling", "UART_OVERSAMPLING_16")):
        require(ioc.get("USART1." + key, default) == default, "Only UART 8N1/no-flow-control is supported: " + key)
    require(ioc.get("VP_FREERTOS_VS_CMSIS_V1.Mode") == "CMSIS_V1", "Expected CubeMX CMSIS_V1 scaffold")
    require(ioc.get("NVIC.PriorityGroup") == "NVIC_PRIORITYGROUP_4", "IRQ priority grouping changed")
    require(ioc.get("NVIC.TimeBase") == "TIM2_IRQn", "HAL timebase must remain TIM2")
    require(ioc.get("ProjectManager.CoupleFile") == "true", "Enable separate peripheral .c/.h generation")
    task_rows = rows(ioc["FREERTOS.Tasks01"])
    require(task_rows.keys() == TASKS.keys(), "Expected exactly the four application tasks")
    values = {"SERIAL_BAUD_RATE": int(ioc["USART1.BaudRate"])}
    for name, (entry, prefix) in TASKS.items():
        row = task_rows[name]
        require(len(row) == 9 and row[3] == entry and row[5:] == ["NULL", "Dynamic", "NULL", "NULL"],
                "Unsupported task definition: " + name)
        # In the supplied CubeF1 CMSIS-V1 adapter: native = CMSIS - osPriorityIdle(-3).
        values[prefix + "_PRIORITY"] = int(row[1]) + 3
        values[prefix + "_STACK_WORDS"] = int(row[2])
        require(0 <= int(row[1]) + 3 < int(ioc["FREERTOS.configMAX_PRIORITIES"]), "Invalid task priority")
        require(int(row[2]) > 0, "Invalid task stack")
    queue_rows = rows(ioc["FREERTOS.Queues01"])
    require(queue_rows.keys() == QUEUES.keys(), "Expected eleven application queues")
    for name, (length, item) in QUEUES.items():
        row = queue_rows[name]
        require(len(row) == 7 and row[2] == item and row[3:] == ["0", "Dynamic", "NULL", "NULL"],
                "Unsupported queue definition: " + name)
        require(int(row[1]) > 0 and (length is None or int(row[1]) == length), "Queue length mismatch: " + name)
    values["SERIAL_RX_QUEUE_LENGTH"] = int(queue_rows["serialRxQueue"][1])
    values["STORAGE_REQUEST_QUEUE_LENGTH"] = int(queue_rows["storageRequestQueue"][1])
    values["HISTORY_RECORD_QUEUE_LENGTH"] = int(queue_rows["storageSampleQueue"][1])
    require(values["HISTORY_RECORD_QUEUE_LENGTH"] <= 16, "History FIFO exceeds checked F103 RAM limit")
    values["STORAGE_TEXT_QUEUE_LENGTH"] = int(queue_rows["storageTextQueue"][1])
    irq = ioc["NVIC.USART1_IRQn"].replace("\\:", ":").split(":")
    require(irq[0] == "true" and int(irq[2]) == 0, "USART1 IRQ must be enabled, subpriority zero")
    values["SERIAL_IRQ_PRIORITY"] = int(irq[1])
    kernel = {key: int(ioc["FREERTOS." + key]) for key in KERNEL_KEYS}
    require(kernel["configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY"] <= int(irq[1]) <= 15,
            "USART1 priority does not permit FromISR calls")
    require(kernel["configTICK_RATE_HZ"] == 1000 and kernel["configSUPPORT_STATIC_ALLOCATION"] == 1
            and kernel["configSUPPORT_DYNAMIC_ALLOCATION"] == 1, "Unsupported kernel allocation/tick settings")
    require(kernel["configUSE_PREEMPTION"] == 1 and kernel["configLIBRARY_LOWEST_INTERRUPT_PRIORITY"] == 15,
            "Keep preemptive scheduling and lowest IRQ priority 15")
    return ioc, values, kernel


def check_layout(root):
    ioc = properties(read(root / (PROJECT_NAME + ".ioc")))
    require(ioc.get("FREERTOS.Mutexes01", "") == "",
            "No shared storage mutex: W25Q has one owner")
    project_tree = ET.parse(root / ("MDK-ARM/" + PROJECT_NAME + ".uvprojx"))
    options = project_tree.find("./Targets/Target/TargetOption")
    target = options.find("TargetArmAds")
    flash_start = 0x08000000
    flash_size = 0x10000  # Settings live in external W25Q, outside program Flash.
    # uVision also reads the CPU descriptor when loading the target. A stale
    # 62KB descriptor can restore the former internal-settings reservation.
    cpu = options.findtext("TargetCommonOption/Cpu", "")
    cpu_rom = re.search(r"\bIROM\(\s*(0x[0-9a-f]+)\s*-\s*(0x[0-9a-f]+)\s*\)", cpu, re.I)
    require(cpu_rom is not None and int(cpu_rom[1], 0) == flash_start and
            int(cpu_rom[2], 0) == flash_start + flash_size - 1,
            "Keil CPU IROM descriptor must be 0x08000000-0x0800FFFF; "
            "reload the corrected .uvprojx before building")
    for region in ("IROM", "OCR_RVCT4"):
        node = target.find("ArmAdsMisc/OnChipMemories/" + region)
        start = int(node.findtext("StartAddress"), 0)
        size = int(node.findtext("Size"), 0)
        require(start == flash_start and size == flash_size,
                f"Keil {region}: Start=0x{start:08X}, Size=0x{size:X}; "
                "expected Start=0x08000000, Size=0x10000 (64KB). "
                "Reload the corrected .uvprojx; check Options for Target > Target > IROM1")
    require(target.findtext("LDads/umfTarg") == "1" and target.findtext("LDads/useFile") == "0" and
            not target.findtext("LDads/ScatterFile") and not target.findtext("LDads/Misc"),
            "Use the checked target memory layout; custom linker overrides need review")
    main = read(root / "Core/Src/main.c")
    mutex_region = main.split("/* USER CODE BEGIN RTOS_MUTEX */")[1].split("/* USER CODE END RTOS_MUTEX */")[0]
    require(mutex_region.count("xSemaphoreCreateMutex()") == 0 and
            main.count("xSemaphoreCreateMutex()") == 0, "Mutex creation must stay in RTOS_MUTEX")
    for name, (entry, _) in TASKS.items():
        task_body = function(main, entry)[2]
        require(task_body in main.split("/* USER CODE BEGIN 0 */")[1].split("/* USER CODE END 0 */")[0],
                "Task body must stay in main.c USER CODE 0: " + name)
    for region, api, count in (("RTOS_THREADS", "xTaskCreate(", len(TASKS)), ("RTOS_QUEUES", "xQueueCreate(", len(QUEUES))):
        block = main.split("/* USER CODE BEGIN " + region + " */")[1].split("/* USER CODE END " + region + " */")[0]
        require(block.count(api) == count and main.count(api) == count, "Creation region mismatch: " + region)
    require(re.search(r"s_storage_sample_queue\s*=\s*xQueueCreate\(HISTORY_RECORD_QUEUE_LENGTH,\s*sizeof\(History_Sample\)\)", main),
            "History records must use the configured FIFO length")
    require(not re.search(r"xQueueOverwrite\(\s*s_storage_sample_queue", main),
            "History FIFO must never use xQueueOverwrite")
    require("HistoryBuffer_Poll(" in function(main, "StartAlarmTask")[2],
            "Alarm task must generate history records independently of storage")
    threads = main.split("/* USER CODE BEGIN RTOS_THREADS */")[1].split("/* USER CODE END RTOS_THREADS */")[0]
    require(threads.count("RtosDiagnostics_Bind(") == 1 and
            threads.index("RtosDiagnostics_Bind(") > threads.rindex("xTaskCreate("),
            "Diagnostic handles must be bound after checked task creation in RTOS_THREADS")
    order = [main.index(token) for token in ("MX_GPIO_Init();", "SerialPort_Init()", "MX_GPIO_USART1_TX_Init();",
                                              "xQueueCreate(", "xTaskCreate(", "vTaskStartScheduler();")]
    require(order == sorted(order), "Unsafe UART startup/RTOS initialization order")
    gpio = read(root / "Core/Src/gpio.c")
    initial = function(gpio, "MX_GPIO_Init")[2]
    tx_section = initial.split("GPIO_InitStruct.Pin = SERIAL_TX_Pin;")[1].split("HAL_GPIO_Init(")[0]
    require("GPIO_MODE_OUTPUT_PP" in tx_section and "GPIO_MODE_AF_PP" not in tx_section,
            "PA9 must remain GPIO-high before UART enable")
    require(initial.index("HAL_GPIO_WritePin(SERIAL_TX_GPIO_Port") < initial.index("GPIO_InitStruct.Pin = SERIAL_TX_Pin;"),
            "Preload PA9 high before enabling its output driver")
    require("GPIO_MODE_AF_PP" in function(gpio, "MX_GPIO_USART1_TX_Init")[2], "Missing UART TX handover")
    check_gpio(root)
    for path in list((root / "Core/Src").glob("*.c")) + list((root / "Core/Inc").glob("*.h")):
        code = read(path)
        require(not re.search(r'cmsis_os\.h|\bos(Thread\w*|Delay|Kernel\w*)\b', code), "CMSIS wrapper leaked into " + path.name)
    for path in (root / "Core/Src/stm32f1xx_hal_msp.c", root / "Core/Src/serial_port.c"):
        require("HAL_GPIO_Init(" not in read(path), "GPIO configuration must remain in gpio.c")
    entries = ET.parse(root / ("MDK-ARM/" + PROJECT_NAME + ".uvprojx")).findall("./Targets/Target/Groups/Group/Files/File")
    paths = [(root / "MDK-ARM" / entry.findtext("FilePath").replace("\\", "/")).resolve() for entry in entries]
    require(len(paths) == len(set(paths)) and all(path.is_file() for path in paths), "Keil file registration invalid")
    require(set((root / "Core/Inc").glob("*.h")) <= set(paths), "A Core header is missing from Keil")
    require(set((root / "Core/Src").glob("*.c")) <= set(paths), "A Core source is missing from Keil")
    require(not any(path.name in ("cmsis_os.c", "cmsis_os.h", "usart.c", "usart.h", "spi.c", "spi.h", "adc.c", "adc.h", "rtc.c", "rtc.h") for path in paths),
            "Generated duplicate UART/CMSIS scaffold must not be compiled")


def check(root, allow_pending=False):
    require(allow_pending or not (root / "Tools/.cubemx_state/pending.json").exists(),
            "CubeMX generation is unfinished. Check hook logs or run recover before building.")
    _, values, kernel = settings(root)
    config = read(root / "Core/Inc/project_config.h")
    for name, value in values.items():
        require(macro_number(config, name) == value, ".ioc/code mismatch: " + name + "; regenerate from .ioc or export the intentional code change")
    config = read(root / "Core/Inc/FreeRTOSConfig.h")
    for name, value in kernel.items():
        require(macro_number(config, name) == value, ".ioc/kernel mismatch: " + name)
    require(kernel["INCLUDE_uxTaskGetStackHighWaterMark"] == 1 and kernel["INCLUDE_xTaskGetIdleTaskHandle"] == 1,
            "diag requires the native watermark/idle APIs enabled in both .ioc and FreeRTOSConfig.h")
    check_layout(root)
    print("PASS: .ioc matches four native tasks/eleven queues/UART/SPI/ADC/RTC; GPIO and 64KB layout valid.")


def before(root):
    check_layout(root)
    state = root / "Tools/.cubemx_state"
    state.mkdir(parents=True, exist_ok=True)
    pending = state / "pending.json"
    require(not pending.exists(), "An unfinished generation exists. Run recover before retrying.")
    snapshot = state / datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    files = sorted(list((root / "Core/Src").glob("*.c")) + list((root / "Core/Inc").glob("*.h")))
    files += [root / ("MDK-ARM/" + PROJECT_NAME + ".uvprojx")]
    relative = [path.relative_to(root).as_posix() for path in files]
    for path, name in zip(files, relative):
        destination = snapshot / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, destination)
    shutil.copy2(root / (PROJECT_NAME + ".ioc"), snapshot / (PROJECT_NAME + ".ioc"))
    write(pending, json.dumps({"snapshot": snapshot.name, "files": relative}, indent=2))
    # CubeMX can continue even if a hook exits nonzero. Save first, so the
    # after hook/recover can restore the native application on any failure.
    settings(root)
    print("PASS: saved current application and Keil layout before CubeMX generation: " + snapshot.name)


def load_snapshot(root):
    state = root / "Tools/.cubemx_state"
    pending = state / "pending.json"
    require(pending.exists(), "No before-generation snapshot exists; do not apply after hook alone")
    manifest = json.loads(read(pending))
    snapshot = state / manifest["snapshot"]
    require(snapshot.is_relative_to(state) and snapshot.is_dir(), "Invalid snapshot path")
    return state, pending, manifest, snapshot


def restore_files(root, manifest, snapshot):
    for name in manifest["files"]:
        destination = root / name
        require(destination.resolve().is_relative_to(root.resolve()), "Invalid saved file path")
        shutil.copy2(snapshot / name, destination)
    # Keep generated scaffolding as diagnostic evidence, outside Core and Keil.
    for name in ("Core/Src/usart.c", "Core/Inc/usart.h", "Core/Src/spi.c", "Core/Inc/spi.h",
                 "Core/Src/adc.c", "Core/Inc/adc.h", "Core/Src/rtc.c", "Core/Inc/rtc.h"):
        path = root / name
        if name not in manifest["files"] and path.exists():
            require("USER CODE BEGIN Header" in read(path), "Unexpected new file: " + name)
            destination = snapshot / "generated" / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
            path.unlink()


def after(root):
    state, pending, manifest, snapshot = load_snapshot(root)
    ioc, values, kernel = settings(root)
    generated_main = read(root / "Core/Src/main.c")
    generated_clock = function(generated_main, "SystemClock_Config")[2]
    generated_uart = read(root / "Core/Src/usart.c")
    require(re.search(r"huart1.Init.BaudRate\s*=\s*" + str(values["SERIAL_BAUD_RATE"]) + r"\s*;", generated_uart),
            "Generated UART baud does not match .ioc")
    for filename, patterns in {
        "spi.c": (r"hspi1.Init.Mode\s*=\s*SPI_MODE_MASTER", r"hspi1.Init.NSS\s*=\s*SPI_NSS_SOFT",
                  r"hspi1.Init.BaudRatePrescaler\s*=\s*SPI_BAUDRATEPRESCALER_64",
                  r"hspi1.Init.CLKPolarity\s*=\s*SPI_POLARITY_LOW", r"hspi1.Init.CLKPhase\s*=\s*SPI_PHASE_1EDGE"),
        "adc.c": (r"sConfig.Channel\s*=\s*ADC_CHANNEL_0", r"sConfig.SamplingTime\s*=\s*ADC_SAMPLETIME_239CYCLES_5"),
        "rtc.c": (r"hrtc.Init.AsynchPrediv\s*=\s*RTC_AUTO_1_SECOND",),
    }.items():
        generated = read(root / "Core/Src" / filename)
        for pattern in patterns:
            require(re.search(pattern, generated), "Generated peripheral mismatch: " + filename + " " + pattern)
    desired = properties(read(snapshot / (PROJECT_NAME + ".ioc")))
    for key in ("FREERTOS.Tasks01", "FREERTOS.Queues01", "FREERTOS.Mutexes01", "USART1.BaudRate", "NVIC.USART1_IRQn"):
        require(ioc.get(key, "") == desired.get(key, ""), "CubeMX changed an explicit setting during generation: " + key)
    # Save the raw generator output before restoring the native application.
    for name in ("main.c", "gpio.c", "freertos.c", "stm32f1xx_it.c"):
        destination = snapshot / "generated/Core/Src" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / "Core/Src" / name, destination)
    restore_files(root, manifest, snapshot)
    main_path = root / "Core/Src/main.c"
    main = read(main_path)
    start, end, _ = function(main, "SystemClock_Config")
    write(main_path, main[:start] + generated_clock + main[end:])
    path = root / "Core/Inc/project_config.h"
    config = read(path)
    for name, value in values.items():
        config = set_macro(config, name, value)
    write(path, config)
    path = root / "Core/Inc/FreeRTOSConfig.h"
    config = read(path)
    for name, value in kernel.items():
        # ARMCC's inline assembler consumes the IRQ priority macros as operands;
        # C suffixes such as 5U are not valid assembler numeric operands.
        config = set_macro(config, name, value, suffix="")
    write(path, config)
    check(root, allow_pending=True)
    shutil.copy2(pending, state / "last_success.json")
    pending.unlink()
    print("PASS: imported generated clock/.ioc parameters; restored native tasks, private UART and separate Keil groups.")


def export(root):
    settings(root)
    check_layout(root)
    path = root / (PROJECT_NAME + ".ioc")
    text = read(path)
    ioc = properties(text)
    config = read(root / "Core/Inc/project_config.h")
    task_rows = rows(ioc["FREERTOS.Tasks01"])
    for name, (_, prefix) in TASKS.items():
        task_rows[name][1] = str(macro_number(config, prefix + "_PRIORITY") - 3)
        task_rows[name][2] = str(macro_number(config, prefix + "_STACK_WORDS"))
    ioc["FREERTOS.Tasks01"] = ";".join(",".join(row) for row in task_rows.values())
    queue_rows = rows(ioc["FREERTOS.Queues01"])
    queue_rows["serialRxQueue"][1] = str(macro_number(config, "SERIAL_RX_QUEUE_LENGTH"))
    ioc["FREERTOS.Queues01"] = ";".join(",".join(row) for row in queue_rows.values())
    queue_rows["storageRequestQueue"][1] = str(macro_number(config, "STORAGE_REQUEST_QUEUE_LENGTH"))
    queue_rows["storageSampleQueue"][1] = str(macro_number(config, "HISTORY_RECORD_QUEUE_LENGTH"))
    queue_rows["storageTextQueue"][1] = str(macro_number(config, "STORAGE_TEXT_QUEUE_LENGTH"))
    ioc["FREERTOS.Queues01"] = ";".join(",".join(row) for row in queue_rows.values())
    ioc["USART1.BaudRate"] = str(macro_number(config, "SERIAL_BAUD_RATE"))
    irq = ioc["NVIC.USART1_IRQn"].split("\\:")
    irq[1], irq[3] = str(macro_number(config, "SERIAL_IRQ_PRIORITY")), "true"
    ioc["NVIC.USART1_IRQn"] = "\\:".join(irq)
    kernel = read(root / "Core/Inc/FreeRTOSConfig.h")
    for name in KERNEL_KEYS:
        ioc["FREERTOS." + name] = str(macro_number(kernel, name))
    write(path, "\n".join(key + "=" + ioc[key] if key in ioc else line
                          for line in text.splitlines() for key in [line.split("=", 1)[0]]) + "\n")
    check(root)
    print("PASS: exported intentional native parameter changes into .ioc. Reopen CubeMX to reload them.")


def recover(root):
    _, pending, manifest, snapshot = load_snapshot(root)
    restore_files(root, manifest, snapshot)
    # Keep the edited .ioc for inspection; recover restores compiled application files.
    pending.unlink()
    print("RECOVERED: application and Keil layout restored. Review .ioc before retrying generation.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("before", "after", "check", "export", "recover"))
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    try:
        globals()[args.action](args.project.resolve())
    except (ValueError, KeyError, IndexError, OSError) as exc:
        if args.action == "after" and (args.project / "Tools/.cubemx_state/pending.json").exists():
            try:
                _, _, manifest, snapshot = load_snapshot(args.project.resolve())
                restore_files(args.project.resolve(), manifest, snapshot)
                print("RESTORED: kept the pre-generation native application. Run recover after reviewing hook logs.", file=sys.stderr)
            except (ValueError, OSError) as recovery_error:
                print("RECOVERY ERROR: " + str(recovery_error), file=sys.stderr)
        print("SYNC ERROR: " + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
