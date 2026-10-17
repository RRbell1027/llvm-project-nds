#include "NDS.h"
#include "clang/Driver/CommonArgs.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang::driver::tools;
using namespace llvm::opt;

/// 實體平台組態。定義留在這個編譯單元，標頭只保留不完整型別。
///
/// ToolChain::getTriple() 是 LLVM offload triple（例如 aarch64-nds-linux），
/// 用來識別 NDS offload target。
/// ToolchainTriple 來自平台組態檔，是實體 GCC/binutils 的 target triple，
/// 寫入 GCCInstallationDetector。
/// 目錄與工具路徑由組態檔提供。只有 path-layout=buildroot 才會呼叫
/// deriveBuildrootToolchainPaths；上游值齊全時才補未覆寫的路徑。
/// 推導後，explicit 與 buildroot 使用同一組必填路徑檢查。
struct NDSToolChain::PlatformConfig {
  /// 組態鍵 toolchain-triple。沒有內建預設值。
  std::string ToolchainTriple;
  std::string GCCVersion;

  std::string BaseEnvPath;
  std::string ToolchainPath;
  std::string GCCInstallPath;
  std::string GCCParentLibPath;
  std::string SysRoot;
  std::string AssemblerPath;
  std::string LinkerDriverPath;

  std::string DynamicLinker;
  std::string RPath;
  llvm::SmallVector<std::string, 4> LibrarySearchPaths;
  /// 函式庫名稱，不含 -l 前綴。
  llvm::SmallVector<std::string, 8> LinkLibraries;

  std::string SkippedInput;

  struct PathOverrides {
    bool ToolchainPath = false;
    bool InstallPath = false;
    bool ParentLibPath = false;
    bool SysRoot = false;
    bool Assembler = false;
    bool Linker = false;
  };

  static std::unique_ptr<PlatformConfig> create(const Driver &D,
                                                const ArgList &Args);

private:
  static void deriveBuildrootToolchainPaths(
    PlatformConfig &Cfg, const PathOverrides &Overrides);
  static std::string lookupConfigFile(const ArgList &Args);
  static std::unique_ptr<PlatformConfig> loadFromFile(const Driver &D,
                                                      StringRef Path);
};

void NDSToolChain::PlatformConfig::deriveBuildrootToolchainPaths(
    PlatformConfig &Cfg, const PathOverrides &Overrides) {
  // 舊版 Buildroot 目錄佈局。每一步只在該路徑未覆寫且上游值都存在時推導。
  // 字串保留 ".."，不做路徑正規化。
  if (!Overrides.ToolchainPath && !Cfg.BaseEnvPath.empty() &&
      !Cfg.ToolchainTriple.empty())
    Cfg.ToolchainPath = Cfg.BaseEnvPath + "/opt/" + Cfg.ToolchainTriple;
  if (!Overrides.InstallPath && !Cfg.ToolchainPath.empty() &&
      !Cfg.ToolchainTriple.empty() && !Cfg.GCCVersion.empty()) {
    Cfg.GCCInstallPath = Cfg.ToolchainPath + "/libexec/gcc/" +
                         Cfg.ToolchainTriple + "/" + Cfg.GCCVersion;
  }
  if (!Overrides.ParentLibPath && !Cfg.GCCInstallPath.empty())
    Cfg.GCCParentLibPath = Cfg.GCCInstallPath + "/../../..";
  if (!Overrides.SysRoot && !Cfg.GCCParentLibPath.empty() &&
      !Cfg.ToolchainTriple.empty())
    Cfg.SysRoot =
        Cfg.GCCParentLibPath + "/../" + Cfg.ToolchainTriple + "/sysroot";
  if (!Overrides.Assembler && !Cfg.SysRoot.empty())
    Cfg.AssemblerPath = Cfg.SysRoot + "/../bin/as";
  if (!Overrides.Linker && !Cfg.SysRoot.empty() && !Cfg.ToolchainTriple.empty())
    Cfg.LinkerDriverPath =
        Cfg.SysRoot + "/../../bin/" + Cfg.ToolchainTriple + "-g++";
}

std::string NDSToolChain::PlatformConfig::lookupConfigFile(const ArgList &Args) {
  // -nds-platform=<file> 尚未定義於 Options.td。Driver 在建立 toolchain 之前
  // 就會拒絕未知選項，因此這裡還不能讀到該旗標。
  // Options.td 加入 Joined 選項 nds_platform_EQ 之後，只改這個函式：
  //   if (const Arg *A = Args.getLastArg(options::OPT_nds_platform_EQ))
  //     return std::string(A->getValue());
  if (const Arg *A = Args.getLastArg(options::OPT_nds_platform_EQ))
    return A->getValue();
  return {};
}

