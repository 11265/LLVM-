// llvm_asm_wrapper.cpp — LLVM Assembly C Wrapper Implementation
// 在 GitHub Actions macOS 环境中交叉编译为 iOS arm64 架构
// 合并入 libLLVMAssembler.a 提供纯 C 汇编接口给越狱插件
// 适配 LLVM 18.x API

#include "llvm_asm_wrapper.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/MC/MCAsmBackend.h>
#include <llvm/MC/MCAsmInfo.h>
#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCDisassembler/MCDisassembler.h>
#include <llvm/MC/MCInst.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/MCInstrAnalysis.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/MCObjectFileInfo.h>
#include <llvm/MC/MCParser/MCAsmLexer.h>
#include <llvm/MC/MCParser/MCAsmParser.h>
#include <llvm/MC/MCParser/MCTargetAsmParser.h>
#include <llvm/MC/MCRegisterInfo.h>
#include <llvm/MC/MCStreamer.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>

using namespace llvm;

// ============================================================
// CaptureStreamer — 捕获单条指令编码的 MCStreamer 子类
// ============================================================
class CaptureStreamer : public MCStreamer {
    MCCodeEmitter &MCE;
public:
    SmallVector<uint8_t, 16> OutputBytes;

    CaptureStreamer(MCContext &Ctx, MCCodeEmitter &Emitter)
        : MCStreamer(Ctx), MCE(Emitter) {}

    // 核心: 解析器每解析出一条指令就调这里
    void emitInstruction(const MCInst &Inst, const MCSubtargetInfo &STI) override {
        SmallVector<char, 64> Code;
        SmallVector<MCFixup, 4> Fixups;
        MCE.encodeInstruction(Inst, Code, Fixups, STI);
        OutputBytes.append((const uint8_t *)Code.data(),
                           (const uint8_t *)Code.data() + Code.size());
    }

    // 满足 MCStreamer 纯虚函数的最低实现
    bool emitSymbolAttribute(MCSymbol *Symbol, MCSymbolAttr Attribute) override { return true; }
    void emitCommonSymbol(MCSymbol *Symbol, uint64_t Size, Align ByteAlignment) override {}
    void emitZerofill(MCSection *Section, MCSymbol *Symbol, uint64_t Size,
                      Align ByteAlignment, SMLoc Loc) override {}
};

// ============================================================
// 全局状态 — 跨调用共享的 MC 基础设施
// ============================================================
static std::once_flag gInitFlag;
static bool gInitialized = false;

namespace {
struct AssemblerState {
    Triple TT;
    std::unique_ptr<const Target> TheTarget;
    std::unique_ptr<MCRegisterInfo> MRI;
    std::unique_ptr<MCInstrInfo> MII;
    std::unique_ptr<MCSubtargetInfo> STI;
    std::unique_ptr<MCAsmInfo> MAI;
};
}

static std::unique_ptr<AssemblerState> gState;

// ============================================================
// 初始化
// ============================================================
int LLVMAssembleInit(void) {
    std::call_once(gInitFlag, []() {
        LLVMInitializeAArch64TargetInfo();
        LLVMInitializeAArch64TargetMC();
        LLVMInitializeAArch64AsmParser();

        Triple TT("arm64-apple-ios15.0");
        std::string Error;
        auto *rawTarget = TargetRegistry::lookupTarget(TT.str(), Error);
        if (!rawTarget) {
            return;
        }

        auto state = std::make_unique<AssemblerState>();
        state->TT = TT;
        state->TheTarget.reset(rawTarget);
        state->MRI.reset(state->TheTarget->createMCRegInfo(TT.str()));
        state->MII.reset(state->TheTarget->createMCInstrInfo());
        state->STI.reset(state->TheTarget->createMCSubtargetInfo(TT.str(), "", ""));

        MCTargetOptions MCOpts;
        state->MAI.reset(state->TheTarget->createMCAsmInfo(*state->MRI, TT.str(), MCOpts));

        if (state->MRI && state->MII && state->STI && state->MAI) {
            gState = std::move(state);
            gInitialized = true;
        }
    });

    return gInitialized ? 0 : -1;
}

