#include "bytecode.h"

// Implementation of the Slang bytecode (BC)
// (most notably including conversion from IR to BC)

#include "compiler.h"
#include "ir.h"
#include "ir-insts.h"
#include "lower-to-ir.h"

namespace Slang
{
struct SharedBytecodeGenerationContext;

// Representation of a `BCPtr<T>` during actual bytecode generation.
// This representation is to deal with the fact that the actual
// storage for the bytecode data might get reallocated during emission
// so that we need to be careful and not work with raw `BCPtr<T>`.
template<typename T>
struct BytecodeGenerationPtr
{
    UInt                                offset;
    SharedBytecodeGenerationContext*    sharedContext;

    BytecodeGenerationPtr()
        : sharedContext(nullptr)
        , offset(0)
    {}


    BytecodeGenerationPtr(
        SharedBytecodeGenerationContext*    sharedContext,
        UInt                                offset)
        : sharedContext(sharedContext)
        , offset(offset)
    {}

    BytecodeGenerationPtr(
        BytecodeGenerationPtr<T> const& ptr)
        : sharedContext(ptr.sharedContext)
        , offset(ptr.offset)
    {}

    template<typename U>
    BytecodeGenerationPtr(
        BytecodeGenerationPtr<U> const& ptr,
        typename EnableIf<IsConvertible<T*, U*>::Value, void>::type * = 0)
        : sharedContext(ptr.sharedContext)
        , offset(ptr.offset)
    {}

    template<typename U>
    BytecodeGenerationPtr<U> bitCast() const
    {
        return BytecodeGenerationPtr<U>(sharedContext, offset);
    }

    operator BCPtr<T>() const
    {
        return BCPtr<T>(getPtr());
    }

    T* operator->() const
    {
        return getPtr();
    }

    T& operator*() const
    {
        return *getPtr();
    }

    T& operator[](UInt index) const
    {
        return getPtr()[index];
    }

    BytecodeGenerationPtr<T> operator+(Int index) const
    {
        Int delta = index * sizeof(T);
        UInt newOffset = offset + delta;
        return BytecodeGenerationPtr<T>(
            sharedContext,
            newOffset);
    }

    T* getPtr() const;
};

#if 0
template<typename T>
void BCPtr<T>::operator=(BytecodeGenerationPtr<T> const& ptr)
{
    fprintf(stderr, "0x%p: operator=BGP 0x%p\n", this, ptr.getPtr());
    *this = ptr.getPtr();
}
#endif

struct SharedBytecodeGenerationContext
{
    // The final generated bytecode stream
    List<uint8_t>   bytecode;

    // Map from an IR value to a global entity
    // that encodes it:
    Dictionary<IRInst*, BCConst> mapValueToGlobal;

    // Types that have been emitted
    List<BytecodeGenerationPtr<BCType>> bcTypes;
    Dictionary<Type*, UInt> mapTypeToID;

    // Compile-time constant values that need
    // to be emitted...
    List<IRInst*>  constants;
};

struct BytecodeGenerationContext
{
    SharedBytecodeGenerationContext*    shared;

    // The bytecode of the current symbol being
    // output.
    List<uint8_t>   currentBytecode;

    // The function that is in scope for this context
    IRFunc* currentIRFunc;

    // Counter for global symbols that have been assigned
    // so that they can be used by this function
    List<BCConst>  remappedGlobalSymbols;

