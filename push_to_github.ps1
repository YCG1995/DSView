# 把本地分支的整棵代码树同步到 GitHub 远程主分支（快进推送，不覆盖远程历史）。
#
# 背景：本地 main 与 origin/master 没有共同祖先（本地是重新 git init 的一串提交），
# 直接 push 会 non-fast-forward。本脚本的做法是：
#   以 origin/master 为父建临时分支 push-sync，
#   用 git read-tree 把它的文件树整体换成要推送分支的内容，
#   再提交并推送 —— 远程始终是一条连续历史，且是 fast-forward。
#
# 用法：
#   pwsh -File .\push_to_github.ps1 -Message "同步：xxx"
#   pwsh -File .\push_to_github.ps1 -DryRun              # 只看将要做什么
#
# 详细说明见 PUSH.md

param(
    [string]$Message = "同步本地最新代码到 GitHub",

    [string]$Branch = 'main',

    [string]$RemoteBranch = 'master',

    [string]$Remote = 'origin',

    [switch]$DryRun,

    [switch]$AllowDirty
)

$ErrorActionPreference = 'Stop'

function Invoke-Git {
    param([string[]]$GitArgs)
    & git @GitArgs
    if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') 失败 (exit $LASTEXITCODE)" }
}

Push-Location -LiteralPath $PSScriptRoot
$origBranch = $null
try {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot '.git'))) {
        throw "不是 git 仓库: $PSScriptRoot"
    }

    $origBranch = (& git branch --show-current).Trim()
    if (-not $origBranch) { throw '当前处于分离 HEAD 状态，请先切到一个分支' }

    $dirty = (& git status --porcelain)
    if ($dirty -and -not $AllowDirty) {
        throw "工作树有未提交改动，先提交或撤销后再推送。`n$($dirty -join "`n")"
    }

    $localHead = (& git rev-parse $Branch).Trim()
    if ($LASTEXITCODE -ne 0) { throw "本地分支不存在: $Branch" }

    Write-Host "[1/6] fetch $Remote/$RemoteBranch"
    Invoke-Git @('fetch', $Remote, $RemoteBranch)
    $remoteHead = (& git rev-parse "$Remote/$RemoteBranch").Trim()

    Write-Host "      本地 $Branch  = $($localHead.Substring(0,8))"
    Write-Host "      远程 $RemoteBranch = $($remoteHead.Substring(0,8))"

    if ($DryRun) {
        Write-Host "[DryRun] 将执行："
        Write-Host "        git switch -C push-sync $Remote/$RemoteBranch"
        Write-Host "        git read-tree --reset -u $($localHead.Substring(0,8))"
        Write-Host "        git commit -m `"$Message`"（若与远程一致则跳过）"
        Write-Host "        git push $Remote push-sync:$RemoteBranch"
        return
    }

    Write-Host "[2/6] 基于 $Remote/$RemoteBranch 建同步分支 push-sync"
    Invoke-Git @('switch', '-C', 'push-sync', '--no-track', "$Remote/$RemoteBranch")

    Write-Host '[3/6] 把工作树换成目标分支的内容'
    Invoke-Git @('read-tree', '--reset', '-u', $localHead)

    Write-Host '[4/6] 提交'
    & git diff --cached --quiet
    if ($LASTEXITCODE -eq 0) {
        Write-Host '      远程内容已与本地一致，无需推送'
        return
    }
    Invoke-Git @('commit', '-m', $Message)
    $syncHead = (& git rev-parse HEAD).Trim()
    Write-Host "      push-sync = $($syncHead.Substring(0,8))"
    Write-Host '      变更文件数: ' + (& git diff --stat "$remoteHead..HEAD" | Select-Object -Last 1)

    Write-Host "[5/6] push $Remote push-sync:$RemoteBranch"
    Invoke-Git @('push', $Remote, "push-sync:$RemoteBranch")

    Write-Host '[6/6] 校验远程'
    $newRemote = (git ls-remote $Remote "refs/heads/$RemoteBranch").Split()[0]
    if ($newRemote -ne $syncHead) {
        throw "推送后远程 SHA($($newRemote.Substring(0,8))) 与本地($($syncHead.Substring(0,8))) 不一致"
    }
    Write-Host "      远程 $RemoteBranch 现在 = $($newRemote.Substring(0,12))"
    Write-Host '完成。'
}
finally {
    if ($origBranch) {
        & git switch $origBranch --quiet 2>$null
        if ($LASTEXITCODE -eq 0) { Write-Host "已切回分支 $origBranch" }
    }
    Pop-Location
}
