/* unbind.c - decode an ldap unbind operation and pass it to a backend db */

/*
 * Copyright (c) 1995 Regents of the University of Michigan.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that this notice is preserved and that due credit is given
 * to the University of Michigan at Ann Arbor. The name of the University
 * may not be used to endorse or promote products derived from this
 * software without specific prior written permission. This software
 * is provided ``as is'' without express or implied warranty.
 *
 */

#include "portable.h"

#include <stdio.h>

#include <ac/socket.h>

#include "slap.h"


void
do_unbind(
    Connection	*conn,
    Operation	*op
)
{
	Debug( LDAP_DEBUG_TRACE, "do_unbind\n", 0, 0, 0 );

	/*
	 * Parse the unbind request.  It looks like this:
	 *
	 *	UnBindRequest ::= NULL
	 */

	Statslog( LDAP_DEBUG_STATS, "conn=%d op=%d UNBIND\n", conn->c_connid,
	    op->o_opid, 0, 0, 0 );

	/* pass the unbind to all backends */
	backend_unbind( conn, op );
	
	/* close the connection to the client */
	close_connection( conn, op->o_connid, op->o_opid );
}
