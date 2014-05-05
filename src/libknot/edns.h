/*!
 * \file edns.h
 *
 * \author Lubos Slovak <lubos.slovak@nic.cz>
 *
 * \brief Functions for manipulating and parsing EDNS OPT pseudo-RR.
 *
 * \addtogroup libknot
 * @{
 */
/*  Copyright (C) 2011 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _KNOT_EDNS_H_
#define _KNOT_EDNS_H_

#include <stdint.h>

#include "libknot/util/utils.h"
#include "libknot/rrset.h"

struct knot_packet;

/*----------------------------------------------------------------------------*/
/*! \brief Structure representing one OPT RR Option. */
struct knot_opt_option {
	uint16_t code;
	uint16_t length;
	uint8_t *data;
};

/*! \brief Structure representing one OPT RR Option. */
typedef struct knot_opt_option knot_opt_option_t;

/*!
 * \brief Structure for holding EDNS parameters.
 *
 * \todo NSID
 */
struct knot_opt_rr {
	uint16_t payload;    /*!< UDP payload. */
	uint8_t ext_rcode;   /*!< Extended RCODE. */

	/*!
	 * \brief Supported version of EDNS.
	 *
	 * Set to EDNS_NOT_SUPPORTED if not supported.
	 */
	uint8_t version;

	uint16_t flags;                /*!< EDNS flags. */
	knot_opt_option_t *options;  /*!< EDNS options. */
	short option_count;         /*!< Count of EDNS options in this OPT RR.*/
	short options_max;          /*!< Maximum count of options. */
	short size;             /*!< Total size of the OPT RR in wire format. */
};

/*! \brief Structure for holding EDNS parameters. */
typedef struct knot_opt_rr knot_opt_rr_t;

struct knot_edns_params {
	uint16_t payload;    /*!< Max UDP payload. */
	uint8_t version;     /*!< Supported version of EDNS. */
	uint16_t nsid_len;   /*!< Length of NSID string. */
	uint8_t *nsid;       /*!< NSID string. */
};

typedef struct knot_edns_params knot_edns_params_t;

/*----------------------------------------------------------------------------*/
/*! \brief Constants for EDNS. */
enum knot_edns_const {
	EDNS_MIN_UDP_PAYLOAD = 512,  /*!< Minimal UDP payload with EDNS enabled. */
	EDNS_MIN_DNSSEC_PAYLOAD = 1220, /*!< Minimal payload when using DNSSEC (RFC4035/sec.3) */
	EDNS_MAX_UDP_PAYLOAD = 4096, /*!< Maximal UDP payload with EDNS enabled. */
	EDNS_VERSION         = 0,    /*!< Supported EDNS version. */
	EDNS_NOT_SUPPORTED   = 255,  /*!< EDNS not supported. */
	EDNS_OPTION_NSID     = 3,    /*!< NSID option code. */
	EDNS_MIN_SIZE        = 11    /*!< Minimum size of EDNS OPT RR in wire format. */
};

/*! \brief Enumeration of named options. */
enum knot_edns_option {
	KNOT_PKT_EDNS_PAYLOAD = 0,
	KNOT_PKT_EDNS_VERSION = 1,
	KNOT_PKT_EDNS_RCODE   = 2,
	KNOT_PKT_EDNS_FLAG_DO = 3,
	KNOT_PKT_EDNS_NSID    = 4
};

/*! \brief EDNS flags.
 *
 * \note Use only with unsigned 2-byte variables.
 * \warning Flags are represented in little endian, i.e. in reverse order than
 *          on the wire (DO bit is not 1st but 9th).
 */
enum knot_edns_flags {
	KNOT_EDNS_FLAG_DO = (uint16_t)1 << 8
};

/*----------------------------------------------------------------------------*/
/*! \todo [OPT] REWRITE */
/*!
 * \brief Creates new empty OPT RR structure for holding EDNS parameters.
 *
 * \return New empty knot_opt_rr_t structure, or NULL if not successful.
 */
knot_rrset_t *knot_edns_new(uint16_t max_pld, uint8_t ext_rcode, uint8_t ver,
                            uint16_t flags, mm_ctx_t *mm);

/*!
 * \brief Returns the UDP payload stored in the OPT RR.
 *
 * \warning This function does not check the parameter, so ensure to check it
 *          before calling the function. It must not be NULL.
 * \note There is an assert() for debug checking of the parameter.
 *
 * \param opt_rr OPT RR to get the payload from.
 *
 * \return UDP payload in bytes.
 */
uint16_t knot_edns_get_payload(const knot_rrset_t *opt_rr);

