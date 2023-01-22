/* //device/system/reference-ril/sim.c
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
#include <pthread.h>
#include <sys/types.h>
#include <sys/time.h>
#include <telephony/ril.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <cutils/properties.h>
#include <termios.h>
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#include "ril.h"
#include "sim.h"
#include "ril_common.h"

#define LOG_TAG "RIL"
#include <utils/Log.h>

#define UNSOL_SIM_STATUS_INTERVAL    (30)

int g_sim_status = SIM_STATUS_NONE;
static const struct timeval RIL_UNSOL_SIM_STATUS_CHANGED_TIMER = {UNSOL_SIM_STATUS_INTERVAL, 0};
static const struct timeval RIL_REQ_SIM_INFO_TIMER = {2, 0};

static int HexCharToInt(char c)
{
    int value = 0;

    if((c >= 'A') && (c <= 'F')) {
        value = c - 'A' + 10;
    } else if((c >= 'a') || (c <= 'f')) {
        value = c - 'a' + 10;
    } else {
        value = c - '0';
    }

    return value;
}

/* get SIM status */
SIM_STATUS_TYPE getSimStatus(void)
{
    ATResponse *p_response = NULL;
    int sim_status = SIM_STATUS_NONE;
    int err = 0;

    err = at_send_command_singleline("AT+CPIN?", "+CPIN:", &p_response);
    if(p_response == NULL) {
        RLOGE("checkSimStatus: AT+CPIN exec failure and p_response = NULL.");
        sim_status = SIM_STATUS_NONE;
        return sim_status;
    }

    if(err < 0) {
        RLOGE("checkSimStatus: AT+CPIN exec return error, err = %d", err);
        sim_status = SIM_STATUS_NONE;
        return sim_status;
    }

    /* if +CPIN return CME error */
    if((p_response->success == 0) || (p_response->p_intermediates == NULL)) {
        if((strstr(p_response->finalResponse, "CME ERROR: 10") != NULL) || (strstr(p_response->finalResponse, "CME ERROR: SIM not inserted") != NULL)) {
            RLOGE("checkSimStatus: SIM card is not inserted.");
            sim_status = SIM_STATUS_NOT_INSERTED;
        } else if((strstr(p_response->finalResponse, "CME ERROR: 13") != NULL) || (strstr(p_response->finalResponse, "CME ERROR: SIM failure") != NULL)) {
            RLOGE("checkSimStatus: SIM card is failure.");
            sim_status = SIM_STATUS_FAILURE;
        } else {
            RLOGE("checkSimStatus: SIM status is unknown.");
            sim_status = SIM_STATUS_NONE;
        }

        return sim_status;
    }

    /* if +CPIN return +CPIN: */
    if(p_response->p_intermediates != NULL) {
        if(strstr(p_response->p_intermediates->line, "SIM PIN") != NULL) {
            RLOGD("checkSimStatus: SIM PIN required.");
            sim_status = SIM_STATUS_PIN_REQUIRED;
        } else if(strstr(p_response->p_intermediates->line, "SIM PUK") != NULL) {
            RLOGD("checkSimStatus: SIM PUK required.");
            sim_status = SIM_STATUS_PUK_REQUIRED;
        } else if(strstr(p_response->p_intermediates->line, "READY") != NULL) {
            RLOGD("checkSimStatus: SIM is ready.");
            sim_status = SIM_STATUS_READY;
        } else {
            RLOGE("checkSimStatus: SIM status is not prase.");
            sim_status = SIM_STATUS_NONE;
        }

        return sim_status;
    }

    return sim_status;
}

/* get ICCID */
int getIccid(char* tmp, int len)
{
    ATResponse *p_response = NULL;
    int err = 0;
    char *line = NULL;
    char *iccid = NULL;

    bzero(tmp, len);
    err = at_send_command_singleline("AT+ICCID", "ICCID:", &p_response);
    if (err < 0 || p_response->success == 0 || p_response->p_intermediates == NULL)
        goto error;

    line = p_response->p_intermediates->line;
    if (!line)
        goto error;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextstr(&line, &iccid);
    if (err < 0) goto error;

    if (!iccid)
        goto error;

    strcpy(tmp, iccid);
    at_response_free(p_response);
    return 0;

error:
    at_response_free(p_response);
    return -1;
}

