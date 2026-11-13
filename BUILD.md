# DSView 编译与运行说明

本文说明如何在 Windows 上从源码编译本工程、如何得到可直接双击运行的目录，以及必须避开的两个坑。
推送代码到 GitHub 见 [`PUSH.md`](PUSH.md)。

- 源码库：`D:\AI项目\DSView-master`（git 分支 `main`）
- 远程：https://github.com/YCG1995/DSView（`origin`，分支 `master`）
- 构建目录：`build/`（CMake/Ninja，不入库）、`build.dir/`（链接产物与运行资产，不入库）

## 1. 目录说明

| 路径 | 内容 |
|---|---|
| `DSView/` | 主程序源码（`pv/` 界面与逻辑、`icons/`、`themes/`、`stylesheet.qss`） |
| `libsigrok4DSL/` | 硬件驱动层（DSLogic/DSCope 探针、通道模式、`hardware/DSL/dsl.h`） |
| `libsigrokdecode4DSL/` | 解码框架与 683 个 decoders 脚本 |
| `CMake/`、`CMakeLists.txt` | 构建脚本 |
| `deploy_runtime.ps1` | 把运行资产铺进 `build.dir` 的脚本（见 §5） |
| `push_to_github.ps1` | 一键同步推送到 GitHub（见 `PUSH.md`） |
| `doc/`、`*.pdf` | 上游文档 |

## 2. 工具链

MSYS2 / MinGW-w64 **UCRT64**（Qt 5.15.19 + libusb + fftw + zlib + boost + Python 3.14），位于：

```
C:/software/msys64/ucrt64
```

关键可执行：`qmake-qt5.exe`、`g++`、`cmake`、`ninja`、`moc.exe`、`python3.14`（标准库在 `lib/python3.14`，运行库 `libpython3.14.dll`）。
bash 入口：`C:\software\msys64\usr\bin\bash.exe`。

> 旧的 `C:\Users\12971\Doubao\chats\2026-11-10\new-chat\_toolchain\msys64` 已弃用，配置时不要再指向它。
> 工具链里 **没有 `windeployqt.exe`**，运行依赖靠 §5 的脚本从已有运行目录拷贝。

## 3. 两个必须遵守的构建约束

### 3.1 中文路径会让 moc 失败 → 必须走 ASCII 目录联接

工程目录含中文（`D:\AI项目\DSView-master`）。`moc.exe` 经 `cmd.exe` 接收非 ASCII 路径会被打乱，报：

```
moc: D:/AI项目/DSView-master/DSView/pv/mystyle.h: No such file or directory
```

于是所有 moc 目标 FAILED。解决方式是建一个纯 ASCII 的目录联接（junction），编译只走它：

```powershell
cmd /c mklink /J "D:\DSView-work" "D:\AI项目\DSView-master"
```

- 源码编辑、git 操作在 `D:\AI项目\DSView-master`
- 配置与编译在 `D:\DSView-work`（同一份文件，`mklink /J` 只建链接、不复制）
- 联接被删了，重跑上面一行即可

### 3.2 必须显式指定 Python 根目录

不指定时 CMake 会挑系统的 `C:\Program Files\Python314`。程序内嵌解释器需要的是 MSYS2 的 Python 标准库，用系统 Python 会在启动瞬间退出：

```
Fatal Python error: Failed to import encodings module
```

因此配置时固定加 `-DPython3_ROOT_DIR=C:/software/msys64/ucrt64`。

## 4. 配置与编译

### 4.1 首次配置（全量）

PowerShell 下（推荐，命令为最终可用版本）：

```powershell
$env:MSYSTEM='UCRT64'; $env:CHERE_INVOKING='1'
& 'C:\software\msys64\usr\bin\bash.exe' -lc "cd '/d/DSView-work' && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DPython3_ROOT_DIR=C:/software/msys64/ucrt64"
```

在 MSYS2 bash 里等价于：

```bash
export MSYSTEM=UCRT64
cd /d/DSView-work
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DPython3_ROOT_DIR=C:/software/msys64/ucrt64
```

`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` 是为了让老版本 `CMakeLists.txt` 在新 CMake 上不报 policy 错误。

