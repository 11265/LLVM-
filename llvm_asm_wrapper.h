// llvm_asm_wrapper.h — LLVM Assembly C Wrapper
// 提供给越狱插件使用的纯 C API，内部封装 LLVM C++ MC 汇编管线
//
// 【用法——越狱插件端】
//   uint32_t encoded = LLVMAssemble("mov x0, x1", 0x10000);
//   if (encoded == 0) { /* 汇编失败 */ }
//
// 【用法——构建脚本端】
//   此文件和 llvm_asm_wrapper.cpp 在 GitHub Actions 中交叉编译为 iOS arm64 .o，
//   合并进最终的 libLLVMAssembler.a
#ifndef LLVM_ASM_WRAPPER_H
#define LLVM_ASM_WRAPPER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 初始化汇编器（全局一次即可，重复调用安全）
/// 返回 0 成功，非 0 失败
int LLVMAssembleInit(void);

/// 汇编单条 ARM64 指令
/// @param instruction  ARM64 汇编字符串（如 "add x0, x1, #1"）
/// @param address      指令地址（影响 PC-relative 指令编码）
/// @return 编码后的 4 字节指令；0 表示失败
uint32_t LLVMAssemble(const char *instruction, uint64_t address);

/// 释放汇编器资源（进程退出时调用，也可不调）
void LLVMAssembleCleanup(void);

#ifdef __cplusplus
}
#endif

#endif // LLVM_ASM_WRAPPER_H
