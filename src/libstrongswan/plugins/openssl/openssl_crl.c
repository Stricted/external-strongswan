/*
 * Copyright (C) 2010 Martin Willi
 * Copyright (C) 2010 revosec AG
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

/*
 * Copyright (C) 2010 secunet Security Networks AG
 * Copyright (C) 2010 Thomas Egerer
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "openssl_crl.h"
#include "openssl_util.h"

#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <debug.h>
#include <utils/enumerator.h>
#include <credentials/certificates/x509.h>

typedef struct private_openssl_crl_t private_openssl_crl_t;

/**
 * Private data of an openssl_crl_t object.
 */
struct private_openssl_crl_t {

	/**
	 * Public openssl_crl_t interface.
	 */
	openssl_crl_t public;

	/**
	 * OpenSSL representation of a CRL
	 */
	X509_CRL *crl;

	/**
	 * DER encoding of the CRL
	 */
	chunk_t encoding;

	/**
	 * Serial Number (crlNumber) of the CRL)
	 */
	chunk_t serial;

	/**
	 * AuthorityKeyIdentifier of the issuing CA
	 */
	chunk_t authKeyIdentifier;

	/**
	 * Date of this CRL
	 */
	time_t thisUpdate;

	/**
	 * Date of next CRL expected
	 */
	time_t nextUpdate;

	/**
	 * Issuer of this CRL
	 */
	identification_t *issuer;

	/**
	 * Signature scheme used in this CRL
	 */
	signature_scheme_t scheme;

	/**
	 * References to this CRL
	 */
	refcount_t ref;
};

/**
 * Enumerator over revoked certificates
 */
typedef struct {
	/**
	 * Implements enumerator_t
	 */
	enumerator_t public;

	/**
	 * stack of revoked certificates
	 */
	STACK_OF(X509_REVOKED) *stack;

	/**
	 * Total number of revoked certificates
	 */
	int num;

	/**
	 * Current position of enumerator
	 */
	int i;
} crl_enumerator_t;


METHOD(enumerator_t, crl_enumerate, bool,
	crl_enumerator_t *this, chunk_t *serial, time_t *date, crl_reason_t *reason)
{
	if (this->i < this->num)
	{
		X509_REVOKED *revoked;
		ASN1_ENUMERATED *crlrsn;

		revoked = sk_X509_REVOKED_value(this->stack, this->i);
		if (serial)
		{
			*serial = openssl_asn1_str2chunk(revoked->serialNumber);
		}
		if (date)
		{
			*date = openssl_asn1_to_time(revoked->revocationDate);
		}
		if (reason)
		{
			*reason = CRL_REASON_UNSPECIFIED;
			crlrsn = X509_REVOKED_get_ext_d2i(revoked, NID_crl_reason,
											  NULL, NULL);
			if (crlrsn)
			{
				if (ASN1_STRING_type(crlrsn) == V_ASN1_ENUMERATED &&
					ASN1_STRING_length(crlrsn) == 1)
				{
					*reason = *ASN1_STRING_data(crlrsn);
				}
				ASN1_STRING_free(crlrsn);
			}
		}
		this->i++;
		return TRUE;
	}
	return FALSE;
}

METHOD(crl_t, create_enumerator, enumerator_t*,
	private_openssl_crl_t *this)
{
	crl_enumerator_t *enumerator;

	INIT(enumerator,
		.public = {
			.enumerate = (void*)_crl_enumerate,
			.destroy = (void*)free,
		},
		.stack = X509_CRL_get_REVOKED(this->crl),
	);
	if (!enumerator->stack)
	{
		free(enumerator);
		return enumerator_create_empty();
	}
	enumerator->num = sk_X509_EXTENSION_num(enumerator->stack);
	return &enumerator->public;
}

METHOD(crl_t, get_serial, chunk_t,
	private_openssl_crl_t *this)
{
	return this->serial;
}

METHOD(crl_t, get_authKeyIdentifier, chunk_t,
	private_openssl_crl_t *this)
{
	return this->authKeyIdentifier;
}

METHOD(certificate_t, get_type, certificate_type_t,
	private_openssl_crl_t *this)
{
	return CERT_X509_CRL;
}

