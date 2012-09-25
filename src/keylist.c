/* keylist.c - Listing keys.
   Copyright (C) 2000 Werner Koch (dd9jn)
   Copyright (C) 2001, 2002, 2003, 2004, 2006, 2007,
                 2008, 2009  g10 Code GmbH

   This file is part of GPGME.

   GPGME is free software; you can redistribute it and/or modify it
   under the terms of the GNU Lesser General Public License as
   published by the Free Software Foundation; either version 2.1 of
   the License, or (at your option) any later version.

   GPGME is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#if HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SYS_TYPES_H
  /* Solaris 8 needs sys/types.h before time.h.  */
# include <sys/types.h>
#endif
#include <time.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>

/* Suppress warning for accessing deprecated member "class".  */
#define _GPGME_IN_GPGME
#include "gpgme.h"
#include "util.h"
#include "context.h"
#include "ops.h"
#include "debug.h"


struct key_queue_item_s
{
  struct key_queue_item_s *next;
  gpgme_key_t key;
};

typedef struct
{
  struct _gpgme_op_keylist_result result;

  gpgme_key_t tmp_key;

  /* This points to the last uid in tmp_key.  */
  gpgme_user_id_t tmp_uid;

  /* This points to the last sig in tmp_uid.  */
  gpgme_key_sig_t tmp_keysig;

  /* Something new is available.  */
  int key_cond;
  struct key_queue_item_s *key_queue;
} *op_data_t;


static void
release_op_data (void *hook)
{
  op_data_t opd = (op_data_t) hook;
  struct key_queue_item_s *key = opd->key_queue;

  if (opd->tmp_key)
    gpgme_key_unref (opd->tmp_key);

  /* opd->tmp_uid and opd->tmp_keysig are actually part of opd->tmp_key,
     so we do not need to release them here.  */

  while (key)
    {
      struct key_queue_item_s *next = key->next;

      gpgme_key_unref (key->key);
      key = next;
    }
}


gpgme_keylist_result_t
gpgme_op_keylist_result (gpgme_ctx_t ctx)
{
  void *hook;
  op_data_t opd;
  gpgme_error_t err;

  TRACE_BEG (DEBUG_CTX, "gpgme_op_keylist_result", ctx);

  err = _gpgme_op_data_lookup (ctx, OPDATA_KEYLIST, &hook, -1, NULL);
  opd = hook;
  if (err || !opd)
    {
      TRACE_SUC0 ("result=(null)");
      return NULL;
    }

  TRACE_LOG1 ("truncated = %i", opd->result.truncated);

  TRACE_SUC1 ("result=%p", &opd->result);
  return &opd->result;
}


static gpgme_error_t
keylist_status_handler (void *priv, gpgme_status_code_t code, char *args)
{
  gpgme_ctx_t ctx = (gpgme_ctx_t) priv;
  gpgme_error_t err;
  void *hook;
  op_data_t opd;

  err = _gpgme_op_data_lookup (ctx, OPDATA_KEYLIST, &hook, -1, NULL);
  opd = hook;
  if (err)
    return err;

  switch (code)
    {
    case GPGME_STATUS_TRUNCATED:
      opd->result.truncated = 1;
      break;

    default:
      break;
    }
  return 0;
}


static void
set_subkey_trust_info (gpgme_subkey_t subkey, const char *src)
{
  while (*src && !isdigit (*src))
    {
      switch (*src)
	{
	case 'e':
	  subkey->expired = 1;
	  break;

	case 'r':
	  subkey->revoked = 1;
	  break;

	case 'd':
          /* Note that gpg 1.3 won't print that anymore but only uses
             the capabilities field. */
	  subkey->disabled = 1;
	  break;

	case 'i':
	  subkey->invalid = 1;
	  break;
        }
      src++;
    }
}


static void
set_mainkey_trust_info (gpgme_key_t key, const char *src)
{
  /* First set the trust info of the main key (the first subkey).  */
  set_subkey_trust_info (key->subkeys, src);

  /* Now set the summarized trust info.  */
  while (*src && !isdigit (*src))
    {
      switch (*src)
	{
	case 'e':
	  key->expired = 1;
	  break;

	case 'r':
	  key->revoked = 1;
	  break;

	case 'd':
          /* Note that gpg 1.3 won't print that anymore but only uses
             the capabilities field.  However, it is still used for
             external key listings.  */
	  key->disabled = 1;
	  break;

	case 'i':
	  key->invalid = 1;
	  break;
        }
      src++;
    }
}