/*!
 * \brief Sets the UDP payload field in the OPT RR.
 *
 * \warning This function does not check the parameter, so ensure to check it
 *          before calling the function. It must not be NULL.
 * \note There is an assert() for debug checking of the parameter.
 *
 * \param opt_rr OPT RR to set the payload to.
 * \param payload UDP payload in bytes.
 */
void knot_edns_set_payload(knot_rrset_t *opt_rr, uint16_t payload);

/*!
 * \brief Returns the Extended RCODE stored in the OPT RR.
 *
 * \warning This function does not check the parameter, so ensure to check it
 *          before calling the function. It must not be NULL.
 * \note There is an assert() for debug checking of the parameter.
 *
 * \param opt_rr OPT RR to get the Extended RCODE from.
 *
 * \return Extended RCODE.
 */
uint8_t knot_edns_get_ext_rcode(const knot_rrset_t *opt_rr);

/*!
 * \brief Sets the Extended RCODE field in the OPT RR.
 *
 * \warning This function does not check the parameter, so ensure to check it
 *          before calling the function. It must not be NULL.
 * \note There is an assert() for debug checking of the parameter.
 *
 * \param opt_rr OPT RR to set the Extended RCODE to.
 * \param ext_rcode Extended RCODE to set.
 */
void knot_edns_set_ext_rcode(knot_rrset_t *opt_rr, uint8_t ext_rcode);

/*!
 * \brief Returns the EDNS version stored in the OPT RR.
 *
 * \warning This function does not check the parameter, so ensure to check it
 *          before calling the function. It must not be NULL.
 * \note There is an assert() for debug checking of the parameter.
 *
 * \param opt_rr OPT RR to get the EDNS version from.
 *
 * \return EDNS version.
 */
uint8_t knot_edns_get_version(const knot_rrset_t *opt_rr);

/*!
 * \brief Sets the EDNS version field in the OPT RR.
 *
 * \warning This function does not check the parameter, so ensure to check it
 *          before calling the function. It must not be NULL.
 * \note There is an assert() for debug checking of the parameter.
 *
 * \param opt_rr OPT RR to set the EDNS version to.
 * \param version EDNS version to set.
 */
void knot_edns_set_version(knot_rrset_t *opt_rr, uint8_t version);

/*!
 * \brief Returns the state of the DO bit in the OPT RR flags.
 *
 * \warning This function does not check the parameter, so ensure to check it
 *          before calling the function. It must not be NULL.
 *
 * \param opt_rr OPT RR to get the DO bit from.
 *
 * \return <> 0 if the DO bit is set.
 * \return 0 if the DO bit is not set.
 */
bool knot_edns_do(const knot_rrset_t *opt_rr);

/*!
 * \brief Sets the DO bit in the OPT RR.
 *
 * \warning This function does not check the parameter, so ensure to check it
 *          before calling the function. It must not be NULL.
 *
 * \param opt_rr OPT RR to set the DO bit in.
 */
void knot_edns_set_do(knot_rrset_t *opt_rr);

/*
 * <<<<< TODO: REFACTOR
 */

/*!
 * \brief Adds EDNS Option to the OPT RR.
 *
 * \param opt_rr OPT RR structure to add the Option to.
 * \param code Option code.
 * \param length Option data length in bytes.
 * \param data Option data.
 *
 * \retval KNOT_EOK
 * \retval KNOT_ENOMEM
 */
int knot_edns_add_option(knot_rrset_t *opt_rr, uint16_t code,
                         uint16_t length, const uint8_t *data, mm_ctx_t *mm);

/*!
 * \brief Checks if the OPT RR contains Option with the specified code.
 *
 * \param opt_rr OPT RR structure to check for the Option in.
 * \param code Option code to check for.
 *
 * \retval <> 0 if the OPT RR contains Option with Option code \a code.
 * \retval 0 otherwise.
 */
bool knot_edns_has_option(const knot_rrset_t *opt_rr, uint16_t code);

/*!
 * \brief Returns size of the OPT RR in wire format.
 *
 * \param opt_rr OPT RR to get the size of.
 *
 * \return Size of the OPT RR in bytes.
 */
size_t knot_edns_size(knot_rrset_t *opt_rr);

/*
 * >>>>> TODO: REFACTOR
 */

/*----------------------------------------------------------------------------*/
/* NEW API                                                                    */
/*----------------------------------------------------------------------------*/

knot_edns_params_t *knot_edns_new_params();

void knot_edns_free_params(knot_edns_params_t **opt);

knot_rrset_t *knot_edns_new_from_params(const knot_edns_params_t *params,
                                        bool add_nsid);

#endif /* _KNOT_EDNS_H_ */

/*! @} */