std::unique_ptr<NDSToolChain::PlatformConfig>
NDSToolChain::PlatformConfig::loadFromFile(const Driver &D, StringRef Path) {
  // 組態檔為一行一個 key=value，'#' 開頭為註解。
  // path-layout 省略時視為 explicit。
  // buildroot 先依上游是否齊全推導未覆寫的路徑，可從任一中間路徑開始覆寫。
  // 兩種 layout 最後都檢查：toolchain-triple、gcc-install-path、
  //   gcc-parent-lib-path、sysroot、assembler-path、linker-driver-path。
  auto Cfg = std::make_unique<PlatformConfig>();
  PathOverrides Overrides;
  bool UseBuildroot = false;
  bool ReplaceLibraries = false;
  bool ReplaceLibDirs = false;
  std::string Error;

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> BufferOrErr =
      llvm::MemoryBuffer::getFile(Path);
  if (!BufferOrErr) {
    Error = std::string("無法讀取（") + BufferOrErr.getError().message() + "）";
  } else {
    unsigned LineNo = 0;
    StringRef Rest = (*BufferOrErr)->getBuffer();
    while (Error.empty() && !Rest.empty()) {
      std::pair<StringRef, StringRef> Parts = Rest.split('\n');
      StringRef Line = Parts.first;
      Rest = Parts.second;
      ++LineNo;
      if (Line.ends_with('\r'))
        Line = Line.drop_back();
      Line = Line.trim();
      if (Line.empty() || Line.starts_with('#'))
        continue;

      size_t Eq = Line.find('=');
      if (Eq == StringRef::npos) {
        Error = "第 " + std::to_string(LineNo) + " 行缺少 '='";
        break;
      }
      StringRef Key = Line.take_front(Eq).trim();
      StringRef Value = Line.drop_front(Eq + 1).trim();

      if (Key == "toolchain-triple") {
        Cfg->ToolchainTriple = Value.str();
      } else if (Key == "gcc-version") {
        Cfg->GCCVersion = Value.str();
      } else if (Key == "path-layout") {
        if (Value == "buildroot")
          UseBuildroot = true;
        else if (Value == "explicit")
          UseBuildroot = false;
        else
          Error = "第 " + std::to_string(LineNo) +
                  " 行的 path-layout 僅支援 explicit 或 buildroot";
      } else if (Key == "base-env-path") {
        Cfg->BaseEnvPath = Value.str();
      } else if (Key == "toolchain-path") {
        Cfg->ToolchainPath = Value.str();
        Overrides.ToolchainPath = !Value.empty();
      } else if (Key == "gcc-install-path") {
        Cfg->GCCInstallPath = Value.str();
        Overrides.InstallPath = !Value.empty();
      } else if (Key == "gcc-parent-lib-path") {
        Cfg->GCCParentLibPath = Value.str();
        Overrides.ParentLibPath = !Value.empty();
      } else if (Key == "sysroot") {
        Cfg->SysRoot = Value.str();
        Overrides.SysRoot = !Value.empty();
      } else if (Key == "assembler-path") {
        Cfg->AssemblerPath = Value.str();
        Overrides.Assembler = !Value.empty();
      } else if (Key == "linker-driver-path") {
        Cfg->LinkerDriverPath = Value.str();
        Overrides.Linker = !Value.empty();
      } else if (Key == "dynamic-linker") {
        Cfg->DynamicLinker = Value.str();
      } else if (Key == "rpath") {
        Cfg->RPath = Value.str();
      } else if (Key == "lib-dir") {
        if (!ReplaceLibDirs) {
          Cfg->LibrarySearchPaths.clear();
          ReplaceLibDirs = true;
        }
        if (!Value.empty())
          Cfg->LibrarySearchPaths.push_back(Value.str());
      } else if (Key == "library") {
        if (!ReplaceLibraries) {
          Cfg->LinkLibraries.clear();
          ReplaceLibraries = true;
        }
        if (!Value.empty())
          Cfg->LinkLibraries.push_back(Value.str());
      } else if (Key == "skip-input") {
        Cfg->SkippedInput = Value.str();
      } else {
        Error = "第 " + std::to_string(LineNo) + " 行含有未知的設定鍵「" +
                Key.str() + "」";
      }
    }
  }

  if (Error.empty() && UseBuildroot)
    deriveBuildrootToolchainPaths(*Cfg, Overrides);

  if (Error.empty() && Cfg->ToolchainTriple.empty())
    Error = "toolchain-triple 不可為空";
  else if (Error.empty() && Cfg->GCCInstallPath.empty())
    Error = "gcc-install-path 不可為空";
  else if (Error.empty() && Cfg->GCCParentLibPath.empty())
    Error = "gcc-parent-lib-path 不可為空";
  else if (Error.empty() && Cfg->SysRoot.empty())
    Error = "sysroot 不可為空";
  else if (Error.empty() && Cfg->AssemblerPath.empty())
    Error = "assembler-path 不可為空";
  else if (Error.empty() && Cfg->LinkerDriverPath.empty())
    Error = "linker-driver-path 不可為空";

  if (!Error.empty()) {
    D.Diag(clang::diag::err_drv_invalid_argument_to_option) << Path
                                                           << "nds-platform";
    llvm::errs() << "錯誤：NDS 平台組態檔「" << Path << "」：" << Error << "\n";
    return std::make_unique<PlatformConfig>();
  }

  return Cfg;
}