static void
set_userid_flags (gpgme_key_t key, const char *src)
{
  gpgme_user_id_t uid = key->_last_uid;

  assert (uid);
  /* Look at letters and stop at the first digit.  */
  while (*src && !isdigit (*src))
    {
      switch (*src)
	{
	case 'r':
	  uid->revoked = 1;
	  break;

	case 'i':
	  uid->invalid = 1;
	  break;

	case 'n':
	  uid->validity = GPGME_VALIDITY_NEVER;
	  break;

	case 'm':
	  uid->validity = GPGME_VALIDITY_MARGINAL;
	  break;

	case 'f':
	  uid->validity = GPGME_VALIDITY_FULL;
	  break;

	case 'u':
	  uid->validity = GPGME_VALIDITY_ULTIMATE;
	  break;
        }
      src++;
    }
}


static void
set_subkey_capability (gpgme_subkey_t subkey, const char *src)
{
  while (*src)
    {
      switch (*src)
	{
	case 'e':
	  subkey->can_encrypt = 1;
	  break;

	case 's':
	  subkey->can_sign = 1;
	  break;

	case 'c':
	  subkey->can_certify = 1;
	  break;

	case 'a':
	  subkey->can_authenticate = 1;
	  break;

	case 'q':
	  subkey->is_qualified = 1;
	  break;

	case 'd':
	  subkey->disabled = 1;
	  break;
        }
      src++;
    }
}


static void
set_mainkey_capability (gpgme_key_t key, const char *src)
{
  /* First set the capabilities of the main key (the first subkey).  */
  set_subkey_capability (key->subkeys, src);

  while (*src)
    {
      switch (*src)
	{
	case 'd':
        case 'D':
          /* Note, that this flag is also set using the key validity
             field for backward compatibility with gpg 1.2.  We use d
             and D, so that a future gpg version will be able to
             disable certain subkeys. Currently it is expected that
             gpg sets this for the primary key. */
       	  key->disabled = 1;
          break;

	case 'e':
	case 'E':
	  key->can_encrypt = 1;
	  break;

	case 's':
	case 'S':
	  key->can_sign = 1;
	  break;

	case 'c':
	case 'C':
	  key->can_certify = 1;
	  break;

	case 'a':
	case 'A':
	  key->can_authenticate = 1;
	  break;

	case 'q':
	case 'Q':
	  key->is_qualified = 1;
	  break;
        }
      src++;
    }
}


static void
set_ownertrust (gpgme_key_t key, const char *src)
{
  /* Look at letters and stop at the first digit.  */
  while (*src && !isdigit (*src))
    {
      switch (*src)
	{
	case 'n':
	  key->owner_trust = GPGME_VALIDITY_NEVER;
	  break;

	case 'm':
	  key->owner_trust = GPGME_VALIDITY_MARGINAL;
	  break;

	case 'f':
	  key->owner_trust = GPGME_VALIDITY_FULL;
	  break;

	case 'u':
	  key->owner_trust = GPGME_VALIDITY_ULTIMATE;
	  break;

        default:
	  key->owner_trust = GPGME_VALIDITY_UNKNOWN;
	  break;
        }
      src++;
    }
}


/* Parse field 15 of a secret key or subkey.  This fields holds a
   reference to smartcards.  FIELD is the content of the field and we
   are allowed to modify it.  */
static gpg_error_t
parse_sec_field15 (gpgme_subkey_t subkey, char *field)
{
  if (!*field)
    ; /* Empty.  */
  else if (*field == '#')
    {
      /* This is a stub for an offline key.  We reset the SECRET flag
         of the subkey here.  Note that the secret flag of the entire
         key will be true even then.  */
      subkey->secret = 0;
    }
  else if (strchr ("01234567890ABCDEFabcdef", *field))
    {
      /* Fields starts with a hex digit; thus it is a serial number.  */
      subkey->is_cardkey = 1;
      subkey->card_number = strdup (field);
      if (!subkey->card_number)
        return gpg_error_from_syserror ();
    }
  else
    {
      /* RFU.  */
    }

  return 0;
}


/* We have read an entire key into tmp_key and should now finish it.
   It is assumed that this releases tmp_key.  */
