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

#include <stddef.h>
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

// ============================================================
// 反汇编 API
// ============================================================

/// 创建反汇编器上下文
/// @param triple  目标三元组（如 "arm64-apple-ios15.0"），传 NULL 使用默认
/// @return 不透明句柄，失败返回 NULL
void *LLVMDisasmCreate(const char *triple);

/// 反汇编单条指令
/// @param dc       反汇编器句柄
/// @param bytes    指令字节数组
/// @param size     字节数（ARM64 固定 4）
/// @param address  指令地址（影响 PC-relative 显示）
/// @param out      输出字符串缓冲区
/// @param outSize  缓冲区大小
/// @return 写入的字符数（不含末尾 \0），0 表示失败
size_t LLVMDisasmInst(void *dc, const uint8_t *bytes, size_t size,
                      uint64_t address, char *out, size_t outSize);

/// 销毁反汇编器上下文
void LLVMDisasmDestroy(void *dc);

#ifdef __cplusplus
}
#endif

#endif // LLVM_ASM_WRAPPER_H
