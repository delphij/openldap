/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 1999-2005 The OpenLDAP Foundation.
 * Portions Copyright 1999 Dmitry Kovalev.
 * Portions Copyright 2002 Pierangelo Masarati.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */
/* ACKNOWLEDGEMENTS:
 * This work was initially developed by Dmitry Kovalev for inclusion
 * by OpenLDAP Software.  Additional significant contributors include
 * Pierangelo Masarati.
 */

#include "portable.h"

#include <stdio.h>
#include <sys/types.h>
#include "ac/string.h"

#include "slap.h"
#include "proto-sql.h"

int
backsql_modrdn( Operation *op, SlapReply *rs )
{
	backsql_info		*bi = (backsql_info*)op->o_bd->be_private;
	SQLHDBC			dbh = SQL_NULL_HDBC;
	SQLHSTMT		sth = SQL_NULL_HSTMT;
	RETCODE			rc;
	backsql_entryID		e_id = BACKSQL_ENTRYID_INIT,
				n_id = BACKSQL_ENTRYID_INIT;
	backsql_srch_info	bsi = { 0 };
	backsql_oc_map_rec	*oc = NULL;
	struct berval		pdn = BER_BVNULL, pndn = BER_BVNULL,
				*new_pdn = NULL, *new_npdn = NULL,
				new_dn = BER_BVNULL, new_ndn = BER_BVNULL,
				realnew_dn = BER_BVNULL;
	LDAPRDN			new_rdn = NULL;
	LDAPRDN			old_rdn = NULL;
	Entry			r = { 0 },
				p = { 0 },
				n = { 0 },
				*e = NULL;
	int			manageDSAit = get_manageDSAit( op );
	Modifications		*mod = NULL;
	struct berval		*newSuperior = op->oq_modrdn.rs_newSup;
	char			*next;
 
	Debug( LDAP_DEBUG_TRACE, "==>backsql_modrdn() renaming entry \"%s\", "
			"newrdn=\"%s\", newSuperior=\"%s\"\n",
			op->o_req_dn.bv_val, op->oq_modrdn.rs_newrdn.bv_val, 
			newSuperior ? newSuperior->bv_val : "(NULL)" );

	rs->sr_err = backsql_get_db_conn( op, &dbh );
	if ( rs->sr_err != LDAP_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"could not get connection handle - exiting\n", 
			0, 0, 0 );
		rs->sr_text = ( rs->sr_err == LDAP_OTHER )
			?  "SQL-backend error" : NULL;
		e = NULL;
		goto done;
	}

	bsi.bsi_e = &r;
	rs->sr_err = backsql_init_search( &bsi, &op->o_req_ndn,
			LDAP_SCOPE_BASE, 
			SLAP_NO_LIMIT, SLAP_NO_LIMIT,
			(time_t)(-1), NULL, dbh, op, rs,
			slap_anlist_all_attributes,
			( BACKSQL_ISF_MATCHED | BACKSQL_ISF_GET_ENTRY ) );
	switch ( rs->sr_err ) {
	case LDAP_SUCCESS:
		break;

	case LDAP_REFERRAL:
		if ( !BER_BVISNULL( &bsi.bsi_e->e_nname ) &&
				dn_match( &op->o_req_ndn, &bsi.bsi_e->e_nname )
				&& manageDSAit )
		{
			rs->sr_err = LDAP_SUCCESS;
			rs->sr_text = NULL;
			rs->sr_matched = NULL;
			if ( rs->sr_ref ) {
				ber_bvarray_free( rs->sr_ref );
				rs->sr_ref = NULL;
			}
			break;
		}
		e = &r;
		/* fallthru */

	default:
		Debug( LDAP_DEBUG_TRACE, "backsql_modrdn(): "
			"could not retrieve modrdnDN ID - no such entry\n", 
			0, 0, 0 );
		if ( !BER_BVISNULL( &r.e_nname ) ) {
			/* FIXME: should always be true! */
			e = &r;

		} else {
			e = NULL;
		}
		goto done;
	}

#ifdef BACKSQL_ARBITRARY_KEY
	Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): entry id=%s\n",
		e_id.eid_id.bv_val, 0, 0 );
