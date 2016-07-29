/* allowed.c - add allowed attributes based on ACL */
/* $ReOpenLDAP$ */
/* Copyright (c) 2015,2016 Leonid Yuriev <leo@yuriev.ru>.
 * Copyright (c) 2015,2016 Peter-Service R&D LLC <http://billing.ru/>.
 *
 * This file is part of ReOpenLDAP.
 *
 * ReOpenLDAP is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * ReOpenLDAP is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---
 *
 * Copyright 2006-2014 The OpenLDAP Foundation.
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
 * This work was initially developed by Pierangelo Masarati for inclusion in
 * OpenLDAP Software.
 */

/*
 * Rationale: return in allowedAttributes the attributes required/allowed
 * by the objectClasses that are currently present in an object; return
 * in allowedAttributesEffective the subset of the above that can be written
 * by the identity that performs the search.
 *
 * Caveats:
 * - right now, the overlay assumes that all values of the objectClass
 *   attribute will be returned in rs->sr_entry; this may not be true
 *   in general, but it usually is for back-bdb/back-hdb.  To generalize,
 *   the search request should be analyzed, and if allowedAttributes or
 *   allowedAttributesEffective are requested, add objectClass to the
 *   requested attributes
 * - it assumes that there is no difference between write-add and
 *   write-delete
 * - it assumes that access rules do not depend on the values of the
 *   attributes or on the contents of the entry (attr/val, filter, ...)
 *   allowedAttributes and allowedAttributesEffective cannot be used
 *   in filters or in compare
 */

#include "reldap.h"
#include "slap.h"

/*
 * NOTE: part of the schema definition reported below is taken
 * from Microsoft schema definitions (OID, NAME, SYNTAX);
 *
 * EQUALITY is taken from
 * <http://www.redhat.com/archives/fedora-directory-devel/2006-August/msg00007.html>
 * (posted by Andrew Bartlett)
 *
 * The rest is guessed.  Specifically
 *
 * DESC briefly describes the purpose
 *
 * NO-USER-MODIFICATION is added to make attributes operational
 *
 * USAGE is set to "dSAOperation" as per ITS#7493,
 * to prevent replication, since this information
 * is generated (based on ACL and identity of request)
 * and not stored.
 */

#define AA_SCHEMA_AT "1.2.840.113556.1.4"

static AttributeDescription
		*ad_allowedChildClasses,
		*ad_allowedChildClassesEffective,
		*ad_allowedAttributes,
		*ad_allowedAttributesEffective;

static struct {
	char *at;
	AttributeDescription **ad;
} aa_attrs[] = {
	{ "( " AA_SCHEMA_AT ".911 "
		"NAME 'allowedChildClasses' "
		"EQUALITY objectIdentifierMatch "
		"SYNTAX 1.3.6.1.4.1.1466.115.121.1.38 "
		/* added by me :) */
		"DESC 'Child classes allowed for a given object' "
		"NO-USER-MODIFICATION "
		"USAGE dSAOperation )", &ad_allowedChildClasses },
	{ "( " AA_SCHEMA_AT ".912 "
		"NAME 'allowedChildClassesEffective' "
		"EQUALITY objectIdentifierMatch "
		"SYNTAX 1.3.6.1.4.1.1466.115.121.1.38 "
		/* added by me :) */
		"DESC 'Child classes allowed for a given object according to ACLs' "
		"NO-USER-MODIFICATION "
		"USAGE dSAOperation )", &ad_allowedChildClassesEffective },
	{ "( " AA_SCHEMA_AT ".913 "
		"NAME 'allowedAttributes' "
		"EQUALITY objectIdentifierMatch "
		"SYNTAX 1.3.6.1.4.1.1466.115.121.1.38 "
		/* added by me :) */
		"DESC 'Attributes allowed for a given object' "
		"NO-USER-MODIFICATION "
		"USAGE dSAOperation )", &ad_allowedAttributes },
	{ "( " AA_SCHEMA_AT ".914 "
		"NAME 'allowedAttributesEffective' "
		"EQUALITY objectIdentifierMatch "
		"SYNTAX 1.3.6.1.4.1.1466.115.121.1.38 "
		/* added by me :) */
		"DESC 'Attributes allowed for a given object according to ACLs' "
		"NO-USER-MODIFICATION "
		"USAGE dSAOperation )", &ad_allowedAttributesEffective },

	/* TODO: add objectClass stuff? */

	{ NULL, NULL }
};

static int
aa_add_at( AttributeType *at, AttributeType ***atpp )
{
	int		i = 0;

	if ( *atpp ) {
		for ( i = 0; (*atpp)[ i ] != NULL; i++ ) {
			if ( (*atpp)[ i ] == at ) {
				break;
			}
		}

		if ( (*atpp)[ i ] != NULL ) {
			return 0;
		}
	}

	*atpp = ch_realloc( *atpp, sizeof( AttributeType * ) * ( i + 2 ) );
	(*atpp)[ i ] = at;
	(*atpp)[ i + 1 ] = NULL;

	return 0;
}