static void
finish_key (gpgme_ctx_t ctx, op_data_t opd)
{
  gpgme_key_t key = opd->tmp_key;

  opd->tmp_key = NULL;
  opd->tmp_uid = NULL;
  opd->tmp_keysig = NULL;

  if (key)
    _gpgme_engine_io_event (ctx->engine, GPGME_EVENT_NEXT_KEY, key);
}


/* Note: We are allowed to modify LINE.  */
static gpgme_error_t
keylist_colon_handler (void *priv, char *line)
{
  gpgme_ctx_t ctx = (gpgme_ctx_t) priv;
  enum
    {
      RT_NONE, RT_SIG, RT_UID, RT_SUB, RT_PUB, RT_FPR,
      RT_SSB, RT_SEC, RT_CRT, RT_CRS, RT_REV, RT_SPK
    }
  rectype = RT_NONE;
#define NR_FIELDS 16
  char *field[NR_FIELDS];
  int fields = 0;
  void *hook;
  op_data_t opd;
  gpgme_error_t err;
  gpgme_key_t key;
  gpgme_subkey_t subkey = NULL;
  gpgme_key_sig_t keysig = NULL;

  err = _gpgme_op_data_lookup (ctx, OPDATA_KEYLIST, &hook, -1, NULL);
  opd = hook;
  if (err)
    return err;

  key = opd->tmp_key;

  TRACE2 (DEBUG_CTX, "gpgme:keylist_colon_handler", ctx,
	  "key = %p, line = %s", key, line ? line : "(null)");

  if (!line)
    {
      /* End Of File.  */
      finish_key (ctx, opd);
      return 0;
    }

  while (line && fields < NR_FIELDS)
    {
      field[fields++] = line;
      line = strchr (line, ':');
      if (line)
	*(line++) = '\0';
    }

  if (!strcmp (field[0], "sig"))
    rectype = RT_SIG;
  else if (!strcmp (field[0], "rev"))
    rectype = RT_REV;
  else if (!strcmp (field[0], "pub"))
    rectype = RT_PUB;
  else if (!strcmp (field[0], "sec"))
    rectype = RT_SEC;
  else if (!strcmp (field[0], "crt"))
    rectype = RT_CRT;
  else if (!strcmp (field[0], "crs"))
    rectype = RT_CRS;
  else if (!strcmp (field[0], "fpr") && key)
    rectype = RT_FPR;
  else if (!strcmp (field[0], "uid") && key)
    rectype = RT_UID;
  else if (!strcmp (field[0], "sub") && key)
    rectype = RT_SUB;
  else if (!strcmp (field[0], "ssb") && key)
    rectype = RT_SSB;
  else if (!strcmp (field[0], "spk") && key)
    rectype = RT_SPK;
  else
    rectype = RT_NONE;

  /* Only look at signatures immediately following a user ID.  For
     this, clear the user ID pointer when encountering anything but a
     signature.  */
  if (rectype != RT_SIG && rectype != RT_REV)
    opd->tmp_uid = NULL;

  /* Only look at subpackets immediately following a signature.  For
     this, clear the signature pointer when encountering anything but
     a subpacket.  */
  if (rectype != RT_SPK)
    opd->tmp_keysig = NULL;

  switch (rectype)
    {
    case RT_PUB:
    case RT_SEC:
    case RT_CRT:
    case RT_CRS:
      /* Start a new keyblock.  */
      err = _gpgme_key_new (&key);
      if (err)
	return err;
      key->keylist_mode = ctx->keylist_mode;
      err = _gpgme_key_add_subkey (key, &subkey);
      if (err)
	{
	  gpgme_key_unref (key);
	  return err;
	}

      if (rectype == RT_SEC || rectype == RT_CRS)
	key->secret = subkey->secret = 1;
      if (rectype == RT_CRT || rectype == RT_CRS)
	key->protocol = GPGME_PROTOCOL_CMS;
      finish_key (ctx, opd);
      opd->tmp_key = key;

      /* Field 2 has the trust info.  */
      if (fields >= 2)
	set_mainkey_trust_info (key, field[1]);

      /* Field 3 has the key length.  */
      if (fields >= 3)
	{
	  int i = atoi (field[2]);
	  /* Ignore invalid values.  */
	  if (i > 1)
	    subkey->length = i;
	}

      /* Field 4 has the public key algorithm.  */
      if (fields >= 4)
	{
	  int i = atoi (field[3]);
	  if (i >= 1 && i < 128)
	    subkey->pubkey_algo = i;
	}

      /* Field 5 has the long keyid.  Allow short key IDs for the
	 output of an external keyserver listing.  */
      if (fields >= 5 && strlen (field[4]) <= DIM(subkey->_keyid) - 1)
	strcpy (subkey->_keyid, field[4]);

      /* Field 6 has the timestamp (seconds).  */
      if (fields >= 6)
	subkey->timestamp = _gpgme_parse_timestamp (field[5], NULL);

      /* Field 7 has the expiration time (seconds).  */
      if (fields >= 7)
	subkey->expires = _gpgme_parse_timestamp (field[6], NULL);

      /* Field 8 has the X.509 serial number.  */
      if (fields >= 8 && (rectype == RT_CRT || rectype == RT_CRS))
	{
	  key->issuer_serial = strdup (field[7]);
	  if (!key->issuer_serial)
	    return gpg_error_from_errno (errno);
	}

      /* Field 9 has the ownertrust.  */
      if (fields >= 9)
	set_ownertrust (key, field[8]);

      /* Field 10 is not used for gpg due to --fixed-list-mode option
	 but GPGSM stores the issuer name.  */
      if (fields >= 10 && (rectype == RT_CRT || rectype == RT_CRS))
	if (_gpgme_decode_c_string (field[9], &key->issuer_name, 0))
	  return gpg_error (GPG_ERR_ENOMEM);	/* FIXME */

      /* Field 11 has the signature class.  */

      /* Field 12 has the capabilities.  */
      if (fields >= 12)
	set_mainkey_capability (key, field[11]);

      /* Field 15 carries special flags of a secret key.  */
      if (fields >= 15 && key->secret)
        {
          err = parse_sec_field15 (subkey, field[14]);
          if (err)
            return err;
        }
      break;

    case RT_SUB:
    case RT_SSB:
      /* Start a new subkey.  */
      err = _gpgme_key_add_subkey (key, &subkey);
      if (err)
	return err;

      if (rectype == RT_SSB)
	subkey->secret = 1;

      /* Field 2 has the trust info.  */
      if (fields >= 2)
	set_subkey_trust_info (subkey, field[1]);

      /* Field 3 has the key length.  */
      if (fields >= 3)
	{
	  int i = atoi (field[2]);
	  /* Ignore invalid values.  */
	  if (i > 1)
	    subkey->length = i;
	}

      /* Field 4 has the public key algorithm.  */
      if (fields >= 4)
	{
	  int i = atoi (field[3]);
	  if (i >= 1 && i < 128)
	    subkey->pubkey_algo = i;
	}

      /* Field 5 has the long keyid.  */
      if (fields >= 5 && strlen (field[4]) == DIM(subkey->_keyid) - 1)
	strcpy (subkey->_keyid, field[4]);

      /* Field 6 has the timestamp (seconds).  */
      if (fields >= 6)
	subkey->timestamp = _gpgme_parse_timestamp (field[5], NULL);

      /* Field 7 has the expiration time (seconds).  */
      if (fields >= 7)
	subkey->expires = _gpgme_parse_timestamp (field[6], NULL);

      /* Field 8 is reserved (LID).  */
      /* Field 9 has the ownertrust.  */
      /* Field 10, the user ID, is n/a for a subkey.  */

      /* Field 11 has the signature class.  */

      /* Field 12 has the capabilities.  */
      if (fields >= 12)
	set_subkey_capability (subkey, field[11]);

      /* Field 15 carries special flags of a secret key. */
      if (fields >= 15 && key->secret)
        {
          err = parse_sec_field15 (subkey, field[14]);
          if (err)
            return err;
        }
      break;

    case RT_UID:
      /* Field 2 has the trust info, and field 10 has the user ID.  */
      if (fields >= 10)
	{
	  if (_gpgme_key_append_name (key, field[9], 1))
	    return gpg_error_from_errno (GPG_ERR_ENOMEM);	/* FIXME */
	  else
	    {
	      if (field[1])
		set_userid_flags (key, field[1]);
	      opd->tmp_uid = key->_last_uid;
	    }
	}
      break;

    case RT_FPR:
      /* Field 10 has the fingerprint (take only the first one).  */
      if (fields >= 10 && field[9] && *field[9])
	{
          /* Need to apply it to the last subkey because all subkeys
             do have fingerprints. */
          subkey = key->_last_subkey;
          if (!subkey->fpr)
            {
              subkey->fpr = strdup (field[9]);
              if (!subkey->fpr)
                return gpg_error_from_errno (errno);
            }
	}

      /* Field 13 has the gpgsm chain ID (take only the first one).  */
      if (fields >= 13 && !key->chain_id && *field[12])
	{
	  key->chain_id = strdup (field[12]);
	  if (!key->chain_id)
	    return gpg_error_from_errno (errno);
	}
      break;

    case RT_SIG:
    case RT_REV:
      if (!opd->tmp_uid)
	return 0;

      /* Start a new (revoked) signature.  */
      assert (opd->tmp_uid == key->_last_uid);
      keysig = _gpgme_key_add_sig (key, (fields >= 10) ? field[9] : NULL);
      if (!keysig)
	return gpg_error (GPG_ERR_ENOMEM);	/* FIXME */

      /* Field 2 has the calculated trust ('!', '-', '?', '%').  */
      if (fields >= 2)
	switch (field[1][0])
	  {
	  case '!':
	    keysig->status = gpg_error (GPG_ERR_NO_ERROR);
	    break;

	  case '-':
	    keysig->status = gpg_error (GPG_ERR_BAD_SIGNATURE);
	    break;

	  case '?':
	    keysig->status = gpg_error (GPG_ERR_NO_PUBKEY);
	    break;

	  case '%':
	    keysig->status = gpg_error (GPG_ERR_GENERAL);
	    break;

	  default:
	    keysig->status = gpg_error (GPG_ERR_NO_ERROR);
	    break;
	  }

      /* Field 4 has the public key algorithm.  */
      if (fields >= 4)
	{
	  int i = atoi (field[3]);
	  if (i >= 1 && i < 128)
	    keysig->pubkey_algo = i;
	}

      /* Field 5 has the long keyid.  */
      if (fields >= 5 && strlen (field[4]) == DIM(keysig->_keyid) - 1)
	strcpy (keysig->_keyid, field[4]);

      /* Field 6 has the timestamp (seconds).  */
      if (fields >= 6)
	keysig->timestamp = _gpgme_parse_timestamp (field[5], NULL);

      /* Field 7 has the expiration time (seconds).  */
      if (fields >= 7)
	keysig->expires = _gpgme_parse_timestamp (field[6], NULL);

      /* Field 11 has the signature class (eg, 0x30 means revoked).  */
      if (fields >= 11)
	if (field[10][0] && field[10][1])
	  {
	    int sig_class = _gpgme_hextobyte (field[10]);
	    if (sig_class >= 0)
	      {
		keysig->sig_class = sig_class;
		keysig->class = keysig->sig_class;
		if (sig_class == 0x30)
		  keysig->revoked = 1;
	      }
	    if (field[10][2] == 'x')
	      keysig->exportable = 1;
	  }

      opd->tmp_keysig = keysig;
      break;

    case RT_SPK:
      if (!opd->tmp_keysig)
	return 0;
      assert (opd->tmp_keysig == key->_last_uid->_last_keysig);

      if (fields >= 4)
	{
	  /* Field 2 has the subpacket type.  */
	  int type = atoi (field[1]);

	  /* Field 3 has the flags.  */
	  int flags = atoi (field[2]);

	  /* Field 4 has the length.  */
	  int len = atoi (field[3]);

	  /* Field 5 has the data.  */
	  char *data = field[4];

	  /* Type 20: Notation data.  */
	  /* Type 26: Policy URL.  */
	  if (type == 20 || type == 26)
	    {
	      gpgme_sig_notation_t notation;

	      keysig = opd->tmp_keysig;

	      /* At this time, any error is serious.  */
	      err = _gpgme_parse_notation (&notation, type, flags, len, data);
	      if (err)
		return err;

	      /* Add a new notation.  FIXME: Could be factored out.  */
	      if (!keysig->notations)
		keysig->notations = notation;
	      if (keysig->_last_notation)
		keysig->_last_notation->next = notation;
	      keysig->_last_notation = notation;
	    }
	}

    case RT_NONE:
      /* Unknown record.  */
      break;
    }
  return 0;
}


