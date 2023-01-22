/* //device/system/reference-ril/network.c
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
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#include "network.h"
#include "ril_common.h"
#include <telephony/librilutils.h>

#define LOG_TAG "RIL"
#include <utils/Log.h>

#define UNSOL_SIG_INTERVAL    (5)
static const struct timeval RIL_UNSOL_SIGNAL_STRENTH_TIMER = {UNSOL_SIG_INTERVAL, 0};

/* BEGIN Modify get cell info request by yangyao in 20220316 */
/* Convert RSSI to CSQ value */
int convertRssiToCsq(int rssi)
{
    if(rssi > 0 || rssi < -120) return 99;
    else if(rssi <= -113) return 0;
    else if(rssi >= -51) return 31;
    else return (rssi + 113) / 2;
}
/* END Modify get cell info request by yangyao in 20220316 */

/* Convert Android Preferred Network Type to RIL for GOSUCN AT value */
int convertPreferredNetworkTypeToRil(int value)
{
    int current = 0;

    switch(value)
    {
        case PREF_NET_TYPE_LTE_GSM_WCDMA:
        case PREF_NET_TYPE_LTE_CMDA_EVDO_GSM_WCDMA:
        case PREF_NET_TYPE_TD_SCDMA_GSM_LTE:
            current = 0;    /* Auto */
            break;

        case PREF_NET_TYPE_GSM_ONLY:
            current = 1;    /* GSM only */
            break;

        case PREF_NET_TYPE_WCDMA:
            current = 2;    /* WCDMA only */
            break;

        case PREF_NET_TYPE_TD_SCDMA_ONLY:
            current = 3;    /* TDSCDMA only */
            break;

        case PREF_NET_TYPE_CDMA_ONLY:
            current = 4;    /* CDMA only */
            break;

        case PREF_NET_TYPE_EVDO_ONLY:
            current = 5;    /* HDR only */
            break;

        case PREF_NET_TYPE_LTE_ONLY:
            current = 6;    /* LTE only */
            break;

        case PREF_NET_TYPE_LTE_WCDMA:
            current = 7;    /* WCDMA and LTE only */
            break;

        case PREF_NET_TYPE_TD_SCDMA_GSM_WCDMA_LTE:
            current = 8;    /* TDSCDMA GSM WCDMA and LTE only */
            break;

        case PREF_NET_TYPE_CDMA_EVDO_AUTO:
            current = 9;    /* CDMA GSM HDR only */
            break;

        case PREF_NET_TYPE_TD_SCDMA_WCDMA_LTE:
            current = 10;   /* TDSCDMA WCDMA LTE only */
            break;

        default:
            RLOGD("convertPreferredNetworkType: This value RAT is not support in RIL, set default auto mode.");
            current = 0;    /* Auto */
            break;
    }

    return current;
}

/* Convert RIL for GOSUCN AT network preferred value to Android Preferred Network Type */
int convertRilToPreferredNetworkType(int rat)
{
    int response = 0;

    switch(rat)
    {
        /* Auto mode */
        case 0:
            response = PREF_NET_TYPE_LTE_CMDA_EVDO_GSM_WCDMA;
            break;

        /* GSM only */
        case 1:
            response = PREF_NET_TYPE_GSM_ONLY;
            break;

        /* WCDMA only */
        case 2:
            response = PREF_NET_TYPE_WCDMA;
            break;

        /* TDSCDMA only */
        case 3:
            response = PREF_NET_TYPE_TD_SCDMA_ONLY;
            break;

        /* CDMA only */
        case 4:
            response = PREF_NET_TYPE_TD_SCDMA_ONLY;
            break;

        /* HDR only */
        case 5:
            response = PREF_NET_TYPE_EVDO_ONLY;
            break;

        /* LTE only */
        case 6:
            response = PREF_NET_TYPE_LTE_ONLY;
            break;

        /* WCDMA and LTE only */
        case 7:
            response = PREF_NET_TYPE_LTE_WCDMA;
            break;

        /* TDSCDMA GSM WCDMA and LTE only */
        case 8:
            response = PREF_NET_TYPE_TD_SCDMA_GSM_WCDMA_LTE;
            break;

        /* CDMA GSM HDR only */
        case 9:
            response = PREF_NET_TYPE_CDMA_EVDO_AUTO;
            break;

        /* TDSCDMA WCDMA LTE only */
        case 10:
            response = PREF_NET_TYPE_TD_SCDMA_WCDMA_LTE;
            break;

        default:
            RLOGD("requestGetPreferredNetworkType: set default preferred mode = %d", response);
            response = PREF_NET_TYPE_LTE_CMDA_EVDO_GSM_WCDMA;
            break;
    }

    return response;
}

