# 发布签名（SignPath 开源计划）

本仓库使用 [SignPath](https://signpath.io) 的**开源免费计划**对发布物做 Authenticode 签名，
消除 Windows Defender / SmartScreen 对未签名安装包的误报。

## 签名架构

`.github/workflows/build-sign.yml` 在 GitHub Actions 上完成：

1. **构建**：cmake 编译 xfsWinPad + PluginHost，跑 ctest，`scripts/make-payload.ps1` 组装
   `dist\payload\` 并生成 `dist\setup.rsp`（相对路径，本地/CI 通用）。
2. **签名请求 1**：上传 `xfsWinPad.exe`、`xfsWinPadPluginHost.exe`、`xtaclean.exe`，
   用 artifact configuration `pes` 签名（清单见 `.signpath/artifact-configurations/pes.xml`）。
3. **安装器**：用**已签名**的 PE 重建 payload → csc 编出 `setup.exe`
   （先签内层、再嵌入，保证安装后的 exe 带有效签名）。
4. **签名请求 2**：上传 `setup.exe`，用 artifact configuration `setup` 签名。
5. **产物**：`dist/xfsWinPad-setup.exe`（已签）、`dist/xfsWinPad-portable.zip`（含已签 exe）；
   推送 `v*` tag 时自动挂到 GitHub Release。

未配置 SignPath 凭据时（组织 ID 变量为空）工作流照样运行，产出**未签名**构建——
审核通过前即可用来验证 CI 链路。

## 一次性配置（需要仓库所有者本人操作）

### 1. 申请 SignPath 开源计划

1. 用 GitHub 账号在 [signpath.io](https://signpath.io) 注册（选 open source / SignPath Foundation 计划）。
2. 申请时填写仓库 `Kyrieme/xfsWinPad`（必须是公开仓库）。
3. SignPath Foundation 审核需几天，通过后会提供：
   - **Organization ID**（Foundation 托管的组织）
   - 在其组织下建好 **Project**（项目 slug 建议填 `xfsWinPad`）
   - 默认 **signing policy**（slug 建议填 `release-signing`，以 Foundation 实际给的为准）

### 2. 配置仓库凭据

Settings → Secrets and variables → Actions：

| 类型 | 名称 | 值 |
|------|------|----|
| Variable | `SIGNPATH_ORGANIZATION_ID` | Foundation 提供的组织 ID |
| Variable | `SIGNPATH_PROJECT_SLUG` | 项目 slug（默认 `xfsWinPad`） |
| Variable | `SIGNPATH_SIGNING_POLICY_SLUG` | 策略 slug（默认 `release-signing`） |
| Secret | `SIGNPATH_API_TOKEN` | signpath.io 里创建的 API token |

### 3. 配置 artifact configurations

signpath.io 控制台 → Project → Artifact configurations，新建两个，slug 与本仓库文件对应：

- `pes` ← 粘贴 `.signpath/artifact-configurations/pes.xml`
- `setup` ← 粘贴 `.signpath/artifact-configurations/setup.xml`

（若 Foundation 建项目时已有默认 config，按 slug 命名对齐即可。）

### 4. 安装 SignPath GitHub App

按 onboarding 邮件指引，把 SignPath 的 GitHub App 授权给 `Kyrieme/xfsWinPad`
（Actions 签名请求经由官方 connector 提交，见
[signpath/github-action-submit-signing-request](https://github.com/signpath/github-action-submit-signing-request)）。

## 触发发布

```text
Actions 页手动运行 build-and-sign，或推送 v* tag：
git tag v0.3.1 && git push origin v0.3.1
```

产物出现在 workflow 的 Artifacts；tag 触发时同时挂到 Release。

## 本地等价流程（无 CI）

```text
cmake --build build --config Release --target xfsWinPad xfsWinPadPluginHost
powershell -File scripts\make-payload.ps1
```

产出 `dist\payload\`、`dist\setup.exe`、`dist\xfsWinPad-setup.exe`、`dist\xfsWinPad-portable.zip`。

## Defender 误报申诉（签名生效前的兜底）

提交 https://www.microsoft.com/en-us/wdsi/filesubmission ，注明软件官网/仓库地址，
附被误报文件的 SHA256（`Get-FileHash`）。签名生效后误报率会大幅下降。
