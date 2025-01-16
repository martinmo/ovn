/* Copyright (c) 2024, Red Hat, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OVN_NAT_ADDRESSES_H
#define OVN_NAT_ADDRESSES_H

#include "openvswitch/shash.h"
#include "openvswitch/vlog.h"

/* Structure to hold NAT addresses data */
struct ed_type_nat_addresses {
    struct shash nat_addresses;
    struct sset localnet_vifs;
    struct sset local_l3gw_ports;
    struct sset nat_ip_keys;
};

/* Function prototypes */
bool extract_addresses_with_port(const char *addresses,
                                 struct lport_addresses *laddrs, char **lport);
void get_localnet_vifs_l3gwports(
    struct ovsdb_idl_index *sbrec_port_binding_by_datapath,
    struct ovsdb_idl_index *sbrec_port_binding_by_name,
    const struct ovsrec_bridge *br_int,
    const struct sbrec_chassis *chassis,
    const struct hmap *local_datapaths,
    struct sset *localnet_vifs,
    struct sset *local_l3gw_ports);
void get_nat_addresses_and_keys(
    struct ovsdb_idl_index *sbrec_port_binding_by_name,
    struct sset *nat_address_keys,
    struct sset *local_l3gw_ports,
    const struct sbrec_chassis *chassis,
    const struct sset *active_tunnels,
    struct shash *nat_addresses);

#endif /* NAT_ADDRESSES_H */