/* Convert RIL for GOSUCN AT radio access tech to Android Radio Access Tech */
int convertRilToRadioAccessTech(char* radio_tech)
{
    int android_radio_tech = 0;

    if(strncmp(radio_tech, "LTE", strlen("LTE")) == 0)
    {
        android_radio_tech = RADIO_TECH_LTE;
    }
    else if(strncmp(radio_tech, "HSPA+", strlen("HSPA+")) == 0)
    {
        android_radio_tech = RADIO_TECH_HSPAP;
    }
    else if(strncmp(radio_tech, "HSPA", strlen("HSPA")) == 0)
    {
        android_radio_tech = RADIO_TECH_HSPA;
    }
    else if(strncmp(radio_tech, "WCDMA", strlen("WCDMA")) == 0)
    {
        android_radio_tech = RADIO_TECH_UMTS;
    }
    else if(strncmp(radio_tech, "GSM", strlen("GSM")) == 0)
    {
        android_radio_tech = RADIO_TECH_GSM;
    }
    else if(strncmp(radio_tech, "GPRS", strlen("GPRS")) == 0)
    {
        android_radio_tech = RADIO_TECH_GPRS;
    }

    return android_radio_tech;
}

/* Get Cell information */
int getCellInformation(RIL_CellInfo_v12* ci)
{
    /* BEGIN Modify get cell info request by yangyao in 20220316 */
    ATResponse *p_zpas_response      = NULL;
    ATResponse *p_zcellinfo_response = NULL;
    ATResponse *p_zcsq_response      = NULL;
    int err    = 0;
    int commas = 0;
    char *line_zpas      = NULL;
    char *line_zcsq      = NULL;
    char *line_zcellinfo = NULL;
    char *radio_tech     = NULL;
    int cell_id = 0;
    int plmn    = 0;
    int tac     = 0;
    int lac     = 0;
    int sid     = 0;
    int nid     = 0;
    int rssi    = 0;
    int ecio    = 0;
    int rsrp    = 0;
    int rsrq    = 0;
    int sinr    = 0;

    /* exec AT+ZPAS? */
    err = at_send_command_singleline("AT+ZPAS?", "+ZPAS:", &p_zpas_response);
    if(err != 0 || p_zpas_response->success == 0 || p_zpas_response->p_intermediates == NULL) {
        RLOGD("exec AT+ZPAS? failure.");
        goto error;
    } else {
        line_zpas = p_zpas_response->p_intermediates->line;
        RLOGD("exec AT+ZPAS? successful and get response = %s.", line_zpas);
    }

    /* exec AT+ZCELLINFO? */
    err = at_send_command_singleline("AT+ZCELLINFO?", "+ZCELLINFO:", &p_zcellinfo_response);
    if(err != 0 || p_zcellinfo_response->success == 0 || p_zcellinfo_response->p_intermediates == NULL) {
        RLOGD("exec AT+ZCELLINFO? failure.");
        goto error;
    } else {
        line_zcellinfo = p_zcellinfo_response->p_intermediates->line;
        RLOGD("exec AT+ZCELLINFO? successful and get response = %s.", line_zcellinfo);
    }

    /* exec AT+ZCSQ? */
    err = at_send_command_singleline("AT+ZCSQ?", "+ZCSQ:", &p_zcsq_response);
    if(err != 0 || p_zcsq_response->success == 0 || p_zcsq_response->p_intermediates == NULL) {
        RLOGD("exec AT+ZCSQ? failure.");
        goto error;
    } else {
        line_zcsq = p_zcsq_response->p_intermediates->line;
        RLOGD("exec AT+ZCSQ? successful and get response = %s.", line_zcsq);
    }

    err = at_tok_start(&line_zpas);
    if(err < 0) {
        goto error;
    }

    err = at_tok_nextstr(&line_zpas, &radio_tech);
    if(err < 0) {
        goto error;
    }

    RLOGD("parse radio_tech = %s.", radio_tech);
    if(strncmp(radio_tech, "LTE", strlen("LTE")) == 0) {
        err = at_tok_start(&line_zcellinfo);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nexthexint(&line_zcellinfo, &cell_id);
        RLOGD("getCellInformation: cell_id = %d", cell_id);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nextint(&line_zcellinfo, &plmn);
        RLOGD("getCellInformation: plmn = %d", plmn);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nexthexint(&line_zcellinfo, &tac);
        RLOGD("getCellInformation: tac = %d", tac);
        if(err < 0) {
            goto error;
        }

        err = at_tok_start(&line_zcsq);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nextstr(&line_zcsq, &radio_tech);
        if(err < 0) {
            goto error;
        }
        RLOGD("radio_tech:%s", radio_tech);

        err = at_tok_nextint(&line_zcsq, &rsrp);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nextint(&line_zcsq, &rsrq);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nextint(&line_zcsq, &sinr);
        if(err < 0) {
            goto error;
        }

        ci->cellInfoType  = RIL_CELL_INFO_TYPE_LTE;
        ci->registered    = 1;
        ci->timeStampType = RIL_TIMESTAMP_TYPE_OEM_RIL;
        ci->timeStamp     = ril_nano_time();
        ci->CellInfo.lte.cellIdentityLte.ci    = cell_id;
        ci->CellInfo.lte.cellIdentityLte.mcc   = plmn / 100;
        ci->CellInfo.lte.cellIdentityLte.mnc   = plmn % 100;
        ci->CellInfo.lte.cellIdentityLte.tac   = tac;

        ci->CellInfo.lte.signalStrengthLte.rsrp  = rsrp * (-1);
        ci->CellInfo.lte.signalStrengthLte.rsrq  = rsrq * (-1);
        ci->CellInfo.lte.signalStrengthLte.rssnr = sinr * (10);
        ci->CellInfo.lte.signalStrengthLte.cqi   = 0x7FFFFFFF;
        ci->CellInfo.lte.signalStrengthLte.timingAdvance = 0x7FFFFFFF;
    }
    else if((strncmp(radio_tech, "HSPA+", strlen("HSPA+")) == 0) ||
            (strncmp(radio_tech, "HSPA", strlen("HSPA")) == 0) ||
            (strncmp(radio_tech, "WCDMA", strlen("WCDMA")) == 0)) {
        err = at_tok_start(&line_zcellinfo);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nexthexint(&line_zcellinfo, &cell_id);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nextint(&line_zcellinfo, &plmn);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nexthexint(&line_zcellinfo, &lac);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nextint(&line_zcsq, &rssi);
        if(err < 0) {
            goto error;
        }

        ci->cellInfoType = RIL_CELL_INFO_TYPE_WCDMA;
        ci->registered    = 1;
        ci->timeStampType = RIL_TIMESTAMP_TYPE_JAVA_RIL;
        ci->timeStamp     = 0;
        ci->CellInfo.wcdma.cellIdentityWcdma.cid = cell_id;
        ci->CellInfo.wcdma.cellIdentityWcdma.mcc = plmn / 100;
        ci->CellInfo.wcdma.cellIdentityWcdma.mnc = plmn % 100;
        ci->CellInfo.wcdma.cellIdentityWcdma.lac = lac;
        ci->CellInfo.wcdma.cellIdentityWcdma.psc = 0xFFFFFFFF;
        ci->CellInfo.wcdma.signalStrengthWcdma.signalStrength = convertRssiToCsq(rssi);
    }
    else if((strncmp(radio_tech, "GSM", strlen("GSM")) == 0) ||
            (strncmp(radio_tech, "GPRS", strlen("GPRS")) == 0)) {
        err = at_tok_start(&line_zcellinfo);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nexthexint(&line_zcellinfo, &cell_id);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nextint(&line_zcellinfo, &plmn);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nexthexint(&line_zcellinfo, &lac);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nextint(&line_zcsq, &rssi);
        if(err < 0) {
            goto error;
        }

        ci->cellInfoType = RIL_CELL_INFO_TYPE_GSM;
        ci->registered    = 1;
        ci->timeStampType = RIL_TIMESTAMP_TYPE_JAVA_RIL;
        ci->timeStamp     = 0;
        ci->CellInfo.gsm.cellIdentityGsm.cid = cell_id;
        ci->CellInfo.gsm.cellIdentityGsm.mcc = plmn / 100;
        ci->CellInfo.gsm.cellIdentityGsm.mnc = plmn % 100;
        ci->CellInfo.gsm.cellIdentityGsm.lac = lac;
        ci->CellInfo.gsm.signalStrengthGsm.signalStrength = convertRssiToCsq(rssi);
    }
    else if((strncmp(radio_tech, "EVDO", strlen("EVDO")) == 0) ||
            (strncmp(radio_tech, "CDMA", strlen("CDMA")) == 0)) {
        err = at_tok_start(&line_zcellinfo);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nexthexint(&line_zcellinfo, &cell_id);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nextint(&line_zcellinfo, &plmn);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nextint(&line_zcellinfo, &sid);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nextint(&line_zcellinfo, &nid);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nextint(&line_zcsq, &rssi);
        if(err < 0) {
            goto error;
        }

        err = at_tok_nextint(&line_zcsq, &ecio);
        if(err < 0) {
            goto error;
        }

        ci->cellInfoType = RIL_CELL_INFO_TYPE_CDMA;
        ci->registered    = 1;
        ci->timeStampType = RIL_TIMESTAMP_TYPE_JAVA_RIL;
        ci->timeStamp     = 0;
        ci->CellInfo.cdma.cellIdentityCdma.networkId = nid;
        ci->CellInfo.cdma.cellIdentityCdma.systemId  = sid;
        ci->CellInfo.cdma.signalStrengthCdma.dbm     = rssi;
        ci->CellInfo.cdma.signalStrengthCdma.ecio    = ecio;
        ci->CellInfo.cdma.signalStrengthEvdo.dbm     = rssi;
        ci->CellInfo.cdma.signalStrengthEvdo.ecio    = ecio;
    }
    else if(strncmp(radio_tech, "TDSCDMA", strlen("TDSCDMA")) == 0) {
        ci->cellInfoType = RIL_CELL_INFO_TYPE_TD_SCDMA;
        ci->registered    = 1;
        ci->timeStampType = RIL_TIMESTAMP_TYPE_JAVA_RIL;
        ci->timeStamp     = 0;
    }
    else {
        ci->cellInfoType = RIL_CELL_INFO_TYPE_NONE;
        RLOGD("getCellInformation: No service or limited, do not report cell info.");
        goto error;
    }

    at_response_free(p_zpas_response);
    at_response_free(p_zcellinfo_response);
    at_response_free(p_zcsq_response);
    return TRUE;

error:
    at_response_free(p_zpas_response);
    at_response_free(p_zcellinfo_response);
    at_response_free(p_zcsq_response);
    return FALSE;
    /* END Modify get cell info request by yangyao in 20220316 */
}