std::unique_ptr<NDSToolChain::PlatformConfig>
NDSToolChain::PlatformConfig::create(const Driver &D, const ArgList &Args) {
  std::string ConfigPath = lookupConfigFile(Args);
  if (ConfigPath.empty()) {
    D.Diag(clang::diag::err_drv_missing_argument) << "-nds-platform" << 1;
    llvm::errs() << "錯誤：NDS 必須指定 -nds-platform=<組態檔>。"
                    "toolchain-triple 由該檔提供，沒有可替代的預設平台。\n";
    return std::make_unique<PlatformConfig>();
  }
  return loadFromFile(D, ConfigPath);
}

NDSToolChain::~NDSToolChain() = default;

const NDSToolChain::PlatformConfig &NDSToolChain::getPlatformConfig() const {
  return *Platform;
}

void nds::Assembler::ConstructJob(Compilation &C, const JobAction &JA,
                                  const InputInfo &Output,
                                  const InputInfoList &Inputs,
                                  const llvm::opt::ArgList &Args,
                                  const char *AssembleOutput) const {
  claimNoWarnArgs(Args);
  if (Inputs.size() != 1)
    llvm_unreachable("Invalid number of input files.");

  const auto &NDS = static_cast<const NDSToolChain &>(getToolChain());
  const auto &Platform = NDS.getPlatformConfig();
  if (Platform.AssemblerPath.empty())
    return;

  llvm::opt::ArgStringList CmdArgs({});
  CmdArgs.push_back(Inputs[0].getFilename());
  CmdArgs.append({"-o", Output.getFilename()});

  const char *Exec = Args.MakeArgString(Platform.AssemblerPath);
  C.addCommand(std::make_unique<Command>(JA, *this, ResponseFileSupport::None(),
                                         Exec, CmdArgs, Inputs[0], Output));
}

void nds::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                               const InputInfo &Output,
                               const InputInfoList &Inputs, const ArgList &Args,
                               const char *LinkingOutput) const {
  claimNoWarnArgs(Args);
  const auto &NDS = static_cast<const NDSToolChain &>(getToolChain());
  const auto &Platform = NDS.getPlatformConfig();
  if (Platform.LinkerDriverPath.empty())
    return;

  ArgStringList OrigCmdArgs;
  // 共用物件的連結流程。平台差異（rpath、動態連結器、函式庫、g++）來自組態。
  OrigCmdArgs.push_back("-Wl,--no-undefined");
  OrigCmdArgs.push_back("-Wl,-Bsymbolic");
  if (!Platform.RPath.empty())
    OrigCmdArgs.push_back(Args.MakeArgString("-Wl,-rpath," + Platform.RPath));
  if (!Platform.DynamicLinker.empty())
    OrigCmdArgs.push_back(Args.MakeArgString("-Wl,-dynamic-linker," +
                                             Platform.DynamicLinker));

  OrigCmdArgs.push_back("-shared");
  OrigCmdArgs.push_back("-o");
  OrigCmdArgs.push_back(Output.getFilename());

  for (const auto &Input : Inputs) {
    if (!Input.isFilename())
      continue;
    std::string FileName = Input.getFilename();
    if (!Platform.SkippedInput.empty() && FileName == Platform.SkippedInput)
      continue;
    OrigCmdArgs.push_back(Args.MakeArgString(FileName));
  }

  // g++ 驅動程式不會自動補上這些函式庫，因為參數是手動組出來的。
  for (const std::string &Lib : Platform.LinkLibraries)
    OrigCmdArgs.push_back(Args.MakeArgString("-l" + Lib));

  const char *LinkerDriver = Args.MakeArgString(Platform.LinkerDriverPath);
  C.addCommand(std::make_unique<Command>(JA, *this, ResponseFileSupport::None(),
                                         LinkerDriver, OrigCmdArgs, Inputs,
                                         Output));
}

void NDSToolChain::GCCInstallationDetector::init(const PlatformConfig &Config) {
  GCCTriple.setTriple(Config.ToolchainTriple);
  GCCInstallPath = Config.GCCInstallPath;
  GCCParentLibPath = Config.GCCParentLibPath;
  SysRoot = Config.SysRoot;
  GCCDefaultLibDirs.clear();
  for (const std::string &Dir : Config.LibrarySearchPaths)
    GCCDefaultLibDirs.push_back(Dir);
}

NDSToolChain::NDSToolChain(const Driver &D, const llvm::Triple &Triple,
                           const llvm::opt::ArgList &Args)
    : ToolChain(D, Triple, Args), GCCInstallation(),
      Platform(PlatformConfig::create(D, Args)) {
  printf("Using NDS ToolChain\n");
  GCCInstallation.init(*Platform);
}

void NDSToolChain::AddClangCXXStdlibIncludeArgs(
    const llvm::opt::ArgList &DriverArgs,
    llvm::opt::ArgStringList &C11Args) const {
  printf("AddClangCXXStdlibIncludeArgs\n");
}

Tool *NDSToolChain::buildAssembler() const { return new nds::Assembler(*this); }

Tool *NDSToolChain::buildLinker() const { return new nds::Linker(*this); }
