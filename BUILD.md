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

## 已含的定制

- PPS 只采集选中的通道（参考通道 + 测量通道），USB 传输量约降到 1/2.8。
- 16 通道 -> 8 通道档时把未采集通道一并关闭，并在 PPS 起停时按实际使能探针重建通道行。
- 工具栏图标统一 48x48（含采样栏 `addWidget` 按钮，需同时设置所在 QToolBar 的 iconSize）。
