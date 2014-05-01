/* vxi11_user.cc
 * Copyright (C) 2006 Steve D. Sharples
 *
 * User library for opening, closing, sending to and receiving from
 * a device enabled with the VXI11 RPC ethernet protocol. Uses the files
 * generated by rpcgen vxi11.x.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * The author's email address is steve.sharples@nottingham.ac.uk
 */

#include "vxi11_user.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef WIN32
#  include <visa.h>
#else
#  include <rpc/rpc.h>
#  include "vxi11.h"
#endif

#define	VXI11_CLIENT		CLIENT
#define	VXI11_LINK		Create_LinkResp

struct _VXI11_CLINK {
#ifdef WIN32
	ViSession rm;
	ViSession session;
#else
	VXI11_CLIENT *client;
	VXI11_LINK *link;
#endif
};

/***************************************************************************** 
 * GENERAL NOTES
 *****************************************************************************
 *
 * There are four functions at the heart of this library:
 *
 * VXI11_CLINK *vxi11_open_device(char *address, char *device)
 * int	vxi11_close_device(char *address, VXI11_CLINK *clink)
 * int	vxi11_send(VXI11_CLINK *clink, char *cmd, size_t len)
 *    --- or --- (if sending just text)
 * int	vxi11_send(VXI11_CLINK *clink, char *cmd)
 * long	vxi11_receive(VXI11_CLINK *clink, char *buffer, size_t len, unsigned long timeout)
 *
 * There are then useful (to me, anyway) more specific functions built on top
 * of these:
 *
 * int	vxi11_send_data_block(VXI11_CLINK *clink, char *cmd, char *buffer, size_t len)
 * long	vxi11_receive_data_block(VXI11_CLINK *clink, char *buffer, size_t len, unsigned long timeout)
 * long	vxi11_send_and_receive(VXI11_CLINK *clink, char *cmd, char *buf, size_t len, unsigned long timeout)
 * long	vxi11_obtain_long_value(VXI11_CLINK *clink, char *cmd, unsigned long timeout)
 * double vxi11_obtain_double_value(VXI11_CLINK *clink, char *cmd, unsigned long timeout)
 */

/* Global variables. Keep track of multiple links per client. We need this
 * because:
 * - we'd like the library to be able to cope with multiple links to a given
 *   client AND multiple links to multiple clients
 * - we'd like to just refer to a client/link ("clink") as a single
 *   entity from user land, we don't want to worry about different
 *   initialisation procedures, depending on whether it's an instrument
 *   with the same address or not
 */

struct _vxi11_client_t {
	struct _vxi11_client_t *next;
	char address[20];
#ifndef WIN32
	CLIENT *client_address;
#endif
	int link_count;
};

static struct _vxi11_client_t *VXI11_CLIENTS = NULL;

/* Internal function declarations. */
static int _vxi11_open_link(VXI11_CLINK * clink, const char *address,
			    char *device);
static int _vxi11_close_link(VXI11_CLINK * clink, const char *address);

/*****************************************************************************
 * KEY USER FUNCTIONS - USE THESE FROM YOUR PROGRAMS OR INSTRUMENT LIBRARIES *
 *****************************************************************************/

/* OPEN FUNCTIONS *
 * ============== */

/* Use this function from user land to open a device and create a link. Can be
 * used multiple times for the same device (the library will keep track).*/
