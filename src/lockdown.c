/*
 * lockdown.c
 * libiphone built-in lockdownd client
 * 
 * Copyright (c) 2008 Zach C. All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA 
 */

#include "usbmux.h"
#include "utils.h"
#include "iphone.h"
#include "lockdown.h"
#include "userpref.h"
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <glib.h>
#include <libtasn1.h>
#include <gnutls/x509.h>

#include <plist/plist.h>

const ASN1_ARRAY_TYPE pkcs1_asn1_tab[] = {
	{"PKCS1", 536872976, 0},
	{0, 1073741836, 0},
	{"RSAPublicKey", 536870917, 0},
	{"modulus", 1073741827, 0},
	{"publicExponent", 3, 0},
	{0, 0, 0}
};

/** Creates a lockdownd client for the give iPhone.
 *
 * @param phone The iPhone to create a lockdownd client for
 *
 * @return The lockdownd client.
 */
iphone_lckd_client_t new_lockdownd_client(iphone_device_t phone)
{
	if (!phone)
		return NULL;
	iphone_lckd_client_t control = (iphone_lckd_client_t) malloc(sizeof(struct iphone_lckd_client_int));

	if (IPHONE_E_SUCCESS != iphone_mux_new_client(phone, 0x0a00, 0xf27e, &control->connection)) {
		free(control);
		return NULL;
	}

	control->ssl_session = (gnutls_session_t *) malloc(sizeof(gnutls_session_t));
	control->in_SSL = 0;
	control->gtls_buffer_hack_len = 0;
	return control;
}

/** Closes the lockdownd client and does the necessary housekeeping.
 *
 * @param control The lockdown client
 */
iphone_error_t iphone_lckd_free_client(iphone_lckd_client_t client)
{
	if (!client)
		return IPHONE_E_INVALID_ARG;
	iphone_error_t ret = IPHONE_E_UNKNOWN_ERROR;

	if (client->connection) {
		ret = iphone_mux_free_client(client->connection);
	}

	if (client->ssl_session)
		gnutls_deinit(*client->ssl_session);
	free(client->ssl_session);
	free(client);
	return ret;
}

/** Polls the iPhone for lockdownd data.
 *
 * @param control The lockdownd client
 * @param dump_data The pointer to the location of the buffer in which to store
 *                  the received data
 *
 * @return The number of bytes received
 */
iphone_error_t iphone_lckd_recv(iphone_lckd_client_t client, char **dump_data, uint32_t * recv_bytes)
{
	if (!client || !dump_data || !recv_bytes)
		return IPHONE_E_INVALID_ARG;
	iphone_error_t ret = IPHONE_E_UNKNOWN_ERROR;
	char *receive;
	uint32_t datalen = 0, bytes = 0;

	if (!client->in_SSL)
		ret = iphone_mux_recv(client->connection, (char *) &datalen, sizeof(datalen), &bytes);
	else {
		bytes = gnutls_record_recv(*client->ssl_session, &datalen, sizeof(datalen));
		if (bytes > 0)
			ret = IPHONE_E_SUCCESS;
	}
	datalen = ntohl(datalen);

	receive = (char *) malloc(sizeof(char) * datalen);
	if (!client->in_SSL)
		ret = iphone_mux_recv(client->connection, receive, datalen, &bytes);
	else {
		bytes = gnutls_record_recv(*client->ssl_session, receive, datalen);
		if (bytes > 0)
			ret = IPHONE_E_SUCCESS;
	}
	*dump_data = receive;
	*recv_bytes = bytes;
	return ret;
}

/** Sends lockdownd data to the iPhone
 * 
 * @note This function is low-level and should only be used if you need to send
 *        a new type of message.
 *
 * @param control The lockdownd client
 * @param raw_data The null terminated string buffer to send
 * @param length The length of data to send
 *
 * @return The number of bytes sent
 */
iphone_error_t iphone_lckd_send(iphone_lckd_client_t client, char *raw_data, uint32_t length, uint32_t * sent_bytes)
{
	if (!client || !raw_data || length == 0 || !sent_bytes)
		return IPHONE_E_INVALID_ARG;
	char *real_query;
	int bytes;
	iphone_error_t ret = IPHONE_E_UNKNOWN_ERROR;

	real_query = (char *) malloc(sizeof(char) * (length + 4));
	length = htonl(length);
	memcpy(real_query, &length, sizeof(length));
	memcpy(real_query + 4, raw_data, ntohl(length));
	log_debug_msg("lockdownd_send(): made the query, sending it along\n");
	dump_debug_buffer("grpkt", real_query, ntohl(length) + 4);

	if (!client->in_SSL)
		ret = iphone_mux_send(client->connection, real_query, ntohl(length) + sizeof(length), &bytes);
	else {
		gnutls_record_send(*client->ssl_session, real_query, ntohl(length) + sizeof(length));
		ret = IPHONE_E_SUCCESS;
	}
	log_debug_msg("lockdownd_send(): sent it!\n");
	free(real_query);
	*sent_bytes = bytes;
	return ret;
}

/** Initiates the handshake for the lockdown session. Part of the lockdownd handshake.
 * 
 * @note You most likely want lockdownd_init unless you are doing something special.
 *
 * @param control The lockdownd client
 *
 * @return 1 on success and 0 on failure.
 */