// ============================================================
// 汇编
// ============================================================
uint32_t LLVMAssemble(const char *instruction, uint64_t address) {
    if (!gInitialized || !gState) return 0;
    if (!instruction || !instruction[0]) return 0;

    // --- LLVM 18 MCContext 构造: (Triple, MAI, MRI, STI, ...) ---
    MCContext Ctx(gState->TT, gState->MAI.get(), gState->MRI.get(), gState->STI.get());
    MCObjectFileInfo MOFI;
    Ctx.setObjectFileInfo(&MOFI);

    // 为本次调用创建 CodeEmitter
    auto MCE = std::unique_ptr<MCCodeEmitter>(
        gState->TheTarget->createMCCodeEmitter(*gState->MII, Ctx));
    if (!MCE) return 0;

    // --- 创建捕获流 (传入 MCE 引用) ---
    CaptureStreamer Streamer(Ctx, *MCE);

    // --- 创建 SourceMgr / Parser ---
    SourceMgr SrcMgr;
    std::string AsmStr(instruction);
    AsmStr += "\n"; // MCAsmParser 需要换行结束语句
    auto Buffer = MemoryBuffer::getMemBuffer(AsmStr);
    SrcMgr.AddNewSourceBuffer(std::move(Buffer), SMLoc());

    // LLVM 18 createMCAsmParser: (SM, Ctx, Str, MAI, CB=0) — 最多5个参数
    std::unique_ptr<MCAsmParser> Parser(
        createMCAsmParser(SrcMgr, Ctx, Streamer, *gState->MAI, 0));

    MCTargetOptions MCOpts;
    std::unique_ptr<MCTargetAsmParser> TAP(
        gState->TheTarget->createMCAsmParser(*gState->STI, *Parser, *gState->MII, MCOpts));

    if (!TAP) return 0;

    Parser->setTargetParser(*TAP);

    // --- 执行解析 ---
    int parseResult = Parser->Run(false);

    if (parseResult != 0 || Streamer.OutputBytes.empty()) {
        return 0;
    }

    // --- 提取编码结果 (ARM64 固定 4 字节小端) ---
    if (Streamer.OutputBytes.size() >= 4) {
        uint32_t result = 0;
        result |= (uint32_t)Streamer.OutputBytes[0];
        result |= (uint32_t)Streamer.OutputBytes[1] << 8;
        result |= (uint32_t)Streamer.OutputBytes[2] << 16;
        result |= (uint32_t)Streamer.OutputBytes[3] << 24;
        return result;
    }

    return 0;
}

// ============================================================
// 反汇编器上下文
// 直接用 LLVM C++ API 创建，绕过 LLVMCreateDisasm(C API) 的初始化问题
// ============================================================

struct DisasmContext {
    Triple TT;
    const Target *TheTarget;
    std::unique_ptr<MCRegisterInfo> MRI;
    std::unique_ptr<MCAsmInfo> MAI;
    std::unique_ptr<MCSubtargetInfo> STI;
    std::unique_ptr<MCInstrInfo> MII;
    std::unique_ptr<MCDisassembler> DisAsm;
    std::unique_ptr<MCInstPrinter> IP;
    std::unique_ptr<MCContext> Ctx;
};

static bool gDisasmTargetsRegistered = false;
static std::mutex gDisasmMutex;

static void ensureDisasmTargets(void) {
    if (gDisasmTargetsRegistered) return;
    std::lock_guard<std::mutex> lock(gDisasmMutex);
    if (gDisasmTargetsRegistered) return;
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64Disassembler();
    gDisasmTargetsRegistered = true;
}

