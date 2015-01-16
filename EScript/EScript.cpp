// EScript.cpp
// This file is part of the EScript programming language (https://github.com/EScript)
//
// Copyright (C) 2011-2013 Claudius Jähn <ClaudiusJ@live.de>
// Copyright (C) 2011-2012 Benjamin Eikel <benjamin@eikel.org>
//
// Licensed under the MIT License. See LICENSE file for details.
// ---------------------------------------------------------------------------------
#include "EScript.h"
#include "Objects/Identifier.h"
#include "Objects/YieldIterator.h"
#include "Objects/Callables/FnBinder.h"
#include "Objects/Callables/Function.h"
#include "Objects/Exception.h"

#include "../E_Libs/StdLib.h"
#ifdef _WIN32
#include "../E_Libs/Win32Lib.h"
#endif
#include "../E_Libs/IOLib.h"
#include "../E_Libs/MathLib.h"
#if defined(ES_THREADING)
#include "../E_Libs/ThreadingLib.h"
#endif // ES_THREADING

namespace EScript {

//! (static)
void init() {
	Namespace * SGLOBALS = getSGlobals();

	Object::init(*SGLOBALS);
	Type::init(*SGLOBALS);
	ExtObject::init(*SGLOBALS);

	Number::init(*SGLOBALS);
	Bool::init(*SGLOBALS);
	String::init(*SGLOBALS);

	Collection::init(*SGLOBALS);
	Identifier::init(*SGLOBALS);
	Iterator::init(*SGLOBALS);
	Array::init(*SGLOBALS);
	Map::init(*SGLOBALS);
	Exception::init(*SGLOBALS);
	FnBinder::init(*SGLOBALS);
	Namespace::init(*SGLOBALS);
	Function::init(*SGLOBALS);
	UserFunction::init(*SGLOBALS);
	YieldIterator::init(*SGLOBALS);

	Runtime::init(*SGLOBALS);

	declareConstant(SGLOBALS,"SGLOBALS",SGLOBALS);

	// init EScript namespace
	Namespace * escript = new Namespace;
	declareConstant(SGLOBALS,"EScript",escript);

	declareConstant(escript,"VERSION",			ES_VERSION);
	declareConstant(escript,"VERSION_STRING",	ES_VERSION_STRING);

	// define attribute constants
	declareConstant(escript,"ATTR_NORMAL_ATTRIBUTE",	static_cast<uint32_t>(Attribute::NORMAL_ATTRIBUTE));
	declareConstant(escript,"ATTR_CONST_BIT",			static_cast<uint32_t>(Attribute::CONST_BIT));
	declareConstant(escript,"ATTR_PRIVATE_BIT",			static_cast<uint32_t>(Attribute::PRIVATE_BIT));
	declareConstant(escript,"ATTR_TYPE_ATTR_BIT",		static_cast<uint32_t>(Attribute::TYPE_ATTR_BIT));
	declareConstant(escript,"ATTR_INIT_BIT",			static_cast<uint32_t>(Attribute::INIT_BIT));
	declareConstant(escript,"ATTR_OVERRIDE_BIT",		static_cast<uint32_t>(Attribute::OVERRIDE_BIT));
	// -------------

	initLibrary(StdLib::init);
	initLibrary(IOLib::init);
	initLibrary(MathLib::init);
	#ifdef _WIN32
	initLibrary(Win32Lib::init);
	#endif
	#if defined(ES_THREADING)
	initLibrary(ThreadingLib::init);
	#endif // ES_THREADING
}

//! (static,internal)
 Namespace * getSGlobals() {
	static ERef<Namespace> sglobals(new Namespace);
	return sglobals.get();
}

//! (static)
void initLibrary(libInitFunction * initFunction) {
	(*initFunction)(getSGlobals());
}

}