iphone_error_t lockdownd_hello(iphone_lckd_client_t control)
{
	if (!control)
		return IPHONE_E_INVALID_ARG;

	int bytes = 0, i = 0;
	iphone_error_t ret = IPHONE_E_UNKNOWN_ERROR;

	plist_t dict = plist_new_dict();
	plist_add_sub_element(dict, PLIST_KEY, (void *) "Request", strlen("Request"));
	plist_add_sub_element(dict, PLIST_STRING, (void *) "QueryType", strlen("QueryType"));

	log_debug_msg("lockdownd_hello() called\n");
	char *XML_content = NULL;
	uint32_t length = 0;

	plist_to_xml(dict, &XML_content, &length);
	log_debug_msg("Send msg :\nsize : %i\nxml : %s", length, XML_content);
	ret = iphone_lckd_send(control, XML_content, length, &bytes);

	free(XML_content);
	XML_content = NULL;
	plist_free(dict);
	dict = NULL;

	ret = iphone_lckd_recv(control, &XML_content, &bytes);
	log_debug_msg("Receive msg :\nsize : %i\nxml : %s", bytes, XML_content);
	plist_from_xml(XML_content, bytes, &dict);

	if (!dict)
		return IPHONE_E_PLIST_ERROR;

	plist_t query_node = plist_find_node(dict, PLIST_STRING, "QueryType", strlen("QueryType"));
	plist_t result_node = plist_get_next_sibling(query_node);
	plist_t value_node = plist_get_next_sibling(result_node);

	plist_type result_type;
	plist_type value_type;

	char *result_value = NULL;
	char *value_value = NULL;
	uint64_t result_length = 0;
	uint64_t value_length = 0;

	plist_get_type_and_value(result_node, &result_type, (void *) (&result_value), &result_length);
	plist_get_type_and_value(value_node, &value_type, (void *) (&value_value), &value_length);

	if (result_type == PLIST_KEY &&
		value_type == PLIST_STRING && !strcmp(result_value, "Result") && !strcmp(value_value, "Success")) {
		log_debug_msg("lockdownd_hello(): success\n");
		ret = IPHONE_E_SUCCESS;
	}

	return ret;
}

/** Generic function to handle simple (key, value) requests.
 *
 * @param control an initialized lockdownd client.
 * @param key the key to request
 * @param value a pointer to the requested value
 *
 * @return IPHONE_E_SUCCESS on success.
 */
iphone_error_t lockdownd_generic_get_value(iphone_lckd_client_t control, const char *req_key, char *req_string,
										   gnutls_datum_t * value)
{
	if (!control || !req_key || !value || value->data)
		return IPHONE_E_INVALID_ARG;

	plist_t dict = NULL;
	int bytes = 0, i = 0;
	char *XML_content = NULL;
	uint32_t length = 0;
	iphone_error_t ret = IPHONE_E_UNKNOWN_ERROR;

	/* Setup DevicePublicKey request plist */
	dict = plist_new_dict();
	plist_add_sub_element(dict, PLIST_KEY, (void *) req_key, strlen(req_key));
	plist_add_sub_element(dict, PLIST_STRING, (void *) req_string, strlen(req_string));
	plist_add_sub_element(dict, PLIST_KEY, (void *) "Request", strlen("Request"));
	plist_add_sub_element(dict, PLIST_STRING, (void *) "GetValue", strlen("GetValue"));
	plist_to_xml(dict, &XML_content, &length);

	/* send to iPhone */
	log_debug_msg("Send msg :\nsize : %i\nxml : %s", length, XML_content);
	ret = iphone_lckd_send(control, XML_content, length, &bytes);

	free(XML_content);
	XML_content = NULL;
	plist_free(dict);
	dict = NULL;

	if (ret != IPHONE_E_SUCCESS)
		return ret;

	/* Now get iPhone's answer */
	ret = iphone_lckd_recv(control, &XML_content, &bytes);
	log_debug_msg("Receive msg :\nsize : %i\nxml : %s", bytes, XML_content);

	if (ret != IPHONE_E_SUCCESS)
		return ret;

	plist_from_xml(XML_content, bytes, &dict);
	if (!dict)
		return IPHONE_E_PLIST_ERROR;

	plist_t query_node = plist_find_node(dict, PLIST_STRING, "GetValue", strlen("GetValue"));
	plist_t result_key_node = plist_get_next_sibling(query_node);
	plist_t result_value_node = plist_get_next_sibling(result_key_node);

	plist_type result_key_type;
	plist_type result_value_type;
	char *result_key = NULL;
	char *result_value = NULL;
	uint64_t result_length = 0;
	uint64_t value_length = 0;

	plist_get_type_and_value(result_key_node, &result_key_type, (void *) (&result_key), &result_length);
	plist_get_type_and_value(result_value_node, &result_value_type, (void *) (&result_value), &value_length);

	if (result_key_type == PLIST_KEY &&
		result_value_type == PLIST_STRING && !strcmp(result_key, "Result") && !strcmp(result_value, "Success")) {
		log_debug_msg("lockdownd_generic_get_value(): success\n");
		ret = IPHONE_E_SUCCESS;
	}

	if (ret != IPHONE_E_SUCCESS) {
		return IPHONE_E_DICT_ERROR;
	}

	plist_t value_key_node = plist_get_next_sibling(result_key_node);
	plist_t value_value_node = plist_get_next_sibling(value_key_node);
	plist_type value_key_type;
	plist_type value_value_type;
	char *value_key = NULL;
	char *value_value = NULL;
	uint64_t key_length = 0;
	uint64_t valval_length = 0;

	plist_get_type_and_value(value_key_node, &value_key_type, (void *) (&value_key), &key_length);
	plist_get_type_and_value(value_value_node, &value_value_type, (void *) (&value_value), &valval_length);

	if (value_key_type == PLIST_KEY && !strcmp(result_key, "Value")) {
		log_debug_msg("lockdownd_generic_get_value(): success\n");
		value->data = value_value;
		value->size = valval_length;
		ret = IPHONE_E_SUCCESS;
	}

	plist_free(dict);
	free(XML_content);
	return ret;
}