void *LLVMDisasmCreate(const char *triple) {
    ensureDisasmTargets();

    Triple TT(triple && triple[0] ? triple : "arm64-apple-ios15.0");

    std::string Error;
    auto *TheTarget = TargetRegistry::lookupTarget(TT.str(), Error);
    if (!TheTarget) return nullptr;

    auto ctx = std::make_unique<DisasmContext>();
    ctx->TT = TT;
    ctx->TheTarget = TheTarget;

    ctx->MRI.reset(TheTarget->createMCRegInfo(TT.str()));
    ctx->MII.reset(TheTarget->createMCInstrInfo());
    ctx->STI.reset(TheTarget->createMCSubtargetInfo(TT.str(), "", ""));

    if (!ctx->MRI || !ctx->MII || !ctx->STI) return nullptr;

    MCTargetOptions MCOpts;
    ctx->MAI.reset(TheTarget->createMCAsmInfo(*ctx->MRI, TT.str(), MCOpts));
    if (!ctx->MAI) return nullptr;

    // MCContext 必须比 MCDisassembler 活得久
    ctx->Ctx = std::make_unique<MCContext>(TT, ctx->MAI.get(),
                                            ctx->MRI.get(), ctx->STI.get());

    ctx->DisAsm.reset(TheTarget->createMCDisassembler(*ctx->STI, *ctx->Ctx));
    if (!ctx->DisAsm) return nullptr;

    ctx->IP.reset(TheTarget->createMCInstPrinter(TT, 0, *ctx->MAI, *ctx->MII, *ctx->MRI));
    if (!ctx->IP) return nullptr;

    return ctx.release();
}

size_t LLVMDisasmInst(void *dc, const uint8_t *bytes, size_t size,
                      uint64_t address, char *out, size_t outSize) {
    if (!dc || !bytes || !out || !outSize) return 0;

    auto *ctx = static_cast<DisasmContext *>(dc);

    MCInst Inst;
    uint64_t InstSize = 0;
    MCDisassembler::DecodeStatus S =
        ctx->DisAsm->getInstruction(Inst, InstSize,
                                     ArrayRef<uint8_t>(bytes, size),
                                     address, nulls());

    if (S != MCDisassembler::Success) return 0;

    // 用 MCInstPrinter 格式化成字符串
    std::string formatted;
    raw_string_ostream OS(formatted);
    ctx->IP->printInst(&Inst, address, "", *ctx->STI, OS);

    // printInst 输出格式可能是 "\tmov\tx9, #22732" 或 "mov   x9, #22732"
    // 标准化为 "mov\tx9, #22732"（tab 分隔 mnemonic 和 operands）
    const char *s = formatted.c_str();
    while (*s == ' ' || *s == '\t') s++;                     // 跳过前导空白

    std::string result(s);
    size_t sep = result.find('\t');
    if (sep == std::string::npos) sep = result.find(' ');     // 找第一个分隔符

    if (sep != std::string::npos) {
        size_t opStart = sep + 1;
        while (opStart < result.size() && (result[opStart] == ' ' || result[opStart] == '\t'))
            opStart++;
        // 去掉尾部空白
        size_t opEnd = result.size();
        while (opEnd > opStart && (result[opEnd-1] == ' ' || result[opEnd-1] == '\t' || result[opEnd-1] == '\n'))
            opEnd--;
        result = result.substr(0, sep) + "\t" + result.substr(opStart, opEnd - opStart);
    }
    // else: 无操作数指令 (ret, nop 等)，直接用 mnemonic

    size_t copyLen = std::min(result.size(), outSize - 1);
    std::memcpy(out, result.data(), copyLen);
    out[copyLen] = '\0';

    return copyLen;
}

void LLVMDisasmDestroy(void *dc) {
    delete static_cast<DisasmContext *>(dc);
}

// ============================================================
// 清理
// ============================================================
void LLVMAssembleCleanup(void) {
    gState.reset();
    gInitialized = false;
}