#else /* ! BACKSQL_ARBITRARY_KEY */
	Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): entry id=%ld\n",
		e_id.eid_id, 0, 0 );
#endif /* ! BACKSQL_ARBITRARY_KEY */

	if ( get_assert( op ) &&
			( test_filter( op, &r, get_assertion( op ) )
			  != LDAP_COMPARE_TRUE ) )
	{
		rs->sr_err = LDAP_ASSERTION_FAILED;
		e = &r;
		goto done;
	}

	if ( backsql_has_children( bi, dbh, &op->o_req_ndn ) == LDAP_COMPARE_TRUE ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"entry \"%s\" has children\n",
			op->o_req_dn.bv_val, 0, 0 );
		rs->sr_err = LDAP_NOT_ALLOWED_ON_NONLEAF;
		rs->sr_text = "subtree rename not supported";
		e = &r;
		goto done;
	}

	/*
	 * Check for entry access to target
	 */
	if ( !access_allowed( op, &r, slap_schema.si_ad_entry, 
				NULL, ACL_WRITE, NULL ) ) {
		Debug( LDAP_DEBUG_TRACE, "   no access to entry\n", 0, 0, 0 );
		rs->sr_err = LDAP_INSUFFICIENT_ACCESS;
		goto done;
	}

	dnParent( &op->o_req_dn, &pdn );
	dnParent( &op->o_req_ndn, &pndn );

	/*
	 * namingContext "" is not supported
	 */
	if ( BER_BVISEMPTY( &pdn ) ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"parent is \"\" - aborting\n", 0, 0, 0 );
		rs->sr_err = LDAP_UNWILLING_TO_PERFORM;
		rs->sr_text = "not allowed within namingContext";
		e = NULL;
		goto done;
	}

	/*
	 * Check for children access to parent
	 */
	bsi.bsi_e = &p;
	e_id = bsi.bsi_base_id;
	rs->sr_err = backsql_init_search( &bsi, &pndn,
			LDAP_SCOPE_BASE, 
			SLAP_NO_LIMIT, SLAP_NO_LIMIT,
			(time_t)(-1), NULL, dbh, op, rs,
			slap_anlist_all_attributes,
			BACKSQL_ISF_GET_ENTRY );

#ifdef BACKSQL_ARBITRARY_KEY
	Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
		"old parent entry id is %s\n",
		bsi.bsi_base_id.eid_id.bv_val, 0, 0 );
#else /* ! BACKSQL_ARBITRARY_KEY */
	Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
		"old parent entry id is %ld\n",
		bsi.bsi_base_id.eid_id, 0, 0 );
#endif /* ! BACKSQL_ARBITRARY_KEY */

	if ( rs->sr_err != LDAP_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "backsql_modrdn(): "
			"could not retrieve renameDN ID - no such entry\n", 
			0, 0, 0 );
		e = &p;
		goto done;
	}

	if ( !access_allowed( op, &p, slap_schema.si_ad_children, 
				NULL, ACL_WRITE, NULL ) ) {
		Debug( LDAP_DEBUG_TRACE, "   no access to parent\n", 0, 0, 0 );
		rs->sr_err = LDAP_INSUFFICIENT_ACCESS;
		goto done;
	}

	if ( newSuperior ) {
		(void)backsql_free_entryID( op, &bsi.bsi_base_id, 0 );
		
		/*
		 * namingContext "" is not supported
		 */
		if ( BER_BVISEMPTY( newSuperior ) ) {
			Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
				"newSuperior is \"\" - aborting\n", 0, 0, 0 );
			rs->sr_err = LDAP_UNWILLING_TO_PERFORM;
			rs->sr_text = "not allowed within namingContext";
			e = NULL;
			goto done;
		}

		new_pdn = newSuperior;
		new_npdn = op->oq_modrdn.rs_nnewSup;

		/*
		 * Check for children access to new parent
		 */
		bsi.bsi_e = &n;
		rs->sr_err = backsql_init_search( &bsi, new_npdn,
				LDAP_SCOPE_BASE, 
				SLAP_NO_LIMIT, SLAP_NO_LIMIT,
				(time_t)(-1), NULL, dbh, op, rs,
				slap_anlist_all_attributes,
				( BACKSQL_ISF_MATCHED | BACKSQL_ISF_GET_ENTRY ) );
		if ( rs->sr_err != LDAP_SUCCESS ) {
			Debug( LDAP_DEBUG_TRACE, "backsql_modrdn(): "
				"could not retrieve renameDN ID - no such entry\n", 
				0, 0, 0 );
			e = &n;
			goto done;
		}

		n_id = bsi.bsi_base_id;

