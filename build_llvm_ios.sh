#!/bin/bash
# ============================================================
# LLVM iOS ARM64 反汇编静态库构建脚本
#
# 独立仓库: iosgg-llvm-builder
# 产物: output/libLLVMDisasm.a + output/include/llvm-c/
# 用法: 将此目录作为独立 git 仓库推送到 GitHub
#       GitHub Actions (macos-14) 自动编译
#       下载 artifact 后解压到主项目的 External/llvm/
#
# 策略: Homebrew 预编译 TableGen + iOS 交叉编译
#   阶段1: brew install llvm (预编译, ~2分钟)
#   阶段2: 使用 brew 的 TableGen 交叉编译 LLVM 库 (iOS arm64)
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLVM_VERSION="${1:-llvmorg-18.1.8}"
LLVM_DIR="${SCRIPT_DIR}/llvm-src"
IOS_BUILD="${SCRIPT_DIR}/llvm-build-ios"
OUTPUT_DIR="${SCRIPT_DIR}/output"

# ---- 检查平台 ----
if [[ "$(uname)" != "Darwin" ]]; then
    echo "错误: 此脚本需要 macOS 环境 (需要完整 iOS SDK)"
    echo "请在 GitHub Actions macos-14 runner 或本地 Mac 上运行"
    exit 1
fi

command -v cmake >/dev/null 2>&1 || { echo "错误: 需要 cmake"; exit 1; }
command -v xcrun  >/dev/null 2>&1 || { echo "错误: 需要 Xcode / Command Line Tools"; exit 1; }

IOS_SDK=$(xcrun --sdk iphoneos --show-sdk-path 2>/dev/null)
if [ -z "${IOS_SDK}" ] || [ ! -d "${IOS_SDK}" ]; then
    echo "错误: 找不到 iPhoneOS SDK"
    exit 1
fi
echo "iOS SDK: ${IOS_SDK}"
echo "LLVM 版本: ${LLVM_VERSION}"
CPU_COUNT=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
echo "CPU 核心数: ${CPU_COUNT}"

# ---- 下载 LLVM 源码 ----
if [ ! -f "${LLVM_DIR}/llvm/CMakeLists.txt" ]; then
    echo "下载 LLVM 源码 (${LLVM_VERSION})..."
    mkdir -p "${LLVM_DIR}"
    curl -fsSL "https://github.com/llvm/llvm-project/archive/refs/tags/${LLVM_VERSION}.tar.gz" \
        | tar -xz -C "${LLVM_DIR}" --strip-components=1
    echo "LLVM 源码就绪: ${LLVM_DIR}"
else
    echo "LLVM 源码已存在, 跳过下载"
fi

# ============================================================
# 阶段 1: 获取预编译 TableGen (Homebrew)
# ============================================================
BREW_LLVM_PREFIX="/opt/homebrew/opt/llvm@18"
TABLEGEN_BIN="${BREW_LLVM_PREFIX}/bin/llvm-tblgen"

if [ -x "${TABLEGEN_BIN}" ]; then
    echo "[阶段1] TableGen 已安装: ${TABLEGEN_BIN}"
else
    echo "[阶段1] 安装 LLVM@18 (Homebrew)..."
    brew install llvm@18 2>&1 | tail -5
    if [ ! -x "${TABLEGEN_BIN}" ]; then
        echo "错误: TableGen 安装失败"
        exit 1
    fi
    echo "[阶段1] TableGen 就绪: ${TABLEGEN_BIN}"
fi

# ============================================================
# 阶段 2: 交叉编译 LLVM 库 (iOS arm64)
# ============================================================
echo "[阶段2] 交叉编译 LLVM 库 (iOS arm64)..."

