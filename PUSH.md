# 上传说明（推送到 GitHub）

本文说明本工程如何把本地代码上传到 GitHub 远程仓库，以及两条历史线的现状与处理方式。
编译与运行见 [`BUILD.md`](BUILD.md)。

- 本地仓库：`D:\AI项目\DSView-master`，分支 `main`
- 远程：https://github.com/YCG1995/DSView（`origin`），主分支 `master`（**public 仓库**）
- 认证：HTTPS + Git Credential Manager（`git config --system credential.helper=manager`）
- 代理/免密：凭据已缓存，`git push` 不需要再输账号密码；换机器或凭据失效时会弹浏览器授权

## 1. 必须先理解的一件事：两条历史线

| 线 | 位置 | 内容 |
|---|---|---|
| 远程线 | `origin/master` | 上游 fork 历史 + 之前发布 v2.0.1/v2.0.2 的提交（11-12 之前的工作） |
| 本地线 | `main` | 11-13 起重新 `git init` 的 5 个提交（含 38px 图标、PPS 子集、通道还原、`deploy_runtime.ps1`） |

两条线的第一个提交不同（**没有共同祖先**），所以：

- `git push origin main` **不会**成功（`non-fast-forward` / `unrelated histories`）
- 不能 `git pull` 合并，硬合并会产生大量冲突且毫无意义

正确策略：**以 `origin/master` 为父，把本地 `main` 的整棵代码树作为一个提交压上去**。这样远程保持一条连续历史，且是快进（fast-forward）推送——不覆盖、不丢任何已有提交（v2.0.1/v2.0.2 的 tag 也照旧指向原提交）。

## 2. 方式 A：一键同步（推荐）

```powershell
cd 'D:\AI项目\DSView-master'
pwsh -File .\push_to_github.ps1 -Message "同步：<本次改动摘要>"
```

脚本做的事：

1. 检查工作树是否干净（不干净直接中止，避免把半成品推上去）
2. `git fetch origin master`
3. 基于 `origin/master` 建临时分支 `push-sync`
4. `git read-tree --reset -u <main>`：把临时分支的文件树整体换成 `main` 的内容
5. 提交（若与远程完全一致则不提交，直接报 “已是最新”）
6. `git push origin push-sync:master`
7. 无论成功失败，最后都切回原来的 `main` 分支

常用参数：

| 参数 | 说明 |
|---|---|
| `-Message` | 提交信息，不填用默认文案 |
| `-Branch` | 要推送的本地分支，默认 `main` |
| `-RemoteBranch` | 远程分支，默认 `master` |
| `-DryRun` | 只打印将要做什么，不提交不推送 |
| `-AllowDirty` | 允许工作树有未提交改动时继续（一般不要用） |

## 3. 方式 B：手动三步（等价于脚本）

```powershell
cd 'D:\AI项目\DSView-master'

# 1) 拿到远程最新
git fetch origin master

# 2) 基于远程建同步分支，并把它整棵树换成 main 的内容
git switch -C push-sync origin/master
git read-tree --reset -u main          # main 需先提交完，工作树必须是干净的
git commit -m "同步：<摘要>"

# 3) 推上去（快进）
git push origin push-sync:master

# 4) 回本地主线
git switch main
```

`git read-tree --reset -u <树>` 只改索引与工作区，不动当前分支历史，这正是“整棵树同步”的关键一步。

## 4. 方式 C：强制推送（不推荐，仅在明确要覆盖远程历史时）

```powershell
git push origin main:master --force
```

会把远程 `master` 直接指到本地 `main`，远程原有的 2.0.1/2.0.2 提交从 `master` 上消失（仍可从 tag `v2.0.1` / `v2.0.2` 找回）。**只有在你确认不需要保留远程旧历史时才用**。

## 5. 提交前检查清单

```powershell
git status --short            # 期望为空
git diff --stat               # 复核改了哪些文件
git log --oneline -5          # 确认提交信息
```

**不该入库的东西**（`.gitignore` 已覆盖，别手动加回来）：

| 内容 | 原因 |
|---|---|
| `build/`、`build.dir/` | 编译目录与链接产物 |
| `build.dir/` 内的 DLL、`res/`、`lang/`、`decoders/`、`lib/python3.14` | 144 MB 运行资产，可随时由 `deploy_runtime.ps1` 重建 |
| `*.zip`、便携包 | 二进制交付物，走云盘/Release，不进 git |
| 解码器自动生成的 `*.pyc`、`autosave/` | 运行产物 |

`.gitignore` 里写 `build/`、`build.dir/`、`build-*/`，**不要**写成 `build*`——Windows 忽略大小写，`build*` 会把 `BUILD.md` 一起忽略。

## 6. tag 与 Release

远程已有 tag：`v0.3.0`、`v0.9.3`–`v0.9.9`、`v1.0.0`–`v1.3.2`、`v2.0.1`、`v2.0.2`。

打新版本（本地打 tag 后推）：

```powershell
# 建议在 push-sync 上打，使 tag 与远程主线一致
git switch push-sync
git tag -a v2.0.3 -m "v2.0.3: PPS 通道子集采集 / 16->8 通道还原 / 工具栏 38px"
git push origin v2.0.3
```

本机没有 `gh` CLI，创建 Release 用网页：仓库 → Releases → Draft a new release → 选 tag → 上传 zip。
运行目录 zip 约 60–150 MB，GitHub 单文件上限 100 MB（Release 附件上限 2 GB），所以**便携包只发 Release 附件，绝不进 git**。

## 7. 常见错误对照

| 现象 | 根因 | 处理 |
|---|---|---|
| `! [rejected] ... (non-fast-forward)` | 本地线与远程无共同祖先，或远程有新提交 | 用方式 A/B 的同步流程；先 `git fetch` |
| `fatal: refusing to merge unrelated histories` | 直接 `git pull` | 不要合并，改用方式 A/B |
| `Authentication failed` / 反复要账号 | GCM 凭据失效 | 浏览器登录一次；或改用带 token 的 URL |
| `remote: Permission denied` | 账号无该仓库写权限 | 确认登录账号是 `YCG1995` |
| 推送卡住不动 | 网络/代理问题 | 配 `git config --global http.proxy`，或重试 |
| 提交了 100 MB 以上文件被拒 | `build.dir` 之类误入库 | `git rm --cached` 后重新提交，并把规则加回 `.gitignore` |

## 8. 推送后核验

```powershell
git ls-remote origin refs/heads/master            # 远程 SHA
git fetch origin master -q
git show origin/master:DSView/pv/mainwindow.cpp | Select-String 'QSize\(38, 38\)'
git log -1 --format='%h %ad %s' origin/master
```

远程 SHA 与本地同步提交一致、关键代码特征能检出，即推送成功。
