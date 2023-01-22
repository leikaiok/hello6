/* //device/system/reference-ril/reference-ril.c
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

#include <telephony/ril_cdma_sms.h>
#include <telephony/librilutils.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <fcntl.h>
#include <pthread.h>
#include <alloca.h>
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#include <getopt.h>
#include <sys/socket.h>
#include <cutils/properties.h>
#include <cutils/sockets.h>
#include <termios.h>
#include <qemu_pipe.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <net/if.h>
#include <netinet/in.h>
#include "ril.h"
/* BEGIN add network and sms head file by yangyao in 20220317 */
#include "network.h"
#include "sms.h"
#include "ril_common.h"
/* END add network head and sms file by yangyao in 20220317 */
/* BEGIN Add SIM card status check before initial by yangyao in 20220415 */
#include "sim.h"
/* END Add SIM card status check before initial by yangyao in 20220415 */
/* BEGIN add gobinet function for ril by leikai in 20220315 */
#include "gobinet.h"
/* END add gobinet function for ril by leikai in 20220315 */

#define LOG_TAG "RIL"
#include <utils/Log.h>

static void *noopRemoveWarning( void *a ) { return a; }
#define RIL_UNUSED_PARM(a) noopRemoveWarning((void *)&(a));

#define MAX_AT_RESPONSE 0x1000

/* pathname returned from RIL_REQUEST_SETUP_DATA_CALL / RIL_REQUEST_SETUP_DEFAULT_PDP */
// This is used if Wifi is not supported, plain old eth0
#define PPP_TTY_PATH_ETH0 "eth0"
// This is used if Wifi is supported to separate radio and wifi interface
#define PPP_TTY_PATH_RADIO0 "radio0"

// Default MTU value
#define DEFAULT_MTU 1500

#define GOSUNCN_RIL_VERSION    "YF-GM551AC2A V1.0.6B4 2022-09-16"

#ifdef USE_TI_COMMANDS

// Enable a workaround
// 1) Make incoming call, do not answer
// 2) Hangup remote end
// Expected: call should disappear from CLCC line
// Actual: Call shows as "ACTIVE" before disappearing
#define WORKAROUND_ERRONEOUS_ANSWER 1

// Some varients of the TI stack do not support the +CGEV unsolicited
// response. However, they seem to send an unsolicited +CME ERROR: 150
#define WORKAROUND_FAKE_CGEV 1
#endif

/* Modem Technology bits */
#define MDM_GSM         0x01
#define MDM_WCDMA       0x02
#define MDM_CDMA        0x04
#define MDM_EVDO        0x08
#define MDM_LTE         0x10

/* BEGIN Add signal unsolicited by yangyao in 20220322 */
#define UNSOL_SIG_INTERVAL    (5)
/* END Add signal unsolicited by yangyao in 20220322 */

/* BEGIN Add SIM status changed unsolicited by yangyao in 20220418 */
#define UNSOL_SIM_STATUS_INTERVAL    (30)
/* END Add SIM status changed unsolicited by yangyao in 20220418 */

/* BEGIN check at handshake by yangyao in 20220309 */
#define MAX_RILD_RESET_TIMES    (3)
/* END check at handshake by yangyao in 20220309 */

/* BEGIN Add not response ril request when ril initial is not finish by yangyao in 20220525 */
#define MAX_WAIT_RIL_INITIAL_TIMES    (5)
/* END Add not response ril request when ril initial is not finish by yangyao in 20220525 */

/* BEGIN Modify when received SMS and deleted by yangyao in 20220318 */
extern int m_Index;
/* END Modify when received SMS and deleted by yangyao in 20220318 */

/* BEGIN Add SIM card status check before initial by yangyao in 20220415 */
extern int g_sim_status;
/* END Add SIM card status check before initial by yangyao in 20220415 */

typedef struct {
    int supportedTechs; // Bitmask of supported Modem Technology bits
    int currentTech;    // Technology the modem is currently using (in the format used by modem)
    int isMultimode;

    // Preferred mode bitmask. This is actually 4 byte-sized bitmasks with different priority values,
    // in which the byte number from LSB to MSB give the priority.
    //
    //          |MSB|   |   |LSB
    // value:   |00 |00 |00 |00
    // byte #:  |3  |2  |1  |0
    //
    // Higher byte order give higher priority. Thus, a value of 0x0000000f represents
    // a preferred mode of GSM, WCDMA, CDMA, and EvDo in which all are equally preferrable, whereas
    // 0x00000201 represents a mode with GSM and WCDMA, in which WCDMA is preferred over GSM
    int32_t preferredNetworkMode;
    int subscription_source;

} ModemInfo;

static char ttyAT[20];
static ModemInfo *sMdmInfo;
// TECH returns the current technology in the format used by the modem.
// It can be used as an l-value
#define TECH(mdminfo)                 ((mdminfo)->currentTech)
// TECH_BIT returns the bitmask equivalent of the current tech
#define TECH_BIT(mdminfo)            (1 << ((mdminfo)->currentTech))
#define IS_MULTIMODE(mdminfo)         ((mdminfo)->isMultimode)
#define TECH_SUPPORTED(mdminfo, tech) ((mdminfo)->supportedTechs & (tech))
#define PREFERRED_NETWORK(mdminfo)    ((mdminfo)->preferredNetworkMode)
// CDMA Subscription Source
#define SSOURCE(mdminfo)              ((mdminfo)->subscription_source)

static int net2modem[] = {
    MDM_GSM | MDM_WCDMA,                                 // 0  - GSM / WCDMA Pref
    MDM_GSM,                                             // 1  - GSM only
    MDM_WCDMA,                                           // 2  - WCDMA only
    MDM_GSM | MDM_WCDMA,                                 // 3  - GSM / WCDMA Auto
    MDM_CDMA | MDM_EVDO,                                 // 4  - CDMA / EvDo Auto
    MDM_CDMA,                                            // 5  - CDMA only
    MDM_EVDO,                                            // 6  - EvDo only
    MDM_GSM | MDM_WCDMA | MDM_CDMA | MDM_EVDO,           // 7  - GSM/WCDMA, CDMA, EvDo
    MDM_LTE | MDM_CDMA | MDM_EVDO,                       // 8  - LTE, CDMA and EvDo
    MDM_LTE | MDM_GSM | MDM_WCDMA,                       // 9  - LTE, GSM/WCDMA
    MDM_LTE | MDM_CDMA | MDM_EVDO | MDM_GSM | MDM_WCDMA, // 10 - LTE, CDMA, EvDo, GSM/WCDMA
    MDM_LTE,                                             // 11 - LTE only
};

static int32_t net2pmask[] = {
    MDM_GSM | (MDM_WCDMA << 8),                          // 0  - GSM / WCDMA Pref
    MDM_GSM,                                             // 1  - GSM only
    MDM_WCDMA,                                           // 2  - WCDMA only
    MDM_GSM | MDM_WCDMA,                                 // 3  - GSM / WCDMA Auto
    MDM_CDMA | MDM_EVDO,                                 // 4  - CDMA / EvDo Auto
    MDM_CDMA,                                            // 5  - CDMA only
    MDM_EVDO,                                            // 6  - EvDo only
    MDM_GSM | MDM_WCDMA | MDM_CDMA | MDM_EVDO,           // 7  - GSM/WCDMA, CDMA, EvDo
    MDM_LTE | MDM_CDMA | MDM_EVDO,                       // 8  - LTE, CDMA and EvDo
    MDM_LTE | MDM_GSM | MDM_WCDMA,                       // 9  - LTE, GSM/WCDMA
    MDM_LTE | MDM_CDMA | MDM_EVDO | MDM_GSM | MDM_WCDMA, // 10 - LTE, CDMA, EvDo, GSM/WCDMA
    MDM_LTE,                                             // 11 - LTE only
};

static int is3gpp2(int radioTech) {
    switch (radioTech) {
        case RADIO_TECH_IS95A:
        case RADIO_TECH_IS95B:
        case RADIO_TECH_1xRTT:
        case RADIO_TECH_EVDO_0:
        case RADIO_TECH_EVDO_A:
        case RADIO_TECH_EVDO_B:
        case RADIO_TECH_EHRPD:
            return 1;
        default:
            return 0;
    }
}

typedef enum {
    SIM_ABSENT = 0,
    SIM_NOT_READY = 1,
    SIM_READY = 2,
    SIM_PIN = 3,
    SIM_PUK = 4,
    SIM_NETWORK_PERSONALIZATION = 5,
    RUIM_ABSENT = 6,
    RUIM_NOT_READY = 7,
    RUIM_READY = 8,
    RUIM_PIN = 9,
    RUIM_PUK = 10,
    RUIM_NETWORK_PERSONALIZATION = 11,
    ISIM_ABSENT = 12,
    ISIM_NOT_READY = 13,
    ISIM_READY = 14,
    ISIM_PIN = 15,
    ISIM_PUK = 16,
    ISIM_NETWORK_PERSONALIZATION = 17,
} SIM_Status;

static void onRequest (int request, void *data, size_t datalen, RIL_Token t);
static RIL_RadioState currentState();
static int onSupports (int requestCode);
static void onCancel (RIL_Token t);
static const char *getVersion();
static int isRadioOn();
static SIM_Status getSIMStatus();
static int getCardStatus(RIL_CardStatus_v6 **pp_card_status);
static void freeCardStatus(RIL_CardStatus_v6 *p_card_status);
static void onDataCallListChanged(void *param);
static void onATTimeout();
extern const char * requestToString(int request);
extern int get_ttyAT(char *out_ttyname);
extern int detect_module(void);

/*** Static Variables ***/
static const RIL_RadioFunctions s_callbacks = {
    RIL_VERSION,
    onRequest,
    currentState,
    onSupports,
    onCancel,
    getVersion
};

static RIL_RadioState sState = RADIO_STATE_UNAVAILABLE;

static pthread_mutex_t s_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_state_cond = PTHREAD_COND_INITIALIZER;

static int s_port = -1;
static const char * s_device_path = NULL;
static int          s_device_socket = 0;

/* trigger change to this with s_state_cond */
static int s_closed = 0;

static int sFD;     /* file desc of AT channel */
static char sATBuffer[MAX_AT_RESPONSE+1];
static char *sATBufferCur = NULL;

static const struct timeval TIMEVAL_SIMPOLL = {1,0};
static const struct timeval TIMEVAL_CALLSTATEPOLL = {0,500000};
static const struct timeval TIMEVAL_0 = {0,0};

static int s_ims_registered  = 0;        // 0==unregistered
static int s_ims_services    = 1;        // & 0x1 == sms over ims supported
static int s_ims_format    = 1;          // FORMAT_3GPP(1) vs FORMAT_3GPP2(2);
static int s_ims_cause_retry = 0;        // 1==causes sms over ims to temp fail
static int s_ims_cause_perm_failure = 0; // 1==causes sms over ims to permanent fail
static int s_ims_gsm_retry   = 0;        // 1==causes sms over gsm to temp fail
static int s_ims_gsm_fail    = 0;        // 1==causes sms over gsm to permanent fail

#ifdef WORKAROUND_ERRONEOUS_ANSWER
// Max number of times we'll try to repoll when we think
// we have a AT+CLCC race condition
#define REPOLL_CALLS_COUNT_MAX 4

// Line index that was incoming or waiting at last poll, or -1 for none
static int s_incomingOrWaitingLine = -1;
// Number of times we've asked for a repoll of AT+CLCC
static int s_repollCallsCount = 0;
// Should we expect a call to be answered in the next CLCC?
static int s_expectAnswer = 0;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */


static int s_cell_info_rate_ms = INT_MAX;
static int s_mcc = 0;
static int s_mnc = 0;
static int s_lac = 0;
static int s_cid = 0;

/* BEGIN Add RILJ and RILD initial flag by yangyao in 20220322 */
static int g_is_rild_inited = FALSE;
static int g_is_rilj_inited = FALSE;
static pthread_mutex_t g_init_mutex = PTHREAD_MUTEX_INITIALIZER;
/* END Add RILJ and RILD initial flag by yangyao in 20220322 */

static void pollSIMState (void *param);
static void setRadioState(RIL_RadioState newState);
static void setRadioTechnology(ModemInfo *mdm, int newtech);
static int query_ctec(ModemInfo *mdm, int *current, int32_t *preferred);
static int parse_technology_response(const char *response, int *current, int32_t *preferred);
static int techFromModemType(int mdmtype);

static int clccStateToRILState(int state, RIL_CallState *p_state)

{
    switch(state) {
        case 0: *p_state = RIL_CALL_ACTIVE;   return 0;
        case 1: *p_state = RIL_CALL_HOLDING;  return 0;
        case 2: *p_state = RIL_CALL_DIALING;  return 0;
        case 3: *p_state = RIL_CALL_ALERTING; return 0;
        case 4: *p_state = RIL_CALL_INCOMING; return 0;
        case 5: *p_state = RIL_CALL_WAITING;  return 0;
        default: return -1;
    }
}

/**
 * Note: directly modified line and has *p_call point directly into
 * modified line
 */
static int callFromCLCCLine(char *line, RIL_Call *p_call)
{
        //+CLCC: 1,0,2,0,0,\"+18005551212\",145
        //     index,isMT,state,mode,isMpty(,number,TOA)?

    int err;
    int state;
    int mode;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(p_call->index));
    if (err < 0) goto error;

    err = at_tok_nextbool(&line, &(p_call->isMT));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &state);
    if (err < 0) goto error;

    err = clccStateToRILState(state, &(p_call->state));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &mode);
    if (err < 0) goto error;

    p_call->isVoice = (mode == 0);

    err = at_tok_nextbool(&line, &(p_call->isMpty));
    if (err < 0) goto error;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &(p_call->number));

        /* tolerate null here */
        if (err < 0) return 0;

        // Some lame implementations return strings
        // like "NOT AVAILABLE" in the CLCC line
        if (p_call->number != NULL
            && 0 == strspn(p_call->number, "+0123456789")
        ) {
            p_call->number = NULL;
        }

        err = at_tok_nextint(&line, &p_call->toa);
        if (err < 0) goto error;
    }

    p_call->uusInfo = NULL;

    return 0;

error:
    RLOGE("invalid CLCC line\n");
    return -1;
}

static int parseSimResponseLine(char* line, RIL_SIM_IO_Response* response) {
    int err;

    err = at_tok_start(&line);
    if (err < 0) return err;
    err = at_tok_nextint(&line, &response->sw1);
    if (err < 0) return err;
    err = at_tok_nextint(&line, &response->sw2);
    if (err < 0) return err;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &response->simResponse);
        if (err < 0) return err;
    }
    return 0;
}

enum InterfaceState {
    kInterfaceUp,
    kInterfaceDown,
};