/** Askes for the device's unique id. Part of the lockdownd handshake.
 *
 * @note You most likely want lockdownd_init unless you are doing something special.
 *
 * @return 1 on success and 0 on failure.
 */
iphone_error_t lockdownd_get_device_uid(iphone_lckd_client_t control, char **uid)
{
	gnutls_datum_t temp = { NULL, 0 };
	return lockdownd_generic_get_value(control, "Key", "UniqueDeviceID", &temp);
	*uid = temp.data;
}

/** Askes for the device's public key. Part of the lockdownd handshake.
 *
 * @note You most likely want lockdownd_init unless you are doing something special.
 *
 * @return 1 on success and 0 on failure.
 */
iphone_error_t lockdownd_get_device_public_key(iphone_lckd_client_t control, gnutls_datum_t * public_key)
{
	return lockdownd_generic_get_value(control, "Key", "DevicePublicKey", public_key);
}

/** Completes the entire lockdownd handshake.
 *
 * @param phone The iPhone
 * @param lockdownd_client The pointer to the location of the lockdownd_client
 *
 * @return 1 on success and 0 on failure
 */
iphone_error_t iphone_lckd_new_client(iphone_device_t device, iphone_lckd_client_t * client)
{
	if (!device || !client || (client && *client))
		return IPHONE_E_INVALID_ARG;
	iphone_error_t ret = IPHONE_E_SUCCESS;
	char *host_id = NULL;

	iphone_lckd_client_t client_loc = new_lockdownd_client(device);
	if (IPHONE_E_SUCCESS != lockdownd_hello(client_loc)) {
		fprintf(stderr, "Hello failed in the lockdownd client.\n");
		ret = IPHONE_E_NOT_ENOUGH_DATA;
	}


	char *uid = NULL;
	ret = lockdownd_get_device_uid(client_loc, &uid);
	if (IPHONE_E_SUCCESS != ret) {
		fprintf(stderr, "Device refused to send uid.\n");
	}

	host_id = get_host_id();
	if (IPHONE_E_SUCCESS == ret && !host_id) {
		fprintf(stderr, "No HostID found, run libiphone-initconf.\n");
		ret = IPHONE_E_INVALID_CONF;
	}

	if (IPHONE_E_SUCCESS == ret && !is_device_known(uid))
		ret = lockdownd_pair_device(client_loc, uid, host_id);

	if (uid) {
		free(uid);
		uid = NULL;
	}

	ret = lockdownd_start_SSL_session(client_loc, host_id);
	if (IPHONE_E_SUCCESS != ret) {
		ret = IPHONE_E_SSL_ERROR;
		fprintf(stderr, "SSL Session opening failed.\n");
	}

	if (host_id) {
		free(host_id);
		host_id = NULL;
	}

	if (IPHONE_E_SUCCESS == ret)
		*client = client_loc;
	return ret;
}

/** Generates the appropriate keys and pairs the device. It's part of the
 *  lockdownd handshake.
 *
 * @note You most likely want lockdownd_init unless you are doing something special.
 *
 * @return 1 on success and 0 on failure
 */
