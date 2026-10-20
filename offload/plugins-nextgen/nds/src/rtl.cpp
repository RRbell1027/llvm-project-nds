// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// NDS: image contents and mapped buffers are transferred to a remote executor.
#include "PluginInterface.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Frontend/OpenMP/OMPDeviceConstants.h"
#include "nd_runtime.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace llvm::omp::target::plugin {
using namespace error;
struct NDSDeviceTy;
struct NDSImageTy : DeviceImageTy {
  uint64_t RemoteImage;
  NDSImageTy(int32_t Id, GenericDeviceTy &D, const __tgt_device_image *I,
             uint64_t Remote) : DeviceImageTy(Id, D, I), RemoteImage(Remote) {}
};
struct NDSKernelTy : GenericKernelTy {
  uint64_t Function = 0;
  NDSKernelTy(const char *Name) : GenericKernelTy(Name) {}
  Error initImpl(GenericDeviceTy &, DeviceImageTy &) override;
  Error launchImpl(GenericDeviceTy &, uint32_t[3], uint32_t[3], KernelArgsTy &,
                   KernelLaunchParamsTy, AsyncInfoWrapperTy &) const override;
};
struct NDSDeviceTy : GenericDeviceTy {
  static constexpr GV Grid = {1, 1, 1, 1, 1, 1, 1};
  nd_runtime *Client = nullptr;
  NDSDeviceTy(GenericPluginTy &P, int32_t Id, int32_t Count)
      : GenericDeviceTy(P, Id, Count, Grid) {}
  Error rpcError(const char *Op) const {
    return Plugin::error(ErrorCode::UNKNOWN, "NDS %s: %s (errno %d)", Op,
                         nd_rt_error(Client), errno);
  }
  Error initImpl(GenericPluginTy &) override {
    const char *Path = std::getenv("ND_RUNTIME_DEVICE_PATH");
    if (!Path)
      return Plugin::error(ErrorCode::INVALID_VALUE, "ND_RUNTIME_DEVICE_PATH is required");
    Client = nd_rt_open(Path);
    if (!Client) return rpcError("connect");
    if (nd_rt_device_bits(Client) != 32) {
      nd_rt_close(Client); Client = nullptr;
      return Plugin::error(ErrorCode::UNSUPPORTED, "NDS requires a 32-bit ARM executor");
    }
    return Plugin::success();
  }
  Error deinitImpl() override { nd_rt_close(Client); Client = nullptr; return Plugin::success(); }
  Error setContext() override { return Plugin::success(); }
  std::string getComputeUnitKind() const override { return "generic-32bit"; }
  Expected<GenericKernelTy &> constructKernel(const char *Name) override {
    auto *K = Plugin.allocate<NDSKernelTy>();
    if (!K) return Plugin::error(ErrorCode::OUT_OF_RESOURCES, "NDS kernel allocation failed");
    new (K) NDSKernelTy(Name); return *K;
  }
  Expected<DeviceImageTy *> loadBinaryImpl(const __tgt_device_image *Input, int32_t Id) override {
    if (!Input || !Input->ImageStart || !Input->ImageEnd ||
        reinterpret_cast<uintptr_t>(Input->ImageEnd) <= reinterpret_cast<uintptr_t>(Input->ImageStart))
      return Plugin::error(ErrorCode::INVALID_BINARY, "NDS requires nonempty ELF image bytes");
    const size_t Bytes = reinterpret_cast<uintptr_t>(Input->ImageEnd) - reinterpret_cast<uintptr_t>(Input->ImageStart);
    uint64_t Remote;
    // Transfer the entire embedded ELF. No pathname header or device-side lookup.
    if (nd_rt_load_image(Client, Input->ImageStart, Bytes, &Remote)) return rpcError("load image bytes");
    auto *Image = Plugin.allocate<NDSImageTy>();
    if (!Image) {
      nd_rt_unload_image(Client, Remote);
      return Plugin::error(ErrorCode::OUT_OF_RESOURCES, "NDS image allocation failed");
    }
    new (Image) NDSImageTy(Id, *this, Input, Remote); return Image;
  }
  Error unloadBinaryImpl(DeviceImageTy *Image) override {
    if (nd_rt_unload_image(Client, static_cast<NDSImageTy *>(Image)->RemoteImage)) return rpcError("unload");
    Plugin.free(Image); return Plugin::success();
  }
  void *allocate(size_t Size, void *, TargetAllocTy Kind) override {
    if (!Size || (Kind != TARGET_ALLOC_DEFAULT && Kind != TARGET_ALLOC_DEVICE && Kind != TARGET_ALLOC_DEVICE_NON_BLOCKING)) return nullptr;
    uint64_t Address;
    if (nd_rt_alloc(Client, Size, &Address)) return nullptr;
    return reinterpret_cast<void *>(static_cast<uintptr_t>(Address));
  }
  int free(void *P, TargetAllocTy) override {
    return P && nd_rt_free(Client, reinterpret_cast<uintptr_t>(P)) ? OFFLOAD_FAIL : OFFLOAD_SUCCESS;
  }
  Expected<void *> dataLockImpl(void *, int64_t) override {
    return Plugin::error(ErrorCode::UNSUPPORTED, "NDS has no host-shared memory");
  }
  Error dataUnlockImpl(void *) override { return Plugin::success(); }
  Expected<bool> isPinnedPtrImpl(void *, void *&, void *&, size_t &) const override { return false; }
  Error dataSubmitImpl(void *Device, const void *Host, int64_t Bytes, AsyncInfoWrapperTy &) override {
    if (Bytes < 0) return Plugin::error(ErrorCode::INVALID_VALUE, "negative NDS transfer size");
    if (nd_rt_submit(Client, reinterpret_cast<uintptr_t>(Device), Host, Bytes)) return rpcError("submit");
    return Plugin::success();
  }
  Error dataRetrieveImpl(void *Host, const void *Device, int64_t Bytes, AsyncInfoWrapperTy &) override {
    if (Bytes < 0) return Plugin::error(ErrorCode::INVALID_VALUE, "negative NDS transfer size");
    if (nd_rt_retrieve(Client, Host, reinterpret_cast<uintptr_t>(Device), Bytes)) return rpcError("retrieve");
    return Plugin::success();
  }
  Error dataExchangeImpl(const void *SrcPtr, GenericDeviceTy &DstGenericDevice,
                         void *DstPtr, int64_t Size,
                         AsyncInfoWrapperTy &AsyncInfoWrapper) override {
    // This function should never be called because the function
    // NDSPluginTy::isDataExchangable() returns false.
    return Plugin::error(ErrorCode::UNSUPPORTED,
                         "dataExchangeImpl not supported");
  }