METHOD(certificate_t, get_subject_or_issuer, identification_t*,
	private_openssl_crl_t *this)
{
	return this->issuer;
}

METHOD(certificate_t, has_subject_or_issuer, id_match_t,
	private_openssl_crl_t *this, identification_t *id)
{
	if (id->get_type(id) == ID_KEY_ID &&
		chunk_equals(this->authKeyIdentifier, id->get_encoding(id)))
	{
		return ID_MATCH_PERFECT;
	}
	return this->issuer->matches(this->issuer, id);
}

METHOD(certificate_t, issued_by, bool,
	private_openssl_crl_t *this, certificate_t *issuer)
{
	chunk_t fingerprint, tbs;
	public_key_t *key;
	x509_t *x509;
	bool valid;

	if (issuer->get_type(issuer) != CERT_X509)
	{
		return FALSE;
	}
	x509 = (x509_t*)issuer;
	if (!(x509->get_flags(x509) & X509_CA))
	{
		return FALSE;
	}
	key = issuer->get_public_key(issuer);
	if (!key)
	{
		return FALSE;
	}
	if (this->authKeyIdentifier.ptr && key)
	{
		if (!key->get_fingerprint(key, KEYID_PUBKEY_SHA1, &fingerprint) ||
			!chunk_equals(fingerprint, this->authKeyIdentifier))
		{
			return FALSE;
		}
	}
	else
	{
		if (!this->issuer->equals(this->issuer, issuer->get_subject(issuer)))
		{
			return FALSE;
		}
	}
	if (this->scheme == SIGN_UNKNOWN)
	{
		return FALSE;
	}
	tbs = openssl_i2chunk(X509_CRL_INFO, this->crl->crl);
	valid = key->verify(key, this->scheme, tbs,
						openssl_asn1_str2chunk(this->crl->signature));
	free(tbs.ptr);
	key->destroy(key);
	return valid;
}

METHOD(certificate_t, get_public_key, public_key_t*,
	private_openssl_crl_t *this)
{
	return NULL;
}

METHOD(certificate_t, get_validity, bool,
	private_openssl_crl_t *this,
	time_t *when, time_t *not_before, time_t *not_after)
{
	time_t t = when ? *when : time(NULL);

	if (not_before)
	{
		*not_before = this->thisUpdate;
	}
	if (not_after)
	{
		*not_after = this->nextUpdate;
	}
	return t <= this->nextUpdate;
}

METHOD(certificate_t, get_encoding, chunk_t,
	private_openssl_crl_t *this)
{
	return chunk_clone(this->encoding);
}

METHOD(certificate_t, equals, bool,
	private_openssl_crl_t *this, certificate_t *other)
{
	chunk_t encoding;
	bool equal;

	if (&this->public.crl.certificate == other)
	{
		return TRUE;
	}
	if (other->equals == (void*)equals)
	{	/* skip allocation if we have the same implementation */
		return chunk_equals(this->encoding,
							((private_openssl_crl_t*)other)->encoding);
	}
	encoding = other->get_encoding(other);
	equal = chunk_equals(this->encoding, encoding);
	free(encoding.ptr);
	return equal;
}

METHOD(certificate_t, get_ref, certificate_t*,
	private_openssl_crl_t *this)
{
	ref_get(&this->ref);
	return &this->public.crl.certificate;
}

METHOD(certificate_t, destroy, void,
	private_openssl_crl_t *this)
{
	if (ref_put(&this->ref))
	{
		if (this->crl)
		{
			X509_CRL_free(this->crl);
		}
		DESTROY_IF(this->issuer);
		free(this->authKeyIdentifier.ptr);
		free(this->serial.ptr);
		free(this->encoding.ptr);
		free(this);
	}
}

/**
 * Create an empty CRL.
 */