iphone_error_t lockdownd_pair_device(iphone_lckd_client_t control, char *uid, char *host_id)
{
	iphone_error_t ret = IPHONE_E_UNKNOWN_ERROR;
	plist_t dict = NULL;
	plist_t dict_record = NULL;
	int bytes = 0, i = 0;
	char *XML_content = NULL;
	uint32_t length = 0;

	gnutls_datum_t device_cert = { NULL, 0 };
	gnutls_datum_t host_cert = { NULL, 0 };
	gnutls_datum_t root_cert = { NULL, 0 };
	gnutls_datum_t public_key = { NULL, 0 };

	ret = lockdownd_get_device_public_key(control, &public_key);
	if (ret != IPHONE_E_SUCCESS) {
		fprintf(stderr, "Device refused to send public key.\n");
		return ret;
	}

	ret = lockdownd_gen_pair_cert(public_key, &device_cert, &host_cert, &root_cert);
	if (ret != IPHONE_E_SUCCESS) {
		free(public_key.data);
		return ret;
	}

	/* Setup Pair request plist */
	dict = plist_new_dict();
	plist_add_sub_element(dict, PLIST_KEY, (void *) "PairRecord", strlen("PairRecord"));
	dict_record = plist_add_sub_element(dict, PLIST_DICT, NULL, 0);
	plist_add_sub_element(dict_record, PLIST_KEY, (void *) "DeviceCertificate", strlen("DeviceCertificate"));
	plist_add_sub_element(dict_record, PLIST_DATA, (void *) device_cert.data, device_cert.size);
	plist_add_sub_element(dict_record, PLIST_KEY, (void *) "HostCertificate", strlen("HostCertificate"));
	plist_add_sub_element(dict_record, PLIST_DATA, (void *) host_cert.data, host_cert.size);
	plist_add_sub_element(dict_record, PLIST_KEY, (void *) "HostID", strlen("HostID"));
	plist_add_sub_element(dict_record, PLIST_STRING, (void *) host_id, strlen(host_id));
	plist_add_sub_element(dict_record, PLIST_KEY, (void *) "RootCertificate", strlen("RootCertificate"));
	plist_add_sub_element(dict_record, PLIST_DATA, (void *) root_cert.data, root_cert.size);
	plist_add_sub_element(dict_record, PLIST_KEY, (void *) "Request", strlen("Request"));
	plist_add_sub_element(dict_record, PLIST_STRING, (void *) "Pair", strlen("Pair"));
	plist_to_xml(dict, &XML_content, &length);
	log_debug_msg("XML Pairing request :\nsize : %i\nxml :\n %s", length, XML_content);

	/* send to iPhone */
	ret = iphone_lckd_send(control, XML_content, length, &bytes);

	free(XML_content);
	plist_free(dict);
	dict = NULL;

	if (ret != IPHONE_E_SUCCESS)
		return ret;

	/* Now get iPhone's answer */
	ret = iphone_lckd_recv(control, &XML_content, &bytes);

	if (ret != IPHONE_E_SUCCESS)
		return ret;

	log_debug_msg("lockdown_pair_device: iPhone's response to our pair request:\n");
	log_debug_msg(XML_content);
	log_debug_msg("\n\n");

	plist_from_xml(XML_content, bytes, &dict);
	if (!dict)
		return IPHONE_E_PLIST_ERROR;

	plist_t query_node = plist_find_node(dict, PLIST_STRING, "Pair", strlen("Pair"));
	plist_t result_key_node = plist_get_next_sibling(query_node);
	plist_t result_value_node = plist_get_next_sibling(result_key_node);

	plist_type result_key_type;
	plist_type result_value_type;
	char *result_key = NULL;
	char *result_value = NULL;
	uint64_t key_length = 0;
	uint64_t val_length = 0;

	plist_get_type_and_value(result_key_node, &result_key_type, (void *) (&result_key), &key_length);
	plist_get_type_and_value(result_value_node, &result_value_type, (void *) (&result_value), &val_length);

	if (result_key_type == PLIST_KEY &&
		result_value_type == PLIST_STRING && !strcmp(result_key, "Result") && !strcmp(result_value, "Success")) {
		ret = IPHONE_E_SUCCESS;
	}

	/* store public key in config if pairing succeeded */
	if (ret == IPHONE_E_SUCCESS) {
		log_debug_msg("lockdownd_pair_device: pair success\n");
		store_device_public_key(uid, public_key);
		ret = IPHONE_E_SUCCESS;
	} else {
		log_debug_msg("lockdownd_pair_device: pair failure\n");
		ret = IPHONE_E_PAIRING_FAILED;
	}
	free(public_key.data);
	return ret;
}

/** Generates the device certificate from the public key as well as the host
 *  and root certificates.
 * 
 * @return IPHONE_E_SUCCESS on success.
 */
