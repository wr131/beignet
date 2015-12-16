/*
 * Copyright © 2012 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Benjamin Segovia <benjamin.segovia@intel.com>
 */

/**
 * \file gen_insn_selection.cpp
 * \author Benjamin Segovia <benjamin.segovia@intel.com>
 */

/* This is the instruction selection code. First of all, this is a bunch of c++
 * crap. Sorry if this is not that readable. Anyway, the goal here is to take
 * GenIR code (i.e. the very regular, very RISC IR) and to produce GenISA with
 * virtual registers (i.e. regular GenIR registers).
 *
 * Overall idea:
 * =============
 *
 * There is a lot of papers and research about that but I tried to keep it
 * simple. No dynamic programming, nothing like this. Just a recursive maximal
 * munch.
 *
 * Basically, the code is executed per basic block from bottom to top. Patterns
 * of GenIR instructions are defined and each instruction is matched against the
 * best pattern i.e. the pattern that catches the largest number of
 * instructions. Once matched, a sequence of instructions is output.
 *
 * Each instruction the match depends on is then marked as "root" i.e. we
 * indicate that each of these instructions must be generated: we indeed need their
 * destinations for the next instructions (remember that we generate the code in
 * reverse order)
 *
 * Patterns:
 * =========
 *
 * There is a lot of patterns and I did not implement all of them obviously. I
 * just quickly gather the complete code to make pattern implementation kind of
 * easy. This is pretty verbose to add a pattern but it should be not too hard
 * to add new ones.
 *
 * To create and register patterns, I just abused C++ pre-main. A bunch of
 * patterns is then created and sorted per opcode (i.e. the opcode of the root
 * of the pattern): this creates a library of patterns that may be used in
 * run-time.
 *
 * Predication / Masking and CFG linearization
 * ===========================================
 *
 * The current version is based on an unfortunate choice. Basically, the problem
 * to solve is how to map unstructured branches (i.e. regular gotos) onto Gen.
 * Gen has a native support for structured branches (if/else/endif/while...) but
 * nothing really native for unstructured branches.
 *
 * The idea we implemented is simple. We stole one flag register (here f0.0) to
 * mask all the instructions (and only activate the proper SIMD lanes) and we
 * use the CFG linearization technique to properly handle the control flow. This
 * is not really good for one particular reason: Gen instructions must use the
 * *same* flag register for the predicates (used for masking) and the
 * conditional modifier (used as a destination for CMP). This leads to extra
 * complications with compare instructions and select instructions. Basically,
 * we need to insert extra MOVs.
 *
 * Also, there is some extra kludge to handle the predicates for JMPI.
 *
 * TODO:
 * =====
 *
 * Sadly, I recreated here a new DAG class. This is just a bad idea since we
 * already have the DAG per basic block with the Function graph i.e. the
 * complete graph of uses and definitions. I think we should be able to save a
 * lot of code here if we can simply reuse the code from UD / DU chains.
 *
 * Finally, cross-block instruction selection is quite possible with this simple
 * approach. Basically, instructions from dominating blocks could be merged and
 * matched with other instructions in the dominated block. This leads to the
 * interesting approach which consists in traversing the dominator tree in post
 * order
 *
 * We already use if/endif to enclose each basic block. We will continue to identify
 * those blocks which could match to structured branching and use pure structured
 * instruction to handle them completely.
 */

#include "backend/gen_insn_selection.hpp"
#include "backend/gen_context.hpp"
#include "ir/function.hpp"
#include "ir/liveness.hpp"
#include "ir/profile.hpp"
#include "sys/cvar.hpp"
#include "sys/vector.hpp"
#include <algorithm>
#include <climits>

namespace gbe
{

  ///////////////////////////////////////////////////////////////////////////
  // Helper functions
  ///////////////////////////////////////////////////////////////////////////

  uint32_t getGenType(ir::Type type) {
    using namespace ir;
    switch (type) {
      case TYPE_BOOL: return GEN_TYPE_W;
      case TYPE_S8: return GEN_TYPE_B;
      case TYPE_U8: return GEN_TYPE_UB;
      case TYPE_S16: return GEN_TYPE_W;
      case TYPE_U16: return GEN_TYPE_UW;
      case TYPE_S32: return GEN_TYPE_D;
      case TYPE_U32: return GEN_TYPE_UD;
      case TYPE_S64: return GEN_TYPE_L;
      case TYPE_U64: return GEN_TYPE_UL;
      case TYPE_FLOAT: return GEN_TYPE_F;
      case TYPE_DOUBLE: return GEN_TYPE_DF;
      case TYPE_HALF: return GEN_TYPE_HF;
      default: NOT_SUPPORTED; return GEN_TYPE_F;
    }
  }

  ir::Type getIRType(uint32_t genType) {
    using namespace ir;
    switch (genType) {
      case GEN_TYPE_B: return TYPE_S8;
      case GEN_TYPE_UB: return TYPE_U8;
      case GEN_TYPE_W: return TYPE_S16;
      case GEN_TYPE_UW: return TYPE_U16;
      case GEN_TYPE_D: return TYPE_S32;
      case GEN_TYPE_UD: return TYPE_U32;
      case GEN_TYPE_L: return TYPE_S64;
      case GEN_TYPE_UL: return TYPE_U64;
      case GEN_TYPE_F: return TYPE_FLOAT;
      case GEN_TYPE_DF: return TYPE_DOUBLE;
      default: NOT_SUPPORTED; return TYPE_FLOAT;
    }
  }

  uint32_t getGenCompare(ir::Opcode opcode, bool inverse = false) {
    using namespace ir;
    switch (opcode) {
      case OP_LE: return (!inverse) ? GEN_CONDITIONAL_LE : GEN_CONDITIONAL_G;
      case OP_LT: return (!inverse) ? GEN_CONDITIONAL_L : GEN_CONDITIONAL_GE;
      case OP_GE: return (!inverse) ? GEN_CONDITIONAL_GE : GEN_CONDITIONAL_L;
      case OP_GT: return (!inverse) ? GEN_CONDITIONAL_G : GEN_CONDITIONAL_LE;
      case OP_EQ: return (!inverse) ? GEN_CONDITIONAL_EQ : GEN_CONDITIONAL_NEQ;
      case OP_NE: return (!inverse) ? GEN_CONDITIONAL_NEQ : GEN_CONDITIONAL_EQ;
      default: NOT_SUPPORTED; return 0u;
    };
  }

  ///////////////////////////////////////////////////////////////////////////
  // SelectionInstruction
  ///////////////////////////////////////////////////////////////////////////

  SelectionInstruction::SelectionInstruction(SelectionOpcode op, uint32_t dst, uint32_t src) :
    parent(NULL), opcode(op), dstNum(dst), srcNum(src)
  {
    extra.function = 0;
  }

  void SelectionInstruction::prepend(SelectionInstruction &other) {
    gbe::prepend(&other, this);
    other.parent = this->parent;
  }

  void SelectionInstruction::append(SelectionInstruction &other) {
    gbe::append(&other, this);
    other.parent = this->parent;
  }

  bool SelectionInstruction::isRead(void) const {
    return this->opcode == SEL_OP_UNTYPED_READ ||
           this->opcode == SEL_OP_READ64       ||
           this->opcode == SEL_OP_ATOMIC       ||
           this->opcode == SEL_OP_BYTE_GATHER  ||
           this->opcode == SEL_OP_SAMPLE ||
           this->opcode == SEL_OP_VME ||
           this->opcode == SEL_OP_DWORD_GATHER;
  }

  bool SelectionInstruction::modAcc(void) const {
    return this->opcode == SEL_OP_I64SUB ||
           this->opcode == SEL_OP_I64ADD ||
           this->opcode == SEL_OP_MUL_HI ||
           this->opcode == SEL_OP_HADD ||
           this->opcode == SEL_OP_RHADD ||
           this->opcode == SEL_OP_I64MUL ||
           this->opcode == SEL_OP_I64_MUL_HI ||
           this->opcode == SEL_OP_I64MADSAT ||
           this->opcode == SEL_OP_I64DIV ||
           this->opcode == SEL_OP_I64REM ||
           this->opcode == SEL_OP_MACH;
  }

  bool SelectionInstruction::isWrite(void) const {
    return this->opcode == SEL_OP_UNTYPED_WRITE ||
           this->opcode == SEL_OP_WRITE64       ||
           this->opcode == SEL_OP_ATOMIC        ||
           this->opcode == SEL_OP_BYTE_SCATTER  ||
           this->opcode == SEL_OP_TYPED_WRITE;
  }

  bool SelectionInstruction::isBranch(void) const {
    return this->opcode == SEL_OP_JMPI;
  }

  bool SelectionInstruction::isLabel(void) const {
    return this->opcode == SEL_OP_LABEL;
  }

  ///////////////////////////////////////////////////////////////////////////
  // SelectionVector
  ///////////////////////////////////////////////////////////////////////////

  SelectionVector::SelectionVector(void) :
    insn(NULL), reg(NULL), regNum(0), isSrc(0)
  {}

  ///////////////////////////////////////////////////////////////////////////
  // SelectionBlock
  ///////////////////////////////////////////////////////////////////////////

  SelectionBlock::SelectionBlock(const ir::BasicBlock *bb) : bb(bb), isLargeBlock(false), endifLabel( (ir::LabelIndex) 0), removeSimpleIfEndif(false){}

  void SelectionBlock::append(ir::Register reg) { tmp.push_back(reg); }

  void SelectionBlock::append(SelectionInstruction *insn) {
    this->insnList.push_back(insn);
    insn->parent = this;
  }

  void SelectionBlock::prepend(SelectionInstruction *insn) {
    this->insnList.push_front(insn);
    insn->parent = this;
  }

  void SelectionBlock::append(SelectionVector *vec) {
    this->vectorList.push_back(vec);
  }

#define LD_MSG_ORDER_IVB 7
#define LD_MSG_ORDER_SKL 9

  ///////////////////////////////////////////////////////////////////////////
  // Maximal munch selection on DAG
  ///////////////////////////////////////////////////////////////////////////

  /*! All instructions in a block are organized into a DAG */
  class SelectionDAG
  {
  public:
    INLINE SelectionDAG(const ir::Instruction &insn) :
      insn(insn), mergeable(0), childNum(insn.getSrcNum()), isRoot(0) {
      GBE_ASSERT(insn.getSrcNum() <= ir::Instruction::MAX_SRC_NUM);
      for (uint32_t childID = 0; childID < childNum; ++childID)
        this->child[childID] = NULL;
      computeBool = false;
      isUsed = false;
    }
    /*! Mergeable are non-root instructions with valid sources */
    INLINE void setAsMergeable(uint32_t which) { mergeable|=(1<<which); }
    /*! Mergeable are non-root instructions with valid sources */
    INLINE bool isMergeable(uint32_t which) const { return mergeable&(1<<which); }
    /*! Children that need to be matched */
    SelectionDAG *child[ir::Instruction::MAX_SRC_NUM];
    /*! Instruction that needs to be matched */
    const ir::Instruction &insn;
    /*! When sources have been overwritten, a child insn cannot be merged */
    uint64_t mergeable:ir::Instruction::MAX_SRC_NUM;
    /*! Number of children we have in the pattern */
    uint32_t childNum:7;
    /*! A root must be generated, no matter what */
    uint32_t isRoot:1;
    /*! A bool register is used as normal computing sources. */
    bool computeBool;
    /*! is used in this block */
    bool isUsed;
  };

  /*! A pattern is a tree to match. This is the general interface for them. For
   *  pattern to be matched, we need to match the complete tree i.e. this node
   *  and its child nodes
   */
  class SelectionPattern
  {
  public:
    SelectionPattern(uint32_t insnNum, uint32_t cost) :
      insnNum(insnNum), cost(cost) {}
    /*! This is an abstract class */
    virtual ~SelectionPattern(void) {}
    /*! Emit Gen code in the selection. Return false if no match */
    virtual bool emit(Selection::Opaque &sel, SelectionDAG &dag) const = 0;
    /*! All the possible opcodes for this pattern (for fast sort) */
    vector<ir::Opcode> opcodes;
    /*! Number of instruction generated */
    uint32_t insnNum;
    /*! Cost of the pattern */
    uint32_t cost;
  };

  /*! Store and sort all the patterns. This is our global library we use for the
   *  code selection
   */
  class SelectionLibrary
  {
  public:
    /*! Will register all the patterns */
    SelectionLibrary(void);
    /*! Release and destroy all the registered patterns */
    ~SelectionLibrary(void);
    /*! Insert the given pattern for all associated opcodes */
    template <typename PatternType> void insert(void);
    /*! One list of pattern per opcode */
    typedef vector<const SelectionPattern*> PatternList;
    /*! All lists of patterns properly sorted per opcode */
    PatternList patterns[ir::OP_INVALID];
    /*! All patterns to free */
    vector<const SelectionPattern*> toFree;
  };

  ///////////////////////////////////////////////////////////////////////////
  // Code selection internal implementation
  ///////////////////////////////////////////////////////////////////////////

  /*! Actual implementation of the instruction selection engine */
  class Selection::Opaque
  {
  public:
    /*! simdWidth is the default width for the instructions */
    Opaque(GenContext &ctx);
    /*! Release everything */
    virtual ~Opaque(void);
    /*! Implements the instruction selection itself */
    void select(void);
    /*! Start a backward generation (from the end of the block) */
    void startBackwardGeneration(void);
    /*! End backward code generation and output the code in the block */
    void endBackwardGeneration(void);
    /*! Implement public class */
    uint32_t getLargestBlockSize(void) const;
    /*! Implement public class */
    INLINE uint32_t getVectorNum(void) const { return this->vectorNum; }
    /*! Implement public class */
    INLINE ir::Register replaceSrc(SelectionInstruction *insn, uint32_t regID, ir::Type type, bool needMov);
    /*! Implement public class */
    INLINE ir::Register replaceDst(SelectionInstruction *insn, uint32_t regID, ir::Type type, bool needMov);
    /*! spill a register (insert spill/unspill instructions) */
    INLINE bool spillRegs(const SpilledRegs &spilledRegs, uint32_t registerPool);
    bool has32X32Mul() const { return bHas32X32Mul; }
    void setHas32X32Mul(bool b) { bHas32X32Mul = b; }
    bool hasLongType() const { return bHasLongType; }
    bool hasDoubleType() const { return bHasDoubleType; }
    bool hasHalfType() const { return bHasHalfType; }
    void setHasLongType(bool b) { bHasLongType = b; }
    void setHasDoubleType(bool b) { bHasDoubleType = b; }
    void setHasHalfType(bool b) { bHasHalfType = b; }
    bool hasLongRegRestrict() { return bLongRegRestrict; }
    void setLongRegRestrict(bool b) { bLongRegRestrict = b; }
    void setLdMsgOrder(uint32_t type)  { ldMsgOrder = type; }
    uint32_t getLdMsgOrder()  const { return ldMsgOrder; }
    void setSlowByteGather(bool b) { slowByteGather = b; }
    bool getSlowByteGather() { return slowByteGather; }
    /*! indicate whether a register is a scalar/uniform register. */
    INLINE bool isPartialWrite(const ir::Register &reg) const {
      return partialWriteRegs.find(reg.value()) != partialWriteRegs.end();
    }
    INLINE bool isScalarReg(const ir::Register &reg) const {
      const ir::RegisterData &regData = getRegisterData(reg);
      return regData.isUniform();
    }
    INLINE bool isLongReg(const ir::Register &reg) const {
      const ir::RegisterData &regData = getRegisterData(reg);
      return regData.family == ir::FAMILY_QWORD;
    }

    INLINE GenRegister unpacked_ud(const ir::Register &reg) const {
      return GenRegister::unpacked_ud(reg, isScalarReg(reg));
    }

    INLINE GenRegister unpacked_uw(const ir::Register &reg) const {
      return GenRegister::unpacked_uw(reg, isScalarReg(reg), isLongReg(reg));
    }

    INLINE GenRegister unpacked_ub(const ir::Register &reg) const {
      return GenRegister::unpacked_ub(reg, isScalarReg(reg));
    }

    INLINE GenRegister getOffsetReg(GenRegister reg, int nr, int subnr, bool isDst = true) {
      if (isDst)
        partialWriteRegs.insert(reg.value.reg);
      return GenRegister::offset(reg, nr, subnr);
    }

    GenRegister getLaneIDReg();
    /*! Implement public class */
    INLINE uint32_t getRegNum(void) const { return file.regNum(); }
    /*! Implements public interface */
    INLINE ir::RegisterData getRegisterData(ir::Register reg) const {
      return file.get(reg);
    }
    /*! Implement public class */
    INLINE ir::RegisterFamily getRegisterFamily(ir::Register reg) const {
      return file.get(reg).family;
    }
    /*! Implement public class */
    SelectionInstruction *create(SelectionOpcode, uint32_t dstNum, uint32_t srcNum);
    /*! Return the selection register from the GenIR one */
    GenRegister selReg(ir::Register, ir::Type type = ir::TYPE_FLOAT) const;
    /*! Compute the nth register part when using SIMD8 with Qn (n in 2,3,4) */
    GenRegister selRegQn(ir::Register, uint32_t quarter, ir::Type type = ir::TYPE_FLOAT) const;
    /*! Size of the stack (should be large enough) */
    enum { MAX_STATE_NUM = 16 };
    /*! Push the current instruction state */
    INLINE void push(void) {
      assert(stateNum < MAX_STATE_NUM);
      stack[stateNum++] = curr;
    }
    /*! Pop the latest pushed state */
    INLINE void pop(void) {
      assert(stateNum > 0);
      curr = stack[--stateNum];
    }
    /*! Create a new register in the register file and append it in the
     *  temporary list of the current block
     */
    INLINE ir::Register reg(ir::RegisterFamily family, bool scalar = false) {
      GBE_ASSERT(block != NULL);
      const ir::Register reg = file.append(family, scalar);
      block->append(reg);
      return reg;
    }
    /*! Append a block at the block stream tail. It becomes the current block */
    void appendBlock(const ir::BasicBlock &bb);
    /*! Append an instruction in the current block */
    SelectionInstruction *appendInsn(SelectionOpcode, uint32_t dstNum, uint32_t srcNum);
    /*! Append a new vector of registers in the current block */
    SelectionVector *appendVector(void);
    /*! Build a DAG for the basic block (return number of instructions) */
    uint32_t buildBasicBlockDAG(const ir::BasicBlock &bb);
    /*! Perform the selection on the basic block */
    void matchBasicBlock(const ir::BasicBlock &bb, uint32_t insnNum);
    /*! a simple block can use predication instead of if/endif*/
    bool isSimpleBlock(const ir::BasicBlock &bb, uint32_t insnNum);
    /*! an instruction has a QWORD family src or dst operand. */
    bool hasQWord(const ir::Instruction &insn);
    /*! A root instruction needs to be generated */
    bool isRoot(const ir::Instruction &insn) const;
    /*! Set debug infomation to Selection */
    void setDBGInfo_SEL(DebugInfo in) { DBGInfo = in; }

    /*! To handle selection block allocation */
    DECL_POOL(SelectionBlock, blockPool);
    /*! To handle selection instruction allocation */
    LinearAllocator insnAllocator;
    /*! To handle selection vector allocation */
    DECL_POOL(SelectionVector, vecPool);
    /*! Per register information used with top-down block sweeping */
    vector<SelectionDAG*> regDAG;
    /*! Store one DAG per instruction */
    vector<SelectionDAG*> insnDAG;
    /*! Owns this structure */
    GenContext &ctx;
    /*! Tail of the code fragment for backward code generation */
    intrusive_list<SelectionInstruction> bwdList;
    /*! List of emitted blocks */
    intrusive_list<SelectionBlock> blockList;
    /*! Currently processed block */
    SelectionBlock *block;
    /*! Current instruction state to use */
    GenInstructionState curr;
    /*! We append new registers so we duplicate the function register file */
    ir::RegisterFile file;
    /*! State used to encode the instructions */
    GenInstructionState stack[MAX_STATE_NUM];
    /*! Maximum number of instructions in the basic blocks */
    uint32_t maxInsnNum;
    /*! Speed up instruction dag allocation */
    DECL_POOL(SelectionDAG, dagPool);
    /*! Total number of registers in the function we encode */
    uint32_t regNum;
    /*! Number of states currently pushed */
    uint32_t stateNum;
    /*! Number of vector allocated */
    uint32_t vectorNum;
    /*! If true, generate code backward */
    bool bwdCodeGeneration;
    DebugInfo DBGInfo;
    /*! To make function prototypes more readable */
    typedef const GenRegister &Reg;
    /*! If true, the thread map has already been stored */
    bool storeThreadMap;