int vxi11_open_device(VXI11_CLINK **clink, const char *address, char *device)
{
#ifdef WIN32
	ViStatus status;
	char buf[256];
#else
	int ret;
	struct _vxi11_client_t *tail, *client = NULL;
#endif
	char default_device[6] = "inst0";
	char *use_device;

	if(device){
		use_device = device;
	}else{
		use_device = default_device;
	}

	*clink = (VXI11_CLINK *) calloc(1, sizeof(VXI11_CLINK));
	if (!(*clink)) {
		return 1;
	}

#ifdef WIN32
	status = viOpenDefaultRM(&clink->rm);
	if (status != VI_SUCCESS) {
		viStatusDesc(NULL, status, buf);
		printf("%s\n", buf);
		free(*clink);
		*clink = NULL;
		return 1;
	}
	viOpen(clink->rm, (char *)address, VI_NULL, VI_NULL, &clink->session);
	if (status != VI_SUCCESS) {
		viStatusDesc(clink->rm, status, buf);
		printf("%s\n", buf);
		free(*clink);
		*clink = NULL;
		return 1;
	}
#else
	/* Have a look to see if we've already initialised an instrument with
	 * this address */
	tail = VXI11_CLIENTS;
	while (tail) {
		if (strcmp(address, tail->address) == 0) {
			client = tail;
			break;
		}
		tail = tail->next;
	}

	/* Couldn't find a match, must be a new address */
	if (!client) {
		/* Create a new client, keep a note of where the client pointer
		 * is, for this address. Because it's a new client, this
		 * must be link number 1. Keep track of how many devices we've
		 * opened so we don't run out of storage space. */
		client = (struct _vxi11_client_t *)calloc(1, sizeof(struct _vxi11_client_t));
		if (!client) {
			free(*clink);
			*clink = NULL;
			return 1;
		}

		(*clink)->client =
		    clnt_create(address, DEVICE_CORE, DEVICE_CORE_VERSION,
				"tcp");

		if ((*clink)->client == NULL) {
			clnt_pcreateerror(address);
			free(client);
			free(*clink);
			*clink = NULL;
			return 1;
		}
		ret = _vxi11_open_link(*clink, address, device);
		if (ret != 0) {
			clnt_destroy((*clink)->client);
			free(client);
			free(*clink);
			*clink = NULL;
			return 1;
		}

		strncpy(client->address, address, 20);
		client->client_address = (*clink)->client;
		client->link_count = 1;
		client->next = VXI11_CLIENTS;
		VXI11_CLIENTS = client;
	} else {
		/* Copy the client pointer address. Just establish a new link
		 *  not a new client). Add one to the link count */
		(*clink)->client = client->client_address;
		ret = _vxi11_open_link((*clink), address, device);
		client->link_count++;
	}
#endif
	return 0;
}

/* CLOSE FUNCTION *
 * ============== */

/* Use this function from user land to close a device and/or sever a link. Can
 * be used multiple times for the same device (the library will keep track).*/
int vxi11_close_device(VXI11_CLINK * clink, const char *address)
{
	int ret = 0;
#ifdef WIN32
	viClose(clink->session);
	viClose(clink->rm);
#else
	struct _vxi11_client_t *tail, *last = NULL, *client = NULL;

	/* Which instrument are we referring to? */
	tail = VXI11_CLIENTS;
	while (tail) {
		if (strncmp(address, tail->address, 20) == 0) {
			client = tail;
			break;
		}
		last = tail;
		tail = tail->next;
	}

	/* Something's up if we can't find the address! */
	if (!client) {
		printf
		    ("vxi11_close_device: error: I have no record of you ever opening device\n");
		printf("                    with address %s\n", address);
		ret = -4;
	} else {		/* Found the address, there's more than one link to that instrument,
				 * so keep track and just close the link */
		if (client->link_count > 1) {
			ret = _vxi11_close_link(clink, address);
			client->link_count--;
		}
		/* Found the address, it's the last link, so close the device (link
		 * AND client) */
		else {
			ret = _vxi11_close_link(clink, address);
			clnt_destroy(clink->client);

			if (last) {
				last->next = client->next;
			} else {
				VXI11_CLIENTS = client->next;
			}
		}
	}
#endif
	return ret;
}

/* SEND FUNCTIONS *
 * ============== */

int vxi11_send_str(VXI11_CLINK * clink, const char *cmd)
{
	return vxi11_send(clink, cmd, strlen(cmd));
}

