//===-- R600ControlFlowFinalizer.cpp - Finalize Control Flow Inst----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This pass compute turns all control flow pseudo instructions into native one
/// computing their address on the fly ; it also sets STACK_SIZE info.
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "r600cf"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "AMDGPU.h"
#include "R600Defines.h"
#include "R600InstrInfo.h"
#include "R600MachineFunctionInfo.h"
#include "R600RegisterInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"

namespace llvm {

class R600ControlFlowFinalizer : public MachineFunctionPass {

private:
  static char ID;
  const R600InstrInfo *TII;
  unsigned MaxFetchInst;

  bool isFetch(const MachineInstr *MI) const {
    switch (MI->getOpcode()) {
    case AMDGPU::TEX_VTX_CONSTBUF:
    case AMDGPU::TEX_VTX_TEXBUF:
    case AMDGPU::TEX_LD:
    case AMDGPU::TEX_GET_TEXTURE_RESINFO:
    case AMDGPU::TEX_GET_GRADIENTS_H:
    case AMDGPU::TEX_GET_GRADIENTS_V:
    case AMDGPU::TEX_SET_GRADIENTS_H:
    case AMDGPU::TEX_SET_GRADIENTS_V:
    case AMDGPU::TEX_SAMPLE:
    case AMDGPU::TEX_SAMPLE_C:
    case AMDGPU::TEX_SAMPLE_L:
    case AMDGPU::TEX_SAMPLE_C_L:
    case AMDGPU::TEX_SAMPLE_LB:
    case AMDGPU::TEX_SAMPLE_C_LB:
    case AMDGPU::TEX_SAMPLE_G:
    case AMDGPU::TEX_SAMPLE_C_G:
    case AMDGPU::TXD:
    case AMDGPU::TXD_SHADOW:
     return true;
    default:
      return false;
    }
  }

  bool IsTrivialInst(MachineInstr *MI) const {
    switch (MI->getOpcode()) {
    case AMDGPU::KILL:
    case AMDGPU::RETURN:
      return true;
    default:
      return false;
    }
  }

  MachineBasicBlock::iterator
  MakeFetchClause(MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
      unsigned CfAddress) const {
    MachineBasicBlock::iterator ClauseHead = I;
    unsigned AluInstCount = 0;
    for (MachineBasicBlock::iterator E = MBB.end(); I != E; ++I) {
      if (IsTrivialInst(I))
        continue;
      if (!isFetch(I))
        break;
      AluInstCount ++;
      if (AluInstCount > MaxFetchInst)
        break;
    }
    BuildMI(MBB, ClauseHead, MBB.findDebugLoc(ClauseHead),
        TII->get(AMDGPU::CF_TC))
        .addImm(CfAddress) // ADDR
        .addImm(AluInstCount); // COUNT
    return I;
  }
  void CounterPropagateAddr(MachineInstr *MI, unsigned Addr) const {
    MI->getOperand(0).setImm(Addr + MI->getOperand(0).getImm());
  }
  void CounterPropagateAddr(std::set<MachineInstr *> MIs, unsigned Addr)
      const {
    for (std::set<MachineInstr *>::iterator It = MIs.begin(), E = MIs.end();
        It != E; ++It) {
      MachineInstr *MI = *It;
      CounterPropagateAddr(MI, Addr);
    }
  }

public:
  R600ControlFlowFinalizer(TargetMachine &tm) : MachineFunctionPass(ID),
    TII (static_cast<const R600InstrInfo *>(tm.getInstrInfo())) {
      const AMDGPUSubtarget &ST = tm.getSubtarget<AMDGPUSubtarget>();
      if (ST.device()->getGeneration() <= AMDGPUDeviceInfo::HD4XXX)
        MaxFetchInst = 8;
      else
        MaxFetchInst = 16;
  }

