# 上游追新流程（ace_break）

本目录存放将 baldurk/renderdoc 上游更新合并进 `ace_break` 分支所需的工具和流程文档。

## 文件

- `rename_to_noobdawn.py` — 可重复执行的重命名脚本（RenderDoc→NoobDawn 等），
  用于把上游新带入的 renderdoc 字符串/路径重新掩盖。脚本排除自身、文档内容
  （.md/.rst/.txt）和二进制内容（.exe/.dll/.ttf 等），可安全反复运行。

## 追新步骤

### 1. 同步上游

```bash
git fetch upstream
git checkout ace_break
git merge upstream/v1.x
```

冲突预期：

- **rename 提交**涉及的文件（几乎全仓库）在合并时依赖 git 的 rename 检测，
  大部分上游改动能自动落到 noobdawn 命名后的文件上；
- **注入相关文件**（`noobdawn/os/win32/win32_process.cpp`、
  `noobdawn/os/win32/win32_hook.cpp`）上游自 v1.45 起一直未改动，
  一旦上游动了这两个文件，需要对照 `method3_setthreadcontext.patch`
  手工做语义合并，切勿直接取用任一侧；
- `noobdawn.sln`、`util/deploy/deploy.vcxproj` 是本地新增/接线的，
  上游若改 sln 结构需保留 deploy 项目的声明、依赖、配置映射和嵌套条目。

### 2. 重跑 rename（掩盖上游新带入的特征串）

```bash
python upstream_sync/rename_to_noobdawn.py
```

脚本作用于整个仓库根目录（也可以显式传入仓库根路径作为第一个参数）。
它是幂等的：已重命名的内容不会再次变动，只有上游新带入的
renderdoc/RenderDoc/RDC 字符串和新文件路径会被处理。

运行后检查：

```bash
# 应只剩受保护残留（[A-Z]RDC、hardcoded 之类的 rdc、以及文档/注释）
git status -s
```

确认改动仅限上游新增内容后，单独提交一个 rename 提交，例如：

```bash
git add -A
git commit -m "rename newly introduced RenderDoc identifiers after upstream sync"
```

注意：3rdparty 预编译二进制（Qt/PySide/python36/dbghelp 等）被 .gitignore
排除但受 git 跟踪；若上游更新了这些二进制，重命名/移动后需 `git add -f`
对应路径才能保持跟踪。

### 3. 验证

1. 完整编译 `noobdawn.sln`（Release x64 + Win32）；
2. 构建 deploy 项目，确认 `dist\Release\` 产物组织正确；
3. 对 ACE 保护目标做一次实际 capture + replay（编译通过不代表注入行为正确）。

### 4. 推送

```bash
git push origin ace_break
```

## 背景约定

- `ace_break` 相对上游的差异只有四类：deploy 接线、全仓库 rename、
  注入行为（win32_process/win32_hook）、本目录；除此之外不要引入其他改动，
  否则追新合并会变得困难；
- 不固定 PlatformToolset（上游默认，本地如需特定工具集请用
  `WindowsSDKTarget.props` 等本地手段，不要提交）；
- rename 规则详见 `rename_to_noobdawn.py` 头部注释，修改规则前先看
  受保护模式（`(?<![A-Z])RDC` / `(?<![a-z])rdc`），避免误伤
  WORDCHARS / hardcoded 之类的合法标识符。