int vxi11_send(VXI11_CLINK * clink, const char *cmd, size_t len)
{
#ifdef WIN32
	ViStatus status;
	char buf[256];
	unsigned char *send_cmd;
#else
	Device_WriteParms write_parms;
	char *send_cmd;
#endif
	unsigned int bytes_left = len;
	unsigned long write_count;

#ifdef WIN32
	send_cmd = (unsigned char *)malloc(len);
	if (!send_cmd) {
		return 1;
	}
	memcpy(send_cmd, cmd, len);

	while (bytes_left > 0) {
		status =
		    viWrite(clink->session, send_cmd + (len - bytes_left),
			    bytes_left, &write_count);
		if (status == VI_SUCCESS) {
			bytes_left -= write_count;
		} else {
			free(send_cmd);
			viStatusDesc(clink->session, status, buf);
			printf("%s\n", buf);
			return status;
		}
	}
#else
	send_cmd = (char *)malloc(len);
	if (!send_cmd) {
		return 1;
	}
	memcpy(send_cmd, cmd, len);

	write_parms.lid = clink->link->lid;
	write_parms.io_timeout = VXI11_DEFAULT_TIMEOUT;
	write_parms.lock_timeout = VXI11_DEFAULT_TIMEOUT;

/* We can only write (link->maxRecvSize) bytes at a time, so we sit in a loop,
 * writing a chunk at a time, until we're done. */

	do {
		Device_WriteResp write_resp;
		memset(&write_resp, 0, sizeof(write_resp));

		if (bytes_left <= clink->link->maxRecvSize) {
			write_parms.flags = 8;
			write_parms.data.data_len = bytes_left;
		} else {
			write_parms.flags = 0;
			/* We need to check that maxRecvSize is a sane value (ie >0). Believe it
			 * or not, on some versions of Agilent Infiniium scope firmware the scope
			 * returned "0", which breaks Rule B.6.3 of the VXI-11 protocol. Nevertheless
			 * we need to catch this, otherwise the program just hangs. */
			if (clink->link->maxRecvSize > 0) {
				write_parms.data.data_len =
				    clink->link->maxRecvSize;
			} else {
				write_parms.data.data_len = 4096;	/* pretty much anything should be able to cope with 4kB */
			}
		}
		write_parms.data.data_val = send_cmd + (len - bytes_left);

		if (device_write_1(&write_parms, &write_resp, clink->client) !=
		    RPC_SUCCESS) {
			free(send_cmd);
			return -VXI11_NULL_WRITE_RESP;	/* The instrument did not acknowledge the write, just completely
							   dropped it. There was no vxi11 comms error as such, the 
							   instrument is just being rude. Usually occurs when the instrument
							   is busy. If we don't check this first, then the following 
							   line causes a seg fault */
		}
		if (write_resp.error != 0) {
			printf("vxi11_user: write error: %d\n",
			       (int)write_resp.error);
			free(send_cmd);
			return -(write_resp.error);
		}
		bytes_left -= write_resp.size;
	} while (bytes_left > 0);
#endif
	free(send_cmd);

	return 0;
}

/* RECEIVE FUNCTIONS *
 * ================= */

#define RCV_END_BIT	0x04	// An end indicator has been read
#define RCV_CHR_BIT	0x02	// A termchr is set in flags and a character which matches termChar is transferred
#define RCV_REQCNT_BIT	0x01	// requestSize bytes have been transferred.  This includes a request size of zero.

long vxi11_receive(VXI11_CLINK * clink, char *buffer, size_t len)
{
	return vxi11_receive_timeout(clink, buffer, len, VXI11_READ_TIMEOUT);
}

long vxi11_receive_timeout(VXI11_CLINK * clink, char *buffer, size_t len,
		   unsigned long timeout)
{
	unsigned long curr_pos = 0;
#ifdef WIN32
	viRead(clink->session, (unsigned char *)buffer, len, &curr_pos);
#else
	Device_ReadParms read_parms;
	Device_ReadResp read_resp;

	read_parms.lid = clink->link->lid;
	read_parms.requestSize = len;
	read_parms.io_timeout = timeout;	/* in ms */
	read_parms.lock_timeout = timeout;	/* in ms */
	read_parms.flags = 0;
	read_parms.termChar = 0;

	do {
		memset(&read_resp, 0, sizeof(read_resp));

		read_resp.data.data_val = buffer + curr_pos;
		read_parms.requestSize = len - curr_pos;	// Never request more total data than originally specified in len

		if (device_read_1(&read_parms, &read_resp, clink->client) != RPC_SUCCESS) {
			return -VXI11_NULL_READ_RESP;	/* there is nothing to read. Usually occurs after sending a query
							   which times out on the instrument. If we don't check this first,
							   then the following line causes a seg fault */
		}
		if (read_resp.error != 0) {
			/* Read failed for reason specified in error code.
			 *  (From published VXI-11 protocol, section B.5.2)
			 *  0   no error
			 *  1   syntax error
			 *  3   device not accessible
			 *  4   invalid link identifier
			 *  5   parameter error
			 *  6   channel not established
			 *  8   operation not supported
			 *  9   out of resources
			 *  11  device locked by another link
			 *  12  no lock held by this link
			 *  15  I/O timeout
			 *  17  I/O error
			 *  21  invalid address
			 *  23  abort
			 *  29  channel already established
			 */

			printf("vxi11_user: read error: %d\n", (int)read_resp.error);
			return -(read_resp.error);
		}

		if ((curr_pos + read_resp.data.data_len) <= len) {
			curr_pos += read_resp.data.data_len;
		}
		if ((read_resp.reason & RCV_END_BIT) || (read_resp.reason & RCV_CHR_BIT)) {
			break;
		} else if (curr_pos == len) {
			printf("xvi11_user: read error: buffer too small. Read %d bytes without hitting terminator.\n",
			     (int)curr_pos);
			return -100;
		}
	} while (1);
#endif
	return (curr_pos);	/*actual number of bytes received */
}

