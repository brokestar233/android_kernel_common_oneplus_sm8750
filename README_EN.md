<div align="center">

# 🔥 OnePlus SM8750 Custom Kernel

A performance-oriented custom kernel for the OnePlus 13 (Snapdragon 8 Elite / SM8750)

[![简体中文][readme-cn-badge]][readme-cn-url]
[![Latest Release][release-badge]][release-url]
[![Total Downloads][downloads-badge]][downloads-url]
![SoC][soc-badge]
![License][license-badge]

</div>

[readme-cn-badge]: https://img.shields.io/badge/README-简体中文-blue.svg?style=for-the-badge&logo=readme
[readme-cn-url]: README.md
[release-badge]: https://img.shields.io/github/v/release/brokestar233/android_kernel_common_oneplus_sm8750?style=for-the-badge&label=Latest%20Release&logo=github
[release-url]: https://github.com/brokestar233/android_kernel_common_oneplus_sm8750/releases/latest
[downloads-badge]: https://img.shields.io/github/downloads/brokestar233/android_kernel_common_oneplus_sm8750/total?style=for-the-badge&label=Total%20Downloads&logo=download
[downloads-url]: https://github.com/brokestar233/android_kernel_common_oneplus_sm8750/releases
[soc-badge]: https://img.shields.io/badge/SoC-SM8750%20%7C%20Snapdragon%208%20Elite-red?style=for-the-badge
[license-badge]: https://img.shields.io/badge/Source-Closed%20%7C%20Free%20Release-orange?style=for-the-badge

---

> [!WARNING]
> The source code is not public, but all versions are released free of charge on the [Releases][release-url] page.

<!--
Reason for going closed source: some forks (e.g. https://github.com/xkse4/android_kernel_common_oneplus_sm8750 )
reused this project's modifications without keeping the author and Signed-off-by attribution.
Given this situation, the project is now closed source, though versions will continue to be
published free of charge via Releases.
-->

Feature summary of the current dev branch (`b74695f5`).

---

## 🛑 Disclaimer

Flashing a custom kernel carries inherent risks and may cause **a bricked device, data loss, or SafetyNet / Play Integrity failures**.
Always back up your data before flashing. The author is not responsible for any damage caused by using this kernel.

**By flashing, you acknowledge and accept these risks.**

---

## ✨ Features

### 🌟 Original Implementations

- **Device-tree overwriter** — patches device-tree properties, charging protocols, and configuration on the kernel side.
- **Module overlay** — module interception and replacement.
- **Firmware overlay** — firmware overlay with embedded `regdb.bin`.

### 💾 Memory & Storage

- **UKSM** — Ultra KSM same-page merging.
- KSM reverse-mapping walk optimizations and tests.
- **MGLRU optimizations** — reclaim, working-set protection, and vmscan tuning.
- **zram** — automatic disksize, LZ4 as the default compressor, page-type tracking, idle-page reclaim, and GC.
- **ZMS** — packed backing store, neighbor prefetch, and the zram writeback data path.
- **Kcompressd-Unofficial** — asynchronous swapout.
- **le9u/o** — memory-pressure handling.
- vmalloc, contiguous-page freeing, and ARM64 LZ4 decompression optimizations.

### ⚡ Scheduler & I/O

- **BORE** CPU scheduler.
- EAS, full-idle SMT-core selection, iowait boost, and scheduling migration.
- **ADIOS** I/O scheduler.

### 🌐 Networking & Wireless

- **BBRv3** TCP congestion control.
- BBR, FQ-CoDel, and default network-scheduler configuration.
- cfg80211 frequency-compatibility bridge.
- qcacld3 netlink monitor mode.

### 📱 Android, OPLUS & Device Support

- GKI configuration support for UKSM, wakelocks, schedulers, Oryon, ThinLTO, AutoFDO, and Polly.
- Module blacklist.
- Qualcomm SCM firmware-version pinning.
- Boeffla wakelock blocker.
- Sysctl-based global temperature offset.
- F2FS ATGC and GC_MERGE.

### 🛠️ Build & Base Components

- Oryon integer square-root optimization.
- **zstd 1.5.8**, with L1D-cache-based compression parameters.

---

## ⬇️ Download & Installation

Pre-built versions are published free of charge on the [Releases][release-url] page.

> [!TIP]
> Flashing via [Kernel Flasher](https://github.com/libxzr/HorizonKernelFlasher) or an AnyKernel3 zip through Recovery is recommended.

---

## 🙏 Acknowledgements

This kernel integrates open-source patches from the following developers / projects (*listed in no particular order*):

| Contributor | Contribution |
|:---|:---|
| [**firelzrd**](https://github.com/firelzrd) | BORE scheduler, ADIOS I/O scheduler, le9u/o, Kcompressd-Unofficial |
| [**CachyOS**](https://github.com/CachyOS/linux/commits/6.16/bbr3) | BBRv3 congestion control |
| [**sroeschus**](https://github.com/sroeschus/uksm) | UKSM (adapted from their 6.6 patch) |
| [**epicmann24**](https://github.com/epicmann24) | Boeffla wakelock blocker |
| [**tryle17**](https://github.com/tryle17) | Global temperature offset |
| [**whitewhale0612**](https://github.com/whitewhale0612) | ZMS |

Thanks also to the Linux kernel community and all the open-source projects that make this possible.
