// FunctionCallContext.cpp
// This file is part of the EScript programming language (https://github.com/EScript)
//
// Copyright (C) 2012-2013 Claudius Jähn <ClaudiusJ@live.de>
// Copyright (C) 2012 Benjamin Eikel <benjamin@eikel.org>
//
// Licensed under the MIT License. See LICENSE file for details.
// ---------------------------------------------------------------------------------
#include "FunctionCallContext.h"

#include "../Consts.h"
#include "../Objects/Identifier.h"
#include "../Objects/Values/Bool.h"
#include "../Objects/Values/Number.h"
#include "../Objects/Values/String.h"
#include "../Objects/Values/Void.h"
#include <stdexcept>
#include <sstream>

namespace EScript{

static std::stack<FunctionCallContext *> pool;

#if defined(ES_THREADING)
static SyncTools::FastLock poolMutex;
#endif // ES_THREADING

//! (static) Factory
FunctionCallContext * FunctionCallContext::create(ERef<UserFunction> userFunction,ObjRef _caller){
	FunctionCallContext * fcc = nullptr;
	{
#ifdef ES_THREADING
		auto lock = SyncTools::tryLock(poolMutex);
		if(lock.owns_lock()){
			if(pool.empty()){
				lock.unlock();
				fcc = new FunctionCallContext;
			}else{
				fcc = pool.top();
				pool.pop();
			}
		}else{
#endif /* ES_THREADING */
			fcc = new FunctionCallContext;
#ifdef ES_THREADING
		}
#endif /* ES_THREADING */
	}
//	fcc = new FunctionCallContext;
//	assert(userFunction); //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	fcc->init(std::move(userFunction), std::move(_caller));
	return fcc;
}

//! static
void FunctionCallContext::release(FunctionCallContext *fcc){
	fcc->reset();
	{
#ifdef ES_THREADING
		auto lock = SyncTools::tryLock(poolMutex);
		if(lock.owns_lock()){
			pool.push(fcc);
		}else{
#endif /* ES_THREADING */
			delete fcc;
#ifdef ES_THREADING
		}
#endif /* ES_THREADING */
	}
}

// -------------------------------------------------------------------------

std::string FunctionCallContext::getLocalVariablesAsString(const bool includeUndefined)const{
	const std::vector<StringId> & vars = getInstructionBlock().getLocalVariables();
	std::ostringstream os;
	for(size_t i = 0;i<vars.size();++i ){
		ObjPtr value = getLocalVariable(i);
		if(value.isNull() && !includeUndefined )
			continue;
//		os << '$' << vars[i].toString() << '=' << (value ? value->toDbgString() : "undefined" )<< '\t';
		os << '$' << vars[i].toString() << '=' << value.toString("undefined") << '\t';
	}
	return os.str();

}

void FunctionCallContext::init(ERef<UserFunction>&&_userFunction,ObjRef&& _caller){
	caller = std::move(_caller);
	userFunction = std::move(_userFunction);
	instructionCursor = getInstructions().begin();
	constructorCall = false;
	providesCallerAsResult = false;
	stopExecutionAfterEnding = false;
	exceptionHandlerPos = Instruction::INVALID_JUMP_ADDRESS;

	localVariables.resize(getInstructionBlock().getNumLocalVars());

	localVariables[Consts::LOCAL_VAR_INDEX_this] = caller; // ?????????????????
	localVariables[Consts::LOCAL_VAR_INDEX_thisFn] = userFunction.get();
}
void FunctionCallContext::initCaller(const ObjPtr _caller){
	caller = _caller;
	localVariables[Consts::LOCAL_VAR_INDEX_this] = caller;
}


void FunctionCallContext::reset(){
	caller = nullptr;
	userFunction = nullptr;
	localVariables.clear();
	while(!valueStack.empty())
		stack_pop();
}
void FunctionCallContext::stack_clear(){
	while(!valueStack.empty()){
		stack_pop();
	}
}
ObjRef FunctionCallContext::rtValueToObject(RtValue & entry){
	switch(entry.valueType){
	case RtValue::VOID_VALUE:
		return Void::get();
	case RtValue::OBJECT_PTR:{
		ObjRef result(std::move( entry._detachObject() ));
		return result;
	}
	case RtValue::BOOL:{
		return Bool::create(entry._getBool());
	}
	case RtValue::UINT32:{
		return Number::create(entry._getUInt32());
	}
	case RtValue::NUMBER:{
		return Number::create(entry._getNumber());
	}
	case RtValue::IDENTIFIER:{
		return Identifier::create(StringId(entry._getIdentifier()));
	}
	case RtValue::LOCAL_STRING_IDX:{
		return String::create(getInstructionBlock().getStringConstant(entry._getLocalStringIndex()));
	}
	case RtValue::FUNCTION_CALL_CONTEXT:
	case RtValue::UNDEFINED:
	default:;
	}
	return Void::get();
}
ObjRef FunctionCallContext::stack_popObjectValue(){
	RtValue & entry = stack_top();
	ObjRef obj;
	switch(entry.valueType){
	case RtValue::VOID_VALUE:
		obj = Void::get();
		break;
	case RtValue::OBJECT_PTR:{
		obj = std::move(entry.getObject()->getRefOrCopy());
		break;
	}
	case RtValue::BOOL:{
		obj = Bool::create(entry._getBool());
		break;
	}
	case RtValue::UINT32:{
		obj = Number::create(entry._getUInt32());
		break;
	}
	case RtValue::NUMBER:{
		obj = Number::create(entry._getNumber());
		break;
	}
	case RtValue::IDENTIFIER:{
		obj = Identifier::create(StringId(entry._getIdentifier()));
		break;
	}
	case RtValue::LOCAL_STRING_IDX:{
		obj = String::create(getInstructionBlock().getStringConstant(entry._getLocalStringIndex()));
		break;
	}
	case RtValue::FUNCTION_CALL_CONTEXT:
	case RtValue::UNDEFINED:{
//		std::cout << "popUndefined";
	}
	default:; // Important: obj remains nullptr!

	}
	valueStack.pop_back();
	return obj;
}

void FunctionCallContext::throwError(FunctionCallContext::error_t error)const{
	static const std::string prefix("Internal error: ");
	switch(error){
		case STACK_EMPTY_ERROR:
			throw std::logic_error(prefix+"Empty stack.");
		case STACK_WRONG_DATA_TYPE:
			throw std::logic_error(prefix+"Wrong data type on stack.");
		case UNKNOWN_LOCAL_VARIABLE:
			throw std::logic_error(prefix+"Invalid local variable.");
		case UNKNOWN_STATIC_VARIABLE:
			throw std::logic_error(prefix+"Invalid static variable.");
		default:
			throw std::logic_error(prefix+"???");
	}
}

std::string  FunctionCallContext::stack_toDbgString()const{
	std::ostringstream out;
	out<<"[";
	std::vector<RtValue>::const_iterator it = valueStack.begin();
	if(it!=valueStack.end()){
		out << (*it).toDbgString();
		++it;
	}
	for(;it!=valueStack.end();++it)
		out << ", "<<(*it).toDbgString();
	out << "]";
	return out.str();
}
}
