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

MSYS2 / MinGW-w64 UCRT64（Qt 5.15.19 + libusb + fftw + zlib + boost + Python 3.14）：

```
C:/Users/12971/Doubao/chats/2026-11-10/new-chat/_toolchain/msys64/ucrt64
```

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
      -DPython3_ROOT_DIR=C:/Users/12971/Doubao/chats/2026-11-10/new-chat/_toolchain/msys64/ucrt64
cmake --build build          # 或 cd build && ninja
```

产物：`D:\DSView-work\build.dir\DSView.exe`（= `D:\AI项目\DSView-master\build.dir\DSView.exe`）。

## 部署到便携目录

```powershell
Stop-Process -Name DSView -Force -ErrorAction SilentlyContinue
Copy-Item -LiteralPath 'D:\AI项目\DSView-master\build.dir\DSView.exe' -Destination '<DSView-win64>\DSView.exe' -Force
```

## 已含的定制

- PPS 只采集选中的通道（参考通道 + 测量通道），USB 传输量约降到 1/2.8。
- 16 通道 -> 8 通道档时把未采集通道一并关闭，并在 PPS 起停时按实际使能探针重建通道行。
- 工具栏图标统一 48x48（含采样栏 `addWidget` 按钮，需同时设置所在 QToolBar 的 iconSize）。