static private_openssl_crl_t *create_empty()
{
	private_openssl_crl_t *this;

	INIT(this,
		.public = {
			.crl = {
				.certificate = {
					.get_type = _get_type,
					.get_subject = _get_subject_or_issuer,
					.get_issuer = _get_subject_or_issuer,
					.has_subject = _has_subject_or_issuer,
					.has_issuer = _has_subject_or_issuer,
					.issued_by = _issued_by,
					.get_public_key = _get_public_key,
					.get_validity = _get_validity,
					.get_encoding = _get_encoding,
					.equals = _equals,
					.get_ref = _get_ref,
					.destroy = _destroy,
				},
				.get_serial = _get_serial,
				.get_authKeyIdentifier = _get_authKeyIdentifier,
				.create_enumerator = _create_enumerator,
			},
		},
		.ref = 1,
	);
	return this;
}

/**
 * Parse the authKeyIdentifier extension
 */
static bool parse_authKeyIdentifier_ext(private_openssl_crl_t *this,
										X509_EXTENSION *ext)
{
	AUTHORITY_KEYID *keyid;

	keyid = (AUTHORITY_KEYID *)X509V3_EXT_d2i(ext);
	if (keyid)
	{
		free(this->authKeyIdentifier.ptr);
		this->authKeyIdentifier = chunk_clone(
						openssl_asn1_str2chunk(keyid->keyid));
		AUTHORITY_KEYID_free(keyid);
		return TRUE;
	}
	return FALSE;
}

/**
 * Parse the crlNumber extension
 */
static bool parse_crlNumber_ext(private_openssl_crl_t *this,
								X509_EXTENSION *ext)
{
	free(this->serial.ptr);
	this->serial = chunk_clone(
						openssl_asn1_str2chunk(X509_EXTENSION_get_data(ext)));
	return this->serial.len != 0;
}

/**
 * Parse X509 CRL extensions
 */
static bool parse_extensions(private_openssl_crl_t *this)
{
	bool ok;
	int i, num;
	X509_EXTENSION *ext;
	STACK_OF(X509_EXTENSION) *extensions;

	extensions = this->crl->crl->extensions;
	if (extensions)
	{
		num = sk_X509_EXTENSION_num(extensions);
		for (i = 0; i < num; ++i)
		{
			ext = sk_X509_EXTENSION_value(extensions, i);

			switch (OBJ_obj2nid(X509_EXTENSION_get_object(ext)))
			{
				case NID_authority_key_identifier:
					ok = parse_authKeyIdentifier_ext(this, ext);
					break;
				case NID_crl_number:
					ok = parse_crlNumber_ext(this, ext);
					break;
				default:
					ok = TRUE;
					break;
			}
			if (!ok)
			{
				return FALSE;
			}
		}
	}
	return TRUE;
}

/**
 * Parse a X509 CRL
 */
static bool parse_crl(private_openssl_crl_t *this)
{
	const unsigned char *ptr = this->encoding.ptr;

	this->crl = d2i_X509_CRL(NULL, &ptr, this->encoding.len);
	if (!this->crl)
	{
		return FALSE;
	}

	if (!chunk_equals(
			openssl_asn1_obj2chunk(this->crl->crl->sig_alg->algorithm),
			openssl_asn1_obj2chunk(this->crl->sig_alg->algorithm)))
	{
		return FALSE;
	}
	this->scheme = signature_scheme_from_oid(openssl_asn1_known_oid(
												this->crl->sig_alg->algorithm));

	this->issuer = openssl_x509_name2id(X509_CRL_get_issuer(this->crl));
	if (!this->issuer)
	{
		return FALSE;
	}
	this->thisUpdate = openssl_asn1_to_time(X509_CRL_get_lastUpdate(this->crl));
	this->nextUpdate = openssl_asn1_to_time(X509_CRL_get_nextUpdate(this->crl));

	return parse_extensions(this);
}

/**
 * Load the CRL.
 */
openssl_crl_t *openssl_crl_load(certificate_type_t type, va_list args)
{
	chunk_t blob = chunk_empty;

	while (TRUE)
	{
		switch (va_arg(args, builder_part_t))
		{
			case BUILD_BLOB_ASN1_DER:
				blob = va_arg(args, chunk_t);
				continue;
			case BUILD_END:
				break;
			default:
				return NULL;
		}
		break;
	}
	if (blob.ptr)
	{
		private_openssl_crl_t *this = create_empty();

		this->encoding = chunk_clone(blob);
		if (parse_crl(this))
		{
			return &this->public;
		}
		destroy(this);
	}
	return NULL;
}