static int
aa_add_oc( ObjectClass *oc, ObjectClass ***ocpp, AttributeType ***atpp )
{
	int		i = 0;

	if ( *ocpp ) {
		for ( ; (*ocpp)[ i ] != NULL; i++ ) {
			if ( (*ocpp)[ i ] == oc ) {
				break;
			}
		}

		if ( (*ocpp)[ i ] != NULL ) {
			return 0;
		}
	}

	*ocpp = ch_realloc( *ocpp, sizeof( ObjectClass * ) * ( i + 2 ) );
	(*ocpp)[ i ] = oc;
	(*ocpp)[ i + 1 ] = NULL;

	if ( oc->soc_required ) {
		int		i;

		for ( i = 0; oc->soc_required[ i ] != NULL; i++ ) {
			aa_add_at( oc->soc_required[ i ], atpp );
		}
	}

	if ( oc->soc_allowed ) {
		int		i;

		for ( i = 0; oc->soc_allowed[ i ] != NULL; i++ ) {
			aa_add_at( oc->soc_allowed[ i ], atpp );
		}
	}

	return 0;
}

static int
aa_operational( Operation *op, SlapReply *rs )
{
	Attribute		*a, **ap;
	AccessControlState	acl_state = ACL_STATE_INIT;
	struct berval		*v;
	AttributeType		**atp = NULL;
	ObjectClass		**ocp = NULL;

#define	GOT_NONE	(0x0U)
#define	GOT_C		(0x1U)
#define	GOT_CE		(0x2U)
#define	GOT_A		(0x4U)
#define	GOT_AE		(0x8U)
#define	GOT_ALL		(GOT_C|GOT_CE|GOT_A|GOT_AE)
	int		got = GOT_NONE;

	/* only add if requested */
	if ( SLAP_OPATTRS( rs->sr_attr_flags ) ) {
		got = GOT_ALL;

	} else {
		if ( ad_inlist( ad_allowedChildClasses, rs->sr_attrs ) ) {
			got |= GOT_C;
		}

		if ( ad_inlist( ad_allowedChildClassesEffective, rs->sr_attrs ) ) {
			got |= GOT_CE;
		}

		if ( ad_inlist( ad_allowedAttributes, rs->sr_attrs ) ) {
			got |= GOT_A;
		}

		if ( ad_inlist( ad_allowedAttributesEffective, rs->sr_attrs ) ) {
			got |= GOT_AE;
		}
	}

	if ( got == GOT_NONE ) {
		return SLAP_CB_CONTINUE;
	}

	/* shouldn't be called without an entry; please check */
	assert( rs->sr_entry != NULL );

	for ( ap = &rs->sr_operational_attrs; *ap != NULL; ap = &(*ap)->a_next )
		/* go to last */ ;

	/* see caveats; this is not guaranteed for all backends */
	a = attr_find( rs->sr_entry->e_attrs, slap_schema.si_ad_objectClass );
	if ( a == NULL ) {
		goto do_oc;
	}

	/* if client has no access to objectClass attribute; don't compute */
	if ( !access_allowed( op, rs->sr_entry, slap_schema.si_ad_objectClass,
				NULL, ACL_READ, &acl_state ) )
	{
		return SLAP_CB_CONTINUE;
	}

	for ( v = a->a_nvals; !BER_BVISNULL( v ); v++ ) {
		ObjectClass	*oc = oc_bvfind( v );

		assert( oc != NULL );

		/* if client has no access to specific value, don't compute */
		if ( !access_allowed( op, rs->sr_entry,
			slap_schema.si_ad_objectClass,
			&oc->soc_cname, ACL_READ, &acl_state ) )
		{
			continue;
		}

		aa_add_oc( oc, &ocp, &atp );

		if ( oc->soc_sups ) {
			int i;

			for ( i = 0; oc->soc_sups[ i ] != NULL; i++ ) {
				aa_add_oc( oc->soc_sups[ i ], &ocp, &atp );
			}
		}
	}

	ch_free( ocp );

	if ( atp != NULL ) {
		BerVarray	bv_allowed = NULL,
				bv_effective = NULL;
		int		i, ja = 0, je = 0;

		for ( i = 0; atp[ i ] != NULL; i++ )
			/* just count */ ;

		if ( got & GOT_A ) {
			bv_allowed = ber_memalloc( sizeof( struct berval ) * ( i + 1 ) );
		}
		if ( got & GOT_AE ) {
			bv_effective = ber_memalloc( sizeof( struct berval ) * ( i + 1 ) );
		}

		for ( i = 0, ja = 0, je = 0; atp[ i ] != NULL; i++ ) {
			if ( got & GOT_A ) {
				ber_dupbv( &bv_allowed[ ja ], &atp[ i ]->sat_cname );
				ja++;
			}

			if ( got & GOT_AE ) {
				AttributeDescription	*ad = NULL;
				const char		*text = NULL;

				if ( slap_bv2ad( &atp[ i ]->sat_cname, &ad, &text ) ) {
					/* log? */
					continue;
				}

				if ( access_allowed( op, rs->sr_entry,
					ad, NULL, ACL_WRITE, NULL ) )
				{
					ber_dupbv( &bv_effective[ je ], &atp[ i ]->sat_cname );
					je++;
				}
			}
		}

		ch_free( atp );

		if ( ( got & GOT_A ) && ja > 0 ) {
			BER_BVZERO( &bv_allowed[ ja ] );
			*ap = attr_alloc( ad_allowedAttributes );
			(*ap)->a_vals = bv_allowed;
			(*ap)->a_nvals = bv_allowed;
			(*ap)->a_numvals = ja;
			ap = &(*ap)->a_next;
		}

		if ( ( got & GOT_AE ) && je > 0 ) {
			BER_BVZERO( &bv_effective[ je ] );
			*ap = attr_alloc( ad_allowedAttributesEffective );
			(*ap)->a_vals = bv_effective;
			(*ap)->a_nvals = bv_effective;
			(*ap)->a_numvals = je;
			ap = &(*ap)->a_next;
		}

		*ap = NULL;
	}

do_oc:;
	if ( ( got & GOT_C ) || ( got & GOT_CE ) ) {
		BerVarray	bv_allowed = NULL,
				bv_effective = NULL;
		int		i = 0, ja = 0, je = 0;

		ObjectClass	*oc;

		for ( oc_start( &oc ); oc != NULL; oc_next( &oc ) ) {
			/* we can only add AUXILIARY objectClasses */
			if ( oc->soc_kind != LDAP_SCHEMA_AUXILIARY ) {
				continue;
			}

			i++;
		}

		if ( got & GOT_C ) {
			bv_allowed = ber_memalloc( sizeof( struct berval ) * ( i + 1 ) );
		}
		if ( got & GOT_CE ) {
			bv_effective = ber_memalloc( sizeof( struct berval ) * ( i + 1 ) );
		}

		for ( oc_start( &oc ); oc != NULL; oc_next( &oc ) ) {
			/* we can only add AUXILIARY objectClasses */
			if ( oc->soc_kind != LDAP_SCHEMA_AUXILIARY ) {
				continue;
			}

			if ( got & GOT_C ) {
				ber_dupbv( &bv_allowed[ ja ], &oc->soc_cname );
				ja++;
			}

			if ( got & GOT_CE ) {
				if ( !access_allowed( op, rs->sr_entry,
					slap_schema.si_ad_objectClass,
					&oc->soc_cname, ACL_WRITE, NULL ) )
				{
					goto done_ce;
				}

				if ( oc->soc_required ) {
					for ( i = 0; oc->soc_required[ i ] != NULL; i++ ) {
						AttributeDescription	*ad = NULL;
						const char		*text = NULL;

						if ( slap_bv2ad( &oc->soc_required[ i ]->sat_cname, &ad, &text ) ) {
							/* log? */
							continue;
						}

						if ( !access_allowed( op, rs->sr_entry,
							ad, NULL, ACL_WRITE, NULL ) )
						{
							goto done_ce;
						}
					}
				}

				ber_dupbv( &bv_effective[ je ], &oc->soc_cname );
				je++;
			}
done_ce:;
		}

		if ( ( got & GOT_C ) && ja > 0 ) {
			BER_BVZERO( &bv_allowed[ ja ] );
			*ap = attr_alloc( ad_allowedChildClasses );
			(*ap)->a_vals = bv_allowed;
			(*ap)->a_nvals = bv_allowed;
			(*ap)->a_numvals = ja;
			ap = &(*ap)->a_next;
		}

		if ( ( got & GOT_CE ) && je > 0 ) {
			BER_BVZERO( &bv_effective[ je ] );
			*ap = attr_alloc( ad_allowedChildClassesEffective );
			(*ap)->a_vals = bv_effective;
			(*ap)->a_nvals = bv_effective;
			(*ap)->a_numvals = je;
			ap = &(*ap)->a_next;
		}

		*ap = NULL;
	}

	return SLAP_CB_CONTINUE;
}

static slap_overinst aa;

static int
aa_initialize( void )
{
	int i;

	aa.on_bi.bi_type = "allowed";

	aa.on_bi.bi_operational = aa_operational;

	/* aa schema integration */
	for ( i = 0; aa_attrs[i].at; i++ ) {
		int code;

		code = register_at( aa_attrs[i].at, aa_attrs[i].ad, 0 );
		if ( code ) {
			Debug( LDAP_DEBUG_ANY,
				"aa_initialize: register_at failed\n" );
			return -1;
		}
	}

	return overlay_register( &aa );
}

SLAP_MODULE_ENTRY(allowed, modinit) ( int argc, char *argv[] )
{
	return aa_initialize();
}