void
_gpgme_op_keylist_event_cb (void *data, gpgme_event_io_t type, void *type_data)
{
  gpgme_error_t err;
  gpgme_ctx_t ctx = (gpgme_ctx_t) data;
  gpgme_key_t key = (gpgme_key_t) type_data;
  void *hook;
  op_data_t opd;
  struct key_queue_item_s *q, *q2;

  assert (type == GPGME_EVENT_NEXT_KEY);

  err = _gpgme_op_data_lookup (ctx, OPDATA_KEYLIST, &hook, -1, NULL);
  opd = hook;
  if (err)
    return;

  q = malloc (sizeof *q);
  if (!q)
    {
      gpgme_key_unref (key);
      /* FIXME       return GPGME_Out_Of_Core; */
      return;
    }
  q->key = key;
  q->next = NULL;
  /* FIXME: Use a tail pointer?  */
  if (!(q2 = opd->key_queue))
    opd->key_queue = q;
  else
    {
      for (; q2->next; q2 = q2->next)
	;
      q2->next = q;
    }
  opd->key_cond = 1;
}


/* Start a keylist operation within CTX, searching for keys which
   match PATTERN.  If SECRET_ONLY is true, only secret keys are
   returned.  */
gpgme_error_t
gpgme_op_keylist_start (gpgme_ctx_t ctx, const char *pattern, int secret_only)
{
  gpgme_error_t err;
  void *hook;
  op_data_t opd;

  TRACE_BEG2 (DEBUG_CTX, "gpgme_op_keylist_start", ctx,
	      "pattern=%s, secret_only=%i", pattern, secret_only);

  if (!ctx)
    return TRACE_ERR (gpg_error (GPG_ERR_INV_VALUE));

  err = _gpgme_op_reset (ctx, 2);
  if (err)
    return TRACE_ERR (err);

  err = _gpgme_op_data_lookup (ctx, OPDATA_KEYLIST, &hook,
			       sizeof (*opd), release_op_data);
  opd = hook;
  if (err)
    return TRACE_ERR (err);

  _gpgme_engine_set_status_handler (ctx->engine, keylist_status_handler, ctx);

  err = _gpgme_engine_set_colon_line_handler (ctx->engine,
					      keylist_colon_handler, ctx);
  if (err)
    return TRACE_ERR (err);

  err = _gpgme_engine_op_keylist (ctx->engine, pattern, secret_only,
				  ctx->keylist_mode);
  return TRACE_ERR (err);
}