  /// All functions are already synchronous. No need to do anything on this
  /// synchronization function.
  Error synchronizeImpl(__tgt_async_info &AsyncInfo) override {
    return Plugin::success();
  }

  /// All functions are already synchronous. No need to do anything on this
  /// query function.
  Error queryAsyncImpl(__tgt_async_info &AsyncInfo) override {
    return Plugin::success();
  }

  /// This plugin does not support interoperability
  Error initAsyncInfoImpl(AsyncInfoWrapperTy &AsyncInfoWrapper) override {
    return Plugin::error(ErrorCode::UNSUPPORTED,
                         "initAsyncInfoImpl not supported");
  }

  /// This plugin does not support interoperability
  Error initDeviceInfoImpl(__tgt_device_info *DeviceInfo) override {
    return Plugin::error(ErrorCode::UNSUPPORTED,
                         "initDeviceInfoImpl not supported");
  }

  /// This plugin does not support the event API. Do nothing without failing.
  Error createEventImpl(void **EventPtrStorage) override {
    *EventPtrStorage = nullptr;
    return Plugin::success();
  }
  Error destroyEventImpl(void *EventPtr) override { return Plugin::success(); }
  Error recordEventImpl(void *EventPtr,
                        AsyncInfoWrapperTy &AsyncInfoWrapper) override {
    return Plugin::success();
  }
  Error waitEventImpl(void *EventPtr,
                      AsyncInfoWrapperTy &AsyncInfoWrapper) override {
    return Plugin::success();
  }
  Error syncEventImpl(void *EventPtr) override { return Plugin::success(); }

  /// Print information about the device.
  Expected<InfoTreeNode> obtainInfoImpl() override {
    InfoTreeNode Info;
    Info.add("Device Type", "Luckfox NDS ARM32");
    return Info;
  }

  /// This plugin should not setup the device environment or memory pool.
  virtual bool shouldSetupDeviceEnvironment() const override { return false; };
  virtual bool shouldSetupDeviceMemoryPool() const override { return false; };