iphone_error_t lockdownd_gen_pair_cert(gnutls_datum_t public_key, gnutls_datum_t * odevice_cert,
									   gnutls_datum_t * ohost_cert, gnutls_datum_t * oroot_cert)
{
	if (!public_key.data || !odevice_cert || !ohost_cert || !oroot_cert)
		return IPHONE_E_INVALID_ARG;
	iphone_error_t ret = IPHONE_E_UNKNOWN_ERROR;

	gnutls_datum_t modulus = { NULL, 0 };
	gnutls_datum_t exponent = { NULL, 0 };

	/* now decode the PEM encoded key */
	gnutls_datum_t der_pub_key;
	if (GNUTLS_E_SUCCESS == gnutls_pem_base64_decode_alloc("RSA PUBLIC KEY", &public_key, &der_pub_key)) {

		/* initalize asn.1 parser */
		ASN1_TYPE pkcs1 = ASN1_TYPE_EMPTY;
		if (ASN1_SUCCESS == asn1_array2tree(pkcs1_asn1_tab, &pkcs1, NULL)) {

			ASN1_TYPE asn1_pub_key = ASN1_TYPE_EMPTY;
			asn1_create_element(pkcs1, "PKCS1.RSAPublicKey", &asn1_pub_key);

			if (ASN1_SUCCESS == asn1_der_decoding(&asn1_pub_key, der_pub_key.data, der_pub_key.size, NULL)) {

				/* get size to read */
				int ret1 = asn1_read_value(asn1_pub_key, "modulus", NULL, &modulus.size);
				int ret2 = asn1_read_value(asn1_pub_key, "publicExponent", NULL, &exponent.size);

				modulus.data = gnutls_malloc(modulus.size);
				exponent.data = gnutls_malloc(exponent.size);

				ret1 = asn1_read_value(asn1_pub_key, "modulus", modulus.data, &modulus.size);
				ret2 = asn1_read_value(asn1_pub_key, "publicExponent", exponent.data, &exponent.size);
				if (ASN1_SUCCESS == ret1 && ASN1_SUCCESS == ret2)
					ret = IPHONE_E_SUCCESS;
			}
			if (asn1_pub_key)
				asn1_delete_structure(&asn1_pub_key);
		}
		if (pkcs1)
			asn1_delete_structure(&pkcs1);
	}

	/* now generate certifcates */
	if (IPHONE_E_SUCCESS == ret && 0 != modulus.size && 0 != exponent.size) {

		gnutls_global_init();
		gnutls_datum_t essentially_null = { strdup("abababababababab"), strlen("abababababababab") };

		gnutls_x509_privkey_t fake_privkey, root_privkey;
		gnutls_x509_crt_t dev_cert, root_cert, host_cert;

		gnutls_x509_privkey_init(&fake_privkey);
		gnutls_x509_crt_init(&dev_cert);
		gnutls_x509_crt_init(&root_cert);
		gnutls_x509_crt_init(&host_cert);

		if (GNUTLS_E_SUCCESS ==
			gnutls_x509_privkey_import_rsa_raw(fake_privkey, &modulus, &exponent, &essentially_null, &essentially_null,
											   &essentially_null, &essentially_null)) {

			gnutls_x509_privkey_init(&root_privkey);

			/* get root cert */
			gnutls_datum_t pem_root_cert = { NULL, 0 };
			get_root_certificate(&pem_root_cert);
			if (GNUTLS_E_SUCCESS != gnutls_x509_crt_import(root_cert, &pem_root_cert, GNUTLS_X509_FMT_PEM))
				ret = IPHONE_E_SSL_ERROR;

			/* get host cert */
			gnutls_datum_t pem_host_cert = { NULL, 0 };
			get_host_certificate(&pem_host_cert);
			if (GNUTLS_E_SUCCESS != gnutls_x509_crt_import(host_cert, &pem_host_cert, GNUTLS_X509_FMT_PEM))
				ret = IPHONE_E_SSL_ERROR;

			/* get root private key */
			gnutls_datum_t pem_root_priv = { NULL, 0 };
			get_root_private_key(&pem_root_priv);
			if (GNUTLS_E_SUCCESS != gnutls_x509_privkey_import(root_privkey, &pem_root_priv, GNUTLS_X509_FMT_PEM))
				ret = IPHONE_E_SSL_ERROR;

			/* generate device certificate */
			gnutls_x509_crt_set_key(dev_cert, fake_privkey);
			gnutls_x509_crt_set_serial(dev_cert, "\x00", 1);
			gnutls_x509_crt_set_version(dev_cert, 3);
			gnutls_x509_crt_set_ca_status(dev_cert, 0);
			gnutls_x509_crt_set_activation_time(dev_cert, time(NULL));
			gnutls_x509_crt_set_expiration_time(dev_cert, time(NULL) + (60 * 60 * 24 * 365 * 10));
			gnutls_x509_crt_sign(dev_cert, root_cert, root_privkey);

			if (IPHONE_E_SUCCESS == ret) {
				/* if everything went well, export in PEM format */
				gnutls_datum_t dev_pem = { NULL, 0 };
				gnutls_x509_crt_export(dev_cert, GNUTLS_X509_FMT_PEM, NULL, &dev_pem.size);
				dev_pem.data = gnutls_malloc(dev_pem.size);
				gnutls_x509_crt_export(dev_cert, GNUTLS_X509_FMT_PEM, dev_pem.data, &dev_pem.size);

				/* copy buffer for output */
				odevice_cert->data = malloc(dev_pem.size);
				memcpy(odevice_cert->data, dev_pem.data, dev_pem.size);
				odevice_cert->size = dev_pem.size;

				ohost_cert->data = malloc(pem_host_cert.size);
				memcpy(ohost_cert->data, pem_host_cert.data, pem_host_cert.size);
				ohost_cert->size = pem_host_cert.size;

				oroot_cert->data = malloc(pem_root_cert.size);
				memcpy(oroot_cert->data, pem_root_cert.data, pem_root_cert.size);
				oroot_cert->size = pem_root_cert.size;
			}
			gnutls_free(pem_root_priv.data);
			gnutls_free(pem_root_cert.data);
			gnutls_free(pem_host_cert.data);
		}
	}

	gnutls_free(modulus.data);
	gnutls_free(exponent.data);

	gnutls_free(der_pub_key.data);

	return ret;
}

/** Starts SSL communication with lockdownd after the iPhone has been paired.
 *
 * @param control The lockdownd client
 * @param HostID The HostID used with this phone
 *
 * @return 1 on success and 0 on failure
 */
