# DTS提取与修改指南

本文档提供了一种在Android设备上提取和修改设备树（DTS）的操作指南，适用于指定机型。请按照以下步骤操作，并参考附录中的机型与项目名称（prjname）对应表。

## 一、提取DTS文件

1. **安装Termux**  
   在Android设备上下载并安装Termux应用，并授予其Root权限。

2. **安装必要工具**  
   在Termux中执行以下命令安装设备树编译器（dtc）和sudo：
   ```bash
   apt install dtc sudo
   ```

3. **提取DTS文件**  
   执行以下命令，将设备树从`/proc/device-tree`导出为`current_dts.dts`文件：
   ```bash
   sudo dtc -I fs /proc/device-tree > current_dts.dts
   ```

4. **传输文件**  
   将生成的`current_dts.dts`文件复制到电脑上，以备后续修改。

## 二、安装运行环境

1. **安装Python**  
   从Python官方网站（[https://www.python.org/downloads/](https://www.python.org/downloads/)）下载并安装最新版本的Python。

2. **安装tkinter库**  
   在终端执行以下命令安装Python的tkinter库：
   ```bash
   pip install tk
   ```

## 三、修改DTS文件

1. **启动DTS编辑工具**  
   在终端运行DTS编辑脚本：
   ```bash
   python tools/dts_editer.py
   ```

2. **打开DTS文件**  
   在编辑工具界面，点击左上角的“文件”菜单，选择“打开”，然后加载第一步中提取的`current_dts.dts`文件。

3. **修改并导出**  
   根据需求修改DTS文件内容，完成后点击左上角“文件”菜单，选择“导出为补丁”。

4. **保存补丁文件**  
   将导出的补丁文件后缀改为`.conf`，并将其放入对应机型项目名称（prjname）的文件夹中。参考下方的机型与项目名称对应表。

## 附录：机型与项目名称（prjname）对应表

| 机型                 | 项目名称 (prjname) |
|----------------------|--------------------|
| 一加 13T             | 24821             |
| 一加 13              | 23821             |
| 一加 12              | 22825             |
| 一加 Ace 5 竞速版    | 24825             |
| 一加 Ace 5 至尊版    | 24813             |
| 一加 Ace 5 Pro       | 24811             |
| 一加 Ace 5           | 23851             |
| 一加 Ace 3 Pro       | 23814             |
| 一加 Ace 3           | 23801             |
| 真我 GT7 Pro         | 24600             |

## 鸣谢

* 酷安bybycode,提供了一加13的充电参数
