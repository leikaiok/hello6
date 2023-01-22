/* //device/system/reference-ril/misc.c
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
#include <sys/system_properties.h>
#include <telephony/ril.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <cutils/properties.h>
#include <unistd.h>
#include <utils/Log.h>

#include "misc.h"
/* BEGIN Add property set function for RIL by yangyao in 20220309 */
#define PROPERTY_SET_TRY_TIMES    (3)

#ifndef PROPERTY_VALUE_MAX
#define PROPERTY_VALUE_MAX        (92)
#endif
/* END Add property set function for RIL by yangyao in 20220309 */

/** returns 1 if line starts with prefix, 0 if it does not */
int strStartsWith(const char *line, const char *prefix)
{
    for ( ; *line != '\0' && *prefix != '\0' ; line++, prefix++) {
        if (*line != *prefix) {
            return 0;
        }
    }

    return *prefix == '\0';
}

/* BEGIN Add property set function for RIL by yangyao in 20220309 */
int ril_property_set(const char *name, const char *value)
{
    int ret = 0;
    int try_times = 0;

    for(try_times = 0; try_times < PROPERTY_SET_TRY_TIMES; try_times++)
    {
        if((ret = property_set(name, value)) < 0)
        {
            RLOGD("ril_property_set: set %s property failure, try_times = %d, ret = %d", name, try_times, ret);
            usleep(300000);
        }
        else
        {
            RLOGD("ril_property_set: set %s property successful, value = %s", name, value);
            break;
        }
    }

    return ret;
}

int ril_property_get(const char *name, char *value, const char *default_value, int max_len)
{
    char temp_buf[PROPERTY_VALUE_MAX] = {0};
    int ret = 0;

    ret = property_get(name, temp_buf, default_value);
    memcpy(value, temp_buf, ((max_len > PROPERTY_VALUE_MAX) ? PROPERTY_VALUE_MAX : max_len));

    return ret;
}
/* END Add property set function for RIL by yangyao in 20220309 */

// Returns true iff running this process in an emulator VM
bool isInEmulator(void) {
  static int inQemu = -1;
  if (inQemu < 0) {
      char propValue[PROP_VALUE_MAX];
      inQemu = (__system_property_get("ro.kernel.qemu", propValue) != 0);
  }
  return inQemu == 1;
}
