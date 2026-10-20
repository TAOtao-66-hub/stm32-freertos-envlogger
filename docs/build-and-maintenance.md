# 编译与配置维护

原工程记录的环境为 Keil MDK、ARM Compiler 5.06 update 7、STM32F1xx_DFP 2.4.0。工程使用 ARMCC5，不应仅切换 ARMCC6 后假定能够直接构建。

构建前会调用 Tools/check_project.cmd → Run-Sync.ps1 → project_sync.py check。Python 版本要求 3.10+，启动器按 STM32_PYTHON 环境变量、Windows py.exe、python.exe 顺序查找。首次使用时可在命令行运行：

```powershell
python Tools/project_sync.py check
python -m unittest discover -s Tests -p test_export_history.py
```

第二条只执行电脑导出协议的既有单元测试，不需要电路板和 pyserial。

CubeMX 与原生 FreeRTOS 之间有适配流程：

1. 重新生成前运行 Tools/before_generate.cmd，保存应用和工程布局。
2. 生成后运行 Tools/after_generate.cmd，导入支持的时钟/配置变化并恢复原生任务组织。
3. 运行 Tools/check_project.cmd，检查任务、队列、GPIO 与 Keil 文件注册。

请阅读 project_sync.py 的支持范围。仅打开 .ioc 并随意生成，可能引入 CMSIS 包装和重复外设实现。当前直接编译现有工程无需重新生成。

Tests 中其他既有测试使用 ARMCC 与模型执行环境，部分脚本保留原开发机的工具路径。运行前需核对脚本内的编译器路径并安装它们声明的依赖；它们没有在所有电脑环境中完成可移植性验证。

本次整理只替换了电脑端 Run-Sync.ps1 的 Python 查找方式，去掉个人运行时目录依赖；固件 .c/.h/.s、.ioc 与 Keil 工程均保留原始字节。