/*****************************************************************************
 * USEFUL ADDITIONAL HIGHER LEVER USER FUNCTIONS - USE THESE FROM YOUR       *
 * PROGRAMS OR INSTRUMENT LIBRARIES                                          *
 *****************************************************************************/

/* SEND FIXED LENGTH DATA BLOCK FUNCTION *
 * ===================================== */
int vxi11_send_data_block(VXI11_CLINK * clink, const char *cmd, char *buffer,
			  size_t len)
{
	char *out_buffer;
	size_t cmd_len = strlen(cmd);
	int ret;

	out_buffer = (char *)malloc(cmd_len + 10 + len);
	if (!out_buffer) {
		return 1;
	}
	sprintf(out_buffer, "%s#8%08lu", cmd, len);
	memcpy(out_buffer + cmd_len + 10, buffer, len);
	ret = vxi11_send(clink, out_buffer, cmd_len + 10 + len);
	free(out_buffer);
	return ret;
}

/* RECEIVE FIXED LENGTH DATA BLOCK FUNCTION *
 * ======================================== */

/* This function reads a response in the form of a definite-length block, such
 * as when you ask for waveform data. The data is returned in the following
 * format:
 *   #800001000<1000 bytes of data>
 *   ||\______/
 *   ||    |
 *   ||    \---- number of bytes of data
 *   |\--------- number of digits that follow (in this case 8, with leading 0's)
 *   \---------- always starts with #
 */
long vxi11_receive_data_block(VXI11_CLINK * clink, char *buffer,
			      size_t len, unsigned long timeout)
{
/* I'm not sure what the maximum length of this header is, I'll assume it's 
 * 11 (#9 + 9 digits) */
	unsigned long necessary_buffer_size;
	char *in_buffer;
	int ret;
	int ndigits;
	unsigned long returned_bytes;
	int l;
	char scan_cmd[20];
	necessary_buffer_size = len + 12;
	in_buffer = (char *)malloc(necessary_buffer_size);
	if (!in_buffer) {
		return -1;
	}
	ret = vxi11_receive_timeout(clink, in_buffer, necessary_buffer_size, timeout);
	if (ret < 0) {
		return ret;
	}
	if (in_buffer[0] != '#') {
		printf("vxi11_user: data block error: data block does not begin with '#'\n");
		printf("First 20 characters received were: '");
		for (l = 0; l < 20; l++) {
			printf("%c", in_buffer[l]);
		}
		printf("'\n");
		return -3;
	}

	/* first find out how many digits */
	sscanf(in_buffer, "#%1d", &ndigits);
	/* some instruments, if there is a problem acquiring the data, return only "#0" */
	if (ndigits > 0) {
		/* now that we know, we can convert the next <ndigits> bytes into an unsigned long */
		sprintf(scan_cmd, "#%%1d%%%dlu", ndigits);
		sscanf(in_buffer, scan_cmd, &ndigits, &returned_bytes);
		memcpy(buffer, in_buffer + (ndigits + 2), returned_bytes);
		free(in_buffer);
		return (long)returned_bytes;
	} else {
		return 0;
	}
}

/* SEND AND RECEIVE FUNCTION *
 * ========================= */

/* This is mainly a useful function for the overloaded vxi11_obtain_value()
 * fn's, but is also handy and useful for user and library use */