/* get IMSI */
int getImsi(char* tmp, int len)
{
    ATResponse *p_response = NULL;
    int err = 0;
    char *line = NULL;

    bzero(tmp, len);
    err = at_send_command_numeric("AT+CIMI", &p_response);
    if (err < 0 || p_response->success == 0 || p_response->p_intermediates == NULL)
        goto error;

    line = p_response->p_intermediates->line;
    if (!line)
        goto error;

    bzero(tmp, len);
    strcpy(tmp, line);
    at_response_free(p_response);
    return 0;

error:
    at_response_free(p_response);
    return -1;
}

/* get IMEI */
int getImei(char* tmp, int len)
{
    ATResponse *p_response = NULL;
    int err = 0;
    char *line = NULL;

    bzero(tmp, len);
    err = at_send_command_numeric("AT+CGSN", &p_response);
    if (err < 0 || p_response->success == 0 || p_response->p_intermediates == NULL)
        goto error;

    line = p_response->p_intermediates->line;
    if (!line)
        goto error;

    strcpy(tmp, line);
    at_response_free(p_response);
    return 0;

error:
    at_response_free(p_response);
    return -1;
}

/* get number */
int getCnum(char* tmp, int len)
{
    ATResponse *p_response = NULL;
    int err = 0;
    char *line = NULL;
    char *alpha = NULL;
    char *number = NULL;

    bzero(tmp, len);
    err = at_send_command_singleline("AT+CNUM", "+CNUM:", &p_response);
    if (err < 0 || p_response->success == 0 || p_response->p_intermediates == NULL)
        goto error;

    line = p_response->p_intermediates->line;
    if (!line)
        goto error;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextstr(&line, &alpha);
    if (err < 0) goto error;

    err = at_tok_nextstr(&line, &number);
    if (err < 0) goto error;

    strcpy(tmp, number);
    at_response_free(p_response);
    return 0;

error:
    at_response_free(p_response);
    return -1;
}

void cycleUnsolicitedSimStatusChanged(void *param __unused)
{
    RLOGD("%s", __func__);
    int sim_status = getSimStatus();

    if (sim_status != g_sim_status) {
        g_sim_status = sim_status;
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
    }

    RIL_requestTimedCallback(cycleUnsolicitedSimStatusChanged, NULL, &RIL_UNSOL_SIM_STATUS_CHANGED_TIMER);
}

void requestSIMInfo(void *param __unused)
{
    RLOGD("%s", __func__);
    char tmp[128];
    static char flag = 0;
    static int times = 60;
    ATResponse *p_response = NULL;
    int err;

    if (0 == times--) {
        return;
    }

    if ((flag & 0x01) == 0) {
        if (getIccid(tmp, sizeof(tmp)) == 0) {  /*  Query iccid */
            ril_property_set("ro.modem.iccid", tmp);
            flag |= 0x01;
        }
    }

    if ((flag & 0x02) == 0) {
        if (getImsi(tmp, sizeof(tmp)) == 0) {  /*  Query IMSI */
            ril_property_set("ro.modem.imsi", tmp);
            flag |= 0x02;
        }
    }

    if ((flag & 0x04) == 0) {
        if (getImei(tmp, sizeof(tmp)) == 0) {  /*  Query IMEI */
            ril_property_set("ro.modem.imei", tmp);
            flag |= 0x04;
        }
    }

    if ((flag & 0x08) == 0) {
        if (getCnum(tmp, sizeof(tmp)) == 0) {  /*  Query number */
            ril_property_set("ro.modem.number", tmp);
            flag |= 0x08;
        }
    }

    if ((flag & 0x10) == 0) {
        err = at_send_command("AT+GPMMASK=101,28180080", &p_response); /*  enable only WMS QMI indication as default */
        if (err == 0 && p_response->success == 1) {
            flag |= 0x10;
        }
        at_response_free(p_response);
    }

    if (flag < 0x1F)
        RIL_requestTimedCallback(requestSIMInfo, NULL, &RIL_REQ_SIM_INFO_TIMER);
}

void requestUnsolicitedSimStatusChanged(void)
{
    RIL_requestTimedCallback(cycleUnsolicitedSimStatusChanged, NULL, NULL);
    RIL_requestTimedCallback(requestSIMInfo, NULL, NULL);
}