  virtual bool runOnMachineFunction(MachineFunction &MF) {
    unsigned MaxStack = 0;
    unsigned CurrentStack = 0;
    for (MachineFunction::iterator MB = MF.begin(), ME = MF.end(); MB != ME;
        ++MB) {
      MachineBasicBlock &MBB = *MB;
      unsigned CfCount = 0;
      std::vector<std::pair<unsigned, std::set<MachineInstr *> > > LoopStack;
      std::vector<MachineInstr * > IfThenElseStack;
      R600MachineFunctionInfo *MFI = MF.getInfo<R600MachineFunctionInfo>();
      if (MFI->ShaderType == 1) {
        BuildMI(MBB, MBB.begin(), MBB.findDebugLoc(MBB.begin()),
            TII->get(AMDGPU::CF_CALL_FS));
        CfCount++;
      }
      for (MachineBasicBlock::iterator I = MBB.begin(), E = MBB.end();
          I != E;) {
        if (isFetch(I)) {
          DEBUG(dbgs() << CfCount << ":"; I->dump(););
          I = MakeFetchClause(MBB, I, 0);
          CfCount++;
          continue;
        }

        MachineBasicBlock::iterator MI = I;
        I++;
        switch (MI->getOpcode()) {
        case AMDGPU::CF_ALU_PUSH_BEFORE:
          CurrentStack++;
          MaxStack = std::max(MaxStack, CurrentStack);
        case AMDGPU::CF_ALU:
          DEBUG(dbgs() << CfCount << ":"; MI->dump(););
          CfCount++;
          break;
        case AMDGPU::WHILELOOP: {
          CurrentStack++;
          MaxStack = std::max(MaxStack, CurrentStack);
          MachineInstr *MIb = BuildMI(MBB, MI, MBB.findDebugLoc(MI),
              TII->get(AMDGPU::WHILE_LOOP))
              .addImm(2);
          std::pair<unsigned, std::set<MachineInstr *> > Pair(CfCount,
              std::set<MachineInstr *>());
          Pair.second.insert(MIb);
          LoopStack.push_back(Pair);
          MI->eraseFromParent();
          CfCount++;
          break;
        }
        case AMDGPU::ENDLOOP: {
          CurrentStack--;
          std::pair<unsigned, std::set<MachineInstr *> > Pair =
              LoopStack.back();
          LoopStack.pop_back();
          CounterPropagateAddr(Pair.second, CfCount);
          BuildMI(MBB, MI, MBB.findDebugLoc(MI), TII->get(AMDGPU::END_LOOP))
              .addImm(Pair.first + 1);
          MI->eraseFromParent();
          CfCount++;
          break;
        }
        case AMDGPU::IF_PREDICATE_SET: {
          MachineInstr *MIb = BuildMI(MBB, MI, MBB.findDebugLoc(MI),
              TII->get(AMDGPU::CF_JUMP))
              .addImm(0)
              .addImm(0);
          IfThenElseStack.push_back(MIb);
          DEBUG(dbgs() << CfCount << ":"; MIb->dump(););
          MI->eraseFromParent();
          CfCount++;
          break;
        }
        case AMDGPU::ELSE: {
          MachineInstr * JumpInst = IfThenElseStack.back();
          IfThenElseStack.pop_back();
          CounterPropagateAddr(JumpInst, CfCount);
          MachineInstr *MIb = BuildMI(MBB, MI, MBB.findDebugLoc(MI),
              TII->get(AMDGPU::CF_ELSE))
              .addImm(0)
              .addImm(1);
          DEBUG(dbgs() << CfCount << ":"; MIb->dump(););
          IfThenElseStack.push_back(MIb);
          MI->eraseFromParent();
          CfCount++;
          break;
        }
        case AMDGPU::ENDIF: {
          CurrentStack--;
          MachineInstr *IfOrElseInst = IfThenElseStack.back();
          IfThenElseStack.pop_back();
          CounterPropagateAddr(IfOrElseInst, CfCount);
          MachineInstr *MIb = BuildMI(MBB, MI, MBB.findDebugLoc(MI),
              TII->get(AMDGPU::POP))
              .addImm(CfCount + 1)
              .addImm(1);
          DEBUG(dbgs() << CfCount << ":"; MIb->dump(););
          MI->eraseFromParent();
          CfCount++;
          break;
        }
        case AMDGPU::PREDICATED_BREAK: {
          CurrentStack--;
          CfCount += 3;
          BuildMI(MBB, MI, MBB.findDebugLoc(MI), TII->get(AMDGPU::CF_JUMP))
              .addImm(CfCount)
              .addImm(1);
          MachineInstr *MIb = BuildMI(MBB, MI, MBB.findDebugLoc(MI),
              TII->get(AMDGPU::LOOP_BREAK))
              .addImm(0);
          BuildMI(MBB, MI, MBB.findDebugLoc(MI), TII->get(AMDGPU::POP))
              .addImm(CfCount)
              .addImm(1);
          LoopStack.back().second.insert(MIb);
          MI->eraseFromParent();
          break;
        }
        case AMDGPU::CONTINUE: {
          MachineInstr *MIb = BuildMI(MBB, MI, MBB.findDebugLoc(MI),
              TII->get(AMDGPU::CF_CONTINUE))
              .addImm(0);
          LoopStack.back().second.insert(MIb);
          MI->eraseFromParent();
          CfCount++;
          break;
        }
        default:
          break;
        }
      }
      BuildMI(MBB, MBB.begin(), MBB.findDebugLoc(MBB.begin()),
          TII->get(AMDGPU::STACK_SIZE))
          .addImm(MaxStack);
    }

    return false;
  }

  const char *getPassName() const {
    return "R600 Control Flow Finalizer Pass";
  }
};

char R600ControlFlowFinalizer::ID = 0;

}


llvm::FunctionPass *llvm::createR600ControlFlowFinalizer(TargetMachine &TM) {
  return new R600ControlFlowFinalizer(TM);
}