iphone_error_t lockdownd_start_SSL_session(iphone_lckd_client_t control, const char *HostID)
{
	plist_t dict = NULL;
	char *XML_content = NULL;
	uint32_t length = 0, bytes = 0, return_me = 0;

	iphone_error_t ret = IPHONE_E_UNKNOWN_ERROR;

	/* Setup DevicePublicKey request plist */
	dict = plist_new_dict();
	plist_add_sub_element(dict, PLIST_KEY, (void *) "HostID", strlen("HostID"));
	plist_add_sub_element(dict, PLIST_STRING, (void *) HostID, strlen(HostID));
	plist_add_sub_element(dict, PLIST_KEY, (void *) "Request", strlen("Request"));
	plist_add_sub_element(dict, PLIST_STRING, (void *) "StartSession", strlen("StartSession"));
	plist_to_xml(dict, &XML_content, &length);
	log_debug_msg("Send msg :\nsize : %i\nxml : %s", length, XML_content);

	ret = iphone_lckd_send(control, XML_content, length, &bytes);

	free(XML_content);
	XML_content = NULL;
	plist_free(dict);
	dict = NULL;

	if (ret != IPHONE_E_SUCCESS)
		return ret;

	if (bytes > 0) {
		ret = iphone_lckd_recv(control, &XML_content, &bytes);
		log_debug_msg("Receive msg :\nsize : %i\nxml : %s", bytes, XML_content);
		plist_from_xml(XML_content, bytes, &dict);
		if (!dict)
			return IPHONE_E_PLIST_ERROR;

		plist_t query_node = plist_find_node(dict, PLIST_STRING, "StartSession", strlen("StartSession"));
		plist_t result_key_node = plist_get_next_sibling(query_node);
		plist_t result_value_node = plist_get_next_sibling(result_key_node);

		plist_type result_key_type;
		plist_type result_value_type;
		char *result_key = NULL;
		char *result_value = NULL;
		uint64_t key_length = 0;
		uint64_t val_length = 0;

		plist_get_type_and_value(result_key_node, &result_key_type, (void *) (&result_key), &key_length);
		plist_get_type_and_value(result_value_node, &result_value_type, (void *) (&result_value), &val_length);

		free(XML_content);
		XML_content = NULL;
		plist_free(dict);
		dict = NULL;

		if (result_key_type == PLIST_KEY &&
			result_value_type == PLIST_STRING && !strcmp(result_key, "Result") && !strcmp(result_value, "Success")) {
			// Set up GnuTLS...
			//gnutls_anon_client_credentials_t anoncred;
			gnutls_certificate_credentials_t xcred;

			log_debug_msg("We started the session OK, now trying GnuTLS\n");
			errno = 0;
			gnutls_global_init();
			//gnutls_anon_allocate_client_credentials(&anoncred);
			gnutls_certificate_allocate_credentials(&xcred);
			gnutls_certificate_set_x509_trust_file(xcred, "hostcert.pem", GNUTLS_X509_FMT_PEM);
			gnutls_init(control->ssl_session, GNUTLS_CLIENT);
			{
				int protocol_priority[16] = { GNUTLS_SSL3, 0 };
				int kx_priority[16] = { GNUTLS_KX_ANON_DH, GNUTLS_KX_RSA, 0 };
				int cipher_priority[16] = { GNUTLS_CIPHER_AES_128_CBC, GNUTLS_CIPHER_AES_256_CBC, 0 };
				int mac_priority[16] = { GNUTLS_MAC_SHA1, GNUTLS_MAC_MD5, 0 };
				int comp_priority[16] = { GNUTLS_COMP_NULL, 0 };

				gnutls_cipher_set_priority(*control->ssl_session, cipher_priority);
				gnutls_compression_set_priority(*control->ssl_session, comp_priority);
				gnutls_kx_set_priority(*control->ssl_session, kx_priority);
				gnutls_protocol_set_priority(*control->ssl_session, protocol_priority);
				gnutls_mac_set_priority(*control->ssl_session, mac_priority);

			}
			gnutls_credentials_set(*control->ssl_session, GNUTLS_CRD_CERTIFICATE, xcred);	// this part is killing me.

			log_debug_msg("GnuTLS step 1...\n");
			gnutls_transport_set_ptr(*control->ssl_session, (gnutls_transport_ptr_t) control);
			log_debug_msg("GnuTLS step 2...\n");
			gnutls_transport_set_push_function(*control->ssl_session, (gnutls_push_func) & lockdownd_secuwrite);
			log_debug_msg("GnuTLS step 3...\n");
			gnutls_transport_set_pull_function(*control->ssl_session, (gnutls_pull_func) & lockdownd_securead);
			log_debug_msg("GnuTLS step 4 -- now handshaking...\n");

			if (errno)
				log_debug_msg("WARN: errno says %s before handshake!\n", strerror(errno));
			return_me = gnutls_handshake(*control->ssl_session);
			log_debug_msg("GnuTLS handshake done...\n");

			if (return_me != GNUTLS_E_SUCCESS) {
				log_debug_msg("GnuTLS reported something wrong.\n");
				gnutls_perror(return_me);
				log_debug_msg("oh.. errno says %s\n", strerror(errno));
				return IPHONE_E_SSL_ERROR;
			} else {
				control->in_SSL = 1;
				return IPHONE_E_SUCCESS;
			}
		}

		log_debug_msg("Apparently failed negotiating with lockdownd.\n");
		log_debug_msg("Responding dictionary: \n");
		return IPHONE_E_SSL_ERROR;
	} else {
		log_debug_msg("Didn't get enough bytes.\n");
		return IPHONE_E_NOT_ENOUGH_DATA;
	}
}