  /// Getters and setters for stack size and heap size not relevant.
  Error getDeviceStackSize(uint64_t &Value) override {
    Value = 0;
    return Plugin::success();
  }
  Error setDeviceStackSize(uint64_t Value) override {
    return Plugin::success();
  }
  Error getDeviceHeapSize(uint64_t &Value) override {
    Value = 0;
    return Plugin::success();
  }
  Error setDeviceHeapSize(uint64_t Value) override { return Plugin::success(); }

};
Error NDSKernelTy::initImpl(GenericDeviceTy &Generic, DeviceImageTy &Image) {
  auto &D = static_cast<NDSDeviceTy &>(Generic);
  if (nd_rt_symbol(D.Client, static_cast<NDSImageTy &>(Image).RemoteImage, getName(), 0, &Function)) return D.rpcError("kernel symbol");
  KernelEnvironment.Configuration.ExecMode = OMP_TGT_EXEC_MODE_GENERIC;
  KernelEnvironment.Configuration.MayUseNestedParallelism = 2;
  KernelEnvironment.Configuration.UseGenericStateMachine = 2;
  MaxNumThreads = 1; return Plugin::success();
}
Error NDSKernelTy::launchImpl(GenericDeviceTy &Generic, uint32_t Threads[3], uint32_t Blocks[3],
    KernelArgsTy &Args, KernelLaunchParamsTy Params, AsyncInfoWrapperTy &) const {
  auto &D = static_cast<NDSDeviceTy &>(Generic);
  if (Threads[0] > 1 || Threads[1] > 1 || Threads[2] > 1 ||
      Blocks[0] > 1 || Blocks[1] > 1 || Blocks[2] > 1)
    return Plugin::error(ErrorCode::UNSUPPORTED, "NDS initial executor is single-threaded");
  if (Args.NumArgs > 16 || (Args.NumArgs && !Params.Ptrs))
    return Plugin::error(ErrorCode::UNSUPPORTED, "NDS supports at most 16 mapped-pointer arguments");
  auto **Storage = reinterpret_cast<void **>(Params.Ptrs);
  SmallVector<uint64_t, 16> WireArgs;
  for (uint32_t I = 0; I < Args.NumArgs; ++I) {
    if (!Storage[I]) return Plugin::error(ErrorCode::INVALID_VALUE, "NDS argument storage missing");
    uintptr_t Address;
    std::memcpy(&Address, Storage[I], sizeof(Address));
    WireArgs.push_back(Address);
  }
  if (nd_rt_launch(D.Client, Function, WireArgs.data(), WireArgs.size())) return D.rpcError("launch");
  return Plugin::success();
}
class NDSGlobalHandlerTy final : public GenericGlobalHandlerTy {
  Error getGlobalMetadataFromDevice(GenericDeviceTy &Generic, DeviceImageTy &Image, GlobalTy &Global) override {
    auto &D = static_cast<NDSDeviceTy &>(Generic); uint64_t Value;
    if (nd_rt_symbol(D.Client, static_cast<NDSImageTy &>(Image).RemoteImage,
                     Global.getName().c_str(), Global.getSize(), &Value)) return D.rpcError("global symbol");
    Global.setPtr(reinterpret_cast<void *>(static_cast<uintptr_t>(Value)));
    return Plugin::success();
  }
};
struct NDSPluginTy final : GenericPluginTy {
  NDSPluginTy() : GenericPluginTy(Triple::arm) {}
  Expected<int32_t> initImpl() override { return std::getenv("ND_RUNTIME_DEVICE_PATH") ? 1 : 0; }
  Error deinitImpl() override { return Plugin::success(); }
  GenericDeviceTy *createDevice(GenericPluginTy &P, int32_t Id, int32_t Count) override { return new NDSDeviceTy(P, Id, Count); }
  GenericGlobalHandlerTy *createGlobalHandler() override { return new NDSGlobalHandlerTy(); }
  uint16_t getMagicElfBits() const override { return ELF::EM_ARM; }
  Triple::ArchType getTripleArch() const override { return Triple::arm; }
  const char *getName() const override { return "NDS"; }
  bool isDataExchangable(int32_t, int32_t) override { return false; }
  Expected<bool> isELFCompatible(uint32_t, StringRef Image) const override {
    if (Image.size() < sizeof(ELF::Elf32_Ehdr)) return false;
    const auto *P = reinterpret_cast<const unsigned char *>(Image.data());
    return !std::memcmp(P, "\177ELF", 4) && P[4] == ELF::ELFCLASS32 &&
           P[5] == ELF::ELFDATA2LSB && P[16] == ELF::ET_DYN && !P[17] &&
           P[18] == ELF::EM_ARM && !P[19];
  }
};
} // namespace llvm::omp::target::plugin
extern "C" llvm::omp::target::plugin::GenericPluginTy *createPlugin_nds() {
  return new llvm::omp::target::plugin::NDSPluginTy();
}
