/* //device/system/reference-ril/network.h
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
#ifndef NETWORK_H
#define NETWORK_H

#include "ril.h"

/* Convert RSSI to CSQ value */
int convertRssiToCsq(int rssi);

/* Convert Android Preferred Network Type to RIL for GOSUCN AT network preferred value */
int convertPreferredNetworkTypeToRil(int value);

/* Convert RIL for GOSUCN AT network preferred value to Android Preferred Network Type */
int convertRilToPreferredNetworkType(int rat);

/* Convert RIL for GOSUCN AT radio access tech to Android Radio Access Tech */
int convertRilToRadioAccessTech(char* radio_tech);

/* Get Cell information */
int getCellInformation(RIL_CellInfo_v12* ci);

/* Get signal strength */
int getSignalStrength(RIL_SignalStrength_v10* signalStrength);

int getimsstate(int *reg, int *format);

void requestUnsolicitedSignalStrength(void);

#endif