/** gnutls callback for writing data to the iPhone.
 *
 * @param transport It's really the lockdownd client, but the method signature has to match
 * @param buffer The data to send
 * @param length The length of data to send in bytes
 *
 * @return The number of bytes sent
 */
ssize_t lockdownd_secuwrite(gnutls_transport_ptr_t transport, char *buffer, size_t length)
{
	int bytes = 0;
	iphone_lckd_client_t control;
	control = (iphone_lckd_client_t) transport;
	log_debug_msg("lockdownd_secuwrite() called\n");
	log_debug_msg("pre-send\nlength = %zi\n", length);
	iphone_mux_send(control->connection, buffer, length, &bytes);
	log_debug_msg("post-send\nsent %i bytes\n", bytes);

	dump_debug_buffer("sslpacketwrite.out", buffer, length);
	return bytes;
}

/** gnutls callback for reading data from the iPhone
 *
 * @param transport It's really the lockdownd client, but the method signature has to match
 * @param buffer The buffer to store data in
 * @param length The length of data to read in bytes
 *
 * @return The number of bytes read
 */
ssize_t lockdownd_securead(gnutls_transport_ptr_t transport, char *buffer, size_t length)
{
	int bytes = 0, pos_start_fill = 0;
	char *hackhackhack = NULL;
	iphone_lckd_client_t control;
	control = (iphone_lckd_client_t) transport;
	log_debug_msg("lockdownd_securead() called\nlength = %zi\n", length);
	// Buffering hack! Throw what we've got in our "buffer" into the stream first, then get more.
	if (control->gtls_buffer_hack_len > 0) {
		if (length > control->gtls_buffer_hack_len) {	// If it's asking for more than we got
			length -= control->gtls_buffer_hack_len;	// Subtract what we have from their requested length
			pos_start_fill = control->gtls_buffer_hack_len;	// set the pos to start filling at
			memcpy(buffer, control->gtls_buffer_hack, control->gtls_buffer_hack_len);	// Fill their buffer partially
			free(control->gtls_buffer_hack);	// free our memory, it's not chained anymore
			control->gtls_buffer_hack_len = 0;	// we don't have a hack buffer anymore
			log_debug_msg("Did a partial fill to help quench thirst for data\n");
		} else if (length < control->gtls_buffer_hack_len) {	// If it's asking for less...
			control->gtls_buffer_hack_len -= length;	// subtract what they're asking for
			memcpy(buffer, control->gtls_buffer_hack, length);	// fill their buffer
			hackhackhack = (char *) malloc(sizeof(char) * control->gtls_buffer_hack_len);	// strndup is NOT a good solution -- concatenates \0!!!! Anyway, make a new "hack" buffer.
			memcpy(hackhackhack, control->gtls_buffer_hack + length, control->gtls_buffer_hack_len);	// Move what's left into the new one
			free(control->gtls_buffer_hack);	// Free the old one
			control->gtls_buffer_hack = hackhackhack;	// And make it the new one.
			hackhackhack = NULL;
			log_debug_msg("Quenched the thirst for data; new hack length is %i\n", control->gtls_buffer_hack_len);
			return length;		// hand it over.
		} else {				// length == hack length
			memcpy(buffer, control->gtls_buffer_hack, length);	// copy our buffer into theirs
			free(control->gtls_buffer_hack);	// free our "obligation"
			control->gtls_buffer_hack_len = 0;	// free our "obligation"
			log_debug_msg("Satiated the thirst for data; now we have to eventually receive again.\n");
			return length;		// hand it over
		}
	}
	// End buffering hack!
	char *recv_buffer = (char *) malloc(sizeof(char) * (length * 1000));	// ensuring nothing stupid happens

	log_debug_msg("pre-read\nclient wants %zi bytes\n", length);
	iphone_mux_recv(control->connection, recv_buffer, (length * 1000), &bytes);
	log_debug_msg("post-read\nwe got %i bytes\n", bytes);
	if (bytes < 0) {
		log_debug_msg("lockdownd_securead(): uh oh\n");
		log_debug_msg
			("I believe what we have here is a failure to communicate... libusb says %s but strerror says %s\n",
			 usb_strerror(), strerror(errno));
		return bytes + 28;		// an errno
	}
	if (bytes >= length) {
		if (bytes > length) {
			log_debug_msg
				("lockdownd_securead: Client deliberately read less data than was there; resorting to GnuTLS buffering hack.\n");
			if (!control->gtls_buffer_hack_len) {	// if there's no hack buffer yet
				//control->gtls_buffer_hack = strndup(recv_buffer+length, bytes-length); // strndup is NOT a good solution!
				control->gtls_buffer_hack_len += bytes - length;
				control->gtls_buffer_hack = (char *) malloc(sizeof(char) * control->gtls_buffer_hack_len);
				memcpy(control->gtls_buffer_hack, recv_buffer + length, control->gtls_buffer_hack_len);
			} else {			// if there is. 
				control->gtls_buffer_hack =
					realloc(control->gtls_buffer_hack, control->gtls_buffer_hack_len + (bytes - length));
				memcpy(control->gtls_buffer_hack + control->gtls_buffer_hack_len, recv_buffer + length, bytes - length);
				control->gtls_buffer_hack_len += bytes - length;
			}
		}
		memcpy(buffer + pos_start_fill, recv_buffer, length);
		free(recv_buffer);
		if (bytes == length) {
			log_debug_msg("Returning how much we received.\n");
			return bytes;
		} else {
			log_debug_msg("Returning what they want to hear.\nHack length: %i\n", control->gtls_buffer_hack_len);
			return length;
		}
	}
	return bytes;
}

