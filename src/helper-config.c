/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* nm-openvpn-service-openvpn-helper - helper called after OpenVPN established
 * a connection, uses DBUS to send information back to nm-openvpn-service
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright 2005 - 2014 Red Hat, Inc.
 */

#include "config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "nm-utils.h"

#include "helper-config.h"
#include "nm-openvpn-service.h"

static GValue *
str_to_gvalue (const char *str, gboolean try_convert, GError **error)
{
	GValue *val;
	char *converted = NULL;

	/* Empty */
	if (!str || !str[0]) {
		g_set_error_literal (error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
		                     "expected non-empty string");
		return NULL;
	}

	if (!g_utf8_validate (str, -1, NULL)) {
		if (try_convert) {
			converted = g_convert (str, -1, "ISO-8859-1", "UTF-8", NULL, NULL, NULL);
			if (!converted)
				converted = g_convert (str, -1, "C", "UTF-8", NULL, NULL, NULL);
			if (!converted) {
				g_set_error_literal (error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
				                     "failed to convert non-UTF-8 string");
				return NULL;
			}
			str = converted;
		} else {
			g_set_error_literal (error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
			                     "string not UTF-8");
			return NULL;
		}
	}

	val = g_slice_new0 (GValue);
	g_value_init (val, G_TYPE_STRING);
	g_value_set_string (val, str);
	g_free (converted);

	return val;
}

static GValue *
uint_to_gvalue (guint32 num)
{
	GValue *val;

	if (num == 0)
		return NULL;

	val = g_slice_new0 (GValue);
	g_value_init (val, G_TYPE_UINT);
	g_value_set_uint (val, num);

	return val;
}

static GValue *
bool_to_gvalue (gboolean b)
{
	GValue *val;

	val = g_slice_new0 (GValue);
	g_value_init (val, G_TYPE_BOOLEAN);
	g_value_set_boolean (val, b);
	return val;
}

static GValue *
addr4_to_gvalue (const char *str, GError **error)
{
	struct in_addr	temp_addr;
	GValue *val;

	/* Empty */
	if (!str || !str[0]) {
		g_set_error_literal (error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
		                     "expected IPv4 address, not empty string");
		return NULL;
	}

	if (inet_pton (AF_INET, str, &temp_addr) <= 0) {
		g_set_error (error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
		             "failed to convert IPv4 address '%s'", str);
		return NULL;
	}

	val = g_slice_new0 (GValue);
	g_value_init (val, G_TYPE_UINT);
	g_value_set_uint (val, temp_addr.s_addr);

	return val;
}

static GValue *
parse_addr4_list (GValue *value_array, const char *str)
{
	char **split;
	int i;
	struct in_addr temp_addr;
	GArray *array;

	/* Empty */
	if (!str || !str[0])
		return value_array;

	if (value_array)
		array = (GArray *) g_value_get_boxed (value_array);
	else
		array = g_array_new (FALSE, FALSE, sizeof (guint));

	split = g_strsplit (str, " ", -1);
	for (i = 0; split[i]; i++) {
		if (inet_pton (AF_INET, split[i], &temp_addr) > 0)
			g_array_append_val (array, temp_addr.s_addr);
	}

	g_strfreev (split);

	if (!value_array && array->len > 0) {
		value_array = g_slice_new0 (GValue);
		g_value_init (value_array, DBUS_TYPE_G_UINT_ARRAY);
		g_value_set_boxed (value_array, array);
	}
	if (!value_array)
		g_array_free (array, TRUE);

	return value_array;
}

static GValue *
addr6_to_gvalue (const char *str, GError **error)
{
	struct in6_addr temp_addr;
	GValue *val;
	GByteArray *ba;

	/* Empty */
	if (!str || !str[0]) {
		g_set_error_literal (error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
		                     "expected IPv6 address, not empty string");
		return NULL;
	}

	if (inet_pton (AF_INET6, str, &temp_addr) <= 0) {
		g_set_error (error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
		             "failed to convert IPv6 address '%s'", str);
		return NULL;
	}

	val = g_slice_new0 (GValue);
	g_value_init (val, DBUS_TYPE_G_UCHAR_ARRAY);
	ba = g_byte_array_new ();
	g_byte_array_append (ba, (guint8 *) &temp_addr, sizeof (temp_addr));
	g_value_take_boxed (val, ba);
	return val;
}

