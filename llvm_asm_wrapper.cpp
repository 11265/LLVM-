// llvm_asm_wrapper.cpp — LLVM Assembly C Wrapper Implementation
// 在 GitHub Actions macOS 环境中交叉编译为 iOS arm64 架构
// 合并入 libLLVMAssembler.a 提供纯 C 汇编接口给越狱插件

#include "llvm_asm_wrapper.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/MC/MCAsmBackend.h>
#include <llvm/MC/MCAsmInfo.h>
#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCInst.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/MCObjectFileInfo.h>
#include <llvm/MC/MCObjectStreamer.h>
#include <llvm/MC/MCParser/MCAsmLexer.h>
#include <llvm/MC/MCParser/MCAsmParser.h>
#include <llvm/MC/MCParser/MCTargetAsmParser.h>
#include <llvm/MC/MCRegisterInfo.h>
#include <llvm/MC/MCStreamer.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace llvm;

// ============================================================
// CaptureStreamer — 捕获单条指令编码的 MCStreamer 子类
// ============================================================
class CaptureStreamer : public MCStreamer {
public:
    SmallVector<uint8_t, 16> OutputBytes;
    bool Success = true;

    CaptureStreamer(MCContext &Ctx) : MCStreamer(Ctx) {}

    // 核心: 解析器每解析出一条指令就调这里
    void emitInstruction(const MCInst &Inst, const MCSubtargetInfo &STI) override {
        // 延迟获取 MCCodeEmitter 因为构造时 Ctx 还不完整
        auto &MCE = getContext().getMCCodeEmitter();
        SmallVector<char, 64> Code;
        SmallVector<MCFixup, 4> Fixups;
        MCE.encodeInstruction(Inst, Code, Fixups, STI);
        OutputBytes.append((const uint8_t *)Code.data(),
                           (const uint8_t *)Code.data() + Code.size());
    }

    // 以下虚函数为满足 MCStreamer/MCObjectStreamer 纯虚函数的最低实现
    bool emitSymbolAttribute(MCSymbol *Symbol, MCSymbolAttr Attribute) override { return true; }
    void emitCommonSymbol(MCSymbol *Symbol, uint64_t Size, Align ByteAlignment) override {}
    void emitZerofill(MCSection *Section, MCSymbol *Symbol, uint64_t Size,
                      Align ByteAlignment, SMLoc Loc) override {}

private:
    void emitInstructionImpl(const MCInst &Inst, const MCSubtargetInfo &STI) override {
        emitInstruction(Inst, STI);
    }
};

// ============================================================
// 全局状态 — 跨调用共享的 MC 基础设施
// ============================================================
static std::once_flag gInitFlag;
static bool gInitialized = false;

namespace {
struct AssemblerState {
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
        LLVMInitializeAArch64AsmBackend();

        Triple TT("arm64-apple-ios15.0");
        std::string Error;
        auto *rawTarget = TargetRegistry::lookupTarget(TT.str(), Error);
        if (!rawTarget) {
            return;
        }

        gState = std::make_unique<AssemblerState>();
        gState->TheTarget.reset(rawTarget);
        gState->MRI.reset(gState->TheTarget->createMCRegInfo(TT.str()));
        gState->MII.reset(gState->TheTarget->createMCInstrInfo());
        gState->STI.reset(gState->TheTarget->createMCSubtargetInfo(TT.str(), "", ""));

        MCTargetOptions MCOpts;
        gState->MAI.reset(gState->TheTarget->createMCAsmInfo(*gState->MRI, TT.str(), MCOpts));

        if (gState->MRI && gState->MII && gState->STI && gState->MAI) {
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

    // --- 创建本次调用的上下文 ---
    MCContext Ctx(*gState->STI, *gState->MRI, nullptr);
    MCObjectFileInfo MOFI;
    Ctx.setObjectFileInfo(&MOFI);

    // 为本次调用创建 CodeEmitter 并注入 Context
    auto MCE = std::unique_ptr<MCCodeEmitter>(
        gState->TheTarget->createMCCodeEmitter(*gState->MII, Ctx));
    if (!MCE) return 0;
    Ctx.setMCCodeEmitter(std::move(MCE));

    // --- 创建捕获流 ---
    CaptureStreamer Streamer(Ctx);

    // --- 创建 SourceMgr / Parser ---
    SourceMgr SrcMgr;
    std::string AsmStr(instruction);
    AsmStr += "\n"; // MCAsmParser 需要换行结束语句
    auto Buffer = MemoryBuffer::getMemBuffer(AsmStr);
    SrcMgr.AddNewSourceBuffer(std::move(Buffer), SMLoc());

    MCTargetOptions MCOpts;
    std::unique_ptr<MCAsmParser> Parser(
        createMCAsmParser(SrcMgr, Ctx, Streamer, *gState->MAI, 0, 0));

    std::unique_ptr<MCTargetAsmParser> TAP(
        gState->TheTarget->createMCAsmParser(*gState->STI, *Parser, *gState->MII, MCOpts));

    if (!TAP) return 0;

    Parser->setTargetParser(*TAP);
    // 抑制操作数检查输出（只关心成功/失败）
    Parser->setShowParsedOperands(0);

    // --- 执行解析 ---
    int parseResult = Parser->Run(false);

    if (parseResult != 0 || Streamer.OutputBytes.empty()) {
        return 0;
    }

    // --- 提取编码结果 ---
    // ARM64 固定 4 字节
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
// 清理
// ============================================================
void LLVMAssembleCleanup(void) {
    gState.reset();
    gInitialized = false;
}
