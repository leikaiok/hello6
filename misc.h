/* //device/system/reference-ril/misc.h
**
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/
#include <stdbool.h>

/** returns 1 if line starts with prefix, 0 if it does not */
int strStartsWith(const char *line, const char *prefix);

/* BEGIN Add property set function for RIL by yangyao in 20220309 */
/** set android property **/
int ril_property_set(const char *name, const char *value);

/** get android property **/
int ril_property_get(const char *name, char *value, const char *default_value, int max_len);
/* END Add property set function for RIL by yangyao in 20220309 */

/** Returns true iff running this process in an emulator VM */
bool isInEmulator(void);