static inline gboolean
is_domain_valid (const char *str)
{
	return (str && (strlen(str) >= 1) && (strlen(str) <= 255));
}

#define BUFLEN 256

static GValue *
get_ip4_routes (void)
{
	GValue *value = NULL;
	GPtrArray *routes;
	char *tmp;
	int i;

	routes = g_ptr_array_new ();

	for (i = 1; i < 256; i++) {
		GArray *array;
		char buf[BUFLEN];
		struct in_addr network;
		struct in_addr netmask;
		struct in_addr gateway = { 0, };
		guint32 prefix, metric = 0;

		snprintf (buf, BUFLEN, "route_network_%d", i);
		tmp = getenv (buf);
		if (!tmp || !tmp[0])
			break;

		if (inet_pton (AF_INET, tmp, &network) <= 0) {
			g_warning ("Ignoring invalid static route address '%s'", tmp ? tmp : "NULL");
			continue;
		}

		snprintf (buf, BUFLEN, "route_netmask_%d", i);
		tmp = getenv (buf);
		if (!tmp || inet_pton (AF_INET, tmp, &netmask) <= 0) {
			g_warning ("Ignoring invalid static route netmask '%s'", tmp ? tmp : "NULL");
			continue;
		}

		snprintf (buf, BUFLEN, "route_gateway_%d", i);
		tmp = getenv (buf);
		/* gateway can be missing */
		if (tmp && (inet_pton (AF_INET, tmp, &gateway) <= 0)) {
			g_warning ("Ignoring invalid static route gateway '%s'", tmp ? tmp : "NULL");
			continue;
		}

		snprintf (buf, BUFLEN, "route_metric_%d", i);
		tmp = getenv (buf);
		/* metric can be missing */
		if (tmp && tmp[0]) {
			long int tmp_metric;

			errno = 0;
			tmp_metric = strtol (tmp, NULL, 10);
			if (errno || tmp_metric < 0 || tmp_metric > G_MAXUINT32) {
				g_warning ("Ignoring invalid static route metric '%s'", tmp);
				continue;
			}
			metric = (guint32) tmp_metric;
		}

		array = g_array_sized_new (FALSE, TRUE, sizeof (guint32), 4);
		g_array_append_val (array, network.s_addr);
		prefix = nm_utils_ip4_netmask_to_prefix (netmask.s_addr);
		g_array_append_val (array, prefix);
		g_array_append_val (array, gateway.s_addr);
		g_array_append_val (array, metric);
		g_ptr_array_add (routes, array);
	}

	if (routes->len > 0) {
		value = g_new0 (GValue, 1);
		g_value_init (value, DBUS_TYPE_G_ARRAY_OF_ARRAY_OF_UINT);
		g_value_take_boxed (value, routes);
	} else
		g_ptr_array_free (routes, TRUE);

	return value;
}

static GValue *
get_ip6_routes (void)
{
	GValue *value = NULL;
	GSList *routes;
	char *tmp;
	int i;

	routes = NULL;

	for (i = 1; i < 256; i++) {
		NMIP6Route *route;
		char buf[BUFLEN];
		struct in6_addr network, gateway;
		guint32 prefix;
		gchar **dest_prefix;

		snprintf (buf, BUFLEN, "route_ipv6_network_%d", i);
		tmp = getenv (buf);
		if (!tmp || !tmp[0])
			break;

		/* Split network string in "dest/prefix" format */
		dest_prefix = g_strsplit (tmp, "/", 2);

		tmp = dest_prefix[0];
		if (inet_pton (AF_INET6, tmp, &network) <= 0) {
			g_warning ("Ignoring invalid static route address '%s'", tmp ? tmp : "NULL");
			g_strfreev (dest_prefix);
			continue;
		}

		tmp = dest_prefix[1];
		if (tmp) {
			long int tmp_prefix;

			errno = 0;
			tmp_prefix = strtol (tmp, NULL, 10);
			if (errno || tmp_prefix <= 0 || tmp_prefix > 128) {
				g_warning ("Ignoring invalid static route prefix '%s'", tmp ? tmp : "NULL");
				g_strfreev (dest_prefix);
				continue;
			}
			prefix = (guint32) tmp_prefix;
		} else {
			g_warning ("Ignoring static route %d with no prefix length", i);
			g_strfreev (dest_prefix);
			continue;
		}
		g_strfreev (dest_prefix);

		snprintf (buf, BUFLEN, "route_ipv6_gateway_%d", i);
		tmp = getenv (buf);
		/* gateway can be missing */
		if (tmp && (inet_pton (AF_INET6, tmp, &gateway) <= 0)) {
			g_warning ("Ignoring invalid static route gateway '%s'", tmp ? tmp : "NULL");
			continue;
		}

		route = nm_ip6_route_new ();
		nm_ip6_route_set_dest (route, &network);
		nm_ip6_route_set_prefix (route, prefix);
		nm_ip6_route_set_next_hop (route, &gateway);

		routes = g_slist_append (routes, route);
	}

	if (routes) {
		GSList *iter;

		value = g_slice_new0 (GValue);
		g_value_init (value, DBUS_TYPE_G_ARRAY_OF_IP6_ROUTE);
		nm_utils_ip6_routes_to_gvalue (routes, value);

		for (iter = routes; iter; iter = iter->next)
			nm_ip6_route_unref (iter->data);
		g_slist_free (routes);
	}

	return value;
}