long vxi11_send_and_receive(VXI11_CLINK * clink, const char *cmd, char *buf,
			    size_t len, unsigned long timeout)
{
	int ret;
	long bytes_returned;
	do {
		ret = vxi11_send(clink, cmd, strlen(cmd));
		if (ret != 0) {
			if (ret != -VXI11_NULL_WRITE_RESP) {
				printf
				    ("Error: vxi11_send_and_receive: could not send cmd.\n");
				printf
				    ("       The function vxi11_send returned %d. ",
				     ret);
				return -1;
			} else {
				printf("(Info: VXI11_NULL_WRITE_RESP in vxi11_send_and_receive, resending query)\n");
			}
		}

		bytes_returned = vxi11_receive_timeout(clink, buf, len, timeout);
		if (bytes_returned <= 0) {
			if (bytes_returned > -VXI11_NULL_READ_RESP) {
				printf
				    ("Error: vxi11_send_and_receive: problem reading reply.\n");
				printf
				    ("       The function vxi11_receive returned %ld. ",
				     bytes_returned);
				return -2;
			} else {
				printf("(Info: VXI11_NULL_READ_RESP in vxi11_send_and_receive, resending query)\n");
			}
		}
	} while (bytes_returned == -VXI11_NULL_READ_RESP || ret == -VXI11_NULL_WRITE_RESP);
	return 0;
}

/* FUNCTIONS TO RETURN A LONG INTEGER VALUE SENT AS RESPONSE TO A QUERY *
 * ==================================================================== */
long vxi11_obtain_long_value(VXI11_CLINK * clink, const char *cmd)
{
	return vxi11_obtain_long_value_timeout(clink, cmd, VXI11_READ_TIMEOUT);
}

long vxi11_obtain_long_value_timeout(VXI11_CLINK * clink, const char *cmd,
			     unsigned long timeout)
{
	char buf[50];		/* 50=arbitrary length... more than enough for one number in ascii */
	memset(buf, 0, 50);
	if (vxi11_send_and_receive(clink, cmd, buf, 50, timeout) != 0) {
		printf("Returning 0\n");
		return 0;
	}
	return strtol(buf, (char **)NULL, 10);
}

/* FUNCTIONS TO RETURN A DOUBLE FLOAT VALUE SENT AS RESPONSE TO A QUERY *
 * ==================================================================== */
double vxi11_obtain_double_value(VXI11_CLINK * clink, const char *cmd)
{
	return vxi11_obtain_double_value_timeout(clink, cmd, VXI11_READ_TIMEOUT);
}

double vxi11_obtain_double_value_timeout(VXI11_CLINK * clink, const char *cmd,
				 unsigned long timeout)
{
	char buf[50];		/* 50=arbitrary length... more than enough for one number in ascii */
	double val;
	memset(buf, 0, 50);
	if (vxi11_send_and_receive(clink, cmd, buf, 50, timeout) != 0) {
		printf("Returning 0.0\n");
		return 0.0;
	}
	val = strtod(buf, (char **)NULL);
	return val;
}

/*****************************************************************************
 * CORE FUNCTIONS - YOU SHOULDN'T NEED TO USE THESE FROM YOUR PROGRAMS OR    *
 * INSTRUMENT LIBRARIES                                                      *
 *****************************************************************************/

/* OPEN FUNCTIONS *
 * ============== */

static int _vxi11_open_link(VXI11_CLINK * clink, const char *address,
			    char *device)
{
#ifndef WIN32
	Create_LinkParms link_parms;

	/* Set link parameters */
	link_parms.clientId = (long)clink->client;
	link_parms.lockDevice = 0;
	link_parms.lock_timeout = VXI11_DEFAULT_TIMEOUT;
	link_parms.device = device;

	clink->link = (Create_LinkResp *) calloc(1, sizeof(Create_LinkResp));

	if (create_link_1(&link_parms, clink->link, clink->client) !=
	    RPC_SUCCESS) {
		clnt_perror(clink->client, address);
		return -2;
	}
#endif
	return 0;
}

/* CLOSE FUNCTIONS *
 * =============== */

static int _vxi11_close_link(VXI11_CLINK * clink, const char *address)
{
#ifndef WIN32
	Device_Error dev_error;
	memset(&dev_error, 0, sizeof(dev_error));

	if (destroy_link_1(&clink->link->lid, &dev_error, clink->client) !=
	    RPC_SUCCESS) {
		clnt_perror(clink->client, address);
		return -1;
	}
#endif
	return 0;
}