#ifdef BACKSQL_ARBITRARY_KEY
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"new parent entry id=%s\n",
			n_id.eid_id.bv_val, 0, 0 );
#else /* ! BACKSQL_ARBITRARY_KEY */
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"new parent entry id=%ld\n",
			n_id.eid_id, 0, 0 );
#endif /* ! BACKSQL_ARBITRARY_KEY */

		if ( !access_allowed( op, &n, slap_schema.si_ad_children, 
					NULL, ACL_WRITE, NULL ) ) {
			Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
					"no access to new parent \"%s\"\n", 
					new_pdn->bv_val, 0, 0 );
			rs->sr_err = LDAP_INSUFFICIENT_ACCESS;
			e = &n;
			goto done;
		}

	} else {
		n_id = bsi.bsi_base_id;
		new_pdn = &pdn;
		new_npdn = &pndn;
	}

	if ( newSuperior && dn_match( &pndn, new_npdn ) ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"newSuperior is equal to old parent - ignored\n",
			0, 0, 0 );
		newSuperior = NULL;
	}

	if ( newSuperior && dn_match( &op->o_req_ndn, new_npdn ) ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"newSuperior is equal to entry being moved "
			"- aborting\n", 0, 0, 0 );
		rs->sr_err = LDAP_OTHER;
		rs->sr_text = "newSuperior is equal to old DN";
		e = &r;
		goto done;
	}

	build_new_dn( &new_dn, new_pdn, &op->oq_modrdn.rs_newrdn,
			op->o_tmpmemctx );
	build_new_dn( &new_ndn, new_npdn, &op->oq_modrdn.rs_nnewrdn,
			op->o_tmpmemctx );
	
	Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): new entry dn is \"%s\"\n",
			new_dn.bv_val, 0, 0 );

 
	Debug(	LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
		"executing delentry_stmt\n", 0, 0, 0 );

	rc = backsql_Prepare( dbh, &sth, bi->sql_delentry_stmt, 0 );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE,
			"   backsql_modrdn(): "
			"error preparing delentry_stmt\n", 0, 0, 0 );
		backsql_PrintErrors( bi->sql_db_env, dbh, 
				sth, rc );

		rs->sr_text = "SQL-backend error";
		rs->sr_err = LDAP_OTHER;
		e = NULL;
		goto done;
	}

	rc = backsql_BindParamID( sth, 1, SQL_PARAM_INPUT, &e_id.eid_id );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE,
			"   backsql_delete(): "
			"error binding entry ID parameter "
			"for objectClass %s\n",
			oc->bom_oc->soc_cname.bv_val, 0, 0 );
		backsql_PrintErrors( bi->sql_db_env, dbh, sth, rc );
		SQLFreeStmt( sth, SQL_DROP );

		rs->sr_text = "SQL-backend error";
		rs->sr_err = LDAP_OTHER;
		e = NULL;
		goto done;
	}

	rc = SQLExecute( sth );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"failed to delete record from ldap_entries\n",
			0, 0, 0 );
		backsql_PrintErrors( bi->sql_db_env, dbh, sth, rc );
		SQLFreeStmt( sth, SQL_DROP );
		rs->sr_err = LDAP_OTHER;
		rs->sr_text = "SQL-backend error";
		e = NULL;
		goto done;
	}

	SQLFreeStmt( sth, SQL_DROP );

	Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
		"executing insentry_stmt\n", 0, 0, 0 );

	rc = backsql_Prepare( dbh, &sth, bi->sql_insentry_stmt, 0 );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE,
			"   backsql_modrdn(): "
			"error preparing insentry_stmt\n", 0, 0, 0 );
		backsql_PrintErrors( bi->sql_db_env, dbh, 
				sth, rc );

		rs->sr_text = "SQL-backend error";
		rs->sr_err = LDAP_OTHER;
		e = NULL;
		goto done;
	}

	realnew_dn = new_dn;
	if ( backsql_api_dn2odbc( op, rs, &realnew_dn ) ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(\"%s\"): "
			"backsql_api_dn2odbc(\"%s\") failed\n", 
			op->o_req_dn.bv_val, realnew_dn.bv_val, 0 );
		SQLFreeStmt( sth, SQL_DROP );

		rs->sr_text = "SQL-backend error";
		rs->sr_err = LDAP_OTHER;
		e = NULL;
		goto done;
	}

	rc = backsql_BindParamBerVal( sth, 1, SQL_PARAM_INPUT, &realnew_dn );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE,
			"   backsql_add_attr(): "
			"error binding DN parameter for objectClass %s\n",
			oc->bom_oc->soc_cname.bv_val, 0, 0 );
		backsql_PrintErrors( bi->sql_db_env, dbh, 
			sth, rc );
		SQLFreeStmt( sth, SQL_DROP );

		rs->sr_text = "SQL-backend error";
		rs->sr_err = LDAP_OTHER;
		e = NULL;
		goto done;
	}

	rc = backsql_BindParamInt( sth, 2, SQL_PARAM_INPUT, &e_id.eid_oc_id );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE,
			"   backsql_add_attr(): "
			"error binding objectClass ID parameter for objectClass %s\n",
			oc->bom_oc->soc_cname.bv_val, 0, 0 );
		backsql_PrintErrors( bi->sql_db_env, dbh, 
			sth, rc );
		SQLFreeStmt( sth, SQL_DROP );

		rs->sr_text = "SQL-backend error";
		rs->sr_err = LDAP_OTHER;
		e = NULL;
		goto done;
	}

	rc = backsql_BindParamID( sth, 3, SQL_PARAM_INPUT, &n_id.eid_id );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE,
			"   backsql_add_attr(): "
			"error binding parent ID parameter for objectClass %s\n",
			oc->bom_oc->soc_cname.bv_val, 0, 0 );
		backsql_PrintErrors( bi->sql_db_env, dbh, 
			sth, rc );
		SQLFreeStmt( sth, SQL_DROP );

		rs->sr_text = "SQL-backend error";
		rs->sr_err = LDAP_OTHER;
		e = NULL;
		goto done;
	}

	rc = backsql_BindParamID( sth, 4, SQL_PARAM_INPUT, &e_id.eid_keyval );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE,
			"   backsql_add_attr(): "
			"error binding entry ID parameter for objectClass %s\n",
			oc->bom_oc->soc_cname.bv_val, 0, 0 );
		backsql_PrintErrors( bi->sql_db_env, dbh, 
			sth, rc );
		SQLFreeStmt( sth, SQL_DROP );

		rs->sr_text = "SQL-backend error";
		rs->sr_err = LDAP_OTHER;
		e = NULL;
		goto done;
	}

	rc = SQLExecute( sth );
	if ( rc != SQL_SUCCESS ) {
		Debug( LDAP_DEBUG_TRACE, "   backsql_modrdn(): "
			"could not insert ldap_entries record\n", 0, 0, 0 );
		backsql_PrintErrors( bi->sql_db_env, dbh, sth, rc );
		SQLFreeStmt( sth, SQL_DROP );
		rs->sr_err = LDAP_OTHER;
		rs->sr_text = "SQL-backend error";
		e = NULL;
		goto done;
	}
	SQLFreeStmt( sth, SQL_DROP );

	/*
	 * Get attribute type and attribute value of our new rdn,
	 * we will need to add that to our new entry
	 */
	if ( ldap_bv2rdn( &op->oq_modrdn.rs_newrdn, &new_rdn, &next, 
				LDAP_DN_FORMAT_LDAP ) )
	{
		Debug( LDAP_DEBUG_TRACE,
			"   backsql_modrdn: can't figure out "
			"type(s)/values(s) of new_rdn\n", 
			0, 0, 0 );
		rs->sr_err = LDAP_INVALID_DN_SYNTAX;
		e = &r;
		goto done;
	}

	Debug( LDAP_DEBUG_TRACE, "backsql_modrdn: "
		"new_rdn_type=\"%s\", new_rdn_val=\"%s\"\n",
		new_rdn[ 0 ]->la_attr.bv_val,
		new_rdn[ 0 ]->la_value.bv_val, 0 );

	if ( op->oq_modrdn.rs_deleteoldrdn ) {
		if ( ldap_bv2rdn( &op->o_req_dn, &old_rdn, &next,
					LDAP_DN_FORMAT_LDAP ) )
		{
			Debug( LDAP_DEBUG_TRACE,
				"   backsql_modrdn: can't figure out "
				"the old_rdn type(s)/value(s)\n", 
				0, 0, 0 );
			rs->sr_err = LDAP_OTHER;
			e = NULL;
			goto done;
		}
	}

	rs->sr_err = slap_modrdn2mods( op, rs, &r, old_rdn, new_rdn, &mod );
	if ( rs->sr_err != LDAP_SUCCESS ) {
		e = &r;
		goto done;
	}

	oc = backsql_id2oc( bi, e_id.eid_oc_id );
	rs->sr_err = backsql_modify_internal( op, rs, dbh, oc, &e_id, mod );
	e = &r;