static GValue *
trusted_remote_to_gvalue (GError **error)
{
	const char *tmp;
	GValue *val = NULL;
	const char *p;
	gboolean is_name = FALSE;

	tmp = getenv ("trusted_ip6");
	if (tmp) {
		val = addr6_to_gvalue (tmp, error);
		if (!val)
			g_prefix_error (error, "%s: failed to convert VPN gateway address (trusted_ip6): ", __func__);
		return val;
	}

	tmp = getenv ("trusted_ip");
	if (!tmp)
		tmp = getenv ("remote_1");
	if (!tmp) {
		g_set_error (error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
		             "%s: did not receive remote gateway address (trusted_ip or remote_1)", __func__);
		return NULL;
	}

	/* Check if it seems to be a hostname */
	p = tmp;
	while (*p) {
		if (*p != '.' && !isdigit (*p)) {
			is_name = TRUE;
			break;
		}
		p++;
	}

	/* Resolve a hostname if required. Only look for IPv4 addresses */
	if (is_name) {
		struct in_addr addr;
		struct addrinfo hints;
		struct addrinfo *result = NULL, *rp;
		int err;

		addr.s_addr = 0;
		memset (&hints, 0, sizeof (hints));

		hints.ai_family = AF_INET;
		hints.ai_flags = AI_ADDRCONFIG;
		err = getaddrinfo (tmp, NULL, &hints, &result);
		if (err != 0) {
			g_set_error (error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
			             "%s: failed to look up VPN gateway address '%s' (%d)",
			             __func__, tmp, err);
			return NULL;
		}

		/* FIXME: so what if the name resolves to multiple IP addresses?  We
		 * don't know which one pptp decided to use so we could end up using a
		 * different one here, and the VPN just won't work.
		 */
		for (rp = result; rp; rp = rp->ai_next) {
			if (   (rp->ai_family == AF_INET)
			    && (rp->ai_addrlen == sizeof (struct sockaddr_in))) {
				struct sockaddr_in *inptr = (struct sockaddr_in *) rp->ai_addr;

				memcpy (&addr, &(inptr->sin_addr), sizeof (struct in_addr));
				break;
			}
		}

		freeaddrinfo (result);
		if (addr.s_addr != 0)
			return uint_to_gvalue (addr.s_addr);
		else {
			g_set_error (error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
			             "%s: failed to convert or look up VPN gateway address '%s'",
			             __func__, tmp);
			return NULL;
		}
	} else {
		val = addr4_to_gvalue (tmp, error);
		if (val == NULL) {
			g_prefix_error (error, "%s: failed to convert VPN gateway address: ", __func__);
			return NULL;
		}
	}

	return val;
}

