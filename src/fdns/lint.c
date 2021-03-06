/*
 * Copyright (C) 2019-2020 FDNS Authors
 *
 * This file is part of fdns project
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "lint.h"

//***********************************************
// error
//***********************************************
static int dnserror;
static const char *err2str[DNSERR_MAX] = {
	"no error",
	"invalid header",
	"invalid domain",
	"invalid class",
	"nxdomain",
	"multiple questions",
	"invalid packet length"
};

int lint_error(void) {
	return dnserror;
}

const char *lint_err2str(void) {
	assert(dnserror < DNSERR_MAX);
	return err2str[dnserror];
}

//***********************************************
// lint
//***********************************************
static DnsHeader hdr;
static DnsQuestion question;

// check chars in domain name: a-z, A-Z, and 0-9
// return 0 if ok, 1 if bad
//TODO: add support or IDNA and/or Punycode (rfc3492)
static inline int check_char(const uint8_t c)  {
	if (c >= 'a' && c <= 'z')
		return 0;
	else if (c >= 'A' && c <= 'Z')
		return 0;
	else if ( c >= '0' && c <= '9')
		return 0;
	else if (c == '-')
		return 0;

	return 1;
}

// parse a domain name
// error if cross-references
// size - number of packet bytes consumed
// return -1 if error, 0 if ok
static int domain_size_no_crossreference(const uint8_t *data, char *domain_name, unsigned *size) {
	assert(data);
	assert(domain_name);
	assert(size);
	unsigned i = 0;
	unsigned chunk_size = *data;

	// skip each set of chars until (0) at the end
	while(chunk_size != 0) {
		if (chunk_size > 63)
			goto errexit;
		i += chunk_size + 1;
		if (i > 255)
			goto errexit;

		// check chars in domain name
		const uint8_t *ptr = data + i - chunk_size - 1 + 1;
		unsigned j;
		for (j = 0; j < chunk_size; j++, ptr++) {
//printf("%02x - %c\n", *ptr, (char) *ptr);
			if (check_char(*ptr))
				goto errexit;
		}

		memcpy(domain_name + i - chunk_size - 1, data + i - chunk_size - 1 + 1, chunk_size);
		domain_name[i - 1] = '.';
		chunk_size = data[i];
	}

	// domain name including the ending \0
	domain_name[i - 1] = '\0';
	*size = i + 1;
	return 0;
errexit:
	dnserror = DNSERR_INVALID_DOMAIN;
	return -1;
}


static void clean_domain(uint8_t *ptr) {
	assert(ptr);
	uint8_t *end = ptr + strlen(ptr);

	while (*ptr != 0 && ptr < end) {
		if ((*ptr & 0xc0) == 0) {
			uint8_t jump = *ptr + 1;
			*ptr = '.';
			ptr += jump;
		}
		else {
			*ptr = '.';
			ptr++;
		}
	}
}

// return -1 if error, 0 if ok
static int skip_name(uint8_t **pkt, uint8_t *last) {
	dnserror = DNSERR_OK;

	if (*pkt > last) {
		dnserror = DNSERR_INVALID_PKT_LEN;
		return -1;
	}

	while (**pkt != 0 && *pkt < (last - 1)) {
		if ((**pkt & 0xc0) == 0)
			*pkt +=  **pkt + 1;
		else {
			(*pkt)++;
			break;
		}
	}
	(*pkt)++;
	return 0;
}


//***********************************************
// public interface
//***********************************************
// pkt positioned at start of packet
DnsHeader *lint_header(uint8_t **pkt, uint8_t *last) {
	assert(pkt);
	assert(*pkt);
	assert(last);
	dnserror = DNSERR_OK;

	if (*pkt + sizeof(DnsHeader) > last) {
		dnserror = DNSERR_INVALID_HEADER;
		return NULL;
	}

	memcpy(&hdr, *pkt, sizeof(hdr));
	hdr.id = ntohs(hdr.id);
	hdr.flags = ntohs(hdr.flags);
	hdr.questions = ntohs(hdr.questions);
	hdr.answer = ntohs(hdr.answer);
	hdr.authority = ntohs(hdr.authority);
	hdr.additional = ntohs(hdr.additional);
	*pkt += sizeof(DnsHeader);
	return &hdr;
}

// pkt positioned at the the start of question
DnsQuestion *lint_question(uint8_t **pkt, uint8_t *last) {
	assert(pkt);
	assert(*pkt);
	assert(last);
	dnserror = DNSERR_OK;

	if (*pkt + 1 + 2 + 2 > last) { // empty domain + type + class
		dnserror = DNSERR_INVALID_DOMAIN;
		return NULL;
	}

	// clanup
	question.domain[0] = '\0';
	question.type = 0;
	unsigned size = 0;

	// first byte smaller than 63
	if (**pkt > 63) {
		dnserror = DNSERR_INVALID_DOMAIN;
		return NULL;
	}

	if (domain_size_no_crossreference(*pkt, question.domain, &size)) {
		dnserror = DNSERR_INVALID_DOMAIN;
		return NULL;
	}

	// check length
	if (*pkt + size + 4 - 1 > last ) {
		dnserror = DNSERR_INVALID_DOMAIN;
		return NULL;
	}

	// set type
	*pkt += size;
	memcpy(&question.type, *pkt, 2);
	question.type = ntohs(question.type);
	*pkt += 2;

	// check class
	uint16_t cls;
	memcpy(&cls, *pkt,  2);
	cls = ntohs(cls);
	if (cls != 1) {
		dnserror = DNSERR_INVALID_CLASS;
		return NULL;
	}
	*pkt += 2;

	question.len = size + 4;
	question.dlen = question.len - 6; // we are assuming a domain name without crossreferences
	return &question;
}

// return -1 if error, 0 if fine
// pkt positioned at start of packet
int lint_rx(uint8_t *pkt, unsigned len) {
	assert(pkt);
	assert(len);
	uint8_t *last = pkt + len - 1;
	dnserror = DNSERR_OK;

	// check header
	DnsHeader *h = lint_header(&pkt, last);
	if (!h)
		return -1;

	// check errors such as NXDOMAIN
	if ((h->flags & 0x000f) != 0) {
		dnserror = DNSERR_NXDOMAIN;
		return -1;
	}

	// one question
	if (h->questions != 1) {
		dnserror = DNSERR_MULTIPLE_QUESTIONS;
		return -1;
	}

	if (skip_name(&pkt, last))
		return -1;
	pkt += 4;
	if (pkt > last) {
		dnserror = DNSERR_INVALID_PKT_LEN;
		return -1;
	}

	// extract CNAMEs from the answer section
	int i;
	for (i = 0; i < h->answer; i++) {
		if (skip_name(&pkt, last))
			return -1;

		// extract record
		if (pkt + sizeof(DnsRR) > last) {
			dnserror = DNSERR_INVALID_PKT_LEN;
			return -1;
		}
		DnsRR rr;
		memcpy(&rr, pkt, sizeof(DnsRR));
		rr.type = ntohs(rr.type);
		rr.cls = ntohs(rr.cls);
		rr.ttl = ntohl(rr.ttl);
		rr.rlen = ntohs(rr.rlen);
		pkt += sizeof(DnsRR);

//printf("type %u, class %u, ttl %u, rlen %u\n",
//rr.type, rr.cls, rr.ttl, rr.rlen);
		// CNAME processing
		if (rr.type == 5 && rr.rlen <= 256) { // CNAME
			uint8_t cname[256 + 1];
			memcpy(cname, pkt, rr.rlen);
			cname[rr.rlen] = '\0';
			// clean cname
			clean_domain(cname);
			printf("CNAME: %s\n", cname);
			fflush(0);
		}
		pkt += rr.rlen;
	}

	return 0;
}