/** Command to start the desired service
 *
 * @param control The lockdownd client
 * @param service The name of the service to start
 *
 * @return The port number the service was started on or 0 on failure.
 */
iphone_error_t iphone_lckd_start_service(iphone_lckd_client_t client, const char *service, int *port)
{
	if (!client || !service || !port)
		return IPHONE_E_INVALID_ARG;

	char *host_id = get_host_id();
	if (!host_id)
		return IPHONE_E_INVALID_CONF;
	if (!client->in_SSL && !lockdownd_start_SSL_session(client, host_id))
		return IPHONE_E_SSL_ERROR;


	plist_t dict = NULL;
	char *XML_content = NULL;
	uint32_t length, i = 0, port_loc = 0, bytes = 0;
	iphone_error_t ret = IPHONE_E_UNKNOWN_ERROR;

	free(host_id);
	host_id = NULL;

	dict = plist_new_dict();
	plist_add_sub_element(dict, PLIST_KEY, (void *) "Request", strlen("Request"));
	plist_add_sub_element(dict, PLIST_STRING, (void *) "StartService", strlen("StartService"));
	plist_add_sub_element(dict, PLIST_KEY, (void *) "Service", strlen("Service"));
	plist_add_sub_element(dict, PLIST_STRING, (void *) service, strlen(service));
	plist_to_xml(dict, &XML_content, &length);

	/* send to iPhone */
	log_debug_msg("Send msg :\nsize : %i\nxml : %s", length, XML_content);
	ret = iphone_lckd_send(client, XML_content, length, &bytes);

	free(XML_content);
	XML_content = NULL;
	plist_free(dict);
	dict = NULL;

	if (IPHONE_E_SUCCESS != ret)
		return ret;

	ret = iphone_lckd_recv(client, &XML_content, &bytes);

	if (IPHONE_E_SUCCESS != ret)
		return ret;

	plist_from_xml(XML_content, bytes, &dict);
	if (!dict)
		return IPHONE_E_PLIST_ERROR;


	if (bytes <= 0)
		return IPHONE_E_NOT_ENOUGH_DATA;
	else {

		plist_t query_node = plist_find_node(dict, PLIST_STRING, "StartService", strlen("StartService"));
		plist_t result_key_node = plist_get_next_sibling(query_node);
		plist_t result_value_node = plist_get_next_sibling(result_key_node);

		plist_t port_key_node = plist_find_node(dict, PLIST_KEY, "Port", strlen("Port"));
		plist_t port_value_node = plist_get_next_sibling(port_key_node);

		plist_type result_key_type;
		plist_type result_value_type;
		plist_type port_key_type;
		plist_type port_value_type;
		char *result_key = NULL;
		char *result_value = NULL;
		char *port_key = NULL;
		uint64_t res_key_length = 0;
		uint64_t res_val_length = 0;
		uint64_t port_key_length = 0;
		uint64_t port_val_length = 0;
		uint64_t port_value = 0;

		plist_get_type_and_value(result_key_node, &result_key_type, (void *) (&result_key), &res_key_length);
		plist_get_type_and_value(result_value_node, &result_value_type, (void *) (&result_value), &res_val_length);
		plist_get_type_and_value(port_key_node, &port_key_type, (void *) (&port_key), &port_key_length);
		plist_get_type_and_value(port_value_node, &port_value_type, (void *) (&port_value), &port_val_length);

		if (result_key_type == PLIST_KEY &&
			result_value_type == PLIST_STRING &&
			port_key_type == PLIST_KEY &&
			port_value_type == PLIST_UINT &&
			!strcmp(result_key, "Result") && !strcmp(result_value, "Success") && !strcmp(port_key, "Port")) {
			port_loc = port_value;
			ret = IPHONE_E_SUCCESS;
		}

		log_debug_msg("lockdownd_start_service(): DATA RECEIVED:\n\n");
		log_debug_msg(XML_content);
		log_debug_msg("end data received by lockdownd_start_service()\n");

		free(XML_content);
		plist_free(dict);
		dict = NULL;
		if (port && ret == IPHONE_E_SUCCESS) {
			*port = port_loc;
			return IPHONE_E_SUCCESS;
		} else
			return IPHONE_E_UNKNOWN_ERROR;
	}

	return IPHONE_E_UNKNOWN_ERROR;
}
