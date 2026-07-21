/***************************************************************************
                          exc.h  -  description
                             -------------------
    begin                : Fri Jan 12 2001
    copyright            : (C) 2001 by Timo Weggen
    email                : tweggen@cybernoia.de
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef _EXC_H_
#define _EXC_H_

// _EXC_USE_IOSTREAM_ selected between <iostream.h> (the pre-standard header)
// and stdio; it was never defined anywhere, so the iostream arm was dead and
// would not have compiled. Diagnostics now go through the log sink.
#include "tw/core/twlog.h"

class excStandard {
private:
	/* error message */
	const char *errMsg;

public:
	virtual const char *getMsg() const
		{ return errMsg; }
	excStandard (const char *s)
	{
		TW_LOGE( "core", "Exception: %s", s );
		errMsg = s;
	}


	virtual ~excStandard() {};
};

#define DEFINE_EXC_CLASS( x ) \
	class x : public excStandard {\
	public: x( const char *s )	: excStandard( s ) {} }


DEFINE_EXC_CLASS( excNoMemory );
DEFINE_EXC_CLASS( excInternalInvArg );

#endif