gboolean
helper_generate_config (const char **args,
                        gboolean is_tapdev,
                        gboolean is_restart,
                        GHashTable **out_config,
                        GHashTable **out_ip4_config,
                        GHashTable **out_ip6_config,
                        GError **error)
{
	GHashTable *config = NULL, *ip4config = NULL, *ip6config = NULL;
	const char *tmp;
	GValue *val;
	int i;
	GValue *dns_list = NULL;
	GValue *nbns_list = NULL;
	GPtrArray *dns_domains = NULL;
	struct in_addr temp_addr;

	config = g_hash_table_new (g_str_hash, g_str_equal);
	ip4config = g_hash_table_new (g_str_hash, g_str_equal);
	ip6config = g_hash_table_new (g_str_hash, g_str_equal);

	/* External world-visible VPN gateway */
	val = trusted_remote_to_gvalue (error);
	if (!val)
		return FALSE;
	g_hash_table_insert (config, NM_VPN_PLUGIN_CONFIG_EXT_GATEWAY, val);

	/* Internal VPN subnet gateway */
	tmp = getenv ("route_vpn_gateway");
	val = addr4_to_gvalue (tmp, NULL);
	if (val)
		g_hash_table_insert (ip4config, NM_VPN_PLUGIN_IP4_CONFIG_INT_GATEWAY, val);
	else {
		val = addr6_to_gvalue (tmp, NULL);
		if (val)
			g_hash_table_insert (ip6config, NM_VPN_PLUGIN_IP6_CONFIG_INT_GATEWAY, val);
	}

	/* VPN device */
	tmp = getenv ("dev");
	val = str_to_gvalue (tmp, FALSE, error);
	if (!val) {
		g_prefix_error (error, "%s: failed to parse 'dev': ", __func__);
		return FALSE;
	}
	g_hash_table_insert (config, NM_VPN_PLUGIN_CONFIG_TUNDEV, val);

	/* IPv4 address */
	tmp = getenv ("ifconfig_local");
	if (!tmp && is_restart)
		tmp = args[4];
	if (tmp && tmp[0]) {
		val = addr4_to_gvalue (tmp, error);
		if (!val) {
			g_prefix_error (error, "%s: failed to parse 'ifconfig_local': ", __func__);
			return FALSE;
		}
		g_hash_table_insert (ip4config, NM_VPN_PLUGIN_IP4_CONFIG_ADDRESS, val);
	}

	/* PTP address; for vpnc PTP address == internal IP4 address */
	tmp = getenv ("ifconfig_remote");
	if (!tmp && is_restart)
		tmp = args[5];
	val = addr4_to_gvalue (tmp, NULL);
	if (val) {
		/* Sigh.  Openvpn added 'topology' stuff in 2.1 that changes the meaning
		 * of the ifconfig bits without actually telling you what they are
		 * supposed to mean; basically relying on specific 'ifconfig' behavior.
		 */
		if (tmp && !strncmp (tmp, "255.", 4)) {
			guint32 addr;

			/* probably a netmask, not a PTP address; topology == subnet */
			addr = g_value_get_uint (val);
			g_value_set_uint (val, nm_utils_ip4_netmask_to_prefix (addr));
			g_hash_table_insert (ip4config, NM_VPN_PLUGIN_IP4_CONFIG_PREFIX, val);
		} else
			g_hash_table_insert (ip4config, NM_VPN_PLUGIN_IP4_CONFIG_PTP, val);
	}

	/* Netmask
	 *
	 * Either TAP or TUN modes can have an arbitrary netmask in newer versions
	 * of openvpn, while in older versions only TAP mode would.  So accept a
	 * netmask if passed, otherwise default to /32 for TUN devices since they
	 * are usually point-to-point.
	 */
	tmp = getenv ("ifconfig_netmask");
	if (tmp && inet_pton (AF_INET, tmp, &temp_addr) > 0) {
		val = g_slice_new0 (GValue);
		g_value_init (val, G_TYPE_UINT);
		g_value_set_uint (val, nm_utils_ip4_netmask_to_prefix (temp_addr.s_addr));
		g_hash_table_insert (ip4config, NM_VPN_PLUGIN_IP4_CONFIG_PREFIX, val);
	} else if (!is_tapdev) {
		if (!g_hash_table_lookup (ip4config, NM_VPN_PLUGIN_IP4_CONFIG_PREFIX)) {
			val = g_slice_new0 (GValue);
			g_value_init (val, G_TYPE_UINT);
			g_value_set_uint (val, 32);
			g_hash_table_insert (ip4config, NM_VPN_PLUGIN_IP4_CONFIG_PREFIX, val);
		}
	} else {
		g_set_error_literal (error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
		                     "missing or invalid netmask/prefix");
		return FALSE;
	}

	val = get_ip4_routes ();
	if (val)
		g_hash_table_insert (ip4config, NM_VPN_PLUGIN_IP4_CONFIG_ROUTES, val);

	/* IPv6 address */
	tmp = getenv ("ifconfig_ipv6_local");
	if (tmp && tmp[0]) {
		val = addr6_to_gvalue (tmp, error);
		if (!val) {
			g_prefix_error (error, "%s: failed to parse 'ifconfig_ipv6_local': ", __func__);
			return FALSE;
		}
		g_hash_table_insert (ip6config, NM_VPN_PLUGIN_IP6_CONFIG_ADDRESS, val);
	}

	/* IPv6 remote address */
	tmp = getenv ("ifconfig_ipv6_remote");
	if (tmp && tmp[0]) {
		val = addr6_to_gvalue (tmp, error);
		if (!val) {
			g_prefix_error (error, "%s: failed to parse 'ifconfig_ipv6_remote': ", __func__);
			return FALSE;
		}
		g_hash_table_insert (ip6config, NM_VPN_PLUGIN_IP6_CONFIG_PTP, val);
	}

	/* IPv6 netbits */
	tmp = getenv ("ifconfig_ipv6_netbits");
	if (tmp && tmp[0]) {
		long int netbits;

		errno = 0;
		netbits = strtol (tmp, NULL, 10);
		if (errno || netbits < 0 || netbits > 128) {
			g_warning ("Ignoring invalid prefix '%s'", tmp);
		} else {
			val = uint_to_gvalue ((guint32) netbits);
			g_hash_table_insert (ip6config, NM_VPN_PLUGIN_IP6_CONFIG_PREFIX, val);
		}
	}

	val = get_ip6_routes ();
	if (val)
		g_hash_table_insert (ip6config, NM_VPN_PLUGIN_IP6_CONFIG_ROUTES, val);

	/* DNS and WINS servers */
	dns_domains = g_ptr_array_sized_new (3);
	for (i = 1; i < 256; i++) {
		char buf[50];

		snprintf (buf, sizeof (buf), "foreign_option_%d", i);
		tmp = getenv (buf);

		if (!tmp || !tmp[0])
			break;

		if (!g_str_has_prefix (tmp, "dhcp-option "))
			continue;

		tmp += 12; /* strlen ("dhcp-option ") */

		if (g_str_has_prefix (tmp, "DNS "))
			dns_list = parse_addr4_list (dns_list, tmp + 4);
		else if (g_str_has_prefix (tmp, "WINS "))
			nbns_list = parse_addr4_list (nbns_list, tmp + 5);
		else if (g_str_has_prefix (tmp, "DOMAIN ") && is_domain_valid (tmp + 7))
			g_ptr_array_add (dns_domains, (gpointer) tmp + 7);
	}

	if (dns_list)
		g_hash_table_insert (ip4config, NM_VPN_PLUGIN_IP4_CONFIG_DNS, dns_list);
	if (nbns_list)
		g_hash_table_insert (ip4config, NM_VPN_PLUGIN_IP4_CONFIG_NBNS, nbns_list);
	if (dns_domains->len) {
		val = g_slice_new0 (GValue);
		g_value_init (val, DBUS_TYPE_G_PTR_ARRAY_OF_STRING);
		g_value_take_boxed (val, dns_domains);
		g_hash_table_insert (ip4config, NM_VPN_PLUGIN_IP4_CONFIG_DOMAINS, val);
	} else
		g_ptr_array_free (dns_domains, TRUE);

	/* Tunnel MTU */
	tmp = getenv ("tun_mtu");
	if (tmp && tmp[0]) {
		long int mtu;

		errno = 0;
		mtu = strtol (tmp, NULL, 10);
		if (errno || mtu < 0 || mtu > 20000) {
			g_warning ("Ignoring invalid tunnel MTU '%s'", tmp);
		} else {
			val = uint_to_gvalue ((guint32) mtu);
			g_hash_table_insert (config, NM_VPN_PLUGIN_CONFIG_MTU, val);
		}
	}

	if (g_hash_table_size (ip4config)) {
		g_hash_table_insert (config, NM_VPN_PLUGIN_CONFIG_HAS_IP4, bool_to_gvalue (TRUE));
		if (out_ip4_config)
			*out_ip4_config = g_hash_table_ref (ip4config);
	}
	g_hash_table_unref (ip4config);

	if (g_hash_table_size (ip6config)) {
		g_hash_table_insert (config, NM_VPN_PLUGIN_CONFIG_HAS_IP6, bool_to_gvalue (TRUE));
		if (out_ip6_config)
			*out_ip6_config = g_hash_table_ref (ip6config);
	}
	g_hash_table_unref (ip6config);

	if (out_config)
		*out_config = g_hash_table_ref (config);
	g_hash_table_unref (config);
	return TRUE;
}