/* Start a keylist operation within CTX, searching for keys which
   match PATTERN.  If SECRET_ONLY is true, only secret keys are
   returned.  */
gpgme_error_t
gpgme_op_keylist_ext_start (gpgme_ctx_t ctx, const char *pattern[],
			    int secret_only, int reserved)
{
  gpgme_error_t err;
  void *hook;
  op_data_t opd;

  TRACE_BEG2 (DEBUG_CTX, "gpgme_op_keylist_ext_start", ctx,
	      "secret_only=%i, reserved=0x%x", secret_only, reserved);

  if (!ctx)
    return TRACE_ERR (gpg_error (GPG_ERR_INV_VALUE));

  err = _gpgme_op_reset (ctx, 2);
  if (err)
    return TRACE_ERR (err);

  err = _gpgme_op_data_lookup (ctx, OPDATA_KEYLIST, &hook,
			       sizeof (*opd), release_op_data);
  opd = hook;
  if (err)
    return TRACE_ERR (err);

  _gpgme_engine_set_status_handler (ctx->engine, keylist_status_handler, ctx);
  err = _gpgme_engine_set_colon_line_handler (ctx->engine,
					      keylist_colon_handler, ctx);
  if (err)
    return TRACE_ERR (err);

  err = _gpgme_engine_op_keylist_ext (ctx->engine, pattern, secret_only,
				      reserved, ctx->keylist_mode);
  return TRACE_ERR (err);
}