static RIL_Errno setInterfaceState(const char* interfaceName,
                                   enum InterfaceState state) {
    struct ifreq request;
    int status = 0;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock == -1) {
        RLOGE("Failed to open interface socket: %s (%d)",
              strerror(errno), errno);
        return RIL_E_GENERIC_FAILURE;
    }

    memset(&request, 0, sizeof(request));
    strncpy(request.ifr_name, interfaceName, sizeof(request.ifr_name));
    request.ifr_name[sizeof(request.ifr_name) - 1] = '\0';
    status = ioctl(sock, SIOCGIFFLAGS, &request);
    if (status != 0) {
        RLOGE("Failed to get interface flags for %s: %s (%d)",
              interfaceName, strerror(errno), errno);
        close(sock);
        return RIL_E_RADIO_NOT_AVAILABLE;
    }

    bool isUp = (request.ifr_flags & IFF_UP);
    if ((state == kInterfaceUp && isUp) || (state == kInterfaceDown && !isUp)) {
        // Interface already in desired state
        close(sock);
        return RIL_E_SUCCESS;
    }

    // Simply toggle the flag since we know it's the opposite of what we want
    request.ifr_flags ^= IFF_UP;

    status = ioctl(sock, SIOCSIFFLAGS, &request);
    if (status != 0) {
        RLOGE("Failed to set interface flags for %s: %s (%d)",
              interfaceName, strerror(errno), errno);
        close(sock);
        return RIL_E_GENERIC_FAILURE;
    }

    close(sock);
    return RIL_E_SUCCESS;
}

/** do post-AT+CFUN=1 initialization */
static void onRadioPowerOn()
{
#ifdef USE_TI_COMMANDS
    /*  Must be after CFUN=1 */
    /*  TI specific -- notifications for CPHS things such */
    /*  as CPHS message waiting indicator */

    at_send_command("AT%CPHS=1", NULL);

    /*  TI specific -- enable NITZ unsol notifs */
    at_send_command("AT%CTZV=1", NULL);
#endif

    pollSIMState(NULL);
}

/** do post- SIM ready initialization */
static void onSIMReady()
{
    at_send_command_singleline("AT+CSMS=1", "+CSMS:", NULL);
    /*
     * Always send SMS messages directly to the TE
     *
     * mode = 1 // discard when link is reserved (link should never be
     *             reserved)
     * mt = 2   // most messages routed to TE
     * bm = 2   // new cell BM's routed to TE
     * ds = 1   // Status reports routed to TE
     * bfr = 1  // flush buffer
     */
    at_send_command("AT+CNMI=1,2,2,1,1", NULL);
}

static void requestRadioPower(void *data, size_t datalen __unused, RIL_Token t)
{
    RLOGD("%s entry", __func__);
    int onOff;

    int err;
    ATResponse *p_response = NULL;

    assert (datalen >= sizeof(int *));
    onOff = ((int *)data)[0];

    onOff = 1; /* For weishitong, when start up will exec cfun=4. */

    if (onOff == 0 && sState != RADIO_STATE_OFF) {
        /* BEGIN Modify radio power off as AT+CFUN=4 by yangyao in 20220309 */
        err = at_send_command("AT+CFUN=4", &p_response);
        /* END Modify radio power off as AT+CFUN=4 by yangyao in 20220309 */
        if (err < 0 || p_response->success == 0) goto error;
        setRadioState(RADIO_STATE_OFF);
    } else if (onOff > 0 && sState == RADIO_STATE_OFF) {
        err = at_send_command("AT+CFUN=1", &p_response);
        if (err < 0|| p_response->success == 0) {
            // Some stacks return an error when there is no SIM,
            // but they really turn the RF portion on
            // So, if we get an error, let's check to see if it
            // turned on anyway

            if (isRadioOn() != 1) {
                goto error;
            }
        }
        setRadioState(RADIO_STATE_ON);
    }

    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;
error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestShutdown(RIL_Token t)
{
    int onOff;

    int err;
    ATResponse *p_response = NULL;

    if (sState != RADIO_STATE_OFF) {
        err = at_send_command("AT+CFUN=0", &p_response);
        setRadioState(RADIO_STATE_UNAVAILABLE);
    }

    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;
}

static void requestOrSendDataCallList(RIL_Token *t);

static void onDataCallListChanged(void *param __unused)
{
    /* BEGIN Deleted requestOrSendDataCallList(NULL) by yangyao in 20220304 */
    //requestOrSendDataCallList(NULL);
    /* END Deleted requestOrSendDataCallList(NULL) by yangyao in 20220304 */
}

static void requestDataCallList(void *data __unused, size_t datalen __unused, RIL_Token t)
{
    requestOrSendDataCallList(&t);
}

// Hang up, reject, conference, call waiting
static void requestCallSelection(
                void *data __unused, size_t datalen __unused, RIL_Token t, int request)
{
    // 3GPP 22.030 6.5.5
    static char hangupWaiting[]    = "AT+CHLD=0";
    static char hangupForeground[] = "AT+CHLD=1";
    static char switchWaiting[]    = "AT+CHLD=2";
    static char conference[]       = "AT+CHLD=3";
    static char reject[]           = "ATH";

    char* atCommand;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }

    switch(request) {
        case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND:
            // "Releases all held calls or sets User Determined User Busy
            //  (UDUB) for a waiting call."
            atCommand = hangupWaiting;
            break;
        case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
            // "Releases all active calls (if any exist) and accepts
            //  the other (held or waiting) call."
            atCommand = hangupForeground;
            break;
        case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE:
            // "Places all active calls (if any exist) on hold and accepts
            //  the other (held or waiting) call."
            atCommand = switchWaiting;
#ifdef WORKAROUND_ERRONEOUS_ANSWER
            s_expectAnswer = 1;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */
            break;
        case RIL_REQUEST_CONFERENCE:
            // "Adds a held call to the conversation"
            atCommand = conference;
            break;
        case RIL_REQUEST_UDUB:
            // User determined user busy (reject)
            atCommand = reject;
            break;
        default:
            assert(0);
    }
    at_send_command(atCommand, NULL);
    // Success or failure is ignored by the upper layer here.
    // It will call GET_CURRENT_CALLS and determine success that way.
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static bool hasWifiCapability()
{
    char propValue[PROP_VALUE_MAX];
    return property_get("ro.kernel.qemu.wifi", propValue, "") > 0 &&
           strcmp("1", propValue) == 0;
}

static const char* getRadioInterfaceName(bool hasWifi)
{
    return hasWifi ? PPP_TTY_PATH_RADIO0 : PPP_TTY_PATH_ETH0;
}

static void requestOrSendDataCallList(RIL_Token *t)
{
    ATResponse *p_response;
    ATLine *p_cur;
    int err;
    int n = 0;
    char *out;
    char propValue[PROP_VALUE_MAX];
    bool hasWifi = hasWifiCapability();
    const char* radioInterfaceName = getRadioInterfaceName(hasWifi);

    err = at_send_command_multiline ("AT+CGACT?", "+CGACT:", &p_response);
    if (err != 0 || p_response->success == 0) {
        if (t != NULL)
            RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
        else
            RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                      NULL, 0);
        return;
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next)
        n++;

    RIL_Data_Call_Response_v11 *responses =
        alloca(n * sizeof(RIL_Data_Call_Response_v11));

    int i;
    for (i = 0; i < n; i++) {
        responses[i].status = -1;
        responses[i].suggestedRetryTime = -1;
        responses[i].cid = -1;
        responses[i].active = -1;
        responses[i].type = "";
        responses[i].ifname = "";
        responses[i].addresses = "";
        responses[i].dnses = "";
        responses[i].gateways = "";
        responses[i].pcscf = "";
        responses[i].mtu = 0;
    }

    RIL_Data_Call_Response_v11 *response = responses;
    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        char *line = p_cur->line;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &response->cid);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &response->active);
        if (err < 0)
            goto error;

        response++;
    }

    at_response_free(p_response);

    err = at_send_command_multiline ("AT+CGDCONT?", "+CGDCONT:", &p_response);
    if (err != 0 || p_response->success == 0) {
        if (t != NULL)
            RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
        else
            RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                      NULL, 0);
        return;
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        char *line = p_cur->line;
        int cid;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &cid);
        if (err < 0)
            goto error;

        for (i = 0; i < n; i++) {
            if (responses[i].cid == cid)
                break;
        }

        if (i >= n) {
            /* details for a context we didn't hear about in the last request */
            continue;
        }

        // Assume no error
        responses[i].status = 0;

        // type
        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

        int type_size = strlen(out) + 1;
        responses[i].type = alloca(type_size);
        strlcpy(responses[i].type, out, type_size);

        // APN ignored for v5
        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

        int ifname_size = strlen(radioInterfaceName) + 1;
        responses[i].ifname = alloca(ifname_size);
        strlcpy(responses[i].ifname, radioInterfaceName, ifname_size);

        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

        int addresses_size = strlen(out) + 1;
        responses[i].addresses = alloca(addresses_size);
        strlcpy(responses[i].addresses, out, addresses_size);

        if (isInEmulator()) {
            /* We are in the emulator - the dns servers are listed
                * by the following system properties, setup in
                * /system/etc/init.goldfish.sh:
                *  - net.eth0.dns1
                *  - net.eth0.dns2
                *  - net.eth0.dns3
                *  - net.eth0.dns4
                */
            const int   dnslist_sz = 128;
            char*       dnslist = alloca(dnslist_sz);
            const char* separator = "";
            int         nn;

            dnslist[0] = 0;
            for (nn = 1; nn <= 4; nn++) {
                /* Probe net.eth0.dns<n> */
                char  propName[PROP_NAME_MAX];
                char  propValue[PROP_VALUE_MAX];

                snprintf(propName, sizeof propName, "net.eth0.dns%d", nn);

                /* Ignore if undefined */
                if (property_get(propName, propValue, "") <= 0) {
                    continue;
                }

                /* Append the DNS IP address */
                strlcat(dnslist, separator, dnslist_sz);
                strlcat(dnslist, propValue, dnslist_sz);
                separator = " ";
            }
            responses[i].dnses = dnslist;

            /* There is only one gateway in the emulator. If WiFi is
             * configured the interface visible to RIL will be behind a NAT
             * where the gateway is different. */
            if (hasWifi) {
                responses[i].gateways = "192.168.200.1";
            } else if (property_get("net.eth0.gw", propValue, "") > 0) {
                responses[i].gateways = propValue;
            } else {
                responses[i].gateways = "";
            }
            responses[i].mtu = DEFAULT_MTU;
        }
        else {
            /* I don't know where we are, so use the public Google DNS
                * servers by default and no gateway.
                */
            responses[i].dnses = "8.8.8.8 8.8.4.4";
            responses[i].gateways = "";
        }
    }

    at_response_free(p_response);

    if (t != NULL)
        RIL_onRequestComplete(*t, RIL_E_SUCCESS, responses,
                              n * sizeof(RIL_Data_Call_Response_v11));
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                  responses,
                                  n * sizeof(RIL_Data_Call_Response_v11));

    return;

error:
    if (t != NULL)
        RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                  NULL, 0);

    at_response_free(p_response);
}