### 4.2 增量编译

```powershell
$env:MSYSTEM='UCRT64'; $env:CHERE_INVOKING='1'
& 'C:\software\msys64\usr\bin\bash.exe' -lc "cd '/d/DSView-work/build' && ninja"
```

成功标志：

- 全量：`[370/370] Linking CXX executable D:\DSView-work\build.dir\DSView.exe`
- 增量（只改一两处）：`[2/2] Linking ...`

**只改 `.qss`、`.qrc`、`.ui` 时注意**：`qt5_add_resources` 的 qrc 在 cmake 配置期生成，改样式表后不重新配置可能不生效。保险做法是重跑一次 §4.1 的配置命令再 `ninja`。

### 4.3 改了代码要重编的范围

| 改动位置 | 是否需要重新配置 | 说明 |
|---|---|---|
| `DSView/pv/*.cpp/.h` | 否 | 直接 `ninja` |
| `DSView/themes/*.qrc` 内的 qss | 是 | qrc 在配置期生成 |
| `DSView/stylesheet.qss` | 否（且它本身未被任何 qrc 收录，实际不生效） | 真正生效的是 `DSView/themes/breeze.qrc` 里的 `light.qss` / `dark.qss` |
| `CMakeLists.txt`、`CMake/*.cmake` | 是 | 配置期逻辑 |

## 5. 产物与运行

编译产物：`D:\AI项目\DSView-master\build.dir\DSView.exe`（= `D:\DSView-work\build.dir\DSView.exe`）。

Qt 是动态链接，**单独一个 exe 拷到空目录双击会以 `0xC0000135`（找不到 DLL）立即退出**。必须有完整运行目录。

### 5.1 build.dir 默认为什么不能直接运行

`build.dir` 只是 CMake 的 `EXECUTABLE_OUTPUT_PATH`，只放链接产物和中间对象。下面这些不是编译产物：

| 缺失项 | 作用 | 来源 |
|---|---|---|
| `Qt5Core/Gui/Widgets/Network/Svg/WinExtras.dll` | Qt 运行库 | `ucrt64/bin` |
| `platforms/`、`imageformats/`、`iconengines/`、`styles/` | Qt 插件（缺 `qwindows.dll` 无法建窗口） | 同上 |
| `libusb-1.0.dll`、`libfftw3-3.dll`、`libpython3.14.dll`、`libglib-2.0-0.dll`、ICU/PCRE2/zlib/libstdc++/winpthread 等 | 驱动、FFT、内嵌 Python、glib 运行库 | 同上 |
| `lib/python3.14` | 内嵌解释器标准库（缺了报 `Failed to import encodings module`） | `ucrt64/lib/python3.14` |
| `res/`、`demo/` | 固件与示例波形，程序按 **exe 所在目录** 相对查找 | 源码 `DSView/res`、`DSView/demo` |
| `lang/` | 界面语言资源 | 源码 `lang/` |
| `decoders/` | 解码脚本，683 个文件 | 源码 `libsigrokdecode4DSL/decoders` |

上游 `install()` 规则是 Linux 布局（`bin/` + `share/DSView`），Windows 上不产生“就地可运行”目录，所以编译目录与运行目录天然是两个。

### 5.2 让 build.dir 就地可运行

用一个**已经能跑的完整运行目录**做模板，把资产铺进 `build.dir`：

```powershell
pwsh -File .\deploy_runtime.ps1 -Template '<一个完整运行目录>'
```

脚本会复制 33 个 DLL、Qt 插件目录、`res/`、`demo/`、`lang/`、`decoders/`、`lib/python3.14`（约 3,240 个文件 / 144 MB），跳过模板里的旧 exe、`autosave` 和 `*.md`/`*.txt` 说明文档。

铺完即：

```
D:\AI项目\DSView-master\build.dir\DSView.exe
```

双击可直接运行，与本工程目录、工具链路径无关。已实测：从该路径启动、MCP 识别 `DSLogic U2Pro16`、采样率 1G 正常。