    /*! Check for destination register. Major purpose is to find
        out partially updated dst registers. These registers will
        be unspillable. */
    set<uint32_t> partialWriteRegs;

#define ALU1(OP) \
  INLINE void OP(Reg dst, Reg src) { ALU1(SEL_OP_##OP, dst, src); }
#define ALU1WithTemp(OP) \
  INLINE void OP(Reg dst, Reg src, Reg temp) { ALU1WithTemp(SEL_OP_##OP, dst, src, temp); }
#define ALU2(OP) \
  INLINE void OP(Reg dst, Reg src0, Reg src1) { ALU2(SEL_OP_##OP, dst, src0, src1); }
#define ALU2WithTemp(OP) \
  INLINE void OP(Reg dst, Reg src0, Reg src1, Reg temp) { ALU2WithTemp(SEL_OP_##OP, dst, src0, src1, temp); }
#define ALU3(OP) \
  INLINE void OP(Reg dst, Reg src0, Reg src1, Reg src2) { ALU3(SEL_OP_##OP, dst, src0, src1, src2); }
#define I64Shift(OP) \
  INLINE void OP(Reg dst, Reg src0, Reg src1, GenRegister tmp[6]) { I64Shift(SEL_OP_##OP, dst, src0, src1, tmp); }
    ALU1(MOV)
    ALU1(READ_ARF)
    ALU1(LOAD_INT64_IMM)
    ALU1(RNDZ)
    ALU1(RNDE)
    ALU1(F16TO32)
    ALU1(F32TO16)
    ALU1WithTemp(BSWAP)
    ALU2(SEL)
    ALU2(SEL_INT64)
    ALU1(NOT)
    ALU2(AND)
    ALU2(OR)
    ALU2(XOR)
    ALU2(I64AND)
    ALU2(I64OR)
    ALU2(I64XOR)
    ALU2(SHR)
    ALU2(SHL)
    ALU2(RSR)
    ALU2(RSL)
    ALU2(ASR)
    ALU2(ADD)
    ALU2WithTemp(I64ADD)
    ALU2WithTemp(I64SUB)
    ALU2(MUL)
    ALU1(FRC)
    ALU1(RNDD)
    ALU1(RNDU)
    ALU2(MACH)
    ALU1(LZD)
    ALU3(MAD)
    ALU3(LRP)
    ALU2WithTemp(MUL_HI)
    ALU1(FBH)
    ALU1(FBL)
    ALU1(CBIT)
    ALU2WithTemp(HADD)
    ALU2WithTemp(RHADD)
    ALU2(UPSAMPLE_LONG)
    ALU1WithTemp(CONVI_TO_I64)
    ALU1WithTemp(CONVF_TO_I64)
    ALU1(CONVI64_TO_I)
    I64Shift(I64SHL)
    I64Shift(I64SHR)
    I64Shift(I64ASR)
#undef ALU1
#undef ALU1WithTemp
#undef ALU2
#undef ALU2WithTemp
#undef ALU3
#undef I64Shift
    /*! simd shuffle */
    void SIMD_SHUFFLE(Reg dst, Reg src0, Reg src1);
    /*! Convert 64-bit integer to 32-bit float */
    void CONVI64_TO_F(Reg dst, Reg src, GenRegister tmp[6]);
    /*! Convert 64-bit integer to 32-bit float */
    void CONVF_TO_I64(Reg dst, Reg src, GenRegister tmp[2]);
    /*! Saturated 64bit x*y + z */
    void I64MADSAT(Reg dst, Reg src0, Reg src1, Reg src2, GenRegister* tmp, int tmp_num);
    /*! High 64bit of x*y */
    void I64_MUL_HI(Reg dst, Reg src0, Reg src1, GenRegister *tmp, int tmp_num);
    /*! (x+y)>>1 without mod. overflow */
    void I64HADD(Reg dst, Reg src0, Reg src1, GenRegister *tmp, int tmp_num);
    /*! (x+y+1)>>1 without mod. overflow */
    void I64RHADD(Reg dst, Reg src0, Reg src1, GenRegister *tmp, int tmp_num);
    /*! Shift a 64-bit integer */
    void I64Shift(SelectionOpcode opcode, Reg dst, Reg src0, Reg src1, GenRegister tmp[7]);
    /*! Compare 64-bit integer */
    void I64CMP(uint32_t conditional, Reg src0, Reg src1, GenRegister tmp[3]);
    /*! Saturated addition of 64-bit integer */
    void I64SATADD(Reg dst, Reg src0, Reg src1, GenRegister tmp[5]);
    /*! Saturated subtraction of 64-bit integer */
    void I64SATSUB(Reg dst, Reg src0, Reg src1, GenRegister tmp[5]);
    /*! Encode a barrier instruction */
    void BARRIER(GenRegister src, GenRegister fence, uint32_t barrierType);
    /*! Encode a barrier instruction */
    void FENCE(GenRegister dst);
    /*! Encode a label instruction */
    void LABEL(ir::LabelIndex label);
    /*! Jump indexed instruction, return the encoded instruction count according to jump distance. */
    int JMPI(Reg src, ir::LabelIndex target, ir::LabelIndex origin);
    /*! IF indexed instruction */
    void IF(Reg src, ir::LabelIndex jip, ir::LabelIndex uip);
    /*! ELSE indexed instruction */
    void ELSE(Reg src, ir::LabelIndex jip, ir::LabelIndex elseLabel);
    /*! ENDIF indexed instruction */
    void ENDIF(Reg src, ir::LabelIndex jip, ir::LabelIndex endifLabel = ir::LabelIndex(0));
    /*! WHILE indexed instruction */
    void WHILE(Reg src, ir::LabelIndex jip);
    /*! BRD indexed instruction */
    void BRD(Reg src, ir::LabelIndex jip);
    /*! BRC indexed instruction */
    void BRC(Reg src, ir::LabelIndex jip, ir::LabelIndex uip);
    /*! Compare instructions */
    void CMP(uint32_t conditional, Reg src0, Reg src1, Reg dst = GenRegister::null());
    /*! Select instruction with embedded comparison */
    void SEL_CMP(uint32_t conditional, Reg dst, Reg src0, Reg src1);
    /* Constant buffer move instruction */
    void INDIRECT_MOVE(Reg dst, Reg tmp, Reg base, Reg regOffset, uint32_t immOffset);
    /*! EOT is used to finish GPGPU threads */
    void EOT(void);
    /*! No-op */
    void NOP(void);
    /*! Wait instruction (used for the barrier) */
    void WAIT(uint32_t n = 0);
    /*! Atomic instruction */
    void ATOMIC(Reg dst, uint32_t function, uint32_t srcNum, Reg src0, Reg src1, Reg src2, GenRegister bti, vector<GenRegister> temps);
    /*! Read 64 bits float/int array */
    void READ64(Reg addr, const GenRegister *dst, const GenRegister *tmp, uint32_t elemNum, const GenRegister bti, bool native_long, vector<GenRegister> temps);
    /*! Write 64 bits float/int array */
    void WRITE64(Reg addr, const GenRegister *src, const GenRegister *tmp, uint32_t srcNum, GenRegister bti, bool native_long, vector<GenRegister> temps);
    /*! Untyped read (up to 4 elements) */
    void UNTYPED_READ(Reg addr, const GenRegister *dst, uint32_t elemNum, GenRegister bti, vector<GenRegister> temps);
    /*! Untyped write (up to 4 elements) */
    void UNTYPED_WRITE(Reg addr, const GenRegister *src, uint32_t elemNum, GenRegister bti, vector<GenRegister> temps);
    /*! Byte gather (for unaligned bytes, shorts and ints) */
    void BYTE_GATHER(Reg dst, Reg addr, uint32_t elemSize, GenRegister bti, vector<GenRegister> temps);
    /*! Byte scatter (for unaligned bytes, shorts and ints) */
    void BYTE_SCATTER(Reg addr, Reg src, uint32_t elemSize, GenRegister bti, vector <GenRegister> temps);
    /*! DWord scatter (for constant cache read) */
    void DWORD_GATHER(Reg dst, Reg addr, uint32_t bti);
    /*! Unpack the uint to charN */
    void UNPACK_BYTE(const GenRegister *dst, const GenRegister src, uint32_t elemSize, uint32_t elemNum);
    /*! pack the charN to uint */
    void PACK_BYTE(const GenRegister dst, const GenRegister *src, uint32_t elemSize, uint32_t elemNum);
    /*! Unpack the uint to charN */
    void UNPACK_LONG(const GenRegister dst, const GenRegister src);
    /*! pack the charN to uint */
    void PACK_LONG(const GenRegister dst, const GenRegister src);
    /*! Extended math function (2 arguments) */
    void MATH(Reg dst, uint32_t function, Reg src0, Reg src1);
    /*! Extended math function (1 argument) */
    void MATH(Reg dst, uint32_t function, Reg src);
    /*! Encode unary instructions */
    void ALU1(SelectionOpcode opcode, Reg dst, Reg src);
    /*! Encode unary with temp reg instructions */
    void ALU1WithTemp(SelectionOpcode opcode, Reg dst, Reg src0, Reg temp);
    /*! Encode binary instructions */
    void ALU2(SelectionOpcode opcode, Reg dst, Reg src0, Reg src1);
    /*! Encode binary with temp reg instructions */
    void ALU2WithTemp(SelectionOpcode opcode, Reg dst, Reg src0, Reg src1, Reg temp);
    /*! Encode ternary instructions */
    void ALU3(SelectionOpcode opcode, Reg dst, Reg src0, Reg src1, Reg src2);
    /*! Encode sample instructions */
    void SAMPLE(GenRegister *dst, uint32_t dstNum, GenRegister *msgPayloads, uint32_t msgNum, uint32_t bti, uint32_t sampler, bool isLD, bool isUniform);
    /*! Encode vme instructions */
    void VME(uint32_t bti, GenRegister *dst, GenRegister *payloadVal, uint32_t dstNum, uint32_t srcNum, uint32_t msg_type, uint32_t vme_search_path_lut, uint32_t lut_sub);
    /*! Encode typed write instructions */
    void TYPED_WRITE(GenRegister *msgs, uint32_t msgNum, uint32_t bti, bool is3D);
    /*! Get image information */
    void GET_IMAGE_INFO(uint32_t type, GenRegister *dst, uint32_t dst_num, uint32_t bti);
    /*! Calculate the timestamp */
    void CALC_TIMESTAMP(GenRegister ts[4], int tsN, GenRegister tmp, uint32_t pointNum, uint32_t tsType);
    /*! Store the profiling info */
    void STORE_PROFILING(uint32_t profilingType, uint32_t bti, GenRegister tmp0, GenRegister tmp1, GenRegister ts[4], int tsNum);
    /*! Multiply 64-bit integers */
    void I64MUL(Reg dst, Reg src0, Reg src1, GenRegister *tmp, bool native_long);
    /*! 64-bit integer division */
    void I64DIV(Reg dst, Reg src0, Reg src1, GenRegister *tmp, int tmp_int);
    /*! 64-bit integer remainder of division */
    void I64REM(Reg dst, Reg src0, Reg src1, GenRegister *tmp, int tmp_int);
    /*! double division */
    void F64DIV(Reg dst, Reg src0, Reg src1, GenRegister* tmp, int tmpNum);
    /*! Work Group Operations */
    void WORKGROUP_OP(uint32_t wg_op, Reg dst, GenRegister src, GenRegister nextThreadID,
                     GenRegister threadID, GenRegister threadn, GenRegister tmp);
    /* common functions for both binary instruction and sel_cmp and compare instruction.
       It will handle the IMM or normal register assignment, and will try to avoid LOADI
       as much as possible. */
    void getSrcGenRegImm(SelectionDAG &dag, GenRegister &src0,
                      GenRegister &src1, ir::Type type, bool &inverse);
    void getSrcGenRegImm(SelectionDAG &dag,
                      SelectionDAG *dag0, SelectionDAG *dag1,
                      GenRegister &src0, GenRegister &src1,
                      ir::Type type, bool &inverse);

    /* Get current block IP register according to label width. */
    GenRegister getBlockIP() {
      return ctx.isDWLabel() ? selReg(ir::ocl::dwblockip) : selReg(ir::ocl::blockip);
    }

    /* Get proper label immediate gen register from label value. */
    GenRegister getLabelImmReg(uint32_t labelValue) {
      return ctx.isDWLabel() ? GenRegister::immud(labelValue) : GenRegister::immuw(labelValue);
    }

    /* Get proper label immediate gen register from label. */
    GenRegister getLabelImmReg(ir::LabelIndex label) {
      return getLabelImmReg(label.value());
    }

    /* Set current label register to a label value. */
    void setBlockIP(GenRegister blockip, uint32_t labelValue) {
      if (!ctx.isDWLabel())
        MOV(GenRegister::retype(blockip, GEN_TYPE_UW), GenRegister::immuw(labelValue));
      else
        MOV(GenRegister::retype(blockip, GEN_TYPE_UD), GenRegister::immud(labelValue));
    }

    /* Generate comparison instruction to compare block ip address and specified label register.*/
    void cmpBlockIP(uint32_t cond,
                    GenRegister blockip,
                    GenRegister labelReg) {
      if (!ctx.isDWLabel())
        CMP(cond,
            GenRegister::retype(blockip, GEN_TYPE_UW),
            labelReg,
            GenRegister::retype(GenRegister::null(),
            GEN_TYPE_UW));
      else
        CMP(cond,
            GenRegister::retype(blockip, GEN_TYPE_UD),
            labelReg,
            GenRegister::retype(GenRegister::null(),
            GEN_TYPE_UD));
    }

    void cmpBlockIP(uint32_t cond,
                    GenRegister blockip,
                    uint32_t labelValue) {
      if (!ctx.isDWLabel())
        CMP(cond,
            GenRegister::retype(blockip, GEN_TYPE_UW),
            GenRegister::immuw(labelValue),
            GenRegister::retype(GenRegister::null(),
            GEN_TYPE_UW));
      else
        CMP(cond,
            GenRegister::retype(blockip, GEN_TYPE_UD),
            GenRegister::immud(labelValue),
            GenRegister::retype(GenRegister::null(), GEN_TYPE_UD));
    }

    INLINE vector<GenRegister> getBTITemps(const ir::BTI &bti) {
      vector<GenRegister> temps;
      if (!bti.isConst) {
        temps.push_back(selReg(reg(ir::FAMILY_WORD, true), ir::TYPE_U16));
        temps.push_back(selReg(reg(ir::FAMILY_DWORD, true), ir::TYPE_U32));
      }
      return temps;
    }

    /*! Use custom allocators */
    GBE_CLASS(Opaque);
    friend class SelectionBlock;
    friend class SelectionInstruction;
  private:
    /*! Auxiliary label for if/endif. */ 
    uint32_t currAuxLabel;
    bool bHas32X32Mul;
    bool bHasLongType;
    bool bHasDoubleType;
    bool bHasHalfType;
    bool bLongRegRestrict;
    uint32_t ldMsgOrder;
    bool slowByteGather;
    INLINE ir::LabelIndex newAuxLabel()
    {
      currAuxLabel++;
      return (ir::LabelIndex)currAuxLabel;
    }

  };

  ///////////////////////////////////////////////////////////////////////////
  // Helper function
  ///////////////////////////////////////////////////////////////////////////

  /*! Directly mark all sources as root (when no match is found) */
  static void markAllChildren(SelectionDAG &dag) {
    // Do not merge anything, so all sources become roots
    for (uint32_t childID = 0; childID < dag.childNum; ++childID)
      if (dag.child[childID])
        dag.child[childID]->isRoot = 1;
  }

  /*! Helper function to figure if two sources are the same */
  static bool sourceMatch(SelectionDAG *src0DAG, uint32_t src0ID,
                          SelectionDAG *src1DAG, uint32_t src1ID)
  {
    GBE_ASSERT(src0DAG && src1DAG);
    // Ensure they are the same physical registers
    const ir::Register src0 = src0DAG->insn.getSrc(src0ID);
    const ir::Register src1 = src1DAG->insn.getSrc(src1ID);
    if (src0 != src1)
      return false;
    // Ensure they contain the same values
    return src0DAG->child[src0ID] == src1DAG->child[src1ID];
  }

  Selection::Opaque::Opaque(GenContext &ctx) :
    ctx(ctx), block(NULL),
    curr(ctx.getSimdWidth()), file(ctx.getFunction().getRegisterFile()),
    maxInsnNum(ctx.getFunction().getLargestBlockSize()), dagPool(maxInsnNum),
    stateNum(0), vectorNum(0), bwdCodeGeneration(false), storeThreadMap(false),
    currAuxLabel(ctx.getFunction().labelNum()), bHas32X32Mul(false), bHasLongType(false),
    bHasDoubleType(false), bHasHalfType(false), bLongRegRestrict(false),
    ldMsgOrder(LD_MSG_ORDER_IVB), slowByteGather(false)
  {
    const ir::Function &fn = ctx.getFunction();
    this->regNum = fn.regNum();
    this->regDAG.resize(regNum);
    this->insnDAG.resize(maxInsnNum);
  }

  Selection::Opaque::~Opaque(void) {
    for (auto it = blockList.begin(); it != blockList.end();) {
      SelectionBlock &block = *it;
      ++it;
      this->deleteSelectionBlock(&block);
    }
  }

  SelectionInstruction*
  Selection::Opaque::create(SelectionOpcode opcode, uint32_t dstNum, uint32_t srcNum)
  {
    const size_t regSize =  (dstNum+srcNum)*sizeof(GenRegister);
    const size_t size = sizeof(SelectionInstruction) + regSize;
    void *ptr = insnAllocator.allocate(size);
    return new (ptr) SelectionInstruction(opcode, dstNum, srcNum);
  }

  void Selection::Opaque::startBackwardGeneration(void) {
    this->bwdCodeGeneration = true;
  }

  void Selection::Opaque::endBackwardGeneration(void) {
    for (auto it = bwdList.rbegin(); it != bwdList.rend();) {
      SelectionInstruction &insn = *it;
      auto toRemoveIt = it--;
      bwdList.erase(toRemoveIt);
      this->block->prepend(&insn);
    }

    this->bwdCodeGeneration = false;
  }

  uint32_t Selection::Opaque::getLargestBlockSize(void) const {
    size_t maxInsnNum = 0;
    for (const auto &bb : blockList)
      maxInsnNum = std::max(maxInsnNum, bb.insnList.size());
    return uint32_t(maxInsnNum);
  }

  void Selection::Opaque::appendBlock(const ir::BasicBlock &bb) {
    this->block = this->newSelectionBlock(&bb);
    this->blockList.push_back(this->block);
  }

  SelectionInstruction *Selection::Opaque::appendInsn(SelectionOpcode opcode,
                                                      uint32_t dstNum,
                                                      uint32_t srcNum)
  {
    GBE_ASSERT(dstNum <= SelectionInstruction::MAX_DST_NUM && srcNum <= SelectionInstruction::MAX_SRC_NUM);
    GBE_ASSERT(this->block != NULL);
    SelectionInstruction *insn = this->create(opcode, dstNum, srcNum);
    insn->setDBGInfo(DBGInfo);
    if (this->bwdCodeGeneration)
      this->bwdList.push_back(insn);
    else
      this->block->append(insn);
    insn->state = this->curr;
    return insn;
  }

  SelectionVector *Selection::Opaque::appendVector(void) {
    GBE_ASSERT(this->block != NULL);
    SelectionVector *vector = this->newSelectionVector();

    if (this->bwdCodeGeneration)
      vector->insn = this->bwdList.back();
    else
      vector->insn = this->block->insnList.back();
    this->block->append(vector);
    this->vectorNum++;
    return vector;
  }

  bool Selection::Opaque::spillRegs(const SpilledRegs &spilledRegs,
                                    uint32_t registerPool) {
    GBE_ASSERT(registerPool != 0);

    for (auto &block : blockList)
      for (auto &insn : block.insnList) {
        // spill / unspill insn should be skipped when do spilling
        if(insn.opcode == SEL_OP_SPILL_REG
           || insn.opcode == SEL_OP_UNSPILL_REG)
          continue;
        const int simdWidth = insn.state.execWidth;

        const uint32_t srcNum = insn.srcNum, dstNum = insn.dstNum;
        struct RegSlot {
          RegSlot(ir::Register _reg, uint8_t _srcID,
                   uint8_t _poolOffset, bool _isTmp, uint32_t _addr)
                 : reg(_reg), srcID(_srcID), poolOffset(_poolOffset), isTmpReg(_isTmp), addr(_addr)
          {};
          ir::Register reg;
          union {
            uint8_t srcID;
            uint8_t dstID;
          };
          uint8_t poolOffset;
          bool isTmpReg;
          int32_t addr;
        };
        uint8_t poolOffset = 1; // keep one for scratch message header
        vector <struct RegSlot> regSet;
        for (uint32_t srcID = 0; srcID < srcNum; ++srcID) {
          const GenRegister selReg = insn.src(srcID);
          const ir::Register reg = selReg.reg();
          auto it = spilledRegs.find(reg);
          if(it != spilledRegs.end()
             && selReg.file == GEN_GENERAL_REGISTER_FILE
             && selReg.physical == 0) {
            ir::RegisterFamily family = getRegisterFamily(reg);
            if(family == ir::FAMILY_QWORD && poolOffset == 1) {
              poolOffset += simdWidth / 8; // qword register fill could not share the scratch read message payload register
            }
            struct RegSlot regSlot(reg, srcID, poolOffset,
                                   it->second.isTmpReg,
                                   it->second.addr);
            if(family == ir::FAMILY_QWORD) {
              poolOffset += 2 * simdWidth / 8;
            } else {
              poolOffset += simdWidth / 8;
            }
            regSet.push_back(regSlot);
          }
        }

        if (poolOffset > ctx.reservedSpillRegs)
          return false;
        // FIXME, to support post register allocation scheduling,
        // put all the reserved register to the spill/unspill's destination registers.
        // This is not the best way. We need to refine the spill/unspill instruction to
        // only use passed in registers and don't access hard coded offset in the future.
        while(!regSet.empty()) {
          struct RegSlot regSlot = regSet.back();
          regSet.pop_back();
          const GenRegister selReg = insn.src(regSlot.srcID);
          if (!regSlot.isTmpReg) {
          /* For temporary registers, we don't need to unspill. */
            SelectionInstruction *unspill = this->create(SEL_OP_UNSPILL_REG,
                                            1 + (ctx.reservedSpillRegs * 8) / ctx.getSimdWidth(), 0);
            unspill->state = GenInstructionState(simdWidth);
            unspill->state.noMask = 1;
            unspill->dst(0) = GenRegister(GEN_GENERAL_REGISTER_FILE,
                                          registerPool + regSlot.poolOffset, 0,
                                          selReg.type, selReg.vstride,
                                          selReg.width, selReg.hstride);
            for(uint32_t i = 1; i < 1 + (ctx.reservedSpillRegs * 8) / ctx.getSimdWidth(); i++)
              unspill->dst(i) = ctx.getSimdWidth() == 8 ?
                                GenRegister::vec8(GEN_GENERAL_REGISTER_FILE, registerPool + (i - 1), 0 ) :
                                GenRegister::vec16(GEN_GENERAL_REGISTER_FILE, registerPool + (i - 1) * 2, 0);
            unspill->extra.scratchOffset = regSlot.addr + selReg.quarter * 4 * simdWidth;
            unspill->extra.scratchMsgHeader = registerPool;
            insn.prepend(*unspill);
          }

          GenRegister src = insn.src(regSlot.srcID);
          // change nr/subnr, keep other register settings
          src.nr = registerPool + regSlot.poolOffset; src.subnr = 0; src.physical = 1;
          insn.src(regSlot.srcID) = src;
        };

        /*
          To save one register, registerPool + 1 was used by both
          the src0 as source and other operands as payload. To avoid
          side effect, we use a stack model to push all operands
          register, and spill the 0th dest at last. As all the spill
          will be append to the current instruction. Then the last spill
          instruction will be the first instruction after current
          instruction. Thus the registerPool + 1 still contain valid
          data.
         */
        for (uint32_t dstID = 0; dstID < dstNum; ++dstID) {
          const GenRegister selReg = insn.dst(dstID);
          const ir::Register reg = selReg.reg();
          auto it = spilledRegs.find(reg);
          if(it != spilledRegs.end()
             && selReg.file == GEN_GENERAL_REGISTER_FILE
             && selReg.physical == 0) {
            ir::RegisterFamily family = getRegisterFamily(reg);
            if(family == ir::FAMILY_QWORD && poolOffset == 1) {
              poolOffset += simdWidth / 8; // qword register spill could not share the scratch write message payload register
            }
            struct RegSlot regSlot(reg, dstID, poolOffset,
                                   it->second.isTmpReg,
                                   it->second.addr);
            if (family == ir::FAMILY_QWORD) poolOffset += 2 * simdWidth / 8;
            else poolOffset += simdWidth / 8;
            regSet.push_back(regSlot);
          }
        }

        if (poolOffset > ctx.reservedSpillRegs)
          return false;
        while(!regSet.empty()) {
          struct RegSlot regSlot = regSet.back();
          regSet.pop_back();
          const GenRegister selReg = insn.dst(regSlot.dstID);
          if(!regSlot.isTmpReg) {
            /* For temporary registers, we don't need to unspill. */
            SelectionInstruction *spill = this->create(SEL_OP_SPILL_REG,
                                          (ctx.reservedSpillRegs * 8) / ctx.getSimdWidth() , 1);
            spill->state  = insn.state;//GenInstructionState(simdWidth);
            spill->state.accWrEnable = 0;
            spill->state.saturate = 0;
            if (insn.opcode == SEL_OP_SEL)
              spill->state.predicate = GEN_PREDICATE_NONE;
            spill->src(0) = GenRegister(GEN_GENERAL_REGISTER_FILE,
                                        registerPool + regSlot.poolOffset, 0,
                                        selReg.type, selReg.vstride,
                                        selReg.width, selReg.hstride);
            spill->extra.scratchOffset = regSlot.addr + selReg.quarter * 4 * simdWidth;
            spill->extra.scratchMsgHeader = registerPool;
            for(uint32_t i = 0; i < 0 + (ctx.reservedSpillRegs * 8) / ctx.getSimdWidth(); i++)
              spill->dst(i) = ctx.getSimdWidth() == 8 ?
                                GenRegister::vec8(GEN_GENERAL_REGISTER_FILE, registerPool + (i), 0 ) :
                                GenRegister::vec16(GEN_GENERAL_REGISTER_FILE, registerPool + (i) * 2, 0);
            insn.append(*spill);
          }

          GenRegister dst = insn.dst(regSlot.dstID);
          // change nr/subnr, keep other register settings
          dst.physical =1; dst.nr = registerPool + regSlot.poolOffset; dst.subnr = 0;
          insn.dst(regSlot.dstID)= dst;
        }
      }
    return true;
  }

  ir::Register Selection::Opaque::replaceSrc(SelectionInstruction *insn, uint32_t regID, ir::Type type, bool needMov) {
    SelectionBlock *block = insn->parent;
    const uint32_t simdWidth = insn->state.execWidth;
    ir::Register tmp;
    GenRegister gr;

    // This will append the temporary register in the instruction block
    this->block = block;
    tmp = this->reg(ir::getFamily(type), simdWidth == 1);
    gr =  this->selReg(tmp, type);
    if (needMov) {
      // Generate the MOV instruction and replace the register in the instruction
      SelectionInstruction *mov = this->create(SEL_OP_MOV, 1, 1);
      mov->src(0) = GenRegister::retype(insn->src(regID), gr.type);
      mov->state = GenInstructionState(simdWidth);
      if(this->block->removeSimpleIfEndif){
        mov->state.predicate = GEN_PREDICATE_NORMAL;
        mov->state.flag = 0;
        mov->state.subFlag = 0;
      }
      if (this->isScalarReg(insn->src(regID).reg()))
        mov->state.noMask = 1;
      mov->dst(0) = gr;
      insn->prepend(*mov);
    }
    insn->src(regID) = gr;

    return tmp;
  }

  ir::Register Selection::Opaque::replaceDst(SelectionInstruction *insn, uint32_t regID, ir::Type type, bool needMov) {
    SelectionBlock *block = insn->parent;
    uint32_t simdWidth;
    if (!GenRegister::isNull(insn->dst(regID)))
      simdWidth = this->isScalarReg(insn->dst(regID).reg()) ? 1 : insn->state.execWidth;
    else {
      GBE_ASSERT(needMov == false);
      simdWidth = insn->state.execWidth;
    }
    ir::Register tmp;
    GenRegister gr;
    this->block = block;
    tmp = this->reg(ir::getFamily(type));
    gr = this->selReg(tmp, type);
    if (needMov) {
    // Generate the MOV instruction and replace the register in the instruction
      SelectionInstruction *mov = this->create(SEL_OP_MOV, 1, 1);
      mov->dst(0) = GenRegister::retype(insn->dst(regID), gr.type);
      mov->state = GenInstructionState(simdWidth);
      if(this->block->removeSimpleIfEndif){
        mov->state.predicate = GEN_PREDICATE_NORMAL;
        mov->state.flag = 0;
        mov->state.subFlag = 0;
      }
      if (simdWidth == 1) {
        mov->state.noMask = 1;
        mov->src(0) = GenRegister::retype(GenRegister::vec1(GEN_GENERAL_REGISTER_FILE, gr.reg()), gr.type);
      } else
        mov->src(0) = gr;
      insn->append(*mov);
    }
    insn->dst(regID) = gr;
    return tmp;
  }

#define SEL_REG(SIMD16, SIMD8, SIMD1) \
  if (ctx.sel->isScalarReg(reg) == true) \
    return GenRegister::retype(GenRegister::SIMD1(reg), genType); \
  else if (simdWidth == 8) \
    return GenRegister::retype(GenRegister::SIMD8(reg), genType); \
  else { \
    GBE_ASSERT (simdWidth == 16); \
    return GenRegister::retype(GenRegister::SIMD16(reg), genType); \
  }

  GenRegister Selection::Opaque::selReg(ir::Register reg, ir::Type type) const {
    using namespace ir;
    const uint32_t genType = getGenType(type);
    const uint32_t simdWidth = ctx.getSimdWidth();
    const RegisterData data = file.get(reg);
    const RegisterFamily family = data.family;
    switch (family) {
      case FAMILY_BOOL: SEL_REG(uw16grf, uw8grf, uw1grf); break;
      case FAMILY_WORD: SEL_REG(uw16grf, uw8grf, uw1grf); break;
      case FAMILY_BYTE: SEL_REG(ub16grf, ub8grf, ub1grf); break;
      case FAMILY_DWORD: SEL_REG(f16grf, f8grf, f1grf); break;
      case FAMILY_QWORD:
        if (!this->hasLongType()) {
          SEL_REG(ud16grf, ud8grf, ud1grf);
        } else {
          SEL_REG(ul16grf, ul8grf, ul1grf);
        }
        break;
      default: NOT_SUPPORTED;
    }
    GBE_ASSERT(false);
    return GenRegister();
  }

#undef SEL_REG

  GenRegister Selection::Opaque::selRegQn(ir::Register reg, uint32_t q, ir::Type type) const {
    GenRegister sreg = this->selReg(reg, type);
    sreg.quarter = q;
    return sreg;
  }

  /*! Syntactic sugar for method declaration */
  typedef const GenRegister &Reg;

  void Selection::Opaque::LABEL(ir::LabelIndex index) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_LABEL, 0, 0);
    insn->index = index.value();
  }

  void Selection::Opaque::BARRIER(GenRegister src, GenRegister fence, uint32_t barrierType) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_BARRIER, 1, 1);
    insn->src(0) = src;
    insn->dst(0) = fence;
    insn->extra.barrierType = barrierType;
  }

  void Selection::Opaque::FENCE(GenRegister dst) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_FENCE, 1, 0);
    insn->dst(0) = dst;
  }

  int Selection::Opaque::JMPI(Reg src, ir::LabelIndex index, ir::LabelIndex origin) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_JMPI, 0, 1);
    insn->src(0) = src;
    insn->index = index.value();
    ir::LabelIndex start, end;
    if (origin.value() < index.value()) {
    // Forward Jump, need to exclude the target BB. Because we
    // need to jump to the beginning of it.
      start = origin;
      end = ir::LabelIndex(index.value() - 1);
    } else {
      start = index;
      end = origin;
    }
    // FIXME, this longjmp check is too hacky. We need to support instruction
    // insertion at code emission stage in the future.
    insn->extra.longjmp = ctx.getFunction().getDistance(start, end) > 8000;
    return insn->extra.longjmp ? 2 : 1;
  }

  void Selection::Opaque::BRD(Reg src, ir::LabelIndex jip) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_BRD, 0, 1);
    insn->src(0) = src;
    insn->index = jip.value();
  }

  void Selection::Opaque::BRC(Reg src, ir::LabelIndex jip, ir::LabelIndex uip) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_BRC, 0, 1);
    insn->src(0) = src;
    insn->index = jip.value();
    insn->index1 = uip.value();
  }

