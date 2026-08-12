<div align="center">

# 🔥 OnePlus SM8750 客制化内核

基于 OnePlus 13(Snapdragon 8 Elite / SM8750)的性能向客制化内核

[![English][readme-en-badge]][readme-en-url]
[![Latest Release][release-badge]][release-url]
[![Total Downloads][downloads-badge]][downloads-url]
![SoC][soc-badge]
![License][license-badge]

</div>

[readme-en-badge]: https://img.shields.io/badge/README-English-blue.svg?style=for-the-badge&logo=readme
[readme-en-url]: README_EN.md
[release-badge]: https://img.shields.io/github/v/release/brokestar233/android_kernel_common_oneplus_sm8750?style=for-the-badge&label=Latest%20Release&logo=github&v=2
[release-url]: https://github.com/brokestar233/android_kernel_common_oneplus_sm8750/releases/latest
[downloads-badge]: https://img.shields.io/github/downloads/brokestar233/android_kernel_common_oneplus_sm8750/total?style=for-the-badge&label=Total%20Downloads&logo=download&v=2
[downloads-url]: https://github.com/brokestar233/android_kernel_common_oneplus_sm8750/releases
[soc-badge]: https://img.shields.io/badge/SoC-SM8750%20%7C%20Snapdragon%208%20Elite-red?style=for-the-badge
[license-badge]: https://img.shields.io/badge/Source-Closed%20%7C%20Free%20Release-orange?style=for-the-badge

---

> [!WARNING]
> 本项目闭源,但所有版本均在 [Releases][release-url] 页面免费公开发布。

<!--
闭源说明:此前发现部分项目(例如 https://github.com/xkse4/android_kernel_common_oneplus_sm8750 )
在套用本项目的修改后,未保留作者及 Signed-off-by 署名信息。出于这一情况,本项目转为闭源,
但仍会持续通过 Releases 免费发布版本。
-->

基于当前 dev 分支(`b74695f5`)的特性摘要。

---

## 🛑 免责声明

刷写自定义内核存在一定风险,可能导致**设备变砖、数据丢失或 SafetyNet / Play Integrity 校验失败**。
刷入前请务必备份重要数据。因使用本内核造成的任何损失,作者概不负责。

**刷入即代表你已知晓并愿意承担上述风险。**

---

## ✨ 特性

### 🌟 原创实现

- **Device-tree overwriter** —— 在内核侧覆写设备树属性、充电协议与配置生成。
- **Module overlay** —— 模块拦截替换。
- **Firmware overlay** —— 固件 overlay,内置 `regdb.bin`。

### 💾 内存与存储

- **UKSM** —— Ultra KSM 同页合并。
- KSM rmap 遍历优化与测试。
- **MGLRU 优化** —— 回收、工作集保护与 vmscan 调优。
- **zram** —— 自动 disksize、LZ4 默认压缩、页面类型跟踪、空闲页回收与 GC。
- **ZMS** —— packed backing store、邻近页预取与 zram writeback 数据路径。
- **Kcompressd-Unofficial** —— 异步 swapout。
- **le9u/o** —— 内存压力处理。
- vmalloc、连续页释放与 ARM64 LZ4 解压优化。

### ⚡ 调度与 I/O

- **BORE** CPU 调度器。
- EAS、SMT 空闲核选择、iowait boost 与调度迁移。
- **ADIOS** I/O 调度器。

### 🌐 网络与无线

- **BBRv3** TCP 拥塞控制。
- BBR、FQ-CoDel 与默认网络调度配置。
- cfg80211 频率兼容桥接。
- qcacld3 netlink monitor mode。

### 📱 Android、OPLUS 与设备适配

- GKI 配置:UKSM、wakelock、调度器、Oryon、ThinLTO、AutoFDO 与 Polly。
- 模块黑名单。
- Qualcomm SCM 固件版本绑定。
- Boeffla wakelock blocker。
- 基于 sysctl 的全局温度偏移。
- F2FS ATGC 与 GC_MERGE。

### 🛠️ 构建与基础组件

- Oryon 整数平方根优化。
- **zstd 1.5.8**,基于 L1D cache 的压缩参数。

---

## ⬇️ 下载与刷入

所有已编译版本均在 [Releases][release-url] 页面免费发布。

> [!TIP]
> 推荐使用 [Kernel Flasher](https://github.com/libxzr/HorizonKernelFlasher) 或通过 Recovery 刷入 AnyKernel3 刷机包。

---

## 🙏 致谢

本内核整合了以下开发者 / 项目的开源补丁(*排名不分先后*):

| 开发者 | 贡献 |
|:---|:---|
| [**firelzrd**](https://github.com/firelzrd) | BORE 调度器、ADIOS I/O 调度器、le9u/o、Kcompressd-Unofficial |
| [**CachyOS**](https://github.com/CachyOS/linux/commits/6.16/bbr3) | BBRv3 拥塞控制 |
| [**sroeschus**](https://github.com/sroeschus/uksm) | UKSM(基于其 6.6 补丁适配) |
| [**epicmann24**](https://github.com/epicmann24) | Boeffla wakelock blocker |
| [**tryle17**](https://github.com/tryle17) | 全局温度偏移 |
| [**whitewhale0612**](https://github.com/whitewhale0612) | ZMS |

同时感谢 Linux 内核社区及所有让这一切成为可能的开源项目。
