// DefaultComparisonOperatorsTrait.escript
// This file is part of the EScript programming language (http://escript.berlios.de)
//
// Copyright (C) 2013-2014 Claudius Jähn <claudiusj@live.de>
//
// Licensed under the MIT License. See LICENSE file for details.
// ---------------------------------------------------------------------------------

/*!sets up default compare functions for all objects ( <, >, <=, >=, ==, != )
 * @param smaller the < function has to be given as parameter, all others are redirected to it.
 * @note this changes the behavior of == and != which no longer behave like === and !==
 */
var GenericTrait = require('./GenericTrait');
var t = new GenericTrait("Std.Traits.DefaultComparisonOperatorsTrait");

t.attributes.'<'	::= fn(b){
	assert( b ---|> __compare_baseType , "tried to compare objects of different type.");
	return 	__compare_isSmaller(b);
};
t.attributes.'<='	::= fn(b){ return !(b < this);                };
t.attributes.'>='	::= fn(b){ return !(this < b);                };
t.attributes.'>'	::= fn(b){ return  (b < this);                };
t.attributes.'=='	::= fn(b){ return !(this < b) && !(b < this); };
t.attributes.'!='	::= fn(b){ return  (this < b) ||  (b < this); };

t.onInit += fn(Type t,smaller){
	t.__compare_isSmaller @(private) ::= smaller;
	t.__compare_baseType @(private) ::= t; // store the calling Type as basetype. Objects of this type (or of a subtype) can be compared.
};



var Traits = require('./basics');
Traits.DefaultComparisonOperatorsTrait := t;
return t;