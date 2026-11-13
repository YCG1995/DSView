# DSView 工程（D:\AI项目\DSView-master）

本目录是 DSView 的唯一源码库，已用 git 管理（分支 `main`）。
构建产物（`build/`、`build.dir/`）不入库。

## 目录说明

| 路径 | 内容 |
|---|---|
| `DSView/` | 主程序源码（`pv/` 界面与逻辑、`icons/`、`themes/`） |
| `libsigrok4DSL/` | 硬件驱动层（DSLogic/DSCope 探针、通道模式） |
| `libsigrokdecode4DSL/` | 解码框架与 decoders |
| `CMake/`、`CMakeLists.txt` | 构建脚本 |
| `doc/`、`*.pdf` | 上游文档 |

## 工具链

MSYS2 / MinGW-w64 UCRT64（Qt 5.15.19 + libusb + fftw + zlib + boost + Python 3.14），已迁到：

```
C:/software/msys64/ucrt64
```

（旧的 `C:/Users/12971/Doubao/chats/2026-11-10/new-chat/_toolchain/msys64/ucrt64` 已弃用，配置时必须指向新路径。）

## 两个必须知道的构建约束

1. **中文路径会让 moc 失败**。工程目录含中文（`D:\AI项目\...`），`moc.exe` 经 `cmd.exe` 拿到的非 ASCII 路径会被打乱，报 `moc: ...: No such file`。
   因此构建统一走 ASCII 目录联接：

   ```powershell
   cmd /c mklink /J "D:\DSView-work" "D:\AI项目\DSView-master"
   ```

   源码编辑、git 操作在 `D:\AI项目\DSView-master`；编译在 `D:\DSView-work`（同一份文件）。
   若联接被删掉，重建同一行命令即可（`mklink /J` 不会复制文件）。

2. **必须显式指定 Python 根目录**。CMake 默认会挑系统的 `C:\Program Files\Python314`，而运行时内嵌解释器需要 MSYS2 的 Python 标准库，否则启动后立刻 `Fatal Python error: Failed to import encodings module` 退出。

## 配置与编译

```bash
export MSYSTEM=UCRT64
cd /d/DSView-work
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DPython3_ROOT_DIR=C:/software/msys64/ucrt64
cmake --build build          # 或 cd build && ninja
```

产物：`D:\DSView-work\build.dir\DSView.exe`（= `D:\AI项目\DSView-master\build.dir\DSView.exe`）。

## 运行方式（重要）

Qt 是动态链接，**exe 不能单独拷走运行**：单独一个 `DSView.exe` 放到空目录双击会以 `0xC0000135`（DLL 未找到）立即退出。

正确用法是带上完整运行目录（`DSView-win64`，约 160 MB，含 33 个 DLL、`res/`、`lang/`、`decoders/`、`platforms/` 等）：

```powershell
Copy-Item -LiteralPath 'D:\AI项目\DSView-master\build.dir\DSView.exe' `
          -Destination '<运行目录>\DSView.exe' -Force
```

然后双击 `<运行目录>\DSView.exe`。该目录自带全部依赖，与本工程目录、工具链路径均无关，可整体拷到任意机器/任意盘运行。

### 为什么 build.dir 默认不能直接运行

`build.dir` 只是 CMake 的 `EXECUTABLE_OUTPUT_PATH`，里面只有链接产物（`DSView.exe`）和中间对象。下面这些不是编译产物，所以不会自动出现在那里：

| 缺失项 | 作用 | 来源 |
|---|---|---|
| `Qt5Core.dll` / `Qt5Gui.dll` / `Qt5Widgets.dll` / `Qt5Network.dll` / `Qt5Svg.dll` / `Qt5WinExtras.dll` | Qt 运行库（动态链接） | 工具链 `ucrt64/bin` |
| `platforms/`、`imageformats/`、`iconengines/`、`styles/` | Qt 插件（缺 `qwindows.dll` 会直接 `0xc000007b`/无法建窗口） | 同上 |
| `libusb-1.0.dll`、`libfftw3-3.dll`、`libpython3.14.dll`、`libglib-2.0-0.dll`、ICU/PCRE2/zlib 等 | 驱动、FFT、内嵌 Python、glib 的运行库 | 同上 |
| `lib/python3.14` | 内嵌解释器的标准库（缺了报 `Failed to import encodings module`） | `ucrt64/lib/python3.14` |
| `res/`、`demo/` | 固件与示例波形，程序按 **exe 所在目录** 相对查找 | 源码 `DSView/res`、`DSView/demo` |
| `lang/` | 界面语言资源 | 源码 `lang/` |
| `decoders/` | 解码脚本（683 个文件） | 源码 `libsigrokdecode4DSL/decoders` |

上游的 `install()` 规则是 Linux 布局（`bin/` + `share/DSView`），Windows 上不产生“就地可运行”目录，所以默认是编译目录、运行目录分开。

### 让 build.dir 就地可运行

```powershell
pwsh -File .\deploy_runtime.ps1 -Template 'C:\Users\12971\Doubao\chats\2026-11-10\new-chat\DSView-win64'
```

脚本把运行目录里的 33 个 DLL、Qt 插件目录、`res/`、`demo/`、`lang/`、`decoders/`、`lib/python3.14` 铺进 `build.dir`（约 3,244 个文件 / 144 MB，不复制旧 exe、autosave 与说明文档）。之后：

```
D:\AI项目\DSView-master\build.dir\DSView.exe
```

即可直接双击运行，不再需要任何其它路径。已实测：从该路径启动、识别 `DSLogic U2Pro16`、采样率 1G 正常。

注意：`build.dir` 被 `.gitignore` 忽略，且 `cmake -S . -B build` 重新配置不会清空它；但如果手动删了 `build.dir`，重编后要再跑一次这个脚本。

## 已含的定制

- PPS 只采集选中的通道（参考通道 + 测量通道），USB 传输量约降到 1/2.8。
- 16 通道 -> 8 通道档时把未采集通道一并关闭，并在 PPS 起停时按实际使能探针重建通道行。
- 工具栏图标统一 48x48（含采样栏 `addWidget` 按钮，需同时设置所在 QToolBar 的 iconSize）。