  void Selection::Opaque::IF(Reg src, ir::LabelIndex jip, ir::LabelIndex uip) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_IF, 0, 1);
    insn->src(0) = src;
    insn->index = jip.value();
    insn->index1 = uip.value();
  }

  void Selection::Opaque::ELSE(Reg src, ir::LabelIndex jip, ir::LabelIndex elseLabel) {

    SelectionInstruction *insn = this->appendInsn(SEL_OP_ELSE, 0, 1);
    insn->src(0) = src;
    insn->index = jip.value();
    this->LABEL(elseLabel);
  }

  void Selection::Opaque::ENDIF(Reg src, ir::LabelIndex jip, ir::LabelIndex endifLabel) {
    if(endifLabel == 0)
      this->block->endifLabel = this->newAuxLabel();
    else
      this->block->endifLabel = endifLabel;
    this->LABEL(this->block->endifLabel);
    SelectionInstruction *insn = this->appendInsn(SEL_OP_ENDIF, 0, 1);
    insn->src(0) = src;
    insn->index = this->block->endifLabel.value();
  }

  void Selection::Opaque::WHILE(Reg src, ir::LabelIndex jip) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_WHILE, 0, 1);
    insn->src(0) = src;
    insn->index = jip.value();
  }

  void Selection::Opaque::CMP(uint32_t conditional, Reg src0, Reg src1, Reg dst) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_CMP, 1, 2);
    insn->src(0) = src0;
    insn->src(1) = src1;
    insn->dst(0) = dst;
    insn->extra.function = conditional;
  }

  void Selection::Opaque::SEL_CMP(uint32_t conditional, Reg dst, Reg src0, Reg src1) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_SEL_CMP, 1, 2);
    insn->dst(0) = dst;
    insn->src(0) = src0;
    insn->src(1) = src1;
    insn->extra.function = conditional;
  }
  void Selection::Opaque::INDIRECT_MOVE(Reg dst, Reg tmp, Reg base, Reg regOffset, uint32_t immOffset) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_INDIRECT_MOVE, 2, 2);
    insn->dst(0) = dst;
    insn->dst(1) = tmp;
    insn->src(0) = base;
    insn->src(1) = regOffset;
    insn->extra.indirect_offset = immOffset;
  }

  void Selection::Opaque::ATOMIC(Reg dst, uint32_t function,
                                 uint32_t msgPayload, Reg src0,
                                 Reg src1, Reg src2, GenRegister bti,
                                 vector<GenRegister> temps) {
    unsigned dstNum = 1 + temps.size();
    SelectionInstruction *insn = this->appendInsn(SEL_OP_ATOMIC, dstNum, msgPayload + 1);

    if (bti.file != GEN_IMMEDIATE_VALUE) {
      insn->state.flag = 0;
      insn->state.subFlag = 1;
    }

    insn->dst(0) = dst;
    if(temps.size()) {
      insn->dst(1) = temps[0];
      insn->dst(2) = temps[1];
    }

    insn->src(0) = src0;
    if(msgPayload > 1) insn->src(1) = src1;
    if(msgPayload > 2) insn->src(2) = src2;
    insn->src(msgPayload) = bti;

    insn->extra.function = function;
    insn->extra.elem = msgPayload;

    SelectionVector *vector = this->appendVector();
    vector->regNum = msgPayload; //bti not included in SelectionVector
    vector->offsetID = 0;
    vector->reg = &insn->src(0);
    vector->isSrc = 1;
  }

  void Selection::Opaque::EOT(void) { this->appendInsn(SEL_OP_EOT, 0, 0); }
  void Selection::Opaque::NOP(void) { this->appendInsn(SEL_OP_NOP, 0, 0); }
  void Selection::Opaque::WAIT(uint32_t n)
  {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_WAIT, 0, 0);
    insn->extra.waitType = n;
  }

  void Selection::Opaque::READ64(Reg addr,
                                 const GenRegister *dst,
                                 const GenRegister *tmp,
                                 uint32_t elemNum,
                                 const GenRegister bti,
                                 bool native_long,
                                 vector<GenRegister> temps)
  {
    SelectionInstruction *insn = NULL;
    SelectionVector *srcVector = NULL;
    SelectionVector *dstVector = NULL;

    if (!native_long) {
      unsigned dstNum = elemNum + temps.size();
      insn = this->appendInsn(SEL_OP_READ64, dstNum, 2);
      srcVector = this->appendVector();
      dstVector = this->appendVector();
      // Regular instruction to encode
      for (uint32_t elemID = 0; elemID < elemNum; ++elemID)
        insn->dst(elemID) = dst[elemID];

      // flagTemp don't need to be put in SelectionVector
      if (temps.size()) {
        insn->dst(elemNum) = temps[0];
        insn->dst(elemNum + 1) = temps[1];
      }
    } else {
      unsigned dstNum = elemNum*2 + temps.size();
      insn = this->appendInsn(SEL_OP_READ64, dstNum, 2);
      srcVector = this->appendVector();
      dstVector = this->appendVector();

      for (uint32_t elemID = 0; elemID < elemNum; ++elemID)
        insn->dst(elemID) = tmp[elemID];

      for (uint32_t elemID = 0; elemID < elemNum; ++elemID)
        insn->dst(elemID + elemNum) = dst[elemID];

      // flagTemp don't need to be put in SelectionVector
      if (temps.size()) {
        insn->dst(2*elemNum) = temps[0];
        insn->dst(2*elemNum + 1) = temps[1];
      }
    }

    if (bti.file != GEN_IMMEDIATE_VALUE) {
      insn->state.flag = 0;
      insn->state.subFlag = 1;
    }

    insn->src(0) = addr;
    insn->src(1) = bti;

    insn->extra.elem = elemNum;

    dstVector->regNum = elemNum;
    dstVector->isSrc = 0;
    dstVector->offsetID = 0;
    dstVector->reg = &insn->dst(0);

    srcVector->regNum = 1;
    srcVector->offsetID = 0;
    srcVector->isSrc = 1;
    srcVector->reg = &insn->src(0);
  }

  void Selection::Opaque::UNTYPED_READ(Reg addr,
                                       const GenRegister *dst,
                                       uint32_t elemNum,
                                       GenRegister bti,
                                       vector<GenRegister> temps)
  {
    unsigned dstNum = elemNum + temps.size();
    SelectionInstruction *insn = this->appendInsn(SEL_OP_UNTYPED_READ, dstNum, 2);
    SelectionVector *srcVector = this->appendVector();
    SelectionVector *dstVector = this->appendVector();
    if (this->isScalarReg(dst[0].reg()))
      insn->state.noMask = 1;
    // Regular instruction to encode
    for (uint32_t elemID = 0; elemID < elemNum; ++elemID)
      insn->dst(elemID) = dst[elemID];
    if (temps.size()) {
      insn->dst(elemNum) = temps[0];
      insn->dst(elemNum + 1) = temps[1];
    }

    insn->src(0) = addr;
    insn->src(1) = bti;
    if (bti.file != GEN_IMMEDIATE_VALUE) {
      insn->state.flag = 0;
      insn->state.subFlag = 1;
    }

    insn->extra.elem = elemNum;

    // Sends require contiguous allocation
    dstVector->regNum = elemNum;
    dstVector->isSrc = 0;
    dstVector->offsetID = 0;
    dstVector->reg = &insn->dst(0);

    srcVector->regNum = 1;
    srcVector->isSrc = 1;
    srcVector->offsetID = 0;
    srcVector->reg = &insn->src(0);
  }

  void Selection::Opaque::WRITE64(Reg addr,
                                  const GenRegister *src,
                                  const GenRegister *tmp,
                                  uint32_t srcNum,
                                  GenRegister bti,
                                  bool native_long,
                                  vector<GenRegister> temps)
  {
    SelectionVector *vector = NULL;
    SelectionInstruction *insn = NULL;

    if (!native_long) {
      unsigned dstNum = temps.size();
      insn = this->appendInsn(SEL_OP_WRITE64, dstNum, srcNum + 2);
      vector = this->appendVector();
      // Register layout:
      // dst: (flagTemp)
      // src: addr, srcNum, bti
      insn->src(0) = addr;
      for (uint32_t elemID = 0; elemID < srcNum; ++elemID)
        insn->src(elemID + 1) = src[elemID];

      insn->src(srcNum+1) = bti;
      if (temps.size()) {
        insn->dst(0) = temps[0];
        insn->dst(1) = temps[1];
      }
      insn->extra.elem = srcNum;

      vector->regNum = srcNum + 1;
      vector->offsetID = 0;
      vector->reg = &insn->src(0);
      vector->isSrc = 1;
    } else { // handle the native long case
      unsigned dstNum = srcNum + temps.size();
      // Register layout:
      // dst: srcNum, (flagTemp)
      // src: srcNum, addr, srcNum, bti.
      insn = this->appendInsn(SEL_OP_WRITE64, dstNum, srcNum*2 + 2);
      vector = this->appendVector();

      for (uint32_t elemID = 0; elemID < srcNum; ++elemID)
        insn->src(elemID) = src[elemID];

      insn->src(srcNum) = addr;
      for (uint32_t elemID = 0; elemID < srcNum; ++elemID)
        insn->src(srcNum + 1 + elemID) = tmp[0];

      insn->src(srcNum*2+1) = bti;
      /* We also need to add the tmp reigster to dst, in order
         to avoid the post schedule error . */
      for (uint32_t elemID = 0; elemID < srcNum; ++elemID)
        insn->dst(elemID) = tmp[0];

      if (temps.size()) {
        insn->dst(srcNum) = temps[0];
        insn->dst(srcNum + 1) = temps[1];
      }
      insn->extra.elem = srcNum;

      vector->regNum = srcNum + 1;
      vector->offsetID = srcNum;
      vector->reg = &insn->src(srcNum);
      vector->isSrc = 1;
    }

    if (bti.file != GEN_IMMEDIATE_VALUE) {
      insn->state.flag = 0;
      insn->state.subFlag = 1;
    }
  }

  void Selection::Opaque::UNTYPED_WRITE(Reg addr,
                                        const GenRegister *src,
                                        uint32_t elemNum,
                                        GenRegister bti,
                                        vector<GenRegister> temps)
  {
    unsigned dstNum = temps.size();
    unsigned srcNum = elemNum + 2 + temps.size();
    SelectionInstruction *insn = this->appendInsn(SEL_OP_UNTYPED_WRITE, dstNum, srcNum);
    SelectionVector *vector = this->appendVector();

    if (bti.file != GEN_IMMEDIATE_VALUE) {
      insn->state.flag = 0;
      insn->state.subFlag = 1;
    }

    // Regular instruction to encode
    insn->src(0) = addr;
    for (uint32_t elemID = 0; elemID < elemNum; ++elemID)
      insn->src(elemID+1) = src[elemID];
    insn->src(elemNum+1) = bti;
    if (temps.size()) {
      insn->dst(0) = temps[0];
      insn->dst(1) = temps[1];
      insn->src(elemNum + 2) = temps[0];
      insn->src(elemNum + 3) = temps[1];
    }
    insn->extra.elem = elemNum;

    // Sends require contiguous allocation for the sources
    vector->regNum = elemNum+1;
    vector->reg = &insn->src(0);
    vector->offsetID = 0;
    vector->isSrc = 1;
  }

  void Selection::Opaque::BYTE_GATHER(Reg dst, Reg addr,
                                      uint32_t elemSize,
                                      GenRegister bti,
                                      vector<GenRegister> temps) {
    unsigned dstNum = 1 + temps.size();
    SelectionInstruction *insn = this->appendInsn(SEL_OP_BYTE_GATHER, dstNum, 2);
    SelectionVector *srcVector = this->appendVector();
    SelectionVector *dstVector = this->appendVector();

    if (bti.file != GEN_IMMEDIATE_VALUE) {
      insn->state.flag = 0;
      insn->state.subFlag = 1;
    }

    if (this->isScalarReg(dst.reg()))
      insn->state.noMask = 1;
    // Instruction to encode
    insn->src(0) = addr;
    insn->src(1) = bti;
    insn->dst(0) = dst;
    if (temps.size()) {
      insn->dst(1) = temps[0];
      insn->dst(2) = temps[1];
    }

    insn->extra.elem = elemSize;

    // byte gather requires vector in the sense that scalar are not allowed
    // (yet)
    dstVector->regNum = 1;
    dstVector->isSrc = 0;
    dstVector->offsetID = 0;
    dstVector->reg = &insn->dst(0);
    srcVector->regNum = 1;
    srcVector->isSrc = 1;
    srcVector->offsetID = 0;
    srcVector->reg = &insn->src(0);
  }

  void Selection::Opaque::BYTE_SCATTER(Reg addr, Reg src, uint32_t elemSize,
                                       GenRegister bti, vector<GenRegister> temps) {
    unsigned dstNum = temps.size();
    SelectionInstruction *insn = this->appendInsn(SEL_OP_BYTE_SCATTER, dstNum, 3);
    SelectionVector *vector = this->appendVector();

    if (bti.file != GEN_IMMEDIATE_VALUE) {
      insn->state.flag = 0;
      insn->state.subFlag = 1;
    }

    if (temps.size()) {
      insn->dst(0) = temps[0];
      insn->dst(1) = temps[1];
    }
    // Instruction to encode
    insn->src(0) = addr;
    insn->src(1) = src;
    insn->src(2) = bti;
    insn->extra.elem = elemSize;

    // value and address are contiguous in the send
    vector->regNum = 2;
    vector->isSrc = 1;
    vector->offsetID = 0;
    vector->reg = &insn->src(0);
  }

  void Selection::Opaque::DWORD_GATHER(Reg dst, Reg addr, uint32_t bti) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_DWORD_GATHER, 1, 1);
    SelectionVector *vector = this->appendVector();
    SelectionVector *srcVector = this->appendVector();

    if (this->isScalarReg(dst.reg()))
      insn->state.noMask = 1;
    insn->src(0) = addr;
    insn->dst(0) = dst;
    insn->setbti(bti);
    vector->regNum = 1;
    vector->isSrc = 0;
    vector->offsetID = 0;
    vector->reg = &insn->dst(0);
    srcVector->regNum = 1;
    srcVector->isSrc = 1;
    srcVector->offsetID = 0;
    srcVector->reg = &insn->src(0);
  }

  void Selection::Opaque::UNPACK_BYTE(const GenRegister *dst, const GenRegister src, uint32_t elemSize, uint32_t elemNum) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_UNPACK_BYTE, elemNum, 1);
    insn->src(0) = src;
    insn->extra.elem = 4 / elemSize;
    for(uint32_t i = 0; i < elemNum; i++)
      insn->dst(i) = dst[i];
  }
  void Selection::Opaque::PACK_BYTE(const GenRegister dst, const GenRegister *src, uint32_t elemSize, uint32_t elemNum) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_PACK_BYTE, 1, elemNum);
    for(uint32_t i = 0; i < elemNum; i++)
      insn->src(i) = src[i];
    insn->extra.elem = 4 / elemSize;
    insn->dst(0) = dst;
  }

  void Selection::Opaque::UNPACK_LONG(const GenRegister dst, const GenRegister src) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_UNPACK_LONG, 1, 1);
    insn->src(0) = src;
    insn->dst(0) = dst;
  }

  void Selection::Opaque::PACK_LONG(const GenRegister dst, const GenRegister src) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_PACK_LONG, 1, 1);
    insn->src(0) = src;
    insn->dst(0) = dst;
  }

  void Selection::Opaque::MATH(Reg dst, uint32_t function, Reg src0, Reg src1) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_MATH, 1, 2);
    insn->dst(0) = dst;
    insn->src(0) = src0;
    insn->src(1) = src1;
    insn->extra.function = function;
  }

  void Selection::Opaque::MATH(Reg dst, uint32_t function, Reg src) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_MATH, 1, 1);
    insn->dst(0) = dst;
    insn->src(0) = src;
    insn->extra.function = function;
  }

  void Selection::Opaque::I64MUL(Reg dst, Reg src0, Reg src1, GenRegister *tmp, bool native_long) {
    SelectionInstruction *insn = NULL;
    if (native_long)
      insn = this->appendInsn(SEL_OP_I64MUL, 2, 2);
    else
      insn = this->appendInsn(SEL_OP_I64MUL, 7, 2);

    insn->dst(0) = dst;
    insn->src(0) = src0;
    insn->src(1) = src1;

    if (native_long) {
      insn->dst(1) = tmp[0];
    } else {
      for (int i = 0; i < 6; i++)
        insn->dst(i + 1) = tmp[i];
    }
  }

  void Selection::Opaque::I64DIV(Reg dst, Reg src0, Reg src1, GenRegister* tmp, int tmp_num) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_I64DIV, tmp_num + 1, 2);
    insn->dst(0) = dst;
    insn->src(0) = src0;
    insn->src(1) = src1;
    for(int i = 0; i < tmp_num; i++)
      insn->dst(i + 1) = tmp[i];
  }

  void Selection::Opaque::I64REM(Reg dst, Reg src0, Reg src1, GenRegister* tmp, int tmp_num) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_I64REM, tmp_num + 1, 2);
    insn->dst(0) = dst;
    insn->src(0) = src0;
    insn->src(1) = src1;
    for(int i = 0; i < tmp_num; i++)
      insn->dst(i + 1) = tmp[i];
  }

  void Selection::Opaque::F64DIV(Reg dst, Reg src0, Reg src1, GenRegister* tmp, int tmpNum) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_F64DIV, tmpNum + 1, 2);
    insn->dst(0) = dst;
    insn->src(0) = src0;
    insn->src(1) = src1;
    for(int i = 0; i < tmpNum; i++)
      insn->dst(i + 1) = tmp[i];
  }

  void Selection::Opaque::ALU1(SelectionOpcode opcode, Reg dst, Reg src) {
    SelectionInstruction *insn = this->appendInsn(opcode, 1, 1);
    insn->dst(0) = dst;
    insn->src(0) = src;
  }

  void Selection::Opaque::ALU1WithTemp(SelectionOpcode opcode, Reg dst, Reg src, Reg temp) {
    SelectionInstruction *insn = this->appendInsn(opcode, 2, 1);
    insn->dst(0) = dst;
    insn->src(0) = src;
    insn->dst(1) = temp;
  }

  void Selection::Opaque::ALU2(SelectionOpcode opcode, Reg dst, Reg src0, Reg src1) {
    SelectionInstruction *insn = this->appendInsn(opcode, 1, 2);
    insn->dst(0) = dst;
    insn->src(0) = src0;
    insn->src(1) = src1;
  }

  void Selection::Opaque::ALU2WithTemp(SelectionOpcode opcode, Reg dst, Reg src0, Reg src1, Reg temp) {
    SelectionInstruction *insn = this->appendInsn(opcode, 2, 2);
    insn->dst(0) = dst;
    insn->src(0) = src0;
    insn->src(1) = src1;
    insn->dst(1) = temp;
  }

  void Selection::Opaque::ALU3(SelectionOpcode opcode, Reg dst, Reg src0, Reg src1, Reg src2) {
    SelectionInstruction *insn = this->appendInsn(opcode, 1, 3);
    insn->dst(0) = dst;
    insn->src(0) = src0;
    insn->src(1) = src1;
    insn->src(2) = src2;
  }

  void Selection::Opaque::SIMD_SHUFFLE(Reg dst, Reg src0, Reg src1)
  {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_SIMD_SHUFFLE, 1, 2);
    insn->dst(0) = dst;
    insn->src(0) = src0;
    insn->src(1) = src1;
  }

  GenRegister Selection::Opaque::getLaneIDReg()
  {
    const GenRegister laneID = GenRegister::immv(0x76543210);
    ir::Register r = reg(ir::RegisterFamily::FAMILY_WORD);
    const GenRegister dst = selReg(r, ir::TYPE_U16);

    uint32_t execWidth = curr.execWidth;
    if (execWidth == 8)
      MOV(dst, laneID);
    else {
      push();
      curr.execWidth = 8;
      curr.noMask = 1;
      MOV(dst, laneID);
      //Packed Unsigned Half-Byte Integer Vector does not work
      //have to mock by adding 8 to the singed vector
      const GenRegister eight = GenRegister::immuw(8);
      ADD(GenRegister::offset(dst, 0, 16), dst, eight);
      pop();
    }
    return dst;
  }

  void Selection::Opaque::I64CMP(uint32_t conditional, Reg src0, Reg src1, GenRegister tmp[3]) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_I64CMP, 3, 2);
    insn->src(0) = src0;
    insn->src(1) = src1;
    for(int i=0; i<3; i++)
      insn->dst(i) = tmp[i];
    insn->extra.function = conditional;
  }

  void Selection::Opaque::I64SATADD(Reg dst, Reg src0, Reg src1, GenRegister tmp[5]) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_I64SATADD, 6, 2);
    insn->dst(0) = dst;
    insn->src(0) = src0;
    insn->src(1) = src1;
    for(int i=0; i<5; i++)
      insn->dst(i + 1) = tmp[i];
  }

  void Selection::Opaque::I64SATSUB(Reg dst, Reg src0, Reg src1, GenRegister tmp[5]) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_I64SATSUB, 6, 2);
    insn->dst(0) = dst;
    insn->src(0) = src0;
    insn->src(1) = src1;
    for(int i=0; i<5; i++)
      insn->dst(i + 1) = tmp[i];
  }

  void Selection::Opaque::CONVI64_TO_F(Reg dst, Reg src, GenRegister tmp[6]) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_CONVI64_TO_F, 7, 1);
    insn->dst(0) = dst;
    insn->src(0) = src;
    for(int i = 0; i < 6; i ++)
      insn->dst(i + 1) = tmp[i];
  }

  void Selection::Opaque::CONVF_TO_I64(Reg dst, Reg src, GenRegister tmp[2]) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_CONVF_TO_I64, 3, 1);
    insn->dst(0) = dst;
    insn->src(0) = src;
    for(int i = 0; i < 2; i ++)
      insn->dst(i + 1) = tmp[i];
  }

  void Selection::Opaque::I64MADSAT(Reg dst, Reg src0, Reg src1, Reg src2, GenRegister *tmp, int tmp_num) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_I64MADSAT, tmp_num + 1, 3);
    insn->dst(0) = dst;
    insn->src(0) = src0;
    insn->src(1) = src1;
    insn->src(2) = src2;
    for(int i = 0; i < tmp_num; i ++)
      insn->dst(i + 1) = tmp[i];
  }

  void Selection::Opaque::I64_MUL_HI(Reg dst, Reg src0, Reg src1, GenRegister *tmp, int tmp_num) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_I64_MUL_HI, tmp_num + 1, 2);
    insn->dst(0) = dst;
    insn->src(0) = src0;
    insn->src(1) = src1;
    for(int i = 0; i < tmp_num; i ++)
      insn->dst(i + 1) = tmp[i];
  }

  void Selection::Opaque::I64HADD(Reg dst, Reg src0, Reg src1, GenRegister *tmp, int tmp_num) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_I64HADD, tmp_num + 1, 2);
    insn->dst(0) = dst;
    insn->src(0) = src0;
    insn->src(1) = src1;
    for(int i = 0; i < tmp_num; i ++)
      insn->dst(i + 1) = tmp[i];
  }

  void Selection::Opaque::I64RHADD(Reg dst, Reg src0, Reg src1, GenRegister *tmp, int tmp_num) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_I64RHADD, tmp_num + 1, 2);
    insn->dst(0) = dst;
    insn->src(0) = src0;
    insn->src(1) = src1;
    for(int i = 0; i < tmp_num; i ++)
      insn->dst(i + 1) = tmp[i];
  }

  void Selection::Opaque::I64Shift(SelectionOpcode opcode, Reg dst, Reg src0, Reg src1, GenRegister tmp[6]) {
    SelectionInstruction *insn = this->appendInsn(opcode, 7, 2);
    insn->dst(0) = dst;
    insn->src(0) = src0;
    insn->src(1) = src1;
    for(int i = 0; i < 6; i ++)
      insn->dst(i + 1) = tmp[i];
  }

  void Selection::Opaque::CALC_TIMESTAMP(GenRegister ts[4], int tsN, GenRegister tmp, uint32_t pointNum, uint32_t tsType) {
    SelectionInstruction *insn = NULL;
    if (!this->hasLongType()) {
      insn = this->appendInsn(SEL_OP_CALC_TIMESTAMP, tsN + 1, tsN);
    } else {// No need for tmp
      insn = this->appendInsn(SEL_OP_CALC_TIMESTAMP, tsN, tsN);
    }

    for (int i = 0; i < tsN; i++) {
      insn->src(i) = ts[i];
      insn->dst(i) = ts[i];
    }

    if (!this->hasLongType())
      insn->dst(tsN) = tmp;

    insn->extra.pointNum = static_cast<uint16_t>(pointNum);
    insn->extra.timestampType = static_cast<uint16_t>(tsType);
  }

  void Selection::Opaque::STORE_PROFILING(uint32_t profilingType, uint32_t bti,
                GenRegister tmp0, GenRegister tmp1, GenRegister ts[4], int tsNum) {
    if (tsNum == 3) { // SIMD16 mode
      SelectionInstruction *insn = this->appendInsn(SEL_OP_STORE_PROFILING, 1, 3);
      for (int i = 0; i < 3; i++)
        insn->src(i) = ts[i];
      insn->dst(0) = tmp0;

      insn->extra.profilingType = static_cast<uint16_t>(profilingType);
      insn->extra.profilingBTI = static_cast<uint16_t>(bti);
    } else { // SIMD8 mode
      GBE_ASSERT(tsNum == 5);
      SelectionInstruction *insn = this->appendInsn(SEL_OP_STORE_PROFILING, 2, 5);
      SelectionVector *dstVector = this->appendVector();
      for (int i = 0; i < 5; i++)
        insn->src(i) = ts[i];
      insn->dst(0) = tmp0;
      insn->dst(1) = tmp1;

      dstVector->regNum = 2;
      dstVector->isSrc = 0;
      dstVector->offsetID = 0;
      dstVector->reg = &insn->dst(0);

      insn->extra.profilingType = static_cast<uint16_t>(profilingType);
      insn->extra.profilingBTI = static_cast<uint16_t>(bti);
    }
  }

  void Selection::Opaque::WORKGROUP_OP(uint32_t wg_op, Reg dst, GenRegister src, GenRegister nextThreadID,
                      GenRegister threadID, GenRegister threadn, GenRegister tmp) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_WORKGROUP_OP, 3, 4);
    insn->extra.workgroupOp = wg_op;
    insn->dst(0) = dst;
    insn->dst(1) = nextThreadID;
    insn->dst(2) = tmp;

    insn->src(0) = src;
    insn->src(1) = nextThreadID;
    insn->src(2) = threadID;
    insn->src(3) = threadn;
  }

  // Boiler plate to initialize the selection library at c++ pre-main
  static SelectionLibrary *selLib = NULL;
  static void destroySelectionLibrary(void) { GBE_DELETE(selLib); }
  static struct SelectionLibraryInitializer {
    SelectionLibraryInitializer(void) {
      selLib = GBE_NEW_NO_ARG(SelectionLibrary);
      atexit(destroySelectionLibrary);
    }
  } selectionLibraryInitializer;

  bool Selection::Opaque::isRoot(const ir::Instruction &insn) const {
    if (insn.hasSideEffect() ||
        insn.isMemberOf<ir::BranchInstruction>() ||
        insn.isMemberOf<ir::LabelInstruction>())
    return true;

    // No side effect, not a branch and no destination? Impossible
    GBE_ASSERT(insn.getDstNum() >= 1);

    // Root if alive outside the block.
    // XXX we should use Value and not registers in liveness info
    const ir::BasicBlock *insnBlock = insn.getParent();
    const ir::Liveness &liveness = this->ctx.getLiveness();
    const ir::Liveness::LiveOut &liveOut = liveness.getLiveOut(insnBlock);
    const ir::Register reg = insn.getDst(0);
    if (liveOut.contains(reg))
      return true;

    // The instruction is only used in the current basic block
    return false;
  }

  bool Selection::Opaque::hasQWord(const ir::Instruction &insn) {
    for (uint32_t i = 0; i < insn.getSrcNum(); i++) {
      const ir::Register reg = insn.getSrc(i);
      if (getRegisterFamily(reg) == ir::FAMILY_QWORD)
        return true;
    }
    for (uint32_t i = 0; i < insn.getDstNum(); i++) {
      const ir::Register reg = insn.getDst(i);
      if (getRegisterFamily(reg) == ir::FAMILY_QWORD)
        return true;
    }
    return false;
  } 

  bool Selection::Opaque::isSimpleBlock(const ir::BasicBlock &bb, uint32_t insnNum) {

    // FIXME should include structured innermost if/else/endif
    if(bb.belongToStructure)
      return false;

    // FIXME scalar reg should not be excluded and just need some special handling.
    for (int32_t insnID = insnNum-1; insnID >= 0; --insnID) {
      SelectionDAG &dag = *insnDAG[insnID];
      const ir::Instruction& insn = dag.insn;
      if ( (insn.getDstNum() && this->isScalarReg(insn.getDst(0)) == true) ||
         insn.isMemberOf<ir::CompareInstruction>() ||
         insn.isMemberOf<ir::SelectInstruction>() ||
         insn.getOpcode() == ir::OP_SIMD_ANY ||
         insn.getOpcode() == ir::OP_SIMD_ALL ||
         insn.getOpcode() == ir::OP_ELSE)
        return false;

      // Most of the QWord(long) related instruction introduce some CMP or
      // more than 10 actual instructions at latter stage.
      if (hasQWord(insn))
        return false;

      // Unaligned load may introduce CMP instruction.
      if ( insn.isMemberOf<ir::LoadInstruction>()) {
        const ir::LoadInstruction &ld = ir::cast<ir::LoadInstruction>(insn);
        if (!ld.isAligned())
          return false;
      }
    }

    // there would generate a extra CMP instruction for predicated BRA with extern flag,
    // should retrun false to keep the if/endif.
    if((insnDAG[insnNum-1]->insn.isMemberOf<ir::BranchInstruction>())){
      if (insnDAG[insnNum-1]->insn.getOpcode() == ir::OP_BRA) {
        const ir::BranchInstruction &insn = ir::cast<ir::BranchInstruction>(insnDAG[insnNum-1]->insn);
        if(insn.isPredicated() && insnDAG[insnNum-1]->child[0] == NULL){
          return false;
        }
      }
    }

    return true;
  }


  uint32_t Selection::Opaque::buildBasicBlockDAG(const ir::BasicBlock &bb)
  {
    using namespace ir;

    // Clear all registers
    for (uint32_t regID = 0; regID < this->regNum; ++regID)
      this->regDAG[regID] = NULL;

    this->block->hasBarrier = false;
    this->block->hasBranch = bb.getLastInstruction()->getOpcode() == OP_BRA ||
                             bb.getLastInstruction()->getOpcode() == OP_RET;
    if (!this->block->hasBranch)
      this->block->endifOffset = -1;

    // Build the DAG on the fly
    uint32_t insnNum = 0;
    const_cast<BasicBlock&>(bb).foreach([&](const Instruction &insn) {
      if (insn.getOpcode() == OP_SYNC)
        this->block->hasBarrier = true;

      // Build a selectionDAG node for instruction
      SelectionDAG *dag = this->newSelectionDAG(insn);

      // Point to non-root children
      const uint32_t srcNum = insn.getSrcNum();
      for (uint32_t srcID = 0; srcID < srcNum; ++srcID) {
        const ir::Register reg = insn.getSrc(srcID);
        SelectionDAG *child = this->regDAG[reg];
        if (child) {
          const ir::Instruction &childInsn = child->insn;
          const uint32_t childSrcNum = childInsn.getSrcNum();

          // We can merge a child only if its sources are still valid
          bool mergeable = true;
          for (uint32_t otherID = 0; otherID < childSrcNum; ++otherID) {
            const SelectionDAG *srcDAG = child->child[otherID];
            const ir::Register srcReg = childInsn.getSrc(otherID);
            SelectionDAG *currDAG = this->regDAG[srcReg];
            if (srcDAG != currDAG) {
              mergeable = false;
              break;
            }
          }
          if (mergeable) dag->setAsMergeable(srcID);
          dag->child[srcID] = child;
          // Check whether this bool is used as a normal source
          // oprand other than BRA/SEL.
          if (getRegisterFamily(reg) == FAMILY_BOOL) {
            if ((insn.getOpcode() != OP_BRA &&
                 (insn.getOpcode() != OP_SEL ||
                 (insn.getOpcode() == OP_SEL && srcID != 0))) ||
               (isScalarReg(reg)))
              child->computeBool = true;
          }
          child->isUsed = true;
        } else
          dag->child[srcID] = NULL;
      }

      // Make it a root if we must
      if (this->isRoot(insn)) dag->isRoot = 1;

      // Save the DAG <-> instruction mapping
      this->insnDAG[insnNum++] = dag;

      // Associate all output registers to this instruction
      const uint32_t dstNum = insn.getDstNum();
      for (uint32_t dstID = 0; dstID < dstNum; ++dstID) {
        const ir::Register reg = insn.getDst(dstID);
        this->regDAG[reg] = dag;
      }
    });

    return insnNum;
  }

extern bool OCL_DEBUGINFO; // first defined by calling BVAR in program.cpp
#define SET_SEL_DBGINFO(I)  \
	if(OCL_DEBUGINFO)	 \
	  this->setDBGInfo_SEL(I.DBGInfo)

  void Selection::Opaque::matchBasicBlock(const ir::BasicBlock &bb, uint32_t insnNum)
  {
    // Bottom up code generation
    bool needEndif = this->block->hasBranch == false && !this->block->hasBarrier;
    needEndif = needEndif && bb.needEndif;
    this->block->removeSimpleIfEndif = insnNum < 10 && isSimpleBlock(bb, insnNum);
    if (needEndif && !this->block->removeSimpleIfEndif) {
      if(!bb.needIf) // this basic block is the exit of a structure
        this->ENDIF(GenRegister::immd(0), bb.endifLabel, bb.endifLabel);
      else {
        const ir::BasicBlock *next = bb.getNextBlock();
        this->ENDIF(GenRegister::immd(0), next->getLabelIndex());
        needEndif = false;
      }
    }

    for (int32_t insnID = insnNum-1; insnID >= 0; --insnID) {
      // Process all possible patterns for this instruction
      SelectionDAG &dag = *insnDAG[insnID];
      SET_SEL_DBGINFO(dag.insn);
      if (dag.isRoot) {
        const ir::Instruction &insn = dag.insn;
        const ir::Opcode opcode = insn.getOpcode();
        auto it = selLib->patterns[opcode].begin();
        const auto end = selLib->patterns[opcode].end();

        // Start a new code fragment
        this->startBackwardGeneration();

        if(this->block->removeSimpleIfEndif){
          this->push();
            this->curr.predicate = GEN_PREDICATE_NORMAL;
            this->curr.flag = 0;
            this->curr.subFlag = 0;
        }
        // If there is no branch at the end of this block.

        // Try all the patterns from best to worst
        do {
          if ((*it)->emit(*this, dag))
            break;
          ++it;
        } while (it != end);
        GBE_ASSERT(it != end);

        if(this->block->removeSimpleIfEndif){
            this->curr.predicate = GEN_PREDICATE_NONE;
            this->curr.flag = 0;
            this->curr.subFlag = 0;
          this->pop();
        }
        // If we are in if/endif fix mode, and this block is
        // large enough, we need to insert endif/if pair to eliminate
        // the too long if/endif block.
        if (this->ctx.getIFENDIFFix() &&
            this->block->insnList.size() != 0 &&
            this->block->insnList.size() % 1000 == 0 &&
            this->block->endifLabel.value() != 0) {
          ir::LabelIndex jip = this->block->endifLabel;
          this->ENDIF(GenRegister::immd(0), jip);
          this->push();
            this->curr.predicate = GEN_PREDICATE_NORMAL;
            this->IF(GenRegister::immd(0), jip, jip);
          this->pop();
          this->block->isLargeBlock = true;
        }
        // Output the code in the current basic block
        this->endBackwardGeneration();
      }
    }
  }
#undef SET_SEL_DBGINFO

  void Selection::Opaque::select(void)
  {
    using namespace ir;
    const Function &fn = ctx.getFunction();

    // Perform the selection per basic block
    fn.foreachBlock([&](const BasicBlock &bb) {
      this->dagPool.rewind();
      this->appendBlock(bb);
      const uint32_t insnNum = this->buildBasicBlockDAG(bb);
      this->matchBasicBlock(bb, insnNum);
    });
   }

  void Selection::Opaque::SAMPLE(GenRegister *dst, uint32_t dstNum,
                                 GenRegister *msgPayloads, uint32_t msgNum,
                                 uint32_t bti, uint32_t sampler, bool isLD, bool isUniform) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_SAMPLE, dstNum, msgNum);
    SelectionVector *dstVector = this->appendVector();
    SelectionVector *msgVector = this->appendVector();

    // Regular instruction to encode
    for (uint32_t elemID = 0; elemID < dstNum; ++elemID)
      insn->dst(elemID) = dst[elemID];
    for (uint32_t elemID = 0; elemID < msgNum; ++elemID)
      insn->src(elemID) = msgPayloads[elemID];

    // Sends require contiguous allocation
    dstVector->regNum = dstNum;
    dstVector->isSrc = 0;
    dstVector->offsetID = 0;
    dstVector->reg = &insn->dst(0);

    // Only the messages require contiguous registers.
    msgVector->regNum = msgNum;
    msgVector->isSrc = 1;
    msgVector->offsetID = 0;
    msgVector->reg = &insn->src(0);

    insn->setbti(bti);
    insn->extra.sampler = sampler;
    insn->extra.rdmsglen = msgNum;
    insn->extra.isLD = isLD;
    insn->extra.isUniform = isUniform;
  }

  void Selection::Opaque::VME(uint32_t bti, GenRegister *dst, GenRegister *payloadVal,
                              uint32_t dstNum, uint32_t srcNum, uint32_t msg_type,
                              uint32_t vme_search_path_lut, uint32_t lut_sub) {
    SelectionInstruction *insn = this->appendInsn(SEL_OP_VME, dstNum, srcNum);
    SelectionVector *dstVector = this->appendVector();
    SelectionVector *msgVector = this->appendVector();

    for (uint32_t elemID = 0; elemID < dstNum; ++elemID)
      insn->dst(elemID) = dst[elemID];
    for (uint32_t elemID = 0; elemID < srcNum; ++elemID)
      insn->src(elemID) = payloadVal[elemID];

    dstVector->regNum = dstNum;
    dstVector->isSrc = 0;
    dstVector->offsetID = 0;
    dstVector->reg = &insn->dst(0);

    msgVector->regNum = srcNum;
    msgVector->isSrc = 1;
    msgVector->offsetID = 0;
    msgVector->reg = &insn->src(0);

    insn->setbti(bti);
    insn->extra.msg_type = msg_type;
    insn->extra.vme_search_path_lut = vme_search_path_lut;
    insn->extra.lut_sub = lut_sub;
  }

  ///////////////////////////////////////////////////////////////////////////
  // Code selection public implementation
  ///////////////////////////////////////////////////////////////////////////
  const GenContext& Selection::getCtx()
  {
    return this->opaque->ctx;
  }

  Selection::Selection(GenContext &ctx) {
    this->blockList = NULL;
    this->opaque = GBE_NEW(Selection::Opaque, ctx);
    this->opaque->setSlowByteGather(true);
    opt_features = 0;
  }

  Selection75::Selection75(GenContext &ctx) : Selection(ctx) {
    this->opaque->setSlowByteGather(false);
    opt_features = 0;
  }

  Selection8::Selection8(GenContext &ctx) : Selection(ctx) {
    this->opaque->setHas32X32Mul(true);
    this->opaque->setHasLongType(true);
    this->opaque->setHasDoubleType(true);
    this->opaque->setSlowByteGather(true);
    this->opaque->setHasHalfType(true);
    opt_features = SIOF_LOGICAL_SRCMOD;
  }

  SelectionChv::SelectionChv(GenContext &ctx) : Selection(ctx) {
    this->opaque->setHas32X32Mul(true);
    this->opaque->setHasLongType(true);
    this->opaque->setHasDoubleType(true);
    this->opaque->setLongRegRestrict(true);
    this->opaque->setSlowByteGather(true);
    this->opaque->setHasHalfType(true);
    opt_features = SIOF_LOGICAL_SRCMOD | SIOF_OP_MOV_LONG_REG_RESTRICT;
  }

  Selection9::Selection9(GenContext &ctx) : Selection(ctx) {
    this->opaque->setHas32X32Mul(true);
    this->opaque->setHasLongType(true);
    this->opaque->setHasDoubleType(true);
    this->opaque->setLdMsgOrder(LD_MSG_ORDER_SKL);
    this->opaque->setSlowByteGather(true);
    this->opaque->setHasHalfType(true);
    opt_features = SIOF_LOGICAL_SRCMOD;
  }

  SelectionBxt::SelectionBxt(GenContext &ctx) : Selection(ctx) {
    this->opaque->setHas32X32Mul(true);
    this->opaque->setHasLongType(true);
    this->opaque->setLongRegRestrict(true);
    this->opaque->setHasDoubleType(true);
    this->opaque->setLdMsgOrder(LD_MSG_ORDER_SKL);
    this->opaque->setSlowByteGather(true);
    this->opaque->setHasHalfType(true);
    opt_features = SIOF_LOGICAL_SRCMOD | SIOF_OP_MOV_LONG_REG_RESTRICT;
  }

  void Selection::Opaque::TYPED_WRITE(GenRegister *msgs, uint32_t msgNum,
                                      uint32_t bti, bool is3D) {
    uint32_t elemID = 0;
    uint32_t i;
    SelectionInstruction *insn = this->appendInsn(SEL_OP_TYPED_WRITE, 0, msgNum);
    SelectionVector *msgVector = this->appendVector();;

    for( i = 0; i < msgNum; ++i, ++elemID)
      insn->src(elemID) = msgs[i];

    insn->setbti(bti);
    insn->extra.msglen = msgNum;
    insn->extra.is3DWrite = is3D;
    // Sends require contiguous allocation
    msgVector->regNum = msgNum;
    msgVector->isSrc = 1;
    msgVector->offsetID = 0;
    msgVector->reg = &insn->src(0);
  }

  Selection::~Selection(void) { GBE_DELETE(this->opaque); }

  void Selection::select(void) {
    this->opaque->select();
    this->blockList = &this->opaque->blockList;
  }

  uint32_t Selection::getLargestBlockSize(void) const {
    return this->opaque->getLargestBlockSize();
  }

  uint32_t Selection::getVectorNum(void) const {
    return this->opaque->getVectorNum();
  }

  uint32_t Selection::getRegNum(void) const {
    return this->opaque->getRegNum();
  }

  ir::RegisterFamily Selection::getRegisterFamily(ir::Register reg) const {
    return this->opaque->getRegisterFamily(reg);
  }

  ir::RegisterData Selection::getRegisterData(ir::Register reg) const {
    return this->opaque->getRegisterData(reg);
  }

  ir::Register Selection::replaceSrc(SelectionInstruction *insn, uint32_t regID, ir::Type type, bool needMov) {
    return this->opaque->replaceSrc(insn, regID, type, needMov);
  }

  ir::Register Selection::replaceDst(SelectionInstruction *insn, uint32_t regID, ir::Type type, bool needMov) {
    return this->opaque->replaceDst(insn, regID, type, needMov);
  }
  bool Selection::spillRegs(const SpilledRegs &spilledRegs, uint32_t registerPool) {
    return this->opaque->spillRegs(spilledRegs, registerPool);
  }

  bool Selection::isScalarReg(const ir::Register &reg) const {
    return this->opaque->isScalarReg(reg);
  }

  bool Selection::isPartialWrite(const ir::Register &reg) const {
    return this->opaque->isPartialWrite(reg);
  }

  SelectionInstruction *Selection::create(SelectionOpcode opcode, uint32_t dstNum, uint32_t srcNum) {
    return this->opaque->create(opcode, dstNum, srcNum);
  }

  ///////////////////////////////////////////////////////////////////////////
  // Implementation of all patterns
  ///////////////////////////////////////////////////////////////////////////

  bool canGetRegisterFromImmediate(const ir::Instruction &insn) {
    using namespace ir;
    const auto &childInsn = cast<LoadImmInstruction>(insn);
    const auto &imm = childInsn.getImmediate();
    if(imm.getType() != TYPE_DOUBLE && imm.getType() != TYPE_S64 && imm.getType() != TYPE_U64)
      return true;
    return false;
  }

  GenRegister getRegisterFromImmediate(ir::Immediate imm, ir::Type type, bool negate = false)
  {
    using namespace ir;
    int sign = negate ? -1 : 1;
    switch (type) {
      case TYPE_U32:   return GenRegister::immud(imm.getIntegerValue() * sign);
      case TYPE_S32:   return GenRegister::immd(imm.getIntegerValue() * sign);
      case TYPE_FLOAT: return GenRegister::immf(imm.getFloatValue() * sign);
      case TYPE_U16: return GenRegister::immuw(imm.getIntegerValue() * sign);
      case TYPE_S16: return  GenRegister::immw((int16_t)imm.getIntegerValue() * sign);
      case TYPE_U8:  return GenRegister::immuw(imm.getIntegerValue() * sign);
      case TYPE_S8:  return GenRegister::immw((int8_t)imm.getIntegerValue() * sign);
      case TYPE_DOUBLE: return GenRegister::immdf(imm.getDoubleValue() * sign);
      case TYPE_BOOL: return GenRegister::immw((imm.getIntegerValue() == 0) ? 0 : -1);  //return 0xffff when true
      case TYPE_HALF: {
        ir::half hf = imm.getHalfValue();
        int16_t _sign = negate ? -1 : 1;
        ir::half hfSign = ir::half::convToHalf(_sign);
        hf = hf * hfSign;
        return GenRegister::immh(hf.getVal());
      }
      default: NOT_SUPPORTED; return GenRegister::immuw(0);
    }
  }

  BVAR(OCL_OPTIMIZE_IMMEDIATE, true);
  void Selection::Opaque::getSrcGenRegImm(SelectionDAG &dag,
                                          SelectionDAG *dag0, SelectionDAG *dag1,
                                          GenRegister &src0, GenRegister &src1,
                                          ir::Type type, bool &inverse) {
    using namespace ir;
    inverse = false;
    // Right source can always be an immediate
    const int src0Index = dag.insn.isMemberOf<SelectInstruction>() ? SelectInstruction::src0Index : 0;
    const int src1Index = dag.insn.isMemberOf<SelectInstruction>() ? SelectInstruction::src1Index : 1;
    if (OCL_OPTIMIZE_IMMEDIATE && dag1 != NULL && dag1->insn.getOpcode() == OP_LOADI &&
        canGetRegisterFromImmediate(dag1->insn)) {
      const auto &childInsn = cast<LoadImmInstruction>(dag1->insn);
      src0 = this->selReg(dag.insn.getSrc(src0Index), type);
      src1 = getRegisterFromImmediate(childInsn.getImmediate(), type);
      if (dag0) dag0->isRoot = 1;
    }
    // Left source cannot be immediate but it is OK if we can commute
    else if (OCL_OPTIMIZE_IMMEDIATE && dag0 != NULL && dag.insn.isMemberOf<BinaryInstruction>() &&
             ((cast<BinaryInstruction>(dag.insn)).commutes() || dag.insn.getOpcode() == OP_SUB) &&
             dag0->insn.getOpcode() == OP_LOADI && canGetRegisterFromImmediate(dag0->insn)) {
      const auto &childInsn = cast<LoadImmInstruction>(dag0->insn);
      src0 = dag.insn.getOpcode() != OP_SUB ?
             this->selReg(dag.insn.getSrc(src1Index), type) :
             GenRegister::negate(this->selReg(dag.insn.getSrc(src1Index), type));
      Immediate imm = childInsn.getImmediate();
      src1 = getRegisterFromImmediate(imm, type, dag.insn.getOpcode() == OP_SUB);
      if (dag1) dag1->isRoot = 1;
    }
    // If it's a compare instruction, theoritically, we can easily revert the condition code to
    // switch the two operands. But we can't do that for float due to the NaN's exist.
    // For a normal select instruction, we can always inverse the predication to switch the two
    // operands' position.
    else if (OCL_OPTIMIZE_IMMEDIATE && dag0 != NULL &&
             dag0->insn.getOpcode() == OP_LOADI && canGetRegisterFromImmediate(dag0->insn) &&
             ((dag.insn.isMemberOf<CompareInstruction>() && type != TYPE_FLOAT && type != TYPE_DOUBLE) ||
              (dag.insn.isMemberOf<SelectInstruction>()))) {
      const auto &childInsn = cast<LoadImmInstruction>(dag0->insn);
      src0 = this->selReg(dag.insn.getSrc(src1Index), type);
      src1 = getRegisterFromImmediate(childInsn.getImmediate(), type);
      inverse = true;
      if (dag1) dag1->isRoot = 1;
    }
    // Just grab the two sources
    else {
      src0 = this->selReg(dag.insn.getSrc(src0Index), type);
      src1 = this->selReg(dag.insn.getSrc(src1Index), type);
      markAllChildren(dag);
    }
  }

  void Selection::Opaque::getSrcGenRegImm(SelectionDAG &dag, GenRegister &src0,
                                       GenRegister &src1, ir::Type type,
                                       bool &inverse) {
    SelectionDAG *dag0 = dag.child[0];
    SelectionDAG *dag1 = dag.child[1];
    getSrcGenRegImm(dag, dag0, dag1, src0, src1, type, inverse);
  }


  /*! Template for the one-to-many instruction patterns */
  template <typename T, typename U>
  class OneToManyPattern : public SelectionPattern
  {
  public:
    /*! Register the pattern for all opcodes of the family */
    OneToManyPattern(uint32_t insnNum, uint32_t cost) :
      SelectionPattern(insnNum, cost)
    {
      for (uint32_t op = 0; op < ir::OP_INVALID; ++op)
        if (ir::isOpcodeFrom<U>(ir::Opcode(op)) == true)
          this->opcodes.push_back(ir::Opcode(op));
    }
    /*! Call the child method with the proper prototype */
    virtual bool emit(Selection::Opaque &sel, SelectionDAG &dag) const {
      bool markChildren = true;
      if (static_cast<const T*>(this)->emitOne(sel, ir::cast<U>(dag.insn), markChildren)) {
        if (markChildren)
          markAllChildren(dag);
        return true;
      }
      return false;
    }
  };