static void requestQueryNetworkSelectionMode(
                void *data __unused, size_t datalen __unused, RIL_Token t)
{
    int err;
    ATResponse *p_response = NULL;
    int response = 0;
    char *line;

    err = at_send_command_singleline("AT+COPS?", "+COPS:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);

    if (err < 0) {
        goto error;
    }

    err = at_tok_nextint(&line, &response);

    if (err < 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
    at_response_free(p_response);
    return;
error:
    at_response_free(p_response);
    RLOGE("requestQueryNetworkSelectionMode must never return error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void sendCallStateChanged(void *param __unused)
{
    RIL_onUnsolicitedResponse (
        RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
        NULL, 0);
}

static void requestGetCurrentCalls(void *data __unused, size_t datalen __unused, RIL_Token t)
{
    int err;
    ATResponse *p_response;
    ATLine *p_cur;
    int countCalls;
    int countValidCalls;
    RIL_Call *p_calls;
    RIL_Call **pp_calls;
    int i;
    int needRepoll = 0;

#ifdef WORKAROUND_ERRONEOUS_ANSWER
    int prevIncomingOrWaitingLine;

    prevIncomingOrWaitingLine = s_incomingOrWaitingLine;
    s_incomingOrWaitingLine = -1;
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

    err = at_send_command_multiline ("AT+CLCC", "+CLCC:", &p_response);

    if (err != 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    /* count the calls */
    for (countCalls = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next
    ) {
        countCalls++;
    }

    /* yes, there's an array of pointers and then an array of structures */

    pp_calls = (RIL_Call **)alloca(countCalls * sizeof(RIL_Call *));
    p_calls = (RIL_Call *)alloca(countCalls * sizeof(RIL_Call));
    memset (p_calls, 0, countCalls * sizeof(RIL_Call));

    /* init the pointer array */
    for(i = 0; i < countCalls ; i++) {
        pp_calls[i] = &(p_calls[i]);
    }

    for (countValidCalls = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next
    ) {
        err = callFromCLCCLine(p_cur->line, p_calls + countValidCalls);

        if (err != 0) {
            continue;
        }

#ifdef WORKAROUND_ERRONEOUS_ANSWER
        if (p_calls[countValidCalls].state == RIL_CALL_INCOMING
            || p_calls[countValidCalls].state == RIL_CALL_WAITING
        ) {
            s_incomingOrWaitingLine = p_calls[countValidCalls].index;
        }
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

        if (p_calls[countValidCalls].state != RIL_CALL_ACTIVE
            && p_calls[countValidCalls].state != RIL_CALL_HOLDING
        ) {
            needRepoll = 1;
        }

        countValidCalls++;
    }

#ifdef WORKAROUND_ERRONEOUS_ANSWER
    // Basically:
    // A call was incoming or waiting
    // Now it's marked as active
    // But we never answered it
    //
    // This is probably a bug, and the call will probably
    // disappear from the call list in the next poll
    if (prevIncomingOrWaitingLine >= 0
            && s_incomingOrWaitingLine < 0
            && s_expectAnswer == 0
    ) {
        for (i = 0; i < countValidCalls ; i++) {

            if (p_calls[i].index == prevIncomingOrWaitingLine
                    && p_calls[i].state == RIL_CALL_ACTIVE
                    && s_repollCallsCount < REPOLL_CALLS_COUNT_MAX
            ) {
                RLOGI(
                    "Hit WORKAROUND_ERRONOUS_ANSWER case."
                    " Repoll count: %d\n", s_repollCallsCount);
                s_repollCallsCount++;
                goto error;
            }
        }
    }

    s_expectAnswer = 0;
    s_repollCallsCount = 0;
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

    RIL_onRequestComplete(t, RIL_E_SUCCESS, pp_calls,
            countValidCalls * sizeof (RIL_Call *));

    at_response_free(p_response);

#ifdef POLL_CALL_STATE
    if (countValidCalls) {  // We don't seem to get a "NO CARRIER" message from
                            // smd, so we're forced to poll until the call ends.
#else
    if (needRepoll) {
#endif
        RIL_requestTimedCallback (sendCallStateChanged, NULL, &TIMEVAL_CALLSTATEPOLL);
    }

    return;
#ifdef WORKAROUND_ERRONEOUS_ANSWER
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
#endif
}

static void requestDial(void *data, size_t datalen __unused, RIL_Token t)
{
    RIL_Dial *p_dial;
    char *cmd;
    const char *clir;
    int ret;

    p_dial = (RIL_Dial *)data;

    switch (p_dial->clir) {
        case 1: clir = "I"; break;  /*invocation*/
        case 2: clir = "i"; break;  /*suppression*/
        default:
        case 0: clir = ""; break;   /*subscription default*/
    }

    asprintf(&cmd, "ATD%s%s;", p_dial->address, clir);

    ret = at_send_command(cmd, NULL);

    free(cmd);

    /* success or failure is ignored by the upper layer here.
       it will call GET_CURRENT_CALLS and determine success that way */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestWriteSmsToSim(void *data, size_t datalen __unused, RIL_Token t)
{
    RIL_SMS_WriteArgs *p_args;
    char *cmd;
    int length;
    int err;
    ATResponse *p_response = NULL;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
        return;
    }

    p_args = (RIL_SMS_WriteArgs *)data;

    length = strlen(p_args->pdu)/2;
    asprintf(&cmd, "AT+CMGW=%d,%d", length, p_args->status);

    err = at_send_command_sms(cmd, p_args->pdu, "+CMGW:", &p_response);

    if (err != 0 || p_response->success == 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestHangup(void *data, size_t datalen __unused, RIL_Token t)
{
    int *p_line;

    int ret;
    char *cmd;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_MODEM_ERR, NULL, 0);
        return;
    }
    p_line = (int *)data;

    // 3GPP 22.030 6.5.5
    // "Releases a specific active call X"
    asprintf(&cmd, "AT+CHLD=1%d", p_line[0]);

    ret = at_send_command(cmd, NULL);

    free(cmd);

    /* success or failure is ignored by the upper layer here.
       it will call GET_CURRENT_CALLS and determine success that way */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestSignalStrength(void *data __unused, size_t datalen __unused, RIL_Token t)
{
    RIL_SignalStrength_v10 signalStrength;

    if(getSignalStrength(&signalStrength) == FALSE) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &signalStrength, sizeof(RIL_SignalStrength_v10));
    return;

error:
    RLOGE("requestSignalStrength: get request signal failure.");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

/**
 * networkModePossible. Decides whether the network mode is appropriate for the
 * specified modem
 */
static int networkModePossible(ModemInfo *mdm, int nm)
{
    if ((net2modem[nm] & mdm->supportedTechs) == net2modem[nm]) {
       return 1;
    }
    return 0;
}
static void requestSetPreferredNetworkType( int request __unused, void *data,
                                            size_t datalen __unused, RIL_Token t )
{
    /* BEGIN Modify set preferred network by yangyao in 20220311 */
    ATResponse *p_response = NULL;
    char *cmd = NULL;
    char *line = NULL;
    int value = *(int *)data;
    int current, old;
    int err;

    RLOGD("requestSetPreferredNetworkType: set param value = %d", value);

    /* Get cuurent preferred network mode */
    err = at_send_command_singleline("AT+ZSNT?", "+ZSNT:", &p_response);
    if(err < 0 || p_response->success == 0) {
        RLOGE("requestSetPreferredNetworkType: exec AT+ZSNT? failure");
        goto error;
    }

    RLOGD("requestGetPreferredNetworkType: AT resp = %s", p_response->p_intermediates->line);

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if(err < 0) goto error;

    err = at_tok_nextint(&line, &old);
    if(err < 0) goto error;

    RLOGD("requestSetPreferredNetworkType: value RAT = %d", value);
    current = convertPreferredNetworkTypeToRil(value);

    at_response_free(p_response);
    p_response = NULL;

    RLOGD("requestSetPreferredNetworkType: current RAT = %d and old RAT = %d", current, old);
    if(current != old) {
        asprintf(&cmd, "AT+ZSNT=%d,0,0", current);
        err =  at_send_command_singleline(cmd, "+ZSNT:", &p_response);

        if(err < 0 || p_response->success == 0) {
            RLOGE("requestSetPreferredNetworkType: exec AT+ZSNT=%d,0,0 failure", current);
            goto error;
        } else {
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            at_response_free(p_response);
            p_response = NULL;

            /* Need add deactive data call here */
        }

        if(cmd != NULL) {
            free(cmd);
            cmd = NULL;
        }
    }

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    p_response = NULL;
    /* END Modify set preferred network by yangyao in 20220311 */
}

static void requestGetPreferredNetworkType(int request __unused, void *data __unused,
                                   size_t datalen __unused, RIL_Token t)
{
    /* BEGIN Modify get preferred network by yangyao in 20220311 */
    int err = 0;
    ATResponse *p_response = NULL;
    int response = 0;
    int rat      = 0;
    char *line = NULL;

    err = at_send_command_singleline("AT+ZSNT?", "+ZSNT:", &p_response);
    if(err < 0 || p_response->success == 0) {
        RLOGE("requestGetPreferredNetworkType: exec AT+ZSNT? failure.");
        goto error;
    }

    RLOGD("requestGetPreferredNetworkType: AT resp = %s", p_response->p_intermediates->line);

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if(err < 0) goto error;

    err = at_tok_nextint(&line, &rat);
    if(err < 0) goto error;

    response = convertRilToPreferredNetworkType(rat);

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
    at_response_free(p_response);
    p_response = NULL;
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    p_response = NULL;
    /* END Modify get preferred network by yangyao in 20220311 */
}

static void requestCdmaPrlVersion(int request __unused, void *data __unused,
                                   size_t datalen __unused, RIL_Token t)
{
    int err;
    char * responseStr;
    ATResponse *p_response = NULL;
    const char *cmd;
    char *line;

    err = at_send_command_singleline("AT+WPRL?", "+WPRL:", &p_response);
    if (err < 0 || !p_response->success) goto error;
    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;
    err = at_tok_nextstr(&line, &responseStr);
    if (err < 0 || !responseStr) goto error;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, strlen(responseStr));
    at_response_free(p_response);
    return;
error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaBaseBandVersion(int request __unused, void *data __unused,
                                   size_t datalen __unused, RIL_Token t)
{
    int err;
    char * responseStr;
    ATResponse *p_response = NULL;
    const char *cmd;
    const char *prefix;
    char *line, *p;
    int commas;
    int skip;
    int count = 4;

    // Fixed values. TODO: query modem
    responseStr = strdup("1.0.0.0");
    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, sizeof(responseStr));
    free(responseStr);
}

static void requestDeviceIdentity(int request __unused, void *data __unused,
                                        size_t datalen __unused, RIL_Token t)
{
    int err;
    int response[4];
    char * responseStr[4];
    ATResponse *p_response = NULL;
    const char *cmd;
    const char *prefix;
    char *line, *p;
    int commas;
    int skip;
    int count = 4;

    // Fixed values. TODO: Query modem
    responseStr[0] = "----";
    responseStr[1] = "----";
    responseStr[2] = "77777777";
    responseStr[3] = ""; // default empty for non-CDMA

    err = at_send_command_numeric("AT+CGSN", &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    } else {
        if (TECH_BIT(sMdmInfo) == MDM_CDMA) {
            responseStr[3] = p_response->p_intermediates->line;
        } else {
            responseStr[0] = p_response->p_intermediates->line;
        }
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, count*sizeof(char*));
    at_response_free(p_response);
}

static void requestCdmaGetSubscriptionSource(int request __unused, void *data,
                                        size_t datalen __unused, RIL_Token t)
{
    int err;
    int *ss = (int *)data;
    ATResponse *p_response = NULL;
    char *cmd = NULL;
    char *line = NULL;
    int response;

    asprintf(&cmd, "AT+CCSS?");
    if (!cmd) goto error;

    err = at_send_command_singleline(cmd, "+CCSS:", &p_response);
    if (err < 0 || !p_response->success)
        goto error;

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &response);
    free(cmd);
    cmd = NULL;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));

    return;
error:
    free(cmd);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaSetSubscriptionSource(int request __unused, void *data,
                                        size_t datalen, RIL_Token t)
{
    int err;
    int *ss = (int *)data;
    ATResponse *p_response = NULL;
    char *cmd = NULL;

    if (!ss || !datalen) {
        RLOGE("RIL_REQUEST_CDMA_SET_SUBSCRIPTION without data!");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    asprintf(&cmd, "AT+CCSS=%d", ss[0]);
    if (!cmd) goto error;

    err = at_send_command(cmd, &p_response);
    if (err < 0 || !p_response->success)
        goto error;
    free(cmd);
    cmd = NULL;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

    RIL_onUnsolicitedResponse(RIL_UNSOL_CDMA_SUBSCRIPTION_SOURCE_CHANGED, ss, sizeof(ss[0]));

    return;
error:
    free(cmd);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaSubscription(int request __unused, void *data __unused,
                                        size_t datalen __unused, RIL_Token t)
{
    int err;
    int response[5];
    char * responseStr[5];
    ATResponse *p_response = NULL;
    const char *cmd;
    const char *prefix;
    char *line, *p;
    int commas;
    int skip;
    int count = 5;

    // Fixed values. TODO: Query modem
    responseStr[0] = "8587777777"; // MDN
    responseStr[1] = "1"; // SID
    responseStr[2] = "1"; // NID
    responseStr[3] = "8587777777"; // MIN
    responseStr[4] = "1"; // PRL Version
    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, count*sizeof(char*));
}

static void requestCdmaGetRoamingPreference(int request __unused, void *data __unused,
                                                 size_t datalen __unused, RIL_Token t)
{
    int roaming_pref = -1;
    ATResponse *p_response = NULL;
    char *line;
    int res;

    res = at_send_command_singleline("AT+WRMP?", "+WRMP:", &p_response);
    if (res < 0 || !p_response->success) {
        goto error;
    }
    line = p_response->p_intermediates->line;

    res = at_tok_start(&line);
    if (res < 0) goto error;

    res = at_tok_nextint(&line, &roaming_pref);
    if (res < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &roaming_pref, sizeof(roaming_pref));
    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaSetRoamingPreference(int request __unused, void *data,
                                                 size_t datalen __unused, RIL_Token t)
{
    int *pref = (int *)data;
    ATResponse *p_response = NULL;
    char *line;
    int res;
    char *cmd = NULL;

    asprintf(&cmd, "AT+WRMP=%d", *pref);
    if (cmd == NULL) goto error;

    res = at_send_command(cmd, &p_response);
    if (res < 0 || !p_response->success)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    free(cmd);
    return;
error:
    free(cmd);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static int parseRegistrationState(char *str, int *type, int *items, int **response)
{
    int err;
    char *line = str, *p;
    int *resp = NULL;
    int skip;
    int count = 3;
    int commas;

    RLOGD("parseRegistrationState. Parsing: %s",str);
    err = at_tok_start(&line);
    if (err < 0) goto error;

    /* Ok you have to be careful here
     * The solicited version of the CREG response is
     * +CREG: n, stat, [lac, cid]
     * and the unsolicited version is
     * +CREG: stat, [lac, cid]
     * The <n> parameter is basically "is unsolicited creg on?"
     * which it should always be
     *
     * Now we should normally get the solicited version here,
     * but the unsolicited version could have snuck in
     * so we have to handle both
     *
     * Also since the LAC and CID are only reported when registered,
     * we can have 1, 2, 3, or 4 arguments here
     *
     * finally, a +CGREG: answer may have a fifth value that corresponds
     * to the network type, as in;
     *
     *   +CGREG: n, stat [,lac, cid [,networkType]]
     */

    /* count number of commas */
    commas = 0;
    for (p = line ; *p != '\0' ;p++) {
        if (*p == ',') commas++;
    }

    resp = (int *)calloc(commas + 1, sizeof(int));
    if (!resp) goto error;
    switch (commas) {
        case 0: /* +CREG: <stat> */
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            resp[1] = -1;
            resp[2] = -1;
        break;

        case 1: /* +CREG: <n>, <stat> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            resp[1] = -1;
            resp[2] = -1;
            if (err < 0) goto error;
        break;

        case 2: /* +CREG: <stat>, <lac>, <cid> */
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[2]);
            if (err < 0) goto error;
        break;
        case 3: /* +CREG: <n>, <stat>, <lac>, <cid> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[2]);
            if (err < 0) goto error;
        break;
        /* special case for CGREG, there is a fourth parameter
         * that is the network type (unknown/gprs/edge/umts)
         */
        case 4: /* +CGREG: <n>, <stat>, <lac>, <cid>, <networkType> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[2]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[3]);
            if (err < 0) goto error;
            count = 4;
        break;
        default:
            goto error;
    }
    s_lac = resp[1];
    s_cid = resp[2];
    if (response)
        *response = resp;
    if (items)
        *items = commas + 1;
    if (type)
        *type = techFromModemType(TECH(sMdmInfo));
    return 0;
error:
    free(resp);
    return -1;
}

#define REG_STATE_LEN 15
#define REG_DATA_STATE_LEN 6
static void requestRegistrationState(int request, void *data __unused,
                                        size_t datalen __unused, RIL_Token t)
{
    int err;
    int *registration;
    char **responseStr = NULL;
    ATResponse *p_response = NULL;
    /* BEGIN Add get current radio tech for RegistrationState AT cmd by yangyao in 20220304 */
    ATResponse *p_zpas_response = NULL;
    char *radio_tech = NULL;
    /* END Add get current radio tech for RegistrationState AT cmd by yangyao in 20220304 */
    const char *cmd;
    const char *prefix;
    char *line;
    int i = 0, j, numElements = 0;
    int count = 3;
    int type, startfrom;

    RLOGD("requestRegistrationState");

    /* BEGIN Add get current radio tech for RegistrationState AT cmd by yangyao in 20220304 */
    err = at_send_command_singleline("AT+ZPAS?", "+ZPAS:", &p_zpas_response);
    if(err != 0 || p_zpas_response->success == 0 || p_zpas_response->p_intermediates == NULL) {
        RLOGD("exec AT+ZPAS? failure.");
        goto error;
    } else {
        line = p_zpas_response->p_intermediates->line;
        RLOGD("exec AT+ZPAS? successful and get response = %s.", line);
    }

    err = at_tok_start(&line);
    if(err < 0) {
        goto error;
    }

    err = at_tok_nextstr(&line, &radio_tech);
    if(err < 0) {
        goto error;
    }

    RLOGD("parse radio_tech = %s.", radio_tech);
    if (request == RIL_REQUEST_VOICE_REGISTRATION_STATE) {
        cmd = "AT+CREG?";
        prefix = "+CREG:";
        numElements = REG_STATE_LEN;
    } else if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
        if(strncmp(radio_tech, "LTE", strlen("LTE")) == 0) {
            cmd = "AT+CEREG?";
            prefix = "+CEREG:";
        } else {
            cmd = "AT+CGREG?";
            prefix = "+CGREG:";
        }
        numElements = REG_DATA_STATE_LEN;
    } else {
        assert(0);
        goto error;
    }

    /* END Add get current radio tech for RegistrationState AT cmd by yangyao in 20220304 */

    err = at_send_command_singleline(cmd, prefix, &p_response);

    /* BEGIN Add AT failure check by yangyao in 20220610 */
    if(err != 0 || p_response->success == 0 || p_response->p_intermediates == NULL) {
        RLOGD("exec %s failure.", cmd);
        goto error;
    } else {
        line = p_response->p_intermediates->line;
        RLOGD("exec %s successful and get response = %s.", cmd, line);
    }
    /* END Add AT failure check by yangyao in 20220610 */

    if (parseRegistrationState(line, &type, &count, &registration)) goto error;

    responseStr = malloc(numElements * sizeof(char *));
    if (!responseStr) goto error;
    memset(responseStr, 0, numElements * sizeof(char *));

    /* BEGIN Modify 3gpp radio tech by yangyao in 20220304 */
    if(count > 3) {
        registration[3] = convertRilToRadioAccessTech(radio_tech);
        if(registration[0] != 1) {
            registration[0] = 1;
        }
    }
    /* END Modify 3gpp radio tech by yangyao in 20220304 */
    /**
     * The first '4' bytes for both registration states remain the same.
     * But if the request is 'DATA_REGISTRATION_STATE',
     * the 5th and 6th byte(s) are optional.
     */
    /* BEGIN Modify 3gpp2 check by yangyao in 20220304 */
    if ((strncmp(radio_tech, "EVDO", strlen("EVDO")) == 0) ||
        (strncmp(radio_tech, "CDMA", strlen("CDMA")) == 0)) {
    /* END Modify 3gpp2 check by yangyao in 20220304 */
        RLOGD("registration state type: 3GPP2");
        // TODO: Query modem
        startfrom = 3;
        if(request == RIL_REQUEST_VOICE_REGISTRATION_STATE) {
            asprintf(&responseStr[3], "8");     // EvDo revA
            asprintf(&responseStr[4], "1");     // BSID
            asprintf(&responseStr[5], "123");   // Latitude
            asprintf(&responseStr[6], "222");   // Longitude
            asprintf(&responseStr[7], "0");     // CSS Indicator
            /* BEGIN Modify 3gpp2 SID and NID param by yangyao in 20220304 */
            asprintf(&responseStr[8], "%x", registration[1]);     // SID
            asprintf(&responseStr[9], "%x", registration[2]);     // NID
            /* END Modify 3gpp2 SID and NID param by yangyao in 20220304 */
            asprintf(&responseStr[10], "0");    // Roaming indicator
            asprintf(&responseStr[11], "1");    // System is in PRL
            asprintf(&responseStr[12], "0");    // Default Roaming indicator
            asprintf(&responseStr[13], "0");    // Reason for denial
            asprintf(&responseStr[14], "0");    // Primary Scrambling Code of Current cell
      } else if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
            asprintf(&responseStr[3], "8");   // Available data radio technology
      }
    } else { // type == RADIO_TECH_3GPP
        RLOGD("registration state type: 3GPP");
        startfrom = 0;
        asprintf(&responseStr[1], "%x", registration[1]);
        asprintf(&responseStr[2], "%x", registration[2]);
        if (count > 3)
            asprintf(&responseStr[3], "%d", registration[3]);
    }
    asprintf(&responseStr[0], "%d", registration[0]);

    /**
     * Optional bytes for DATA_REGISTRATION_STATE request
     * 4th byte : Registration denial code
     * 5th byte : The max. number of simultaneous Data Calls
     */
    if(request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
        // asprintf(&responseStr[4], "3");
        // asprintf(&responseStr[5], "1");
    }

    for (j = startfrom; j < numElements; j++) {
        if (!responseStr[i]) goto error;
    }
    free(registration);
    registration = NULL;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, numElements*sizeof(responseStr));
    for (j = 0; j < numElements; j++ ) {
        free(responseStr[j]);
        responseStr[j] = NULL;
    }
    free(responseStr);
    responseStr = NULL;
    at_response_free(p_response);
    /* BEGIN Add get current radio tech for RegistrationState AT cmd by yangyao in 20220304 */
    at_response_free(p_zpas_response);
    /* END Add get current radio tech for RegistrationState AT cmd by yangyao in 20220304 */

    return;
error:
    if (responseStr) {
        for (j = 0; j < numElements; j++) {
            free(responseStr[j]);
            responseStr[j] = NULL;
        }
        free(responseStr);
        responseStr = NULL;
    }
    RLOGE("requestRegistrationState must never return an error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    /* BEGIN Add get current radio tech for RegistrationState AT cmd by yangyao in 20220304 */
    at_response_free(p_zpas_response);
    /* END Add get current radio tech for RegistrationState AT cmd by yangyao in 20220304 */
}

static void requestOperator(void *data __unused, size_t datalen __unused, RIL_Token t)
{
    int err;
    int i;
    int skip;
    ATLine *p_cur;
    char *response[3];

    memset(response, 0, sizeof(response));

    ATResponse *p_response = NULL;

    err = at_send_command_multiline(
        "AT+COPS=3,0;+COPS?;+COPS=3,1;+COPS?;+COPS=3,2;+COPS?",
        "+COPS:", &p_response);

    /* we expect 3 lines here:
     * +COPS: 0,0,"T - Mobile"
     * +COPS: 0,1,"TMO"
     * +COPS: 0,2,"310170"
     */

    if (err != 0) goto error;

    for (i = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next, i++
    ) {
        char *line = p_cur->line;

        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        // If we're unregistered, we may just get
        // a "+COPS: 0" response
        if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
        }

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        // a "+COPS: 0, n" response is also possible
        if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
        }

        err = at_tok_nextstr(&line, &(response[i]));
        if (err < 0) goto error;
        /* BEGIN Deleted operator mnc mcc 3 digits by yangyao in 20220304 */
        /* END Deleted operator mnc mcc 3 digits by yangyao in 20220304 */
    }

    if (i != 3) {
        /* expect 3 lines exactly */
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
    at_response_free(p_response);

    return;
error:
    RLOGE("requestOperator must not return error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestCdmaSendSMS(void *data, size_t datalen, RIL_Token t)
{
    int err = 1; // Set to go to error:
    RIL_SMS_Response response;
    RIL_CDMA_SMS_Message* rcsm;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
        return;
    }

    RLOGD("requestCdmaSendSMS datalen=%zu, sizeof(RIL_CDMA_SMS_Message)=%zu",
            datalen, sizeof(RIL_CDMA_SMS_Message));

    // verify data content to test marshalling/unmarshalling:
    rcsm = (RIL_CDMA_SMS_Message*)data;
    RLOGD("TeleserviceID=%d, bIsServicePresent=%d, \
            uServicecategory=%d, sAddress.digit_mode=%d, \
            sAddress.Number_mode=%d, sAddress.number_type=%d, ",
            rcsm->uTeleserviceID,  rcsm->bIsServicePresent,
            rcsm->uServicecategory,rcsm->sAddress.digit_mode,
            rcsm->sAddress.number_mode,rcsm->sAddress.number_type);

    if (err != 0) goto error;

    // Cdma Send SMS implementation will go here:
    // But it is not implemented yet.

    memset(&response, 0, sizeof(response));
    response.messageRef = 1;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    return;

error:
    // Cdma Send SMS will always cause send retry error.
    response.messageRef = -1;
    RIL_onRequestComplete(t, RIL_E_SMS_SEND_FAIL_RETRY, &response, sizeof(response));
}

static void requestSendSMS(void *data, size_t datalen, RIL_Token t)
{
    int err;
    const char *smsc;
    const char *pdu;
    int tpLayerLength;
    char *cmd1, *cmd2;
    RIL_SMS_Response response;
    ATResponse *p_response = NULL;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
        return;
    }

    memset(&response, 0, sizeof(response));
    RLOGD("requestSendSMS datalen =%zu", datalen);

    if (s_ims_gsm_fail != 0) goto error;
    if (s_ims_gsm_retry != 0) goto error2;

    smsc = ((const char **)data)[0];
    pdu = ((const char **)data)[1];

    tpLayerLength = strlen(pdu)/2;

    // "NULL for default SMSC"
    if (smsc == NULL) {
        smsc= "00";
    }

    asprintf(&cmd1, "AT+CMGS=%d", tpLayerLength);
    asprintf(&cmd2, "%s%s", smsc, pdu);

    err = at_send_command_sms(cmd1, cmd2, "+CMGS:", &p_response);

    free(cmd1);
    free(cmd2);

    if (err != 0 || p_response->success == 0) goto error;

    /* FIXME fill in messageRef and ackPDU */
    response.messageRef = 1;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    at_response_free(p_response);

    return;
error:
    response.messageRef = -2;
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, &response, sizeof(response));
    at_response_free(p_response);
    return;
error2:
    // send retry error.
    response.messageRef = -1;
    RIL_onRequestComplete(t, RIL_E_SMS_SEND_FAIL_RETRY, &response, sizeof(response));
    at_response_free(p_response);
    return;
}

static void requestImsSendSMS(void *data, size_t datalen, RIL_Token t)
{
    RIL_IMS_SMS_Message *p_args;
    RIL_SMS_Response response;

    memset(&response, 0, sizeof(response));

    RLOGD("requestImsSendSMS: datalen=%zu, "
        "registered=%d, service=%d, format=%d, ims_perm_fail=%d, "
        "ims_retry=%d, gsm_fail=%d, gsm_retry=%d",
        datalen, s_ims_registered, s_ims_services, s_ims_format,
        s_ims_cause_perm_failure, s_ims_cause_retry, s_ims_gsm_fail,
        s_ims_gsm_retry);

    // figure out if this is gsm/cdma format
    // then route it to requestSendSMS vs requestCdmaSendSMS respectively
    p_args = (RIL_IMS_SMS_Message *)data;

    if (0 != s_ims_cause_perm_failure ) goto error;

    // want to fail over ims and this is first request over ims
    if (0 != s_ims_cause_retry && 0 == p_args->retry) goto error2;

    if (RADIO_TECH_3GPP == p_args->tech) {
        return requestSendSMS(p_args->message.gsmMessage,
                datalen - sizeof(RIL_RadioTechnologyFamily),
                t);
    } else if (RADIO_TECH_3GPP2 == p_args->tech) {
        return requestCdmaSendSMS(p_args->message.cdmaMessage,
                datalen - sizeof(RIL_RadioTechnologyFamily),
                t);
    } else {
        RLOGE("requestImsSendSMS invalid format value =%d", p_args->tech);
    }

error:
    response.messageRef = -2;
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, &response, sizeof(response));
    return;

error2:
    response.messageRef = -1;
    RIL_onRequestComplete(t, RIL_E_SMS_SEND_FAIL_RETRY, &response, sizeof(response));
}

static void requestSimOpenChannel(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    int32_t session_id;
    int err;
    char cmd[32];
    char dummy;
    char *line;

    // Max length is 16 bytes according to 3GPP spec 27.007 section 8.45
    if (data == NULL || datalen == 0 || datalen > 16) {
        ALOGE("Invalid data passed to requestSimOpenChannel");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    snprintf(cmd, sizeof(cmd), "AT+CCHO=%s", data);

    err = at_send_command_numeric(cmd, &p_response);
    if (err < 0 || p_response == NULL || p_response->success == 0) {
        ALOGE("Error %d opening logical channel: %d",
              err, p_response ? p_response->success : 0);
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        at_response_free(p_response);
        return;
    }

    // Ensure integer only by scanning for an extra char but expect one result
    line = p_response->p_intermediates->line;
    if (sscanf(line, "%" SCNd32 "%c", &session_id, &dummy) != 1) {
        ALOGE("Invalid AT response, expected integer, was '%s'", line);
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &session_id, sizeof(&session_id));
    at_response_free(p_response);
}

static void requestSimCloseChannel(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    int32_t session_id;
    int err;
    char cmd[32];

    if (data == NULL || datalen != sizeof(session_id)) {
        ALOGE("Invalid data passed to requestSimCloseChannel");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    session_id = ((int32_t *)data)[0];
    snprintf(cmd, sizeof(cmd), "AT+CCHC=%" PRId32, session_id);
    err = at_send_command_singleline(cmd, "+CCHC", &p_response);

    if (err < 0 || p_response == NULL || p_response->success == 0) {
        ALOGE("Error %d closing logical channel %d: %d",
              err, session_id, p_response ? p_response->success : 0);
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        at_response_free(p_response);
        return;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

    at_response_free(p_response);
}

static void requestSimTransmitApduChannel(void *data,
                                          size_t datalen,
                                          RIL_Token t)
{
    ATResponse *p_response = NULL;
    int err;
    char *cmd;
    char *line;
    size_t cmd_size;
    RIL_SIM_IO_Response sim_response;
    RIL_SIM_APDU *apdu = (RIL_SIM_APDU *)data;

    if (apdu == NULL || datalen != sizeof(RIL_SIM_APDU)) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    cmd_size = 10 + (apdu->data ? strlen(apdu->data) : 0);
    asprintf(&cmd, "AT+CGLA=%d,%zu,%02x%02x%02x%02x%02x%s",
             apdu->sessionid, cmd_size, apdu->cla, apdu->instruction,
             apdu->p1, apdu->p2, apdu->p3, apdu->data ? apdu->data : "");

    err = at_send_command_singleline(cmd, "+CGLA", &p_response);
    free(cmd);
    if (err < 0 || p_response == NULL || p_response->success == 0) {
        ALOGE("Error %d transmitting APDU: %d",
              err, p_response ? p_response->success : 0);
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        at_response_free(p_response);
        return;
    }

    line = p_response->p_intermediates->line;
    err = parseSimResponseLine(line, &sim_response);

    if (err == 0) {
        RIL_onRequestComplete(t, RIL_E_SUCCESS,
                              &sim_response, sizeof(sim_response));
    } else {
        ALOGE("Error %d parsing SIM response line: %s", err, line);
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }
    at_response_free(p_response);
}

/* BEGIN modify requestSetupDataCall by leikai in 20220315 */
static void requestSetupDataCall(void *data, size_t datalen, RIL_Token t)
{
    RLOGD("%s entry", __func__);
    const char *pdpid_s = NULL;
    int pdp_id;
    const char *apn = NULL;
    const char *user = NULL;
    const char *pass = NULL;
    const char *auth_type = "0";
    const char *pdp_type = "IP";
    int err;
    char *line = NULL;
    ATResponse *p_response = NULL;
    char *networktype = NULL;
    char *domain = NULL;
    char *port[] = {IF_NAME0, IF_NAME1, IF_NAME2};
    int if_id[] = {MB_FIRST_INTERFACE, MB_SEC_INTERFACE, MB_THIRD_INTERFACE};
    static ResponseParam para;

    pdpid_s = ((const char **)data)[1];
    apn = ((const char **)data)[2];
    if (datalen > 3 * sizeof(char *))
        user = ((char **)data)[3];
    if (datalen > 4 * sizeof(char *))
        pass = ((char **)data)[4];
    if (datalen > 5 * sizeof(char *))
        auth_type = ((const char **)data)[5]; // 0 ~ NONE, 1 ~ PAP, 1 ~ CHAP, 3 ~ PAP / CHAP
    if (datalen > 6 * sizeof(char *))
        pdp_type = ((const char **)data)[6];


    if (!pdpid_s) {
        RLOGE("param error, pdp_id is null.");
        goto error;
    }

    RLOGD("pdp_id:%s,apn:%s,user:%s,pass:%s,auth_type:%s,pdp_type :%s", pdpid_s, apn, user, pass, auth_type, pdp_type);
    pdp_id = atoi(pdpid_s);
    if((pdp_id < MIN_PROFILE_ID) || (pdp_id > MAX_PROFILE_ID)) {
        RLOGE("pdp_id is invalid.");
        goto error;
    }

    err = at_send_command_singleline("AT+ZPAS?", "+ZPAS:", &p_response);
    if (err < 0 || p_response->success == 0 || p_response->p_intermediates == NULL) {
        goto error;
    }
    line = p_response->p_intermediates->line;

    /* we expect response such as "CS_PS", but sometimes it return limited service. */
    err = at_tok_start(&line);
    if (err < 0) goto error;
    err = at_tok_nextstr(&line, &networktype);
    if (err < 0) goto error;

    /* must register ps domain */
    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &domain);
        if (err < 0) goto error;

        if (strcmp(domain,"CS_PS") && strcmp(domain,"PS_ONLY") && strcmp(domain,"cs_ps") && strcmp(domain,"ps_only")) {
            RLOGD("not register ps domain:%s", domain);
            goto error;
        }
    }

    if (gobinet_start(pdp_id, pdp_type, apn, user, pass, auth_type, port[pdp_id], if_id[pdp_id]) < 0) {
        goto error;
    }
    at_response_free(p_response);

    memset(&para, 0, sizeof(para));
    memcpy(para.port, port[pdp_id], strlen(port[pdp_id]));
    para.pdn_id = pdp_id;
    para.if_id = if_id[pdp_id];
    para.t = t;
    memcpy(para.protocol, pdp_type, strlen(pdp_type));
    RIL_requestTimedCallback(ReadResponse, (void *)&para, NULL);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}
/* END modify requestSetupDataCall by leikai in 20220315 */

/* BEGIN modify requestDeactivateDataCall by leikai in 20220315 */
static void requestDeactivateDataCall(void *data, size_t datalen, RIL_Token t)
{
    RLOGD("%s entry", __func__);
    char *pdpid_s = ((char **)data)[0];
    int pdp_id = atoi(pdpid_s);
    char *port[] = {"", IF_NAME0, IF_NAME1, IF_NAME2};
    char cmd[50] = {0};

    if (gobinet_stop(pdp_id) < 0) {
        goto error;
    }

    sprintf(cmd, "/system/bin/ifconfig %s down", port[pdp_id]);
    int err = system(cmd);
    RLOGD("cmd:%s, err:%d", cmd, err);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}
/* END modify requestDeactivateDataCall by leikai in 20220315 */

static void requestSMSAcknowledge(void *data, size_t datalen __unused, RIL_Token t)
{
    /* BEGIN Modify when received SMS and deleted by yangyao in 20220318 */
    int err;

    err = deleteSmsByIndex(m_Index);
    if(err == 0) {
        goto error;
    }
    /* END Modify when received SMS and deleted by yangyao in 20220318 */

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void  requestSIM_IO(void *data, size_t datalen __unused, RIL_Token t)
{
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response sr;
    int err;
    char *cmd = NULL;
    RIL_SIM_IO_v6 *p_args;
    char *line;

    memset(&sr, 0, sizeof(sr));

    p_args = (RIL_SIM_IO_v6 *)data;

    /* FIXME handle pin2 */

    if (p_args->data == NULL) {
        asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d",
                    p_args->command, p_args->fileid,
                    p_args->p1, p_args->p2, p_args->p3);
    } else {
        asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d,%s",
                    p_args->command, p_args->fileid,
                    p_args->p1, p_args->p2, p_args->p3, p_args->data);
    }

    err = at_send_command_singleline(cmd, "+CRSM:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = parseSimResponseLine(line, &sr);
    if (err < 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
    at_response_free(p_response);
    free(cmd);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    free(cmd);

}

static void  requestEnterSimPin(void*  data, size_t  datalen, RIL_Token  t)
{
    ATResponse   *p_response = NULL;
    int           err;
    char*         cmd = NULL;
    const char**  strings = (const char**)data;;

    if ( datalen == sizeof(char*) ) {
        asprintf(&cmd, "AT+CPIN=%s", strings[0]);
    } else if ( datalen == 2*sizeof(char*) ) {
        asprintf(&cmd, "AT+CPIN=%s,%s", strings[0], strings[1]);
    } else
        goto error;

    err = at_send_command_singleline(cmd, "+CPIN:", &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
error:
        RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}


static void  requestSendUSSD(void *data, size_t datalen __unused, RIL_Token t)
{
    const char *ussdRequest;

    ussdRequest = (char *)(data);


    RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);

// @@@ TODO

}

static void requestExitEmergencyMode(void *data __unused, size_t datalen __unused, RIL_Token t)
{
    int err;
    ATResponse *p_response = NULL;

    err = at_send_command("AT+WSOS=0", &p_response);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

// TODO: Use all radio types
static int techFromModemType(int mdmtype)
{
    int ret = -1;
    switch (1 << mdmtype) {
        case MDM_CDMA:
            ret = RADIO_TECH_1xRTT;
            break;
        case MDM_EVDO:
            ret = RADIO_TECH_EVDO_A;
            break;
        case MDM_GSM:
            ret = RADIO_TECH_GPRS;
            break;
        case MDM_WCDMA:
            ret = RADIO_TECH_HSPA;
            break;
        case MDM_LTE:
            ret = RADIO_TECH_LTE;
            break;
    }
    return ret;
}

static void requestGetCellInfoList(void *data __unused, size_t datalen __unused, RIL_Token t)
{
    /* BEGIN Modify get cell info request by yangyao in 20220316 */
    RIL_CellInfo_v12 ci = {0};

    if(getCellInformation(&ci) == FALSE) {
        RLOGD("requestGetCellInfoList: getCellInformation return FALSE.");
        goto error;
    } else {
        RLOGD("requestGetCellInfoList: getCellInformation return TRUE.");
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &ci, sizeof(ci));
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    return;
    /* END Modify get cell info request by yangyao in 20220316 */
}


static void requestSetCellInfoListRate(void *data, size_t datalen __unused, RIL_Token t)
{
    // For now we'll save the rate but no RIL_UNSOL_CELL_INFO_LIST messages
    // will be sent.
    assert (datalen == sizeof(int));
    s_cell_info_rate_ms = ((int *)data)[0];

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestGetHardwareConfig(void *data, size_t datalen, RIL_Token t)
{
   // TODO - hook this up with real query/info from radio.

   RIL_HardwareConfig hwCfg;

   RIL_UNUSED_PARM(data);
   RIL_UNUSED_PARM(datalen);

   hwCfg.type = -1;

   RIL_onRequestComplete(t, RIL_E_SUCCESS, &hwCfg, sizeof(hwCfg));
}

static void requestGetTtyMode(void *data, size_t datalen, RIL_Token t)
{
   int  ttyModeResponse;

   RIL_UNUSED_PARM(data);
   RIL_UNUSED_PARM(datalen);

   ttyModeResponse = (getSIMStatus() == SIM_READY) ? 1  // TTY Full
                                                   : 0; // TTY Off

   RIL_onRequestComplete(t, RIL_E_SUCCESS, &ttyModeResponse, sizeof(ttyModeResponse));
}

static void requestGetRadioCapability(void *data, size_t datalen, RIL_Token t)
{
   RIL_RadioCapability radioCapability;

   RIL_UNUSED_PARM(data);
   RIL_UNUSED_PARM(datalen);

   radioCapability.version = RIL_RADIO_CAPABILITY_VERSION;
   radioCapability.session = 0;
   radioCapability.phase   = 0;
   radioCapability.rat     = 0;
   radioCapability.logicalModemUuid[0] = '\0';
   radioCapability.status  = RC_STATUS_SUCCESS;

   RIL_onRequestComplete(t, RIL_E_SUCCESS, &radioCapability, sizeof(radioCapability));
}

static void requestGetMute(void *data, size_t datalen, RIL_Token t)
{
   int  muteResponse;

   RIL_UNUSED_PARM(data);
   RIL_UNUSED_PARM(datalen);

   muteResponse = 0; // Mute disabled

   RIL_onRequestComplete(t, RIL_E_SUCCESS, &muteResponse, sizeof(muteResponse));
}

static void requestSetInitialAttachApn(void *data, size_t datalen, RIL_Token t)
{
    RLOGD("%s entry", __func__);
    ATResponse *p_response = NULL;
    RIL_InitialAttachApn *apn = data;
    char cmd[64] = {0};

    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"%s\",\"%s\"", apn->protocol, apn->apn);
    int err = at_send_command(cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}

/*** Callback methods from the RIL library to us ***/

/**
 * Call from RIL to us to make a RIL_REQUEST
 *
 * Must be completed with a call to RIL_onRequestComplete()
 *
 * RIL_onRequestComplete() may be called from any thread, before or after
 * this function returns.
 *
 * Because onRequest function could be called from multiple different thread,
 * we must ensure that the underlying at_send_command_* function
 * is atomic.
 */
static void
onRequest (int request, void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response;
    /* BEGIN Add not response ril request when ril initial is not finish by yangyao in 20220525 */
    int ril_inited_wait_times = 0;
    /* END Add not response ril request when ril initial is not finish by yangyao in 20220525 */
    int err;

    RLOGD("onRequest: %s, sState: %d", requestToString(request), sState);

    /* BEGIN Add RIL Requests are valid when RADIO_STATE_UNAVAILABLE by yangyao in 20220304 */
    /* Ignore all requests except RIL_REQUEST_GET_SIM_STATUS
     * when RADIO_STATE_UNAVAILABLE.
     */
    if (sState == RADIO_STATE_UNAVAILABLE) {
        switch(request) {
            case RIL_REQUEST_GET_SIM_STATUS:
            case RIL_REQUEST_GET_IMEI:
            case RIL_REQUEST_GET_IMSI:
            case RIL_REQUEST_GET_RADIO_CAPABILITY:
            case RIL_REQUEST_DEVICE_IDENTITY:
            case RIL_REQUEST_BASEBAND_VERSION:
                break;

            default:
                RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
                return;
        }
    }
    /* END Add RIL Requests are valid when RADIO_STATE_UNAVAILABLE by yangyao in 20220304 */

    /* Ignore all non-power requests when RADIO_STATE_OFF
     * (except RIL_REQUEST_GET_SIM_STATUS)
     */
    if (sState == RADIO_STATE_OFF) {
        switch(request) {
            case RIL_REQUEST_BASEBAND_VERSION:
            case RIL_REQUEST_CDMA_GET_SUBSCRIPTION_SOURCE:
            case RIL_REQUEST_CDMA_QUERY_PREFERRED_VOICE_PRIVACY_MODE:
            case RIL_REQUEST_CDMA_SET_PREFERRED_VOICE_PRIVACY_MODE:
            case RIL_REQUEST_CDMA_SET_ROAMING_PREFERENCE:
            case RIL_REQUEST_CDMA_SET_SUBSCRIPTION_SOURCE:
            case RIL_REQUEST_CDMA_SUBSCRIPTION:
            case RIL_REQUEST_DEVICE_IDENTITY:
            case RIL_REQUEST_EXIT_EMERGENCY_CALLBACK_MODE:
            case RIL_REQUEST_GET_ACTIVITY_INFO:
            case RIL_REQUEST_GET_CARRIER_RESTRICTIONS:
            case RIL_REQUEST_GET_CURRENT_CALLS:
            case RIL_REQUEST_GET_IMEI:
            case RIL_REQUEST_GET_MUTE:
            case RIL_REQUEST_GET_NEIGHBORING_CELL_IDS:
            case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
            case RIL_REQUEST_GET_RADIO_CAPABILITY:
            case RIL_REQUEST_GET_SIM_STATUS:
            /* BEGIN Add RIL Requests are valid when RADIO_STATE_OFF by yangyao in 20220304 */
            case RIL_REQUEST_GET_IMSI:
            /* END Add RIL Requests are valid when RADIO_STATE_OFF by yangyao in 20220304 */
            case RIL_REQUEST_NV_RESET_CONFIG:
            case RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE:
            case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
            case RIL_REQUEST_QUERY_TTY_MODE:
            case RIL_REQUEST_RADIO_POWER:
            case RIL_REQUEST_SET_BAND_MODE:
            case RIL_REQUEST_SET_CARRIER_RESTRICTIONS:
            case RIL_REQUEST_SET_LOCATION_UPDATES:
            case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
            case RIL_REQUEST_SET_TTY_MODE:
            case RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE:
            case RIL_REQUEST_STOP_LCE:
            case RIL_REQUEST_VOICE_RADIO_TECH:
                // Process all the above, even though the radio is off
                break;

            default:
                // For all others, say NOT_AVAILABLE because the radio is off
                RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
                return;
        }
    }

    /* BEGIN Add not response ril request when ril initial is not finish by yangyao in 20220525 */
    while((g_is_rild_inited != TRUE) && (ril_inited_wait_times < MAX_WAIT_RIL_INITIAL_TIMES))
    {
        RLOGD("onRequest: g_is_rild_inited = %d and wait_times = %d, wait for 1s retry.", g_is_rild_inited, ril_inited_wait_times);

        ril_inited_wait_times++;
        sleep(1);
    }

    if(g_is_rild_inited != TRUE)
    {
        RLOGD("onRequest: initialCallback is not finished.");
        return;
    }
    /* END Add not response ril request when ril initial is not finish by yangyao in 20220525 */

    switch (request) {
        case RIL_REQUEST_GET_SIM_STATUS: {
            RIL_CardStatus_v6 *p_card_status;
            char *p_buffer;
            int buffer_size;

            int result = getCardStatus(&p_card_status);
            if (result == RIL_E_SUCCESS) {
                p_buffer = (char *)p_card_status;
                buffer_size = sizeof(*p_card_status);
            } else {
                p_buffer = NULL;
                buffer_size = 0;
            }
            RIL_onRequestComplete(t, result, p_buffer, buffer_size);
            freeCardStatus(p_card_status);
            break;
        }
        case RIL_REQUEST_GET_CURRENT_CALLS:
            requestGetCurrentCalls(data, datalen, t);
            break;
        case RIL_REQUEST_DIAL:
            requestDial(data, datalen, t);
            break;
        case RIL_REQUEST_HANGUP:
            requestHangup(data, datalen, t);
            break;
        case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND:
        case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
        case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE:
        case RIL_REQUEST_CONFERENCE:
        case RIL_REQUEST_UDUB:
             requestCallSelection(data, datalen, t, request);
             break;
        case RIL_REQUEST_ANSWER:
            at_send_command("ATA", NULL);

#ifdef WORKAROUND_ERRONEOUS_ANSWER
            s_expectAnswer = 1;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

            if (getSIMStatus() != SIM_READY) {
                RIL_onRequestComplete(t, RIL_E_MODEM_ERR, NULL, 0);
            } else {
                // Success or failure is ignored by the upper layer here.
                // It will call GET_CURRENT_CALLS and determine success that way.
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            break;

        case RIL_REQUEST_SEPARATE_CONNECTION:
            {
                char  cmd[12];
                int   party = ((int*)data)[0];

                if (getSIMStatus() == SIM_ABSENT) {
                    RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
                    return;
                }
                // Make sure that party is in a valid range.
                // (Note: The Telephony middle layer imposes a range of 1 to 7.
                // It's sufficient for us to just make sure it's single digit.)
                if (party > 0 && party < 10) {
                    sprintf(cmd, "AT+CHLD=2%d", party);
                    at_send_command(cmd, NULL);
                    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
                } else {
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                }
            }
            break;

        case RIL_REQUEST_SIGNAL_STRENGTH:
            requestSignalStrength(data, datalen, t);
            break;
        case RIL_REQUEST_VOICE_REGISTRATION_STATE:
        case RIL_REQUEST_DATA_REGISTRATION_STATE:
            requestRegistrationState(request, data, datalen, t);
            break;
        case RIL_REQUEST_OPERATOR:
            requestOperator(data, datalen, t);
            break;
        case RIL_REQUEST_RADIO_POWER:
            requestRadioPower(data, datalen, t);
            break;
        case RIL_REQUEST_DTMF: {
            char c = ((char *)data)[0];
            char *cmd;
            asprintf(&cmd, "AT+VTS=%c", (int)c);
            at_send_command(cmd, NULL);
            free(cmd);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        }
        case RIL_REQUEST_SEND_SMS:
        case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
            requestSendSMS(data, datalen, t);
            break;
        case RIL_REQUEST_CDMA_SEND_SMS:
            requestCdmaSendSMS(data, datalen, t);
            break;
        case RIL_REQUEST_IMS_SEND_SMS:
            requestImsSendSMS(data, datalen, t);
            break;
        case RIL_REQUEST_SIM_OPEN_CHANNEL:
            requestSimOpenChannel(data, datalen, t);
            break;
        case RIL_REQUEST_SIM_CLOSE_CHANNEL:
            requestSimCloseChannel(data, datalen, t);
            break;
        case RIL_REQUEST_SIM_TRANSMIT_APDU_CHANNEL:
            requestSimTransmitApduChannel(data, datalen, t);
            break;
        case RIL_REQUEST_SETUP_DATA_CALL:
            requestSetupDataCall(data, datalen, t);
            break;
        case RIL_REQUEST_DEACTIVATE_DATA_CALL:
            /* BEGIN modify DEACTIVATE param by leikai in 20220315 */
            requestDeactivateDataCall(data, datalen, t);
            /* END modify DEACTIVATE param by leikai in 20220315 */
            break;
        case RIL_REQUEST_SMS_ACKNOWLEDGE:
            requestSMSAcknowledge(data, datalen, t);
            break;

        case RIL_REQUEST_GET_IMSI:
            p_response = NULL;
            err = at_send_command_numeric("AT+CIMI", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }

            break;

        case RIL_REQUEST_GET_IMEI:
            p_response = NULL;
            err = at_send_command_numeric("AT+CGSN", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }
            at_response_free(p_response);
            break;

        case RIL_REQUEST_SIM_IO:
            requestSIM_IO(data,datalen,t);
            break;

        case RIL_REQUEST_SEND_USSD:
            requestSendUSSD(data, datalen, t);
            break;

        case RIL_REQUEST_CANCEL_USSD:
            if (getSIMStatus() == SIM_ABSENT) {
                RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
                return;
            }
            p_response = NULL;
            err = at_send_command_numeric("AT+CUSD=2", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }
            at_response_free(p_response);
            break;

        case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC:
            if (getSIMStatus() == SIM_ABSENT) {
                RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
            } else {
                at_send_command("AT+COPS=0", NULL);
            }
            break;

        case RIL_REQUEST_DATA_CALL_LIST:
            requestDataCallList(data, datalen, t);
            break;

        case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
            requestQueryNetworkSelectionMode(data, datalen, t);
            break;

        case RIL_REQUEST_OEM_HOOK_RAW:
            // echo back data
            RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
            break;


        case RIL_REQUEST_OEM_HOOK_STRINGS: {
            int i;
            const char ** cur;

            RLOGD("got OEM_HOOK_STRINGS: 0x%8p %lu", data, (long)datalen);


            for (i = (datalen / sizeof (char *)), cur = (const char **)data ;
                    i > 0 ; cur++, i --) {
                RLOGD("> '%s'", *cur);
            }

            // echo back strings
            RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
            break;
        }

        case RIL_REQUEST_WRITE_SMS_TO_SIM:
            requestWriteSmsToSim(data, datalen, t);
            break;

        case RIL_REQUEST_DELETE_SMS_ON_SIM: {
            char * cmd;
            p_response = NULL;
            asprintf(&cmd, "AT+CMGD=%d", ((int *)data)[0]);
            err = at_send_command(cmd, &p_response);
            free(cmd);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }

        case RIL_REQUEST_ENTER_SIM_PIN:
        case RIL_REQUEST_ENTER_SIM_PUK:
        case RIL_REQUEST_ENTER_SIM_PIN2:
        case RIL_REQUEST_ENTER_SIM_PUK2:
        case RIL_REQUEST_CHANGE_SIM_PIN:
        case RIL_REQUEST_CHANGE_SIM_PIN2:
            requestEnterSimPin(data, datalen, t);
            break;

        case RIL_REQUEST_IMS_REGISTRATION_STATE: {
            int reply[2];

            getimsstate(&s_ims_registered, &s_ims_format);

            //0==unregistered, 1==registered
            reply[0] = s_ims_registered;

            //to be used when changed to include service supporated info
            //reply[1] = s_ims_services;

            // FORMAT_3GPP(1) vs FORMAT_3GPP2(2);
            reply[1] = s_ims_format;

            RLOGD("IMS_REGISTRATION=%d, format=%d ",
                    reply[0], reply[1]);
            if (reply[1] != -1) {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, reply, sizeof(reply));
            } else {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            }
            break;
        }

        case RIL_REQUEST_VOICE_RADIO_TECH:
            {
                int tech = techFromModemType(TECH(sMdmInfo));
                if (tech < 0 )
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                else
                    RIL_onRequestComplete(t, RIL_E_SUCCESS, &tech, sizeof(tech));
            }
            break;
        case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
            requestSetPreferredNetworkType(request, data, datalen, t);
            break;

        case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
            requestGetPreferredNetworkType(request, data, datalen, t);
            break;

        case RIL_REQUEST_GET_CELL_INFO_LIST:
            requestGetCellInfoList(data, datalen, t);
            break;

        case RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE:
            requestSetCellInfoListRate(data, datalen, t);
            break;

        case RIL_REQUEST_GET_HARDWARE_CONFIG:
            requestGetHardwareConfig(data, datalen, t);
            break;

        case RIL_REQUEST_SHUTDOWN:
            requestShutdown(t);
            break;

        case RIL_REQUEST_QUERY_TTY_MODE:
            requestGetTtyMode(data, datalen, t);
            break;

        case RIL_REQUEST_GET_RADIO_CAPABILITY:
            requestGetRadioCapability(data, datalen, t);
            break;

        case RIL_REQUEST_GET_MUTE:
            requestGetMute(data, datalen, t);
            break;

        case RIL_REQUEST_SET_INITIAL_ATTACH_APN:
            requestSetInitialAttachApn(data, datalen, t);
            break;

        case RIL_REQUEST_ALLOW_DATA:
        case RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION:
        case RIL_REQUEST_SET_CLIR:
        case RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION:
        case RIL_REQUEST_SET_BAND_MODE:
        case RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE:
        case RIL_REQUEST_GET_NEIGHBORING_CELL_IDS:
        case RIL_REQUEST_SET_LOCATION_UPDATES:
        case RIL_REQUEST_CDMA_SET_PREFERRED_VOICE_PRIVACY_MODE:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        /* BEGIN RILJ is initial and unsolicited SMS when SET_TTY_MODE sent by yangyao in 20220322 */
        case RIL_REQUEST_SET_TTY_MODE:
            pthread_mutex_lock(&g_init_mutex);
            if(g_is_rilj_inited != TRUE) {
                g_is_rilj_inited = TRUE;

                RLOGD("initializeCallback: g_is_rild_inited = %d, g_is_rilj_inited = %d", g_is_rild_inited, g_is_rilj_inited);
                if(g_is_rild_inited == TRUE) {
                   RIL_requestTimedCallback(unsolicitedUnreadSms, NULL, NULL);
                }
            }

            pthread_mutex_unlock(&g_init_mutex);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        /* END RILJ is initial and unsolicited SMS when SET_TTY_MODE sent by yangyao in 20220322 */

        case RIL_REQUEST_BASEBAND_VERSION:
            requestCdmaBaseBandVersion(request, data, datalen, t);
            break;

        case RIL_REQUEST_DEVICE_IDENTITY:
            requestDeviceIdentity(request, data, datalen, t);
            break;

        case RIL_REQUEST_CDMA_SUBSCRIPTION:
            requestCdmaSubscription(request, data, datalen, t);
            break;

        case RIL_REQUEST_CDMA_GET_SUBSCRIPTION_SOURCE:
            requestCdmaGetSubscriptionSource(request, data, datalen, t);
            break;

        case RIL_REQUEST_START_LCE:
        case RIL_REQUEST_STOP_LCE:
        case RIL_REQUEST_PULL_LCEDATA:
            if (getSIMStatus() == SIM_ABSENT) {
                RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_LCE_NOT_SUPPORTED, NULL, 0);
            }
            break;

        case RIL_REQUEST_CDMA_QUERY_ROAMING_PREFERENCE:
            if (TECH_BIT(sMdmInfo) == MDM_CDMA) {
                requestCdmaGetRoamingPreference(request, data, datalen, t);
            } else {
                RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
            }
            break;

        case RIL_REQUEST_CDMA_SET_SUBSCRIPTION_SOURCE:
            if (TECH_BIT(sMdmInfo) == MDM_CDMA) {
                requestCdmaSetSubscriptionSource(request, data, datalen, t);
            } else {
                // VTS tests expect us to silently do nothing
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            break;

        case RIL_REQUEST_CDMA_SET_ROAMING_PREFERENCE:
            if (TECH_BIT(sMdmInfo) == MDM_CDMA) {
                requestCdmaSetRoamingPreference(request, data, datalen, t);
            } else {
                // VTS tests expect us to silently do nothing
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            break;

        case RIL_REQUEST_EXIT_EMERGENCY_CALLBACK_MODE:
            if (TECH_BIT(sMdmInfo) == MDM_CDMA) {
                requestExitEmergencyMode(data, datalen, t);
            } else {
                // VTS tests expect us to silently do nothing
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            break;

        default:
            RLOGD("Request not supported. Tech: %d",TECH(sMdmInfo));
            RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
            break;
    }
}

/**
 * Synchronous call from the RIL to us to return current radio state.
 * RADIO_STATE_UNAVAILABLE should be the initial state.
 */
static RIL_RadioState
currentState()
{
    return sState;
}
/**
 * Call from RIL to us to find out whether a specific request code
 * is supported by this implementation.
 *
 * Return 1 for "supported" and 0 for "unsupported"
 */

static int
onSupports (int requestCode __unused)
{
    //@@@ todo

    return 1;
}

static void onCancel (RIL_Token t __unused)
{
    //@@@todo

}

static const char * getVersion(void)
{
    return "android reference-ril 1.0";
}

static void
setRadioTechnology(ModemInfo *mdm, int newtech)
{
    RLOGD("setRadioTechnology(%d)", newtech);

    int oldtech = TECH(mdm);

    if (newtech != oldtech) {
        RLOGD("Tech change (%d => %d)", oldtech, newtech);
        TECH(mdm) = newtech;
        if (techFromModemType(newtech) != techFromModemType(oldtech)) {
            int tech = techFromModemType(TECH(sMdmInfo));
            if (tech > 0 ) {
                RIL_onUnsolicitedResponse(RIL_UNSOL_VOICE_RADIO_TECH_CHANGED,
                                          &tech, sizeof(tech));
            }
        }
    }
}

static void
setRadioState(RIL_RadioState newState)
{
    RLOGD("setRadioState(%d)", newState);
    RIL_RadioState oldState;

    pthread_mutex_lock(&s_state_mutex);

    oldState = sState;

    if (s_closed > 0) {
        // If we're closed, the only reasonable state is
        // RADIO_STATE_UNAVAILABLE
        // This is here because things on the main thread
        // may attempt to change the radio state after the closed
        // event happened in another thread
        newState = RADIO_STATE_UNAVAILABLE;
    }

    if (sState != newState || s_closed > 0) {
        sState = newState;

        pthread_cond_broadcast (&s_state_cond);
    }

    pthread_mutex_unlock(&s_state_mutex);


    /* do these outside of the mutex */
    if (sState != oldState) {
        RIL_onUnsolicitedResponse (RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
                                    NULL, 0);
        /* BEGIN Delete onUnsolicited RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED when AT+CFUN=4 by yangyao in 20220304 */
        /* END Delete onUnsolicited RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED when AT+CFUN=4 by yangyao in 20220304 */

        /* FIXME onSimReady() and onRadioPowerOn() cannot be called
         * from the AT reader thread
         * Currently, this doesn't happen, but if that changes then these
         * will need to be dispatched on the request thread
         */
        if (sState == RADIO_STATE_ON) {
            onRadioPowerOn();
        }
    }
}

/** Returns RUIM_NOT_READY on error */
static SIM_Status
getRUIMStatus()
{
    ATResponse *p_response = NULL;
    int err;
    int ret;
    char *cpinLine;
    char *cpinResult;

    if (sState == RADIO_STATE_OFF || sState == RADIO_STATE_UNAVAILABLE) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_send_command_singleline("AT+CPIN?", "+CPIN:", &p_response);

    if (err != 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    switch (at_get_cme_error(p_response)) {
        case CME_SUCCESS:
            break;

        case CME_SIM_NOT_INSERTED:
            ret = SIM_ABSENT;
            goto done;

        default:
            ret = SIM_NOT_READY;
            goto done;
    }

    /* CPIN? has succeeded, now look at the result */

    cpinLine = p_response->p_intermediates->line;
    err = at_tok_start (&cpinLine);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_tok_nextstr(&cpinLine, &cpinResult);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    if (0 == strcmp (cpinResult, "SIM PIN")) {
        ret = SIM_PIN;
        goto done;
    } else if (0 == strcmp (cpinResult, "SIM PUK")) {
        ret = SIM_PUK;
        goto done;
    } else if (0 == strcmp (cpinResult, "PH-NET PIN")) {
        return SIM_NETWORK_PERSONALIZATION;
    } else if (0 != strcmp (cpinResult, "READY"))  {
        /* we're treating unsupported lock types as "sim absent" */
        ret = SIM_ABSENT;
        goto done;
    }

    at_response_free(p_response);
    p_response = NULL;
    cpinResult = NULL;

    ret = SIM_READY;

done:
    at_response_free(p_response);
    return ret;
}

/** Returns SIM_NOT_READY on error */
static SIM_Status
getSIMStatus()
{
    ATResponse *p_response = NULL;
    int err;
    int ret;
    char *cpinLine;
    char *cpinResult;

    RLOGD("getSIMStatus(). sState: %d",sState);
    err = at_send_command_singleline("AT+CPIN?", "+CPIN:", &p_response);

    if (err != 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    switch (at_get_cme_error(p_response)) {
        case CME_SUCCESS:
            break;

        case CME_SIM_NOT_INSERTED:
            ret = SIM_ABSENT;
            goto done;

        default:
            ret = SIM_NOT_READY;
            goto done;
    }

    /* CPIN? has succeeded, now look at the result */

    cpinLine = p_response->p_intermediates->line;
    err = at_tok_start (&cpinLine);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_tok_nextstr(&cpinLine, &cpinResult);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    if (0 == strcmp (cpinResult, "SIM PIN")) {
        ret = SIM_PIN;
        goto done;
    } else if (0 == strcmp (cpinResult, "SIM PUK")) {
        ret = SIM_PUK;
        goto done;
    } else if (0 == strcmp (cpinResult, "PH-NET PIN")) {
        return SIM_NETWORK_PERSONALIZATION;
    } else if (0 != strcmp (cpinResult, "READY"))  {
        /* we're treating unsupported lock types as "sim absent" */
        ret = SIM_ABSENT;
        goto done;
    }

    at_response_free(p_response);
    p_response = NULL;
    cpinResult = NULL;

    ret = (sState == RADIO_STATE_ON) ? SIM_READY : SIM_NOT_READY;

done:
    at_response_free(p_response);
    return ret;
}


/**
 * Get the current card status.
 *
 * This must be freed using freeCardStatus.
 * @return: On success returns RIL_E_SUCCESS
 */
static int getCardStatus(RIL_CardStatus_v6 **pp_card_status) {
    static RIL_AppStatus app_status_array[] = {
        // SIM_ABSENT = 0
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_NOT_READY = 1
        { RIL_APPTYPE_USIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_READY = 2
        { RIL_APPTYPE_USIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_PIN = 3
        { RIL_APPTYPE_USIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // SIM_PUK = 4
        { RIL_APPTYPE_USIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // SIM_NETWORK_PERSONALIZATION = 5
        { RIL_APPTYPE_USIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // RUIM_ABSENT = 6
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // RUIM_NOT_READY = 7
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // RUIM_READY = 8
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // RUIM_PIN = 9
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // RUIM_PUK = 10
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // RUIM_NETWORK_PERSONALIZATION = 11
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
           NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // ISIM_ABSENT = 12
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // ISIM_NOT_READY = 13
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // ISIM_READY = 14
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // ISIM_PIN = 15
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // ISIM_PUK = 16
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // ISIM_NETWORK_PERSONALIZATION = 17
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },

    };
    RIL_CardState card_state;
    int num_apps;

    int sim_status = getSIMStatus();
    if (sim_status == SIM_ABSENT) {
        card_state = RIL_CARDSTATE_ABSENT;
        num_apps = 0;
    } else {
        card_state = RIL_CARDSTATE_PRESENT;
        num_apps = 3;
    }

    // Allocate and initialize base card status.
    RIL_CardStatus_v6 *p_card_status = malloc(sizeof(RIL_CardStatus_v6));
    p_card_status->card_state = card_state;
    p_card_status->universal_pin_state = RIL_PINSTATE_UNKNOWN;
    p_card_status->gsm_umts_subscription_app_index = -1;
    p_card_status->cdma_subscription_app_index = -1;
    p_card_status->ims_subscription_app_index = -1;
    p_card_status->num_applications = num_apps;

    // Initialize application status
    int i;
    for (i = 0; i < RIL_CARD_MAX_APPS; i++) {
        p_card_status->applications[i] = app_status_array[SIM_ABSENT];
    }

    // Pickup the appropriate application status
    // that reflects sim_status for gsm.
    if (num_apps != 0) {
        p_card_status->num_applications = 3;
        p_card_status->gsm_umts_subscription_app_index = 0;
        p_card_status->cdma_subscription_app_index = 1;
        p_card_status->ims_subscription_app_index = 2;

        // Get the correct app status
        p_card_status->applications[0] = app_status_array[sim_status];
        p_card_status->applications[1] = app_status_array[sim_status + RUIM_ABSENT];
        p_card_status->applications[2] = app_status_array[sim_status + ISIM_ABSENT];
    }

    *pp_card_status = p_card_status;
    return RIL_E_SUCCESS;
}

/**
 * Free the card status returned by getCardStatus
 */
static void freeCardStatus(RIL_CardStatus_v6 *p_card_status) {
    free(p_card_status);
}

/**
 * SIM ready means any commands that access the SIM will work, including:
 *  AT+CPIN, AT+CSMS, AT+CNMI, AT+CRSM
 *  (all SMS-related commands)
 */

static void pollSIMState (void *param __unused)
{
    ATResponse *p_response;
    int ret;

    if (sState != RADIO_STATE_UNAVAILABLE) {
        // no longer valid to poll
        return;
    }

    switch(getSIMStatus()) {
        case SIM_ABSENT:
        case SIM_PIN:
        case SIM_PUK:
        case SIM_NETWORK_PERSONALIZATION:
        default:
            RLOGI("SIM ABSENT or LOCKED");
            RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
        return;

        case SIM_NOT_READY:
            RIL_requestTimedCallback (pollSIMState, NULL, &TIMEVAL_SIMPOLL);
        return;

        case SIM_READY:
            RLOGI("SIM_READY");
            onSIMReady();
            RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
        return;
    }
}

/** returns 1 if on, 0 if off, and -1 on error */
static int isRadioOn()
{
    ATResponse *p_response = NULL;
    int err;
    char *line;
    char ret;

    err = at_send_command_singleline("AT+CFUN?", "+CFUN:", &p_response);

    if (err < 0 || p_response->success == 0) {
        // assume radio is off
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextbool(&line, &ret);
    if (err < 0) goto error;

    at_response_free(p_response);

    return (int)ret;

error:

    at_response_free(p_response);
    return -1;
}

/**
 * Parse the response generated by a +CTEC AT command
 * The values read from the response are stored in current and preferred.
 * Both current and preferred may be null. The corresponding value is ignored in that case.
 *
 * @return: -1 if some error occurs (or if the modem doesn't understand the +CTEC command)
 *          1 if the response includes the current technology only
 *          0 if the response includes both current technology and preferred mode
 */
int parse_technology_response( const char *response, int *current, int32_t *preferred )
{
    int err;
    char *line, *p;
    int ct;
    int32_t pt = 0;
    char *str_pt;

    line = p = strdup(response);
    RLOGD("Response: %s", line);
    err = at_tok_start(&p);
    if (err || !at_tok_hasmore(&p)) {
        RLOGD("err: %d. p: %s", err, p);
        free(line);
        return -1;
    }

    err = at_tok_nextint(&p, &ct);
    if (err) {
        free(line);
        return -1;
    }
    if (current) *current = ct;

    RLOGD("line remaining after int: %s", p);

    err = at_tok_nexthexint(&p, &pt);
    if (err) {
        free(line);
        return 1;
    }
    if (preferred) {
        *preferred = pt;
    }
    free(line);

    return 0;
}

int query_supported_techs( ModemInfo *mdm __unused, int *supported )
{
    ATResponse *p_response;
    int err, val, techs = 0;
    char *tok;
    char *line;

    RLOGD("query_supported_techs");
    err = at_send_command_singleline("AT+CTEC=?", "+CTEC:", &p_response);
    if (err || !p_response->success)
        goto error;
    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err || !at_tok_hasmore(&line))
        goto error;
    while (!at_tok_nextint(&line, &val)) {
        techs |= ( 1 << val );
    }
    if (supported) *supported = techs;
    return 0;
error:
    at_response_free(p_response);
    return -1;
}

/**
 * query_ctec. Send the +CTEC AT command to the modem to query the current
 * and preferred modes. It leaves values in the addresses pointed to by
 * current and preferred. If any of those pointers are NULL, the corresponding value
 * is ignored, but the return value will still reflect if retreiving and parsing of the
 * values suceeded.
 *
 * @mdm Currently unused
 * @current A pointer to store the current mode returned by the modem. May be null.
 * @preferred A pointer to store the preferred mode returned by the modem. May be null.
 * @return -1 on error (or failure to parse)
 *         1 if only the current mode was returned by modem (or failed to parse preferred)
 *         0 if both current and preferred were returned correctly
 */
int query_ctec(ModemInfo *mdm __unused, int *current, int32_t *preferred)
{
    ATResponse *response = NULL;
    int err;
    int res;

    RLOGD("query_ctec. current: %p, preferred: %p", current, preferred);
    err = at_send_command_singleline("AT+CTEC?", "+CTEC:", &response);
    if (!err && response->success) {
        res = parse_technology_response(response->p_intermediates->line, current, preferred);
        at_response_free(response);
        return res;
    }
    RLOGE("Error executing command: %d. response: %p. status: %d", err, response, response? response->success : -1);
    at_response_free(response);
    return -1;
}

int is_multimode_modem(ModemInfo *mdm)
{
    ATResponse *response;
    int err;
    char *line;
    int tech;
    int32_t preferred;

    if (query_ctec(mdm, &tech, &preferred) == 0) {
        mdm->currentTech = tech;
        mdm->preferredNetworkMode = preferred;
        if (query_supported_techs(mdm, &mdm->supportedTechs)) {
            return 0;
        }
        return 1;
    }
    return 0;
}

/**
 * Find out if our modem is GSM, CDMA or both (Multimode)
 */
static void probeForModemMode(ModemInfo *info)
{
    info->isMultimode = 0;
    info->supportedTechs = MDM_GSM | MDM_WCDMA | MDM_CDMA | MDM_EVDO | MDM_LTE;
    info->currentTech = MDM_LTE;
}

/**
 * Initialize everything that can be configured while we're still in
 * AT+CFUN=0
 */
static void initializeCallback(void *param __unused)
{
    ATResponse *p_response = NULL;
    int err;
    /* BEGIN Add SIM card status check before initial by yangyao in 20220415 */
    int check_sim_status_time = 0;
    /* END Add SIM card status check before initial by yangyao in 20220415 */

    setRadioState (RADIO_STATE_OFF);

    RLOGD("at_handshake");
    /* BEGIN check at handshake by yangyao in 20220309 */
    if(at_handshake() != 0) {
        RLOGE("AT handshake failure, restart rild.");
        onATTimeout();
    }
    /* END check at handshake by yangyao in 20220309 */

    RLOGD("probeForModemMode");
    probeForModemMode(sMdmInfo);
    /* note: we don't check errors here. Everything important will
       be handled in onATTimeout and onATReaderClosed */

    /* BEGIN Add SIM card status check before initial by yangyao in 20220415 */
    for(check_sim_status_time = 0; check_sim_status_time < SIM_CHECK_MAX_RETRY; check_sim_status_time++) {
        g_sim_status = getSimStatus();

        if((g_sim_status == SIM_STATUS_READY) ||
           (g_sim_status == SIM_STATUS_PIN_REQUIRED) ||
           (g_sim_status == SIM_STATUS_PUK_REQUIRED)) {
            RLOGD("initializeCallback: SIM status is fine, g_sim_status = %d", g_sim_status);
            break;
        } else {
            RLOGD("initializeCallback: SIM status is not ready, g_sim_status = %d, check_sim_status_time = %d", g_sim_status,check_sim_status_time);
            sleep(SIM_CHECK_TIMEVAL);
        }
    }
    /* END Add SIM card status check before initial by yangyao in 20220415 */

    /*  atchannel is tolerant of echo but it must */
    /*  have verbose result codes */

    system("/system/bin/ip link add link usb0 name usb0.1 type vlan id 1");
    system("/system/bin/ip link add link usb0 name usb0.2 type vlan id 2");
    system("ifconfig usb0 up");

    /*  Query modem version */
    at_send_command_singleline_no_prefix("AT+CGMR", &p_response);
    ril_property_set("ro.modem.version", p_response->p_intermediates->line);
    at_response_free(p_response);

    /*  No auto-answer */
    at_send_command("ATS0=0", NULL);

    /*  Extended errors */
    at_send_command("AT+CMEE=1", NULL);

    /* BEGIN Make sure all CREG/CGREG/CEREG=2 as default by yangyao in 20220309 */
    /*  Network registration events */
    err = at_send_command("AT+CREG=2", &p_response);

    /* some handsets -- in tethered mode -- don't support CREG=2 */
    if (err < 0 || p_response->success == 0) {
        at_send_command("AT+CREG=1", NULL);
    }

    at_response_free(p_response);

    /*  GPRS registration events */
    at_send_command("AT+CGREG=2", NULL);

    /* EGPRS registration events */
    at_send_command("AT+CEREG=2", NULL);
    /* END Make sure all CREG/CGREG/CEREG=2 as default by yangyao in 20220309 */

    /*  Call Waiting notifications */
    at_send_command("AT+CCWA=1", NULL);

    /*  Alternating voice/data off */
    at_send_command("AT+CMOD=0", NULL);

    /*  Not muted */
    at_send_command("AT+CMUT=0", NULL);

    /* BEGIN Deleted AT+CSSN=0,1 in initial by yangyao in 20220309 */
    /*  +CSSU unsolicited supp service notifications */
    //at_send_command("AT+CSSN=0,1", NULL);
    /* END Deleted AT+CSSN=0,1 in initial by yangyao in 20220309 */

    /*  no connected line identification */
    at_send_command("AT+COLP=0", NULL);

    /*  HEX character set */
    //at_send_command("AT+CSCS=\"HEX\"", NULL);

    /*  USSD unsolicited */
    at_send_command("AT+CUSD=1", NULL);

    /*  Enable +CGEV GPRS event notifications, but don't buffer */
    at_send_command("AT+CGEREP=1,0", NULL);

    /*  SMS PDU mode */
    at_send_command("AT+CMGF=0", NULL);

#ifdef USE_TI_COMMANDS

    at_send_command("AT%CPI=3", NULL);

    /*  TI specific -- notifications when SMS is ready (currently ignored) */
    at_send_command("AT%CSTAT=1", NULL);

#endif /* USE_TI_COMMANDS */

    /* assume radio is off on error */
    if (isRadioOn() > 0) {
        setRadioState (RADIO_STATE_ON);
    }

    /* BEGIN if radio not power on, send AT+CFUN=1 by yangyao in 20220311 */
    if (isRadioOn() == 0) {
        at_send_command("AT+CFUN=1", NULL);
    }
    /* BEGIN if radio not power on, send AT+CFUN=1 by yangyao in 20220311 */

    /* BEGIN add datacall state report by leikai in 20220314 */
    start_gobinet_event_report();
    /* END add datacall state report by leikai in 20220314 */

    /* BEGIN Add signal unsolicited by yangyao in 20220322 */
    requestUnsolicitedSignalStrength();
    /* END Add signal unsolicited by yangyao in 20220322 */

    /* BEGIN Add SIM status changed unsolicited by yangyao in 20220418 */
    requestUnsolicitedSimStatusChanged();
    /* END Add SIM status changed unsolicited by yangyao in 20220418 */

    /* BEGIN ADD read SMS when RIL initial or wakeup by yangyao in 20220323 */
    pthread_mutex_lock(&g_init_mutex);
    g_is_rild_inited = TRUE;

    RLOGD("initializeCallback: g_is_rild_inited = %d, g_is_rilj_inited = %d", g_is_rild_inited, g_is_rilj_inited);
    if(g_is_rilj_inited == TRUE) {
        RIL_requestTimedCallback(unsolicitedUnreadSms, NULL, NULL);
    }

    pthread_mutex_unlock(&g_init_mutex);
    /* END ADD read SMS when RIL initial or wakeup by yangyao in 20220323 */
}

static void waitForClose()
{
    pthread_mutex_lock(&s_state_mutex);

    while (s_closed == 0) {
        pthread_cond_wait(&s_state_cond, &s_state_mutex);
    }

    pthread_mutex_unlock(&s_state_mutex);
}

static void sendUnsolImsNetworkStateChanged()
{
#if 0 // to be used when unsol is changed to return data.
    int reply[2];
    reply[0] = s_ims_registered;
    reply[1] = s_ims_services;
    reply[1] = s_ims_format;
#endif
    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_IMS_NETWORK_STATE_CHANGED,
            NULL, 0);
}

/**
 * Called by atchannel when an unsolicited line appears
 * This is called on atchannel's reader thread. AT commands may
 * not be issued here
 */
static void onUnsolicited (const char *s, const char *sms_pdu)
{
    RLOGI("onUnsolicited s:%s, sms_pdu:%s", s, sms_pdu);
    char *line = NULL, *p;
    int err;

    /* Ignore unsolicited responses until we're initialized.
     * This is OK because the RIL library will poll for initial state
     */
    if (sState == RADIO_STATE_UNAVAILABLE) {
        return;
    }

    if (strStartsWith(s, "%CTZV:")) {
        /* TI specific -- NITZ time */
        char *response;

        line = p = strdup(s);
        at_tok_start(&p);

        err = at_tok_nextstr(&p, &response);

        if (err != 0) {
            RLOGE("invalid NITZ line %s\n", s);
        } else {
            RIL_onUnsolicitedResponse (
                RIL_UNSOL_NITZ_TIME_RECEIVED,
                response, strlen(response) + 1);
        }
        free(line);
    } else if (strStartsWith(s,"+CRING:")
                || strStartsWith(s,"RING")
                || strStartsWith(s,"NO CARRIER")
                || strStartsWith(s,"+CCWA")
    ) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
            NULL, 0);
#ifdef WORKAROUND_FAKE_CGEV
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL); //TODO use new function
#endif /* WORKAROUND_FAKE_CGEV */
    } else if (strStartsWith(s,"+CREG:")
                || strStartsWith(s,"+CGREG:")
                /* BEGIN Add +CEREG unsolicited by yangyao in 20220317 */
                || strStartsWith(s,"+CEREG:")
                /* END Add +CEREG unsolicited by yangyao in 20220317 */
    ) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,
            NULL, 0);
#ifdef WORKAROUND_FAKE_CGEV
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#endif /* WORKAROUND_FAKE_CGEV */
    /* BEGIN Add SMS report process by yangyao in 20220317 */
    } else if (strStartsWith(s, "+CMTI:") || strStartsWith(s, "+CBMI:") || strStartsWith(s, "+CDSI:")) {
        onNewSmsNotification(s);
    } else if (strStartsWith(s, "+CMGR:")) {
        onNewSmsArrived(sms_pdu);
    /* END Add SMS report process by yangyao in 20220317 */
    } else if (strStartsWith(s, "+CMT:")) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_NEW_SMS,
            sms_pdu, strlen(sms_pdu));
    } else if (strStartsWith(s, "+CDS:")) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT,
            sms_pdu, strlen(sms_pdu));
    } else if (strStartsWith(s, "+CGEV:")) {
        /* Really, we can ignore NW CLASS and ME CLASS events here,
         * but right now we don't since extranous
         * RIL_UNSOL_DATA_CALL_LIST_CHANGED calls are tolerated
         */
        /* can't issue AT commands here -- call on main thread */
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#ifdef WORKAROUND_FAKE_CGEV
    } else if (strStartsWith(s, "+CME ERROR: 150")) {
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#endif /* WORKAROUND_FAKE_CGEV */
    } else if (strStartsWith(s, "+CTEC: ")) {
        int tech, mask;
        switch (parse_technology_response(s, &tech, NULL))
        {
            case -1: // no argument could be parsed.
                RLOGE("invalid CTEC line %s\n", s);
                break;
            case 1: // current mode correctly parsed
            case 0: // preferred mode correctly parsed
                mask = 1 << tech;
                if (mask != MDM_GSM && mask != MDM_CDMA &&
                     mask != MDM_WCDMA && mask != MDM_LTE) {
                    RLOGE("Unknown technology %d\n", tech);
                } else {
                    setRadioTechnology(sMdmInfo, tech);
                }
                break;
        }
    } else if (strStartsWith(s, "+CCSS: ")) {
        int source = 0;
        line = p = strdup(s);
        if (!line) {
            RLOGE("+CCSS: Unable to allocate memory");
            return;
        }
        if (at_tok_start(&p) < 0) {
            free(line);
            return;
        }
        if (at_tok_nextint(&p, &source) < 0) {
            RLOGE("invalid +CCSS response: %s", line);
            free(line);
            return;
        }
        SSOURCE(sMdmInfo) = source;
        RIL_onUnsolicitedResponse(RIL_UNSOL_CDMA_SUBSCRIPTION_SOURCE_CHANGED,
                                  &source, sizeof(source));
    } else if (strStartsWith(s, "+WSOS: ")) {
        char state = 0;
        int unsol;
        line = p = strdup(s);
        if (!line) {
            RLOGE("+WSOS: Unable to allocate memory");
            return;
        }
        if (at_tok_start(&p) < 0) {
            free(line);
            return;
        }
        if (at_tok_nextbool(&p, &state) < 0) {
            RLOGE("invalid +WSOS response: %s", line);
            free(line);
            return;
        }
        free(line);

        unsol = state ?
                RIL_UNSOL_ENTER_EMERGENCY_CALLBACK_MODE : RIL_UNSOL_EXIT_EMERGENCY_CALLBACK_MODE;

        RIL_onUnsolicitedResponse(unsol, NULL, 0);

    } else if (strStartsWith(s, "+WPRL: ")) {
        int version = -1;
        line = p = strdup(s);
        if (!line) {
            RLOGE("+WPRL: Unable to allocate memory");
            return;
        }
        if (at_tok_start(&p) < 0) {
            RLOGE("invalid +WPRL response: %s", s);
            free(line);
            return;
        }
        if (at_tok_nextint(&p, &version) < 0) {
            RLOGE("invalid +WPRL response: %s", s);
            free(line);
            return;
        }
        free(line);
        RIL_onUnsolicitedResponse(RIL_UNSOL_CDMA_PRL_CHANGED, &version, sizeof(version));
    } else if (strStartsWith(s, "+CFUN: 0")) {
        setRadioState(RADIO_STATE_OFF);
    }
}

/* Called on command or reader thread */
static void onATReaderClosed()
{
    RLOGI("AT channel closed\n");
    at_close();
    s_closed = 1;

    setRadioState (RADIO_STATE_UNAVAILABLE);
}

/* Called on command thread */
static void onATTimeout()
{
    RLOGI("AT channel timeout; closing\n");
    at_close();

    s_closed = 1;

    /* FIXME cause a radio reset here */
    /* BEGIN restart module when AT channel timeout by yangyao in 20220309 */
    ril_property_set("ro.reset.rild", "1");
    setRadioState (RADIO_STATE_UNAVAILABLE);

    /* wait 3s and restart rild */
    sleep(3);
    exit(-1);
    RLOGE("RIL_Restart.");
    /* END restart module when AT channel timeout by yangyao in 20220309 */
}

/* Called to pass hardware configuration information to telephony
 * framework.
 */
static void setHardwareConfiguration(int num, RIL_HardwareConfig *cfg)
{
   RIL_onUnsolicitedResponse(RIL_UNSOL_HARDWARE_CONFIG_CHANGED, cfg, num*sizeof(*cfg));
}

static void usage(char *s __unused)
{
#ifdef RIL_SHLIB
    fprintf(stderr, "reference-ril requires: -p <tcp port> or -d /dev/tty_device\n");
#else
    fprintf(stderr, "usage: %s [-p <tcp port>] [-d /dev/tty_device]\n", s);
    exit(-1);
#endif
}

static void *
mainLoop(void *param __unused)
{
    int fd;
    int ret;
    char atdevice[10];

    RLOGD("mainLoop");
    AT_DUMP("== ", "entering mainLoop()", -1 );
    at_set_on_reader_closed(onATReaderClosed);
    at_set_on_timeout(onATTimeout);

    for (;;) {
        detect_module();
        if (get_ttyAT(atdevice) == -1) {
            sleep(3);
            continue;
        }
        sprintf(ttyAT, "/dev/%s", atdevice);
        RLOGD("ttyAT:%s", ttyAT);

        fd = -1;
        while  (fd < 0) {
            if (s_device_path != NULL) {
                fd = open (s_device_path, O_RDWR);
                if ( fd >= 0) {
                    /* disable echo on serial ports */
                    struct termios  ios;
                    memset(&ios, 0, sizeof(ios));
                    tcgetattr( fd, &ios );
                    cfmakeraw(&ios);
                    ios.c_lflag = 0;  /* disable ECHO, ICANON, etc... */
                    cfsetispeed(&ios, B115200);
                    cfsetospeed(&ios, B115200);
                    tcsetattr( fd, TCSANOW, &ios );
                    RLOGD("open device %s correctly\n", s_device_path);
                    tcflush(fd, TCIOFLUSH);
                }
            }

            if (fd < 0) {
                RLOGE("open device %s error for %s\n", s_device_path, strerror(errno));
                perror ("opening AT interface. retrying...");
                sleep(3);
                /* never returns */
            }
        }

        s_closed = 0;
        ret = at_open(fd, onUnsolicited);

        if (ret < 0) {
            RLOGE ("AT error %d on at_open\n", ret);
            return 0;
        }

        RIL_requestTimedCallback(initializeCallback, NULL, &TIMEVAL_0);

        // Give initializeCallback a chance to dispatched, since
        // we don't presently have a cancellation mechanism
        sleep(1);

        waitForClose();
        RLOGI("Re-opening after close");
    }
}

#ifdef RIL_SHLIB

pthread_t s_tid_mainloop;

const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc, char **argv)
{
    int ret;
    int fd = -1;
    int opt;
    pthread_attr_t attr;

    s_rilenv = env;

    RLOGD("RIL_Init, Version: %s", GOSUNCN_RIL_VERSION);

    while ( -1 != (opt = getopt(argc, argv, "p:d:s:c:"))) {
        switch (opt) {
            case 'p':
                s_port = atoi(optarg);
                if (s_port == 0) {
                    usage(argv[0]);
                    return NULL;
                }
                RLOGI("Opening loopback port %d\n", s_port);
            break;

            case 'd':
                s_device_path = optarg;
                RLOGI("Opening tty device %s\n", s_device_path);
            break;

            case 's':
                s_device_path   = optarg;
                s_device_socket = 1;
                RLOGI("Opening socket %s\n", s_device_path);
            break;

            case 'c':
                RLOGI("Client id received %s\n", optarg);
            break;

            default:
                usage(argv[0]);
                return NULL;
        }
    }

    s_device_path = ttyAT;
    if (s_port < 0 && s_device_path == NULL && !isInEmulator()) {
        usage(argv[0]);
        return NULL;
    }

    sMdmInfo = calloc(1, sizeof(ModemInfo));
    if (!sMdmInfo) {
        RLOGE("Unable to alloc memory for ModemInfo");
        return NULL;
    }
    pthread_attr_init (&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ret = pthread_create(&s_tid_mainloop, &attr, mainLoop, NULL);

    return &s_callbacks;
}
#else /* RIL_SHLIB */
int main (int argc, char **argv)
{
    int ret;
    int fd = -1;
    int opt;

    while ( -1 != (opt = getopt(argc, argv, "p:d:"))) {
        switch (opt) {
            case 'p':
                s_port = atoi(optarg);
                if (s_port == 0) {
                    usage(argv[0]);
                }
                RLOGI("Opening loopback port %d\n", s_port);
            break;

            case 'd':
                s_device_path = optarg;
                RLOGI("Opening tty device %s\n", s_device_path);
            break;

            case 's':
                s_device_path   = optarg;
                s_device_socket = 1;
                RLOGI("Opening socket %s\n", s_device_path);
            break;

            default:
                usage(argv[0]);
        }
    }

    if (s_port < 0 && s_device_path == NULL && !isInEmulator()) {
        usage(argv[0]);
    }

    RIL_register(&s_callbacks);

    mainLoop(NULL);

    return 0;
}

#endif /* RIL_SHLIB */
