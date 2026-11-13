# 把运行时资产铺进 build.dir，使 D:\AI项目\DSView-master\build.dir\DSView.exe 可以就地双击运行。
#
# 用法：
#   pwsh -File .\deploy_runtime.ps1 -Template <一个已可运行的完整运行目录>
#
# 说明：
#   build.dir 里只有链接出来的 exe 和中间对象。Qt 是动态链接，exe 需要同目录的
#   Qt5*.dll、platforms/、imageformats/、iconengines/、styles/，以及程序自己按
#   exe 相对路径查找的 res/、lang/、decoders/、demo/、lib/python3.14（内嵌解释器
#   的标准库）。这些都不是编译产物，所以要么从已有的运行目录拷，要么手工
#   windeployqt + 逐项拷贝。本脚本做前者，模板目录通常就是 DSView-win64。
#
#   模板里以 exe 结尾的旧程序、autosave 数据、说明文档不会被复制。

param(
    [Parameter(Mandatory = $true)]
    [string]$Template,

    [string]$Target = (Join-Path $PSScriptRoot 'build.dir')
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Template -PathType Container)) {
    throw "模板目录不存在: $Template"
}
if (-not (Test-Path -LiteralPath $Target -PathType Container)) {
    throw "目标目录不存在（先编译一次生成 build.dir）: $Target"
}

$skipNames = @('autosave', 'DSView.exe', 'dlls.txt', 'tmp_old.exe')
$skipExt = @('.md', '.txt')

$copied = 0
$bytes = 0L

foreach ($item in Get-ChildItem -LiteralPath $Template -Force) {
    if ($skipNames -contains $item.Name) { continue }
    if ($item.PSIsContainer -eq $false -and $skipExt -contains $item.Extension) { continue }

    $dest = Join-Path $Target $item.Name
    if ($item.PSIsContainer) {
        Copy-Item -LiteralPath $item.FullName -Destination $Target -Recurse -Force
        $bytes += (Get-ChildItem -LiteralPath $dest -Recurse -File | Measure-Object -Property Length -Sum).Sum
        $copied += (Get-ChildItem -LiteralPath $dest -Recurse -File | Measure-Object).Count
    }
    else {
        Copy-Item -LiteralPath $item.FullName -Destination $dest -Force
        $bytes += $item.Length
        $copied += 1
    }
}

"copied files: $copied, $([math]::Round($bytes / 1MB, 1)) MB -> $Target"
"exe: " + (Join-Path $Target 'DSView.exe')