/*! Declare a naive one-to-many pattern */
#define DECL_PATTERN(FAMILY) \
  struct FAMILY##Pattern : public OneToManyPattern<FAMILY##Pattern, ir::FAMILY>

#define DECL_CTOR(FAMILY, INSN_NUM, COST) \
  FAMILY##Pattern(void) : OneToManyPattern<FAMILY##Pattern, ir::FAMILY>(INSN_NUM, COST) {}

  /*! Nullary instruction patterns */
  class NullaryInstructionPattern : public SelectionPattern
  {
  public:
    NullaryInstructionPattern(void) : SelectionPattern(1,1) {
      for (uint32_t op = 0; op < ir::OP_INVALID; ++op)
        if (ir::isOpcodeFrom<ir::NullaryInstruction>(ir::Opcode(op)) == true)
          this->opcodes.push_back(ir::Opcode(op));
    }

    INLINE bool emit(Selection::Opaque &sel, SelectionDAG &dag) const {
      using namespace ir;
      const ir::NullaryInstruction &insn = cast<NullaryInstruction>(dag.insn);
      const Opcode opcode = insn.getOpcode();
      const Type type = insn.getType();
      GenRegister dst = sel.selReg(insn.getDst(0), type);

      sel.push();
      if (sel.isScalarReg(insn.getDst(0))) {
        sel.curr.execWidth = 1;
        sel.curr.predicate = GEN_PREDICATE_NONE;
        sel.curr.noMask = 1;
      }

      switch (opcode) {
        case ir::OP_SIMD_SIZE:
          {
            const GenRegister src = GenRegister::immud(sel.ctx.getSimdWidth());
            sel.MOV(dst, src);
          }
          break;
        case ir::OP_SIMD_ID:
          {
            GenRegister laneID = sel.getLaneIDReg();
            sel.MOV(dst, laneID);
          }
          break;
        default: NOT_SUPPORTED;
      }
      sel.pop();
      return true;
    }
  };

  /*! Unary instruction patterns */
  DECL_PATTERN(UnaryInstruction)
  {
    static ir::Type getType(const ir::Opcode opcode, const ir::Type insnType, bool isSrc = false) {
      if (opcode == ir::OP_CBIT)
        return isSrc ? insnType : ir::TYPE_U32;
      if (insnType == ir::TYPE_S64 || insnType == ir::TYPE_U64 || insnType == ir::TYPE_S8 || insnType == ir::TYPE_U8)
        return insnType;
      if (opcode == ir::OP_FBH || opcode == ir::OP_FBL || opcode == ir::OP_LZD)
        return ir::TYPE_U32;
      if (opcode == ir::OP_SIMD_ANY || opcode == ir::OP_SIMD_ALL)
        return ir::TYPE_S32;
      if (insnType == ir::TYPE_S16 || insnType == ir::TYPE_U16)
        return insnType;
      if (insnType == ir::TYPE_BOOL)
        return ir::TYPE_U16;
      return ir::TYPE_FLOAT;
    }

    INLINE bool emitOne(Selection::Opaque &sel, const ir::UnaryInstruction &insn, bool &markChildren) const {
      const ir::Opcode opcode = insn.getOpcode();
      const ir::Type insnType = insn.getType();
      const GenRegister dst = sel.selReg(insn.getDst(0), getType(opcode, insnType, false));
      const GenRegister src = sel.selReg(insn.getSrc(0), getType(opcode, insnType, true));
      sel.push();
        if (sel.isScalarReg(insn.getDst(0)) == true) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }
        switch (opcode) {
          case ir::OP_ABS:
            {
              const GenRegister src_ = GenRegister::retype(src, getGenType(insnType));
              const GenRegister dst_ = GenRegister::retype(dst, getGenType(insnType));
              sel.MOV(dst_, GenRegister::abs(src_));
            }
            break;
          case ir::OP_MOV:
            {
              sel.push();
                auto dag = sel.regDAG[insn.getDst(0)];
                if (sel.getRegisterFamily(insn.getDst(0)) == ir::FAMILY_BOOL &&
                    dag->isUsed) {
                sel.curr.physicalFlag = 0;
                sel.curr.flagIndex = insn.getDst(0).value();
                sel.curr.modFlag = 1;
              }
              sel.MOV(dst, src);
              sel.pop();
            }
            break;
          case ir::OP_RNDD: sel.RNDD(dst, src); break;
          case ir::OP_RNDE: sel.RNDE(dst, src); break;
          case ir::OP_RNDU: sel.RNDU(dst, src); break;
          case ir::OP_RNDZ: sel.RNDZ(dst, src); break;
          case ir::OP_FBH: sel.FBH(dst, src); break;
          case ir::OP_FBL: sel.FBL(dst, src); break;
          case ir::OP_CBIT: sel.CBIT(dst, src); break;
          case ir::OP_LZD: sel.LZD(dst, src); break;
          case ir::OP_COS: sel.MATH(dst, GEN_MATH_FUNCTION_COS, src); break;
          case ir::OP_SIN: sel.MATH(dst, GEN_MATH_FUNCTION_SIN, src); break;
          case ir::OP_LOG: sel.MATH(dst, GEN_MATH_FUNCTION_LOG, src); break;
          case ir::OP_EXP: sel.MATH(dst, GEN_MATH_FUNCTION_EXP, src); break;
          case ir::OP_SQR: sel.MATH(dst, GEN_MATH_FUNCTION_SQRT, src); break;
          case ir::OP_RSQ: sel.MATH(dst, GEN_MATH_FUNCTION_RSQ, src); break;
          case ir::OP_RCP: sel.MATH(dst, GEN_MATH_FUNCTION_INV, src); break;
          case ir::OP_BSWAP:
            {
              ir::Register tmp = sel.reg(getFamily(insnType));
              const GenRegister src_ = GenRegister::retype(src, getGenType(insnType));
              const GenRegister dst_ = GenRegister::retype(dst, getGenType(insnType));
              sel.BSWAP(dst_, src_, sel.selReg(tmp, insnType));
              break;
            }
          case ir::OP_SIMD_ANY:
            {
              const GenRegister constZero = GenRegister::immuw(0);;
              const GenRegister regOne = GenRegister::uw1grf(ir::ocl::one);
              const GenRegister flag01 = GenRegister::flag(0, 1);

              sel.push();
                int simdWidth = sel.curr.execWidth;
                sel.curr.predicate = GEN_PREDICATE_NONE;
                sel.curr.execWidth = 1;
                sel.curr.noMask = 1;
                sel.MOV(flag01, constZero);
                sel.curr.execWidth = simdWidth;
                sel.curr.noMask = 0;

                sel.curr.flag = 0;
                sel.curr.subFlag = 1;
                sel.CMP(GEN_CONDITIONAL_NEQ, src, constZero);

                if (sel.curr.execWidth == 16)
                  sel.curr.predicate = GEN_PREDICATE_ALIGN1_ANY16H;
                else if (sel.curr.execWidth == 8)
                  sel.curr.predicate = GEN_PREDICATE_ALIGN1_ANY8H;
                else
                  NOT_IMPLEMENTED;
                sel.SEL(dst, regOne, constZero);
              sel.pop();
            }
            break;
          case ir::OP_SIMD_ALL:
            {
              const GenRegister constZero = GenRegister::immuw(0);
              const GenRegister regOne = GenRegister::uw1grf(ir::ocl::one);
              const GenRegister flag01 = GenRegister::flag(0, 1);

              sel.push();
                int simdWidth = sel.curr.execWidth;
                sel.curr.predicate = GEN_PREDICATE_NONE;
                sel.curr.execWidth = 1;
                sel.curr.noMask = 1;
                sel.MOV(flag01, regOne);

                sel.curr.execWidth = simdWidth;
                sel.curr.noMask = 0;

                sel.curr.flag = 0;
                sel.curr.subFlag = 1;
                sel.CMP(GEN_CONDITIONAL_NEQ, src, constZero);

                if (sel.curr.execWidth == 16)
                  sel.curr.predicate = GEN_PREDICATE_ALIGN1_ALL16H;
                else if (sel.curr.execWidth == 8)
                  sel.curr.predicate = GEN_PREDICATE_ALIGN1_ALL8H;
                else
                  NOT_IMPLEMENTED;
                sel.SEL(dst, regOne, constZero);
              sel.pop();
            }
            break;

          default: NOT_SUPPORTED;
        }
      sel.pop();
      return true;
    }
    DECL_CTOR(UnaryInstruction, 1, 1)
  };


  /*! Binary regular instruction pattern */
  class BinaryInstructionPattern : public SelectionPattern
  {
  public:
    BinaryInstructionPattern(void) : SelectionPattern(1,1) {
      for (uint32_t op = 0; op < ir::OP_INVALID; ++op)
        if (ir::isOpcodeFrom<ir::BinaryInstruction>(ir::Opcode(op)) == true)
          this->opcodes.push_back(ir::Opcode(op));
    }

    bool emitDivRemInst(Selection::Opaque &sel, SelectionDAG &dag, ir::Opcode op) const
    {
      using namespace ir;
      const ir::BinaryInstruction &insn = cast<BinaryInstruction>(dag.insn);
      const Type type = insn.getType();
      GenRegister dst  = sel.selReg(insn.getDst(0), type);
      GenRegister src0 = sel.selReg(insn.getSrc(0), type);
      GenRegister src1 = sel.selReg(insn.getSrc(1), type);
      const uint32_t simdWidth = sel.curr.execWidth;
      const bool isUniform = simdWidth == 1;
      const RegisterFamily family = getFamily(type);
      uint32_t function = (op == OP_DIV)?
                          GEN_MATH_FUNCTION_INT_DIV_QUOTIENT :
                          GEN_MATH_FUNCTION_INT_DIV_REMAINDER;

      //bytes and shorts must be converted to int for DIV and REM per GEN restriction
      if((family == FAMILY_WORD || family == FAMILY_BYTE) && (type != TYPE_HALF)) {
        GenRegister tmp0, tmp1;
        ir::Register reg = sel.reg(FAMILY_DWORD, isUniform);
        tmp0 = sel.selReg(reg, ir::TYPE_S32);
        sel.MOV(tmp0, src0);
        tmp1 = sel.selReg(sel.reg(FAMILY_DWORD, isUniform), ir::TYPE_S32);
        sel.MOV(tmp1, src1);
        sel.MATH(tmp0, function, tmp0, tmp1);
        GenRegister unpacked;
        if(family == FAMILY_WORD) {
          unpacked = sel.unpacked_uw(reg);
        } else {
          unpacked = sel.unpacked_ub(reg);
        }
        unpacked = GenRegister::retype(unpacked, getGenType(type));
        sel.MOV(dst, unpacked);
      } else if (type == TYPE_HALF) {
        ir::Register reg = sel.reg(FAMILY_DWORD, isUniform);
        GenRegister tmp0 = sel.selReg(sel.reg(FAMILY_DWORD, isUniform), ir::TYPE_FLOAT);
        GenRegister tmp1 = sel.selReg(reg, ir::TYPE_FLOAT);
        sel.MOV(tmp0, src0);
        sel.MOV(tmp1, src1);
        GBE_ASSERT(op != OP_REM);
        sel.MATH(tmp0, GEN_MATH_FUNCTION_FDIV, tmp0, tmp1);
        GenRegister unpacked = GenRegister::retype(sel.unpacked_uw(reg), GEN_TYPE_HF);
        sel.MOV(unpacked, tmp0);
        sel.MOV(dst, unpacked);
      } else if (type == TYPE_S32 || type == TYPE_U32 ) {
        sel.MATH(dst, function, src0, src1);
      } else if(type == TYPE_FLOAT) {
        GBE_ASSERT(op != OP_REM);
        sel.MATH(dst, GEN_MATH_FUNCTION_FDIV, src0, src1);
      } else if (type == TYPE_S64 || type == TYPE_U64) {
        GenRegister tmp[15];
        int tmp_num = 13;
        for(int i=0; i < 13; i++) {
          tmp[i] = sel.selReg(sel.reg(FAMILY_DWORD));
          tmp[i].type = GEN_TYPE_UD;
        }

        if (sel.hasLongType()) {
          if (!sel.isScalarReg(insn.getSrc(0))) {
            tmp[tmp_num] = GenRegister::retype(sel.selReg(sel.reg(FAMILY_QWORD)), src0.type);
            tmp_num++;
          }

          if (!sel.isScalarReg(insn.getSrc(1))) {
            tmp[tmp_num] = GenRegister::retype(sel.selReg(sel.reg(FAMILY_QWORD)), src1.type);
            tmp_num++;
          }

          /* We at least one tmp register to convert if dst is not scalar. */
          if (!sel.isScalarReg(insn.getDst(0)) && sel.isScalarReg(insn.getSrc(0))
              && sel.isScalarReg(insn.getSrc(1))) {
            GBE_ASSERT(tmp_num == 13);
            tmp[tmp_num] = sel.selReg(sel.reg(FAMILY_QWORD), ir::TYPE_U64);
            tmp_num++;
          }
        }
        sel.push();
          sel.curr.flag = 0;
          sel.curr.subFlag = 1;
          if(op == OP_DIV)
            sel.I64DIV(dst, src0, src1, tmp, tmp_num);
          else
            sel.I64REM(dst, src0, src1, tmp, tmp_num);
        sel.pop();
      } else if (type == TYPE_DOUBLE) {
        if (!sel.hasDoubleType())
          GBE_ASSERT(0);

        GenRegister tmp[10];
        int tmpNum = 7;
        ir::RegisterFamily fm;
        if (sel.ctx.getSimdWidth() == 16) {
          fm = FAMILY_WORD;
        } else {
          fm = FAMILY_DWORD;
        }

        /* madm and invm need special accumutor support, which require us in align16
           mode. If any src is uniform, we need another tmp register and MOV the
           uniform one to it. Because the madm and invm will work in align16 mode,
           the channel mask is different from the align1 mode. So we can not directly
           write the result to the dst and need a tmp register to hold the result and
           MOV it to dst later. */
        tmpNum++; //For the dst.
        if (src0.hstride == GEN_HORIZONTAL_STRIDE_0) tmpNum++;
        if (src1.hstride == GEN_HORIZONTAL_STRIDE_0) tmpNum++;

        for (int i = 0; i < tmpNum; i++)
          tmp[i] = GenRegister::df8grf(sel.reg(fm));

        sel.push();
          sel.curr.flag = 0;
          sel.curr.subFlag = 1;
          sel.F64DIV(dst, src0, src1, tmp, tmpNum);
        sel.pop();
      } else {
        GBE_ASSERT(0);
      }
      markAllChildren(dag);
      return true;
    }

    INLINE bool emit(Selection::Opaque &sel, SelectionDAG &dag) const
    {
      using namespace ir;
      const ir::BinaryInstruction &insn = cast<BinaryInstruction>(dag.insn);
      const Opcode opcode = insn.getOpcode();
      const Type type = insn.getType();
      GenRegister dst  = sel.selReg(insn.getDst(0), type);

      sel.push();

      // Boolean values use scalars
      if (sel.isScalarReg(insn.getDst(0)) == true) {
        sel.curr.execWidth = 1;
        sel.curr.predicate = GEN_PREDICATE_NONE;
        sel.curr.noMask = 1;
      }

      if(opcode == OP_DIV || opcode == OP_REM) {
        bool ret = this->emitDivRemInst(sel, dag, opcode);
        sel.pop();
        return ret;
      }
      // Immediates not supported
      if (opcode == OP_POW) {
        GenRegister src0 = sel.selReg(insn.getSrc(0), type);
        GenRegister src1 = sel.selReg(insn.getSrc(1), type);

        if(type == TYPE_FLOAT) {
          sel.MATH(dst, GEN_MATH_FUNCTION_POW, src0, src1);
        } else {
          NOT_IMPLEMENTED;
        }
        markAllChildren(dag);
        sel.pop();
        return true;
      }

      // Look for immediate values
      GenRegister src0, src1;
      bool inverse = false;
      sel.getSrcGenRegImm(dag, src0, src1, type, inverse);
      // Output the binary instruction
      if (sel.getRegisterFamily(insn.getDst(0)) == ir::FAMILY_BOOL &&
          dag.isUsed) {
        GBE_ASSERT(insn.getOpcode() == OP_AND ||
                   insn.getOpcode() == OP_OR ||
                   insn.getOpcode() == OP_XOR);
        sel.curr.physicalFlag = 0;
        sel.curr.flagIndex = insn.getDst(0).value();
        sel.curr.modFlag = 1;
      }

      switch (opcode) {
        case OP_ADD:
          if ((type == Type::TYPE_U64 || type == Type::TYPE_S64) && !sel.hasLongType()) {
            GenRegister t = sel.selReg(sel.reg(RegisterFamily::FAMILY_QWORD), Type::TYPE_S64);
            sel.I64ADD(dst, src0, src1, t);
          } else
            sel.ADD(dst, src0, src1);
          break;
        case OP_ADDSAT:
          if ((type == Type::TYPE_U64 || type == Type::TYPE_S64) && !sel.hasLongType()) {
            GenRegister tmp[5];
            for(int i=0; i<5; i++) {
              tmp[i] = sel.selReg(sel.reg(FAMILY_DWORD));
              tmp[i].type = GEN_TYPE_UD;
            }
            sel.push();
              sel.curr.flag = 0;
              sel.curr.subFlag = 1;
              sel.I64SATADD(dst, src0, src1, tmp);
            sel.pop();
            break;
          }
          sel.push();
            sel.curr.saturate = GEN_MATH_SATURATE_SATURATE;
            sel.ADD(dst, src0, src1);
          sel.pop();
          break;
        case OP_XOR:
          if ((type == Type::TYPE_U64 || type == Type::TYPE_S64) && !sel.hasLongType())
            sel.I64XOR(dst, src0, src1);
          else
            sel.XOR(dst, src0, src1);
          break;
        case OP_OR:
          if ((type == Type::TYPE_U64 || type == Type::TYPE_S64) && !sel.hasLongType())
            sel.I64OR(dst, src0, src1);
          else
            sel.OR(dst, src0, src1);
          break;
        case OP_AND:
          if ((type == Type::TYPE_U64 || type == Type::TYPE_S64) && !sel.hasLongType())
            sel.I64AND(dst, src0, src1);
          else
            sel.AND(dst, src0, src1);
          break;
        case OP_SUB:
          if ((type == Type::TYPE_U64 || type == Type::TYPE_S64) && !sel.hasLongType()) {
            GenRegister t = sel.selReg(sel.reg(RegisterFamily::FAMILY_QWORD), Type::TYPE_S64);
            sel.I64SUB(dst, src0, src1, t);
          } else
            sel.ADD(dst, src0, GenRegister::negate(src1));
          break;
        case OP_SUBSAT:
          if ((type == Type::TYPE_U64 || type == Type::TYPE_S64) && !sel.hasLongType()) {
            GenRegister tmp[5];
            for(int i=0; i<5; i++) {
              tmp[i] = sel.selReg(sel.reg(FAMILY_DWORD));
              tmp[i].type = GEN_TYPE_UD;
            }
            sel.push();
              sel.curr.flag = 0;
              sel.curr.subFlag = 1;
              sel.I64SATSUB(dst, src0, src1, tmp);
            sel.pop();
            break;
          }
          sel.push();
            sel.curr.saturate = GEN_MATH_SATURATE_SATURATE;
            sel.ADD(dst, src0, GenRegister::negate(src1));
          sel.pop();
          break;
        case OP_SHL:
          if ((type == Type::TYPE_U64 || type == Type::TYPE_S64) && !sel.hasLongType()) {
            GenRegister tmp[6];
            for(int i = 0; i < 6; i ++)
              tmp[i] = sel.selReg(sel.reg(FAMILY_DWORD));
            sel.push();
              sel.curr.flag = 0;
              sel.curr.subFlag = 1;
              sel.I64SHL(dst, src0, src1, tmp);
            sel.pop();
          } else
            sel.SHL(dst, src0, src1);
          break;
        case OP_SHR:
          if ((type == Type::TYPE_U64 || type == Type::TYPE_S64) && !sel.hasLongType()) {
            GenRegister tmp[6];
            for(int i = 0; i < 6; i ++)
              tmp[i] = sel.selReg(sel.reg(FAMILY_DWORD));
            sel.push();
              sel.curr.flag = 0;
              sel.curr.subFlag = 1;
              sel.I64SHR(dst, src0, src1, tmp);
            sel.pop();
          } else
            sel.SHR(dst, src0, src1);
          break;
        case OP_ASR:
          if ((type == Type::TYPE_U64 || type == Type::TYPE_S64) && !sel.hasLongType()) {
            GenRegister tmp[6];
            for(int i = 0; i < 6; i ++)
              tmp[i] = sel.selReg(sel.reg(FAMILY_DWORD));
            sel.push();
              sel.curr.flag = 0;
              sel.curr.subFlag = 1;
              sel.I64ASR(dst, src0, src1, tmp);
            sel.pop();
          } else
            sel.ASR(dst, src0, src1);
          break;
        case OP_MUL_HI: {
            GenRegister temp = GenRegister::retype(sel.selReg(sel.reg(FAMILY_DWORD)), GEN_TYPE_UD);
            sel.MUL_HI(dst, src0, src1, temp);
            break;
          }
        case OP_I64_MUL_HI:
         {
           int tmp_num;
           GenRegister temp[9];
           if (sel.hasLongType()) {
             for(int i=0; i<9; i++) {
               temp[i] = sel.selReg(sel.reg(FAMILY_QWORD), ir::TYPE_U64);
             }
             tmp_num = 6;
           } else {
             for(int i=0; i<9; i++) {
               temp[i] = sel.selReg(sel.reg(FAMILY_DWORD));
               temp[i].type = GEN_TYPE_UD;
             }
             tmp_num = 9;
           }
           sel.push();
           sel.curr.flag = 0;
           sel.curr.subFlag = 1;
           sel.I64_MUL_HI(dst, src0, src1, temp, tmp_num);
           sel.pop();
           break;
         }
        case OP_MUL:
          if (type == TYPE_U32 || type == TYPE_S32) {
            sel.pop();
            return false;
          } else if (type == TYPE_S64 || type == TYPE_U64) {
            if (sel.hasLongType()) {
              GenRegister tmp;
              tmp = sel.selReg(sel.reg(FAMILY_QWORD), ir::TYPE_U64);
              sel.I64MUL(dst, src0, src1, &tmp, true);
            } else {
              GenRegister tmp[6];
              for(int i = 0; i < 6; i++)
                tmp[i] = sel.selReg(sel.reg(FAMILY_DWORD));
              sel.I64MUL(dst, src0, src1, tmp, false);
            }
          } else
            sel.MUL(dst, src0, src1);
          break;
        case OP_HADD: {
            GenRegister temp = GenRegister::retype(sel.selReg(sel.reg(FAMILY_DWORD)), GEN_TYPE_D);
            sel.HADD(dst, src0, src1, temp);
            break;
          }
        case OP_RHADD: {
            GenRegister temp = GenRegister::retype(sel.selReg(sel.reg(FAMILY_DWORD)), GEN_TYPE_D);
            sel.RHADD(dst, src0, src1, temp);
            break;
          }
        case OP_I64HADD:
          {
            GenRegister tmp[4];
            if (!sel.hasLongType()) {
              for(int i=0; i<4; i++)
                tmp[i] = sel.selReg(sel.reg(FAMILY_DWORD));
              sel.I64HADD(dst, src0, src1, tmp, 4);
            } else {
              tmp[0] = sel.selReg(sel.reg(FAMILY_QWORD), ir::TYPE_U64);
              tmp[1] = sel.selReg(sel.reg(FAMILY_QWORD), ir::TYPE_U64);
              sel.I64HADD(dst, src0, src1, tmp, 2);
            }
            break;
          }
        case OP_I64RHADD:
          {
            GenRegister tmp[4];
            if (!sel.hasLongType()) {
              for(int i=0; i<4; i++)
                tmp[i] = sel.selReg(sel.reg(FAMILY_DWORD));
              sel.I64RHADD(dst, src0, src1, tmp, 4);
            } else {
              tmp[0] = sel.selReg(sel.reg(FAMILY_QWORD), ir::TYPE_U64);
              tmp[1] = sel.selReg(sel.reg(FAMILY_QWORD), ir::TYPE_U64);
              sel.I64RHADD(dst, src0, src1, tmp, 2);
            }
            break;
          }
        case OP_UPSAMPLE_SHORT:
        {
          dst = GenRegister::retype(sel.unpacked_uw(dst.reg()), GEN_TYPE_B);
          src0 = GenRegister::retype(sel.unpacked_uw(src0.reg()), GEN_TYPE_B);
          src1 = GenRegister::retype(sel.unpacked_uw(src1.reg()), GEN_TYPE_B);
          sel.MOV(dst, src1);
          dst = sel.getOffsetReg(dst, 0, typeSize(GEN_TYPE_B));
          sel.MOV(dst, src0);
          break;
        }
        case OP_UPSAMPLE_INT:
        {
          dst = sel.unpacked_uw(dst.reg());
          src0 = sel.unpacked_uw(src0.reg());
          src1 = sel.unpacked_uw(src1.reg());
          sel.MOV(dst, src1);
          dst = sel.getOffsetReg(dst, 0, typeSize(GEN_TYPE_W));
          sel.MOV(dst, src0);
          break;
        }
        case OP_UPSAMPLE_LONG:
          sel.UPSAMPLE_LONG(dst, src0, src1);
          break;
        default: NOT_IMPLEMENTED;
      }
      sel.pop();
      return true;
    }
  };

  /*! MAD pattern */
  class MulAddInstructionPattern : public SelectionPattern
  {
  public:
    /*! Register the pattern for all opcodes of the family */
    MulAddInstructionPattern(void) : SelectionPattern(2, 1) {
       this->opcodes.push_back(ir::OP_ADD);
       this->opcodes.push_back(ir::OP_SUB);
    }

    /*! Implements base class */
    virtual bool emit(Selection::Opaque  &sel, SelectionDAG &dag) const
    {
      using namespace ir;

      // XXX TODO: we need a clean support of FP_CONTRACT to remove below line 'return false'
      // if 'pragma FP_CONTRACT OFF' is used in cl kernel, we should not do mad optimization.
      if (!sel.ctx.relaxMath || sel.ctx.getSimdWidth() == 16)
        return false;
      // MAD tend to increase liveness of the sources (since there are three of
      // them). TODO refine this strategy. Well, we should be able at least to
      // evaluate per basic block register pressure and selectively enable
      // disable MADs
       if (sel.ctx.limitRegisterPressure)
        return false;

      // We are good to try. We need a MUL for one of the two sources
      const ir::BinaryInstruction &insn = cast<ir::BinaryInstruction>(dag.insn);
      if (insn.getType() != TYPE_FLOAT)
        return false;
      SelectionDAG *child0 = dag.child[0];
      SelectionDAG *child1 = dag.child[1];
      const GenRegister dst = sel.selReg(insn.getDst(0), TYPE_FLOAT);
      if (child0 && child0->insn.getOpcode() == OP_MUL) {
        GBE_ASSERT(cast<ir::BinaryInstruction>(child0->insn).getType() == TYPE_FLOAT);
        SelectionDAG *child00 = child0->child[0];
        SelectionDAG *child01 = child0->child[1];
        if ((child00 && child00->insn.getOpcode() == OP_LOADI) ||
            (child01 && child01->insn.getOpcode() == OP_LOADI) ||
            (child1 && child1->insn.getOpcode() == OP_LOADI))
          return false;
        const GenRegister src0 = sel.selReg(child0->insn.getSrc(0), TYPE_FLOAT);
        const GenRegister src1 = sel.selReg(child0->insn.getSrc(1), TYPE_FLOAT);
        GenRegister src2 = sel.selReg(insn.getSrc(1), TYPE_FLOAT);
        if(insn.getOpcode() == ir::OP_SUB) src2 = GenRegister::negate(src2);
        sel.MAD(dst, src2, src0, src1); // order different on HW!
        if (child0->child[0]) child0->child[0]->isRoot = 1;
        if (child0->child[1]) child0->child[1]->isRoot = 1;
        if (child1) child1->isRoot = 1;
        return true;
      }
      if (child1 && child1->insn.getOpcode() == OP_MUL) {
        GBE_ASSERT(cast<ir::BinaryInstruction>(child1->insn).getType() == TYPE_FLOAT);
        SelectionDAG *child10 = child1->child[0];
        SelectionDAG *child11 = child1->child[1];
        if ((child10 && child10->insn.getOpcode() == OP_LOADI) ||
            (child11 && child11->insn.getOpcode() == OP_LOADI) ||
            (child0 && child0->insn.getOpcode() == OP_LOADI))
          return false;
        GenRegister src0 = sel.selReg(child1->insn.getSrc(0), TYPE_FLOAT);
        const GenRegister src1 = sel.selReg(child1->insn.getSrc(1), TYPE_FLOAT);
        const GenRegister src2 = sel.selReg(insn.getSrc(0), TYPE_FLOAT);
        if(insn.getOpcode() == ir::OP_SUB) src0 = GenRegister::negate(src0);
        sel.MAD(dst, src2, src0, src1); // order different on HW!
        if (child1->child[0]) child1->child[0]->isRoot = 1;
        if (child1->child[1]) child1->child[1]->isRoot = 1;
        if (child0) child0->isRoot = 1;
        return true;
      }
      return false;
    }
  };

  /*! sel.{le,l,ge...} like patterns */
  class SelectModifierInstructionPattern : public SelectionPattern
  {
  public:
    /*! Register the pattern for all opcodes of the family */
    SelectModifierInstructionPattern(void) : SelectionPattern(2, 1) {
      this->opcodes.push_back(ir::OP_SEL);
    }

    /*! Implements base class */
    virtual bool emit(Selection::Opaque &sel, SelectionDAG &dag) const
    {
      using namespace ir;
      SelectionDAG *cmp = dag.child[0];
      const SelectInstruction &insn = cast<SelectInstruction>(dag.insn);

      if (insn.getType() == TYPE_S64 || insn.getType() == TYPE_U64) // not support
        return false;

      // Not in this block
      if (cmp == NULL) return false;

      // We need to match a compare
      if (cmp->insn.isMemberOf<CompareInstruction>() == false) return false;

      // We look for something like that:
      // cmp.{le,ge...} flag src0 src1
      // sel dst flag src0 src1
      // So both sources must match
      if (sourceMatch(cmp, 0, &dag, 1) == false) return false;
      if (sourceMatch(cmp, 1, &dag, 2) == false) return false;
      // OK, we merge the instructions
      const ir::CompareInstruction &cmpInsn = cast<CompareInstruction>(cmp->insn);
      const ir::Opcode opcode = cmpInsn.getOpcode();
      if(opcode == OP_ORD) return false;
      GenRegister src0, src1;
      const ir::Type type = cmpInsn.getType();
      bool inverse = false;
      sel.getSrcGenRegImm(*cmp, src0, src1, type, inverse);

      const uint32_t genCmp = getGenCompare(opcode, inverse);
      sel.push();
        if (sel.isScalarReg(insn.getDst(0)) == true) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }

        // Like for regular selects, we need a temporary since we cannot predicate
        // properly
        const uint32_t simdWidth = sel.curr.execWidth;
        const GenRegister dst  = sel.selReg(insn.getDst(0), type);
        sel.curr.predicate = GEN_PREDICATE_NONE;
        sel.curr.execWidth = simdWidth;
        sel.SEL_CMP(genCmp, dst, src0, src1);
      sel.pop();
      return true;
    }
  };

  /*! 32 bits integer multiply needs more instructions */
  class Int32x32MulInstructionPattern : public SelectionPattern
  {
  public:
    /*! Register the pattern for all opcodes of the family */
    Int32x32MulInstructionPattern(void) : SelectionPattern(1, 4) {
       this->opcodes.push_back(ir::OP_MUL);
    }

    /*! Implements base class */
    virtual bool emit(Selection::Opaque &sel, SelectionDAG &dag) const
    {
      using namespace ir;
      const ir::BinaryInstruction &insn = cast<ir::BinaryInstruction>(dag.insn);
      const Type type = insn.getType();
      if (type != TYPE_U32 && type != TYPE_S32)
        return false;

      GenRegister dst  = sel.selReg(insn.getDst(0), type);
      GenRegister src0 = sel.selReg(insn.getSrc(0), type);
      GenRegister src1 = sel.selReg(insn.getSrc(1), type);

      sel.push();
      if (sel.has32X32Mul()) {
        if (sel.isScalarReg(insn.getDst(0)) == true) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }
        sel.MUL(dst, src0, src1);
      } else {
        if (sel.isScalarReg(insn.getDst(0)) == true) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }

        const int simdWidth = sel.curr.execWidth;

        // Either left part of the 16-wide register or just a simd 8 register
        dst  = GenRegister::retype(dst,  GEN_TYPE_D);
        src0 = GenRegister::retype(src0, GEN_TYPE_D);
        src1 = GenRegister::retype(src1, GEN_TYPE_D);
        sel.curr.execWidth = 8;
        sel.curr.quarterControl = GEN_COMPRESSION_Q1;
        sel.MUL(GenRegister::retype(GenRegister::acc(), GEN_TYPE_D), src0, src1);
        sel.curr.accWrEnable = 1;
        sel.MACH(GenRegister::retype(GenRegister::null(), GEN_TYPE_D), src0, src1);
        sel.curr.accWrEnable = 0;
        if (simdWidth == 1) {
          sel.curr.execWidth = 1;
          sel.MOV(GenRegister::retype(dst, GEN_TYPE_F), GenRegister::vec1(GenRegister::acc()));
        } else {
          sel.curr.execWidth = 8;
          sel.MOV(GenRegister::retype(dst, GEN_TYPE_F), GenRegister::acc());
        }

        // Right part of the 16-wide register now
        if (simdWidth == 16) {
          int predicate = sel.curr.predicate;
          int noMask = sel.curr.noMask;
          sel.curr.noMask = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          const GenRegister nextSrc0 = sel.selRegQn(insn.getSrc(0), 1, TYPE_S32);
          const GenRegister nextSrc1 = sel.selRegQn(insn.getSrc(1), 1, TYPE_S32);
          sel.MUL(GenRegister::retype(GenRegister::acc(), GEN_TYPE_D), nextSrc0, nextSrc1);
          sel.curr.accWrEnable = 1;
          sel.MACH(GenRegister::retype(GenRegister::null(), GEN_TYPE_D), nextSrc0, nextSrc1);
          sel.curr.accWrEnable = 0;
          sel.curr.quarterControl = GEN_COMPRESSION_Q2;
          if (predicate != GEN_PREDICATE_NONE || noMask != 1) {
            const ir::Register reg = sel.reg(FAMILY_DWORD);
            sel.MOV(GenRegister::f8grf(reg), GenRegister::acc());
            sel.curr.noMask = noMask;;
            sel.curr.predicate = predicate;
            sel.MOV(GenRegister::retype(GenRegister::next(dst), GEN_TYPE_F),
                    GenRegister::f8grf(reg));
          } else
            sel.MOV(GenRegister::retype(GenRegister::next(dst), GEN_TYPE_F), GenRegister::acc());
        }
      }
      sel.pop();

      // All children are marked as root
      markAllChildren(dag);
      return true;
    }
  };

  /*! 32x16 bits integer can be done in one instruction */
  class Int32x16MulInstructionPattern : public SelectionPattern
  {
  public:
    /*! Register the pattern for all opcodes of the family */
    Int32x16MulInstructionPattern(void) : SelectionPattern(1, 1) {
       this->opcodes.push_back(ir::OP_MUL);
    }

    bool is16BitSpecialReg(ir::Register reg) const {
      if (reg == ir::ocl::lid0 ||
          reg == ir::ocl::lid1 ||
          reg == ir::ocl::lid2 ||
          reg == ir::ocl::lsize0 ||
          reg == ir::ocl::lsize1||
          reg == ir::ocl::lsize2)
        return true;
      else
        return false;
    }

    /*! Try to emit a multiply where child childID is a 16 immediate */
    bool emitMulImmediate(Selection::Opaque  &sel, SelectionDAG &dag, uint32_t childID) const {
      using namespace ir;
      const ir::BinaryInstruction &insn = cast<ir::BinaryInstruction>(dag.insn);
      const Register dst  = insn.getDst(0);
      const Register src1 = insn.getSrc(childID ^ 1);
      const SelectionDAG *src0DAG = dag.child[childID];
      if (src0DAG != NULL) {
        if (src0DAG->insn.getOpcode() == OP_LOADI) {
          const auto &loadimm = cast<LoadImmInstruction>(src0DAG->insn);
          const Immediate imm = loadimm.getImmediate();
          const Type type = imm.getType();
          GBE_ASSERT(type == TYPE_U32 || type == TYPE_S32);
          if (type == TYPE_U32 && imm.getIntegerValue() <= 0xffff) {
            sel.push();
              if (sel.isScalarReg(insn.getDst(0)) == true) {
                sel.curr.execWidth = 1;
                sel.curr.predicate = GEN_PREDICATE_NONE;
                sel.curr.noMask = 1;
              }

              sel.MUL(sel.selReg(dst, type),
                      sel.selReg(src1, type),
                      GenRegister::immuw(imm.getIntegerValue()));
            sel.pop();
            if (dag.child[childID ^ 1] != NULL)
              dag.child[childID ^ 1]->isRoot = 1;
            return true;
          }
          if (type == TYPE_S32 && (imm.getIntegerValue() >= -32768 && imm.getIntegerValue() <= 32767)) {
            sel.push();
              if (sel.isScalarReg(insn.getDst(0)) == true) {
                sel.curr.execWidth = 1;
                sel.curr.predicate = GEN_PREDICATE_NONE;
                sel.curr.noMask = 1;
              }

              sel.MUL(sel.selReg(dst, type),
                      sel.selReg(src1, type),
                      GenRegister::immw(imm.getIntegerValue()));
            sel.pop();
            if (dag.child[childID ^ 1] != NULL)
              dag.child[childID ^ 1]->isRoot = 1;
            return true;
          }
        }
      }
      return false;
    }

    /*! Try to emit a multiply with a 16 bit special register */
    bool emitMulSpecialReg(Selection::Opaque &sel, SelectionDAG &dag, uint32_t childID) const {
      using namespace ir;
      const BinaryInstruction &insn = cast<ir::BinaryInstruction>(dag.insn);
      const Type type = insn.getType();
      const Register dst  = insn.getDst(0);
      const Register src0 = insn.getSrc(childID);
      const Register src1 = insn.getSrc(childID ^ 1);
      if (is16BitSpecialReg(src0)) {
        sel.push();
          if (sel.isScalarReg(insn.getDst(0)) == true) {
            sel.curr.execWidth = 1;
            sel.curr.predicate = GEN_PREDICATE_NONE;
            sel.curr.noMask = 1;
          }
          sel.MUL(sel.selReg(dst, type),
                  sel.selReg(src1, type),
                  sel.selReg(src0, TYPE_U32));
        sel.pop();
        markAllChildren(dag);
        return true;
      }
      return false;
    }

    virtual bool emit(Selection::Opaque &sel, SelectionDAG &dag) const
    {
      using namespace ir;
      const BinaryInstruction &insn = cast<ir::BinaryInstruction>(dag.insn);
      const Type type = insn.getType();
      if (type == TYPE_U32 || type == TYPE_S32) {
        if (this->emitMulSpecialReg(sel, dag, 0))
          return true;
        if (this->emitMulSpecialReg(sel, dag, 1))
          return true;
        if (this->emitMulImmediate(sel, dag, 0))
          return true;
        if (this->emitMulImmediate(sel, dag, 1))
          return true;
      }
      return false;
    }
  };