done:;
#ifdef SLAP_ACL_HONOR_DISCLOSE
	if ( e != NULL ) {
		if ( !access_allowed( op, e, slap_schema.si_ad_entry, NULL,
					ACL_DISCLOSE, NULL ) )
		{
			rs->sr_err = LDAP_NO_SUCH_OBJECT;
			rs->sr_text = NULL;
			rs->sr_matched = NULL;
			if ( rs->sr_ref ) {
				ber_bvarray_free( rs->sr_ref );
				rs->sr_ref = NULL;
			}
		}
	}
#endif /* SLAP_ACL_HONOR_DISCLOSE */

	send_ldap_result( op, rs );

	/*
	 * Commit only if all operations succeed
	 */
	if ( sth != SQL_NULL_HSTMT ) {
		SQLUSMALLINT	CompletionType = SQL_ROLLBACK;
	
		if ( rs->sr_err == LDAP_SUCCESS && !op->o_noop ) {
			CompletionType = SQL_COMMIT;
		}

		SQLTransact( SQL_NULL_HENV, dbh, CompletionType );
	}

	if ( !BER_BVISNULL( &realnew_dn ) && realnew_dn.bv_val != new_dn.bv_val ) {
		ch_free( realnew_dn.bv_val );
	}

	if ( !BER_BVISNULL( &new_dn ) ) {
		slap_sl_free( new_dn.bv_val, op->o_tmpmemctx );
	}
	
	if ( !BER_BVISNULL( &new_ndn ) ) {
		slap_sl_free( new_ndn.bv_val, op->o_tmpmemctx );
	}
	
	/* LDAP v2 supporting correct attribute handling. */
	if ( new_rdn != NULL ) {
		ldap_rdnfree( new_rdn );
	}
	if ( old_rdn != NULL ) {
		ldap_rdnfree( old_rdn );
	}
	if ( mod != NULL ) {
		Modifications *tmp;
		for (; mod; mod = tmp ) {
			tmp = mod->sml_next;
			free( mod );
		}
	}

	if ( !BER_BVISNULL( &e_id.eid_ndn ) ) {
		(void)backsql_free_entryID( op, &e_id, 0 );
	}

	if ( !BER_BVISNULL( &n_id.eid_ndn ) ) {
		(void)backsql_free_entryID( op, &n_id, 0 );
	}

	if ( !BER_BVISNULL( &r.e_nname ) ) {
		entry_clean( &r );
	}

	if ( !BER_BVISNULL( &p.e_nname ) ) {
		entry_clean( &p );
	}

	if ( !BER_BVISNULL( &n.e_nname ) ) {
		entry_clean( &n );
	}

	Debug( LDAP_DEBUG_TRACE, "<==backsql_modrdn()\n", 0, 0, 0 );

	return rs->sr_err;
}