    // Map an instruction to its ID for use local
    // to the current context
    Dictionary<IRInst*, Int> mapInstToLocalID;
};

template<typename T>
T* BytecodeGenerationPtr<T>::getPtr() const
{
    if(!sharedContext) return nullptr;
    return (T*)(sharedContext->bytecode.Buffer() + offset);
}


BCPtr<void>::RawVal allocateRaw(
    BytecodeGenerationContext*  context,
    size_t                      size,
    size_t                      alignment)
{
    size_t currentOffset = context->shared->bytecode.Count();

    size_t beginOffset = (currentOffset + (alignment-1)) & ~(alignment-1);

    size_t endOffset = beginOffset + size;

    for(size_t ii = currentOffset; ii < endOffset; ++ii)
        context->shared->bytecode.Add(0);

    return (BCPtr<void>::RawVal)beginOffset;
}

template<typename T>
BytecodeGenerationPtr<T> allocate(
    BytecodeGenerationContext*  context)
{
    return BytecodeGenerationPtr<T>(context->shared, allocateRaw(context, sizeof(T), alignof(T)));
}

template<typename T>
BytecodeGenerationPtr<T> allocateArray(
    BytecodeGenerationContext*  context,
    UInt                        count)
{
    return BytecodeGenerationPtr<T>(context->shared, allocateRaw(context, count * sizeof(T), alignof(T)));
}

template<typename T>
BytecodeGenerationPtr<T> getPtr(
    BytecodeGenerationContext*  context)
{
    return BytecodeGenerationPtr<T>(context->shared, context->shared->bytecode.Count());
}


void encodeUInt8(
    BytecodeGenerationContext*  context,
    uint8_t                     value)
{
    context->currentBytecode.Add(value);
}

void encodeUInt(
    BytecodeGenerationContext*  context,
    UInt                        value)
{
    if( value < 128 )
    {
        encodeUInt8(context, (uint8_t)value);
        return;
    }

    uint8_t bytes[16];
    UInt count = 0;

    for(;;)
    {
        UInt index = count++;
        bytes[index] = value & 0x7F;
        value = value >> 7;
        if (!value)
            break;

        bytes[index] |= 0x80;
    }

    UInt index = count;
    while (index--)
    {
        encodeUInt8(context, bytes[index]);
    }
}

void encodeSInt(
    BytecodeGenerationContext*  context,
    Int                         value)
{
    UInt uValue;
    if( value < 0 )
    {
        uValue = (~UInt(value) << 1) | 1;
    }
    else
    {
        uValue = UInt(value) << 1;
    }

    encodeUInt(context, uValue);
}

BCConst getGlobalValue(
    BytecodeGenerationContext*  context,
    IRInst*                     value)
{
    {
        BCConst bcConst;
        if (context->shared->mapValueToGlobal.TryGetValue(value, bcConst))
            return bcConst;
    }
    // Next we need to check for things that can be mapped to
    // global IDs on the fly.

    switch( value->op )
    {
    case kIROp_IntLit:
        {
            UInt constID = context->shared->constants.Count();
            context->shared->constants.Add(value);

            BCConst bcConst;
            bcConst.flavor = kBCConstFlavor_Constant;
            bcConst.id = (uint32_t)constID;

            context->shared->mapValueToGlobal.Add(value, bcConst);

            return bcConst;
        }
        break;

    default:
        break;
    }

    SLANG_UNEXPECTED("no ID for inst");
    {
        UNREACHABLE(BCConst bcConst);
        UNREACHABLE(bcConst.flavor = (BCConstFlavor)-1);
        UNREACHABLE(bcConst.id = -9999);
        UNREACHABLE_RETURN(bcConst);
    }
}

Int getLocalID(
    BytecodeGenerationContext*  context,
    IRInst*                     value)
{
    Int localID = 0;
    if( context->mapInstToLocalID.TryGetValue(value, localID) )
    {
        return localID;
    }

    BCConst bcConst = getGlobalValue(context, value);
    UInt remappedSymbolIndex = context->remappedGlobalSymbols.Count();
    context->remappedGlobalSymbols.Add(bcConst);

    localID = ~remappedSymbolIndex;
    context->mapInstToLocalID.Add(value, localID);
    return localID;
}

void encodeOperand(
    BytecodeGenerationContext*  context,
    IRInst*                     operand)
{
    auto id = getLocalID(context, operand);
    encodeSInt(context, id);
}

uint32_t getTypeID(
    BytecodeGenerationContext*  context,
    Type*                       type);

void encodeOperand(
    BytecodeGenerationContext*  context,
    IRType*                     type)
{
    encodeUInt(context, getTypeID(context, type));
}

bool opHasResult(IRInst* inst)
{
    auto type = inst->getDataType();
    if (!type) return false;

    // As a bit of a hack right now, we need to check whether
    // the function returns the distinguished `Void` type,
    // since that is conceptually the same as "not returning
    // a value."
    if (auto basicType = dynamic_cast<BasicExpressionType*>(type))
    {
        if (basicType->baseType == BaseType::Void)
            return false;
    }

    return true;
}

void generateBytecodeForInst(
    BytecodeGenerationContext*  context,
    IRInst*                     inst)
{
    // We are generating bytecode for a local instruction
    // inside a function or similar context.
    switch( inst->op )
    {
    default:
        {
            // As a default case, we will assume that bytecode ops
            // and the IR's internal opcodes are the same, and then
            // encode the necessary extra info:
            //

            auto operandCount = inst->getOperandCount();
            encodeUInt(context, inst->op);
            encodeOperand(context, inst->getDataType());
            encodeUInt(context, operandCount);
            for( UInt aa = 0; aa < operandCount; ++aa )
            {
                encodeOperand(context, inst->getOperand(aa));
            }

            if (!opHasResult(inst))
            {
                // This instructions has no type, so don't emit a destination
            }
            else
            {
                // The instruction can be encoded
                // as its own operand for the destination.
                encodeOperand(context, inst);
            }
        }
        break;

    case kIROp_ReturnVoid:
        // Trivial encoding here
        encodeUInt(context, inst->op);
        break;

    case kIROp_IntLit:
        {
            auto ii = (IRConstant*) inst;
            encodeUInt(context, ii->op);
            encodeOperand(context, ii->getDataType());

            // TODO: probably want distinct encodings
            // for signed vs. unsigned here.
            encodeUInt(context, UInt(ii->u.intVal));

            // destination:
            encodeOperand(context, inst);
        }
        break;

    case kIROp_FloatLit:
        {
            auto cInst = (IRConstant*) inst;
            encodeUInt(context, cInst->op);
            encodeOperand(context, cInst->getDataType());

            static const UInt size = sizeof(IRFloatingPointValue);
            unsigned char buffer[size];
            memcpy(buffer, &cInst->u.floatVal, sizeof(buffer));

            for(UInt ii = 0; ii < size; ++ii)
            {
                encodeUInt8(context, buffer[ii]);
            }

            // destination:
            encodeOperand(context, inst);
        }
        break;

    case kIROp_boolConst:
        {
            auto ii = (IRConstant*) inst;
            encodeUInt(context, ii->op);
            encodeUInt(context, ii->u.intVal ? 1 : 0);

            // destination:
            encodeOperand(context, inst);
        }
        break;

#if 0
    case kIROp_Func:
        {
            encodeUInt(context, inst->op);

            // We just want to encode the ID for the function
            // symbol data, and then do the rest on the decode side
            UInt nestedID = 0;
            context->mapInstToNestedID.TryGetValue(inst, nestedID);
            encodeUInt(context, nestedID);

            // destination:
            encodeOperand(context, inst);
        }
        break;
#endif

    case kIROp_Store:
        {
            encodeUInt(context, inst->op);

            // We need to encode the type being stored, to make
            // our lives easier.
            encodeOperand(context, inst->getOperand(1)->getDataType());
            encodeOperand(context, inst->getOperand(0));
            encodeOperand(context, inst->getOperand(1));
        }
        break;

    case kIROp_Load:
        {
            encodeUInt(context, inst->op);
            encodeOperand(context, inst->getDataType());
            encodeOperand(context, inst->getOperand(0));
            encodeOperand(context, inst);
        }
        break;
    }
}

BytecodeGenerationPtr<BCType> emitBCType(
    BytecodeGenerationContext*              context,
    Type*                                   type,
    IROp                                    op,
    BytecodeGenerationPtr<uint8_t> const*   args,
    UInt                                    argCount)
{
    UInt size = sizeof(BCType)
        + argCount * sizeof(BCPtr<void>);

    BytecodeGenerationPtr<uint8_t> bcAllocation(
        context->shared,
        allocateRaw(context, size, alignof(BCPtr<void>)));

    BytecodeGenerationPtr<BCType> bcType = bcAllocation.bitCast<BCType>();
    auto bcArgs = (bcType + 1).bitCast<BCPtr<uint8_t>>();

    bcType->op = op;
    bcType->argCount = (uint32_t)argCount;

    for(UInt aa = 0; aa < argCount; ++aa)
    {
        bcArgs[aa] = args[aa];
    }

    UInt id = context->shared->bcTypes.Count();
    context->shared->mapTypeToID.Add(type, id);
    context->shared->bcTypes.Add(bcType);
    bcType->id = (uint32_t)id;

    return bcType;
}

BytecodeGenerationPtr<BCType> emitBCVarArgType(
    BytecodeGenerationContext*              context,
    Type*                                   type,
    IROp                                    op,
    List<BytecodeGenerationPtr<uint8_t>>    args)
{
    return emitBCType(context, type, op, args.Buffer(), args.Count());
}

BytecodeGenerationPtr<BCType> emitBCType(
    BytecodeGenerationContext*  context,
    Type*                       type,
    IROp                        op)
{
    return emitBCType(context, type, op, nullptr, 0);
}

BytecodeGenerationPtr<BCType> emitBCType(
    BytecodeGenerationContext*  context,
    Type*                       type);

// Emit a `BCType` representation for the given `Type`
BytecodeGenerationPtr<BCType> emitBCTypeImpl(
    BytecodeGenerationContext*  context,
    Type*                       type)
{
    // A NULL type is interpreted as equivalent to `Void` for now.
    if( !type )
    {
        return emitBCType(context, type, kIROp_VoidType);
    }

    if( auto basicType = type->As<BasicExpressionType>() )
    {
        switch(basicType->baseType)
        {
        case BaseType::Void:    return emitBCType(context, type, kIROp_VoidType);
        case BaseType::Bool:    return emitBCType(context, type, kIROp_BoolType);
        case BaseType::Int:     return emitBCType(context, type, kIROp_Int32Type);
        case BaseType::UInt:    return emitBCType(context, type, kIROp_UInt32Type);
        case BaseType::UInt64:  return emitBCType(context, type, kIROp_UInt64Type);
        case BaseType::Half:    return emitBCType(context, type, kIROp_Float16Type);
        case BaseType::Float:   return emitBCType(context, type, kIROp_Float32Type);
        case BaseType::Double:  return emitBCType(context, type, kIROp_Float64Type);

        default:
            break;
        }
    }
    else if( auto funcType = type->As<FuncType>() )
    {
        List<BytecodeGenerationPtr<uint8_t>> operands;
        
        operands.Add(emitBCType(context, funcType->resultType).bitCast<uint8_t>());
        UInt paramCount = funcType->getParamCount();
        for(UInt pp = 0; pp < paramCount; ++pp)
        {
            operands.Add(emitBCType(context, funcType->getParamType(pp)).bitCast<uint8_t>());
        }

        return emitBCVarArgType(context, type, kIROp_FuncType, operands);
    }
    else if( auto ptrType = type->As<PtrType>() )
    {
        List<BytecodeGenerationPtr<uint8_t>> operands;
        operands.Add(emitBCType(context, ptrType->getValueType()).bitCast<uint8_t>());
        return emitBCVarArgType(context, type, kIROp_PtrType, operands);
    }
    else if( auto rwStructuredBufferType = type->As<HLSLRWStructuredBufferType>() )
    {
        List<BytecodeGenerationPtr<uint8_t>> operands;
        operands.Add(emitBCType(context, rwStructuredBufferType->elementType).bitCast<uint8_t>());
        return emitBCVarArgType(context, type, kIROp_readWriteStructuredBufferType, operands);
    }
    else if( auto structuredBufferType = type->As<HLSLStructuredBufferType>() )
    {
        List<BytecodeGenerationPtr<uint8_t>> operands;
        operands.Add(emitBCType(context, structuredBufferType->elementType).bitCast<uint8_t>());
        return emitBCVarArgType(context, type, kIROp_structuredBufferType, operands);
    }


    SLANG_UNEXPECTED("unimplemented");
    UNREACHABLE_RETURN(BytecodeGenerationPtr<BCType>());
}

BytecodeGenerationPtr<BCType> emitBCType(
    BytecodeGenerationContext*  context,
    Type*                       type)
{
    auto canonical = type->GetCanonicalType();
    UInt id = 0;
    if(context->shared->mapTypeToID.TryGetValue(canonical, id))
    {
        return context->shared->bcTypes[id];
    }

    BytecodeGenerationPtr<BCType> bcType = emitBCTypeImpl(context, canonical);
    return bcType;
}

uint32_t getTypeID(
    BytecodeGenerationContext*  context,
    Type*                       type)
{
    // We have a type, and we need to emit it (if we haven't
    // already) and return its index in the global type table.
    BytecodeGenerationPtr<BCType> bcType = emitBCType(context, type);
    return bcType->id;
}

uint32_t getTypeIDForGlobalSymbol(
    BytecodeGenerationContext*  context,
    IRInst*                     inst)
{
    auto type = inst->getDataType();
    if(!type)
        return 0;

    return getTypeID(context, type);
}

BytecodeGenerationPtr<char> allocateString(
    BytecodeGenerationContext*  context,
    char const*                 data,
    UInt                        size)
{
    BytecodeGenerationPtr<char> ptr = allocateArray<char>(context, size + 1);
    memcpy(ptr.getPtr(), data, size);
    return ptr;
}

BytecodeGenerationPtr<char> allocateString(
    BytecodeGenerationContext*  context,
    String const&               str)
{
    return allocateString(context,
        str.Buffer(),
        str.Length());
}

BytecodeGenerationPtr<char> allocateString(
    BytecodeGenerationContext*  context,
    Name*                       name)
{
    return allocateString(context, name->text);
}

BytecodeGenerationPtr<char> tryGenerateNameForSymbol(
    BytecodeGenerationContext*      context,
    IRGlobalValue*                  inst)
{
    // TODO: this is gross, and the IR should probably have
    // a more direct means of querying a name for a symbol.
    if (auto highLevelDeclDecoration = inst->findDecoration<IRHighLevelDeclDecoration>())
    {
        auto decl = highLevelDeclDecoration->decl;
        if (auto reflectionNameMod = decl->FindModifier<ParameterGroupReflectionName>())
        {
            return allocateString(context, reflectionNameMod->name);
        }
        else if(auto name = decl->nameAndLoc.name)
        {
            return allocateString(context, name);
        }
    }

    return BytecodeGenerationPtr<char>();
}

// Generate a `BCSymbol` that can represent a global value.
BytecodeGenerationPtr<BCSymbol> generateBytecodeSymbolForInst(
    BytecodeGenerationContext*  context,
    IRGlobalValue*              inst)
{
    switch( inst->op )
    {
    case kIROp_Func:
        {
            auto irFunc = (IRFunc*) inst;
            BytecodeGenerationPtr<BCFunc> bcFunc = allocate<BCFunc>(context);

            bcFunc->op = inst->op;
            bcFunc->typeID = getTypeIDForGlobalSymbol(context, inst);

            BytecodeGenerationContext   subContextStorage;
            BytecodeGenerationContext*  subContext = &subContextStorage;
            subContext->shared = context->shared;
            subContext->currentIRFunc = irFunc;

            // First we need to enumerate our basic blocks, so that they
            // can reference one another (basic blocks can forward reference
            // blocks that haven't been seen yet).
            //
            // Note: we allow the IDs of blocks to overlap with ordinary
            // "register" numbers, because there is no case where an operand
            // could be either a block or an ordinary register.
            //
            UInt blockCounter = 0;
            for( auto bb = irFunc->getFirstBlock(); bb; bb = bb->getNextBlock() )
            {
                Int blockID = Int(blockCounter++);
                subContext->mapInstToLocalID.Add(bb, blockID);
            }
            UInt blockCount = blockCounter;

            // Allocate the array of block objects to be stored in the
            // bytecode file.
            auto bcBlocks = allocateArray<BCBlock>(context, blockCount);
            bcFunc->blockCount = (uint32_t)blockCount;
            bcFunc->blocks = bcBlocks;

            // Now loop through the blocks again, and allocate the storage
            // for any parameters, variables, or registers used inside
            // each block.
            //
            // We'll count in a first pass, and then fill things in
            // using a second pass
            Int regCounter = 0;
            blockCounter = 0;
            for( auto bb = irFunc->getFirstBlock(); bb; bb = bb->getNextBlock() )
            {
                UInt blockID = blockCounter++;
                UInt paramCount = 0;

                for( auto ii = bb->getFirstInst(); ii; ii = ii->getNextInst() )
                {
                    switch( ii->op )
                    {
                    default:
                        // Default behavior: if an op has a result,
                        // then it needs a register to store it.
                        if(opHasResult(ii))
                        {
                            regCounter++;
                        }
                        break;

                    case kIROp_Param:
                        // A parameter always uses a register.
                        regCounter++;
                        //
                        // We also want to keep a count of the parameters themselves.
                        paramCount++;
                        break;

                    case kIROp_Var:
                        // A `var` (`alloca`) node needs two registers:
                        // one to hold the actual storage, and another
                        // to hold the pointer.
                        regCounter += 2;
                        break;
                    }
                }

                bcBlocks[blockID].paramCount = (uint32_t)paramCount;
            }

            // Okay, we've counted how many registers we need for each block,
            // and now we can allocate the contiguous array we will use.
            UInt regCount = regCounter;
            auto bcRegs = allocateArray<BCReg>(context, regCount);

            bcFunc->regCount = (uint32_t)regCount;
            bcFunc->regs = bcRegs;

            // Now we will loop over things again to fill in the information
            // on all these registers.

            regCounter = 0;
            blockCounter = 0;
            for( auto bb = irFunc->getFirstBlock(); bb; bb = bb->getNextBlock() )
            {
                UInt blockID = blockCounter++;

                // Loop over the instruction in the block, to allocate registers
                // for them. The parameters of a block will always be the first
                // N instructions in the block, so they will always get the
                // first N registers in that block. Similarly, the entry block
                // is always the first block, so that the parameters of the function
                // will always be the first N registers.
                //
                bcBlocks[blockID].params = bcRegs + regCounter;
                for( auto ii = bb->getFirstInst(); ii; ii = ii->getNextInst() )
                {
                    switch(ii->op)
                    {
                    default:
                        // For a parameter, or an ordinary instruction with
                        // a result, allocate it here.
                        if( opHasResult(ii) )
                        {
                            Int localID = regCounter++;
                            subContext->mapInstToLocalID.Add(ii, localID);

                            bcRegs[localID].op = ii->op;
#if 0
                            bcRegs[localID].name = tryGenerateNameForSymbol(context, ii);
#endif
                            bcRegs[localID].previousVarIndexPlusOne = (uint32_t)localID;
                            bcRegs[localID].typeID = getTypeIDForGlobalSymbol(context, ii);
                        }
                        break;

                    case kIROp_Var:
                        // As handled in the earlier loop, we are
                        // allocating *two* locations for each `var`
                        // instruction. The first of these will be
                        // the actual pointer value, while the second
                        // will be the storage for the variable value.
                        {
                            Int localID = regCounter;
                            regCounter += 2;

                            subContext->mapInstToLocalID.Add(ii, localID);
                            bcRegs[localID].op = ii->op;
#if 0
                            bcRegs[localID].name = tryGenerateNameForSymbol(context, ii);
#endif
                            bcRegs[localID].previousVarIndexPlusOne = (uint32_t)localID;
                            bcRegs[localID].typeID = getTypeIDForGlobalSymbol(context, ii);

                            bcRegs[localID+1].op = ii->op;
                            bcRegs[localID+1].previousVarIndexPlusOne = (uint32_t)localID+1;
                            bcRegs[localID+1].typeID = getTypeID(context,
                                (ii->getDataType()->As<PtrType>())->getValueType());
                        }
                        break;
                    }
                }
            }
            assert((UInt)regCounter == regCount);

            // Now that we've allocated our blocks and our registers
            // we can go through the actual process of emitting instructions. Hooray!
            blockCounter = 0;

            // Offset of each basic block from the start of the code
            // for the current funciton.
            List<UInt> blockOffsets;
            for( auto bb = irFunc->getFirstBlock(); bb; bb = bb->getNextBlock() )
            {
                blockCounter++;

                // Get local bytecode offset for current block.
                UInt blockOffset = subContext->currentBytecode.Count();
                blockOffsets.Add( blockOffset );

                for( auto ii = bb->getFirstInst(); ii; ii = ii->getNextInst() )
                {
                    // What we do with each instruction depends a bit on the
                    // kind of instruction it is.
                    switch( ii->op )
                    {
                    default:
                        // For most instructions we just emit their bytecode
                        // ops directly.
                        generateBytecodeForInst(subContext, ii);
                        break;

                    case kIROp_Param:
                        // Don't actually emit code for these, because
                        // there isn't really anything to *execute*
                        //
                        // Note that we *do* allow the `var` nodes
                        // to be executed, just because they need
                        // to set up a register with the pointer value.
                        break;
                    }
                }
            }


            // We've collected bytecode for the instruction stream
            // into a sub-context, so we can now append that code.
            UInt byteCount = subContext->currentBytecode.Count();
            BytecodeGenerationPtr<uint8_t> bytes = allocateArray<uint8_t>(context, byteCount);
            memcpy(&bytes[0], subContext->currentBytecode.Buffer(), byteCount);

            // Now that we've allocated the storage, we can write
            // the bytecode pointers into the blocks.
            blockCounter = 0;
            for( auto bb = irFunc->getFirstBlock(); bb; bb = bb->getNextBlock() )
            {
                UInt blockID = blockCounter++;
                bcBlocks[blockID].code = bytes + blockOffsets[blockID];
            }

            // Finally, after emitting all the instructions we can
            // build a table of global symbols taht need to be
            // imported into the current function as constants.
            UInt constCount = subContext->remappedGlobalSymbols.Count();
            auto bcConsts = allocateArray<BCConst>(context, constCount);

            bcFunc->constCount = (uint32_t)constCount;
            bcFunc->consts = bcConsts;

            for( UInt cc = 0; cc < constCount; ++cc )
            {
                bcConsts[cc] = subContext->remappedGlobalSymbols[cc];
            }

            return bcFunc;
        }
        break;

    case kIROp_global_var:
    case kIROp_global_constant:
        {
            auto bcVar = allocate<BCSymbol>(context);

            bcVar->op = inst->op;
            bcVar->typeID = getTypeID(context, inst->type);

            // TODO: actually need to intialize with body instructions

            return bcVar;
        }
        break;

    default:
        // Most instructions don't need a custom representation.
        return BytecodeGenerationPtr<BCSymbol>();
    }
}

BytecodeGenerationPtr<BCModule> generateBytecodeForModule(
    BytecodeGenerationContext*  context,
    IRModule*                   irModule)
{
    if (!irModule)
    {
        // Not IR module? Then return a null pointer.
        return BytecodeGenerationPtr<BCModule>();
    }

    // A module in the bytecode is mostly just a list of the
    // global symbols in the module.
    //
    // TODO: we need to be careful and recognize the distinction
    // between the global symbols in the *AST* module, vs. those
    // symbols which are effectively global in the *IR* module.
    //
    // We probably need to store these distinctly, since we
    // need the AST global symbols for reflection, and then
    // also to reconstruct the AST on load when importing a
    // serialized module. We then need the global IR symbols
    // to use when linking, to quickly resolve things without
    // needing any semantic knowledge of nesting at the AST level.
    //
    auto bcModule = allocate<BCModule>(context);

    // We need to compute how many "registers" to allocate
    // for the module, where the registers represent the
    // values being computed at the global scope.
    UInt symbolCount = 0;
    for(auto ii : irModule->getGlobalInsts())
    {
        auto gv = as<IRGlobalValue>(ii);
        if (!gv)
            continue;

        Int globalID = Int(symbolCount++);

        // Ensure that local code inside functions can see these symbols
        BCConst bcConst;
        bcConst.flavor = kBCConstFlavor_GlobalSymbol;
        bcConst.id = (uint32_t)globalID;
        context->shared->mapValueToGlobal.Add(gv, bcConst);

        // In the global scope, global IDs are also the local IDs
        context->mapInstToLocalID.Add(gv, globalID);
    }

    auto bcSymbols = allocateArray<BCPtr<BCSymbol>>(context, symbolCount);

    bcModule->symbolCount = (uint32_t)symbolCount;
    bcModule->symbols = bcSymbols;

    for(auto ii : irModule->getGlobalInsts())
    {
        auto gv = as<IRGlobalValue>(ii);
        if (!gv)
            continue;

        UInt symbolIndex = *context->mapInstToLocalID.TryGetValue(gv);

        auto bcSymbol = generateBytecodeSymbolForInst(context, gv);
        if (!bcSymbol.getPtr())
            continue;

        auto name = tryGenerateNameForSymbol(context, gv);
        bcSymbol->name = name;

        bcSymbols[symbolIndex] = bcSymbol;
    }

    // At this point we should have identified all the literals we need:
    UInt constantCount = context->shared->constants.Count();
    auto bcConstants = allocateArray<BCConstant>(context, constantCount);
    bcModule->constantCount = (uint32_t)constantCount;
    bcModule->constants = bcConstants;

    for(UInt cc = 0; cc < constantCount; ++cc)
    {
        auto irConstant = (IRConstant*) context->shared->constants[cc];
        bcConstants[cc].op = irConstant->op;
        bcConstants[cc].typeID = getTypeID(context, irConstant->type);

        switch(irConstant->op)
        {
        case kIROp_IntLit:
            {
                auto ptr = allocate<IRIntegerValue>(context);
                *ptr = irConstant->u.intVal;
                bcConstants[cc].ptr = ptr.bitCast<uint8_t>();
            }
            break;

        default:
            break;
        }

    }

    // At this point we should have collected all the types we need:
    UInt typeCount = context->shared->bcTypes.Count();
    auto bcTypes = allocateArray<BCPtr<BCType>>(context, typeCount);
    bcModule->typeCount = (uint32_t)typeCount;
    bcModule->types = bcTypes;

    for(UInt tt = 0; tt < typeCount; ++tt)
    {
        bcTypes[tt] = context->shared->bcTypes[tt];
    }


    return bcModule;
}

void generateBytecodeContainer(
    BytecodeGenerationContext*  context,
    CompileRequest*             compileReq)
{
    // Header must be the very first thing in the bytecode stream
    BytecodeGenerationPtr<BCHeader> header = allocate<BCHeader>(context);

    memcpy(header->magic, "slang\0bc", sizeof(header->magic));
    header->version = 0;

    // TODO: Need to generate BC representation of all the public/exported
    // declrations in the translation units, so that they can be used to
    // resolve depenencies downstream.

    // TODO: Need to dump BC representation of compiled kernel codes
    // for each specified code-generation target.

    List<BytecodeGenerationPtr<BCModule>> bcModulesList;
    for (auto translationUnitReq : compileReq->translationUnits)
    {
        auto bcModule = generateBytecodeForModule(context, translationUnitReq->irModule);
        bcModulesList.Add(bcModule);
    }

    UInt bcModuleCount = bcModulesList.Count();
    header->moduleCount = (uint32_t)bcModuleCount;

    auto bcModules = allocateArray<BCPtr<BCModule>>(context, bcModuleCount);
    header->modules = bcModules;
    for(UInt ii = 0; ii < bcModuleCount; ++ii)
    {
        bcModules[ii] = bcModulesList[ii];
    }
}

void generateBytecodeForCompileRequest(
    CompileRequest* compileReq)
{
    SharedBytecodeGenerationContext sharedContext;

    BytecodeGenerationContext context;
    context.shared = &sharedContext;

    generateBytecodeContainer(&context, compileReq);

    compileReq->generatedBytecode = sharedContext.bytecode;
}

// TODO: Need to support IR emit at the whole-module/compile-request
// level, and not just for individual entry points.
#if 0
List<uint8_t> emitSlangIRForEntryPoint(
    EntryPointRequest*  entryPoint)
{
    auto compileRequest = entryPoint->compileRequest;
    auto irModule = lowerEntryPointToIR(
        entryPoint,
        compileRequest->layout.Ptr(),
        // TODO: we need to pick the target more carefully here
        CodeGenTarget::HLSL);

#if 0
    String irAsm = getSlangIRAssembly(irModule);
    fprintf(stderr, "%s\n", irAsm.Buffer());
#endif

    // Now we need to encode that IR into a binary format
    // for transmission/serialization/etc.

    SharedBytecodeGenerationContext sharedContext;

    BytecodeGenerationContext context;
    context.shared = &sharedContext;

    generateBytecodeStream(&context, irModule);

    return sharedContext.bytecode;
}
#endif

} // namespace Slang
