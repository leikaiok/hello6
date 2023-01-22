/* //device/system/reference-ril/sms.c
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
#include "sms.h"
#include "ril_common.h"

#define LOG_TAG "RIL"
#include <utils/Log.h>

#define SMS_MEM_MAX         (255)
#define SMS_TIMEOUT_MESC    (10 * 1000)

#define NS_PER_S            (1000000000)

/* This is SMS memory index for recived and read index to make sure
 * called different AT cmd and RIL unsolicited with different SMS type
 */
static int in_index  = 0;
static int out_index = 0;
SMS_TYPE smsType[SMS_MEM_MAX] = {0};
int smsReceivedAck = FALSE;

static pthread_mutex_t s_command_mutex_sync = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_command_cond       = PTHREAD_COND_INITIALIZER;
int m_Index   = 0;

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

static void setTimespecRelative(struct timespec *p_ts, long long msec)
{
    struct timeval tv;

    gettimeofday(&tv, (struct timezone *)NULL);

    p_ts->tv_sec  = tv.tv_sec + (msec / 1000);
    p_ts->tv_nsec = (tv.tv_usec + (msec % 1000) * 1000L) * 1000L;

    if(p_ts->tv_nsec >= NS_PER_S) {
        p_ts->tv_sec++;
        p_ts->tv_nsec -= NS_PER_S;
    }
}

/* receive SMS and Read it */
void receiveSms(void *param)
{
    int sms_index = 0;
    int err       = 0;
    char *atcmd = NULL;
    struct timespec ts;

    if(param != NULL) {
        sms_index = *((int*)param);
        free(param);

        RLOGD("receiveSms: ready to read index = %d of SMS.", sms_index);
    }

    sleep(3);
    setTimespecRelative(&ts, SMS_TIMEOUT_MESC);

    if(smsType[out_index] == SMS_GENERAL) {
        at_send_command("AT+CPMS=\"ME\",\"ME\"\"ME\"", NULL);
    } else if(smsType[out_index] == SMS_SEND_REPORT) {
        at_send_command("AT+CPMS=\"SR\",\"ME\"\"ME\"", NULL);
    } else if(smsType[out_index] == SMS_SEND_REPORT) {
        at_send_command("AT+CPMS=\"ME\",\"ME\"\"ME\"", NULL);
    }

    asprintf(&atcmd, "AT+CMGR=%d", sms_index);
    err = at_send_command(atcmd, NULL);
    if(err < 0) {
        RLOGE("receiveSms: send AT+CMGR=%d failure.", sms_index);
        free(atcmd);
        out_index++;
        return;
    }

    pthread_mutex_lock(&s_command_mutex_sync);
    m_Index = sms_index;
    while(smsReceivedAck == FALSE) {
        err = pthread_cond_timedwait(&s_command_cond, &s_command_mutex_sync, &ts);
        if(err == ETIMEDOUT) {
            RLOGE("receiveSms: recieve SMS timeout.");
            break;
        }
    }

    smsReceivedAck = FALSE;
    pthread_mutex_unlock(&s_command_mutex_sync);

    free(atcmd);
    return;
}

/* New Sms Notification */
void onNewSmsNotification(const char *s)
{
    char* head = NULL;
    char* line = NULL;
    char* mem  = NULL;
    int err = 0;
    int *sms_index = NULL;

    RLOGD("onNewSmsNotification: recived a new SMS.");

    if(in_index == SMS_MEM_MAX) in_index = 0;
    if(out_index == SMS_MEM_MAX) out_index = 0;

    if(strStartsWith(s, "+CMTI:")) {
        smsType[in_index] = SMS_GENERAL;
        in_index++;
    } else if(strStartsWith(s, "+CDSI:")) {
        smsType[in_index] = SMS_SEND_REPORT;
        in_index++;
    } else if(strStartsWith(s, "+CBMI:")) {
        smsType[in_index] = SMS_BROADCAST;
        in_index++;
    }

    head = strdup(s);
    line = head;
    err = at_tok_start(&line);
    if(err < 0) {
        RLOGE("onNewSmsNotification: token start failure");
        out_index++;
        free(head);
        return;
    }

    err = at_tok_nextstr(&line, &mem);
    if(err < 0) {
        RLOGE("onNewSmsNotification: get SMS mem failure.");
        out_index++;
        free(head);
        return;
    }

    sms_index = (int *)malloc(sizeof(int));
    err = at_tok_nextint(&line, sms_index);
    if(err < 0) {
        RLOGE("onNewSmsNotification: get SMS index failure.");
        out_index++;
        free(head);
        free(sms_index);
        return;
    }

    if((*sms_index < 0) || (*sms_index > SMS_MEM_MAX - 1)) {
        RLOGE("onNewSmsNotification: sms_index value is invalid.");
        out_index++;
        free(head);
        free(sms_index);
        return;
    }

    RLOGD("onNewSmsNotification: ready to exec receiveSms and sms index = %d", *sms_index);
    if(s_rilenv == NULL) {
        RLOGD("onNewSmsNotification: s_rilenv is NULL");
        free(head);
        return;
    }

    RIL_requestTimedCallback(receiveSms, (void*)sms_index, NULL);
    free(head);
    return;
}

