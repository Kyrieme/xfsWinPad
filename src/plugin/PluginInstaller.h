#pragma once
// xfsWinPad - 插件安装器：下载 + ZIP 解压 + 落盘安装/更新编排。
//
// 安装源（优先级从高到低）：
//   1) 本地已存在 <pluginsDir>\downloads\<folder>.zip —— 直接使用（离线安装）。
//   2) catalog 的 repository 字段（http/https）—— 用 WinHTTP 下载到上述路径。
//   3) 两者都没有 —— 报错「无下载地址」。
//
// 流程：取得 ZIP 路径 → 校验并解压到临时目录 → 规整内容根目录（见
// NormalizeExtractedRoot 的启发式）→ 拷入目标目录（ZIP 自带 plugins\ 根则直接
// 并入宿主 plugins\，否则落进 plugins\<folder>\）→ 更新本地记录
// （由调用方负责调用 PluginRegistry::MarkInstalled）→ 清理临时目录。
//
// 线程模型：Install() 可在后台线程调用（内部只做文件/网络/解压，不触 UI）。
// 进度通过 progress 回调上报（可空）；调用方负责把回调结果 marshal 回 UI 线程。
// 单测通过注入 Fake Downloader/ZipExtractor 脱离真实网络与磁盘 ZIP。

#include <functional>
#include <string>

namespace xfs {

// 安装/更新进度。phase 表示当前阶段；percent∈[0,100] 供进度条使用。
struct InstallProgress {
    enum Phase {
        kIdle = 0,
        kDownloading,   // 正在下载（percent=下载进度）
        kExtracting,    // 正在解压
        kInstalling,    // 正在写入插件目录
        kDone,          // 成功
        kFailed,        // 失败（message 带原因）
    };
    Phase phase = kIdle;
    int percent = 0;
    std::wstring message;
};

// 下载抽象：把 url 内容写入 dstPath（覆盖）。生产实现 WinHttpDownloader；
// 单测注入假实现（本地文件模拟网络）。
struct Downloader {
    virtual ~Downloader() = default;
    // 返回 true=成功。progress 可空；若提供，在下载期间上报 kDownloading。
    virtual bool Download(const std::wstring& url, const std::wstring& dstPath,
                          const std::function<void(const InstallProgress&)>& progress) = 0;
};

// ZIP 解压抽象：把 zipPath 解压到 outDir（不存在则创建）。生产实现
// MinizZipExtractor（miniz 读入内存 + 逐条写出，绕开 stdio 宽路径问题）；
// 单测注入假实现。
struct ZipExtractor {
    virtual ~ZipExtractor() = default;
    virtual bool Extract(const std::wstring& zipPath,
                         const std::wstring& outDir, std::wstring* errOut) = 0;
};

// 默认下载器：WinHTTP 实现 http/https 下载（支持本地路径/ file:// 直拷）。
class WinHttpDownloader : public Downloader {
public:
    bool Download(const std::wstring& url, const std::wstring& dstPath,
                  const std::function<void(const InstallProgress&)>& progress) override;
};

// 默认解压器：miniz 实现。ZIP 整体读入内存后逐条解压写出（自定义目录创建），
// 路径无 stdio 编码问题；拒绝 zip-slip（.. / 绝对路径 / 盘符）。
class MinizZipExtractor : public ZipExtractor {
public:
    bool Extract(const std::wstring& zipPath, const std::wstring& outDir,
                 std::wstring* errOut) override;
};

class PluginInstaller {
public:
    explicit PluginInstaller(std::wstring pluginsDir);

    // 安装或更新 folder 到 version。
    //   url：catalog 的 repository；为空则只用本地包。
    //   成功返回 true；失败返回 false 并在 errOut 写原因（可空）。
    //   progress 可空；非空则按阶段回调。
    bool Install(const std::wstring& folder, const std::wstring& version,
                 const std::wstring& url, const std::function<void(const InstallProgress&)>& progress,
                 std::wstring* errOut);

    // 下载/缓存目录：<pluginsDir>\downloads（不存在则创建）。
    std::wstring DownloadDir() const;

    // 测试注入（非拥有）。
    void SetDownloader(Downloader* d) { downloader_ = d; }
    void SetExtractor(ZipExtractor* z) { extractor_ = z; }

    // ---- 可独立测试的纯函数 ----
    // 规整解压内容根目录（启发式）：
    //   1) 内含名为 "plugins" 的子目录 → 根切到该子目录（N++ 打包惯例
    //      plugins\<folder>\*）；
    //   2) 否则根下仅一个子目录且无散文件 → 根切进该子目录；
    //   3) 否则原样。
    static std::wstring NormalizeExtractedRoot(const std::wstring& tempDir,
                                               bool* hadPluginsRoot);

private:
    bool EnsureLocalPackage(const std::wstring& folder, const std::wstring& url,
                            std::wstring& zipPath,
                            const std::function<void(const InstallProgress&)>& progress,
                            std::wstring* errOut);

    std::wstring pluginsDir_;
    Downloader* downloader_ = nullptr;    // 空 → defaultDownloader_
    ZipExtractor* extractor_ = nullptr;   // 空 → defaultExtractor_
    WinHttpDownloader defaultDownloader_; // 默认实现（不注入时使用）
    MinizZipExtractor defaultExtractor_;
};

} // namespace xfs
