#ifndef _CBASIC_BLOCK_H_

#define _CBASIC_BLOCK_H_

#include <llvm/Function.h>


namespace s2etools {
namespace translator {

enum EBasicBlockType
{
    BB_DEFAULT=0,
    BB_JMP, BB_JMP_IND,
    BB_COND_JMP, BB_COND_JMP_IND,
    BB_CALL, BB_CALL_IND, BB_REP, BB_RET
};

class CBasicBlock {
public:
    //List of basic block addresses that follow this block
    typedef std::vector<uint64_t> Successors;

private:
    llvm::Function *m_function;
    uint64_t m_address;
    unsigned m_size;
    EBasicBlockType m_type;
    Successors m_successors;

    void markInstructionBoundaries();
    void markCallInstruction();
    void computeSuccessors();

public:
    CBasicBlock(llvm::Function *f, uint64_t va, unsigned size, EBasicBlockType type);
    ~CBasicBlock();

    const llvm::Function *getFunction() const {
        return m_function;
    }

    EBasicBlockType getType() const {
        return m_type;
    }

    const Successors& getSuccessors() const {
        return m_successors;
    }

};

}
}

#endif
