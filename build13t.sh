WORKDIR="$(pwd)"
 echo "编译器信息:"
          export PATH="$WORKDIR/clang/bin:$PATH"
          export CCACHE_COMPRESS=1     # 启用压缩
          export CCACHE_COMPRESSLEVEL=5   # 压缩级别
          export CCACHE_MAXSIZE=20G   # 缓存大小上限
          clang --version
          pahole_version=$(pahole --version 2>/dev/null | head -n1); [ -z "$pahole_version" ] && echo "pahole版本：未安装" || echo "pahole版本：$pahole_version"
        make ARCH=arm64 SUBARCH=arm64 O=out \
          LLVM=1 \
          LLVM_IAS=1 \
          CC="ccache clang" \
          CXX="ccache clang++" \
          HOSTCC="ccache clang" \
          HOSTCXX="ccache clang++" \
          CLANG_TRIPLE=aarch64-linux-gnu- \
          CROSS_COMPILE=aarch64-linux-gnu- \
          CROSS_COMPILE_ARM32=arm-linux-gnueabi- \
          CLANG_AUTOFDO_PROFILE="../android/gki/aarch64/afdo/kernel.afdo" \
          gki_defconfig
        make ARCH=arm64 SUBARCH=arm64 -j$(nproc --all) O=out \
          LLVM=1 \
          LLVM_IAS=1 \
          CC="ccache clang" \
          CXX="ccache clang++" \
          HOSTCC="ccache clang" \
          HOSTCXX="ccache clang++" \
          CLANG_TRIPLE=aarch64-linux-gnu- \
          CROSS_COMPILE=aarch64-linux-gnu- \
          CROSS_COMPILE_ARM32=arm-linux-gnueabi- \
          CLANG_AUTOFDO_PROFILE="../android/gki/aarch64/afdo/kernel.afdo"

 echo ">>> 内核编译成功！"