/* New Sms Read and report */
void onNewSmsArrived(const char *sms_pdu)
{
    const char *pdu = sms_pdu;
    char *sms_pdu_report = NULL;
    int i = 0;
    int sc_length = 0;

    if((smsType[out_index] == SMS_GENERAL) || (smsType[out_index] == SMS_BROADCAST)) {
        RLOGD("onNewSmsArrived: new sms pdu = %s", sms_pdu);
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NEW_SMS, sms_pdu, strlen(sms_pdu));
    } else if(smsType[out_index] == SMS_SEND_REPORT) {
        for(i = 0; i < 2; i++) {
            sc_length = (16 * sc_length) + HexCharToInt(*(pdu++));
        }

        asprintf(&sms_pdu_report, "0891682143658719F206%s", pdu + (sc_length + 1) * 2);
        RLOGD("onNewSmsArrived: new sms pdu report = %s", sms_pdu_report);
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT, sms_pdu_report, strlen(sms_pdu_report));
    } else {
        RLOGE("onNewSmsArrived: SMS type = %d is invalid.", smsType[out_index]);
    }

    out_index++;
    free(sms_pdu_report);
}

/* Deleted SMS by index */
int deleteSmsByIndex(int index)
{
    char *atcmd = NULL;

    pthread_mutex_lock(&s_command_mutex_sync);
    if((index >= 0) || (index < SMS_MEM_MAX)) {
        asprintf(&atcmd, "AT+CMGD=%d", index);
        at_send_command(atcmd, NULL);

        RLOGD("deleteSmsByIndex: deleted SMS by index = %d.", index);
    } else {
        RLOGE("deleteSmsByIndex: index = %d is invalid.", index);
        return FALSE;
    }

    smsReceivedAck = TRUE;
    pthread_cond_signal(&s_command_cond);
    pthread_mutex_unlock(&s_command_mutex_sync);
    return TRUE;
}

/* unsolicited unread SMS */
void unsolicitedUnreadSms(void *param __unused)
{
    int err = 0;
    int unreadSmsCount = 0;
    int smsIndex       = 0;

    ATResponse *p_response = NULL;
    ATLine *p_cur = NULL;
    char *head = NULL;
    char *line = NULL;

    /* send AT+CMGL=0 to list unread SMS */
    err = at_send_command_multiline_no_prefix("AT+CMGL=0", &p_response);
    RLOGD("unsolicitedUnreadSms: err = %d, p_response->success = %d", err, p_response->success);
    if(p_response == NULL) {
        RLOGD("unsolicitedUnreadSms: p_response is NULL");
    }
    if(p_response->p_intermediates == NULL) {
        RLOGD("unsolicitedUnreadSms: p_response->p_intermediates is NULL");
    }
    if(err != 0 || p_response == NULL || p_response->success == 0 || p_response->p_intermediates == NULL) {
        RLOGD("unsolicitedUnreadSms: send AT+CMGL=0 failure.");
        goto error;
    }

    for(unreadSmsCount = 0, p_cur = p_response->p_intermediates; p_cur != NULL && p_cur->p_next != NULL;) {
        head = strdup(p_cur->line);
        line = head;
        err = at_tok_start(&line);
        if(err < 0) {
            RLOGD("unsolicitedUnreadSms: parse AT+CMGL start failure.");
            goto error;
        }

        err = at_tok_nextint(&line, &smsIndex);
        if(err < 0) {
            RLOGD("unsolicitedUnreadSms: parse AT+CMGL index failure.");
            goto error;
        }

        RLOGD("unsolicitedUnreadSms: parse index = %d, sms pdu: %s", smsIndex, p_cur->p_next->line);
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NEW_SMS, p_cur->p_next->line, strlen(p_cur->p_next->line));

        p_cur = p_cur->p_next->p_next;
    }

    struct timespec ts;
    setTimespecRelative(&ts, SMS_TIMEOUT_MESC);

    pthread_mutex_lock(&s_command_mutex_sync);
    m_Index = smsIndex;
    while(smsReceivedAck == FALSE) {
        err = pthread_cond_timedwait(&s_command_cond, &s_command_mutex_sync, &ts);
        if(err == ETIMEDOUT) {
            RLOGE("unsolicitedUnreadSms: recieve SMS timeout.");
            break;
        }
    }

    smsReceivedAck = FALSE;
    pthread_mutex_unlock(&s_command_mutex_sync);

    at_response_free(p_response);
    free(head);
    return;

error:
    at_response_free(p_response);
    return;
}