/* Return the next key from the keylist in R_KEY.  */
gpgme_error_t
gpgme_op_keylist_next (gpgme_ctx_t ctx, gpgme_key_t *r_key)
{
  gpgme_error_t err;
  struct key_queue_item_s *queue_item;
  void *hook;
  op_data_t opd;

  TRACE_BEG (DEBUG_CTX, "gpgme_op_keylist_next", ctx);

  if (!ctx || !r_key)
    return TRACE_ERR (gpg_error (GPG_ERR_INV_VALUE));
  *r_key = NULL;
  if (!ctx)
    return TRACE_ERR (gpg_error (GPG_ERR_INV_VALUE));

  err = _gpgme_op_data_lookup (ctx, OPDATA_KEYLIST, &hook, -1, NULL);
  opd = hook;
  if (err)
    return TRACE_ERR (err);
  if (opd == NULL)
    return TRACE_ERR (gpg_error (GPG_ERR_INV_VALUE));

  if (!opd->key_queue)
    {
      err = _gpgme_wait_on_condition (ctx, &opd->key_cond, NULL);
      if (err)
	return TRACE_ERR (err);

      if (!opd->key_cond)
	return TRACE_ERR (gpg_error (GPG_ERR_EOF));

      opd->key_cond = 0;
      assert (opd->key_queue);
    }
  queue_item = opd->key_queue;
  opd->key_queue = queue_item->next;
  if (!opd->key_queue)
    opd->key_cond = 0;

  *r_key = queue_item->key;
  free (queue_item);

  return TRACE_SUC2 ("key=%p (%s)", *r_key,
		     ((*r_key)->subkeys && (*r_key)->subkeys->fpr) ?
		     (*r_key)->subkeys->fpr : "invalid");
}


