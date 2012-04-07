// RuntimeInternals.cpp
// This file is part of the EScript programming language.
// See copyright notice in EScript.h
// ------------------------------------------------------
#include "RuntimeInternals.h"
#include "FunctionCallContext.h"
#include "../EScript.h"
#include "../Utils/Macros.h"
#include "../Utils/Helper.h"
#include "../Objects/Callables/Delegate.h"
#include "../Objects/Collections/Array.h"
#include "../Objects/Collections/Map.h"
#include "../Objects/Exception.h"
#include "../Objects/Namespace.h"
#include "../Objects/Values/Void.h"
#include "../Objects/YieldIterator.h"

#include <sstream>

namespace EScript{

//! (ctor)
RuntimeInternals::RuntimeInternals(Runtime & rt) : 
		runtime(rt),activeInstructionPos(-1),state(STATE_NORMAL){
	initSystemFunctions();
	
	globals = EScript::getSGlobals()->clone();
	declareConstant(globals.get(),"GLOBALS",globals.get());
	declareConstant(globals.get(),"SGLOBALS",EScript::getSGlobals());
}

RuntimeInternals::~RuntimeInternals(){
	declareConstant(globals.get(), "GLOBALS",NULL);
}

// -------------------------------------------------------------
// Function execution

//! (internal)
Object * RuntimeInternals::executeFunctionCallContext(_Ptr<FunctionCallContext> fcc){

	fcc->enableStopExecutionAfterEnding();
	pushActiveFCC(fcc);
	InstructionBlock * instructions = &fcc->getInstructions();

//	std::cout << fcc->getInstructions().toString()<<"\n";

	while( true ){
		// end of function? continue with calling function
		if(fcc->getInstructionCursor() >= instructions->getNumInstructions()){
			ObjRef result = fcc->getLocalVariable(Consts::LOCAL_VAR_INDEX_internalResult);
			if(fcc->isConstructorCall()){
				if(result.isNotNull()){
					warn("Constructors should not return a value.");
				}
				// \note the local variable $0 contains the created object, "fcc->getCaller()" contains the instanciated Type-Object.
				result = fcc->getLocalVariable(Consts::LOCAL_VAR_INDEX_this); 
			}
			if(fcc->isExecutionStoppedAfterEnding()){
				popActiveFCC();
				return result.detachAndDecrease();
			}
			popActiveFCC();
			fcc = getActiveFCC();
			if(fcc.isNull()){ //! just to be safe (should never occur)
				return result.detachAndDecrease();
			}
			
			instructions = &fcc->getInstructions();
			if(fcc->isAwaitingCaller()){
				fcc->initCaller(result);
			}else{
				fcc->stack_pushObject(result);
			}
			continue;
		}

		//! \todo this could probably improved by using iterators internally!
		activeInstructionPos = fcc->getInstructionCursor();
		const Instruction & instruction = instructions->getInstruction(activeInstructionPos);
		fcc->increaseInstructionCursor();
//
//		std::cout << fcc->stack_toDbgString()<<"\n";
//		std::cout << instruction.toString(*instructions)<<"\n";

		// --------------------------------------------------------------------------------------------------------------
		// Instructio execution...
		
		switch(instruction.getType()){

		case Instruction::I_ASSIGN_ATTRIBUTE:{
			/*	object = popObject
				value = popValueObject
				if object.identifier and not const and not private then object.identifier = value	*/
			ObjRef obj = fcc->stack_popObject();
			ObjRef value = fcc->stack_popObjectValue();

			Attribute * attr = obj->_accessAttribute(instruction.getValue_Identifier(),false);

			if(attr){
				if(attr->getProperties()&Attribute::ASSIGNMENT_RELEVANT_BITS){
					if(attr->isConst()){
						setException("Cannot assign to const attribute.");
						break;
					}else if(attr->isPrivate() && fcc->getCaller()!=obj ) {
						setException("Cannot access private attribute from outside of its owning object.");
						break;
					}
				}
				attr->setValue(value.get());
			}else{
				warn("Attribute not found..."); //! \todo proper warning!
			}
			continue;
		}
		case Instruction::I_ASSIGN_LOCAL:{
			/* 	assignLocal (uint32_t) variableIndex
				------------
				pop value
				$variableIndex = value	*/
			fcc->assignToLocalVariable(instruction.getValue_uint32(), fcc->stack_popObjectValue());
			continue;
		}

		case Instruction::I_ASSIGN_VARIABLE:{
			/*	value = popValueObject
				if caller.identifier then caller.identifier = value
				else if globals.identifier then globals.identigier = value
				else warning */
			ObjRef value = fcc->stack_popObjectValue();

			Attribute * attr = fcc->getCaller().isNotNull() ?
					fcc->getCaller()->_accessAttribute(instruction.getValue_Identifier(),false) :
					globals->_accessAttribute(instruction.getValue_Identifier(),true);
			if(attr){
				if(attr->isConst()){
					setException("Cannot assign to const attribute.");
				}else{
					attr->setValue(value.get());
				}
			}else{
				warn("Attribute not found..."); //! \todo proper warning!
			}
			continue;
		}
		case Instruction::I_CALL:{ 
			/*	call (uint32_t) numParams
				-------------
				pop numParams * parameters
				pop function
				pop object
				call the function
				push result (or jump to exception point)	*/
			const uint32_t numParams = instruction.getValue_uint32();

			std::vector<ObjRef> paramRefHolder; //! \todo why doesn't the ParameterValues keep a reference?
			paramRefHolder.reserve(numParams);
			ParameterValues params(numParams);
			for(int i=numParams-1;i>=0;--i ){
				Object * paramValue = fcc->stack_popObjectValue();
				params.set(i,paramValue);
				paramRefHolder.push_back(paramValue);
			}

			ObjRef fun = fcc->stack_popObject();
			ObjRef caller = fcc->stack_popObject();


			// returnValue , newUserFunctionCallContext
			executeFunctionResult_t result = startFunctionExecution(fun,caller,params);
			if(result.second){
				fcc = result.second;
				pushActiveFCC(fcc);
				instructions = &fcc->getInstructions();
			}else{
				fcc->stack_pushObject(result.first);
			}
			break;
		}
		case Instruction::I_CREATE_INSTANCE:{
			/*	create (uint32_t) numParams
				-------------
				pop numParams * parameters
				pop object
				call object._constructor
				push result (or jump to exception point)	*/
				

			// get the parameters for the first constructor
			const uint32_t numParams = instruction.getValue_uint32();
			std::vector<ObjRef> paramRefHolder; //! \todo why doesn't the ParameterValues keep a reference?
			paramRefHolder.reserve(numParams);
			ParameterValues params(numParams);
			for(int i=numParams-1;i>=0;--i ){
				Object * paramValue = fcc->stack_popObjectValue();
				params.set(i,paramValue);
				paramRefHolder.push_back(paramValue);
			}

			// collect constructor functions
			ObjRef caller = fcc->stack_popObject();
			EPtr<Type> type = caller.toType<Type>();
			if(type.isNull()){
				setException("Can't instanciate object not of type 'Type'");
				break;
			}

			executeFunctionResult_t result = startInstanceCreation(type,params);
			if(result.second){
				fcc = result.second;
				pushActiveFCC(fcc);
				instructions = &fcc->getInstructions();
			}else{
				fcc->stack_pushObject(result.first);
			}

			break;
		}
		case Instruction::I_CHECK_TYPE:{
			/*	checkType localVariableIdx
				--------------
				pop (Object) TypeOrObj
				if(localVar ---|> Type || localVar == Obj)
					push true
				else 
					push false
			*/
			const ObjPtr localVariable = fcc->getLocalVariable(instruction.getValue_uint32());
			const ObjRef typeOrObj = fcc->stack_popObject();
			fcc->stack_pushBool( checkParameterConstraint(runtime,localVariable,typeOrObj) );
			continue;
		}
		case Instruction::I_DUP:{
			// duplicate topmost stack entry
			fcc->stack_dup();
			continue;
		}
		case Instruction::I_FIND_VARIABLE:{
			/*	if caller.Identifier -> push (caller, caller.Identifier)
				else push (GLOBALS, GLOBALS.Identifier) (or NULL,NULL + Warning) 	*/
			if(fcc->getCaller().isNotNull()){
				const Attribute & attr = fcc->getCaller()->getAttribute(instruction.getValue_Identifier());
				if(attr.isNotNull()){
					fcc->stack_pushObject(fcc->getCaller());
					fcc->stack_pushObject(attr.getValue());
					continue;
				}
			}
			ObjPtr obj = getGlobalVariable(instruction.getValue_Identifier());
			if(obj.isNotNull()){
				fcc->stack_pushObject(globals.get());
				fcc->stack_pushObject(obj);
			}else{
				warn("Variable not found: "); //! \todo proper warning!
				fcc->stack_pushVoid();
				fcc->stack_pushVoid();
			}
			break;
		}
		case Instruction::I_GET_ATTRIBUTE:{ //! \todo check for @(private)
			/*	pop Object
				push Object.Identifier (or NULL + Warning)	*/
			ObjRef obj = fcc->stack_popObject();
			const Attribute & attr = obj->getAttribute(instruction.getValue_Identifier());
			if(attr.isNull()) {
				warn("Attribute not found: "); //! \todo add proper warning
				fcc->stack_pushVoid();
			}else if(attr.isPrivate() && fcc->getCaller()!=obj ) {
				setException("Cannot access private attribute from outside of its owning object.");
				break;
			}else{
				fcc->stack_pushObject( attr.getValue() );
			}
			continue;
		}
		case Instruction::I_GET_VARIABLE:{
			/*	if caller.Identifier -> push (caller.Identifier)
				else push (GLOBALS.Identifier) (or NULL + Warning) 	*/
			if(fcc->getCaller().isNotNull()){
				const Attribute & attr = fcc->getCaller()->getAttribute(instruction.getValue_Identifier());
				if(attr.isNotNull()){
					fcc->stack_pushObject(attr.getValue());
					continue;
				}
			}
			ObjPtr obj = getGlobalVariable(instruction.getValue_Identifier());
			if(obj.isNotNull()){
				fcc->stack_pushObject(obj);
			}else{
				warn("Variable not found: "); //! \todo proper warning!
				fcc->stack_pushVoid();
			}
			break;
		}
		case Instruction::I_GET_LOCAL_VARIABLE:{
			/* 	getLocalVariable (uint32_t) variableIndex
				------------
				push $variableIndex	*/
			fcc->stack_pushObject( fcc->getLocalVariable(instruction.getValue_uint32()).get() );
			continue;
		}
		case Instruction::I_INIT_CALLER:{
			const uint32_t numParams = instruction.getValue_uint32();

			if(fcc->stack_empty()){ // no constructor call
				if(numParams>0){
					warn("Calling constructor function with @(super) attribute as normal function.");
				}
//				fcc->initCaller(fcc->getCaller()); ???????
				continue;
			}

			// get super constructor parameters
			std::vector<ObjRef> paramRefHolder; //! \todo why doesn't the ParameterValues keep a reference?
			paramRefHolder.reserve(numParams);
			ParameterValues params(numParams);
			for(int i=numParams-1;i>=0;--i ){
				Object * paramValue = fcc->stack_popObjectValue();
				params.set(i,paramValue);
				paramRefHolder.push_back(paramValue);
			}

			// call super constructor
			ObjRef superConstructor = fcc->stack_popObjectValue();
			executeFunctionResult_t result = startFunctionExecution(superConstructor,fcc->getCaller(),params);
			std::vector<ObjPtr> constructors;
			while(!fcc->stack_empty()){
				constructors.push_back(fcc->stack_popObject());
			}
			
			if(result.second){
				// pass remaining super constructors to new calling context
				fcc = result.second;
				pushActiveFCC(fcc);
				instructions = &fcc->getInstructions();
				for(std::vector<ObjPtr>::const_reverse_iterator it = constructors.rbegin();it!=constructors.rend();++it) 
					fcc->stack_pushObject( *it );
				fcc->enableAwaitingCaller();
				fcc->markAsConstructorCall();
			}else{
				ObjPtr newObj = result.first;
				if(newObj.isNull()){
					if(state!=STATE_EXCEPTION) // if an exception occured in the constructor, the result may be NULL
						setException("Constructor did not create an Object."); //! \todo improve message!
					break;
				}
				// init attributes
				newObj->_initAttributes(runtime); //! \todo catch exceptions!!!!!!!!
				fcc->initCaller(newObj);
			}

			break;
		}		
		case Instruction::I_JMP:{
			fcc->setInstructionCursor( instruction.getValue_uint32() );
			continue;
		}
		case Instruction::I_JMP_IF_SET:{
			/* 	jmpIfSet (uint32) targetAddress
				-------------
				pop (uint32) local variable index
				jmp if variable != NULL */
			if( fcc->getLocalVariable( fcc->stack_popUInt32() ).isNotNull() )
				fcc->setInstructionCursor( instruction.getValue_uint32() );
			continue;
		}
		case Instruction::I_JMP_ON_TRUE:{
			if(fcc->stack_popBool())
				fcc->setInstructionCursor( instruction.getValue_uint32() );
			continue;
		}
		case Instruction::I_JMP_ON_FALSE:{
			if(!fcc->stack_popBool())
				fcc->setInstructionCursor( instruction.getValue_uint32() );
			continue;
		}
		case Instruction::I_NOT:{
			/*	not
				-------
				bool b = popBool
				push !b	*/
			fcc->stack_pushBool( !fcc->stack_popBool() );
			continue;
		}
		case Instruction::I_POP:{
			// remove entry from stack
			fcc->stack_pop();
			continue;
		}
		case Instruction::I_PUSH_BOOL:{
			fcc->stack_pushBool( instruction.getValue_Bool() );
			continue;
		}
		case Instruction::I_PUSH_ID:{
			fcc->stack_pushIdentifier( instruction.getValue_Identifier() );
			continue;
		}
		case Instruction::I_PUSH_FUNCTION:{
			fcc->stack_pushFunction( instruction.getValue_uint32() );
			continue;
		}
		case Instruction::I_PUSH_NUMBER:{
			fcc->stack_pushNumber( instruction.getValue_Number() );
			continue;
		}
		case Instruction::I_PUSH_STRING:{
			fcc->stack_pushStringIndex( instruction.getValue_uint32() );
			continue;
		}
		case Instruction::I_PUSH_UINT:{
			fcc->stack_pushUInt32( instruction.getValue_uint32() );
			continue;
		}
		case Instruction::I_PUSH_UNDEFINED:{
			fcc->stack_pushUndefined();
			continue;
		}		
		case Instruction::I_PUSH_VOID:{
			fcc->stack_pushVoid( );
			continue;
		}
		case Instruction::I_RESET_LOCAL_VARIABLE:{
			// $localVarId = NULL
			fcc->assignToLocalVariable(instruction.getValue_uint32(), NULL);
			continue;
		}
		case Instruction::I_SET_ATTRIBUTE:{
			/*	setAttribute identifierId
				-------------
				properies = pop uint32
				obj = pop object
				value = pop parameterObj
				obj.identifier @(properties) := value	*/

			const uint32_t properties = fcc->stack_popUInt32();
			ObjRef obj = fcc->stack_popObject();
			ObjRef value = fcc->stack_popObjectValue();
			if( (properties & Attribute::OVERRIDE_BIT) && (obj->_accessAttribute(instruction.getValue_Identifier(),false) == NULL ) ) {
				warn("Attribute marked with @(override) does not override.");
			} 
			if(!obj->setAttribute(instruction.getValue_Identifier(),Attribute(value,properties))){
				warn("Could not set Attribute..."); //! \todo proper warning!
			}
			continue;
		}
		case Instruction::I_SET_EXCEPTION_HANDLER:{
			fcc->setExceptionHandlerPos(instruction.getValue_uint32());
			continue;
		}
		case Instruction::I_SYS_CALL:{
			/*	sysCall (uint32_t) numParams
				-------------
				pop functionId
				pop numParams * parameters
				sysCall functionId,parameters
				push result (or jump to exception point)	*/
			const uint32_t numParams = instruction.getValue_uint32();
			const uint32_t funId = fcc->stack_popUInt32();
			
			std::vector<ObjRef> paramRefHolder; //! \todo why doesn't the ParameterValues keep a reference?
			paramRefHolder.reserve(numParams);
			ParameterValues params(numParams);
			for(int i=numParams-1;i>=0;--i ){
				Object * paramValue = fcc->stack_popObjectValue();
				params.set(i,paramValue);
				paramRefHolder.push_back(paramValue);
			}
			fcc->stack_pushObject( sysCall(funId,params) );
			break;
		}
		case Instruction::I_YIELD:{
			/*	yield
				-------------
				pop result	*/
			ObjRef value = fcc->stack_popObjectValue();
			ERef<YieldIterator> yIt = new YieldIterator();
			yIt->setFCC(fcc);
			yIt->setValue(value);
			if(fcc->isExecutionStoppedAfterEnding()){
				popActiveFCC();
				return yIt.detachAndDecrease();
			} // continue with the next fcc...
			else{
				popActiveFCC();
				fcc = getActiveFCC();
				if(fcc.isNull())
					return NULL;
				instructions = &fcc->getInstructions();
				fcc->stack_pushObject(yIt.get());
			}
			break;
		}
		case Instruction::I_UNDEFINED:
		case Instruction::I_SET_MARKER:
		default:{
			warn("Unknown Instruction");
		}
		}
		// --------------------------------------------------------------------------------------------------------------
		if(getState()==STATE_NORMAL){
			continue;
		}else if(getState()==STATE_EXCEPTION){
			while(true){
				fcc->stack_clear(); // remove current stack content
				
				// catch-block available?
				if(fcc->getExceptionHandlerPos()!=Instruction::INVALID_JUMP_ADDRESS){
					fcc->assignToLocalVariable(Consts::LOCAL_VAR_INDEX_internalResult,getResult()); // ___result = exceptionResult
					resetState();
					fcc->setInstructionCursor(fcc->getExceptionHandlerPos());
					break;
				} // execution stops here? Keep the exception-state and return;
				else if(fcc->isExecutionStoppedAfterEnding()){
					popActiveFCC();
					return NULL;
				} // continue with the next fcc...
				else{
					popActiveFCC();
					fcc = getActiveFCC();
					if(fcc.isNull())
						return NULL;
					instructions = &fcc->getInstructions();
				}
			}
		} else if(getState()==STATE_EXITING){
			while(true){
				// execution stops here? Keep the exiting-state and return;
				if(fcc->isExecutionStoppedAfterEnding()){
					popActiveFCC();
					return NULL;
				} // continue with the next fcc...
				else{
					popActiveFCC();
					fcc = getActiveFCC();
					if(fcc.isNull())
						return NULL;
					instructions = &fcc->getInstructions();
				}
			}
		}
	}
	return Void::get();
}

//! (internal)
RuntimeInternals::executeFunctionResult_t RuntimeInternals::startFunctionExecution(const ObjPtr & fun,const ObjPtr & _callingObject,ParameterValues & params){
	ObjPtr result;
	switch( fun->_getInternalTypeId() ){
		case _TypeIds::TYPE_USER_FUNCTION:{
			UserFunction * userFunction = static_cast<UserFunction*>(fun.get());
			_CountedRef<FunctionCallContext> fcc = FunctionCallContext::create(userFunction,_callingObject);

			// check for too few parameter values -> throw exception
			if(userFunction->getMinParamCount()>=0 && params.size()<static_cast<size_t>(userFunction->getMinParamCount())){
				setException("Too few parameters given."); //! \todo improve error message (which parameters are missing?)
				return std::make_pair(result.get(),static_cast<FunctionCallContext*>(NULL));
			}
			ParameterValues::const_iterator paramsEnd;
			const int maxParamCount = userFunction->getMaxParamCount();
			if( maxParamCount<0 ){ // multiParameter
				ERef<Array> multiParamArray = Array::create();
				// copy values into multiParamArray
				if(params.size()>=userFunction->getParamCount()){
					for(size_t i = userFunction->getParamCount()-1;i<params.size();++i){
						multiParamArray->pushBack(params[i]);
					}
					paramsEnd = params.begin()+(userFunction->getParamCount()-1);

				}else{
					paramsEnd = params.end();
				}
				// directly assign to last parameter
				fcc->assignToLocalVariable(Consts::LOCAL_VAR_INDEX_firstParameter + (userFunction->getParamCount()-1) ,multiParamArray.get());
			} // too many parameters
			else if( params.size()>static_cast<size_t>(maxParamCount) ){
				warn("Too many parameters given.");  //! \todo improve warning message (how many?)
				paramsEnd = params.begin()+maxParamCount; // std::next(...)
			} // normal parameter count range
			else{
				paramsEnd = params.end();
			}
			// init $thisFn
			fcc->assignToLocalVariable(Consts::LOCAL_VAR_INDEX_thisFn,fun);

			uint32_t i = Consts::LOCAL_VAR_INDEX_firstParameter;
			for(ParameterValues::const_iterator it = params.begin(); it!= paramsEnd; ++it){
				fcc->assignToLocalVariable(i++,*it);
			}
			return std::make_pair(result.get(),fcc.detachAndDecrease());
		}
		case _TypeIds::TYPE_DELEGATE:{
			Delegate * delegate = static_cast<Delegate*>(fun.get());
			return startFunctionExecution(delegate->getFunction(),delegate->getObject(),params);
		}
		case _TypeIds::TYPE_FUNCTION:{ // is  C++ function ?
			Function * libfun=static_cast<Function*>(fun.get());
			{	// check param count
				const int min = libfun->getMinParamCount();
				const int max = libfun->getMaxParamCount();
				if( (min>0 && static_cast<int>(params.count())<min)){
					std::ostringstream sprinter;
					sprinter<<"Too few parameters: Expected " <<min<<", got "<<params.count()<<'.';
					//! \todo improve message
					setException(sprinter.str());
					return std::make_pair(result.get(),static_cast<FunctionCallContext*>(NULL));
				} else  if (max>=0 && static_cast<int>(params.count())>max) {
					std::ostringstream sprinter;
					sprinter<<"Too many parameters: Expected " <<max<<", got "<<params.count()<<'.';
					//! \todo improve message
					warn(sprinter.str());
				}
			}
			libfun->increaseCallCounter();

			try {
				result = (*libfun->getFnPtr())(runtime,_callingObject.get(),params);
			} catch (Exception * e) {
				setExceptionState(e);
			} catch(const char * message) {
				setException(std::string("C++ exception: ")+message);
			} catch(const std::string & message) {
				setException(std::string("C++ exception: ") + message);
			} catch(const std::exception & e) {
				setException(std::string("C++ exception: ") + e.what());
			} catch (Object * obj) {
				// workaround: this should be covered by catching the Exception* directly, but that doesn't always seem to work!?!
				Exception * e=dynamic_cast<Exception *>(obj);
				if(e){
					setExceptionState(e);
				}else{
					const std::string message=(obj?obj->toString():"NULL");
					setException(message);
				}
			}  catch (...){
				setException("C++ exception");
			}
			break;
		}

		default:{
			// function-object has a user defined "_call"-member?
			const Attribute & attr = fun->getAttribute(Consts::IDENTIFIER_fn_call);		//! \todo check for @(private)
			
			if(attr.getValue()){
				// fun._call( callingObj , param0 , param1 , ... )
				ParameterValues params2(params.count()+1);
				params2.set(0,_callingObject.isNotNull() ? _callingObject : Void::get());
				std::copy(params.begin(),params.end(),params2.begin()+1);

				return startFunctionExecution(attr.getValue(),fun,params2);
			}

			warn("Cannot use '"+fun->toDbgString()+"' as a function.");
	
		}
	}

	return std::make_pair(result.get(),static_cast<FunctionCallContext*>(NULL));
}


//! (internal)
RuntimeInternals::executeFunctionResult_t RuntimeInternals::startInstanceCreation(EPtr<Type> type,ParameterValues & params){
	static const executeFunctionResult_t failureResult = std::make_pair(static_cast<Object*>(NULL),static_cast<FunctionCallContext*>(NULL));
	ObjRef createdObject;
	
	// collect constructors
	std::vector<ObjPtr> constructors;
	EPtr<Type> typeCursor = type;
	do{
		const Attribute * ctorAttr = typeCursor->_accessAttribute(Consts::IDENTIFIER_fn_constructor,true);
		if(ctorAttr==NULL){
			typeCursor = typeCursor->getBaseType();
			continue;
		}else if(constructors.empty() && ctorAttr->isPrivate()){ // first constructor must not be private!
			setException("Can't instanciate Type with private _contructor."); //! \todo check this!
			return failureResult; // failure
		}else{
			ObjPtr fun = ctorAttr->getValue();
			const internalTypeId_t funType = fun->_getInternalTypeId();
			
			if(funType==_TypeIds::TYPE_USER_FUNCTION){
				constructors.push_back(fun);
				typeCursor = typeCursor->getBaseType();
				continue;
			}else if(_TypeIds::TYPE_FUNCTION){
				constructors.push_back(fun);
			}else{
				setException("Constructor has to be a UserFunction or a Function.");
				return failureResult; // failure
			}
			break;
		}
	}while(typeCursor.isNotNull());
	
	if(constructors.empty()) // failure
		return failureResult;
	
	{ // call the first constructor and pass the other constructor-functions by adding them to the stack
		ObjRef fun = constructors.front();
		
		executeFunctionResult_t result = startFunctionExecution(fun,type.get(),params);
		if(result.second){
			FunctionCallContext * fcc = result.second;
			for(std::vector<ObjPtr>::const_reverse_iterator it = constructors.rbegin();(it+1)!=constructors.rend();++it) //! \todo c++11 use std::next
				fcc->stack_pushObject( *it );
			fcc->enableAwaitingCaller();
			fcc->markAsConstructorCall();
			return std::make_pair(static_cast<Object*>(NULL),fcc);
		}else{
			createdObject = result.first;
			if(createdObject.isNull()){
				if(state!=STATE_EXCEPTION) // if an exception occured in the constructor, the result may be NULL
					setException("Constructor did not create an Object."); //! \todo improve message!
				return failureResult;
			}
			
			// init attribute
			createdObject->_initAttributes(runtime); //! \todo catch exceptions!!!!!!!!
			return std::make_pair(createdObject.detachAndDecrease(),static_cast<FunctionCallContext*>(NULL));
		}
	}
	return failureResult;
}

// -------------------------------------------------------------
// Globals
Namespace * RuntimeInternals::getGlobals()const	{
	return globals.get();
}

ObjPtr RuntimeInternals::getGlobalVariable(const StringId id) {
	// \note getLocalAttribute is used to skip the members of Type
	// 	which are otherwise found as false global variables  [BUG20100618]
	return globals->getLocalAttribute(id).getValue();
}



// -------------------------------------------------------------
// Helper

//! (static)			
bool RuntimeInternals::checkParameterConstraint(Runtime & rt,const ObjPtr & value,const ObjPtr & constraint){
	return value->isA(constraint.toType<Type>()) || value->isIdentical(rt,constraint);
}

// -------------------------------------------------------------
// Information

std::string RuntimeInternals::getCurrentFile()const{
	if(getActiveFCC().isNotNull()){
		return getActiveFCC()->getUserFunction()->getCode().getFilename();
	}
	return std::string();
}

int RuntimeInternals::getCurrentLine()const{
	if(getActiveFCC().isNotNull() && activeInstructionPos>=0){
		return getActiveFCC()->getInstructions().getLine(activeInstructionPos);
	}else{
		return -1;
	}
}
// -------------------------------------------------------------
// State
void RuntimeInternals::setException(const std::string & s) {
	Exception * e = new Exception(s,getCurrentLine());
	e->setFilename(getCurrentFile());
	setException(e);
}

void RuntimeInternals::setException(Exception * e){
//	e->setStackInfo(getStackInfo());
	setExceptionState(e);
}


// -------------------------------------------------------------
// System calls

//! (internal)
void RuntimeInternals::initSystemFunctions(){
	systemFunctions.resize(7);

	//! init system calls \note the order of the functions MUST correspond to their funcitonId as defined in Consts.h
	{	// SYS_CALL_CREATE_ARRAY = 0;
		struct _{
			ESF( sysCall,0,-1,Array::create(parameter) )
		};
		systemFunctions[Consts::SYS_CALL_CREATE_ARRAY] = new Function(_::sysCall);
	}	
	{	//! [ESF] Map SYS_CALL_CREATE_MAP( key0,value0, key1,value1, ... )
		struct _{
			ES_FUNCTION( sysCall) {
					if ( (parameter.count()%2)==1 ) runtime.warn("Map: Last parameter ignored!");
					Map * a=Map::create();
					for (ParameterValues::size_type i=0;i<parameter.count();i+=2)
						a->setValue(parameter[i],parameter[i+1]);
				return a;			
			}
		};
		systemFunctions[Consts::SYS_CALL_CREATE_MAP] = new Function(_::sysCall);
	}
	{	//! [ESMF] Void SYS_CALL_THROW_TYPE_EXCEPTION( expectedType, receivedValue )
		struct _{
			ES_FUNCTION( sysCall) {
				assertParamCount(runtime,2,-1);
				std::ostringstream os;
				os << "Wrong parameter type: Expected ";
				for(size_t i = 0;i<parameter.size()-1;++i ){
					if(i>0) os <<", ";
					os<<(parameter[i].isNotNull() ? parameter[i]->toDbgString() : "???");
				}
				os << " but got " << parameter[parameter.size()-1]->toDbgString()<<".";
				runtime.setException(os.str());
				return static_cast<Object*>(NULL);
			}
		};
		systemFunctions[Consts::SYS_CALL_THROW_TYPE_EXCEPTION] = new Function(_::sysCall);
	}
	{	//! [ESMF] Void SYS_CALL_THROW( [value] )
		struct _{
			ESF( sysCall,0,1,(runtime._setExceptionState( parameter.count()>0 ? parameter[0] : Void::get() ),static_cast<Object*>(NULL)))
		};
		systemFunctions[Consts::SYS_CALL_THROW] = new Function(_::sysCall);
	}	
	{	//! [ESMF] Void SYS_CALL_EXIT( [value] )
		struct _{
			ESF( sysCall,0,1,(runtime._setExitState( parameter.count()>0 ? parameter[0] : Void::get() ),static_cast<Object*>(NULL)))
		};
		systemFunctions[Consts::SYS_CALL_EXIT] = new Function(_::sysCall);
	}
	{	//! [ESMF] Iterator SYS_CALL_GET_ITERATOR( object );
		struct _{
			ES_FUNCTION( sysCall) {
				assertParamCount(runtime,1,1);
				ObjRef it;
				if(	Collection * c=parameter[0].toType<Collection>()){
					it = c->getIterator();
				}else if(parameter[0].toType<YieldIterator>()){
					it = parameter[0].get();
				}else {
					it = callMemberFunction(runtime,parameter[0] ,Consts::IDENTIFIER_fn_getIterator,ParameterValues());
				}
				if(it==NULL){
					runtime.setException("Could not get iterator from '" + parameter[0]->toDbgString() + '\'');
					return NULL;
				}
				return it.detachAndDecrease();
			}
		};
		systemFunctions[Consts::SYS_CALL_GET_ITERATOR] = new Function(_::sysCall);
	}
	{	//! [ESMF] Void SYS_CALL_TEST_ARRAY_PARAMETER_CONSTRAINTS( expectedTypes*, Array receivedValue )
		struct _{
			ES_FUNCTION( sysCall) {
				assertParamCount(runtime,2,-1);
				const size_t constraintEnd = parameter.size()-1;

				Array * values = assertType<Array>(runtime,parameter[constraintEnd]);
				
				for(Array::iterator it = values->begin();it!=values->end();++it){
					bool success = false;
					for(size_t i = 0; i<constraintEnd; ++i){
						if(RuntimeInternals::checkParameterConstraint(runtime,*it,parameter[i])){
							success = true;
							break;
						}
					}
					if(!success){
						std::ostringstream os;
						os << "Wrong parameter type: Expected ";
						for(size_t i = 0;i<constraintEnd;++i ){
							if(i>0) os <<", ";
							os<<(parameter[i].isNotNull() ? parameter[i]->toDbgString() : "???");
						}
						os << " but got " << (*it)->toDbgString()<<".";
						runtime.setException(os.str());
						return static_cast<Object*>(NULL);
					}
				}
				return static_cast<Object*>(NULL);
			}
		};
		systemFunctions[Consts::SYS_CALL_TEST_ARRAY_PARAMETER_CONSTRAINTS] = new Function(_::sysCall);
	}
}

//! (static)
Object * RuntimeInternals::sysCall(uint32_t sysFnId,ParameterValues & params){
	Function * fn = NULL;
	if(sysFnId<systemFunctions.size()){
		fn = systemFunctions.at(sysFnId).get();
	}
	if(!fn){
		runtime.setException("Unknown systemCall."); // \todo improve message
		return NULL;
	}
	return fn->getFnPtr()(runtime,NULL,params);
}

// -------------------------------------------------------------

}