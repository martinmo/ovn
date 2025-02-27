/* Copyright (c) 2015, 2016 Nicira, Inc.
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


#ifndef OVN_CONTROLLER_H
#define OVN_CONTROLLER_H 1

#include <stdint.h>

#define IDL_LOOP_MAX_DURATION_MS 500

struct ovsrec_bridge_table;

const struct ovsrec_bridge *get_bridge(const struct ovsrec_bridge_table *,
                                       const char *br_name);

uint32_t get_tunnel_type(const char *name);

#endif /* controller/ovn-controller.h */