/* Terminate a pending keylist operation within CTX.  */
gpgme_error_t
gpgme_op_keylist_end (gpgme_ctx_t ctx)
{
  TRACE (DEBUG_CTX, "gpgme_op_keylist_end", ctx);

  if (!ctx)
    return gpg_error (GPG_ERR_INV_VALUE);

  return 0;
}


/* Get the key with the fingerprint FPR from the crypto backend.  If
   SECRET is true, get the secret key.  */
gpgme_error_t
gpgme_get_key (gpgme_ctx_t ctx, const char *fpr, gpgme_key_t *r_key,
	       int secret)
{
  gpgme_ctx_t listctx;
  gpgme_error_t err;
  gpgme_key_t key;

  TRACE_BEG2 (DEBUG_CTX, "gpgme_get_key", ctx,
	      "fpr=%s, secret=%i", fpr, secret);

  if (!ctx || !r_key || !fpr)
    return TRACE_ERR (gpg_error (GPG_ERR_INV_VALUE));

  if (strlen (fpr) < 8)	/* We have at least a key ID.  */
    return TRACE_ERR (gpg_error (GPG_ERR_INV_VALUE));

  /* FIXME: We use our own context because we have to avoid the user's
     I/O callback handlers.  */
  err = gpgme_new (&listctx);
  if (err)
    return TRACE_ERR (err);
  {
    gpgme_protocol_t proto;
    gpgme_engine_info_t info;

    /* Clone the relevant state.  */
    proto = gpgme_get_protocol (ctx);
    gpgme_set_protocol (listctx, proto);
    gpgme_set_keylist_mode (listctx, gpgme_get_keylist_mode (ctx));
    info = gpgme_ctx_get_engine_info (ctx);
    while (info && info->protocol != proto)
      info = info->next;
    if (info)
      gpgme_ctx_set_engine_info (listctx, proto,
				 info->file_name, info->home_dir);
  }

  err = gpgme_op_keylist_start (listctx, fpr, secret);
  if (!err)
    err = gpgme_op_keylist_next (listctx, r_key);
  if (!err)
    {
    try_next_key:
      err = gpgme_op_keylist_next (listctx, &key);
      if (gpgme_err_code (err) == GPG_ERR_EOF)
	err = 0;
      else
	{
          if (!err
              && *r_key && (*r_key)->subkeys && (*r_key)->subkeys->fpr
              && key && key->subkeys && key->subkeys->fpr
              && !strcmp ((*r_key)->subkeys->fpr, key->subkeys->fpr))
            {
              /* The fingerprint is identical.  We assume that this is
                 the same key and don't mark it as an ambiguous.  This
                 problem may occur with corrupted keyrings and has
                 been noticed often with gpgsm.  In fact gpgsm uses a
                 similar hack to sort out such duplicates but it can't
                 do that while listing keys.  */
              gpgme_key_unref (key);
              goto try_next_key;
            }
	  if (!err)
	    {
	      gpgme_key_unref (key);
	      err = gpg_error (GPG_ERR_AMBIGUOUS_NAME);
	    }
	  gpgme_key_unref (*r_key);
	}
    }
  gpgme_release (listctx);
  if (! err)
    {
      TRACE_LOG2 ("key=%p (%s)", *r_key,
		  ((*r_key)->subkeys && (*r_key)->subkeys->fpr) ?
		  (*r_key)->subkeys->fpr : "invalid");
    }
  return TRACE_ERR (err);
}