int getSignalStrength(RIL_SignalStrength_v10* signalStrength)
{
    ATResponse *p_csq_response  = NULL;
    ATResponse *p_zcsq_response = NULL;
    int err = 0;
    char *line = NULL;
    char *tmp  = NULL;
    int csq_response[2] = {0};
    int zcsq_response[4] = {0};

    /* initial signalStrength initial value */
    signalStrength->GW_SignalStrength.signalStrength = -1;
    signalStrength->GW_SignalStrength.bitErrorRate   = -1;
    signalStrength->CDMA_SignalStrength.dbm          = -1;
    signalStrength->CDMA_SignalStrength.ecio         = -1;
    signalStrength->EVDO_SignalStrength.dbm          = -1;
    signalStrength->EVDO_SignalStrength.ecio         = -1;
    signalStrength->EVDO_SignalStrength.signalNoiseRatio = -1;
    signalStrength->LTE_SignalStrength.signalStrength = 99;
    signalStrength->LTE_SignalStrength.rsrp           = 0x7FFFFFFF;
    signalStrength->LTE_SignalStrength.rsrq           = 0x7FFFFFFF;
    signalStrength->LTE_SignalStrength.rssnr          = 0x7FFFFFFF;
    signalStrength->LTE_SignalStrength.cqi            = 0x7FFFFFFF;

    err = at_send_command_singleline("AT+CSQ", "+CSQ:", &p_csq_response);

    if (err < 0 || p_csq_response->success == 0 || p_csq_response->p_intermediates == NULL) {
        goto error;
    }

    line = p_csq_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;
    RLOGI("line:%s", line);

    err = at_tok_nextint(&line, &(csq_response[0]));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(csq_response[1]));
    if (err < 0) goto error;

    err = at_send_command_singleline("AT+ZCSQ?", "+ZCSQ:", &p_zcsq_response);

    if (err < 0 || p_zcsq_response->success == 0 || p_zcsq_response->p_intermediates == NULL) {
        goto error;
    }

    line = p_zcsq_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;
    RLOGI("line:%s", line);

    err = at_tok_nextstr(&line, &tmp);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(zcsq_response[0]));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(zcsq_response[1]));
    if (err < 0) goto error;

    if (strncmp(tmp, "LTE", strlen("LTE")) == 0) {
        err = at_tok_nextint(&line, &(zcsq_response[2]));
        if (err < 0) goto error;

        signalStrength->LTE_SignalStrength.signalStrength = csq_response[0];
        signalStrength->LTE_SignalStrength.rsrp  = zcsq_response[0] * (-1);
        signalStrength->LTE_SignalStrength.rsrq  = zcsq_response[1] * (-1);
        signalStrength->LTE_SignalStrength.rssnr = zcsq_response[2] * 10;
    } else if (strncmp(tmp, "CDMA", strlen("CDMA")) == 0) {
        signalStrength->CDMA_SignalStrength.dbm = zcsq_response[0] * (-1);
        signalStrength->CDMA_SignalStrength.ecio = zcsq_response[1] * 10 * (-1);
    } else if (strncmp(tmp, "GSM", strlen("GSM")) == 0) {
        signalStrength->GW_SignalStrength.signalStrength = csq_response[0];
        signalStrength->GW_SignalStrength.bitErrorRate = csq_response[1];
    } else if (strncmp(tmp, "WCDMA", strlen("WCDMA")) == 0){
        signalStrength->GW_SignalStrength.signalStrength = csq_response[0];
        signalStrength->GW_SignalStrength.bitErrorRate = csq_response[1];
    }

    at_response_free(p_csq_response);
    at_response_free(p_zcsq_response);
    return TRUE;

