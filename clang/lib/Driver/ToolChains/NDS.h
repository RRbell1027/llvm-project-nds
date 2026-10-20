#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_NDS_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_NDS_H

#include "clang/Driver/Driver.h"
#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"
#include "llvm/Support/Compiler.h"

#include <memory>

namespace clang {
namespace driver {

namespace tools {
namespace nds {

class LLVM_LIBRARY_VISIBILITY Assembler final : public Tool {
public:
  Assembler(const ToolChain &TC) : Tool("nds::Assembler", "assembler", TC) {
    printf("Using NDS Assembler\n");
  }
  bool hasIntegratedAssembler() const override { return false; }
  bool hasIntegratedCPP() const override { return true; }
  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &TCArgs,
                    const char *LinkingOutput) const override;
};

class LLVM_LIBRARY_VISIBILITY Linker final : public Tool {
public:
  Linker(const ToolChain &TC) : Tool("nds::Linker", "linker", TC) {
    printf("Using NDS Linker\n");
  }
  bool hasIntegratedAssembler() const override { return true; }
  bool hasIntegratedCPP() const override { return false; }
  bool isLinkJob() const override { return true; }
  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &TCArgs,
                    const char *LinkingOutput) const override;
};

} // namespace nds
} // namespace tools

namespace toolchains {

class LLVM_LIBRARY_VISIBILITY NDSToolChain : public ToolChain {
  /// 實體 NDS 平台的交叉工具鏈組態。欄位與解析只定義在 NDS.cpp。
  /// LLVM offload triple 仍由 ToolChain 持有，不放在這份組態裡。
  struct PlatformConfig;

public:
  class GCCInstallationDetector {
    friend class NDSToolChain;

    llvm::Triple GCCTriple;
    std::string GCCInstallPath;
    std::string GCCParentLibPath;
    SmallVector<std::string, 4> GCCDefaultLibDirs;
    std::string SysRoot;

    /// 路徑與 triple 一律來自該 toolchain 實例的平台組態。
    void init(const PlatformConfig &Config);

  public:
    /// 平台組態的 toolchain-triple。路徑與工具前綴使用同一字串。
    const llvm::Triple &getTriple() const { return GCCTriple; }
    StringRef getInstallPath() const { return GCCInstallPath; }
    StringRef getParentLibPath() const { return GCCParentLibPath; }
    std::string getSysRoot() const { return SysRoot; }
  };

protected:
  /// 宣告在 Platform 之前，初始化順序與此一致。
  GCCInstallationDetector GCCInstallation;

private:
  friend class tools::nds::Assembler;
  friend class tools::nds::Linker;

  const PlatformConfig &getPlatformConfig() const;

  /// 每個 NDSToolChain 各自擁有一份，不使用全域可寫狀態。
  std::unique_ptr<PlatformConfig> Platform;

public:
  NDSToolChain(const Driver &D, const llvm::Triple &Triple,
               const llvm::opt::ArgList &Args);
  ~NDSToolChain() override;

  bool IsMathErrnoDefault() const override { return false; }
  bool isCrossCompiling() const override { return true; }
  bool isPICDefault() const override { return false; }
  bool isPIEDefault(const llvm::opt::ArgList &Args) const override {
    return true;
  }
  bool isPICDefaultForced() const override { return false; }
  bool IsIntegratedAssemblerDefault() const override { return false; }

  void AddClangCXXStdlibIncludeArgs(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &C11Args) const override;
  void AddClangSystemIncludeArgs(const llvm::opt::ArgList &Args,
                                llvm::opt::ArgStringList &CC1Args) const override;
  std::string computeSysRoot() const override {
    return GCCInstallation.getSysRoot();
  }

protected:
  Tool *buildAssembler() const override;
  Tool *buildLinker() const override;
};

} // namespace toolchains

} // namespace driver
} // namespace clang

#endif