#define DECL_NOT_IMPLEMENTED_ONE_TO_MANY(FAMILY) \
  struct FAMILY##Pattern : public OneToManyPattern<FAMILY##Pattern, ir::FAMILY>\
  {\
    INLINE bool emitOne(Selection::Opaque &sel, const ir::FAMILY &insn, bool &markChildren) const {\
      NOT_IMPLEMENTED;\
      return false;\
    }\
    DECL_CTOR(FAMILY, 1, 1); \
  }
#undef DECL_NOT_IMPLEMENTED_ONE_TO_MANY

  /*! Load immediate pattern */
  DECL_PATTERN(LoadImmInstruction)
  {
    INLINE bool emitOne(Selection::Opaque &sel, const ir::LoadImmInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const Type type = insn.getType();
      const Immediate imm = insn.getImmediate();
      const GenRegister dst = sel.selReg(insn.getDst(0), type);

      sel.push();
      if (sel.isScalarReg(insn.getDst(0)) == true) {
        sel.curr.execWidth = 1;
        sel.curr.predicate = GEN_PREDICATE_NONE;
        sel.curr.noMask = 1;
      }

      switch (type) {
        case TYPE_BOOL:
          if (!sel.isScalarReg(insn.getDst(0)) && sel.regDAG[insn.getDst(0)]->isUsed) {
            sel.curr.modFlag = 1;
            sel.curr.physicalFlag = 0;
            sel.curr.flagIndex = insn.getDst(0).value();
          }
          sel.MOV(dst, imm.getIntegerValue() ? GenRegister::immuw(0xffff) : GenRegister::immuw(0));
        break;
        case TYPE_U32: sel.MOV(dst, GenRegister::immud(imm.getIntegerValue())); break;
        case TYPE_S32: sel.MOV(dst, GenRegister::immd(imm.getIntegerValue())); break;
        case TYPE_FLOAT:
          sel.MOV(GenRegister::retype(dst, GEN_TYPE_F),
                  GenRegister::immf(imm.asFloatValue()));
        break;
        case TYPE_HALF: {
          ir::half hf = imm.getHalfValue();
          sel.MOV(GenRegister::retype(dst, GEN_TYPE_HF), GenRegister::immh(hf.getVal()));
          break;
        }
        case TYPE_U16: sel.MOV(dst, GenRegister::immuw(imm.getIntegerValue())); break;
        case TYPE_S16: sel.MOV(dst, GenRegister::immw(imm.getIntegerValue())); break;
        case TYPE_U8:  sel.MOV(dst, GenRegister::immuw(imm.getIntegerValue())); break;
        case TYPE_S8:  sel.MOV(dst, GenRegister::immw(imm.getIntegerValue())); break;
        case TYPE_DOUBLE: sel.MOV(dst, GenRegister::immdf(imm.getDoubleValue())); break;
        case TYPE_S64: sel.LOAD_INT64_IMM(dst, GenRegister::immint64(imm.getIntegerValue())); break;
        case TYPE_U64: sel.LOAD_INT64_IMM(dst, GenRegister::immuint64(imm.getIntegerValue())); break;
        default: NOT_SUPPORTED;
      }
      sel.pop();
      return true;
    }

    DECL_CTOR(LoadImmInstruction, 1,1);
  };

  /*! Sync instruction */
  DECL_PATTERN(SyncInstruction)
  {
    INLINE bool emitOne(Selection::Opaque &sel, const ir::SyncInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const ir::Register reg = sel.reg(FAMILY_DWORD);
      const uint32_t params = insn.getParameters();

      // A barrier is OK to start the thread synchronization *and* SLM fence
      sel.BARRIER(GenRegister::ud8grf(reg), sel.selReg(sel.reg(FAMILY_DWORD)), params);
      return true;
    }

    DECL_CTOR(SyncInstruction, 1,1);
  };

  /*! Wait instruction */
  DECL_PATTERN(WaitInstruction)
  {
    INLINE bool emitOne(Selection::Opaque &sel, const ir::WaitInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      // Debugwait will use reg 1, which is different from barrier
      sel.push();
        sel.curr.noMask = 1;
        sel.curr.execWidth = 1;
        sel.curr.predicate = GEN_PREDICATE_NONE;
        sel.WAIT(1);
      sel.pop();
      return true;
    }

    DECL_CTOR(WaitInstruction, 1,1);
  };

  INLINE uint32_t getByteScatterGatherSize(Selection::Opaque &sel, ir::Type type) {
    using namespace ir;
    switch (type) {
      case TYPE_DOUBLE:
      case TYPE_S64:
      case TYPE_U64:
        return GEN_BYTE_SCATTER_QWORD;
      case TYPE_FLOAT:
      case TYPE_U32:
      case TYPE_S32:
        return GEN_BYTE_SCATTER_DWORD;
      case TYPE_BOOL:
      case TYPE_U16:
      case TYPE_S16:
        return GEN_BYTE_SCATTER_WORD;
      case TYPE_U8:
      case TYPE_S8:
        return GEN_BYTE_SCATTER_BYTE;
      case TYPE_HALF:
        if (sel.hasHalfType())
          return GEN_BYTE_SCATTER_WORD;
      default: NOT_SUPPORTED;
        return GEN_BYTE_SCATTER_BYTE;
    }
  }

  class LoadInstructionPattern : public SelectionPattern
  {
  public:
    /*! Register the pattern for all opcodes of the family */
    LoadInstructionPattern(void) : SelectionPattern(1, 1) {
       this->opcodes.push_back(ir::OP_LOAD);
    }
    void readDWord(Selection::Opaque &sel,
                   vector<GenRegister> &dst,
                   GenRegister addr,
                   uint32_t valueNum,
                   ir::BTI bti) const
    {
        GenRegister b = bti.isConst ? GenRegister::immud(bti.imm) : sel.selReg(bti.reg, ir::TYPE_U32);
        sel.UNTYPED_READ(addr, dst.data(), valueNum, b, sel.getBTITemps(bti));
    }

    void emitUntypedRead(Selection::Opaque &sel,
                         const ir::LoadInstruction &insn,
                         GenRegister addr,
                         ir::BTI bti) const
    {
      using namespace ir;
      const uint32_t valueNum = insn.getValueNum();
      vector<GenRegister> dst(valueNum);
      for (uint32_t dstID = 0; dstID < valueNum; ++dstID)
        dst[dstID] = sel.selReg(insn.getValue(dstID), TYPE_U32);
      readDWord(sel, dst, addr, valueNum, bti);
    }

    void emitDWordGather(Selection::Opaque &sel,
                         const ir::LoadInstruction &insn,
                         GenRegister addr,
                         ir::BTI bti) const
    {
      using namespace ir;
      GBE_ASSERT(bti.isConst == 1);
      GBE_ASSERT(insn.getValueNum() == 1);
      const uint32_t isUniform = sel.isScalarReg(insn.getValue(0));

      if(isUniform) {
        GenRegister dst = sel.selReg(insn.getValue(0), ir::TYPE_U32);
        sel.push();
          sel.curr.noMask = 1;
          sel.SAMPLE(&dst, 1, &addr, 1, bti.imm, 0, true, true);
        sel.pop();
        return;
      }

      GenRegister dst = GenRegister::retype(sel.selReg(insn.getValue(0)), GEN_TYPE_F);
      // get dword based address
      GenRegister addrDW = sel.selReg(sel.reg(FAMILY_DWORD, isUniform), ir::TYPE_U32);

      sel.push();
        if (sel.isScalarReg(addr.reg())) {
          sel.curr.noMask = 1;
        }
        sel.SHR(addrDW, GenRegister::retype(addr, GEN_TYPE_UD), GenRegister::immud(2));
      sel.pop();

      sel.DWORD_GATHER(dst, addrDW, bti.imm);
    }

    void emitRead64(Selection::Opaque &sel,
                         const ir::LoadInstruction &insn,
                         GenRegister addr,
                         ir::BTI bti) const
    {
      using namespace ir;
      const uint32_t valueNum = insn.getValueNum();
      /* XXX support scalar only right now. */
      GBE_ASSERT(valueNum == 1);
      GBE_ASSERT(bti.isConst == 1);
      vector<GenRegister> dst(valueNum);
      GenRegister b = bti.isConst ? GenRegister::immud(bti.imm) : sel.selReg(bti.reg, ir::TYPE_U32);
      for ( uint32_t dstID = 0; dstID < valueNum; ++dstID)
        dst[dstID] = sel.selReg(insn.getValue(dstID), ir::TYPE_U64);

      if (sel.hasLongType()) {
        vector<GenRegister> tmp(valueNum);
        for (uint32_t valueID = 0; valueID < valueNum; ++valueID) {
          tmp[valueID] = GenRegister::retype(sel.selReg(sel.reg(ir::FAMILY_QWORD), ir::TYPE_U64), GEN_TYPE_UL);
        }

        sel.READ64(addr, dst.data(), tmp.data(), valueNum, b, true, sel.getBTITemps(bti));
      } else {
        sel.READ64(addr, dst.data(), NULL, valueNum, b, false, sel.getBTITemps(bti));
      }
    }

    void readByteAsDWord(Selection::Opaque &sel,
                        const uint32_t elemSize,
                        GenRegister address,
                        GenRegister dst,
                        bool isUniform,
                        ir::BTI bti) const
    {
      using namespace ir;
        Register tmpReg = sel.reg(FAMILY_DWORD, isUniform);
        GenRegister tmpAddr = sel.selReg(sel.reg(FAMILY_DWORD, isUniform), ir::TYPE_U32);
        GenRegister tmpData = sel.selReg(tmpReg, ir::TYPE_U32);

        GenRegister b = bti.isConst ? GenRegister::immud(bti.imm) : sel.selReg(bti.reg, ir::TYPE_U32);

        // Get dword aligned addr
        sel.push();
          if (isUniform) {
            sel.curr.noMask = 1;
            sel.curr.execWidth = 1;
          }
          sel.AND(tmpAddr, GenRegister::retype(address,GEN_TYPE_UD), GenRegister::immud(0xfffffffc));
        sel.pop();
        sel.push();
          if (isUniform)
            sel.curr.noMask = 1;
          sel.UNTYPED_READ(tmpAddr, &tmpData, 1, b, sel.getBTITemps(bti));

          if (isUniform)
            sel.curr.execWidth = 1;
          // Get the remaining offset from aligned addr
          sel.AND(tmpAddr, GenRegister::retype(address,GEN_TYPE_UD), GenRegister::immud(0x3));
          sel.SHL(tmpAddr, tmpAddr, GenRegister::immud(0x3));
          sel.SHR(tmpData, tmpData, tmpAddr);

          if (elemSize == GEN_BYTE_SCATTER_WORD)
            sel.MOV(GenRegister::retype(dst, GEN_TYPE_UW), sel.unpacked_uw(tmpReg));
          else if (elemSize == GEN_BYTE_SCATTER_BYTE)
            sel.MOV(GenRegister::retype(dst, GEN_TYPE_UB), sel.unpacked_ub(tmpReg));
        sel.pop();
    }

    // The address is dw aligned.
    void emitAlignedByteGather(Selection::Opaque &sel,
                               const ir::LoadInstruction &insn,
                               const uint32_t elemSize,
                               GenRegister address,
                               ir::BTI bti) const
    {
      using namespace ir;
      const uint32_t valueNum = insn.getValueNum();
      const bool isUniform = sel.isScalarReg(insn.getValue(0));
      RegisterFamily family = getFamily(insn.getValueType());

      vector<GenRegister> dst(valueNum);
      const uint32_t typeSize = getFamilySize(family);

      for(uint32_t i = 0; i < valueNum; i++)
        dst[i] = sel.selReg(insn.getValue(i), getType(family));

      uint32_t tmpRegNum = (typeSize*valueNum + 3) / 4;
      vector<GenRegister> tmp(tmpRegNum);
      for(uint32_t i = 0; i < tmpRegNum; i++) {
        tmp[i] = sel.selReg(sel.reg(FAMILY_DWORD, isUniform), ir::TYPE_U32);
      }

      readDWord(sel, tmp, address, tmpRegNum, bti);

      for(uint32_t i = 0; i < tmpRegNum; i++) {
        unsigned int elemNum = (valueNum - i * (4 / typeSize)) > 4/typeSize ?
                               4/typeSize : (valueNum - i * (4 / typeSize));
        sel.UNPACK_BYTE(dst.data() + i * 4/typeSize, tmp[i], typeSize, elemNum);
      }
    }

    // Gather effect data to the effectData vector from the tmp vector.
    //  x x d0 d1 | d2 d3 d4 d5 | ... ==> d0 d1 d2 d3 | d4 d5 ...
    void getEffectByteData(Selection::Opaque &sel,
                           vector<GenRegister> &effectData,
                           vector<GenRegister> &tmp,
                           uint32_t effectDataNum,
                           const GenRegister &address,
                           bool isUniform) const
    {
      using namespace ir;
      GBE_ASSERT(effectData.size() == effectDataNum);
      GBE_ASSERT(tmp.size() == effectDataNum + 1);
      sel.push();
        Register alignedFlag = sel.reg(FAMILY_BOOL, isUniform);
        GenRegister shiftL = sel.selReg(sel.reg(FAMILY_DWORD, isUniform), ir::TYPE_U32);
        Register shiftHReg = sel.reg(FAMILY_DWORD, isUniform);
        GenRegister shiftH = sel.selReg(shiftHReg, ir::TYPE_U32);
        sel.push();
          if (isUniform)
            sel.curr.noMask = 1;
          sel.AND(shiftL, GenRegister::retype(address, GEN_TYPE_UD), GenRegister::immud(0x3));
          sel.SHL(shiftL, shiftL, GenRegister::immud(0x3));
          sel.ADD(shiftH, GenRegister::negate(shiftL), GenRegister::immud(32));
          sel.curr.physicalFlag = 0;
          sel.curr.modFlag = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.flagIndex = alignedFlag.value();
          sel.CMP(GEN_CONDITIONAL_NEQ, GenRegister::unpacked_uw(shiftHReg), GenRegister::immuw(32));
        sel.pop();

        sel.curr.noMask = 1;
        for(uint32_t i = 0; i < effectDataNum; i++) {
          GenRegister tmpH = sel.selReg(sel.reg(FAMILY_DWORD, isUniform), ir::TYPE_U32);
          GenRegister tmpL = effectData[i];
          sel.SHR(tmpL, tmp[i], shiftL);
          sel.push();
            // Only need to consider the tmpH when the addr is not aligned.
            sel.curr.modFlag = 0;
            sel.curr.physicalFlag = 0;
            sel.curr.flagIndex = alignedFlag.value();
            sel.curr.predicate = GEN_PREDICATE_NORMAL;
            sel.SHL(tmpH, tmp[i + 1], shiftH);
            sel.OR(effectData[i], tmpL, tmpH);
          sel.pop();
        }
      sel.pop();
    }

    void emitUnalignedByteGather(Selection::Opaque &sel,
                                 const ir::LoadInstruction &insn,
                                 const uint32_t elemSize,
                                 GenRegister address,
                                 ir::BTI bti) const
    {
      using namespace ir;
      const uint32_t valueNum = insn.getValueNum();
      const uint32_t simdWidth = sel.isScalarReg(insn.getValue(0)) ?
                                 1 : sel.ctx.getSimdWidth();
      const bool isUniform = simdWidth == 1;
      RegisterFamily family = getFamily(insn.getValueType());

      if(valueNum > 1) {
        GBE_ASSERT(!isUniform && "vector load should not be uniform. Something went wrong.");
        vector<GenRegister> dst(valueNum);
        const uint32_t typeSize = getFamilySize(family);

        for(uint32_t i = 0; i < valueNum; i++)
          dst[i] = sel.selReg(insn.getValue(i), getType(family));

        uint32_t effectDataNum = (typeSize*valueNum + 3) / 4;
        vector<GenRegister> tmp(effectDataNum + 1);
        vector<GenRegister> tmp2(effectDataNum + 1);
        vector<GenRegister> effectData(effectDataNum);
        for(uint32_t i = 0; i < effectDataNum + 1; i++)
          tmp2[i] = tmp[i] = sel.selReg(sel.reg(FAMILY_DWORD, isUniform), ir::TYPE_U32);

        GenRegister alignedAddr = sel.selReg(sel.reg(FAMILY_DWORD, isUniform), ir::TYPE_U32);
        sel.push();
          if (isUniform)
            sel.curr.noMask = 1;
          sel.AND(alignedAddr, GenRegister::retype(address, GEN_TYPE_UD), GenRegister::immud(~0x3));
        sel.pop();

        uint32_t remainedReg = effectDataNum + 1;
        uint32_t pos = 0;
        do {
          uint32_t width = remainedReg > 4 ? 4 : remainedReg;
          vector<GenRegister> t1(tmp.begin() + pos, tmp.begin() + pos + width);
          vector<GenRegister> t2(tmp2.begin() + pos, tmp2.begin() + pos + width);
          if (pos != 0) {
            sel.push();
              if (isUniform)
                sel.curr.noMask = 1;
              sel.ADD(alignedAddr, alignedAddr, GenRegister::immud(pos * 4));
            sel.pop();
          }
          readDWord(sel, t1, alignedAddr, width, bti);
          remainedReg -= width;
          pos += width;
        } while(remainedReg);

        for(uint32_t i = 0; i < effectDataNum; i++)
          effectData[i] = sel.selReg(sel.reg(FAMILY_DWORD, isUniform), ir::TYPE_U32);

        getEffectByteData(sel, effectData, tmp, effectDataNum, address, isUniform);

        for(uint32_t i = 0; i < effectDataNum; i++) {
          unsigned int elemNum = (valueNum - i * (4 / typeSize)) > 4/typeSize ?
                                 4/typeSize : (valueNum - i * (4 / typeSize));
          sel.UNPACK_BYTE(dst.data() + i * 4/typeSize, effectData[i], typeSize, elemNum);
        }
      } else {
        GBE_ASSERT(insn.getValueNum() == 1);
        const GenRegister value = sel.selReg(insn.getValue(0), insn.getValueType());
        GBE_ASSERT(elemSize == GEN_BYTE_SCATTER_WORD || elemSize == GEN_BYTE_SCATTER_BYTE);
        if(sel.getSlowByteGather())
          readByteAsDWord(sel, elemSize, address, value, isUniform, bti);
        else {
          GenRegister b = bti.isConst ? GenRegister::immud(bti.imm) : sel.selReg(bti.reg, ir::TYPE_U32);

          // We need a temporary register if we read bytes or words
          Register dst = sel.reg(FAMILY_DWORD, isUniform);
          sel.push();
            if (isUniform)
              sel.curr.noMask = 1;
            sel.BYTE_GATHER(sel.selReg(dst, ir::TYPE_U32), address, elemSize, b, sel.getBTITemps(bti));
          sel.pop();

          sel.push();
            if (isUniform) {
              sel.curr.noMask = 1;
              sel.curr.execWidth = 1;
            }
            if (elemSize == GEN_BYTE_SCATTER_WORD)
              sel.MOV(GenRegister::retype(value, GEN_TYPE_UW), GenRegister::unpacked_uw(dst, isUniform));
            else if (elemSize == GEN_BYTE_SCATTER_BYTE)
              sel.MOV(GenRegister::retype(value, GEN_TYPE_UB), GenRegister::unpacked_ub(dst, isUniform));
          sel.pop();
        }
      }
    }

    // check whether all binded table index point to constant memory
    INLINE bool isAllConstant(const ir::BTI &bti) const {
      if (bti.isConst && bti.imm == BTI_CONSTANT)
        return true;
      return false;
    }

    /*! Implements base class */
    virtual bool emit(Selection::Opaque  &sel, SelectionDAG &dag) const
    {
      using namespace ir;
      const ir::LoadInstruction &insn = cast<ir::LoadInstruction>(dag.insn);
      GenRegister address = sel.selReg(insn.getAddressRegister(), ir::TYPE_U32);
      GBE_ASSERT(insn.getAddressSpace() == MEM_GLOBAL ||
                 insn.getAddressSpace() == MEM_CONSTANT ||
                 insn.getAddressSpace() == MEM_PRIVATE ||
                 insn.getAddressSpace() == MEM_LOCAL ||
                 insn.getAddressSpace() == MEM_MIXED);
      //GBE_ASSERT(sel.isScalarReg(insn.getValue(0)) == false);

      BTI bti;
      AddressMode am = insn.getAddressMode();
      if (am == AM_StaticBti) {
        bti.isConst = 1;
        bti.imm = insn.getSurfaceIndex();
      } else if (am == AM_DynamicBti) {
        bti.isConst = 0;
        bti.reg = insn.getBtiReg();
      } else {
        assert(0 && "stateless not supported yet");
      }
      const Type type = insn.getValueType();
      const uint32_t elemSize = getByteScatterGatherSize(sel, type);
      bool allConstant = isAllConstant(bti);

      if (allConstant) {
        // XXX TODO read 64bit constant through constant cache
        // Per HW Spec, constant cache messages can read at least DWORD data.
        // So, byte/short data type, we have to read through data cache.
        if(insn.isAligned() == true && elemSize == GEN_BYTE_SCATTER_QWORD)
          this->emitRead64(sel, insn, address, bti);
        else if(insn.isAligned() == true && elemSize == GEN_BYTE_SCATTER_DWORD)
          this->emitDWordGather(sel, insn, address, bti);
        else if (insn.isAligned() == true)
          this->emitAlignedByteGather(sel, insn, elemSize, address, bti);
        else
          this->emitUnalignedByteGather(sel, insn, elemSize, address, bti);
      } else {
        if (insn.isAligned() == true && elemSize == GEN_BYTE_SCATTER_QWORD)
          this->emitRead64(sel, insn, address, bti);
        else if (insn.isAligned() == true && elemSize == GEN_BYTE_SCATTER_DWORD)
          this->emitUntypedRead(sel, insn, address, bti);
        else if (insn.isAligned())
          this->emitAlignedByteGather(sel, insn, elemSize, address, bti);
        else
          this->emitUnalignedByteGather(sel, insn, elemSize, address, bti);
      }

      markAllChildren(dag);
      return true;
    }
  };

  class StoreInstructionPattern : public SelectionPattern
  {
  public:
    /*! Register the pattern for all opcodes of the family */
    StoreInstructionPattern(void) : SelectionPattern(1, 1) {
       this->opcodes.push_back(ir::OP_STORE);
    }
    void emitUntypedWrite(Selection::Opaque &sel,
                          const ir::StoreInstruction &insn,
                          GenRegister address,
                          ir::BTI &bti) const
    {
      using namespace ir;
      const uint32_t valueNum = insn.getValueNum();
      vector<GenRegister> value(valueNum), tmps;
      GenRegister b = bti.isConst ? GenRegister::immud(bti.imm) : sel.selReg(bti.reg, ir::TYPE_U32);

      for (uint32_t valueID = 0; valueID < valueNum; ++valueID)
        value[valueID] = GenRegister::retype(sel.selReg(insn.getValue(valueID)), GEN_TYPE_UD);
      sel.UNTYPED_WRITE(address, value.data(), valueNum, b, sel.getBTITemps(bti));
    }

    void emitWrite64(Selection::Opaque &sel,
                     const ir::StoreInstruction &insn,
                     GenRegister address,
                     ir::BTI &bti) const
    {
      using namespace ir;
      const uint32_t valueNum = insn.getValueNum();
      /* XXX support scalar only right now. */
      GBE_ASSERT(valueNum == 1);
      GenRegister b = bti.isConst ? GenRegister::immud(bti.imm) : sel.selReg(bti.reg, ir::TYPE_U32);
      vector<GenRegister> src(valueNum);

      for (uint32_t valueID = 0; valueID < valueNum; ++valueID)
        src[valueID] = sel.selReg(insn.getValue(valueID), ir::TYPE_U64);

      if (sel.hasLongType()) {
        vector<GenRegister> tmp(valueNum);
        for (uint32_t valueID = 0; valueID < valueNum; ++valueID) {
          tmp[valueID] = GenRegister::retype(sel.selReg(sel.reg(ir::FAMILY_QWORD), ir::TYPE_U64), GEN_TYPE_UL);
        }
        sel.WRITE64(address, src.data(), tmp.data(), valueNum, b, true, sel.getBTITemps(bti));
      } else {
        sel.WRITE64(address, src.data(), NULL, valueNum, b, false, sel.getBTITemps(bti));
      }
    }

    void emitByteScatter(Selection::Opaque &sel,
                         const ir::StoreInstruction &insn,
                         const uint32_t elemSize,
                         GenRegister address,
                         ir::BTI &bti,
                         bool isUniform) const
    {
      using namespace ir;
      uint32_t valueNum = insn.getValueNum();

      GenRegister b = bti.isConst ? GenRegister::immud(bti.imm) : sel.selReg(bti.reg, ir::TYPE_U32);
      if(valueNum > 1) {
        const uint32_t typeSize = getFamilySize(getFamily(insn.getValueType()));
        vector<GenRegister> value(valueNum);

        if(elemSize == GEN_BYTE_SCATTER_WORD) {
          for(uint32_t i = 0; i < valueNum; i++)
            value[i] = sel.selReg(insn.getValue(i), ir::TYPE_U16);
        } else if(elemSize == GEN_BYTE_SCATTER_BYTE) {
          for(uint32_t i = 0; i < valueNum; i++)
            value[i] = sel.selReg(insn.getValue(i), ir::TYPE_U8);
        }

        uint32_t tmpRegNum = typeSize*valueNum / 4;
        vector<GenRegister> tmp(tmpRegNum);
        for(uint32_t i = 0; i < tmpRegNum; i++) {
          tmp[i] = sel.selReg(sel.reg(FAMILY_DWORD, isUniform), ir::TYPE_U32);
          sel.PACK_BYTE(tmp[i], value.data() + i * 4/typeSize, typeSize, 4/typeSize);
        }

        sel.UNTYPED_WRITE(address, tmp.data(), tmpRegNum, b, sel.getBTITemps(bti));
      } else {
        const GenRegister value = sel.selReg(insn.getValue(0));
        GBE_ASSERT(insn.getValueNum() == 1);
        const GenRegister tmp = sel.selReg(sel.reg(FAMILY_DWORD, isUniform), ir::TYPE_U32);

        sel.push();
          if (isUniform) {
            sel.curr.noMask = 1;
            sel.curr.execWidth = 1;
          }

          if (elemSize == GEN_BYTE_SCATTER_WORD)
            sel.MOV(tmp, GenRegister::retype(value, GEN_TYPE_UW));
          else if (elemSize == GEN_BYTE_SCATTER_BYTE)
            sel.MOV(tmp, GenRegister::retype(value, GEN_TYPE_UB));
        sel.pop();
        sel.BYTE_SCATTER(address, tmp, elemSize, b, sel.getBTITemps(bti));
      }
    }

    virtual bool emit(Selection::Opaque  &sel, SelectionDAG &dag) const
    {
      using namespace ir;
      const ir::StoreInstruction &insn = cast<ir::StoreInstruction>(dag.insn);
      GenRegister address = sel.selReg(insn.getAddressRegister(), ir::TYPE_U32);
      const Type type = insn.getValueType();
      const uint32_t elemSize = getByteScatterGatherSize(sel, type);

      const bool isUniform = sel.isScalarReg(insn.getAddressRegister()) && sel.isScalarReg(insn.getValue(0));
      BTI bti;
      AddressMode am = insn.getAddressMode();
      if (am == AM_StaticBti) {
        bti.isConst = 1;
        bti.imm = insn.getSurfaceIndex();
      } else if (am == AM_DynamicBti) {
        bti.isConst = 0;
        bti.reg = insn.getBtiReg();
      } else {
        assert(0 && "stateless not supported yet");
      }

      if (insn.isAligned() == true && elemSize == GEN_BYTE_SCATTER_QWORD)
        this->emitWrite64(sel, insn, address, bti);
      else if (insn.isAligned() == true && elemSize == GEN_BYTE_SCATTER_DWORD)
        this->emitUntypedWrite(sel, insn, address,  bti);
      else {
        this->emitByteScatter(sel, insn, elemSize, address, bti, isUniform);
      }

      markAllChildren(dag);
      return true;
    }
  };

  /*! Compare instruction pattern */
  class CompareInstructionPattern : public SelectionPattern
  {
  public:
    CompareInstructionPattern(void) : SelectionPattern(1,1) {
      for (uint32_t op = 0; op < ir::OP_INVALID; ++op)
        if (ir::isOpcodeFrom<ir::CompareInstruction>(ir::Opcode(op)) == true)
          this->opcodes.push_back(ir::Opcode(op));
    }

    INLINE bool emit(Selection::Opaque &sel, SelectionDAG &dag) const
    {
      using namespace ir;
      const ir::CompareInstruction &insn = cast<CompareInstruction>(dag.insn);
      const Opcode opcode = insn.getOpcode();
      const Type type = insn.getType();
      const Register dst = insn.getDst(0);
      GenRegister tmpDst;
      const BasicBlock *curr = insn.getParent();
      const ir::Liveness &liveness = sel.ctx.getLiveness();
      const ir::Liveness::LiveOut &liveOut = liveness.getLiveOut(curr);
      bool needStoreBool = false;
      if (liveOut.contains(dst) || dag.computeBool)
        needStoreBool = true;

      if(type == TYPE_S64 || type == TYPE_U64 ||
         type == TYPE_DOUBLE || type == TYPE_FLOAT ||
         type == TYPE_U32 ||  type == TYPE_S32 /*||
         (!needStoreBool)*/)
        tmpDst = GenRegister::retype(GenRegister::null(), GEN_TYPE_F);
      else
        tmpDst = sel.selReg(dst, TYPE_BOOL);

      // Look for immediate values for the right source
      GenRegister src0, src1;
      bool inverseCmp = false;
      sel.getSrcGenRegImm(dag, src0, src1, type, inverseCmp);
      sel.push();
        if (sel.isScalarReg(dst))
          sel.curr.noMask = 1;
        sel.curr.physicalFlag = 0;
        sel.curr.modFlag = 1;
        sel.curr.flagIndex = dst.value();
        sel.curr.grfFlag = needStoreBool; // indicate whether we need to allocate grf to store this boolean.
        if ((type == TYPE_S64 || type == TYPE_U64) && !sel.hasLongType()) {
          GenRegister tmp[3];
          for(int i=0; i<3; i++)
            tmp[i] = sel.selReg(sel.reg(FAMILY_DWORD));
          sel.curr.flagGen = 1;
          sel.I64CMP(getGenCompare(opcode, inverseCmp), src0, src1, tmp);
        } else if(opcode == OP_ORD) {
          sel.push();
            sel.CMP(GEN_CONDITIONAL_EQ, src0, src0, tmpDst);
            sel.curr.predicate = GEN_PREDICATE_NORMAL;
            sel.curr.flagGen = 1;
            sel.CMP(GEN_CONDITIONAL_EQ, src1, src1, tmpDst);
          sel.pop();
        } else {
          if((type == TYPE_S64 || type == TYPE_U64 ||
              type == TYPE_DOUBLE || type == TYPE_FLOAT ||
              type == TYPE_U32 ||  type == TYPE_S32))
            sel.curr.flagGen = 1;
          else if (sel.isScalarReg(dst)) {
            // If the dest reg is a scalar bool, we can't set it as
            // dst register, as the execution width is still 8 or 16.
            // Instead, we set the needStoreBool to flagGen, and change
            // the dst to null register. And let the flag reg allocation
            // function to generate the flag grf on demand correctly latter.
            sel.curr.flagGen = needStoreBool;
            tmpDst = GenRegister::retype(GenRegister::null(), GEN_TYPE_UW);
          }
          sel.CMP(getGenCompare(opcode, inverseCmp), src0, src1, tmpDst);
        }
      sel.pop();
      return true;
    }
  };

  /*! Bit cast instruction pattern */
  DECL_PATTERN(BitCastInstruction)
  {
    INLINE bool emitOne(Selection::Opaque &sel, const ir::BitCastInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const Type dstType = insn.getDstType();
      const Type srcType = insn.getSrcType();
      const uint32_t dstNum = insn.getDstNum();
      const uint32_t srcNum = insn.getSrcNum();
      int index = 0, multiple, narrowNum, wideNum;
      bool narrowDst;
      Type narrowType;
      bool wideScalar = false;

      if(dstNum > srcNum) {
        multiple = dstNum / srcNum;
        narrowType = dstType;
        narrowNum = dstNum;
        wideNum = srcNum;
        narrowDst = 1;
        wideScalar = sel.isScalarReg(insn.getSrc(0));
      } else {
        multiple = srcNum / dstNum;
        narrowType = srcType;
        narrowNum = srcNum;
        wideNum = dstNum;
        narrowDst = 0;
        wideScalar = sel.isScalarReg(insn.getDst(0));
      }

      sel.push();
      if (sel.isScalarReg(insn.getDst(0)) == true) {
        sel.curr.execWidth = 1;
        sel.curr.predicate = GEN_PREDICATE_NONE;
        sel.curr.noMask = 1;
      }

      // As we store long/ulong low/high part separately,
      // we need to deal with it separately, we need to change it back again
      // when hardware support native long type.
      const bool isInt64 = (srcType == TYPE_S64 || srcType == TYPE_U64 || dstType == TYPE_S64 || dstType == TYPE_U64);
      const int simdWidth = sel.curr.execWidth;

      /* because we do not have hstride = 8, here, we need to seperate
         the long into top have and bottom half. */
      vector<GenRegister> tmp(wideNum);
      if (multiple == 8 && sel.hasLongType() && !wideScalar) {
        GBE_ASSERT(isInt64); // Must relate to long and char conversion.
        if (narrowDst) {
          for (int i = 0; i < wideNum; i++) {
            tmp[i] = sel.selReg(sel.reg(FAMILY_QWORD), ir::TYPE_U64);
            sel.UNPACK_LONG(tmp[i], sel.selReg(insn.getSrc(i), srcType));
          }
        } else {
          for (int i = 0; i < wideNum; i++) {
            tmp[i] = sel.selReg(sel.reg(FAMILY_QWORD), ir::TYPE_U64);
          }
        }
      }

      for(int i = 0; i < narrowNum; i++, index++) {
        GenRegister narrowReg, wideReg;
        if (multiple == 8 && sel.hasLongType() && !wideScalar) {
          if(narrowDst) {
            narrowReg = sel.selReg(insn.getDst(i), narrowType);
            wideReg = GenRegister::retype(tmp[index/multiple], narrowType);  //retype to narrow type
          } else {
            wideReg = GenRegister::retype(tmp[index/multiple], narrowType);
            narrowReg = sel.selReg(insn.getSrc(i), narrowType);  //retype to narrow type
          }
        } else {
          if(narrowDst) {
            narrowReg = sel.selReg(insn.getDst(i), narrowType);
            wideReg = sel.selReg(insn.getSrc(index/multiple), narrowType);  //retype to narrow type
          } else {
            wideReg = sel.selReg(insn.getDst(index/multiple), narrowType);
            narrowReg = sel.selReg(insn.getSrc(i), narrowType);  //retype to narrow type
          }
        }

        // set correct horizontal stride
        if(wideReg.hstride != GEN_HORIZONTAL_STRIDE_0) {
          if(multiple == 2) {
            if (sel.hasLongType() && isInt64) {
              // long to int or int to long
              wideReg = sel.unpacked_ud(wideReg.reg());
              wideReg = GenRegister::retype(wideReg, getGenType(narrowType));
            } else {
              wideReg = sel.unpacked_uw(wideReg.reg());
              wideReg = GenRegister::retype(wideReg, getGenType(narrowType));
              if(isInt64) {
                wideReg.width = GEN_WIDTH_8;
                wideReg.hstride = GEN_HORIZONTAL_STRIDE_1;
                wideReg.vstride = GEN_VERTICAL_STRIDE_8;
              }
            }
          } else if(multiple == 4) {
            if (sel.hasLongType() && isInt64) {
              // long to short or short to long
              wideReg = sel.unpacked_uw(wideReg.reg());
              wideReg = GenRegister::retype(wideReg, getGenType(narrowType));
            } else {
              wideReg = sel.unpacked_ub(wideReg.reg());
              wideReg = GenRegister::retype(wideReg, getGenType(narrowType));
              if(isInt64) {
                wideReg.hstride = GEN_HORIZONTAL_STRIDE_2;
                wideReg.vstride = GEN_VERTICAL_STRIDE_16;
              }
            }
          } else if(multiple == 8) {
            // we currently store high/low 32bit separately in register,
            // so, its hstride is 4 here.
            wideReg = sel.unpacked_ub(wideReg.reg());
            wideReg = GenRegister::retype(wideReg, getGenType(narrowType));
          } else {
            GBE_ASSERT(0);
          }
        }

        if((!isInt64 || (sel.hasLongType() && multiple != 8)) && index % multiple) {
          wideReg = sel.getOffsetReg(wideReg, 0, (index % multiple) * typeSize(wideReg.type));
        }
        if(isInt64 && (multiple == 8 || !sel.hasLongType())) {
          // Offset to next half
          if((i % multiple) >= multiple/2)
            wideReg = sel.getOffsetReg(wideReg, 0, sel.isScalarReg(wideReg.reg()) ? 4 : simdWidth*4);
          // Offset to desired narrow element in wideReg
          if(index % (multiple/2))
            wideReg = sel.getOffsetReg(wideReg, 0, (index % (multiple/2)) * typeSize(wideReg.type));
        }

        GenRegister xdst = narrowDst ? narrowReg : wideReg;
        GenRegister xsrc = narrowDst ? wideReg : narrowReg;

        if(isInt64) {
          sel.MOV(xdst, xsrc);
        } else if(srcType == TYPE_DOUBLE || dstType == TYPE_DOUBLE) {
          sel.push();
            sel.curr.execWidth = 8;
            for(int i = 0; i < simdWidth/4; i ++) {
              sel.curr.chooseNib(i);
              sel.MOV(xdst, xsrc);
              xdst = sel.getOffsetReg(xdst, 0, 4 * typeSize(getGenType(dstType)));
              xsrc = sel.getOffsetReg(xsrc, 0, 4 * typeSize(getGenType(srcType)));
            }
          sel.pop();
        } else
          sel.MOV(xdst, xsrc);
      }

      if (multiple == 8 && sel.hasLongType() && !wideScalar && !narrowDst) {
        for (int i = 0; i < wideNum; i++) {
          sel.PACK_LONG(sel.selReg(insn.getDst(i), dstType), tmp[i]);
        }
      }

      sel.pop();

      return true;
    }
    DECL_CTOR(BitCastInstruction, 1, 1);
  };

  /*! Convert instruction pattern */
  DECL_PATTERN(ConvertInstruction)
  {

    INLINE bool lowerI64Reg(Selection::Opaque &sel, SelectionDAG *dag, GenRegister &src, uint32_t type) const {
      using namespace ir;
      GBE_ASSERT(type == GEN_TYPE_UD || type == GEN_TYPE_F);
      if (dag->insn.getOpcode() == OP_LOADI) {
        const auto &immInsn = cast<LoadImmInstruction>(dag->insn);
        const auto imm = immInsn.getImmediate();
        const Type immType = immInsn.getType();
        if (immType == TYPE_S64 &&
          imm.getIntegerValue() <= INT_MAX &&
          imm.getIntegerValue() >= INT_MIN) {
          src = GenRegister::immd((int32_t)imm.getIntegerValue());
          return true;
        } else if (immType == TYPE_U64 &&
                   imm.getIntegerValue() <= UINT_MAX) {
          src = GenRegister::immud((uint32_t)imm.getIntegerValue());
          return true;
        }
      } else if (dag->insn.getOpcode() == OP_CVT) {
        const auto cvtInsn = cast<ConvertInstruction>(dag->insn);
        auto srcType = cvtInsn.getSrcType();
        if (((srcType == TYPE_U32 || srcType == TYPE_S32) &&
            (type == GEN_TYPE_UD || type == GEN_TYPE_D)) ||
             ((srcType == TYPE_FLOAT) && type == GEN_TYPE_F)) {
          src = GenRegister::retype(sel.selReg(cvtInsn.getSrc(0), srcType), type);
          dag->isRoot = 1;
          return true;
        } else if (srcType == TYPE_FLOAT ||
                   srcType == TYPE_U16 ||
                   srcType == TYPE_S16 ||
                   srcType == TYPE_U32 ||
                   srcType == TYPE_S32) {
          src = GenRegister::retype(sel.selReg(sel.reg(FAMILY_DWORD), TYPE_U32), type);
          dag->isRoot = 1;
          sel.MOV(src, sel.selReg(cvtInsn.getSrc(0), srcType));
          return true;
        }
      }
      return false;
    }

    INLINE void convertBetweenHalfFloat(Selection::Opaque &sel, const ir::ConvertInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const Type dstType = insn.getDstType();
      const Type srcType = insn.getSrcType();
      const GenRegister dst = sel.selReg(insn.getDst(0), dstType);
      const GenRegister src = sel.selReg(insn.getSrc(0), srcType);
      const Opcode opcode = insn.getOpcode();

      if (opcode == OP_F16TO32) {
        sel.F16TO32(dst, src);
      } else if (opcode == OP_F32TO16) {
        // We need two instructions to make the conversion
        GenRegister unpacked;
        unpacked = sel.unpacked_uw(sel.reg(FAMILY_DWORD, sel.isScalarReg(insn.getSrc(0))));
        sel.push();
        if (sel.isScalarReg(insn.getSrc(0))) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }
        sel.F32TO16(unpacked, src);
        sel.pop();
        sel.MOV(dst, unpacked);
      } else {
        GBE_ASSERT("Not conversion between float and half\n");
      }
    }

    INLINE void convert32bitsToSmall(Selection::Opaque &sel, const ir::ConvertInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const Type dstType = insn.getDstType();
      const Type srcType = insn.getSrcType();
      const GenRegister dst = sel.selReg(insn.getDst(0), dstType);
      const GenRegister src = sel.selReg(insn.getSrc(0), srcType);
      GenRegister unpacked;
      const RegisterFamily dstFamily = getFamily(dstType);

      if (dstFamily == FAMILY_WORD) {
        uint32_t type = dstType == TYPE_U16 ? GEN_TYPE_UW : GEN_TYPE_W;

        /* The special case, when dst is half, float->word->half will lose accuracy. */
        if (dstType == TYPE_HALF) {
          GBE_ASSERT(sel.hasHalfType());
          type = GEN_TYPE_HF;
        }

        if (!sel.isScalarReg(dst.reg())) {
          unpacked = sel.unpacked_uw(sel.reg(FAMILY_DWORD, sel.isScalarReg(insn.getSrc(0))));
          unpacked = GenRegister::retype(unpacked, type);
        } else
          unpacked = GenRegister::retype(sel.unpacked_uw(dst.reg()), type);
      } else {
        const uint32_t type = dstType == TYPE_U8 ? GEN_TYPE_UB : GEN_TYPE_B;
        if (!sel.isScalarReg(dst.reg())) {
          unpacked = sel.unpacked_ub(sel.reg(FAMILY_DWORD, sel.isScalarReg(insn.getSrc(0))));
          unpacked = GenRegister::retype(unpacked, type);
        } else
          unpacked = GenRegister::retype(sel.unpacked_ub(dst.reg()), type);
      }

      sel.push();
      if (sel.isScalarReg(insn.getSrc(0))) {
        sel.curr.execWidth = 1;
        sel.curr.predicate = GEN_PREDICATE_NONE;
        sel.curr.noMask = 1;
      }
      sel.MOV(unpacked, src);
      sel.pop();

      if (unpacked.reg() != dst.reg())
        sel.MOV(dst, unpacked);
    }

    INLINE void convertI64To16bits(Selection::Opaque &sel, const ir::ConvertInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const Type dstType = insn.getDstType();
      const Type srcType = insn.getSrcType();
      const GenRegister dst = sel.selReg(insn.getDst(0), dstType);
      const GenRegister src = sel.selReg(insn.getSrc(0), srcType);

      if (dstType == TYPE_HALF) {
        /* There is no MOV for Long <---> Half. So Long-->Float-->half. */
        GBE_ASSERT(sel.hasLongType());
        GBE_ASSERT(sel.hasHalfType());
        sel.push();
        if (sel.isScalarReg(insn.getSrc(0))) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }

        GenRegister funpacked = sel.unpacked_ud(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
        funpacked = GenRegister::retype(funpacked, GEN_TYPE_F);
        sel.MOV(funpacked, src);
        GenRegister ftmp = sel.selReg(sel.reg(FAMILY_DWORD, sel.isScalarReg(insn.getSrc(0))));
        ftmp = GenRegister::retype(ftmp, GEN_TYPE_F);
        sel.MOV(ftmp, funpacked);
        GenRegister unpacked = sel.unpacked_uw(sel.reg(FAMILY_DWORD, sel.isScalarReg(insn.getSrc(0))));
        unpacked = GenRegister::retype(unpacked, GEN_TYPE_HF);
        sel.MOV(unpacked, ftmp);
        sel.pop();
        sel.MOV(dst, unpacked);
      } else {
        uint32_t type = dstType == TYPE_U16 ? GEN_TYPE_UW : GEN_TYPE_W;

        GenRegister unpacked;
        if (!sel.isScalarReg(dst.reg())) {
          if (sel.hasLongType()) {
            unpacked = sel.unpacked_uw(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
          } else {
            unpacked = sel.unpacked_uw(sel.reg(FAMILY_DWORD, sel.isScalarReg(insn.getSrc(0))));
          }
          unpacked = GenRegister::retype(unpacked, type);
        } else {
          unpacked = GenRegister::retype(sel.unpacked_uw(dst.reg()), type);
        }

        if(!sel.hasLongType()) {
          GenRegister tmp = sel.selReg(sel.reg(FAMILY_DWORD));
          tmp.type = GEN_TYPE_D;
          sel.CONVI64_TO_I(tmp, src);
          sel.MOV(unpacked, tmp);
        } else {
          sel.push();
          if (sel.isScalarReg(insn.getSrc(0))) {
            sel.curr.execWidth = 1;
            sel.curr.predicate = GEN_PREDICATE_NONE;
            sel.curr.noMask = 1;
          }
          sel.MOV(unpacked, src);
          sel.pop();
        }

        if (unpacked.reg() != dst.reg()) {
          sel.MOV(dst, unpacked);
        }
      }
    }

    INLINE void convertI64ToI8(Selection::Opaque &sel, const ir::ConvertInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const Type dstType = insn.getDstType();
      const Type srcType = insn.getSrcType();
      const GenRegister dst = sel.selReg(insn.getDst(0), dstType);
      const GenRegister src = sel.selReg(insn.getSrc(0), srcType);
      GenRegister unpacked;
      const uint32_t type = dstType == TYPE_U8 ? GEN_TYPE_UB : GEN_TYPE_B;

      if (sel.hasLongType()) { // handle the native long logic.
        if (!sel.isScalarReg(dst.reg())) {
          /* When convert i64 to i8, the hstride should be 8, but the hstride do not
             support more than 4, so we need to split it to 2 steps. */
          unpacked = sel.unpacked_uw(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
          unpacked = GenRegister::retype(unpacked, dstType == TYPE_U8 ? GEN_TYPE_UW : GEN_TYPE_W);
        } else {
          unpacked = GenRegister::retype(sel.unpacked_ub(dst.reg()), type);
        }

        sel.push();
        if (sel.isScalarReg(insn.getSrc(0))) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }
        sel.MOV(unpacked, src);
        sel.pop();

        if (unpacked.reg() != dst.reg()) {
          sel.MOV(dst, unpacked);
        }
      } else { // Do not have native long
        if (!sel.isScalarReg(dst.reg())) {
          unpacked = sel.unpacked_ub(sel.reg(FAMILY_DWORD, sel.isScalarReg(insn.getSrc(0))));
          unpacked = GenRegister::retype(unpacked, type);
        } else {
          unpacked = GenRegister::retype(sel.unpacked_ub(dst.reg()), type);
        }

        GenRegister tmp = sel.selReg(sel.reg(FAMILY_DWORD));
        tmp.type = GEN_TYPE_D;
        sel.CONVI64_TO_I(tmp, src);
        sel.MOV(unpacked, tmp);

        if (unpacked.reg() != dst.reg()) {
          sel.MOV(dst, unpacked);
        }
      }
    }

    INLINE void convertI64ToI32(Selection::Opaque &sel, const ir::ConvertInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const Type dstType = insn.getDstType();
      const Type srcType = insn.getSrcType();
      const GenRegister dst = sel.selReg(insn.getDst(0), dstType);
      const GenRegister src = sel.selReg(insn.getSrc(0), srcType);
      if (sel.hasLongType()) {
        GenRegister unpacked;
        const uint32_t type = dstType == TYPE_U32 ? GEN_TYPE_UD : GEN_TYPE_D;
        if (!sel.isScalarReg(dst.reg())) {
          unpacked = sel.unpacked_ud(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
          unpacked = GenRegister::retype(unpacked, dstType == TYPE_U32 ? GEN_TYPE_UD : GEN_TYPE_D);
        } else {
          unpacked = GenRegister::retype(sel.unpacked_ud(dst.reg()), type);
        }

        sel.push();
        if (sel.isScalarReg(insn.getSrc(0))) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }
        sel.MOV(unpacked, src);
        sel.pop();

        if (unpacked.reg() != dst.reg()) {
          sel.MOV(dst, unpacked);
        }
      } else {
        sel.CONVI64_TO_I(dst, src);
      }
    }

    INLINE void convertI64ToFloat(Selection::Opaque &sel, const ir::ConvertInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const Type dstType = insn.getDstType();
      const Type srcType = insn.getSrcType();
      const GenRegister dst = sel.selReg(insn.getDst(0), dstType);
      const GenRegister src = sel.selReg(insn.getSrc(0), srcType);
      auto dag = sel.regDAG[src.reg()];

      // FIXME, in the future, we need to do a common I64 lower to I32 analysis
      // at llvm IR layer which could cover more cases then just this one.
      SelectionDAG *dag0, *dag1;
      if (dag && dag->child[0] && dag->child[1]) {
        if (dag->child[0]->insn.getOpcode() == OP_LOADI) {
          dag0 = dag->child[1];
          dag1 = dag->child[0];
        } else {
          dag0 = dag->child[0];
          dag1 = dag->child[1];
        }
        GBE_ASSERT(!(dag->child[0]->insn.getOpcode() == OP_LOADI &&
              dag->child[1]->insn.getOpcode() == OP_LOADI));
        if (dag->insn.getOpcode() == OP_AND ||
            dag->insn.getOpcode() == OP_OR  ||
            dag->insn.getOpcode() == OP_XOR) {
          GenRegister src0;
          GenRegister src1;
          if (lowerI64Reg(sel, dag0, src0, GEN_TYPE_UD) &&
              lowerI64Reg(sel, dag1, src1, GEN_TYPE_UD)) {
            switch (dag->insn.getOpcode()) {
              default:
              case OP_AND: sel.AND(GenRegister::retype(dst, GEN_TYPE_UD), src0, src1); break;
              case OP_OR:  sel.OR(GenRegister::retype(dst, GEN_TYPE_UD), src0, src1); break;
              case OP_XOR: sel.XOR(GenRegister::retype(dst, GEN_TYPE_UD), src0, src1); break;
            }
            sel.MOV(dst, GenRegister::retype(dst, GEN_TYPE_UD));
            markChildren = false;
            return;
          }
        }
      }

      if (!sel.hasLongType()) {
        GenRegister tmp[6];
        for(int i=0; i<6; i++) {
          tmp[i] = sel.selReg(sel.reg(FAMILY_DWORD), TYPE_U32);
        }
        sel.push();
        sel.curr.flag = 0;
        sel.curr.subFlag = 1;
        sel.CONVI64_TO_F(dst, src, tmp);
        sel.pop();
      } else {
        GenRegister unpacked;
        const uint32_t type = GEN_TYPE_F;
        unpacked = sel.unpacked_ud(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
        unpacked = GenRegister::retype(unpacked, type);

        sel.push();
        if (sel.isScalarReg(insn.getSrc(0))) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }
        sel.MOV(unpacked, src);
        sel.pop();

        if (unpacked.reg() != dst.reg()) {
          sel.MOV(dst, unpacked);
        }
      }
    }

    INLINE void convertSmallIntsToI64(Selection::Opaque &sel, const ir::ConvertInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const Type dstType = insn.getDstType();
      const Type srcType = insn.getSrcType();
      const GenRegister dst = sel.selReg(insn.getDst(0), dstType);
      const GenRegister src = sel.selReg(insn.getSrc(0), srcType);
      const RegisterFamily srcFamily = getFamily(srcType);

      if (sel.hasLongType() && sel.hasLongRegRestrict()) {
        // Convert i32/i16/i8 to i64 if hasLongRegRestrict(src and dst hstride must be aligned to the same qword).
        GenRegister unpacked;
        GenRegister unpacked_src = src;

        sel.push();
        if (sel.isScalarReg(insn.getSrc(0))) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }

        if(srcFamily == FAMILY_DWORD) {
          unpacked = sel.unpacked_ud(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
          unpacked = GenRegister::retype(unpacked, srcType == TYPE_U32 ? GEN_TYPE_UD : GEN_TYPE_D);
        } else if(srcFamily == FAMILY_WORD) {
          unpacked = sel.unpacked_uw(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
          unpacked = GenRegister::retype(unpacked, srcType == TYPE_U16 ? GEN_TYPE_UW : GEN_TYPE_W);
        } else if(srcFamily == FAMILY_BYTE) {
          GenRegister tmp = sel.selReg(sel.reg(FAMILY_WORD, sel.isScalarReg(insn.getSrc(0))));
          tmp = GenRegister::retype(tmp, srcType == TYPE_U8 ? GEN_TYPE_UW : GEN_TYPE_W);
          unpacked = sel.unpacked_uw(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
          unpacked = GenRegister::retype(unpacked, srcType == TYPE_U8 ? GEN_TYPE_UW : GEN_TYPE_W);
          sel.MOV(tmp, src);
          unpacked_src = tmp;
        } else
          GBE_ASSERT(0);

        sel.MOV(unpacked, unpacked_src);
        sel.pop();
        sel.MOV(dst, unpacked);
      } else if (sel.hasLongType()) {
        sel.MOV(dst, src);
      } else {
        sel.CONVI_TO_I64(dst, src, sel.selReg(sel.reg(FAMILY_DWORD)));
      }
    }

    INLINE void convertFToI64(Selection::Opaque &sel, const ir::ConvertInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const Type dstType = insn.getDstType();
      const Type srcType = insn.getSrcType();
      const GenRegister dst = sel.selReg(insn.getDst(0), dstType);
      const GenRegister src = sel.selReg(insn.getSrc(0), srcType);

      if (sel.hasLongType() && sel.hasLongRegRestrict() && srcType == ir::TYPE_FLOAT) { // typical bsw float->long case
        // Convert float to i64 if hasLongRegRestrict(src and dst hstride must be aligned to the same qword).
        GenRegister unpacked;
        GenRegister unpacked_src = src;

        sel.push();
        if (sel.isScalarReg(insn.getSrc(0))) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }

        unpacked = sel.unpacked_ud(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
        unpacked = GenRegister::retype(unpacked, GEN_TYPE_F);
        sel.MOV(unpacked, unpacked_src);
        sel.pop();
        sel.MOV(dst, unpacked);
      } else if (srcType == ir::TYPE_FLOAT) {
        if (sel.hasLongType()) { // typical bdw float->long case
          sel.MOV(dst, src);
        } else { // typical old platform float->long case
          GenRegister tmp[2];
          tmp[0] = sel.selReg(sel.reg(FAMILY_DWORD), TYPE_U32);
          tmp[1] = sel.selReg(sel.reg(FAMILY_DWORD), TYPE_FLOAT);
          sel.push();
          sel.curr.flag = 0;
          sel.curr.subFlag = 1;
          sel.CONVF_TO_I64(dst, src, tmp);
          sel.pop();
        }
      } else if (srcType == ir::TYPE_HALF) {
        /* No need to consider old platform. if we support half, we must have native long. */
        GBE_ASSERT(sel.hasLongType());
        GBE_ASSERT(sel.hasHalfType());
        uint32_t type = dstType == TYPE_U64 ? GEN_TYPE_UD : GEN_TYPE_D;
        GenRegister tmp = GenRegister::retype(sel.selReg(sel.reg(FAMILY_DWORD, sel.isScalarReg(insn.getSrc(0))), TYPE_U32), type);
        sel.push();
        if (sel.isScalarReg(insn.getSrc(0))) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }

        sel.MOV(tmp, src);

        if (sel.hasLongRegRestrict()) { // special for BSW case.
          GenRegister unpacked = sel.unpacked_ud(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
          unpacked = GenRegister::retype(unpacked, type);
          sel.MOV(unpacked, tmp);
          sel.pop();
          sel.MOV(dst, unpacked);
        } else {
          sel.pop();
          sel.MOV(dst, tmp);
        }
      } else if (src.type == GEN_TYPE_DF) {
        GBE_ASSERT(sel.hasDoubleType());
        GBE_ASSERT(sel.hasLongType()); //So far, if we support double, we support native long.

        // Just Mov
        sel.MOV(dst, src);
      } else {
        /* Invalid case. */
        GBE_ASSERT(0);
      }
    }

    INLINE void convertBetweenFloatDouble(Selection::Opaque &sel, const ir::ConvertInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const Type dstType = insn.getDstType();
      const Type srcType = insn.getSrcType();
      const GenRegister dst = sel.selReg(insn.getDst(0), dstType);
      const GenRegister src = sel.selReg(insn.getSrc(0), srcType);

      GBE_ASSERT(sel.hasDoubleType());

      if (sel.isScalarReg(insn.getDst(0))) {
        // dst is scalar, just MOV and nothing more.
        GBE_ASSERT(sel.isScalarReg(insn.getSrc(0)));
        sel.MOV(dst, src);
      } else if (srcType == ir::TYPE_DOUBLE) {
        // double to float
        GBE_ASSERT(dstType == ir::TYPE_FLOAT);
        GenRegister unpacked;
        unpacked = sel.unpacked_ud(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
        unpacked = GenRegister::retype(unpacked, GEN_TYPE_F);

        sel.push();
        if (sel.isScalarReg(insn.getSrc(0))) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }
        sel.MOV(unpacked, src);
        sel.pop();

        sel.MOV(dst, unpacked);
      } else {
        // float to double, just mov
        sel.MOV(dst, src);
      }

      return;
    }

    INLINE void convertBetweenHalfDouble(Selection::Opaque &sel, const ir::ConvertInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const Type dstType = insn.getDstType();
      const Type srcType = insn.getSrcType();
      const GenRegister dst = sel.selReg(insn.getDst(0), dstType);
      const GenRegister src = sel.selReg(insn.getSrc(0), srcType);

      GBE_ASSERT(sel.hasDoubleType());
      GBE_ASSERT(sel.hasHalfType()); //So far, if we support double, we support half.

      if (sel.isScalarReg(insn.getDst(0))) { // uniform case.
        GBE_ASSERT(sel.isScalarReg(insn.getSrc(0)));
        GBE_ASSERT(sel.curr.execWidth == 1);
        GenRegister tmpFloat = GenRegister::retype(sel.selReg(sel.reg(FAMILY_DWORD)), GEN_TYPE_F);
        sel.MOV(tmpFloat, src);
        sel.MOV(dst, tmpFloat);
        return;
      }

      if (dstType == ir::TYPE_DOUBLE) {
        // half to double. There is no direct double to half MOV, need tmp float.
        GBE_ASSERT(srcType == ir::TYPE_HALF);
        GenRegister tmpFloat = GenRegister::retype(sel.selReg(sel.reg(FAMILY_DWORD)), GEN_TYPE_F);

        sel.push();
        if (sel.isScalarReg(insn.getSrc(0))) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }
        sel.MOV(tmpFloat, src);
        sel.pop();

        sel.MOV(dst, tmpFloat);
      } else {
        // double to half. No direct MOV from double to half, so double->float->half
        GBE_ASSERT(srcType == ir::TYPE_DOUBLE);
        GBE_ASSERT(dstType == ir::TYPE_HALF);

        sel.push();
        if (sel.isScalarReg(insn.getSrc(0))) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }

        // double to float
        GenRegister unpackedFloat = sel.unpacked_ud(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
        unpackedFloat = GenRegister::retype(unpackedFloat, GEN_TYPE_F);
        sel.MOV(unpackedFloat, src);

        // float to half
        GenRegister unpackedHalf = sel.unpacked_uw(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
        unpackedHalf = GenRegister::retype(unpackedHalf, GEN_TYPE_HF);
        sel.MOV(unpackedHalf, unpackedFloat);
        sel.pop();

        sel.MOV(dst, unpackedHalf);
      }
    }

    INLINE void convertHalfToSmallInts(Selection::Opaque &sel, const ir::ConvertInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const Type dstType = insn.getDstType();
      const Type srcType = insn.getSrcType();
      const GenRegister dst = sel.selReg(insn.getDst(0), dstType);
      const GenRegister src = sel.selReg(insn.getSrc(0), srcType);
      const RegisterFamily dstFamily = getFamily(dstType);

      // Special case, half -> char/short.
      /* [DevBDW+]:	Format conversion to or from HF (Half Float) must be DWord-aligned and
         strided by a DWord on the destination. */
      GBE_ASSERT(sel.hasHalfType());
      GenRegister tmp;
      sel.push();
      if (sel.isScalarReg(insn.getSrc(0))) {
        sel.curr.execWidth = 1;
        sel.curr.predicate = GEN_PREDICATE_NONE;
        sel.curr.noMask = 1;
      }
      if (dstFamily == FAMILY_BYTE) {
        const uint32_t type = dstType == TYPE_U8 ? GEN_TYPE_UB : GEN_TYPE_B;
        tmp = GenRegister::retype(sel.unpacked_ub(sel.reg(FAMILY_DWORD, sel.isScalarReg(insn.getSrc(0)))), type);
        sel.MOV(tmp, src);
      } else {
        const uint32_t type = dstType == TYPE_U16 ? GEN_TYPE_UW : GEN_TYPE_W;
        tmp = GenRegister::retype(sel.unpacked_uw(sel.reg(FAMILY_DWORD, sel.isScalarReg(insn.getSrc(0)))), type);
        sel.MOV(tmp, src);
      }
      sel.pop();
      sel.MOV(dst, tmp);
    }

    INLINE void convertSmallIntsToHalf(Selection::Opaque &sel, const ir::ConvertInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const Type dstType = insn.getDstType();
      const Type srcType = insn.getSrcType();
      const GenRegister dst = sel.selReg(insn.getDst(0), dstType);
      const GenRegister src = sel.selReg(insn.getSrc(0), srcType);

      // Special case, char/uchar -> half
      /* [DevBDW+]:  Format conversion to or from HF (Half Float) must be DWord-aligned and
         strided by a DWord on the destination. */
      GBE_ASSERT(sel.hasHalfType());
      GenRegister tmp = GenRegister::retype(sel.unpacked_uw(sel.reg(FAMILY_DWORD, sel.isScalarReg(insn.getSrc(0)))), GEN_TYPE_HF);
      sel.push();
      if (sel.isScalarReg(insn.getSrc(0))) {
        sel.curr.execWidth = 1;
        sel.curr.predicate = GEN_PREDICATE_NONE;
        sel.curr.noMask = 1;
      }
      sel.MOV(tmp, src);
      sel.pop();
      sel.MOV(dst, tmp);
    }

    INLINE void convertDoubleToSmallInts(Selection::Opaque &sel, const ir::ConvertInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const Type dstType = insn.getDstType();
      const Type srcType = insn.getSrcType();
      const GenRegister dst = sel.selReg(insn.getDst(0), dstType);
      const GenRegister src = sel.selReg(insn.getSrc(0), srcType);
      const RegisterFamily dstFamily = getFamily(dstType);

      GBE_ASSERT(sel.hasDoubleType());
      GBE_ASSERT(sel.hasHalfType()); //So far, if we support double, we support half.
      if (sel.isScalarReg(insn.getDst(0))) {
        // dst is scalar, just MOV and nothing more.
        GBE_ASSERT(sel.isScalarReg(insn.getSrc(0)));
        sel.MOV(dst, src);
      } else {
        GenRegister unpacked;
        if (dstFamily == FAMILY_DWORD) {
          // double to int
          unpacked = sel.unpacked_ud(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
          unpacked = GenRegister::retype(unpacked, dstType == TYPE_U32 ? GEN_TYPE_UD : GEN_TYPE_D);
        } else if (dstFamily == FAMILY_WORD) {
          // double to short
          unpacked = sel.unpacked_uw(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
          unpacked = GenRegister::retype(unpacked, dstType == TYPE_U16 ? GEN_TYPE_UW : GEN_TYPE_W);
        } else {
          GBE_ASSERT(dstFamily == FAMILY_BYTE);
          // double to char
          unpacked = sel.unpacked_uw(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
          unpacked = GenRegister::retype(unpacked, dstType == TYPE_U8 ? GEN_TYPE_UW : GEN_TYPE_W);
        }

        sel.push();
        if (sel.isScalarReg(insn.getSrc(0))) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }
        sel.MOV(unpacked, src);
        sel.pop();

        sel.MOV(dst, unpacked);
      }
    }

    INLINE void convertI64ToDouble(Selection::Opaque &sel, const ir::ConvertInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const Type dstType = insn.getDstType();
      const Type srcType = insn.getSrcType();
      const GenRegister dst = sel.selReg(insn.getDst(0), dstType);
      const GenRegister src = sel.selReg(insn.getSrc(0), srcType);

      GBE_ASSERT(sel.hasDoubleType());
      GBE_ASSERT(sel.hasLongType()); //So far, if we support double, we support native long.
      // Just Mov
      sel.MOV(dst, src);
    }

    INLINE void convertSmallIntsToDouble(Selection::Opaque &sel, const ir::ConvertInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const Type dstType = insn.getDstType();
      const Type srcType = insn.getSrcType();
      const GenRegister dst = sel.selReg(insn.getDst(0), dstType);
      const GenRegister src = sel.selReg(insn.getSrc(0), srcType);
      const RegisterFamily srcFamily = getFamily(srcType);

      GBE_ASSERT(sel.hasDoubleType());
      GBE_ASSERT(sel.hasLongType()); //So far, if we support double, we support native long.

      if (sel.hasLongType() && sel.hasLongRegRestrict()) {
        // Convert i32/i16/i8 to i64 if hasLongRegRestrict(src and dst hstride must be aligned to the same qword).
        GenRegister unpacked;
        GenRegister unpacked_src = src;

        sel.push();
        if (sel.isScalarReg(insn.getSrc(0))) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }

        if(srcFamily == FAMILY_DWORD) {
          unpacked = sel.unpacked_ud(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
          unpacked = GenRegister::retype(unpacked, srcType == TYPE_U32 ? GEN_TYPE_UD : GEN_TYPE_D);
        } else if(srcFamily == FAMILY_WORD) {
          unpacked = sel.unpacked_uw(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
          unpacked = GenRegister::retype(unpacked, srcType == TYPE_U16 ? GEN_TYPE_UW : GEN_TYPE_W);
        } else if(srcFamily == FAMILY_BYTE) {
          GenRegister tmp = sel.selReg(sel.reg(FAMILY_WORD, sel.isScalarReg(insn.getSrc(0))));
          tmp = GenRegister::retype(tmp, srcType == TYPE_U8 ? GEN_TYPE_UW : GEN_TYPE_W);
          unpacked = sel.unpacked_uw(sel.reg(FAMILY_QWORD, sel.isScalarReg(insn.getSrc(0))));
          unpacked = GenRegister::retype(unpacked, srcType == TYPE_U8 ? GEN_TYPE_UW : GEN_TYPE_W);
          sel.MOV(tmp, src);
          unpacked_src = tmp;
        } else
          GBE_ASSERT(0);

        sel.MOV(unpacked, unpacked_src);
        sel.pop();
        sel.MOV(dst, unpacked);
      } else if (sel.hasLongType()) {
        sel.MOV(dst, src);
      }
    }

    INLINE bool emitOne(Selection::Opaque &sel, const ir::ConvertInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const Type dstType = insn.getDstType();
      const Type srcType = insn.getSrcType();
      const RegisterFamily dstFamily = getFamily(dstType);
      const RegisterFamily srcFamily = getFamily(srcType);
      const GenRegister dst = sel.selReg(insn.getDst(0), dstType);
      const GenRegister src = sel.selReg(insn.getSrc(0), srcType);
      const Opcode opcode = insn.getOpcode();
      sel.push();
      if (sel.isScalarReg(insn.getDst(0)) == true) {
        sel.curr.execWidth = 1;
        sel.curr.predicate = GEN_PREDICATE_NONE;
        sel.curr.noMask = 1;
      }
      if(opcode == ir::OP_SAT_CVT)
        sel.curr.saturate = 1;

      if (opcode == OP_F16TO32 || opcode == OP_F32TO16) {
        /* Conversion between float and half. */
        convertBetweenHalfFloat(sel, insn, markChildren);
      } else if (dstFamily != FAMILY_DWORD && dstFamily != FAMILY_QWORD && srcFamily == FAMILY_DWORD) {
        //convert i32/float to small int/half
        convert32bitsToSmall(sel, insn, markChildren);
      } else if (dstFamily == FAMILY_WORD && srcFamily == FAMILY_QWORD && srcType != ir::TYPE_DOUBLE) {
        //convert i64 to i16 and half.
        convertI64To16bits(sel, insn, markChildren);
      } else if (dstFamily == FAMILY_BYTE && srcFamily == FAMILY_QWORD && srcType != ir::TYPE_DOUBLE) {
        //convert i64 to i8
        convertI64ToI8(sel, insn, markChildren);
      } else if ((dstType == ir::TYPE_S32 || dstType == ir::TYPE_U32) &&
          (srcType == ir::TYPE_U64 || srcType == ir::TYPE_S64)) {
        // Convert i64 to i32
        convertI64ToI32(sel, insn, markChildren);
      } else if (dstType == ir::TYPE_FLOAT && (srcType == ir::TYPE_U64 || srcType == ir::TYPE_S64)) {
        // long -> float
        convertI64ToFloat(sel, insn, markChildren);
      } else if (dstType == ir::TYPE_DOUBLE && (srcType == ir::TYPE_U64 || srcType == ir::TYPE_S64)) {
        // long -> double
        convertI64ToDouble(sel, insn, markChildren);
      } else if ((dstType == ir::TYPE_U64 || dstType == ir::TYPE_S64)
          && (srcFamily != FAMILY_QWORD && srcType != ir::TYPE_FLOAT && srcType != ir::TYPE_HALF)) {
        // int/short/char to long
        convertSmallIntsToI64(sel, insn, markChildren);
      } else if ((dstType == ir::TYPE_DOUBLE)
          && (srcFamily != FAMILY_QWORD && srcType != ir::TYPE_FLOAT && srcType != ir::TYPE_HALF)) {
        // int/short/char to double
        convertSmallIntsToDouble(sel, insn, markChildren);
      } else if ((dstType == ir::TYPE_U64 || dstType == ir::TYPE_S64)
          && (srcType == ir::TYPE_FLOAT || srcType == ir::TYPE_HALF || srcType == ir::TYPE_DOUBLE)) {
        // All float type to long
        convertFToI64(sel, insn, markChildren);
      } else if ((srcType == ir::TYPE_FLOAT && dstType == ir::TYPE_DOUBLE) ||
            (dstType == ir::TYPE_FLOAT && srcType == ir::TYPE_DOUBLE)) {
        // float and double conversion
        convertBetweenFloatDouble(sel, insn, markChildren);
      } else if ((srcType == ir::TYPE_HALF && dstType == ir::TYPE_DOUBLE) ||
            (dstType == ir::TYPE_HALF && srcType == ir::TYPE_DOUBLE)) {
        // float and half conversion
        convertBetweenHalfDouble(sel, insn, markChildren);
      } else if (srcType == ir::TYPE_DOUBLE && dstType != ir::TYPE_FLOAT
             && dstType != ir::TYPE_HALF && dstFamily != FAMILY_QWORD) {
        // double to int/short/char
        convertDoubleToSmallInts(sel, insn, markChildren);
      } else if (srcType == ir::TYPE_HALF && (dstFamily == FAMILY_BYTE || dstFamily == FAMILY_WORD)) {
        // Convert half to small int
        convertHalfToSmallInts(sel, insn, markChildren);
      } else if (dstType == ir::TYPE_HALF && (srcFamily == FAMILY_BYTE || srcFamily == FAMILY_WORD)) {
        // Convert small int to half
        convertSmallIntsToHalf(sel, insn, markChildren);
      } else {
        /* All special cases has been handled, just MOV. */
        sel.MOV(dst, src);
      }

      sel.pop();
      return true;
    }
    DECL_CTOR(ConvertInstruction, 1, 1);
  };

  /*! atomic instruction pattern */
  class AtomicInstructionPattern : public SelectionPattern
  {
  public:
    AtomicInstructionPattern(void) : SelectionPattern(1,1) {
      for (uint32_t op = 0; op < ir::OP_INVALID; ++op)
        if (ir::isOpcodeFrom<ir::AtomicInstruction>(ir::Opcode(op)) == true)
          this->opcodes.push_back(ir::Opcode(op));
    }

    INLINE bool emit(Selection::Opaque &sel, SelectionDAG &dag) const {
      using namespace ir;
      const ir::AtomicInstruction &insn = cast<ir::AtomicInstruction>(dag.insn);

      ir::BTI b;
      const AtomicOps atomicOp = insn.getAtomicOpcode();
      unsigned srcNum = insn.getSrcNum();
      unsigned msgPayload;

      AddressMode AM = insn.getAddressMode();
      if (AM == AM_DynamicBti) {
        b.reg = insn.getBtiReg();
        msgPayload = srcNum - 1;
      } else {
        b.imm = insn.getSurfaceIndex();
        b.isConst = 1;
        msgPayload = srcNum;
      }

      GenRegister dst  = sel.selReg(insn.getDst(0), TYPE_U32);
      GenRegister bti =  b.isConst ? GenRegister::immud(b.imm) : sel.selReg(b.reg, ir::TYPE_U32);
      GenRegister src0 = sel.selReg(insn.getAddressRegister(), TYPE_U32);
      GenRegister src1 = src0, src2 = src0;
      if(msgPayload > 1) src1 = sel.selReg(insn.getSrc(1), TYPE_U32);
      if(msgPayload > 2) src2 = sel.selReg(insn.getSrc(2), TYPE_U32);

      GenAtomicOpCode genAtomicOp = (GenAtomicOpCode)atomicOp;
      sel.ATOMIC(dst, genAtomicOp, msgPayload, src0, src1, src2, bti, sel.getBTITemps(b));

      markAllChildren(dag);
      return true;
    }
  };

  /*! Select instruction pattern */
  class SelectInstructionPattern : public SelectionPattern
  {
  public:
    SelectInstructionPattern(void) : SelectionPattern(1,1) {
      for (uint32_t op = 0; op < ir::OP_INVALID; ++op)
        if (ir::isOpcodeFrom<ir::SelectInstruction>(ir::Opcode(op)) == true)
          this->opcodes.push_back(ir::Opcode(op));
    }

    INLINE bool emit(Selection::Opaque &sel, SelectionDAG &dag) const
    {
      using namespace ir;
      const ir::SelectInstruction &insn = cast<SelectInstruction>(dag.insn);

      // Get all registers for the instruction
      const Type type = insn.getType();
      const GenRegister dst  = sel.selReg(insn.getDst(0), type);

      // Look for immediate values for the right source
      GenRegister src0, src1;
      SelectionDAG *dag0 = dag.child[0]; // source 0 is the predicate!
      SelectionDAG *dag1 = dag.child[1];
      SelectionDAG *dag2 = dag.child[2];

      if (dag0) dag0->isRoot = 1;
      bool inverse = false;
      sel.getSrcGenRegImm(dag, dag1, dag2, src0, src1, type, inverse);
      const Register pred = insn.getPredicate();
      sel.push();
        if (sel.isScalarReg(insn.getDst(0)) == true) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }
        sel.curr.inversePredicate ^= inverse;
        sel.curr.physicalFlag = 0;
        sel.curr.flagIndex = pred.value();
        sel.curr.predicate = GEN_PREDICATE_NORMAL;
        // FIXME in general, if the flag is a uniform flag.
        // we should treat that flag as extern flag, as we
        // never genrate a uniform physical flag. As we can
        // never predicate which channel is active when this
        // flag is used.
        // We need to concentrate this logic to the modFlag bit.
        // If an instruction has that bit, it will generate physical
        // flag, otherwise it will not. But current modFlag is
        // just a hint. We need to fix it in the future.
        if (!dag0 || (sel.isScalarReg(dag0->insn.getDst(0))))
          sel.curr.externFlag = 1;
        if((type == ir::TYPE_S64 || type == ir::TYPE_U64) && !sel.hasLongType())
          sel.SEL_INT64(dst, src0, src1);
        else
          sel.SEL(dst, src0, src1);
      sel.pop();

      return true;
    }
  };

  DECL_PATTERN(TernaryInstruction)
   {
    INLINE bool emitOne(Selection::Opaque &sel, const ir::TernaryInstruction &insn, bool &markChildren) const {
      using namespace ir;
      const Type type = insn.getType();
      const GenRegister dst = sel.selReg(insn.getDst(0), type),
                        src0 = sel.selReg(insn.getSrc(0), type),
                        src1 = sel.selReg(insn.getSrc(1), type),
                        src2 = sel.selReg(insn.getSrc(2), type);
      switch(insn.getOpcode()) {
        case OP_I64MADSAT:
         {
           GenRegister tmp[9];
           int tmp_num;
           if (!sel.hasLongType()) {
             tmp_num = 9;
             for(int i=0; i<9; i++) {
               tmp[i] = sel.selReg(sel.reg(FAMILY_DWORD));
               tmp[i].type = GEN_TYPE_UD;
             }
           } else {
             tmp_num = 6;
             for(int i=0; i<6; i++) {
               tmp[i] = sel.selReg(sel.reg(FAMILY_QWORD), ir::TYPE_U64);
               tmp[i].type = GEN_TYPE_UL;
             }
           }
           sel.push();
           sel.curr.flag = 0;
           sel.curr.subFlag = 1;
           sel.I64MADSAT(dst, src0, src1, src2, tmp, tmp_num);
           sel.pop();
           break;
         }
        case OP_MAD:
         {
          sel.MAD(dst, src2, src0, src1);
          break;
         }
        case OP_LRP:
         {
          sel.LRP(dst, src0, src1, src2);
          break;
         }
        default:
          NOT_IMPLEMENTED;
      }
      return true;
    }

    DECL_CTOR(TernaryInstruction, 1, 1);
   };


  /*! Label instruction pattern */
  DECL_PATTERN(LabelInstruction)
  {
    INLINE bool emitOne(Selection::Opaque &sel, const ir::LabelInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const LabelIndex label = insn.getLabelIndex();
      const GenRegister src0 = sel.getBlockIP();
      const GenRegister src1 = sel.getLabelImmReg(label);
      const uint32_t simdWidth = sel.ctx.getSimdWidth();
      GBE_ASSERTM(label < sel.ctx.getMaxLabel(), "We reached the maximum label number which is reserved for barrier handling");
      sel.LABEL(label);

      if(!insn.getParent()->needIf)
        return true;

      // Do not emit any code for the "returning" block. There is no need for it
      if (insn.getParent() == &sel.ctx.getFunction().getBottomBlock())
        return true;

      LabelIndex jip;
      const LabelIndex nextLabel = insn.getParent()->getNextBlock()->getLabelIndex();
      if (sel.ctx.hasJIP(&insn))
        jip = sel.ctx.getLabelIndex(&insn);
      else
        jip = nextLabel;

      // Emit the mask computation at the head of each basic block
      sel.push();
        sel.curr.noMask = 1;
        sel.curr.predicate = GEN_PREDICATE_NONE;
        sel.cmpBlockIP(GEN_CONDITIONAL_LE, src0, src1);
      sel.pop();

      if (sel.block->hasBarrier) {
        // If this block has barrier, we don't execute the block until all lanes
        // are 1s. Set each reached lane to 1, then check all lanes. If there is any
        // lane not reached, we jump to jip. And no need to issue if/endif for
        // this block, as it will always excute with all lanes activated.
        sel.push();
          sel.curr.predicate = GEN_PREDICATE_NORMAL;
          sel.setBlockIP(src0, sel.ctx.getMaxLabel());
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
          sel.cmpBlockIP(GEN_CONDITIONAL_EQ, src0, sel.ctx.getMaxLabel());
          if (simdWidth == 8)
            sel.curr.predicate = GEN_PREDICATE_ALIGN1_ALL8H;
          else if (simdWidth == 16)
            sel.curr.predicate = GEN_PREDICATE_ALIGN1_ALL16H;
          else
            NOT_IMPLEMENTED;
          sel.curr.noMask = 1;
          sel.curr.execWidth = 1;
          sel.curr.inversePredicate = 1;
          sel.JMPI(GenRegister::immd(0), jip, label);
        sel.pop();
        // FIXME, if the last BRA is unconditional jump, we don't need to update the label here.
        sel.push();
         sel.curr.predicate = GEN_PREDICATE_NORMAL;
         sel.setBlockIP(src0, label.value());
        sel.pop();
      }
      else {
        if (sel.ctx.hasJIP(&insn) &&
            // If jump to next label and the endif offset is -1, then
            // We don't need to add a jmpi here, as the following IF will do the same
            // thing if all channels are disabled.
            (jip != nextLabel || sel.block->endifOffset != -1)) {
          // If it is required, insert a JUMP to bypass the block
          sel.push();
            if (simdWidth == 8)
              sel.curr.predicate = GEN_PREDICATE_ALIGN1_ANY8H;
            else if (simdWidth == 16)
              sel.curr.predicate = GEN_PREDICATE_ALIGN1_ANY16H;
            else
              NOT_IMPLEMENTED;
            sel.curr.noMask = 1;
            sel.curr.execWidth = 1;
            sel.curr.inversePredicate = 1;
            sel.JMPI(GenRegister::immd(0), jip, label);
          sel.pop();
        }
        if(!sel.block->removeSimpleIfEndif){
          sel.push();
            sel.curr.predicate = GEN_PREDICATE_NORMAL;
            if(!insn.getParent()->needEndif && insn.getParent()->needIf) {
              ir::LabelIndex label = insn.getParent()->endifLabel;
              sel.IF(GenRegister::immd(0), label, label);
            }
            else
              sel.IF(GenRegister::immd(0), sel.block->endifLabel, sel.block->endifLabel);
          sel.pop();
        }
      }

      return true;
    }
    DECL_CTOR(LabelInstruction, 1, 1);
  };

  DECL_PATTERN(SampleInstruction)
  {
    INLINE void emitLd_ivb(Selection::Opaque &sel, const ir::SampleInstruction &insn,
                           GenRegister msgPayloads[4], uint32_t &msgLen) const
    {
      // pre SKL: U, lod, [V], [W]
      using namespace ir;
      GBE_ASSERT(insn.getSrcType() != TYPE_FLOAT);
      uint32_t srcNum = insn.getSrcNum();
      msgPayloads[0] = sel.selReg(insn.getSrc(0), insn.getSrcType());
      msgPayloads[1] = sel.selReg(sel.reg(FAMILY_DWORD), TYPE_U32);
      sel.MOV(msgPayloads[1], GenRegister::immud(0));
      if (srcNum > 1)
        msgPayloads[2] = sel.selReg(insn.getSrc(1), insn.getSrcType());
      if (srcNum > 2)
        msgPayloads[3] = sel.selReg(insn.getSrc(2), insn.getSrcType());
      // Clear the lod to zero.
      msgLen = srcNum + 1;
    }

    INLINE void emitLd_skl(Selection::Opaque &sel, const ir::SampleInstruction &insn,
                           GenRegister msgPayloads[4], uint32_t &msgLen) const
    {
      // SKL: U, [V], [lod], [W]
      using namespace ir;
      GBE_ASSERT(insn.getSrcType() != TYPE_FLOAT);
      uint32_t srcNum = msgLen = insn.getSrcNum();
      msgPayloads[0] = sel.selReg(insn.getSrc(0), insn.getSrcType());
      if (srcNum > 1)
        msgPayloads[1] = sel.selReg(insn.getSrc(1), insn.getSrcType());
      if (srcNum > 2) {
        // Clear the lod to zero.
        msgPayloads[2] = sel.selReg(sel.reg(FAMILY_DWORD), TYPE_U32);
        sel.MOV(msgPayloads[2], GenRegister::immud(0));
        msgLen += 1;

        msgPayloads[3] = sel.selReg(insn.getSrc(2), insn.getSrcType());
      }
    }

    INLINE bool emitOne(Selection::Opaque &sel, const ir::SampleInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      GenRegister msgPayloads[4];
      vector<GenRegister> dst(insn.getDstNum());
      uint32_t srcNum = insn.getSrcNum();
      uint32_t valueID = 0;
      uint32_t msgLen = 0;
      for (valueID = 0; valueID < insn.getDstNum(); ++valueID)
        dst[valueID] = sel.selReg(insn.getDst(valueID), insn.getDstType());

      if (insn.getSamplerOffset() != 0) {
        if(sel.getLdMsgOrder() < LD_MSG_ORDER_SKL)
          this->emitLd_ivb(sel, insn, msgPayloads, msgLen);
        else
          this->emitLd_skl(sel, insn, msgPayloads, msgLen);
      } else {
        // U, V, [W]
        GBE_ASSERT(insn.getSrcType() == TYPE_FLOAT);
        for (valueID = 0; valueID < srcNum; ++valueID)
          msgPayloads[valueID] = sel.selReg(insn.getSrc(valueID), insn.getSrcType());
        msgLen = srcNum;
      }
      // We switch to a fixup bti for linear filter on a image1d array sampling.
      uint32_t bti = insn.getImageIndex() + (insn.getSamplerOffset() == 2 ? BTI_WORKAROUND_IMAGE_OFFSET : 0);
      if (bti > BTI_MAX_ID) {
        std::cerr << "Too large bti " << bti;
        return false;
      }
      uint32_t sampler = insn.getSamplerIndex();

      sel.SAMPLE(dst.data(), insn.getDstNum(), msgPayloads, msgLen, bti, sampler, insn.getSamplerOffset() != 0, false);
      return true;
    }
    DECL_CTOR(SampleInstruction, 1, 1);
  };

  DECL_PATTERN(VmeInstruction)
  {
    INLINE bool emitOne(Selection::Opaque &sel, const ir::VmeInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      uint32_t msg_type, vme_search_path_lut, lut_sub;
      msg_type = insn.getMsgType();
      vme_search_path_lut = 0;
      lut_sub = 0;
      GBE_ASSERT(msg_type == 1);
      uint32_t payloadLen = 0;
      //We allocate 5 virtual payload grfs to selection dst register.
      if(msg_type == 1){
        payloadLen = 5;
      }
      uint32_t selDstNum = insn.getDstNum() + payloadLen;
      uint32_t srcNum = insn.getSrcNum();
      vector<GenRegister> dst(selDstNum);
      vector<GenRegister> payloadVal(srcNum);
      uint32_t valueID = 0;
      for (valueID = 0; valueID < insn.getDstNum(); ++valueID)
        dst[valueID] = sel.selReg(insn.getDst(valueID), insn.getDstType());
      for (valueID = insn.getDstNum(); valueID < selDstNum; ++valueID)
        dst[valueID] = sel.selReg(sel.reg(FAMILY_DWORD), TYPE_U32);

      for (valueID = 0; valueID < srcNum; ++valueID)
        payloadVal[valueID] = sel.selReg(insn.getSrc(valueID), insn.getSrcType());

      uint32_t bti = insn.getImageIndex();
      if (bti > BTI_MAX_ID) {
        std::cerr << "Too large bti " << bti;
        return false;
      }

      sel.VME(bti, dst.data(), payloadVal.data(), selDstNum, srcNum, msg_type, vme_search_path_lut, lut_sub);

      return true;
    }
    DECL_CTOR(VmeInstruction, 1, 1);
  };

  /*! Typed write instruction pattern. */
  DECL_PATTERN(TypedWriteInstruction)
  {
    INLINE bool emitOne(Selection::Opaque &sel, const ir::TypedWriteInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const uint32_t simdWidth = sel.ctx.getSimdWidth();
      GenRegister msgs[9]; // (header + U + V + R + LOD + 4)
      const uint32_t msgNum = (8 / (simdWidth / 8)) + 1;
      const uint32_t dim = insn.getSrcNum() - 4;

      if (simdWidth == 16) {
        for(uint32_t i = 0; i < msgNum; i++)
          msgs[i] = sel.selReg(sel.reg(FAMILY_DWORD), TYPE_U32);
      } else {
        uint32_t valueID = 0;
        uint32_t msgID = 0;
        msgs[msgID++] = sel.selReg(sel.reg(FAMILY_DWORD), TYPE_U32);
        for(; msgID < 1 + dim; msgID++, valueID++)
          msgs[msgID] = sel.selReg(insn.getSrc(msgID - 1), insn.getCoordType());

        // fake v.
        if (dim < 2)
          msgs[msgID++] = sel.selReg(sel.reg(FAMILY_DWORD), TYPE_U32);
        // fake w.
        if (dim < 3)
          msgs[msgID++] = sel.selReg(sel.reg(FAMILY_DWORD), TYPE_U32);
        // LOD.
        msgs[msgID++] = sel.selReg(sel.reg(FAMILY_DWORD), TYPE_U32);
        for(; valueID < insn.getSrcNum(); msgID++, valueID++)
          msgs[msgID] = sel.selReg(insn.getSrc(valueID), insn.getSrcType());
      }

      sel.push();
      sel.curr.predicate = GEN_PREDICATE_NONE;
      sel.curr.noMask = 1;
      sel.MOV(msgs[0], GenRegister::immud(0));
      sel.curr.execWidth = 1;

      GenRegister channelEn = sel.getOffsetReg(msgs[0], 0, 7*4);
      // Enable all channels.
      sel.MOV(channelEn, GenRegister::immud(0xffff));
      sel.curr.execWidth = 8;
      // Set zero LOD.
      if (simdWidth == 8)
        sel.MOV(msgs[4], GenRegister::immud(0));
      else
        sel.MOV(GenRegister::Qn(msgs[2], 0), GenRegister::immud(0));
      sel.pop();

      uint32_t bti = insn.getImageIndex();
      if (simdWidth == 8)
        sel.TYPED_WRITE(msgs, msgNum, bti, dim == 3);
      else {
        sel.push();
        sel.curr.execWidth = 8;
        for( uint32_t quarter = 0; quarter < 2; quarter++)
        {
          #define QUARTER_MOV0(msgs, msgid, src) \
                    sel.MOV(GenRegister::Qn(GenRegister::retype(msgs[msgid/2], GEN_TYPE_UD), msgid % 2), \
                            GenRegister::Qn(src, quarter))

          #define QUARTER_MOV1(msgs, msgid, src) \
                  sel.MOV(GenRegister::Qn(GenRegister::retype(msgs[msgid/2], src.type), msgid % 2), \
                          GenRegister::Qn(src, quarter))
          sel.curr.quarterControl = (quarter == 0) ? GEN_COMPRESSION_Q1 : GEN_COMPRESSION_Q2;
          // Set U,V,W
          QUARTER_MOV0(msgs, 1, sel.selReg(insn.getSrc(0), insn.getCoordType()));
          if (dim > 1)
            QUARTER_MOV0(msgs, 2, sel.selReg(insn.getSrc(1), insn.getCoordType()));
          if (dim > 2)
            QUARTER_MOV0(msgs, 3, sel.selReg(insn.getSrc(2), insn.getCoordType()));
          // Set R, G, B, A
          QUARTER_MOV1(msgs, 5, sel.selReg(insn.getSrc(dim), insn.getSrcType()));
          QUARTER_MOV1(msgs, 6, sel.selReg(insn.getSrc(dim + 1), insn.getSrcType()));
          QUARTER_MOV1(msgs, 7, sel.selReg(insn.getSrc(dim + 2), insn.getSrcType()));
          QUARTER_MOV1(msgs, 8, sel.selReg(insn.getSrc(dim + 3), insn.getSrcType()));
          sel.TYPED_WRITE(msgs, msgNum, bti, dim == 3);
          #undef QUARTER_MOV0
          #undef QUARTER_MOV1
        }
        sel.pop();
      }
      return true;
    }
    DECL_CTOR(TypedWriteInstruction, 1, 1);
  };

  /*! get image info instruction pattern. */
  DECL_PATTERN(GetImageInfoInstruction)
  {
    INLINE bool emitOne(Selection::Opaque &sel, const ir::GetImageInfoInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      GenRegister dst;
      dst = sel.selReg(insn.getDst(0), TYPE_U32);
      GenRegister imageInfoReg = GenRegister::ud1grf(insn.getSrc(0));
      sel.MOV(dst, imageInfoReg);

      return true;
    }
    DECL_CTOR(GetImageInfoInstruction, 1, 1);
  };

  class ReadARFInstructionPattern : public SelectionPattern
  {
  public:
    ReadARFInstructionPattern(void) : SelectionPattern(1,1) {
      this->opcodes.push_back(ir::OP_READ_ARF);
    }

    INLINE uint32_t getRegNum(ir::ARFRegister arf) const {
      if (arf == ir::ARF_TM) {
        return 0xc0;
      } else {
        GBE_ASSERT(0);
        return 0;
      }
    }

    INLINE bool emit(Selection::Opaque &sel, SelectionDAG &dag) const {
      using namespace ir;
      const ir::ReadARFInstruction &insn = cast<ir::ReadARFInstruction>(dag.insn);
      GenRegister dst;
      dst = sel.selReg(insn.getDst(0), insn.getType());

      sel.push();
        sel.curr.predicate = GEN_PREDICATE_NONE;
        sel.curr.noMask = 1;
        sel.curr.execWidth = 8;
        sel.READ_ARF(dst, GenRegister(GEN_ARCHITECTURE_REGISTER_FILE,
                      getRegNum(insn.getARFRegister()),
                      0,
                      getGenType(insn.getType()),
                      GEN_VERTICAL_STRIDE_8,
                      GEN_WIDTH_8,
                      GEN_HORIZONTAL_STRIDE_1));
      sel.pop();
      return true;
    }
  };

  class SimdShuffleInstructionPattern : public SelectionPattern
  {
  public:
    SimdShuffleInstructionPattern(void) : SelectionPattern(1,1) {
      this->opcodes.push_back(ir::OP_SIMD_SHUFFLE);
    }
    INLINE bool emit(Selection::Opaque &sel, SelectionDAG &dag) const {
      using namespace ir;
      const ir::SimdShuffleInstruction &insn = cast<SimdShuffleInstruction>(dag.insn);
      assert(insn.getOpcode() == OP_SIMD_SHUFFLE);
      const Type type = insn.getType();
      GenRegister dst  = sel.selReg(insn.getDst(0), type);
      GenRegister src0  = sel.selReg(insn.getSrc(0), type);
      GenRegister src1;

      SelectionDAG *dag0 = dag.child[0];
      SelectionDAG *dag1 = dag.child[1];
      if (dag1 != NULL && dag1->insn.getOpcode() == OP_LOADI && canGetRegisterFromImmediate(dag1->insn)) {
        const auto &childInsn = cast<LoadImmInstruction>(dag1->insn);
        src1 = getRegisterFromImmediate(childInsn.getImmediate(), TYPE_U32);
        if (dag0) dag0->isRoot = 1;
      } else {
        markAllChildren(dag);
        src1 = sel.selReg(insn.getSrc(1), TYPE_U32);
      }

      sel.push();
      if (sel.isScalarReg(insn.getSrc(0))) {
        if (sel.isScalarReg(insn.getDst(0))) {
          sel.curr.execWidth = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
        }
        sel.MOV(dst, src0);     //no matter what src1 is
      } else {
        if (src1.file == GEN_IMMEDIATE_VALUE) {
          uint32_t offset = src1.value.ud % sel.curr.execWidth;
          GenRegister reg = GenRegister::subphysicaloffset(src0, offset);
          reg.vstride = GEN_VERTICAL_STRIDE_0;
          reg.hstride = GEN_HORIZONTAL_STRIDE_0;
          reg.width = GEN_WIDTH_1;
          sel.MOV(dst, reg);
        }
        else {
          GenRegister shiftL = sel.selReg(sel.reg(FAMILY_DWORD), TYPE_U32);
          sel.SHL(shiftL, src1, GenRegister::immud(0x2));
          sel.SIMD_SHUFFLE(dst, src0, shiftL);
        }
      }
      sel.pop();
      return true;
    }

  };

  /*! Get a region of a register */
  class RegionInstructionPattern : public SelectionPattern
  {
  public:
    RegionInstructionPattern(void) : SelectionPattern(1,1) {
      this->opcodes.push_back(ir::OP_REGION);
    }
    INLINE bool emit(Selection::Opaque &sel, SelectionDAG &dag) const {
      using namespace ir;
      const ir::RegionInstruction &insn = cast<ir::RegionInstruction>(dag.insn);
      GenRegister dst, src;
      dst = sel.selReg(insn.getDst(0), ir::TYPE_U32);
      src = GenRegister::ud1grf(insn.getSrc(0));
      src = sel.getOffsetReg(src, 0, insn.getOffset()*4);

      sel.push();
        sel.curr.noMask = 1;
        sel.curr.predicate = GEN_PREDICATE_NONE;
        sel.MOV(dst, src);
      sel.pop();
      markAllChildren(dag);
      return true;
    }
  };

  /*! Get a region of a register */
  class IndirectMovInstructionPattern : public SelectionPattern
  {
  public:
    IndirectMovInstructionPattern(void) : SelectionPattern(1,1) {
      this->opcodes.push_back(ir::OP_INDIRECT_MOV);
    }
    INLINE bool emit(Selection::Opaque &sel, SelectionDAG &dag) const {
      using namespace ir;
      const ir::IndirectMovInstruction &insn = cast<ir::IndirectMovInstruction>(dag.insn);
      GenRegister dst, src0, src1;
      uint32_t offset = insn.getOffset();
      dst = sel.selReg(insn.getDst(0), insn.getType());
      src0 = sel.selReg(insn.getSrc(0), TYPE_U32);
      src1 = sel.selReg(insn.getSrc(1), TYPE_U32);
      GenRegister tmp = sel.selReg(sel.reg(FAMILY_WORD), TYPE_U16);

      sel.INDIRECT_MOVE(dst, tmp, src0, src1, offset);
      markAllChildren(dag);
      return true;
    }
  };

  class CalcTimestampInstructionPattern : public SelectionPattern
  {
  public:
    CalcTimestampInstructionPattern(void) : SelectionPattern(1,1) {
      this->opcodes.push_back(ir::OP_CALC_TIMESTAMP);
    }
    INLINE bool emit(Selection::Opaque &sel, SelectionDAG &dag) const {
      using namespace ir;
      const ir::CalcTimestampInstruction &insn = cast<ir::CalcTimestampInstruction>(dag.insn);
      uint32_t pointNum = insn.getPointNum();
      uint32_t tsType = insn.getTimestamptType();
      GBE_ASSERT(sel.ctx.getSimdWidth() == 16 || sel.ctx.getSimdWidth() == 8);
      GenRegister tmp;
      GenRegister ts[5];
      int tsNum;
      if (sel.ctx.getSimdWidth() == 16) {
        if (!sel.hasLongType())
          tmp = GenRegister::retype(sel.selReg(sel.reg(FAMILY_WORD)), GEN_TYPE_UD);
        ts[0] = GenRegister::retype(sel.selReg(ir::ocl::profilingts0, ir::TYPE_U32), GEN_TYPE_UD);
        ts[1] = GenRegister::retype(sel.selReg(ir::ocl::profilingts1, ir::TYPE_U32), GEN_TYPE_UD);
        ts[2] = GenRegister::retype(sel.selReg(ir::ocl::profilingts2, ir::TYPE_U32), GEN_TYPE_UW);
        tsNum = 3;
      } else {
        if (!sel.hasLongType())
          tmp = GenRegister::retype(sel.selReg(sel.reg(FAMILY_DWORD)), GEN_TYPE_UD);
        ts[0] = GenRegister::retype(sel.selReg(ir::ocl::profilingts0, ir::TYPE_U32), GEN_TYPE_UD);
        ts[1] = GenRegister::retype(sel.selReg(ir::ocl::profilingts1, ir::TYPE_U32), GEN_TYPE_UD);
        ts[2] = GenRegister::retype(sel.selReg(ir::ocl::profilingts2, ir::TYPE_U32), GEN_TYPE_UD);
        ts[3] = GenRegister::retype(sel.selReg(ir::ocl::profilingts3, ir::TYPE_U32), GEN_TYPE_UD);
        ts[4] = GenRegister::retype(sel.selReg(ir::ocl::profilingts4, ir::TYPE_U32), GEN_TYPE_UD);
        tsNum = 5;
      }

      sel.push(); {
        sel.curr.flag = 0;
        sel.curr.subFlag = 1;
        sel.CALC_TIMESTAMP(ts, tsNum, tmp, pointNum, tsType);
      } sel.pop();
      markAllChildren(dag);
      return true;
    }
  };

  class StoreProfilingInstructionPattern : public SelectionPattern
  {
  public:
    StoreProfilingInstructionPattern(void) : SelectionPattern(1,1) {
      this->opcodes.push_back(ir::OP_STORE_PROFILING);
    }
    INLINE bool emit(Selection::Opaque &sel, SelectionDAG &dag) const {
      using namespace ir;
      const ir::StoreProfilingInstruction &insn = cast<ir::StoreProfilingInstruction>(dag.insn);
      uint32_t profilingType = insn.getProfilingType();
      uint32_t BTI = insn.getBTI();
      GBE_ASSERT(sel.ctx.getSimdWidth() == 16 || sel.ctx.getSimdWidth() == 8);
      GenRegister tmp0;
      GenRegister tmp1;
      GenRegister ts[5];
      int tsNum;
      if (sel.ctx.getSimdWidth() == 16) {
        tmp0 = GenRegister::retype(sel.selReg(sel.reg(FAMILY_DWORD)), GEN_TYPE_UD);
        ts[0] = GenRegister::retype(sel.selReg(ir::ocl::profilingts0, ir::TYPE_U32), GEN_TYPE_UD);
        ts[1] = GenRegister::retype(sel.selReg(ir::ocl::profilingts1, ir::TYPE_U32), GEN_TYPE_UD);
        ts[2] = GenRegister::retype(sel.selReg(ir::ocl::profilingts2, ir::TYPE_U32), GEN_TYPE_UW);
        tsNum = 3;
      } else {
        tmp0 = GenRegister::retype(sel.selReg(sel.reg(FAMILY_DWORD)), GEN_TYPE_UD);
        tmp1 = GenRegister::retype(sel.selReg(sel.reg(FAMILY_DWORD)), GEN_TYPE_UD);
        ts[0] = GenRegister::retype(sel.selReg(ir::ocl::profilingts0, ir::TYPE_U32), GEN_TYPE_UD);
        ts[1] = GenRegister::retype(sel.selReg(ir::ocl::profilingts1, ir::TYPE_U32), GEN_TYPE_UD);
        ts[2] = GenRegister::retype(sel.selReg(ir::ocl::profilingts2, ir::TYPE_U32), GEN_TYPE_UD);
        ts[3] = GenRegister::retype(sel.selReg(ir::ocl::profilingts3, ir::TYPE_U32), GEN_TYPE_UD);
        ts[4] = GenRegister::retype(sel.selReg(ir::ocl::profilingts4, ir::TYPE_U32), GEN_TYPE_UD);
        tsNum = 5;
      }
      sel.push(); {
        sel.curr.flag = 0;
        sel.curr.subFlag = 1;
        sel.STORE_PROFILING(profilingType, BTI, tmp0, tmp1, ts, tsNum);
      } sel.pop();
      markAllChildren(dag);
      return true;
    }
  };

  /*! Branch instruction pattern */
  class BranchInstructionPattern : public SelectionPattern
  {
  public:
    BranchInstructionPattern(void) : SelectionPattern(1,1) {
      for (uint32_t op = 0; op < ir::OP_INVALID; ++op)
        if (ir::isOpcodeFrom<ir::BranchInstruction>(ir::Opcode(op)) == true)
          this->opcodes.push_back(ir::Opcode(op));
    }

    void emitForwardBranch(Selection::Opaque &sel,
                           const ir::BranchInstruction &insn,
                           ir::LabelIndex dst,
                           ir::LabelIndex src) const
    {
      using namespace ir;
      const GenRegister ip = sel.getBlockIP();

      // We will not emit any jump if we must go the next block anyway
      const BasicBlock *curr = insn.getParent();
      const BasicBlock *next = curr->getNextBlock();
      const LabelIndex nextLabel = next->getLabelIndex();
      if (insn.isPredicated() == true) {
        const Register pred = insn.getPredicateIndex();
        sel.push();
          // we don't need to set next label to the pcip
          // as if there is no backward jump latter, then obviously everything will work fine.
          // If there is backward jump latter, then all the pcip will be updated correctly there.
          sel.curr.physicalFlag = 0;
          sel.curr.flagIndex = pred.value();
          sel.curr.predicate = GEN_PREDICATE_NORMAL;
          sel.setBlockIP(ip, dst.value());
          sel.curr.predicate = GEN_PREDICATE_NONE;
          if (!sel.block->hasBarrier && !sel.block->removeSimpleIfEndif)
            sel.ENDIF(GenRegister::immd(0), nextLabel);
          sel.block->endifOffset = -1;
        sel.pop();
      } else {
        // Update the PcIPs
        const LabelIndex jip = sel.ctx.getLabelIndex(&insn);
        if(insn.getParent()->needEndif)
          sel.setBlockIP(ip, dst.value());

        if (!sel.block->hasBarrier && !sel.block->removeSimpleIfEndif) {
          if(insn.getParent()->needEndif && !insn.getParent()->needIf)
            sel.ENDIF(GenRegister::immd(0), insn.getParent()->endifLabel, insn.getParent()->endifLabel);
          else if(insn.getParent()->needEndif)
            sel.ENDIF(GenRegister::immd(0), nextLabel);
        }
        sel.block->endifOffset = -1;
        if (nextLabel == jip) return;
        // Branch to the jump target
        sel.push();
          sel.curr.execWidth = 1;
          sel.curr.noMask = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          // Actually, the origin of this JMPI should be the beginning of next BB.
          sel.block->endifOffset -= sel.JMPI(GenRegister::immd(0), jip, ir::LabelIndex(curr->getLabelIndex().value() + 1));
        sel.pop();
      }
    }

    void emitBackwardBranch(Selection::Opaque &sel,
                            const ir::BranchInstruction &insn,
                            ir::LabelIndex dst,
                            ir::LabelIndex src) const
    {
      using namespace ir;
      //const GenRegister ip = sel.selReg(ocl::blockip, TYPE_U16);
      const GenRegister ip = sel.getBlockIP();
      const Function &fn = sel.ctx.getFunction();
      const BasicBlock &bb = fn.getBlock(src);
      const LabelIndex jip = sel.ctx.getLabelIndex(&insn);
      const LabelIndex label = bb.getLabelIndex();
      const uint32_t simdWidth = sel.ctx.getSimdWidth();
      GBE_ASSERT(bb.getNextBlock() != NULL);

      if (insn.isPredicated() == true) {
        const Register pred = insn.getPredicateIndex();

        // Update the PcIPs for all the branches. Just put the IPs of the next
        // block. Next instruction will properly update the IPs of the lanes
        // that actually take the branch
        const LabelIndex next = bb.getNextBlock()->getLabelIndex();
        sel.setBlockIP(ip, next.value());
        GBE_ASSERT(jip == dst);
        sel.push();
          sel.curr.physicalFlag = 0;
          sel.curr.flagIndex = pred.value();
          sel.curr.predicate = GEN_PREDICATE_NORMAL;
          sel.setBlockIP(ip, dst.value());
          sel.block->endifOffset = -1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          if (!sel.block->hasBarrier && !sel.block->removeSimpleIfEndif)
            sel.ENDIF(GenRegister::immd(0), next);
          sel.curr.execWidth = 1;
          if (simdWidth == 16)
            sel.curr.predicate = GEN_PREDICATE_ALIGN1_ANY16H;
          else
            sel.curr.predicate = GEN_PREDICATE_ALIGN1_ANY8H;
          sel.curr.noMask = 1;
          sel.block->endifOffset -= sel.JMPI(GenRegister::immd(0), jip, label);
        sel.pop();
      } else {
        const LabelIndex next = bb.getNextBlock()->getLabelIndex();
        // Update the PcIPs
        if(insn.getParent()->needEndif)
        sel.setBlockIP(ip, dst.value());
        sel.block->endifOffset = -1;
        if (!sel.block->hasBarrier && !sel.block->removeSimpleIfEndif) {
          if(insn.getParent()->needEndif && !insn.getParent()->needIf)
            sel.ENDIF(GenRegister::immd(0), insn.getParent()->endifLabel, insn.getParent()->endifLabel);
          else if(insn.getParent()->needEndif)
            sel.ENDIF(GenRegister::immd(0), next);
        }
        // Branch to the jump target
        sel.push();
          sel.curr.execWidth = 1;
          sel.curr.noMask = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.block->endifOffset -= sel.JMPI(GenRegister::immd(0), jip, label);
        sel.pop();
      }
    }

    INLINE bool emit(Selection::Opaque &sel, SelectionDAG &dag) const {
      using namespace ir;
      const ir::BranchInstruction &insn = cast<BranchInstruction>(dag.insn);
      const Opcode opcode = insn.getOpcode();
      if (opcode == OP_RET)
        sel.EOT();
      else if (opcode == OP_BRA) {
        const LabelIndex dst = insn.getLabelIndex();
        const LabelIndex src = insn.getParent()->getLabelIndex();

        sel.push();
        if (insn.isPredicated() == true) {
          if (dag.child[0] == NULL)
            sel.curr.externFlag = 1;
        }

        // We handle foward and backward branches differently
        if (uint32_t(dst) <= uint32_t(src))
          this->emitBackwardBranch(sel, insn, dst, src);
        else
          this->emitForwardBranch(sel, insn, dst, src);
        sel.pop();
      }
      else if(opcode == OP_IF) {
        const Register pred = insn.getPredicateIndex();
        const LabelIndex jip = insn.getLabelIndex();
        LabelIndex uip;
        if(insn.getParent()->matchingEndifLabel != 0)
          uip = insn.getParent()->matchingEndifLabel;
        else
          uip = jip;
        sel.push();
          sel.curr.physicalFlag = 0;
          sel.curr.flagIndex = (uint64_t)pred;
          sel.curr.externFlag = 1;
          sel.curr.inversePredicate = insn.getInversePredicated();
          sel.curr.predicate = GEN_PREDICATE_NORMAL;
          sel.IF(GenRegister::immd(0), jip, uip);
          sel.curr.inversePredicate = 0;
        sel.pop();
      } else if(opcode == OP_ENDIF) {
        const LabelIndex label = insn.getLabelIndex();
        sel.push();
          sel.curr.noMask = 1;
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.ENDIF(GenRegister::immd(0), label, label);
        sel.pop();
      } else if(opcode == OP_ELSE) {
        const LabelIndex label = insn.getLabelIndex();
        sel.ELSE(GenRegister::immd(0), label, insn.getParent()->thisElseLabel);
      } else if(opcode == OP_WHILE) {
        const Register pred = insn.getPredicateIndex();
        const LabelIndex jip = insn.getLabelIndex();
        sel.push();
          sel.curr.physicalFlag = 0;
          sel.curr.flagIndex = (uint64_t)pred;
          sel.curr.externFlag = 1;
          sel.curr.inversePredicate = insn.getInversePredicated();
          sel.curr.predicate = GEN_PREDICATE_NORMAL;
          sel.WHILE(GenRegister::immd(0), jip);
          sel.curr.inversePredicate = 0;
        sel.pop();
      } else
        NOT_IMPLEMENTED;

      markAllChildren(dag);
      return true;
    }

  };

  /*! WorkGroup instruction pattern */
  DECL_PATTERN(WorkGroupInstruction)
  {
    INLINE bool storeThreadID(Selection::Opaque &sel, uint32_t slmAddr) const
    {
      using namespace ir;
      GenRegister sr0_0 = GenRegister::retype(GenRegister::sr(0), GEN_TYPE_UW);
      const uint32_t simdWidth = sel.ctx.getSimdWidth();
      GenRegister tmp;
      GenRegister addr;
      vector<GenRegister> fakeTemps;

      if (simdWidth == 16) {
        tmp = GenRegister::retype(sel.selReg(sel.reg(FAMILY_WORD), ir::TYPE_U16), GEN_TYPE_UD);
        addr = GenRegister::retype(sel.selReg(sel.reg(FAMILY_WORD), ir::TYPE_U16), GEN_TYPE_UD);
      } else {
        tmp = GenRegister::retype(sel.selReg(sel.reg(FAMILY_DWORD), ir::TYPE_U32), GEN_TYPE_UD);
        addr = GenRegister::retype(sel.selReg(sel.reg(FAMILY_DWORD), ir::TYPE_U32), GEN_TYPE_UD);
      }

      sr0_0 = GenRegister::vec1(sr0_0);
      sel.push(); {
        sel.curr.predicate = GEN_PREDICATE_NONE;
        sel.curr.noMask = 1;
        sel.curr.execWidth = 8;

        sel.MOV(tmp, sr0_0);

        sel.MUL(addr, sel.selReg(ocl::threadid, ir::TYPE_U32), GenRegister::immud(2));
        sel.ADD(addr, addr, GenRegister::immud(slmAddr));

        sel.push(); {
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
          sel.push(); {
            sel.curr.execWidth = 1;
            sel.MOV(GenRegister::flag(0, 1), GenRegister::immuw(0x01));
          } sel.pop();
          sel.curr.flag = 0;
          sel.curr.subFlag = 1;
          sel.curr.predicate = GEN_PREDICATE_NORMAL;
          sel.BYTE_SCATTER(addr, tmp, 1, GenRegister::immw(0xfe), fakeTemps);
        } sel.pop();
      } sel.pop();
      return true;
    }

    INLINE GenRegister getNextThreadID(Selection::Opaque &sel, uint32_t slmAddr) const
    {
      using namespace ir;
      const uint32_t simdWidth = sel.ctx.getSimdWidth();
      GenRegister addr;
      GenRegister nextThread;
      GenRegister tid;
      vector<GenRegister> fakeTemps;

      if (simdWidth == 16) {
        addr = GenRegister::retype(sel.selReg(sel.reg(FAMILY_WORD), ir::TYPE_U16), GEN_TYPE_UD);
        nextThread = GenRegister::retype(sel.selReg(sel.reg(FAMILY_WORD), ir::TYPE_U16), GEN_TYPE_UD);
        tid = GenRegister::retype(sel.selReg(sel.reg(FAMILY_WORD), ir::TYPE_U16), GEN_TYPE_UD);
      } else {
        addr = sel.selReg(sel.reg(FAMILY_DWORD), ir::TYPE_U32);
        nextThread = sel.selReg(sel.reg(FAMILY_DWORD), ir::TYPE_U32);
        tid = sel.selReg(sel.reg(FAMILY_DWORD), ir::TYPE_U32);
      }

      sel.push(); {
        sel.curr.execWidth = 8;
        sel.curr.predicate = GEN_PREDICATE_NONE;
        sel.curr.noMask = 1;
        sel.ADD(nextThread, sel.selReg(ocl::threadid, ir::TYPE_U32), GenRegister::immud(1));

        /* Wrap the next thread id. */
        sel.push(); {
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
          sel.curr.flag = 0;
          sel.curr.subFlag = 1;
          sel.CMP(GEN_CONDITIONAL_EQ, nextThread, sel.selReg(ocl::threadn, ir::TYPE_U32), GenRegister::null());
          sel.curr.predicate = GEN_PREDICATE_NORMAL;
          sel.MOV(nextThread, GenRegister::immud(0));
        } sel.pop();

        sel.MUL(addr, nextThread, GenRegister::immud(2));
        sel.ADD(addr, addr, GenRegister::immud(slmAddr));

        sel.push(); {
          sel.curr.predicate = GEN_PREDICATE_NONE;
          sel.curr.noMask = 1;
          sel.push(); {
            sel.curr.execWidth = 1;
            sel.MOV(GenRegister::flag(0, 1), GenRegister::immuw(0x010));
          } sel.pop();
          sel.curr.flag = 0;
          sel.curr.subFlag = 1;
          sel.curr.predicate = GEN_PREDICATE_NORMAL;
          sel.BYTE_GATHER(tid, addr, 1, GenRegister::immw(0xfe), fakeTemps);
        } sel.pop();

      } sel.pop();
      return tid;
    }

    INLINE bool emitWGBroadcast(Selection::Opaque &sel, const ir::WorkGroupInstruction &insn) const {
      /*  1. BARRIER    Ensure all the threads have set the correct value for the var which will be broadcasted.
          2. CMP IDs    Compare the local IDs with the specified ones in the function call.
          3. STORE	   Use flag to control the store of the var. Only the specified item will execute the store.
          4. BARRIER    Ensure the specified value has been stored.
          5. LOAD	   Load the stored value to all the dst value, the dst of all the items will have same value,
          so broadcasted.	*/
      using namespace ir;
      const Type type = insn.getType();
      const GenRegister src = sel.selReg(insn.getSrc(0), type);
      const GenRegister dst = sel.selReg(insn.getDst(0), type);
      const uint32_t srcNum = insn.getSrcNum();
      const uint32_t slmAddr = insn.getSlmAddr();
      GenRegister addr = sel.selReg(sel.reg(FAMILY_DWORD), ir::TYPE_U32);
      vector<GenRegister> fakeTemps;

      /* Then we insert a barrier to make sure all the var we are interested in
         have been assigned the final value. */
      sel.BARRIER(GenRegister::ud8grf(sel.reg(FAMILY_DWORD)), sel.selReg(sel.reg(FAMILY_DWORD)), syncLocalBarrier);

      GBE_ASSERT(srcNum >= 2);
      GenRegister coords[3];
      for (uint32_t i = 1; i < srcNum; i++) {
        coords[i - 1] = sel.selReg(insn.getSrc(i), TYPE_U32);
      }

      sel.push(); {
        sel.curr.predicate = GEN_PREDICATE_NONE;
        sel.curr.noMask = 1;
        sel.MOV(addr, GenRegister::immud(slmAddr));
      } sel.pop();

      sel.push(); {
        sel.curr.flag = 0;
        sel.curr.subFlag = 1;
        sel.curr.predicate = GEN_PREDICATE_NONE;
        sel.curr.noMask = 1;
        GenRegister lid0, lid1, lid2;
        uint32_t dim = srcNum - 1;
        lid0 = sel.selReg(ir::ocl::lid0);
        lid1 = sel.selReg(ir::ocl::lid1);
        lid2 = sel.selReg(ir::ocl::lid2);

        sel.CMP(GEN_CONDITIONAL_EQ, coords[0], lid0, GenRegister::retype(GenRegister::null(), GEN_TYPE_UD));
        sel.curr.predicate = GEN_PREDICATE_NORMAL;
        if (dim >= 2)
          sel.CMP(GEN_CONDITIONAL_EQ, coords[1], lid1, GenRegister::retype(GenRegister::null(), GEN_TYPE_UD));
        if (dim >= 3)
          sel.CMP(GEN_CONDITIONAL_EQ, coords[2], lid2, GenRegister::retype(GenRegister::null(), GEN_TYPE_UD));

        if (typeSize(src.type) == 4) {
          GenRegister _addr = GenRegister::retype(addr, GEN_TYPE_F);
          GenRegister _src = GenRegister::retype(src, GEN_TYPE_F);
          sel.UNTYPED_WRITE(_addr, &_src, 1, GenRegister::immw(0xfe), fakeTemps);
        }
      } sel.pop();
      /* Make sure the slm var have the valid value now */
      sel.BARRIER(GenRegister::ud8grf(sel.reg(FAMILY_DWORD)), sel.selReg(sel.reg(FAMILY_DWORD)), syncLocalBarrier);

      if (typeSize(src.type) == 4) {
        sel.UNTYPED_READ(addr, &dst, 1, GenRegister::immw(0xfe), fakeTemps);
      }

      return true;
    }

    INLINE bool emitOne(Selection::Opaque &sel, const ir::WorkGroupInstruction &insn, bool &markChildren) const
    {
      using namespace ir;
      const WorkGroupOps workGroupOp = insn.getWorkGroupOpcode();

      if (workGroupOp == WORKGROUP_OP_BROADCAST) {
        return emitWGBroadcast(sel, insn);
      } else if (workGroupOp >= WORKGROUP_OP_REDUCE_ADD && workGroupOp <= WORKGROUP_OP_EXCLUSIVE_MAX) {
        const uint32_t slmAddr = insn.getSlmAddr();
        /* First, we create the TheadID/localID map, in order to get which thread hold the next 16 workitems. */

        if (!sel.storeThreadMap) {
          this->storeThreadID(sel, slmAddr);
          sel.storeThreadMap = true;
        }

        /* Then we insert a barrier to make sure all the var we are interested in
           have been assigned the final value. */
        sel.BARRIER(GenRegister::ud8grf(sel.reg(FAMILY_DWORD)), sel.selReg(sel.reg(FAMILY_DWORD)), syncLocalBarrier);

        /* Third, get the next thread ID which we will Forward MSG to. */
        GenRegister nextThreadID = getNextThreadID(sel, slmAddr);
        GenRegister threadID = sel.selReg(ocl::threadid, ir::TYPE_U32);
        GenRegister threadNum = sel.selReg(ocl::threadn, ir::TYPE_U32);
        GenRegister tmp = GenRegister::retype(sel.selReg(sel.reg(FAMILY_DWORD)), GEN_TYPE_UD);

        const Type type = insn.getType();
        const GenRegister dst = sel.selReg(insn.getDst(0), type);
        const uint32_t srcNum = insn.getSrcNum();
        GBE_ASSERT(srcNum == 3);
        GBE_ASSERT(insn.getSrc(0) == ir::ocl::threadn);
        GBE_ASSERT(insn.getSrc(1) == ir::ocl::threadid);
        GenRegister src = sel.selReg(insn.getSrc(2), type);
        sel.push(); {
          sel.curr.flag = 0;
          sel.curr.subFlag = 1;
          sel.WORKGROUP_OP(workGroupOp, dst, src, nextThreadID, threadID, threadNum, tmp);
        } sel.pop();
      } else {
        GBE_ASSERT(0);
      }

      return true;
    }
    DECL_CTOR(WorkGroupInstruction, 1, 1);
  };

  /*! Sort patterns */
  INLINE bool cmp(const SelectionPattern *p0, const SelectionPattern *p1) {
    if (p0->insnNum != p1->insnNum)
      return p0->insnNum > p1->insnNum;
    return p0->cost < p1->cost;
  }

  SelectionLibrary::SelectionLibrary(void) {
    this->insert<UnaryInstructionPattern>();
    this->insert<BinaryInstructionPattern>();
    this->insert<TypedWriteInstructionPattern>();
    this->insert<SyncInstructionPattern>();
    this->insert<LoadImmInstructionPattern>();
    this->insert<LoadInstructionPattern>();
    this->insert<StoreInstructionPattern>();
    this->insert<SelectInstructionPattern>();
    this->insert<CompareInstructionPattern>();
    this->insert<BitCastInstructionPattern>();
    this->insert<ConvertInstructionPattern>();
    this->insert<AtomicInstructionPattern>();
    this->insert<TernaryInstructionPattern>();
    this->insert<LabelInstructionPattern>();
    this->insert<BranchInstructionPattern>();
    this->insert<Int32x32MulInstructionPattern>();
    this->insert<Int32x16MulInstructionPattern>();
    this->insert<MulAddInstructionPattern>();
    this->insert<SelectModifierInstructionPattern>();
    this->insert<SampleInstructionPattern>();
    this->insert<VmeInstructionPattern>();
    this->insert<GetImageInfoInstructionPattern>();
    this->insert<ReadARFInstructionPattern>();
    this->insert<RegionInstructionPattern>();
    this->insert<SimdShuffleInstructionPattern>();
    this->insert<IndirectMovInstructionPattern>();
    this->insert<CalcTimestampInstructionPattern>();
    this->insert<StoreProfilingInstructionPattern>();
    this->insert<WorkGroupInstructionPattern>();
    this->insert<NullaryInstructionPattern>();
    this->insert<WaitInstructionPattern>();

    // Sort all the patterns with the number of instructions they output
    for (uint32_t op = 0; op < ir::OP_INVALID; ++op)
      std::sort(this->patterns[op].begin(), this->patterns[op].end(), cmp);
  }

  SelectionLibrary::~SelectionLibrary(void) {
    for (auto pattern : this->toFree)
      GBE_DELETE(const_cast<SelectionPattern*>(pattern));
  }

  template <typename PatternType>
  void SelectionLibrary::insert(void) {
    const SelectionPattern *pattern = GBE_NEW_NO_ARG(PatternType);
    this->toFree.push_back(pattern);
    for (auto opcode : pattern->opcodes)
      this->patterns[opcode].push_back(pattern);
  }

} /* namespace gbe */