error:
    at_response_free(p_csq_response);
    at_response_free(p_zcsq_response);
    return FALSE;
}

/* 0==unregistered, 1==registered; FORMAT_3GPP(1) vs FORMAT_3GPP2(2); */
int getimsstate(int *reg, int *format)
{
    RLOGD("%s", __func__);
    ATResponse *p_ims_response = NULL;
    char *line = NULL;
    int imsreg;

    *reg = 0;
    int err = at_send_command_singleline("AT+ZIMSREGSTATE?", "+IMSREGSTATE:", &p_ims_response);
    if (err < 0 || p_ims_response->success == 0 || p_ims_response->p_intermediates == NULL) {
        goto error;
    }
    line = p_ims_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &imsreg);
    if (err < 0) goto error;

    *reg = imsreg;
    *format = 1;
    return 0;

error:
    return -1;
}

void cycleUnsolicitedSignalStrength(void *param __unused)
{
    RLOGD("%s", __func__);
    RIL_SignalStrength_v10 signalStrength;

    if (getSignalStrength(&signalStrength) == TRUE) {
        RIL_onUnsolicitedResponse(RIL_UNSOL_SIGNAL_STRENGTH, &signalStrength, sizeof(RIL_SignalStrength_v10));
    }

    RIL_requestTimedCallback(cycleUnsolicitedSignalStrength, NULL, &RIL_UNSOL_SIGNAL_STRENTH_TIMER);
}

void requestUnsolicitedSignalStrength(void)
{
    RIL_requestTimedCallback(cycleUnsolicitedSignalStrength, NULL, NULL);
}