cmake -S "${LLVM_DIR}/llvm" \
      -B "${IOS_BUILD}" \
      -G "Ninja" \
      -DCMAKE_BUILD_TYPE=MinSizeRel \
      \
      `# iOS ARM64 交叉编译 (一等公民)` \
      -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_SYSROOT="${IOS_SDK}" \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
      -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
      \
      `# 使用阶段1的原生 TableGen` \
      -DLLVM_TABLEGEN="${TABLEGEN_BIN}" \
      \
      `# 仅构建 AArch64 目标` \
      -DLLVM_TARGETS_TO_BUILD=AArch64 \
      -DLLVM_DEFAULT_TARGET_TRIPLE=arm64-apple-ios15.0 \
      \
      `# 静态库模式` \
      -DLLVM_BUILD_STATIC=ON \
      -DLLVM_BUILD_LLVM_DYLIB=OFF \
      -DLLVM_LINK_LLVM_DYLIB=OFF \
      
      `# 最小化功能` \
      -DLLVM_ENABLE_TERMINFO=OFF \
      -DLLVM_ENABLE_ZLIB=OFF \
      -DLLVM_ENABLE_ZSTD=OFF \
      -DLLVM_ENABLE_LIBXML2=OFF \
      -DLLVM_ENABLE_LIBEDIT=OFF \
      -DLLVM_ENABLE_LIBPFM=OFF \
      -DLLVM_ENABLE_CURL=OFF \
      -DLLVM_ENABLE_HTTPLIB=OFF \
      -DLLVM_ENABLE_THREADS=OFF \
      -DLLVM_ENABLE_UNWIND_TABLES=OFF \
      -DLLVM_ENABLE_EH=OFF \
      -DLLVM_ENABLE_RTTI=OFF \
      -DLLVM_ENABLE_MODULES=OFF \
      -DLLVM_ENABLE_BINDINGS=OFF \
      \
      `# 不需要的项目/工具` \
      -DLLVM_ENABLE_PROJECTS="" \
      -DLLVM_INCLUDE_UTILS=OFF \
      -DLLVM_INCLUDE_TESTS=OFF \
      -DLLVM_INCLUDE_EXAMPLES=OFF \
      -DLLVM_INCLUDE_DOCS=OFF \
      -DLLVM_INCLUDE_BENCHMARKS=OFF \
      -DLLVM_INCLUDE_RUNTIMES=OFF \
      -DLLVM_BUILD_TOOLS=OFF \
      -DLLVM_BUILD_UTILS=OFF \
      -DLLVM_BUILD_EXAMPLES=OFF \
      -DLLVM_BUILD_TESTS=OFF \
      -DLLVM_BUILD_DOCS=OFF \
      -DLLVM_BUILD_BENCHMARKS=OFF

echo "[阶段2] CMake 配置完成, 开始编译..."

cmake --build "${IOS_BUILD}" \
      --target LLVMMCDisassembler \
      LLVMAArch64Disassembler \
      LLVMAArch64Desc \
      LLVMAArch64Info \
      LLVMAArch64Utils \
      -j"${CPU_COUNT}"

echo "[阶段2] 编译完成"

# ============================================================
# 安装头文件
# ============================================================
echo "安装 C API 头文件..."
mkdir -p "${OUTPUT_DIR}/include/llvm-c"

HEADER_SRC="${LLVM_DIR}/llvm/include/llvm-c"
if [ -d "${HEADER_SRC}" ]; then
    cp "${HEADER_SRC}/"*.h "${OUTPUT_DIR}/include/llvm-c/"
    echo "  已复制 $(ls "${OUTPUT_DIR}/include/llvm-c/"*.h 2>/dev/null | wc -l) 个头文件"
fi

# ============================================================
# 合并静态库
# ============================================================
echo "合并静态库..."

LLVM_LIBS=(
    "libLLVMAArch64Disassembler.a"
    "libLLVMAArch64Desc.a"
    "libLLVMAArch64Info.a"
    "libLLVMAArch64Utils.a"
    "libLLVMMCDisassembler.a"
    "libLLVMMC.a"
    "libLLVMSupport.a"
    "libLLVMDemangle.a"
    "libLLVMBinaryFormat.a"
    "libLLVMTargetParser.a"
    "libLLVMDebugInfoCodeView.a"
    "libLLVMDebugInfoDWARF.a"
)

OUTPUT_LIB="${OUTPUT_DIR}/libLLVMDisasm.a"
TMP_OBJ="${IOS_BUILD}/tmp_obj"
rm -rf "${TMP_OBJ}"
mkdir -p "${TMP_OBJ}"

FOUND=0
MISSING=0
for lib in "${LLVM_LIBS[@]}"; do
    libpath="${IOS_BUILD}/lib/${lib}"
    if [ -f "${libpath}" ]; then
        (cd "${TMP_OBJ}" && ar x "${libpath}" 2>/dev/null) || true
        ((FOUND++)) || true
    else
        echo "  (跳过缺失) ${lib}"
        ((MISSING++)) || true
    fi
done

OBJ_COUNT=$(find "${TMP_OBJ}" -name "*.o" 2>/dev/null | wc -l)
echo "  找到 ${FOUND} 个库, 缺失 ${MISSING} 个"
echo "  共 ${OBJ_COUNT} 个 object 文件"

if [ "${OBJ_COUNT}" -gt 0 ]; then
    mkdir -p "$(dirname "${OUTPUT_LIB}")"
    rm -f "${OUTPUT_LIB}"
    ar rcs "${OUTPUT_LIB}" "${TMP_OBJ}"/*.o
    echo "  已生成: ${OUTPUT_LIB}"
    ls -lh "${OUTPUT_LIB}"
else
    echo "错误: 没有 object 文件可合并"
    exit 1
fi

rm -rf "${TMP_OBJ}"

# ============================================================
# 收尾
# ============================================================
echo ""
echo "============================================"
echo "  构建成功!"
echo "============================================"
echo "  静态库: ${OUTPUT_LIB} ($(du -h "${OUTPUT_LIB}" | cut -f1))"
echo "  头文件: ${OUTPUT_DIR}/include/llvm-c/"
echo "============================================"
echo ""
echo "下载本仓库的 GitHub Actions artifact 后:"
echo "  unzip llvm-ios-arm64-disasm.zip -d /path/to/iosgg/External/llvm/"