> **模板已经没了怎么办**：早期那个 `DSView-win64` 便携目录现在只剩 `autosave`，模板不可用。
> 但当前 `build.dir` 本身就是一份完整运行目录（3,246 个文件），重建时这样自救：
>
> ```powershell
> Copy-Item -LiteralPath 'D:\AI项目\DSView-master\build.dir' -Destination 'D:\DSView-runtime-template' -Recurse
> pwsh -File .\deploy_runtime.ps1 -Template 'D:\DSView-runtime-template'
> ```
>
> 注意 `deploy_runtime.ps1` 的模板排除规则只跳 `DSView.exe` 等少量名字，用 build.dir 当模板时会连自己一起拷，所以务必拷到**另一个目录**再当模板。

### 5.3 交叉验证与交付

```powershell
# 产物大小与哈希
Get-Item -LiteralPath 'D:\AI项目\DSView-master\build.dir\DSView.exe' | Select-Object Length
Get-FileHash -LiteralPath 'D:\AI项目\DSView-master\build.dir\DSView.exe' -Algorithm SHA256
```

当前基线（38px 图标版）：`4,398,193` 字节 / SHA-256 `3738DA93…F166296D`。

真机验证要点：

1. 从 `build.dir` 双击启动，窗口能建（不闪退、不报 `0xc000007b`）。
2. MCP 能列出设备：`http://127.0.0.1:6347/mcp` 的 `get_device_info` 返回 `DSLogic U2Pro16`。
3. PPS 跑一轮，`get_pps_status` 的 `last_report_ms` 与 CH1 delta 正常。

## 6. 常见错误对照

| 现象 | 根因 | 处理 |
|---|---|---|
| `moc: ...: No such file or directory`，所有 moc 目标 FAILED | 中文路径传给 moc.exe | 建 `D:\DSView-work` 联接，编译走联接 |
| 启动即退，`Fatal Python error: Failed to import encodings module` | 用了系统 Python | 配置加 `-DPython3_ROOT_DIR=C:/software/msys64/ucrt64` |
| 双击 exe 报 `0xC0000135` | 缺 Qt DLL | 用完整运行目录 / 跑 `deploy_runtime.ps1` |
| 双击 exe 报 `0xc000007b` | `platforms/qwindows.dll` 缺失或架构不符 | 同上 |
| 改 qss 后界面没变化 | qrc 在配置期生成 | 重跑配置命令再 `ninja` |
| `.gitignore` 把 `BUILD.md` 也忽略了 | Windows 下 `build*` 忽略大小写 | `.gitignore` 写 `build/`、`build.dir/`、`build-*/` |
| CMake 报 policy 版本错误 | 老 CMakeLists + 新 CMake | 加 `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` |

## 7. 已含的定制

- **PPS 只采集选中通道**：`ppsmanager.cpp` 的 `apply_hardware_config()` 重排为 op_mode → 选通道模式 → 读 ch_num/max_rate → `SAMPLERATE` → `LIMIT_SAMPLES` → 探针开关 → 触发；`QSet<int> needed` = 参考通道 ∪ 测量通道。8ch 模式全开 8 探针 `last_report_ms≈237.4`，只开 3 探针 `≈83.8`（约 2.8 倍）。
- **16 → 8 通道档的覆盖与还原**：采样率/深度前置、探针开关最后写；新增 `SigSession::reload_signals()`；`stop()` 后延迟 300 ms 重建通道行；`restore_probe_state()` 按 `SR_CONF_CHANNEL_MODE` 差异写回。采样率/深度**不还原**（还原会让下次采集直接失败）。
- **工具栏图标 38×38**：`mainwindow.cpp` 中 `setIconSize(QSize(38, 38))`；桌面分支对 `_sampling_bar`/`_trig_bar`/`_file_bar`/`_logo_bar` 四条工具栏先 `bar->setIconSize(38)`，再用 `findChildren<QToolButton*>()` 逐个设置（`addWidget` 挂进来的按钮不吃 `QMainWindow::setIconSize`）；标签宽度按 `fontMetrics().boundingRect(text).width()+14` 兜底防折行；Android 分支保持 34。
- **build.dir 就地可运行**：`deploy_runtime.ps1`。
