#ifndef EN_NORTHD_OUTPUT_H
#define EN_NORTHD_OUTPUT_H 1

#include "lib/inc-proc-eng.h"

void *en_northd_output_init(struct engine_node *node OVS_UNUSED,
                            struct engine_arg *arg OVS_UNUSED);
void en_northd_output_run(struct engine_node *node OVS_UNUSED,
                          void *data OVS_UNUSED);

void en_northd_output_cleanup(void *data);
bool northd_output_sync_to_sb_handler(struct engine_node *node,
                                      void *data OVS_UNUSED);
bool northd_output_lflow_handler(struct engine_node *node,
                                 void *data OVS_UNUSED);
bool northd_output_mac_binding_aging_handler(struct engine_node *node,
                                             void *data OVS_UNUSED);
bool northd_output_fdb_aging_handler(struct engine_node *node,
                                     void *data OVS_UNUSED);
bool northd_output_ecmp_nexthop_handler(struct engine_node *node,
                                        void *data OVS_UNUSED);
bool northd_output_acl_id_handler(struct engine_node *node,
                                  void *data OVS_UNUSED);
bool northd_output_advertised_route_sync_handler(struct engine_node *node,
                                                 void *data OVS_UNUSED);

#endif
