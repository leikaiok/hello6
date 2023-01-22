/* //device/system/reference-ril/sim.h
**
** Copyright 2022, GOSUNCN GROUP RIL
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
#ifndef SIM_H
#define SIM_H

#define SIM_CHECK_MAX_RETRY    (5)
#define SIM_CHECK_TIMEVAL      (2)

typedef enum
{
    SIM_STATUS_NONE         = 0,
    SIM_STATUS_READY        = 1,
    SIM_STATUS_NOT_INSERTED = 2,
    SIM_STATUS_FAILURE      = 3,
    SIM_STATUS_PIN_REQUIRED = 4,
    SIM_STATUS_PUK_REQUIRED = 5
} SIM_STATUS_TYPE;

/* Get SIM status */
SIM_STATUS_TYPE getSimStatus();
void requestUnsolicitedSimStatusChanged(void);

#endif